// schneidi-cli: commands on projects (new, info, import, edit, backups, restore, render, effects) and the app
#include "cli/CommandsDetail.h"
#include "cli/Live.h"

#include "core/Editor.h"
#include "core/StreamPresets.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/GradeWindows.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/Exporter.h"
#include "engine/Profiles.h"
#include "engine/RenderQueue.h"
#include "engine/Snapshot.h"
#include "engine/Stabilizer.h"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Cli::detail {

// ---------- Commands ----------

QJsonObject cmdNew(const QJsonObject& a, Context&)
{
    const QString path = absolute(a["project"].toString());
    if (!path.endsWith("." + QString(ProjectFile::Extension)))
        fail("BAD_ARGUMENT", QString("project file must end in .%1").arg(ProjectFile::Extension));
    if (QFileInfo::exists(path) && !a["overwrite"].toBool())
        fail("EXISTS", "project already exists (pass overwrite to replace it): " + path);
    ProjectData d;
    d.format = parseFormat(a);
    d.timeline = emptyTimeline();
    Project p;
    p.load(d);
    for (const QJsonValue& v : a["media"].toArray()) {
        const MediaInfo info = Engine::probe(p.format(), v.toString());
        if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) fail("BAD_MEDIA", "cannot read media: " + v.toString());
        p.addMedia(info);
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QString error;
    if (!ProjectFile::save(p.data(), path, &error)) fail("SAVE_FAILED", error);
    return projectJson(p, path);
}

QJsonObject cmdInfo(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString());
    s.selectTimeline(a["timeline"]);
    QJsonObject o = projectJson(s.project, s.path);
    if (s.live) o["app"] = s.app; // open in the app: what the user sees there
    if (a["summary"].toBool()) {
        // Counts instead of the lists (the full JSON of a long timeline is big)
        QJsonObject tl = o["timeline"].toObject();
        QJsonArray tracks;
        int clips = 0;
        for (const QJsonValue& v : tl["tracks"].toArray()) {
            QJsonObject t = v.toObject();
            const int n = t["clips"].toArray().size();
            clips += n;
            t.remove("clips");
            t["clips"] = n;
            tracks << t;
        }
        tl["tracks"] = tracks;
        tl["clips"] = clips;
        tl["markers"] = tl["markers"].toArray().size();
        QJsonArray subs;
        for (const QJsonValue& v : tl["subtitles"].toArray()) {
            QJsonObject st = v.toObject();
            st["cues"] = st["cues"].toArray().size();
            st.remove("style");
            subs << st;
        }
        if (!subs.isEmpty()) tl["subtitles"] = subs;
        else tl.remove("subtitles");
        o["timeline"] = tl;
        o["media"] = o["media"].toArray().size();
    }
    return o;
}

QJsonObject cmdImport(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString(), Session::Access::Write);
    if (!a.contains("files") && !a.contains("timeline_file")) fail("BAD_ARGUMENT", "give 'files' (media) or 'timeline_file'");
    for (const QJsonValue& v : a["files"].toArray()) s.media(v.toString());
    const QJsonObject imported = a.contains("timeline_file") ? importTimeline(s, a) : QJsonObject();
    const QString backup = s.save();
    QJsonObject o = projectJson(s.project, s.path);
    o.remove("timeline");
    for (auto it = imported.begin(); it != imported.end(); ++it) o[it.key()] = it.value();
    if (!backup.isEmpty()) o["backup"] = backup;
    if (s.live) o["live"] = true;
    return o;
}

QJsonObject cmdEdit(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString(), a["dry_run"].toBool() ? Session::Access::Read : Session::Access::Write);
    s.selectTimeline(a["timeline"]);
    QJsonValue opsValue = a["ops"];
    if (opsValue.isObject()) opsValue = QJsonArray{opsValue};
    const QJsonArray ops = opsValue.toArray();
    if (ops.isEmpty()) fail("BAD_ARGUMENT", "'ops' must be a non-empty list of operations");
    for (int i = 0; i < ops.size(); ++i) {
        try {
            if (!ops[i].isObject()) fail("BAD_ARGUMENT", "operation is not an object");
            applyOp(s, ops[i].toObject());
        } catch (const Error& e) {
            throw Error{e.code, QString("op %1 (%2): %3").arg(i).arg(ops[i]["op"].toString(), e.message)};
        }
    }
    const bool dry = a["dry_run"].toBool();
    // The timeline edited last (save() may switch back to the one the app opens)
    QJsonObject o{{"project", s.path}, {"ops", ops.size()}, {"saved", !dry && !s.live}, {"timeline", timelineJson(s.project)}};
    if (s.live) o["live"] = true; // changed in the app (one undo step there), the user saves
    if (!s.notes.isEmpty()) o["notes"] = s.notes;
    if (s.project.sequences().size() > 1) o["timelines"] = timelinesJson(s.project, s.multicamSync);
    if (!dry) {
        const QString backup = s.save();
        if (!backup.isEmpty()) o["backup"] = backup;
    }
    return o;
}

// Project file of `project` ("@app" or a file open in the app = the file the app saved it to); throws if the
// project in the app was never saved
QString projectFileOf(const QString& project)
{
    QString path = absolute(project);
    bool live = false;
    if (LiveHost* h = Live::host()) {
        path = h->liveProjectPath();
        live = true;
    } else if (Live::Connection* c = Live::connectionFor(path)) {
        path = Live::projectPath(c);
        live = true;
    }
    if (live && path.isEmpty())
        fail("NOT_SAVED", "the project in the schneidi app was never saved, so it has no backups yet");
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "project not found: " + path);
    return path;
}

// Backup copy by number from `backups` (1 = newest) or file
QString backupFile(const QString& path, const QJsonObject& a)
{
    const QStringList files = ProjectFile::backups(path);
    const QString which = a.contains("backup") ? a["backup"].toVariant().toString() : QString("1");
    bool isIndex = false;
    const int index = which.toInt(&isIndex);
    if (isIndex) {
        if (index < 1 || index > files.size())
            fail("NOT_FOUND", QString("no backup %1 (there are %2, see `backups`)").arg(index).arg(files.size()));
        return files[index - 1];
    }
    const QString source = absolute(which);
    if (!QFileInfo::exists(source)) fail("NOT_FOUND", "backup not found: " + source);
    return source;
}

QJsonObject cmdBackups(const QJsonObject& a, Context&)
{
    const QString path = projectFileOf(a["project"].toString());
    QJsonArray list;
    const QStringList files = ProjectFile::backups(path);
    for (int i = 0; i < files.size(); ++i) {
        QJsonObject o{{"index", i + 1}, {"file", files[i]},
                      {"saved", QFileInfo(files[i]).lastModified().toString(Qt::ISODate)}};
        ProjectData d;
        if (ProjectFile::load(files[i], &d, nullptr)) {
            int clips = 0;
            for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
                for (const Track& t : d.timeline.tracks(k)) clips += t.clips.size();
            const int end = TimelineOps::endFrame(d.timeline);
            o["clips"] = clips;
            o["length_tc"] = tc(end, d.format);
        } else {
            o["unreadable"] = true;
        }
        list << o;
    }
    return {{"project", path}, {"backups", list}};
}

QJsonObject cmdRestore(const QJsonObject& a, Context&)
{
    // In the app: the backup replaces its timelines as one undo step, nothing is saved
    if (LiveHost* h = Live::host()) {
        const QString path = projectFileOf(a["project"].toString());
        const QString source = backupFile(path, a);
        ProjectData d;
        QString error;
        if (!ProjectFile::load(source, &d, &error)) fail("BAD_PROJECT", error);
        Project* p = h->liveProject();
        if (!(d.format == p->format()))
            fail("FORMAT_DIFFERS", "the backup has another project format (resolution/frame rate); close the project "
                                   "in the app and restore the file");
        Project restored;
        restored.load(d);
        for (const MediaInfo& m : restored.media()) p->addMedia(m);
        p->closeMerge();
        p->editSequences(T("KI: %1").arg("restore"), [&](QVector<Sequence>& seqs, int& current) {
            seqs = restored.sequences();
            current = restored.currentSequence();
        });
        p->closeMerge();
        return {{"project", path}, {"restored", source}, {"live", true}, {"timeline", timelineJson(*p)}};
    }
    const QString path = absolute(a["project"].toString());
    if (Live::isAppProject(path)) fail("NO_APP", "the schneidi app is not running");
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "project not found: " + path);
    const auto lock = ProjectFile::lock(path);
    if (!lock) fail("PROJECT_LOCKED", "the project is being saved by someone else (schneidi app?), try again: " + path);
    // Pick the copy before the current state goes into the backups (that shifts the numbers)
    const QString source = backupFile(path, a);
    ProjectData d;
    QString error;
    if (!ProjectFile::load(source, &d, &error)) fail("BAD_PROJECT", error);
    // Saved again (not copied): media paths relative to the project file stay right
    const QString backup = ProjectFile::backup(path);
    if (!ProjectFile::save(d, path, &error)) fail("SAVE_FAILED", error);
    Project p;
    p.load(d);
    QJsonObject o{{"project", path}, {"restored", source}, {"timeline", timelineJson(p)}};
    if (!backup.isEmpty()) o["backup"] = backup;
    return o;
}

QJsonObject cmdRender(const QJsonObject& a, Context& ctx)
{
    Session s(a["project"].toString());
    s.selectTimeline(a["timeline"]);
    RenderJob job;
    if (a.contains("preset")) {
        // Deliver presets of the app (built-in and the user's own)
        const QString name = a["preset"].toString();
        QStringList names;
        bool found = false;
        for (const RenderPreset& p : RenderPresets::all()) {
            names << p.name;
            if (p.name.compare(name, Qt::CaseInsensitive) == 0) {
                job.settings = p.settings;
                job.preset = p.name;
                found = true;
            }
        }
        if (!found) fail("NOT_FOUND", QString("no preset '%1' (there are: %2)").arg(name, names.join(", ")));
    }
    const QString formatId = a.contains("format") ? a["format"].toString() : job.settings.format;
    const auto& formats = renderFormats();
    const bool srtOnly = formatId == "srt";
    if (!srtOnly && std::none_of(formats.begin(), formats.end(), [&](const RenderFormatInfo& i) { return formatId == i.id; })) {
        QStringList ids;
        for (const RenderFormatInfo& i : formats) ids << i.id;
        fail("BAD_ARGUMENT", "unknown format " + formatId + " (" + ids.join(", ") + ", srt)");
    }
    QString out = absolute(a["out"].toString());
    if (QFileInfo(out).suffix().isEmpty()) out += "." + QString(srtOnly ? "srt" : renderFormat(formatId).extension);
    RenderSettings target = job.settings;
    if (!srtOnly) target.format = formatId;
    if (QFileInfo::exists(firstOutputFile(out, target)) && !a["overwrite"].toBool())
        fail("EXISTS", "output exists (pass overwrite to replace it): " + out);
    job.path = out;
    if (!srtOnly) job.settings.format = formatId;
    if (a.contains("quality")) job.settings.quality = std::clamp(a["quality"].toInt(), 0, RenderSettings::kQualityWeb);
    if (a.contains("height")) {
        job.settings.shortSide = a["height"].toInt();
        job.settings.size = {};
    }
    if (a.contains("size")) {
        const QStringList wh = a["size"].toString().toLower().split('x');
        const int w = wh.value(0).toInt(), h = wh.value(1).toInt();
        if (wh.size() != 2 || w < 16 || h < 16 || w > 8192 || h > 8192) fail("BAD_ARGUMENT", "size must look like 1080x1920");
        job.settings.size = QSize(w & ~1, h & ~1);
    }
    if (a.contains("fps")) {
        const FrameRate r = nearestExportRate(a["fps"].toDouble());
        job.settings.rateNum = r.num;
        job.settings.rateDen = r.den;
    }
    if (a.contains("encoder")) {
        bool ok = false;
        job.settings.encoder = videoEncoderFromId(a["encoder"].toString(), VideoEncoder::Auto, &ok);
        if (!ok) fail("BAD_ARGUMENT", "encoder must be auto, software, nvenc, vaapi or qsv");
    }
    if (a.contains("audio_bitrate")) job.settings.audioBitrateK = std::clamp(a["audio_bitrate"].toInt(), 32, 512);
    if (a.contains("subtitles")) {
        const int i = QStringList{"none", "burn", "srt"}.indexOf(a["subtitles"].toString());
        if (i < 0) fail("BAD_ARGUMENT", "subtitles must be none, burn (into the picture) or srt (file next to the video)");
        job.settings.subtitles = i;
    }
    job.size = job.settings.outputSize(s.format().size());
    const Timeline tl = s.project.renderTimeline();
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) fail("EMPTY", "the timeline is empty");
    if (a.contains("from") || a.contains("to")) {
        job.inOut = true;
        job.from = a.contains("from") ? parseTime(a["from"], s.format(), "from") : 0;
        job.to = (a.contains("to") ? parseTime(a["to"], s.format(), "to") : end) - 1;
        if (job.to < job.from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
    }
    if (srtOnly) {
        // Only the subtitle file (visible subtitle track, times from the range start)
        const SubtitleTrack* track = nullptr;
        for (const SubtitleTrack& t : tl.subtitles)
            if (t.enabled && !t.cues.isEmpty()) track = &t;
        for (const SubtitleTrack& t : tl.subtitles)
            if (!track && !t.cues.isEmpty()) track = &t;
        if (!track) fail("EMPTY", "the timeline has no subtitles");
        QDir().mkpath(QFileInfo(out).absolutePath());
        QFile f(out);
        if (!f.open(QIODevice::WriteOnly)
            || f.write(Subtitles::toSrt(track->cues, s.project.frameRate(), job.inOut ? job.from : 0, job.inOut ? job.to + 1 : -1)) < 0)
            fail("WRITE_FAILED", "cannot write " + out);
        return {{"out", out}, {"format", "srt"}, {"cues", int(track->cues.size())}};
    }
    if (Exporter::readsFile(tl, out)) fail("BAD_ARGUMENT", "the output would overwrite a source file of the project");
    QDir().mkpath(QFileInfo(out).absolutePath());

    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QString message;
    QObject::connect(&ex, &Exporter::progress, &loop, [&ctx](int percent) {
        if (ctx.progress) ctx.progress(percent / 100.0);
    });
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        message = msg;
        loop.quit();
    });
    QElapsedTimer timer;
    timer.start();
    QString error;
    const ExportSettings es = RenderQueue::exportSettings(job, s.format());
    if (!ex.start(tl, es, &error)) fail("RENDER_FAILED", error);
    loop.exec();
    if (!ok) fail("RENDER_FAILED", message.isEmpty() ? QString("rendering failed") : message);
    QJsonObject o{{"out", out}, {"format", formatId}, {"render_s", std::round(timer.elapsed() / 100.0) / 10}};
    if (!ex.usedCodec().isEmpty()) o["encoder"] = ex.usedCodec(); // video codec that wrote the file, e.g. h264_nvenc
    if (job.settings.kind() == RenderKind::ImageSequence) {
        o["frames_dir"] = imageSequenceDir(out);
        o["pattern"] = QFileInfo(imageSequencePattern(out)).fileName();
    }
    if (!ex.note().isEmpty()) o["encoder_note"] = ex.note();
    if (!job.settings.audioOnly()) {
        o["width"] = es.size.width();
        o["height"] = es.size.height();
    }
    if (!job.preset.isEmpty()) o["preset"] = job.preset;
    if (!es.subtitlePath.isEmpty()) o["srt"] = es.subtitlePath;
    return o;
}


namespace {
// LUTs, luma images and presets for `effects`: built-in ones by id, the user's by folder/name
QJsonArray assetsJson(const QVector<EffectFolders::LutEntry>& list)
{
    QJsonArray out;
    for (const EffectFolders::LutEntry& e : list) {
        const bool builtin = EffectFolders::isBuiltin(e.path);
        out << QJsonObject{{"name", builtin ? EffectFolders::builtinId(e.path) : (e.group.isEmpty() ? e.name : e.group + "/" + e.name)},
                           {"label", builtin ? e.name : QFileInfo(e.path).fileName()}};
    }
    return out;
}
} // namespace

QJsonObject cmdEffects(const QJsonObject& a, Context&)
{
    const QString only = a["effect"].toString();
    QJsonArray list;
    for (const EffectDescriptor& d : EffectRegistry::all()) {
        if (!only.isEmpty() && d.id != only) continue;
        QJsonObject o{{"effect", d.id}, {"name", d.name}, {"kind", d.video ? "video" : "audio"}};
        if (!d.category.isEmpty()) o["category"] = d.category;
        if (d.id == "grade")
            o["note"] = "the Color page grade (lift/gamma/gain/offset, LUT); windows (shape masks) via the `window` op";
        if (d.id == Stabilizer::EffectId)
            o["note"] = Stabilizer::available()
                            ? "needs an analysis pass (background in the app, before encoding in `render`)"
                            : "MLT module vid.stab is missing: the effect does nothing on this computer";
        if (only.isEmpty()) {
            QJsonArray keys;
            for (const EffectParam& p : d.params)
                if (!p.sparse) keys << p.key; // grade windows: `help edit --op window`
            o["params"] = keys;
        } else {
            static const char* types[] = {"number", "color", "path", "boolean", "choice"};
            QJsonArray params;
            for (const EffectParam& p : d.params) {
                if (p.sparse) continue;
                QJsonObject po{{"key", p.key}, {"label", p.label}, {"type", types[p.type]}};
                if (p.type == EffectParam::Double) {
                    po["min"] = p.min;
                    po["max"] = p.max;
                }
                if (p.type == EffectParam::Choice) po["choices"] = QJsonArray::fromStringList(p.choices);
                if (p.analysis) po["analysis"] = true; // changing it means a new analysis pass
                po["default"] = p.type == EffectParam::Color ? QJsonValue(p.defaultValue.value<QColor>().name())
                                                             : QJsonValue::fromVariant(p.defaultValue);
                if (!p.description.isEmpty()) po["description"] = p.description;
                params << po;
            }
            o["params"] = params;
            if (!d.description.isEmpty()) o["description"] = d.description;
        }
        list << o;
    }
    if (list.isEmpty() && !only.isEmpty()) fail("NOT_FOUND", "no effect " + only);
    if (!only.isEmpty()) return {{"effects", list}};
    QJsonArray types;
    for (const auto& t : kTransitionTypes) types << t.id;
    return {{"effects", list},
            {"luts", assetsJson(EffectFolders::builtinLuts() + EffectFolders::userLuts())},
            {"transition_types", types},
            {"lumas", assetsJson(EffectFolders::builtinTransitions() + EffectFolders::userTransitions())},
            {"presets", assetsJson(EffectFolders::userPresets())}};
}

QString presetHelp()
{
    QStringList names;
    for (const RenderPreset& p : RenderPresets::all()) names << p.name;
    return "Deliver preset of the app (other parameters override it): " + names.join(", ");
}

// The running app: state (playhead, selection …) and control (play, seek, select, undo, save, proxies)
QJsonObject cmdApp(const QJsonObject& a, Context&)
{
    LiveHost* h = Live::host();
    if (!h) fail("NO_APP", "the project is not open in the schneidi app (start it and open the project, or use \"@app\")");
    Project* p = h->liveProject();
    const QString action = a["action"].toString().trimmed().toLower();
    QJsonObject o;
    if (action == "state") {
    } else if (action == "seek") {
        if (!a.contains("at")) fail("BAD_ARGUMENT", "seek needs 'at' (a time)");
        h->liveSeek(parseTime(a["at"], p->format(), "at"));
    } else if (action == "play" || action == "stop") {
        if (a.contains("at")) h->liveSeek(parseTime(a["at"], p->format(), "at"));
        h->livePlay(action == "play");
    } else if (action == "select") {
        QSet<int> ids;
        for (const QJsonValue& v : a["clips"].toArray()) {
            const int id = v.toVariant().toInt();
            if (!TimelineOps::findClip(p->timeline(), id)) fail("NOT_FOUND", QString("no clip with id %1 in the open timeline").arg(id));
            ids.insert(id);
        }
        h->liveSelection()->set(ids);
    } else if (action == "undo" || action == "redo") {
        QUndoStack* u = p->undoStack();
        const bool undo = action == "undo";
        if (undo ? !u->canUndo() : !u->canRedo()) fail("NOTHING_TO_DO", "nothing to " + action);
        o[undo ? "undone" : "redone"] = undo ? u->undoText() : u->redoText();
        undo ? u->undo() : u->redo();
    } else if (action == "save") {
        QString error;
        if (!h->liveSave(&error)) fail("SAVE_FAILED", error);
        o["saved"] = h->liveProjectPath();
    } else if (action == "proxies") {
        QStringList files;
        for (const QJsonValue& v : a["files"].toArray()) files << v.toString();
        if (files.isEmpty())
            for (const MediaInfo& m : p->media())
                if (m.hasVideo && !m.isImage) files << m.path;
        if (a["generate"].toBool(true)) h->liveGenerateProxies(files);
        QJsonArray list;
        for (const QString& f : files) {
            QJsonObject st = h->liveProxyState(f);
            st["file"] = f;
            list << st;
        }
        o["proxies"] = list;
    } else {
        fail("BAD_ARGUMENT", "action must be state, seek, play, stop, select, undo, redo, save or proxies");
    }
    o["app"] = Live::appState(h);
    return o;
}

} // namespace Cli::detail
