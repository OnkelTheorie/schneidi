#include "cli/Commands.h"
#include "cli/CommandsDetail.h"

#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
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

#include <algorithm>
#include <cmath>
#include <memory>

namespace Cli {

namespace detail {

void fail(const QString& code, const QString& message) { throw Error{code, message}; }

QString absolute(const QString& path) { return path.isEmpty() ? path : QFileInfo(path).absoluteFilePath(); }

// ---------- Times ----------

// Frames from 120 (frames), "120" (frames), "4.8s" (seconds) or "00:00:04:20" (timecode)
int parseTime(const QJsonValue& v, const ProjectFormat& fmt, const QString& what)
{
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d < 0 || d != std::floor(d)) fail("BAD_ARGUMENT", QString("%1: frames must be a whole number >= 0, got %2").arg(what).arg(d));
        return int(d);
    }
    const QString s = v.toString().trimmed();
    if (s.endsWith('s')) {
        bool ok = false;
        const double sec = s.chopped(1).toDouble(&ok);
        if (ok && sec >= 0) return int(std::lround(sec * fmt.rate.fps()));
    } else if (!s.isEmpty()) {
        bool ok = false;
        const int frames = s.toInt(&ok);
        if (ok && frames >= 0) return frames;
        const int tc = Timecode::parse(s, fmt.rate.timebase());
        if (tc >= 0 && s.contains(':')) return tc;
    }
    fail("BAD_ARGUMENT", QString("%1: '%2' is not a time (use frames, \"4.5s\" or \"HH:MM:SS:FF\")").arg(what, s));
}

QString tc(int frames, const ProjectFormat& fmt) { return Timecode::format(frames, fmt.rate.timebase()); }
double seconds(int frames, const ProjectFormat& fmt) { return std::round(frames / fmt.rate.fps() * 1000) / 1000; }

// [from, to) as frames, seconds and timecode
QJsonObject range(int from, int to, const ProjectFormat& fmt)
{
    return {{"from", from}, {"to", to}, {"from_s", seconds(from, fmt)}, {"to_s", seconds(to, fmt)},
            {"from_tc", tc(from, fmt)}, {"to_tc", tc(to, fmt)}};
}

ProjectFormat parseFormat(const QJsonObject& a)
{
    ProjectFormat f;
    if (a.contains("width")) f.width = a["width"].toInt();
    if (a.contains("height")) f.height = a["height"].toInt();
    if (f.width < 16 || f.height < 16 || f.width > 8192 || f.height > 8192 || f.width % 2 || f.height % 2)
        fail("BAD_ARGUMENT", "width/height must be even numbers between 16 and 8192");
    if (a.contains("fps")) {
        const double fps = a["fps"].toDouble();
        if (fps < 1 || fps > 120) fail("BAD_ARGUMENT", "fps must be between 1 and 120");
        f.rate = nearestFrameRate(fps);
    }
    return f;
}

QJsonObject formatJson(const ProjectFormat& f)
{
    return {{"width", f.width}, {"height", f.height}, {"fps", std::round(f.rate.fps() * 1000) / 1000},
            {"fps_label", QString("%1/%2").arg(f.rate.num).arg(f.rate.den)}};
}

// ---------- Projects ----------

Session::Session(const QString& file, Access access) : path(absolute(file))
{
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "project not found: " + path);
    if (access == Access::Write) {
        lock = ProjectFile::lock(path);
        if (!lock) fail("PROJECT_LOCKED", "the project is being saved by someone else (schneidi app?), try again: " + path);
    }
    ProjectData d;
    QString error;
    if (!ProjectFile::load(path, &d, &error)) fail("BAD_PROJECT", error);
    project.load(d);
    m_open = project.currentSequence();
}

int Session::sequenceOf(const QJsonValue& v) const
{
    if (v.isDouble() && project.sequence(v.toInt())) return v.toInt();
    const QString name = v.toVariant().toString().trimmed();
    for (const Sequence& q : project.sequences())
        if (q.name.compare(name, Qt::CaseInsensitive) == 0 || QString::number(q.id) == name) return q.id;
    QStringList names;
    for (const Sequence& q : project.sequences()) names << QString("%1 \"%2\"").arg(q.id).arg(q.name);
    fail("NOT_FOUND", QString("no timeline '%1' (there are: %2)").arg(name, names.join(", ")));
}

void Session::selectTimeline(const QJsonValue& v, bool open)
{
    if (v.isUndefined() || v.isNull()) return;
    const int id = sequenceOf(v);
    project.setCurrentSequence(id);
    if (open) m_open = id;
}

QString Session::save()
{
    if (!lock) fail("INTERNAL", "session was opened read-only");
    // The app opens the timeline the user had open (or the one an op asked for), not the last one edited
    if (project.sequence(m_open)) project.setCurrentSequence(m_open);
    const QString backup = ProjectFile::backup(path);
    QString error;
    if (!ProjectFile::save(project.data(), path, &error)) fail("SAVE_FAILED", error);
    return backup;
}

const MediaInfo& Session::media(const QString& file)
{
    const QString path = absolute(file);
    if (const MediaInfo* m = project.mediaInfo(path)) return *m;
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "media not found: " + path);
    const MediaInfo info = Engine::probe(format(), path);
    if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) fail("BAD_MEDIA", "cannot read media: " + path);
    project.addMedia(info);
    return *project.mediaInfo(path);
}

QString trackName(TrackRef r) { return QString("%1%2").arg(r.kind == TrackKind::Video ? "V" : "A").arg(r.index + 1); }

// "V1", "A2" or a number (1 = first track); returns the index (0-based)
int parseTrack(const QJsonValue& v, TrackKind* kind)
{
    if (v.isUndefined() || v.isNull()) return 0;
    QString s = v.isDouble() ? QString::number(v.toInt()) : v.toString().trimmed().toUpper();
    TrackKind k = TrackKind::Video;
    if (s.startsWith('V') || s.startsWith('A')) {
        k = s.startsWith('A') ? TrackKind::Audio : TrackKind::Video;
        s = s.mid(1);
    }
    bool ok = false;
    const int n = s.toInt(&ok);
    if (!ok || n < 1 || n > 99) fail("BAD_ARGUMENT", "track must look like V1, A2 or 1, got " + v.toVariant().toString());
    if (kind) *kind = k;
    return n - 1;
}

// Title/subtitle style: only what differs from `def`
QJsonObject titleStyleJson(const TitleStyle& t, const TitleStyle& def)
{
    QJsonObject o;
    const auto num = [&](const char* k, double v, double d) {
        if (v != d) o[k] = v;
    };
    const auto color = [&](const char* k, const QColor& v, const QColor& d) {
        if (v != d) o[k] = v.alpha() == 255 ? v.name() : v.name(QColor::HexArgb);
    };
    const auto flag = [&](const char* k, bool v, bool d) {
        if (v != d) o[k] = v;
    };
    if (t.font != def.font) o["font"] = t.font;
    num("size", t.size, def.size);
    color("color", t.color, def.color);
    flag("bold", t.bold, def.bold);
    flag("italic", t.italic, def.italic);
    if (t.align != def.align) o["align"] = QStringList{"left", "center", "right"}.value(t.align);
    num("x", t.posX, def.posX);
    num("y", t.posY, def.posY);
    flag("outline", t.outlineOn, def.outlineOn);
    color("outline_color", t.outlineColor, def.outlineColor);
    num("outline_width", t.outlineWidth, def.outlineWidth);
    flag("box", t.boxOn, def.boxOn);
    color("box_color", t.boxColor, def.boxColor);
    num("box_padding", t.boxPad, def.boxPad);
    return o;
}

namespace {
QJsonObject clipJson(const Project& p, const Clip& c, TrackRef ref)
{
    const ProjectFormat& f = p.format();
    QJsonObject o{{"id", c.id}, {"track", trackName(ref)}, {"name", p.clipName(c)},
                  {"start", c.start}, {"end", c.end()}, {"start_tc", tc(c.start, f)}, {"end_tc", tc(c.end(), f)}};
    if (c.isTitle()) {
        o["kind"] = "title";
        o["text"] = c.title.text;
        o["style"] = titleStyleJson(c.title, TitleStyle{});
    } else if (c.isCompound()) {
        o["kind"] = "compound";
    } else {
        o["kind"] = "media";
        o["media"] = c.mediaPath;
        o["src_in"] = c.in;
        o["src_out"] = c.out + 1;
    }
    if (c.linkId) o["link"] = c.linkId;
    if (!c.enabled) o["enabled"] = false;
    if (ref.kind == TrackKind::Audio && c.volumeDb != 0) o["volume_db"] = c.volumeDb;
    if (ref.kind == TrackKind::Audio && c.pan != 0) o["pan"] = c.pan;
    if (ref.kind == TrackKind::Audio && c.audioStream != 0) o["audio_stream"] = c.audioStream;
    if (ref.kind == TrackKind::Video && !c.transform.isIdentity()) {
        const ClipTransform& t = c.transform;
        const ClipTransform def;
        QJsonObject to;
        const QPair<const char*, double> fields[] = {
            {"zoom_x", t.zoomX}, {"zoom_y", t.zoomY}, {"x", t.posX}, {"y", t.posY}, {"rotation", t.rotation},
            {"crop_left", t.cropLeft}, {"crop_right", t.cropRight}, {"crop_top", t.cropTop},
            {"crop_bottom", t.cropBottom}, {"opacity", t.opacity}};
        const double defaults[] = {def.zoomX, def.zoomY, def.posX, def.posY, def.rotation, def.cropLeft,
                                   def.cropRight, def.cropTop, def.cropBottom, def.opacity};
        for (int i = 0; i < int(std::size(fields)); ++i)
            if (fields[i].second != defaults[i]) to[fields[i].first] = fields[i].second;
        o["transform"] = to;
    }
    if (const QJsonObject keys = keyframesJson(c); !keys.isEmpty()) o["keyframes"] = keys;
    if (c.freeze) o["freeze"] = true;
    if (c.isRetimed() && !c.keepPitch) o["keep_pitch"] = false;
    if (c.fadeIn) o["fade_in"] = c.fadeIn;
    if (c.fadeOut) o["fade_out"] = c.fadeOut;
    if (c.transIn) o["transition_in"] = c.transIn;
    if (c.transOut) o["transition_out"] = c.transOut;
    if (const QJsonObject st = transitionStyleJson(c.transInStyle); c.transIn && !st.isEmpty()) o["transition_in_style"] = st;
    if (const QJsonObject st = transitionStyleJson(c.transOutStyle); c.transOut && !st.isEmpty()) o["transition_out_style"] = st;
    if (c.hasRamp()) {
        const MediaInfo* m = p.mediaInfo(c.mediaPath);
        const RetimeMap map(c, m ? m->length : 0);
        const QVector<double> points = map.pointMaterial();
        QJsonArray ramp;
        for (int i = 0; i < c.ramp.size() && i < points.size(); ++i) {
            QJsonObject r{{"at", c.start + int(std::lround(points[i])) - c.in}, {"speed", c.ramp[i].speed}};
            if (c.ramp[i].smooth) r["smooth"] = c.ramp[i].smooth;
            ramp << r;
        }
        o["speed_ramp"] = ramp;
    }
    if (c.speed != 1.0) o["speed"] = c.speed;
    if (c.reverse) o["reverse"] = true;
    if (!c.effects.isEmpty()) {
        QJsonArray fx;
        for (const EffectInstance& e : c.effects) {
            QJsonObject eo{{"effect", e.effectId}};
            QJsonObject params;
            for (auto it = e.params.begin(); it != e.params.end(); ++it)
                params[it.key()] = it.value().typeId() == QMetaType::QColor ? QJsonValue(it.value().value<QColor>().name())
                                                                            : QJsonValue::fromVariant(it.value());
            if (!params.isEmpty()) eo["params"] = params;
            if (!e.enabled) eo["enabled"] = false;
            fx << eo;
        }
        o["effects"] = fx;
    }
    return o;
}

} // namespace

QJsonObject timelineJson(const Project& p)
{
    const Timeline& tl = p.timeline();
    const ProjectFormat& f = p.format();
    QJsonArray tracks;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (int i = 0; i < tl.tracks(k).size(); ++i) {
            const Track& t = tl.tracks(k)[i];
            QJsonArray clips;
            for (const Clip& c : t.clips) clips << clipJson(p, c, {k, i});
            QJsonObject o{{"track", trackName({k, i})}, {"clips", clips}};
            if (!t.name.isEmpty()) o["name"] = t.name;
            if (t.locked) o["locked"] = true;
            if (t.muted) o["muted"] = true;
            if (t.hidden) o["hidden"] = true;
            if (!t.color.isEmpty()) o["color"] = t.color;
            if (k == TrackKind::Audio) {
                if (t.volumeDb != 0) o["volume_db"] = t.volumeDb;
                if (t.pan != 0) o["pan"] = t.pan;
                if (t.solo) o["solo"] = true;
            }
            tracks << o;
        }
    QJsonArray markers;
    for (int m : tl.markers) markers << m;
    const int end = TimelineOps::endFrame(tl);
    QJsonObject o{{"id", p.currentSequence()}, {"name", p.sequenceName(p.currentSequence())},
                  {"length", end}, {"length_s", seconds(end, f)}, {"length_tc", tc(end, f)}, {"tracks", tracks},
                  {"markers", markers}};
    QJsonArray subtitles;
    for (int i = 0; i < tl.subtitles.size(); ++i) {
        const SubtitleTrack& st = tl.subtitles[i];
        QJsonArray cues;
        for (const SubtitleCue& c : st.cues)
            cues << QJsonObject{{"id", c.id}, {"start", c.start}, {"end", c.end}, {"start_tc", tc(c.start, f)},
                                {"end_tc", tc(c.end, f)}, {"text", c.text}};
        QJsonObject so{{"track", QString("ST%1").arg(i + 1)}, {"cues", cues}};
        if (!st.name.isEmpty()) so["name"] = st.name;
        if (!st.enabled) so["enabled"] = false; // not shown/burnt in (one subtitle track at a time)
        if (st.locked) so["locked"] = true;
        if (const QJsonObject style = titleStyleJson(st.style, subtitleBaseStyle()); !style.isEmpty()) so["style"] = style;
        subtitles << so;
    }
    if (!subtitles.isEmpty()) o["subtitles"] = subtitles;
    if (tl.masterVolumeDb != 0 || tl.masterLimiter) {
        QJsonObject m{{"volume_db", tl.masterVolumeDb}};
        if (tl.masterLimiter) m["limiter_db"] = tl.masterLimiterDb;
        o["master"] = m;
    }
    if (tl.markIn >= 0) o["mark_in"] = tl.markIn;
    if (tl.markOut >= 0) o["mark_out"] = tl.markOut + 1;
    return o;
}

QString findAsset(Asset kind, const QString& name)
{
    QVector<EffectFolders::LutEntry> list;
    if (kind == Asset::Lut) list = EffectFolders::builtinLuts() + EffectFolders::userLuts();
    if (kind == Asset::Luma) list = EffectFolders::builtinTransitions() + EffectFolders::userTransitions();
    if (kind == Asset::Preset) list = EffectFolders::userPresets();
    for (const EffectFolders::LutEntry& e : list) {
        const QString id = EffectFolders::isBuiltin(e.path) ? EffectFolders::builtinId(e.path) : QString();
        const QString grouped = e.group.isEmpty() ? e.name : e.group + "/" + e.name;
        if (name == id || name.compare(e.name, Qt::CaseInsensitive) == 0 || name.compare(grouped, Qt::CaseInsensitive) == 0
            || name == QFileInfo(e.path).fileName())
            return e.path;
    }
    if (QFileInfo::exists(name)) return absolute(name);
    static const char* what[] = {"LUT", "luma transition", "effect preset"};
    fail("NOT_FOUND", QString("no %1 '%2' (`effects` lists them, or give a file)").arg(what[int(kind)], name));
}

namespace {
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

QJsonArray timelinesJson(const Project& p)
{
    QJsonArray list;
    for (const Sequence& q : p.sequences()) {
        const int end = TimelineOps::endFrame(q.timeline);
        QJsonObject o{{"id", q.id}, {"name", q.name}, {"length", end}, {"length_tc", tc(end, p.format())}};
        if (q.compound) o["compound"] = true;
        list << o;
    }
    return list;
}

QJsonObject projectJson(const Project& p, const QString& path)
{
    QJsonArray media;
    for (const MediaInfo& m : p.media()) {
        QJsonObject o{{"path", m.path}, {"frames", m.length}, {"seconds", seconds(m.length, p.format())},
                      {"video", m.hasVideo}, {"audio", m.hasAudio}};
        if (m.isImage) o["image"] = true;
        if (!QFileInfo::exists(m.path)) o["offline"] = true;
        if (m.audioStreamCount() > 1) o["audio_streams"] = m.audioStreamCount();
        if (m.bin) o["bin"] = p.binName(m.bin);
        if (!m.clipColor.isEmpty()) o["color"] = m.clipColor;
        if (!m.flags.isEmpty()) o["flags"] = QJsonArray::fromStringList(m.flags);
        if (m.markIn >= 0) o["mark_in"] = m.markIn;
        if (m.markOut >= 0) o["mark_out"] = m.markOut + 1;
        media << o;
    }
    return {{"project", path}, {"format", formatJson(p.format())}, {"media", media}, {"timelines", timelinesJson(p)},
            {"timeline", timelineJson(p)}};
}

} // namespace detail

namespace {

using namespace detail;

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
    return projectJson(s.project, s.path);
}

QJsonObject cmdImport(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString(), Session::Access::Write);
    for (const QJsonValue& v : a["files"].toArray()) s.media(v.toString());
    const QString backup = s.save();
    QJsonObject o = projectJson(s.project, s.path);
    o.remove("timeline");
    if (!backup.isEmpty()) o["backup"] = backup;
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
    QJsonObject o{{"project", s.path}, {"ops", ops.size()}, {"saved", !dry}, {"timeline", timelineJson(s.project)}};
    if (s.project.sequences().size() > 1) o["timelines"] = timelinesJson(s.project);
    if (!dry) {
        const QString backup = s.save();
        if (!backup.isEmpty()) o["backup"] = backup;
    }
    return o;
}

QJsonObject cmdBackups(const QJsonObject& a, Context&)
{
    const QString path = absolute(a["project"].toString());
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "project not found: " + path);
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
    const QString path = absolute(a["project"].toString());
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "project not found: " + path);
    const auto lock = ProjectFile::lock(path);
    if (!lock) fail("PROJECT_LOCKED", "the project is being saved by someone else (schneidi app?), try again: " + path);
    // Pick the copy before the current state goes into the backups (that shifts the numbers)
    const QStringList files = ProjectFile::backups(path);
    const QString which = a.contains("backup") ? a["backup"].toString() : QString("1");
    QString source;
    bool isIndex = false;
    const int index = which.toInt(&isIndex);
    if (isIndex) {
        if (index < 1 || index > files.size())
            fail("NOT_FOUND", QString("no backup %1 (there are %2, see `backups`)").arg(index).arg(files.size()));
        source = files[index - 1];
    } else {
        source = absolute(which);
        if (!QFileInfo::exists(source)) fail("NOT_FOUND", "backup not found: " + source);
    }
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
    if (!srtOnly && std::none_of(formats.begin(), formats.end(), [&](const RenderFormatInfo& i) { return formatId == i.id; }))
        fail("BAD_ARGUMENT", "unknown format " + formatId + " (h264, h265, prores, aac, mp3, wav, aiff, alac, srt)");
    QString out = absolute(a["out"].toString());
    if (QFileInfo(out).suffix().isEmpty()) out += "." + QString(srtOnly ? "srt" : renderFormat(formatId).extension);
    if (QFileInfo::exists(out) && !a["overwrite"].toBool())
        fail("EXISTS", "output exists (pass overwrite to replace it): " + out);
    job.path = out;
    if (!srtOnly) job.settings.format = formatId;
    if (a.contains("quality")) job.settings.quality = std::clamp(a["quality"].toInt(), 0, 2);
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
        const FrameRate r = nearestFrameRate(a["fps"].toDouble());
        job.settings.rateNum = r.num;
        job.settings.rateDen = r.den;
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
    if (!ex.start(tl, RenderQueue::exportSettings(job, s.format()), &error)) fail("RENDER_FAILED", error);
    loop.exec();
    if (!ok) fail("RENDER_FAILED", message.isEmpty() ? QString("rendering failed") : message);
    const ExportSettings es = RenderQueue::exportSettings(job, s.format());
    QJsonObject o{{"out", out}, {"format", formatId}, {"render_s", std::round(timer.elapsed() / 100.0) / 10}};
    if (!job.settings.audioOnly()) {
        o["width"] = es.size.width();
        o["height"] = es.size.height();
    }
    if (!job.preset.isEmpty()) o["preset"] = job.preset;
    if (!es.subtitlePath.isEmpty()) o["srt"] = es.subtitlePath;
    return o;
}

QJsonObject cmdHelp(const QJsonObject& a, Context&);

QJsonObject cmdEffects(const QJsonObject& a, Context&)
{
    const QString only = a["effect"].toString();
    QJsonArray list;
    for (const EffectDescriptor& d : EffectRegistry::all()) {
        if (!d.video || (!only.isEmpty() && d.id != only)) continue;
        QJsonObject o{{"effect", d.id}, {"name", d.name}};
        if (!d.category.isEmpty()) o["category"] = d.category;
        if (d.id == "grade") o["note"] = "the Color page grade (lift/gamma/gain/offset, LUT)";
        if (only.isEmpty()) {
            QJsonArray keys;
            for (const EffectParam& p : d.params) keys << p.key;
            o["params"] = keys;
        } else {
            static const char* types[] = {"number", "color", "path", "boolean", "choice"};
            QJsonArray params;
            for (const EffectParam& p : d.params) {
                QJsonObject po{{"key", p.key}, {"label", p.label}, {"type", types[p.type]}};
                if (p.type == EffectParam::Double) {
                    po["min"] = p.min;
                    po["max"] = p.max;
                }
                if (p.type == EffectParam::Choice) po["choices"] = QJsonArray::fromStringList(p.choices);
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
    if (list.isEmpty() && !only.isEmpty()) fail("NOT_FOUND", "no video effect " + only);
    if (!only.isEmpty()) return {{"effects", list}};
    QJsonArray types;
    for (const auto& t : kTransitionTypes) types << t.id;
    return {{"effects", list},
            {"luts", assetsJson(EffectFolders::builtinLuts() + EffectFolders::userLuts())},
            {"transition_types", types},
            {"lumas", assetsJson(EffectFolders::builtinTransitions() + EffectFolders::userTransitions())},
            {"presets", assetsJson(EffectFolders::userPresets())}};
}

} // namespace

namespace {
QString presetHelp()
{
    QStringList names;
    for (const RenderPreset& p : RenderPresets::all()) names << p.name;
    return "Deliver preset of the app (other parameters override it): " + names.join(", ");
}
} // namespace

const QVector<Command>& commands()
{
    using T = ParamType;
    static const QVector<Command> list{
        {"help", "List all commands with their parameters (JSON).",
         {{"command", T::String, "only this command", false, true}}, cmdHelp},
        {"new", "Create a new empty project (.schneidi), optionally with media already in the media pool.",
         {{"project", T::Path, "project file to create (.schneidi)", true, true},
          {"width", T::Integer, "timeline width (default 1920)"},
          {"height", T::Integer, "timeline height (default 1080)"},
          {"fps", T::Number, "frame rate (default 25; 23.976, 24, 25, 29.97, 30, 50, 59.94, 60)"},
          {"media", T::Paths, "media files to import"},
          {"overwrite", T::Boolean, "replace an existing project"}},
         cmdNew},
        {"info", "Show a project: format, media, its timelines and one timeline's tracks and clips (ids, positions, "
                 "source ranges).",
         {{"project", T::Path, "project file", true, true},
          {"timeline", T::String, "timeline to show (id or name; default: the open one)"}},
         cmdInfo},
        {"import", "Add media files to a project's media pool.",
         {{"project", T::Path, "project file", true, true}, {"files", T::Paths, "media files", true, true}}, cmdImport},
        {"edit", "Apply edit operations to a project and save it (a backup of the previous state is kept).",
         {{"project", T::Path, "project file", true, true},
          {"ops", T::Json, editHelp(), true},
          {"timeline", T::String, "timeline to edit (id or name from `info`; default: the open one)"},
          {"dry_run", T::Boolean, "only show the resulting timeline, do not save"}},
         cmdEdit},
        {"backups", "List the backup copies of a project (every save keeps the previous state, newest first).",
         {{"project", T::Path, "project file", true, true}}, cmdBackups},
        {"restore", "Undo: put a backup copy back as the project (the current state becomes a backup itself, so "
                    "restore can be undone too).",
         {{"project", T::Path, "project file", true, true},
          {"backup", T::String, "number from `backups` (default 1 = state before the last change) or a backup file"}},
         cmdRestore},
        {"render", "Render the project timeline to a video or audio file.",
         {{"project", T::Path, "project file", true, true},
          {"out", T::Path, "output file (extension added if missing)", true},
          {"preset", T::String, presetHelp()},
          {"format", T::String, "h264 (default), h265, prores, aac, mp3, wav, aiff, alac; srt = only the subtitle file"},
          {"quality", T::Integer, "0 = high (default), 1 = medium, 2 = small (H.264/H.265)"},
          {"height", T::Integer, "shorter image edge, e.g. 720 (default: timeline resolution)"},
          {"size", T::String, "fixed output size, e.g. 1080x1920 (portrait; the picture is fitted in)"},
          {"fps", T::Number, "output frame rate (default: the timeline's)"},
          {"audio_bitrate", T::Integer, "kbit/s for AAC/MP3 (default 320)"},
          {"subtitles", T::String, "none (default), burn (into the picture) or srt (file next to the video)"},
          {"from", T::Time, "start of the range (default: timeline start)"},
          {"to", T::Time, "end of the range, exclusive (default: timeline end)"},
          {"timeline", T::String, "timeline to render (id or name; default: the open one)"},
          {"overwrite", T::Boolean, "replace an existing output file"}},
         cmdRender},
        {"probe", "Media file details: duration, frames, resolution, frame rate, audio streams.",
         {{"file", T::Path, "media file", true, true},
          {"project", T::Path, "count frames in this project's frame rate (default: the file's own)"}},
         cmdProbe},
        {"silence", "Find pauses in the audio of a media file. Returns 'silence' and 'sound' ranges [from, to); "
                    "'sound' fits the 'keep' edit operation directly (jump cut).",
         {{"file", T::Path, "media file", true, true},
          {"project", T::Path, "count frames in this project's frame rate (recommended before editing)"},
          {"threshold_db", T::Number, "quieter than this counts as silence (default -35)"},
          {"min", T::Time, "shortest pause (default 0.5s)"},
          {"pad", T::Time, "sound kept around each pause (default 0.1s)"}},
         cmdSilence},
        {"scenes", "Find scene changes (cuts) in a video file. Returns the 'cuts' and the 'scenes' between them as "
                   "ranges [from, to) in source frames, e.g. to pick shots for 'keep' or to look at them with `frames`.",
         {{"file", T::Path, "video file", true, true},
          {"project", T::Path, "count frames in this project's frame rate (recommended before editing)"},
          {"threshold", T::Number, "how different two frames must be, 0..1 (default 0.3; lower finds more cuts)"},
          {"min", T::Time, "shortest scene (default 0.5s)"}},
         cmdScenes},
        {"transcribe", "Speech to text with whisper (offline; install it with `extensions`). Returns subtitle-sized "
                       "'segments' with times and, with words, every word's time (to cut out words or filler). "
                       "Source: a media file or a project (its timeline sound, times in timeline frames).",
         {{"source", T::Path, "media file or project (.schneidi)", true, true},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"},
          {"language", T::String, "spoken language: de, en, fr, … (default auto = detect)"},
          {"model", T::String, "turbo, small, base, tiny or a model file (default: best installed)"},
          {"words", T::Boolean, "also return every word with its time"},
          {"srt", T::Path, "also write the segments as an SRT file"},
          {"max_chars", T::Integer, "longest segment in characters (default 42, one subtitle line)"},
          {"audio_stream", T::Integer, "for a media file: which audio stream, 0 = first"},
          {"from", T::Time, "for a project: start of the range"},
          {"to", T::Time, "for a project: end of the range"},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"tracks", T::String, "for a project: only these audio tracks, e.g. \"A3\" or \"A2,A3\" (the voice without music; "
                                "muted ones too; default: what is audible)"},
          {"threads", T::Integer, "processor threads (default: half of them)"}},
         cmdTranscribe},
        {"loudness", "Measure loudness like the Mixer's meter (EBU R128 / BS.1770): integrated LUFS, loudness range, "
                     "true peak. Source: a media file, a project's mix (as exported) or single clips of a project. "
                     "Common targets: -14 LUFS / -1 dBTP (streaming), -23 LUFS (EBU broadcast).",
         {{"source", T::Path, "media file or project (.schneidi)", true, true},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"from", T::Time, "for a project: start of the range"},
          {"to", T::Time, "for a project: end of the range"},
          {"tracks", T::String, "for a project: only these audio tracks, e.g. \"A1,A2\""},
          {"clips", T::Times, "for a project: measure these clips (ids) one by one instead of the mix"},
          {"audio_stream", T::Integer, "for a media file: which audio stream, 0 = first"}},
         cmdLoudness},
        {"scopes", "Video scopes of one frame like the Color page (waveform, RGB parade, vectorscope, histogram) as "
                   "one image, plus numbers to judge exposure and color: luma spread (10-bit, 0..1023), clipped "
                   "black/white in %, RGB means and color cast in % (+ = too much of that channel), saturation.",
         {{"source", T::Path, "media file or project (.schneidi)", true, true},
          {"at", T::Time, "frame to look at (default 0)"},
          {"types", T::String, "comma list: waveform, parade, vectorscope, histogram (default all)"},
          {"out", T::Path, "image file to write (the MCP tool shows the image anyway)"},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"}},
         cmdScopes},
        {"extensions", "Optional downloads (not part of schneidi): whisper speech recognition and its models. "
                       "Without arguments: list with sizes and what is installed.",
         {{"install", T::String, "ids to download and install, e.g. whisper,small (model sizes: turbo 574 MB, "
                                 "small 190 MB, base 60 MB, tiny 32 MB)"},
          {"remove", T::String, "ids to delete again"},
          {"reinstall", T::Boolean, "download again even if installed"}},
         cmdExtensions},
        {"effects", "Video effects for the 'effect' edit operation: all ids with their parameter names, or one "
                    "effect with types, ranges and defaults. Also lists LUTs (op 'color' lut), transition types and "
                    "luma images (op 'transition') and effect presets (op 'effect' preset).",
         {{"effect", T::String, "only this effect, with parameter details", false, true}}, cmdEffects},
        {"frames", "Still images of a media file or a project timeline at given times, as files or one contact "
                   "sheet with timecodes (lets you look at the material).",
         {{"source", T::Path, "media file or project (.schneidi)", true, true},
          {"at", T::Times, "times to show"},
          {"every", T::Time, "one frame every … (e.g. 10s)"},
          {"count", T::Integer, "this many frames spread evenly"},
          {"out", T::Path, "folder for the images (with sheet: image file)"},
          {"sheet", T::Boolean, "one contact sheet image instead of single images"},
          {"width", T::Integer, "longer image edge in pixels (default 640, MCP 480)"},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"},
          {"timeline", T::String, "for a project: which timeline (id or name; default: the open one)"}},
         cmdFrames},
    };
    return list;
}

const Command* find(const QString& name)
{
    for (const Command& c : commands())
        if (c.name == name) return &c;
    return nullptr;
}

QJsonObject inputSchema(const Command& cmd)
{
    QJsonObject props;
    QJsonArray required;
    for (const Param& p : cmd.params) {
        QJsonObject s{{"description", p.help}};
        switch (p.type) {
        case ParamType::String:
        case ParamType::Path: s["type"] = "string"; break;
        case ParamType::Integer: s["type"] = "integer"; break;
        case ParamType::Number: s["type"] = "number"; break;
        case ParamType::Boolean: s["type"] = "boolean"; break;
        case ParamType::Time: s["type"] = QJsonArray{"integer", "string"}; break;
        case ParamType::Times: s["type"] = "array"; s["items"] = QJsonObject{{"type", QJsonArray{"integer", "string"}}}; break;
        case ParamType::Paths: s["type"] = "array"; s["items"] = QJsonObject{{"type", "string"}}; break;
        case ParamType::Json: s["type"] = "array"; s["items"] = QJsonObject{{"type", "object"}}; break;
        }
        props[p.name] = s;
        if (p.required) required << p.name;
    }
    QJsonObject o{{"type", "object"}, {"properties", props}};
    if (!required.isEmpty()) o["required"] = required;
    return o;
}

QJsonObject normalize(const Command& cmd, const QJsonObject& raw)
{
    QJsonObject out;
    for (auto it = raw.begin(); it != raw.end(); ++it)
        if (std::none_of(cmd.params.begin(), cmd.params.end(), [&](const Param& p) { return p.name == it.key(); }))
            fail("BAD_ARGUMENT", QString("unknown parameter '%1' for %2").arg(it.key(), cmd.name));
    for (const Param& p : cmd.params) {
        if (!raw.contains(p.name) || raw[p.name].isNull()) {
            if (p.required) fail("BAD_ARGUMENT", QString("missing parameter '%1' for %2").arg(p.name, cmd.name));
            continue;
        }
        const QJsonValue v = raw[p.name];
        const QString str = v.isString() ? v.toString() : QString();
        const auto list = [&]() {
            if (v.isArray()) return v.toArray();
            if (str.trimmed().startsWith('[')) return QJsonDocument::fromJson(str.toUtf8()).array();
            QJsonArray arr;
            for (const QString& x : str.split(',', Qt::SkipEmptyParts)) arr << x.trimmed();
            return arr;
        };
        bool ok = true;
        switch (p.type) {
        case ParamType::String: out[p.name] = v.isString() ? str : v.toVariant().toString(); break;
        case ParamType::Path: out[p.name] = absolute(str); ok = v.isString() && !str.isEmpty(); break;
        case ParamType::Integer:
            if (v.isDouble()) out[p.name] = v.toInt();
            else out[p.name] = str.toInt(&ok);
            break;
        case ParamType::Number:
            if (v.isDouble()) out[p.name] = v.toDouble();
            else out[p.name] = QLocale::c().toDouble(str, &ok);
            break;
        case ParamType::Boolean:
            if (v.isBool()) out[p.name] = v.toBool();
            else if (str == "true" || str == "1" || str == "yes") out[p.name] = true;
            else if (str == "false" || str == "0" || str == "no") out[p.name] = false;
            else ok = false;
            break;
        case ParamType::Time: out[p.name] = v; ok = v.isDouble() || v.isString(); break;
        case ParamType::Times: out[p.name] = list(); break;
        case ParamType::Paths: {
            QJsonArray arr;
            for (const QJsonValue& x : list()) arr << absolute(x.toString());
            out[p.name] = arr;
            break;
        }
        case ParamType::Json:
            if (v.isString()) {
                QJsonParseError err;
                const QJsonDocument doc = QJsonDocument::fromJson(str.toUtf8(), &err);
                if (err.error != QJsonParseError::NoError)
                    fail("BAD_ARGUMENT", QString("%1: invalid JSON (%2)").arg(p.name, err.errorString()));
                out[p.name] = doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
            } else {
                out[p.name] = v;
            }
            break;
        }
        if (!ok) fail("BAD_ARGUMENT", QString("parameter '%1' has a wrong value: %2").arg(p.name, v.toVariant().toString()));
    }
    return out;
}

namespace {

QJsonObject cmdHelp(const QJsonObject& a, Context&)
{
    QJsonArray list;
    for (const Command& c : commands()) {
        if (a.contains("command") && c.name != a["command"].toString()) continue;
        QJsonArray positional;
        for (const Param& p : c.params)
            if (p.positional) positional << p.name;
        QJsonObject o{{"name", c.name}, {"summary", c.summary}, {"positional", positional}, {"params", inputSchema(c)}};
        if (c.name == "edit" && a.contains("command")) o["ops"] = editOpsHelp();
        list << o;
    }
    if (list.isEmpty()) fail("NOT_FOUND", "no command " + a["command"].toString());
    return {{"program", "schneidi-cli"}, {"version", QCoreApplication::applicationVersion()},
            {"api_version", ApiVersion},
            {"usage", "schneidi-cli <command> [positional…] [--param value]; output is JSON on stdout, "
                      "exit code 0 = ok, 1 = error, 2 = usage error. `schneidi-cli mcp` runs an MCP server on stdio."},
            {"commands", list}};
}

} // namespace

} // namespace Cli