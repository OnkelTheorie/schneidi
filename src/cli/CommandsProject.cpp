// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: commands on projects (new, info, import, edit, backups, restore, render, effects) and the app
#include "cli/CommandsDetail.h"
#include "cli/Live.h"
#include "cli/Sandbox.h"

#include "core/Accents.h"
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
#include "core/ReviewSheet.h"
#include "core/Selection.h"
#include "core/StartTemplates.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/ExportWriter.h"
#include "engine/Exporter.h"
#include "engine/HardwareEncoders.h"
#include "engine/ClipFormat.h"
#include "engine/ProxyCleanup.h"
#include "engine/RenderQueue.h"
#include "engine/Snapshot.h"
#include "engine/Stabilizer.h"
#include "engine/picture/ReviewOverlay.h"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Cli::detail {

namespace {

// A timeline as `info` with summary shows it: counts instead of the lists (the full JSON of a long timeline is big)
QJsonObject summarized(QJsonObject tl)
{
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
    if (tl.contains("notes")) tl["notes"] = tl["notes"].toArray().size(); // story sections stay (a few, small)
    QJsonArray subs;
    for (const QJsonValue& v : tl["subtitles"].toArray()) {
        QJsonObject st = v.toObject();
        st["cues"] = st["cues"].toArray().size();
        st.remove("style");
        subs << st;
    }
    if (!subs.isEmpty()) tl["subtitles"] = subs;
    else tl.remove("subtitles");
    return tl;
}

// Every clip and subtitle of every timeline as `info` shows it, by timeline and id: what an edit changed
struct Shown {
    int sequence = 0;
    QJsonObject json;
};
QMap<QString, Shown> shownItems(const Project& p)
{
    QMap<QString, Shown> out;
    const ProjectFormat& f = p.format();
    for (const Sequence& q : p.sequences()) {
        const Timeline& tl = q.id == p.currentSequence() ? p.timeline() : q.timeline;
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (int i = 0; i < tl.tracks(k).size(); ++i)
                for (const Clip& c : tl.tracks(k)[i].clips)
                    out.insert(QString("%1/c%2").arg(q.id).arg(c.id), {q.id, clipJson(p, c, {k, i})});
        for (int i = 0; i < tl.subtitles.size(); ++i)
            for (const SubtitleCue& c : tl.subtitles[i].cues)
                out.insert(QString("%1/s%2").arg(q.id).arg(c.id),
                           {q.id, QJsonObject{{"id", c.id}, {"track", QString("ST%1").arg(i + 1)}, {"start", c.start},
                                              {"end", c.end}, {"start_tc", tc(c.start, f)}, {"end_tc", tc(c.end, f)},
                                              {"text", c.text}}});
    }
    return out;
}

// What an edit added, changed and removed (clips and subtitles); an item of another timeline than `shown` names it.
// Long lists only with the place of each item (a `format` change touches every clip; `info` has the rest).
// A changed item: where it is (id, track, start, end) and the values that changed - keyframes as their count per
// parameter, a value that went back to its default as null (accents or a transform on 20 clips answered with every
// keyframe of every clip, 30-40 KB)
QJsonObject changedJson(const QJsonObject& before, const QJsonObject& after)
{
    QJsonObject j{{"id", after["id"]}, {"track", after["track"]}, {"start", after["start"]}, {"end", after["end"]}};
    for (auto it = after.begin(); it != after.end(); ++it) {
        if (j.contains(it.key()) || before.value(it.key()) == it.value()) continue;
        if (it.key() != "keyframes" || !it.value().isObject()) {
            j[it.key()] = it.value();
            continue;
        }
        QJsonObject counts;
        const QJsonObject keys = it.value().toObject();
        for (auto k = keys.begin(); k != keys.end(); ++k) counts[k.key()] = k.value().toArray().size();
        j["keyframes"] = counts;
    }
    for (auto it = before.begin(); it != before.end(); ++it)
        if (!after.contains(it.key())) j[it.key()] = QJsonValue::Null;
    return j;
}

QJsonObject changesJson(const QMap<QString, Shown>& before, const QMap<QString, Shown>& after, int shown)
{
    QVector<Shown> added, changed, removed;
    for (auto it = after.begin(); it != after.end(); ++it) {
        const auto old = before.find(it.key());
        if (old == before.end()) added << it.value();
        else if (old.value().json != it.value().json)
            changed << Shown{it.value().sequence, changedJson(old.value().json, it.value().json)};
    }
    for (auto it = before.begin(); it != before.end(); ++it)
        if (!after.contains(it.key()))
            removed << Shown{it.value().sequence, {{"id", it.value().json["id"]}, {"track", it.value().json["track"]}}};
    const auto list = [shown](const QVector<Shown>& items) {
        QJsonArray out;
        const bool brief = items.size() > 40;
        for (const Shown& s : items) {
            QJsonObject j = s.json;
            if (brief) j = QJsonObject{{"id", j["id"]}, {"track", j["track"]}, {"start", j["start"]}, {"end", j["end"]}};
            if (s.sequence != shown) j["timeline"] = s.sequence;
            out << j;
        }
        return out;
    };
    QJsonObject o;
    if (!added.isEmpty()) o["added"] = list(added);
    if (!changed.isEmpty()) o["changed"] = list(changed);
    if (!removed.isEmpty()) o["removed"] = list(removed);
    return o;
}

// The start template of a new project: id or name, a template file or another project; empty = the user's default
StartTemplate startTemplate(const QString& given)
{
    const QString key = given.trimmed();
    if (key.isEmpty()) return StartTemplates::defaultTemplate();
    if (key.endsWith(".json", Qt::CaseInsensitive)
        || key.endsWith("." + QString(ProjectFile::Extension), Qt::CaseInsensitive)) {
        const QString path = absolute(key);
        if (!QFileInfo::exists(path)) fail("NOT_FOUND", "template file not found: " + path);
        QString error;
        const std::optional<StartTemplate> t = StartTemplates::load(path, &error);
        if (!t) fail("BAD_ARGUMENT", QString("template %1: %2").arg(path, error));
        return *t;
    }
    if (const std::optional<StartTemplate> t = StartTemplates::find(key)) return *t;
    QStringList ids;
    for (const StartTemplate& t : StartTemplates::all()) ids << t.id;
    fail("NOT_FOUND", QString("no start template '%1' (there are: %2; or a template file)").arg(key, ids.join(", ")));
}

// A start template as `templates` shows it: the project `new` makes from it, as `info` shows a project
QJsonObject templateJson(const StartTemplate& t, const QString& defaultId)
{
    Project p;
    p.load(StartTemplates::apply(t));
    QJsonObject o{{"id", t.id}, {"name", t.displayName()}, {"built_in", t.builtIn}};
    if (const QString d = t.displayDescription(); !d.isEmpty()) o["description"] = d;
    if (t.id == defaultId) o["default"] = true;
    if (!t.builtIn && !Sandbox::active()) o["file"] = t.path;
    QJsonObject format = formatJson(p.format());
    format["fixed_resolution"] = p.fixedResolution();
    format["fixed_frame_rate"] = p.fixedFrameRate();
    o["format"] = format;
    const QJsonObject tl = summarized(timelineJson(p));
    QJsonArray tracks;
    for (const QJsonValue& v : tl["tracks"].toArray()) {
        QJsonObject track = v.toObject();
        track.remove("clips");
        tracks << track;
    }
    o["tracks"] = tracks;
    if (tl.contains("subtitles")) {
        QJsonArray subtitles;
        for (const QJsonValue& v : tl["subtitles"].toArray()) {
            QJsonObject st = v.toObject();
            st.remove("cues");
            subtitles << st;
        }
        o["subtitles"] = subtitles;
    }
    o["master"] = tl["master"];
    if (p.importPresets()) o["import_presets"] = importPresetsJson(*p.importPresets());
    return o;
}

} // namespace

// ---------- Commands ----------

QJsonObject cmdNew(const QJsonObject& a, Context&)
{
    const QString path = absolute(a["project"].toString());
    if (!path.endsWith("." + QString(ProjectFile::Extension)))
        fail("BAD_ARGUMENT", QString("project file must end in .%1").arg(ProjectFile::Extension));
    const bool replaces = QFileInfo::exists(path);
    if (replaces && !a["overwrite"].toBool())
        fail("EXISTS", "project already exists (pass overwrite to replace it): " + path);
    // Not under the app's feet: it would keep its own state and save it over the new file
    const LiveHost* h = Live::host();
    if (replaces && ((h && QFileInfo(h->liveProjectPath()) == QFileInfo(path)) || (!h && Live::connectionFor(path))))
        fail("IN_USE", "the project is open in the schneidi app; close it there first: " + path);
    const StartTemplate t = startTemplate(a["template"].toString());
    ProjectData d = StartTemplates::apply(t);
    // What is not given comes from the first video file (frame rate, and the size unless width or height is given),
    // as the app offers for the first clip in an empty timeline: an AI once cut 60-fps gameplay in a 25-fps project.
    // A start template's fixed frame rate or resolution stays; no video (sound, stills only): the template's format
    QString formatFrom;
    const bool sizeGiven = a.contains("width") || a.contains("height");
    if ((!a.contains("fps") && !d.fixedFrameRate) || (!sizeGiven && !d.fixedResolution))
        for (const QJsonValue& v : a["media"].toArray()) {
            const ClipFormat cf = detectClipFormat(absolute(v.toString()));
            if (!cf.ok) continue;
            if (!a.contains("fps") && !d.fixedFrameRate && cf.suggested.num > 0) d.format.rate = cf.suggested;
            const int w = cf.width & ~1, h = cf.height & ~1;
            if (!sizeGiven && !d.fixedResolution && w >= 16 && h >= 16 && w <= 8192 && h <= 8192) {
                d.format.width = w;
                d.format.height = h;
            }
            formatFrom = absolute(v.toString());
            break;
        }
    d.format = parseFormat(a, d.format);
    Project p;
    p.load(d);
    for (const QJsonValue& v : a["media"].toArray()) {
        const MediaInfo info = Engine::probe(p.format(), v.toString());
        if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) fail("BAD_MEDIA", unreadableMessage(absolute(v.toString())));
        p.addMedia(info);
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    // Replacing: like every writing command under the write lock, and the old state goes into the backups
    // (`restore` brings it back)
    std::unique_ptr<QLockFile> lock;
    QString backup;
    if (replaces) {
        lock = ProjectFile::lock(path);
        if (!lock) fail("PROJECT_LOCKED", "the project is being saved by someone else (schneidi app?), try again: " + path);
        backup = ProjectFile::backup(path);
    }
    QString error;
    if (!ProjectFile::save(p.data(), path, &error)) fail("SAVE_FAILED", error);
    QJsonObject o = projectJson(p, path);
    o["template"] = QJsonObject{{"id", t.id}, {"name", t.displayName()}};
    if (!formatFrom.isEmpty()) o["format_from"] = formatFrom; // the video whose frame rate/size the project took
    if (!backup.isEmpty()) o["backup"] = backup;
    return o;
}

QJsonObject cmdStartTemplates(const QJsonObject& a, Context&)
{
    const QString def = StartTemplates::defaultTemplate().id;
    if (!a["template"].toString().trimmed().isEmpty()) return templateJson(startTemplate(a["template"].toString()), def);
    QJsonArray list;
    for (const StartTemplate& t : StartTemplates::all()) list << templateJson(t, def);
    return {{"default", def}, {"templates", list}};
}

QJsonObject cmdInfo(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString());
    s.selectTimeline(a["timeline"]);
    QJsonObject o = projectJson(s.project, s.path);
    if (s.live) o["app"] = s.app; // open in the app: what the user sees there
    // Watch folders: how many, and the revision of the newest file they brought in (`watch` since)
    if (!s.project.watchFolders().isEmpty() || s.project.watchRevision() > 0)
        o["watch"] = QJsonObject{{"folders", int(s.project.watchFolders().size())}, {"revision", s.project.watchRevision()}};
    if (a["summary"].toBool()) {
        o["timeline"] = summarized(o["timeline"].toObject());
        o["media"] = o["media"].toArray().size();
    }
    return o;
}

QJsonObject cmdImport(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString(), Session::Access::Write);
    if (!a.contains("files") && !a.contains("timeline_file")) fail("BAD_ARGUMENT", "give 'files' (media) or 'timeline_file'");
    QString firstVideo;
    for (const QJsonValue& v : a["files"].toArray()) {
        const bool known = s.project.mediaInfo(absolute(v.toString()));
        const MediaInfo& m = s.media(v.toString());
        if (!known && firstVideo.isEmpty() && m.hasVideo && !m.isImage) firstVideo = m.path;
    }
    const QJsonObject imported = a.contains("timeline_file") ? importTimeline(s, a) : QJsonObject();
    const QString backup = s.save();
    QJsonObject o = projectJson(s.project, s.path);
    o.remove("timeline");
    for (auto it = imported.begin(); it != imported.end(); ++it) o[it.key()] = it.value();
    if (!backup.isEmpty()) o["backup"] = backup;
    if (const QString note = frameRateNote(s.project, firstVideo); !note.isEmpty()) o["notes"] = QJsonArray{note};
    if (s.live) o["live"] = true;
    return o;
}

QString frameRateNote(const Project& p, const QString& video)
{
    // Material in another frame rate than the project, which can only change while every timeline is empty (an AI
    // once cut 60-fps gameplay in a 25-fps project). The first new video only: detecting runs ffprobe.
    if (video.isEmpty() || p.fixedFrameRate()) return {}; // fixed: the user's choice (start template)
    bool empty = TimelineOps::endFrame(p.timeline()) == 0;
    for (const Sequence& q : p.sequences())
        if (q.id != p.currentSequence()) empty = empty && TimelineOps::endFrame(q.timeline) == 0;
    if (!empty) return {};
    const ClipFormat cf = detectClipFormat(video);
    const double project = p.format().rate.fps();
    if (!cf.ok || std::abs(cf.rate.fps() - project) <= 0.01) return {};
    return QString("%1 has %2 fps%3, the project %4 fps: while the timeline is empty, the edit op `format` with fps %5 "
                   "matches the material (later it cannot change)")
        .arg(QFileInfo(video).fileName())
        .arg(std::round(cf.rate.fps() * 1000) / 1000)
        .arg(cf.variable ? " (variable frame rate)" : "")
        .arg(std::round(project * 1000) / 1000)
        .arg(std::round(cf.suggested.fps() * 1000) / 1000);
}

// A new plan id: p1, p2 … per folder, never given twice (a counter file; an old id never names a newer plan). The
// plan's file is created here exclusively (QFile::NewOnly), so two processes planning at the same moment cannot get
// the same id: the one that loses the race takes the next number
QString newPlanId(const QString& dir)
{
    QFile counter(QDir(dir).filePath("next"));
    int next = 1;
    if (counter.open(QIODevice::ReadOnly)) next = std::max(1, counter.readAll().trimmed().toInt());
    counter.close();
    for (int tries = 0;; ++next, ++tries) {
        QFile plan(QDir(dir).filePath(QString("p%1.json").arg(next)));
        if (plan.open(QIODevice::WriteOnly | QIODevice::NewOnly)) break;
        if (!plan.exists() || tries > 100000) fail("SAVE_FAILED", "cannot write a plan into " + dir);
    }
    if (counter.open(QIODevice::WriteOnly | QIODevice::Truncate)) counter.write(QByteArray::number(next + 1));
    return QString("p%1").arg(next);
}

namespace {

// ---------- Plans (edit plan:true, then plan_id + run or discard) ----------

constexpr int kPlanHours = 24; // a plan nobody ran or discarded is gone after this

// Plans of a project live beside its backups (AppDataLocation, portable); a project in the app never saved has
// one folder for all of them (the project state in each plan tells them apart)
QString planDir(const QString& projectPath)
{
    if (projectPath.isEmpty()) return QDir(ProjectFile::backupDir({})).filePath("unsaved-plans");
    return QDir(ProjectFile::backupDir(projectPath)).filePath("plans");
}

// The project a plan is made on, as a fingerprint: a plan runs only on exactly that state. Not part of it: what does
// not change what the ops do (playhead, render queue, which timeline is open - the plan names its timeline, where
// the project file is).
// Inside the app the app's own project (a planning session there edits a copy of it).
QByteArray planBase(const Session& s, const QString& path)
{
    ProjectData d = Live::host() ? Live::host()->liveProject()->data() : s.project.data();
    d.playhead = 0;
    d.renderQueue.clear();
    d.currentSequence = -1; // every timeline in "sequences"
    d.timeline = emptyTimeline();
    // Not the path either: a plan made while the project was never saved runs once it is saved under a name
    // (media paths are absolute in the file; relative ones against a fixed folder)
    Q_UNUSED(path);
    return QCryptographicHash::hash(ProjectFile::toJson(d, QDir::rootPath() + "plan.schneidi"), QCryptographicHash::Sha256).toHex();
}

// Removes plans older than kPlanHours; returns the ids of those left
QStringList prunePlans(const QString& dir)
{
    QStringList ids;
    const QDateTime limit = QDateTime::currentDateTimeUtc().addSecs(-kPlanHours * 3600);
    for (const QFileInfo& fi : QDir(dir).entryInfoList({"p*.json"}, QDir::Files, QDir::Name)) {
        if (fi.lastModified().toUTC() < limit) QFile::remove(fi.absoluteFilePath());
        else ids << fi.completeBaseName();
    }
    std::sort(ids.begin(), ids.end(), [](const QString& x, const QString& y) { return x.mid(1).toInt() < y.mid(1).toInt(); });
    return ids;
}

// The clip ids an op names: clip, clips, and {clip:id} inside it (a relative time)
QVector<int> clipRefs(const QJsonValue& v, const QString& key = {})
{
    QVector<int> out;
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) out += clipRefs(it.value(), it.key());
    } else if (v.isArray()) {
        for (const QJsonValue& x : v.toArray()) out += clipRefs(x, key);
    } else if (v.isDouble() && (key == "clip" || key == "clips")) {
        out << v.toInt();
    }
    return out;
}

// One line for a step: what it changes in numbers ("keep: 2 added, 1 changed")
QString stepSummary(const QString& op, const QJsonObject& changes)
{
    QStringList parts;
    if (const int n = changes["added"].toArray().size()) parts << QString("%1 added").arg(n);
    if (const int n = changes["changed"].toArray().size()) parts << QString("%1 changed").arg(n);
    if (const int n = changes["removed"].toArray().size()) parts << QString("%1 removed").arg(n);
    return op + ": " + (parts.isEmpty() ? QString("no clip or subtitle changes") : parts.join(", "));
}

// Applies `ops` in order (all or nothing) and answers like `edit`. steps (plan): what each op would change.
// numbers: the plan's step numbers of the ops (errors name them), empty = op indexes.
QJsonObject applyEdit(Session& s, const QJsonArray& ops, const QJsonObject& a, bool dry, QJsonArray* steps = nullptr,
                      const QVector<int>& numbers = {})
{
    const QMap<QString, Shown> before = shownItems(s.project);
    QMap<QString, Shown> last = before;
    for (int i = 0; i < ops.size(); ++i) {
        try {
            if (!ops[i].isObject()) fail("BAD_ARGUMENT", "operation is not an object");
            s.opIndex = i;
            applyOp(s, ops[i].toObject());
            s.opIndex = -1;
        } catch (const Error& e) {
            if (!numbers.isEmpty())
                throw Error{e.code, QString("step %1 (%2): %3").arg(numbers.value(i)).arg(ops[i]["op"].toString(), e.message)};
            throw Error{e.code, QString("op %1 (%2): %3").arg(i).arg(ops[i]["op"].toString(), e.message)};
        }
        if (steps) {
            const QMap<QString, Shown> now = shownItems(s.project);
            const QJsonObject op = ops[i].toObject();
            const QJsonObject changes = changesJson(last, now, s.project.currentSequence());
            QJsonObject step{{"n", i + 1}, {"op", op}, {"summary", stepSummary(op["op"].toString(), changes)},
                             {"changes", changes}};
            if (const QString why = op["why"].toString().trimmed(); !why.isEmpty()) step["why"] = why;
            *steps << step;
            last = now;
        }
    }
    // The timeline edited last (save() may switch back to the one the app opens): what changed and a summary; the
    // whole timeline only on request (an AI reads every answer: it was up to 56 KB per edit)
    const QJsonObject tl = timelineJson(s.project);
    QJsonObject o{{"project", s.path}, {"ops", ops.size()}, {"saved", !dry && !s.live},
                  {"timeline", a["full"].toBool() ? tl : summarized(tl)},
                  {"changes", changesJson(before, shownItems(s.project), s.project.currentSequence())}};
    if (s.live) o["live"] = true; // changed in the app (one undo step there), the user saves
    if (!s.notes.isEmpty()) o["notes"] = s.notes;
    if (!s.why.isEmpty()) o["why"] = s.why; // the reasons the ops were given, by op index
    if (s.project.sequences().size() > 1) o["timelines"] = timelinesJson(s.project, s.multicamSync);
    if (!dry) {
        const QString backup = s.save();
        if (!backup.isEmpty()) o["backup"] = backup;
    }
    return o;
}

// edit plan_id + run|discard: runs the chosen steps of a plan as one edit (one undo step in the app) or drops it
QJsonObject runPlan(const QJsonObject& a)
{
    const QString id = a["plan_id"].toString().trimmed().toLower();
    const bool discard = a["discard"].toBool();
    if (a.contains("ops") || a["plan"].toBool())
        fail("BAD_ARGUMENT", "plan_id runs the plan's own ops: give run (step numbers or \"all\") or discard:true, "
                             "no 'ops' or 'plan'");
    if (!discard && !a.contains("run"))
        fail("BAD_ARGUMENT", "plan_id needs run:[step numbers] (or \"all\") or discard:true");
    if (discard && a.contains("run")) fail("BAD_ARGUMENT", "run or discard, not both");
    // dry_run: what running the steps would change, nothing saved, the plan stays (a test run changed the project)
    const bool dry = a["dry_run"].toBool();
    Session s(a["project"].toString(), discard || dry ? Session::Access::Read : Session::Access::Write);
    QString dir = planDir(s.path);
    QStringList ids = prunePlans(dir);
    static const QRegularExpression valid("^p[1-9][0-9]{0,8}$");
    // Made before the project was first saved: in the folder of unsaved projects (its base tells whether it fits)
    if (!s.path.isEmpty() && valid.match(id).hasMatch() && !QFileInfo::exists(QDir(dir).filePath(id + ".json"))
        && QFileInfo::exists(QDir(planDir({})).filePath(id + ".json"))) {
        dir = planDir({});
        ids = prunePlans(dir);
    }
    const QString file = QDir(dir).filePath(id + ".json");
    QFile f(file);
    if (!valid.match(id).hasMatch() || !f.open(QIODevice::ReadOnly))
        fail("NOT_FOUND", QString("no plan '%1' for this project (%2; a plan is gone once it was run or discarded, "
                                  "and after %3 h)")
                              .arg(a["plan_id"].toString(), ids.isEmpty() ? QString("it has none")
                                                                          : "it has: " + ids.join(", "))
                              .arg(kPlanHours));
    const QJsonObject plan = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    if (discard) {
        QFile::remove(file);
        return {{"project", s.path}, {"plan", QJsonObject{{"id", id}, {"discarded", true}}}};
    }
    if (planBase(s, s.path) != plan["base"].toString().toLatin1())
        fail("PLAN_STALE", QString("the project changed since plan %1 was made (an edit, the user's own changes or an "
                                   "undo); nothing was run - look at `info` and plan again (edit with plan:true)")
                               .arg(id));
    const QJsonArray all = plan["ops"].toArray();
    QVector<int> chosen;
    const QJsonValue run = a["run"];
    const bool everything = (run.isString() && run.toString().trimmed().toLower() == "all")
                            || (run.isArray() && run.toArray().size() == 1 && run.toArray()[0].toString().toLower() == "all");
    if (everything)
        for (int n = 1; n <= all.size(); ++n) chosen << n;
    else
        for (const QJsonValue& v : run.toArray()) {
            bool ok = v.isDouble();
            const int n = ok ? v.toInt() : v.toString().trimmed().toInt(&ok);
            if (!ok || n < 1 || n > all.size())
                fail("BAD_ARGUMENT", QString("plan %1 has steps 1 to %2, not '%3'").arg(id).arg(all.size()).arg(v.toVariant().toString()));
            if (!chosen.contains(n)) chosen << n;
        }
    if (chosen.isEmpty()) fail("BAD_ARGUMENT", "run: no step chosen (step numbers like [1,2,4] or \"all\")");
    std::sort(chosen.begin(), chosen.end()); // always in the plan's order
    QJsonArray ops;
    for (int n : chosen) ops << all[n - 1];
    // A step that works on a clip a step left out makes: the clip does not exist then, or (ids are handed out in
    // order) another one has its id - refused instead of changing the wrong clip
    const QJsonArray adds = plan["adds"].toArray();
    QHash<int, int> madeBy; // clip id -> the step that adds it
    for (int n = 1; n <= adds.size(); ++n)
        for (const QJsonValue& v : adds[n - 1].toArray()) madeBy.insert(v.toInt(), n);
    for (int k = 0; k < chosen.size(); ++k)
        for (int id : clipRefs(all[chosen[k] - 1].toObject()))
            if (const int by = madeBy.value(id); by && !chosen.contains(by))
                fail("BAD_ARGUMENT", QString("step %1 works on clip %2 that step %3 makes: run step %3 too (or not %1)")
                                         .arg(chosen[k])
                                         .arg(id)
                                         .arg(by));
    if (const int seq = plan["timeline"].toInt(); s.project.sequence(seq)) s.selectTimeline(seq);
    QJsonObject o = applyEdit(s, ops, a, dry, nullptr, chosen);
    if (dry) {
        o["plan"] = QJsonObject{{"id", id}, {"dry_run", true}, {"would_run", QJsonArray::fromVariantList([&] {
                                     QVariantList l;
                                     for (int n : chosen) l << n;
                                     return l;
                                 }())}};
        return o;
    }
    QFile::remove(file); // done: the project is another one now
    if (o.contains("why")) { // by the plan's step numbers, not by the index among the steps run
        QJsonArray why;
        for (const QJsonValue& v : o["why"].toArray()) {
            QJsonObject w = v.toObject();
            if (w.contains("index")) w["step"] = chosen.value(w.take("index").toInt());
            why << w;
        }
        o["why"] = why;
    }
    QJsonArray ran, skipped;
    for (int n = 1; n <= all.size(); ++n) (chosen.contains(n) ? ran : skipped) << n;
    QJsonObject done{{"id", id}, {"ran", ran}};
    if (!skipped.isEmpty()) done["skipped"] = skipped;
    o["plan"] = done;
    return o;
}

} // namespace

QJsonObject cmdEdit(const QJsonObject& a, Context&)
{
    if (a.contains("plan_id")) return runPlan(a);
    if (a.contains("run") || a.contains("discard"))
        fail("BAD_ARGUMENT", "run/discard belong to a plan: give plan_id (from the answer of edit with plan:true)");
    const bool plan = a["plan"].toBool();
    const bool dry = a["dry_run"].toBool() || plan;
    Session s(a["project"].toString(), dry ? Session::Access::Read : Session::Access::Write);
    const QByteArray base = plan ? planBase(s, s.path) : QByteArray();
    s.selectTimeline(a["timeline"]);
    const int sequence = s.project.currentSequence();
    QJsonValue opsValue = a["ops"];
    if (opsValue.isObject()) opsValue = QJsonArray{opsValue};
    QJsonArray ops = opsValue.toArray();
    if (ops.isEmpty()) fail("BAD_ARGUMENT", "'ops' must be a non-empty list of operations");
    // A plan keeps where the app's playhead is now: `speak` without `at` lands there, not where the playhead is when
    // the user runs the plan (the playhead is not part of the plan's base)
    if (plan && s.app.contains("playhead"))
        for (int i = 0; i < ops.size(); ++i) {
            QJsonObject op = ops[i].toObject();
            if (op.value("op").toString() != "speak" || op.contains("at")) continue;
            op["at"] = s.app.value("playhead").toInt(0);
            ops[i] = op;
        }
    QJsonArray steps;
    QJsonObject o = applyEdit(s, ops, a, dry, plan ? &steps : nullptr);
    if (!plan) return o;
    // A plan: nothing changed; the ops (as given, with their reasons) wait for the user's choice
    const QString dir = planDir(s.path);
    if (!QDir().mkpath(dir)) fail("SAVE_FAILED", "cannot write the plan into " + dir);
    prunePlans(dir);
    const QString id = newPlanId(dir);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonArray adds; // per step: the ids of the clips it adds (runPlan: a step on a clip of a step left out)
    for (const QJsonValue& st : steps) {
        QJsonArray ids;
        for (const QJsonValue& c : st["changes"]["added"].toArray())
            if (c["id"].isDouble()) ids << c["id"];
        adds << ids;
    }
    const QJsonObject stored{{"id", id},         {"created", now.toString(Qt::ISODate)}, {"project", s.path},
                             {"timeline", sequence}, {"base", QString::fromLatin1(base)},  {"ops", ops},
                             {"adds", adds}};
    QFile f(QDir(dir).filePath(id + ".json"));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(stored).toJson(QJsonDocument::Compact)) < 0)
        fail("SAVE_FAILED", "cannot write the plan " + f.fileName());
    f.close();
    int added = 0, changed = 0, removed = 0;
    const QJsonObject total = o["changes"].toObject();
    added = total["added"].toArray().size();
    changed = total["changed"].toArray().size();
    removed = total["removed"].toArray().size();
    o["saved"] = false;
    o["plan"] = QJsonObject{
        {"id", id},
        {"steps", steps},
        {"summary", QString("%1 step(s): %2 added, %3 changed, %4 removed").arg(ops.size()).arg(added).arg(changed).arg(removed)},
        {"expires", now.addSecs(kPlanHours * 3600).toString(Qt::ISODate)},
        {"next", QString("nothing changed yet: run the steps with edit {plan_id:'%1', run:[1,2,…] or \"all\"} (one "
                         "edit, one undo step in the app), or edit {plan_id:'%1', discard:true}; the plan is void "
                         "once the project changes").arg(id)}};
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
    if (files.contains(which)) return which; // a file from `backups` (outside a server's work folder)
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

namespace {

// File extensions a render format can write: of the usual ones for its kind, those whose container takes its codecs
// (the format's own first)
QStringList fittingExtensions(const RenderFormatInfo& f)
{
    static const QStringList video{"mp4", "mov", "mkv", "webm"};
    static const QStringList audio{"m4a", "mp3", "opus", "ogg", "wav", "aiff", "aif", "flac", "mka"};
    QStringList out{f.extension};
    for (const QString& ext : f.kind == RenderKind::Audio ? audio : video)
        if (!out.contains(ext)
            && ExportWriter::containerSurelyTakes("x." + ext, f.videoCodec, f.audioCodec, f.kind == RenderKind::Audio))
            out << ext;
    return out;
}

// The output file's extension must fit the format (the format decides the codecs, the extension the container):
// "out.webm" with the default h264 or "out.mp4" with wav would fail late or write an odd file
void checkExtension(const QString& out, const QString& formatId)
{
    const RenderFormatInfo& f = renderFormat(formatId);
    if (f.kind != RenderKind::Video && f.kind != RenderKind::Audio) return; // GIF, image sequences: named by the format
    const QString ext = QFileInfo(out).suffix().toLower();
    const QStringList fits = fittingExtensions(f);
    // The common ones, or any other whose container surely takes the codecs (.m4v, .avi, .mxf, .aac …); for video
    // also one that does not refuse them (MPEG-TS takes H.264 without saying so). Sound stays strict: ogg/flac/opus
    // say nothing and refuse AAC at the header
    const bool sound = f.kind == RenderKind::Audio; // a sound format into .mp4/.mov: an odd file, refused
    if (fits.contains(ext) || ExportWriter::containerSurelyTakes(out, f.videoCodec, f.audioCodec, sound)) return;
    if (f.kind == RenderKind::Video && ExportWriter::containerTakes(out, {}, f.videoCodec, f.audioCodec)) return;
    QStringList formats; // formats that write this extension themselves
    for (const RenderFormatInfo& i : renderFormats())
        if (ext == QLatin1String(i.extension) && HardwareEncoders::formatAvailable(i.id)) formats << i.id;
    QString message = QString("'.%1' does not fit format %2: use .%3").arg(ext, formatId, fits.join(", ."));
    if (!formats.isEmpty()) message += QString(" - or format %1 for .%2").arg(formats.join(" / "), ext);
    fail("BAD_ARGUMENT", message);
}

} // namespace

QJsonObject cmdRender(const QJsonObject& a, Context& ctx)
{
    Session s(a["project"].toString());
    s.selectTimeline(a["timeline"]);
    // Review render (cli/RenderReview.cpp): a small fast video with the marks burned in, or one picture of the timeline
    const QString review = a["review"].toString();
    if (a.contains("review") && review != "video" && review != "sheet")
        fail("BAD_ARGUMENT", "review must be video (a small fast render with beats, cuts, hits, sections burned in) or "
                             "sheet (one PNG of the whole timeline)");
    const bool sheet = review == "sheet", reviewVideo = review == "video";
    RenderJob job;
    if (a.contains("preset")) {
        // Deliver presets of the app (built-in and the user's own), by name or id ("tiktok", "youtube-1080p")
        const QString name = a["preset"].toString();
        const std::optional<RenderPreset> p = RenderPresets::find(name);
        if (!p) {
            QStringList names;
            for (const RenderPreset& q : RenderPresets::all()) names << (q.id.isEmpty() ? q.name : q.id);
            fail("NOT_FOUND", QString("no preset '%1' (there are: %2)").arg(name, names.join(", ")));
        }
        job.settings = p->settings;
        job.preset = p->name;
    }
    const QString formatId = a.contains("format") ? a["format"].toString() : job.settings.format;
    const auto& formats = renderFormats();
    const bool srtOnly = formatId == "srt" || formatId == "vtt"; // only the subtitle files (SRT or WebVTT)
    if (!srtOnly && std::none_of(formats.begin(), formats.end(), [&](const RenderFormatInfo& i) { return formatId == i.id; })) {
        QStringList ids;
        for (const RenderFormatInfo& i : formats) ids << i.id;
        fail("BAD_ARGUMENT", "unknown format " + formatId + " (" + ids.join(", ") + ", srt)");
    }
    if (!srtOnly && !HardwareEncoders::formatAvailable(formatId)) {
        QStringList ids;
        for (const RenderFormatInfo& i : formats)
            if (HardwareEncoders::formatAvailable(i.id)) ids << i.id;
        fail("BAD_ARGUMENT", "format " + formatId + " has no encoder in this build of FFmpeg (available: " + ids.join(", ")
                                  + ", srt)");
    }
    if (reviewVideo && (srtOnly || renderFormat(formatId).kind != RenderKind::Video))
        fail("BAD_ARGUMENT", "review video needs a video format (h264 default, h265, av1, vp9, prores …)");
    QString out = absolute(a["out"].toString());
    if (QFileInfo(out).suffix().isEmpty())
        out += "." + QString(sheet ? QStringLiteral("png") : srtOnly ? formatId : QString(renderFormat(formatId).extension));
    if (sheet) {
        if (!QStringList{"png", "jpg", "jpeg"}.contains(QFileInfo(out).suffix().toLower()))
            fail("BAD_ARGUMENT", "review sheet writes a picture: out must end in .png (or .jpg)");
    } else if (!srtOnly) {
        checkExtension(out, formatId);
    }
    RenderSettings target = job.settings;
    if (!srtOnly) target.format = formatId;
    if (QFileInfo::exists(sheet ? out : firstOutputFile(out, target)) && !a["overwrite"].toBool())
        fail("EXISTS", "output exists (pass overwrite to replace it): " + out);
    job.path = out;
    if (!srtOnly) job.settings.format = formatId;
    if (a.contains("quality")) job.settings.quality = std::clamp(a["quality"].toInt(), 0, RenderSettings::kQualityWeb);
    // Review video: small (540 p) and the fastest encoder setting unless a size is given
    if (reviewVideo && !a.contains("height") && !a.contains("size")) {
        job.settings.shortSide = 540;
        job.settings.size = {};
    }
    if (a.contains("height")) {
        // Like size: a too small one gave an empty size, so the file had the timeline's size and the answer said 2x0
        const int h = a["height"].toInt();
        if (h < 16 || h > 8192) fail("BAD_ARGUMENT", "height (the shorter image edge) must be 16 to 8192");
        job.settings.shortSide = h;
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
    // A render beside a game froze the PC (2026-10-28: all threads, 3.2 GB): the caller can keep it small
    int threads = 0;
    if (a.contains("threads")) {
        threads = a["threads"].toInt();
        if (threads < 1) fail("BAD_ARGUMENT", "threads must be 1 or more");
        threads = std::min(threads, Exporter::availableThreads());
    }
    if (a.contains("audio_bitrate")) job.settings.audioBitrateK = std::clamp(a["audio_bitrate"].toInt(), 32, 512);
    if (a.contains("subtitles")) {
        const int i = QStringList{"none", "burn", "srt", "vtt"}.indexOf(a["subtitles"].toString());
        if (i < 0)
            fail("BAD_ARGUMENT", "subtitles must be none, burn (into the picture), srt or vtt (file next to the video)");
        job.settings.subtitles = i;
    }
    if (a.contains("embed_subtitles")) {
        job.settings.embedSubtitles = a["embed_subtitles"].toBool();
        if (job.settings.embedSubtitles && !job.settings.canEmbedSubtitles())
            fail("BAD_ARGUMENT", "embed_subtitles needs a video file (not audio only, GIF or images)");
    }
    job.size = job.settings.outputSize(s.project.timelineFormat().size());
    Timeline tl = s.project.renderTimeline();
    // Tracks left out of this render only (a voice-over, a music bed): the project keeps its mixer
    for (const QJsonValue& v : a["mute_tracks"].toArray()) {
        TrackKind k = TrackKind::Video;
        const int index = parseTrack(v, &k);
        auto& tracks = tl.tracks(k);
        if (index >= tracks.size())
            fail("NOT_FOUND", QString("mute_tracks: there is no %1 (the timeline has V1-V%2 and A1-A%3)")
                                  .arg(trackName({k, index})).arg(tl.video.size()).arg(tl.audio.size()));
        if (k == TrackKind::Audio) tracks[index].muted = true;
        else tracks[index].hidden = true;
    }
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) s.failEmpty();
    if (a.contains("from") || a.contains("to")) {
        job.inOut = true;
        job.from = a.contains("from") ? parseTime(a["from"], s.format(), "from") : 0;
        job.to = (a.contains("to") ? parseTime(a["to"], s.format(), "to") : end) - 1;
        if (job.to < job.from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
    }
    QJsonObject reviewInfo;
    std::shared_ptr<const ReviewSheet::Data> marks;
    if (sheet || reviewVideo) {
        if (Exporter::readsFile(tl, out)) fail("BAD_ARGUMENT", "the output would overwrite a source file of the project");
        QElapsedTimer timer;
        timer.start();
        marks = reviewData(s, tl, end, a, ctx, sheet, &reviewInfo);
        if (sheet) {
            QJsonObject o = reviewSheetRender(s, tl, *marks, out, ctx);
            o["review"] = "sheet";
            o["render_s"] = std::round(timer.elapsed() / 100.0) / 10;
            for (auto it = reviewInfo.begin(); it != reviewInfo.end(); ++it) o[it.key()] = it.value();
            const QJsonObject numbers = reviewNumbers(*marks);
            for (auto it = numbers.begin(); it != numbers.end(); ++it) o[it.key()] = it.value();
            return o;
        }
    }
    if (srtOnly) {
        // Only the subtitle files (the visible track into out, every other one with entries next to it), times from
        // the range start
        const QVector<Subtitles::SrtFile> files = Subtitles::srtFiles(tl, out);
        if (files.isEmpty()) fail("EMPTY", "the timeline has no subtitles");
        QDir().mkpath(QFileInfo(out).absolutePath());
        QJsonArray more;
        for (const Subtitles::SrtFile& srt : files) {
            const SubtitleTrack& track = tl.subtitles[srt.track];
            QFile f(srt.path);
            if (!f.open(QIODevice::WriteOnly)
                || f.write(Subtitles::toFile(srt.path, track.cues, s.project.frameRate(), job.inOut ? job.from : 0, job.inOut ? job.to + 1 : -1)) < 0)
                fail("WRITE_FAILED", "cannot write " + srt.path);
            if (srt.path != out) more << srt.path;
        }
        QJsonObject o{{"out", out}, {"format", formatId}, {"cues", int(tl.subtitles[files[0].track].cues.size())}};
        if (!more.isEmpty()) o["srt_more"] = more; // the other subtitle tracks, next to out
        return o;
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
    ExportSettings es = RenderQueue::exportSettings(job, s.project);
    if (threads > 0) es.threads = threads;
    es.partId = ctx.partId;
    if (marks) {
        // The marks drawn onto every frame after compositing; the timeline itself stays as it is
        es.overlay = Picture::reviewOverlay(marks);
        es.smartRender = false;
        es.preset = "ultrafast";
        es.crf = std::max(es.crf, 28);
    }
    if (!ex.start(tl, es, &error)) fail("RENDER_FAILED", error);
    loop.exec();
    if (!ok) fail("RENDER_FAILED", message.isEmpty() ? QString("rendering failed") : message);
    QJsonObject o{{"out", out}, {"format", formatId}, {"render_s", std::round(timer.elapsed() / 100.0) / 10}};
    o["threads"] = ex.threads();
    if (ex.threadsLowMemory()) o["threads_reason"] = "low memory"; // fewer than the processors: little RAM free
    if (marks) {
        o["review"] = "video";
        o["size"] = QJsonArray{es.size.width(), es.size.height()};
        for (auto it = reviewInfo.begin(); it != reviewInfo.end(); ++it) o[it.key()] = it.value();
        const QJsonObject numbers = reviewNumbers(*marks);
        for (auto it = numbers.begin(); it != numbers.end(); ++it) o[it.key()] = it.value();
    }
    if (!ex.usedCodec().isEmpty()) o["encoder"] = ex.usedCodec(); // video codec that wrote the file, e.g. h264_nvenc ("copy": smart render)
    if (job.settings.kind() == RenderKind::ImageSequence) {
        o["frames_dir"] = imageSequenceDir(out);
        o["pattern"] = QFileInfo(imageSequencePattern(out)).fileName();
    }
    if (!ex.note().isEmpty()) o["encoder_note"] = ex.note();
    // Free H.264/H.265 encoder because neither a graphics card encoder nor x264/x265 is there: how to get better
    if (const QString hint = HardwareEncoders::betterQualityHint(ex.usedCodec()); !hint.isEmpty()) {
        o["encoder_hint"] = hint;
        o["better_encoder"] = QJsonObject{{"extension", "x264"}, {"install", "schneidi-cli extensions --install x264"}};
    }
    if (!ex.skippedEffects().isEmpty()) {
        // Not in this version: the clip renders without it (the project keeps it; info marks it "available": false)
        QJsonArray skipped;
        for (const Picture::SkippedEffect& e : ex.skippedEffects()) {
            QJsonObject so{{"clip", e.clip}, {"effect", e.effect}, {"kind", e.video ? "video" : "audio"}};
            if (e.sequence >= 0) so["sequence"] = e.sequence;
            skipped << so;
        }
        o["skipped_effects"] = skipped;
    }
    if (!job.settings.audioOnly()) {
        o["width"] = es.size.width();
        o["height"] = es.size.height();
    }
    if (!job.preset.isEmpty()) o["preset"] = job.preset;
    if (!es.subtitlePath.isEmpty()) {
        o["srt"] = es.subtitlePath;
        const QVector<Subtitles::SrtFile> files = Subtitles::srtFiles(tl, es.subtitlePath);
        if (files.size() > 1) { // the other subtitle tracks, next to it
            QJsonArray more;
            for (qsizetype i = 1; i < files.size(); ++i) more << files[i].path;
            o["srt_more"] = more;
        }
    }
    return o;
}


namespace {
// LUTs, luma images and presets for `effects`: built-in ones by id, the user's by folder/name
QJsonArray assetsJson(const QVector<EffectFolders::LutEntry>& list)
{
    QJsonArray out;
    for (const EffectFolders::LutEntry& e : list) {
        const bool builtin = EffectFolders::isBuiltin(e.path);
        QJsonObject o{{"name", builtin ? EffectFolders::builtinId(e.path) : (e.group.isEmpty() ? e.name : e.group + "/" + e.name)},
                      {"label", builtin ? e.name : QFileInfo(e.path).fileName()}};
        if (const QString d = EffectFolders::builtinPresetDescription(e.path); !d.isEmpty()) o["description"] = d;
        out << o;
    }
    return out;
}
} // namespace


QJsonObject cmdEffects(const QJsonObject& a, Context&)
{
    const QString kind = a["kind"].toString();
    if (kind == "sfx")
        return {{"sounds", sfxSounds()}, {"folder", EffectFolders::soundDir()},
                {"note", "built-in sounds are schneidi's own (synthesized, variants by seed); own: files in the folder"}};
    if (!kind.isEmpty()) fail("BAD_ARGUMENT", "kind: 'sfx' (or leave it out)");
    const QString only = a["effect"].toString();
    QJsonArray list;
    for (const EffectDescriptor& d : EffectRegistry::all()) {
        if (!only.isEmpty() && d.id != only) continue;
        QJsonObject o{{"effect", d.id}, {"name", d.name}, {"kind", d.video ? "video" : "audio"}};
        if (d.id == "grade")
            o["note"] = "the Color page grade (lift/gamma/gain/offset, LUT); windows (shape masks) via the `window` op";
        if (d.id == "chromakey")
            o["note"] = "own keyer (chroma distance, soft edge, spill suppression); fixed stage after the grade";
        if (d.id == Stabilizer::EffectId)
            o["note"] = "needs an analysis pass (background in the app, before encoding in `render`)";
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
    // Accents on hits (op 'accent'): what each preset does, so a model picks one instead of writing keyframes
    QJsonArray accents;
    for (const Accents::Preset& p : Accents::presets())
        accents << QJsonObject{{"preset", p.id}, {"description", p.description}};
    return {{"effects", list},
            {"luts", assetsJson(EffectFolders::builtinLuts() + EffectFolders::userLuts())},
            {"transition_types", types},
            {"lumas", assetsJson(EffectFolders::builtinTransitions() + EffectFolders::userTransitions())},
            {"presets", assetsJson(EffectFolders::builtinPresets() + EffectFolders::userPresets())},
            {"accents", accents},
            {"sounds", sfxSounds()}};
}

QString presetHelp()
{
    QStringList names;
    for (const RenderPreset& p : RenderPresets::all())
        names << (p.id.isEmpty() ? "'" + p.name + "'" : QString("%1 ('%2')").arg(p.id, p.name));
    return "Deliver preset of the app by id or name (other parameters override it): " + names.join(", ")
           + ". youtube-shorts / instagram-reels / tiktok (also 'shorts', 'reels'): 1080x1920 H.264 web quality, the "
             "timeline's frame rate up to 60 fps, AAC 48 kHz; run `check` with the same platform first (length "
             "limit, -14 LUFS, true peak)";
}

// The running app: state (playhead, selection …) and control (play, seek, select, undo, save, open, proxies, cleaning
// up proxy files)
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
        // A new file only for a project that was never saved (Save As of the user's project stays the user's)
        QString path;
        if (a.contains("path")) {
            path = absolute(a["path"].toString());
            if (QFileInfo(path).suffix() != ProjectFile::Extension) path += QString(".%1").arg(ProjectFile::Extension);
            const QString current = h->liveProjectPath();
            if (!current.isEmpty() && QFileInfo(path).absoluteFilePath() != QFileInfo(current).absoluteFilePath())
                fail("BAD_ARGUMENT", "the project already has a file (" + current
                                         + "); 'path' is only for a project that was never saved");
            if (current.isEmpty() && QFileInfo::exists(path)) fail("EXISTS", "there is a file already: " + path);
        }
        QString error;
        if (!h->liveSave(path, &error)) fail("SAVE_FAILED", error);
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
    } else if (action == "clean_proxies") {
        // Proxy files neither the app's project (unsaved changes included) nor a recent project needs
        // (engine/ProxyCleanup): listed, deleted only with delete:true
        const QStringList keep =
            ProxyCleanup::projectMedia(p->data()) + ProxyCleanup::mediaOfProjects(ProxyCleanup::recentProjects());
        const QStringList busy{h->liveProxyWriting()};
        const ProxyCleanup::Plan plan = ProxyCleanup::plan(keep, busy);
        QJsonArray files;
        for (const ProxyCleanup::Item& it : plan.items) {
            QJsonObject f{{"file", it.file}, {"reason", ProxyCleanup::reasonId(it.reason)}, {"bytes", it.bytes}};
            if (!it.original.isEmpty()) f["original"] = it.original;
            files << f;
        }
        o["files"] = files;
        o["bytes"] = plan.bytes;
        o["kept"] = plan.kept;
        o["kept_bytes"] = plan.keptBytes;
        if (a["delete"].toBool()) o["freed_bytes"] = ProxyCleanup::apply(plan, busy);
        else o["dry_run"] = true;
    } else if (action == "open") {
        // Like the user's `schneidi <file>`: a project file opens (never over unsaved changes), media come into the
        // pool and are selected, the window comes to the front
        QStringList projects, media;
        for (const QJsonValue& v : a["files"].toArray()) {
            const QString f = absolute(v.toString());
            if (!QFileInfo(f).isFile()) fail("NOT_FOUND", "no such file: " + f);
            (QFileInfo(f).suffix() == QLatin1String(ProjectFile::Extension) ? projects : media) << f;
        }
        if (projects.isEmpty() && media.isEmpty())
            fail("BAD_ARGUMENT", "open needs 'files': a .schneidi project and/or media files");
        if (projects.size() > 1) fail("BAD_ARGUMENT", "open takes one project file at a time");
        QString error;
        if (!projects.isEmpty()) {
            if (!h->liveOpen(projects, false, &error)) fail("OPEN_FAILED", error);
            o["opened"] = h->liveProjectPath();
        }
        if (!media.isEmpty()) {
            {
                Session s(QString(Live::AppProject), Session::Access::Write);
                for (const QString& f : media) s.media(f); // into the pool; throws for a file that cannot be read
            }
            if (!h->liveOpen(media, false, &error)) fail("OPEN_FAILED", error);
            QJsonArray list;
            for (const QString& f : media) list << f;
            o["media"] = list;
        }
    } else {
        fail("BAD_ARGUMENT", "action must be state, seek, play, stop, select, undo, redo, save, open, proxies or "
                             "clean_proxies");
    }
    o["app"] = Live::appState(h);
    return o;
}

} // namespace Cli::detail
