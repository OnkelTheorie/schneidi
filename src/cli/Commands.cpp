#include "cli/Commands.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/Selection.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/Exporter.h"
#include "engine/Profiles.h"
#include "engine/RenderQueue.h"
#include "engine/Snapshot.h"

#include <QBuffer>
#include <QCoreApplication>
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

namespace {

[[noreturn]] void fail(const QString& code, const QString& message) { throw Error{code, message}; }

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

// A loaded project with an editor; save() writes it back (with a backup copy of the old state first)
struct Session {
    QString path;
    Project project;
    Selection selection;
    Editor editor{&project, &selection};

    explicit Session(const QString& file) : path(absolute(file))
    {
        if (!QFileInfo::exists(path)) fail("NOT_FOUND", "project not found: " + path);
        ProjectData d;
        QString error;
        if (!ProjectFile::load(path, &d, &error)) fail("BAD_PROJECT", error);
        project.load(d);
    }
    const ProjectFormat& format() const { return project.format(); }

    QString save()
    {
        const QString backup = ProjectFile::backup(path);
        QString error;
        if (!ProjectFile::save(project.data(), path, &error)) fail("SAVE_FAILED", error);
        return backup;
    }

    // Media in the project (imported on first use); throws if the file cannot be read
    const MediaInfo& media(const QString& file)
    {
        const QString path = absolute(file);
        if (const MediaInfo* m = project.mediaInfo(path)) return *m;
        if (!QFileInfo::exists(path)) fail("NOT_FOUND", "media not found: " + path);
        const MediaInfo info = Engine::probe(format(), path);
        if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) fail("BAD_MEDIA", "cannot read media: " + path);
        project.addMedia(info);
        return *project.mediaInfo(path);
    }
};

QString trackName(TrackRef r) { return QString("%1%2").arg(r.kind == TrackKind::Video ? "V" : "A").arg(r.index + 1); }

// "V1", "A2" or a number (1 = first track); returns the index (0-based)
int parseTrack(const QJsonValue& v, TrackKind* kind = nullptr)
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

QJsonObject clipJson(const Project& p, const Clip& c, TrackRef ref)
{
    const ProjectFormat& f = p.format();
    QJsonObject o{{"id", c.id}, {"track", trackName(ref)}, {"name", p.clipName(c)},
                  {"start", c.start}, {"end", c.end()}, {"start_tc", tc(c.start, f)}, {"end_tc", tc(c.end(), f)}};
    if (c.isTitle()) {
        o["kind"] = "title";
        o["text"] = c.title.text;
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
    if (c.fadeIn) o["fade_in"] = c.fadeIn;
    if (c.fadeOut) o["fade_out"] = c.fadeOut;
    if (c.transIn) o["transition_in"] = c.transIn;
    if (c.transOut) o["transition_out"] = c.transOut;
    if (c.speed != 1.0) o["speed"] = c.speed;
    if (c.reverse) o["reverse"] = true;
    if (!c.effects.isEmpty()) {
        QJsonArray fx;
        for (const EffectInstance& e : c.effects) fx << e.effectId;
        o["effects"] = fx;
    }
    return o;
}

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
            tracks << o;
        }
    QJsonArray markers;
    for (int m : tl.markers) markers << m;
    const int end = TimelineOps::endFrame(tl);
    QJsonObject o{{"length", end}, {"length_s", seconds(end, f)}, {"length_tc", tc(end, f)}, {"tracks", tracks},
                  {"markers", markers}};
    int cues = 0;
    for (const SubtitleTrack& st : tl.subtitles) cues += st.cues.size();
    if (cues) o["subtitle_cues"] = cues;
    if (tl.markIn >= 0) o["mark_in"] = tl.markIn;
    if (tl.markOut >= 0) o["mark_out"] = tl.markOut + 1;
    return o;
}

QJsonObject projectJson(const Project& p, const QString& path)
{
    QJsonArray media;
    for (const MediaInfo& m : p.media()) {
        QJsonObject o{{"path", m.path}, {"frames", m.length}, {"seconds", seconds(m.length, p.format())},
                      {"video", m.hasVideo}, {"audio", m.hasAudio}};
        if (m.isImage) o["image"] = true;
        if (!QFileInfo::exists(m.path)) o["offline"] = true;
        media << o;
    }
    return {{"project", path}, {"format", formatJson(p.format())}, {"media", media}, {"timeline", timelineJson(p)}};
}

// ---------- Edit operations ----------

const Clip& clipOf(Session& s, const QJsonObject& op, const char* key = "clip")
{
    if (!op.contains(key)) fail("BAD_ARGUMENT", QString("missing '%1' (clip id)").arg(key));
    const int id = op[key].toInt();
    const Clip* c = TimelineOps::findClip(s.project.timeline(), id);
    if (!c) fail("NOT_FOUND", QString("no clip with id %1").arg(id));
    return *c;
}

QVector<int> clipIds(Session& s, const QJsonObject& op)
{
    QVector<int> ids;
    const QJsonValue v = op.value("clips");
    const QJsonArray arr = v.isArray() ? v.toArray() : QJsonArray{op.value("clip")};
    for (const QJsonValue& x : arr) {
        const int id = x.toInt();
        if (!TimelineOps::findClip(s.project.timeline(), id)) fail("NOT_FOUND", QString("no clip with id %1").arg(id));
        ids << id;
    }
    if (ids.isEmpty()) fail("BAD_ARGUMENT", "missing 'clips' (list of clip ids)");
    // Linked partners (video + its audio) go along, like clicking a clip in the timeline
    return op.value("linked").toBool(true) ? s.editor.withLinked(ids) : ids;
}

int timeOf(Session& s, const QJsonObject& op, const char* key, int fallback = -1)
{
    if (!op.contains(key)) {
        if (fallback >= 0) return fallback;
        fail("BAD_ARGUMENT", QString("missing '%1'").arg(key));
    }
    return parseTime(op[key], s.format(), key);
}

// Source range [in, out) of a media file: defaults to the whole file
void sourceRange(Session& s, const QJsonObject& op, const MediaInfo& m, int* in, int* out)
{
    *in = timeOf(s, op, "in", 0);
    *out = op.contains("out") ? timeOf(s, op, "out") : m.length;
    if (op.contains("duration")) *out = *in + timeOf(s, op, "duration");
    *out = std::min(*out, m.length);
    if (*in >= *out) fail("BAD_ARGUMENT", QString("empty source range %1..%2 (media has %3 frames)").arg(*in).arg(*out).arg(m.length));
}

void applyOp(Session& s, const QJsonObject& op)
{
    const QString name = op.value("op").toString();
    Editor& ed = s.editor;
    const auto end = [&s] { return TimelineOps::endFrame(s.project.timeline()); };
    s.selection.clear();

    if (name == "place" || name == "append") {
        const MediaInfo m = s.media(op.value("media").toString());
        int in = 0, out = 0;
        sourceRange(s, op, m, &in, &out);
        const int at = name == "append" ? timeOf(s, op, "at", end()) : timeOf(s, op, "at");
        ed.placeSourceRange(m.path, in, out - 1, at, parseTrack(op.value("track")));
    } else if (name == "keep") {
        const MediaInfo m = s.media(op.value("media").toString());
        int at = timeOf(s, op, "at", end());
        const int track = parseTrack(op.value("track"));
        const QJsonArray ranges = op.value("ranges").toArray();
        if (ranges.isEmpty()) fail("BAD_ARGUMENT", "missing 'ranges' ([[from, to], …] in the source)");
        for (const QJsonValue& r : ranges) {
            QJsonObject sub;
            if (r.isArray()) sub = {{"in", r.toArray().at(0)}, {"out", r.toArray().at(1)}};
            else sub = {{"in", r.toObject().value("from")}, {"out", r.toObject().value("to")}};
            int in = 0, out = 0;
            sourceRange(s, sub, m, &in, &out);
            ed.placeSourceRange(m.path, in, out - 1, at, track);
            at += out - in;
        }
    } else if (name == "split") {
        const int at = timeOf(s, op, "at");
        if (op.contains("clip")) ed.bladeAt(clipOf(s, op).id, at);
        else ed.splitAtPlayhead(at); // no selection: every clip under `at`
    } else if (name == "delete") {
        const QVector<int> ids = clipIds(s, op);
        s.selection.set(QSet<int>(ids.begin(), ids.end()));
        if (op.value("ripple").toBool()) ed.rippleDeleteSelection();
        else ed.deleteSelection();
    } else if (name == "delete_range") {
        const int from = timeOf(s, op, "from"), to = timeOf(s, op, "to");
        if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
        ed.deleteRange(from, to, op.value("ripple").toBool(true));
    } else if (name == "move") {
        const QJsonValue by = op.value("by");
        int delta = 0;
        if (by.isDouble()) delta = by.toInt();
        else if (by.isString() && by.toString().startsWith('-')) delta = -parseTime(by.toString().mid(1), s.format(), "by");
        else if (by.isString()) delta = parseTime(by, s.format(), "by");
        if (op.contains("to")) delta = timeOf(s, op, "to") - clipOf(s, {{"clip", clipIds(s, op).first()}}).start;
        ed.moveClips(clipIds(s, op), delta, TrackKind::Video, op.value("track_delta").toInt());
    } else if (name == "trim") {
        const Clip& c = clipOf(s, op);
        const bool start = op.value("edge").toString() == "start";
        int delta = op.value("by").toInt();
        if (op.contains("to")) delta = timeOf(s, op, "to") - (start ? c.start : c.end());
        ed.trimClip(c.id, start ? TimelineOps::Edge::Start : TimelineOps::Edge::End, delta);
    } else if (name == "title") {
        const int at = timeOf(s, op, "at", 0);
        ed.addTitle(at, op.contains("track") ? parseTrack(op.value("track")) : -1);
        if (s.selection.ids().isEmpty()) fail("FAILED", "could not place the title (locked track?)");
        const int id = *s.selection.ids().begin();
        const int length = timeOf(s, op, "duration", 5 * s.project.fps());
        const QString text = op.value("text").toString("Title");
        ed.modifyClips({id}, "Title", [&](Clip& c) {
            c.title.text = text;
            if (op.contains("size")) c.title.size = op.value("size").toDouble();
            if (op.contains("y")) c.title.posY = op.value("y").toDouble();
            c.out = c.in + std::max(1, length) - 1;
        });
    } else if (name == "fade") {
        const Clip& c = clipOf(s, op);
        const int id = c.id;
        if (op.contains("in")) ed.setClipFade(id, TimelineOps::Edge::Start, timeOf(s, op, "in"));
        if (op.contains("out")) ed.setClipFade(id, TimelineOps::Edge::End, timeOf(s, op, "out"));
    } else if (name == "volume") {
        if (!op.contains("db")) fail("BAD_ARGUMENT", "missing 'db'");
        for (int id : clipIds(s, op))
            if (TrackRef r; TimelineOps::findClip(s.project.timeline(), id, &r) && r.kind == TrackKind::Audio)
                ed.setClipVolume(id, op.value("db").toDouble());
    } else if (name == "speed") {
        Editor::Retime r;
        r.speed = op.value("speed").toDouble(1.0);
        r.reverse = op.value("reverse").toBool();
        if (r.speed <= 0.01 || r.speed > 100) fail("BAD_ARGUMENT", "speed must be between 0.01 and 100 (1 = normal)");
        ed.setClipSpeed(clipIds(s, op), r, op.value("ripple").toBool(true));
    } else if (name == "transition") {
        ed.addTransitions(timeOf(s, op, "at"));
    } else if (name == "marker") {
        const int at = timeOf(s, op, "at");
        if (!s.project.timeline().markers.contains(at)) ed.toggleMarker(at);
    } else if (name == "enable" || name == "disable") {
        const QVector<int> ids = clipIds(s, op);
        const bool on = name == "enable";
        ed.modifyClips(ids, on ? "Enable" : "Disable", [on](Clip& c) { c.enabled = on; });
    } else if (name == "clear") {
        ed.deleteRange(0, std::max(1, end()), false);
    } else {
        fail("BAD_ARGUMENT", QString("unknown op '%1'").arg(name));
    }
}

// ---------- Analysis ----------

// Media file + the frame rate frames count in (project format, or the file's own rate)
struct MediaSource {
    ProjectFormat format;
    MediaInfo info;
};

MediaSource openMedia(const QJsonObject& a)
{
    const QString path = a["file"].toString();
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "file not found: " + path);
    MediaSource m;
    if (a.contains("project")) {
        Session s(a["project"].toString());
        m.format = s.format();
    } else {
        const ClipFormat cf = detectClipFormat(path);
        if (cf.ok) {
            m.format.rate = cf.suggested;
            m.format.width = cf.width & ~1;
            m.format.height = cf.height & ~1;
        }
    }
    m.info = Engine::probe(m.format, path);
    if (m.info.length <= 0 || (!m.info.hasVideo && !m.info.hasAudio)) fail("BAD_MEDIA", "cannot read media: " + path);
    return m;
}

QJsonObject cmdProbe(const QJsonObject& a, Context&)
{
    const MediaSource m = openMedia(a);
    const ClipFormat cf = detectClipFormat(m.info.path);
    QJsonObject o{{"file", m.info.path}, {"frames", m.info.length}, {"seconds", seconds(m.info.length, m.format)},
                  {"duration_tc", tc(m.info.length, m.format)}, {"video", m.info.hasVideo}, {"audio", m.info.hasAudio},
                  {"frames_count_in", formatJson(m.format)["fps"]}};
    if (m.info.isImage) o["image"] = true;
    if (cf.ok) {
        o["width"] = cf.width;
        o["height"] = cf.height;
        o["fps"] = std::round(cf.rate.fps() * 1000) / 1000;
        if (cf.variable) o["variable_frame_rate"] = true;
    }
    if (m.info.hasAudio) {
        QJsonArray streams;
        for (int i = 0; i < m.info.audioStreamCount(); ++i) streams << m.info.audioStreamName(i);
        o["audio_streams"] = streams;
    }
    return o;
}

QJsonObject cmdSilence(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
    const ProjectFormat& f = m.format;
    const double thresholdDb = a.contains("threshold_db") ? a["threshold_db"].toDouble() : -35.0;
    const int minLen = a.contains("min") ? parseTime(a["min"], f, "min") : int(std::lround(0.5 * f.rate.fps()));
    const int pad = a.contains("pad") ? parseTime(a["pad"], f, "pad") : int(std::lround(0.1 * f.rate.fps()));
    Clip c;
    c.mediaPath = m.info.path;
    c.out = m.info.length - 1;
    const auto peaks = AudioAnalysis::clipFramePeaks(f, c, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    });
    if (!peaks) fail("NO_AUDIO", "could not decode the audio of " + m.info.path);
    const float limit = float(std::pow(10.0, thresholdDb / 20.0));
    const int n = int(peaks->size());
    // Quiet runs of at least minLen frames; the pad of sound stays on both sides (not at the file start/end)
    QJsonArray silence, sound;
    int keepFrom = 0, silent = 0;
    for (int i = 0; i <= n; ++i) {
        if (i < n && (*peaks)[i] < limit) {
            ++silent;
            continue;
        }
        if (silent >= minLen) {
            const int from = i - silent, to = i;
            const int cutFrom = from == 0 ? 0 : std::min(to, from + pad);
            const int cutTo = to == n ? n : std::max(cutFrom, to - pad);
            if (cutTo > cutFrom) {
                silence << range(cutFrom, cutTo, f);
                if (cutFrom > keepFrom) sound << range(keepFrom, cutFrom, f);
                keepFrom = cutTo;
            }
        }
        silent = 0;
    }
    if (keepFrom < n) sound << range(keepFrom, n, f);
    int kept = 0;
    for (const QJsonValue& r : sound) kept += r["to"].toInt() - r["from"].toInt();
    return {{"file", m.info.path}, {"frames", n}, {"fps", formatJson(f)["fps"]}, {"threshold_db", thresholdDb},
            {"silence", silence}, {"sound", sound}, {"kept_frames", kept}, {"kept_s", seconds(kept, f)},
            {"removed_s", seconds(n - kept, f)}};
}

QImage sheet(const QVector<QImage>& images, const QStringList& labels)
{
    const int cols = std::min<int>(images.size(), images.size() <= 4 ? 2 : images.size() <= 9 ? 3 : 4);
    const int rows = (images.size() + cols - 1) / cols;
    const QSize cell = images.first().size();
    QImage out(cols * cell.width(), rows * cell.height(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    QFont font = p.font();
    font.setPixelSize(std::max(12, cell.height() / 14));
    p.setFont(font);
    for (int i = 0; i < images.size(); ++i) {
        const QPoint at((i % cols) * cell.width(), (i / cols) * cell.height());
        p.drawImage(at, images[i]);
        const QRect box(at + QPoint(4, 4), QSize(p.fontMetrics().horizontalAdvance(labels[i]) + 8, p.fontMetrics().height() + 2));
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, labels[i]);
        p.setPen(QColor(60, 60, 60));
        p.drawRect(QRect(at, cell).adjusted(0, 0, -1, -1));
    }
    return out;
}

QJsonObject cmdFrames(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    const bool isProject = src.endsWith("." + QString(ProjectFile::Extension));
    std::unique_ptr<Session> session;
    MediaSource media;
    ProjectFormat f;
    int length = 0;
    if (isProject) {
        session = std::make_unique<Session>(src);
        f = session->format();
        length = TimelineOps::endFrame(session->project.timeline());
    } else {
        QJsonObject ma{{"file", src}};
        if (a.contains("project")) ma["project"] = a["project"];
        media = openMedia(ma);
        f = media.format;
        length = media.info.length;
    }
    if (length <= 0) fail("EMPTY", "nothing to show (empty timeline)");
    QVector<int> at;
    for (const QJsonValue& v : a["at"].toArray()) at << parseTime(v, f, "at");
    if (a.contains("every")) {
        const int step = parseTime(a["every"], f, "every");
        if (step <= 0) fail("BAD_ARGUMENT", "'every' must be > 0");
        for (int fr = 0; fr < length; fr += step) at << fr;
    }
    if (a.contains("count")) {
        const int n = std::clamp(a["count"].toInt(), 1, 100);
        for (int i = 0; i < n; ++i) at << int((i + 0.5) * length / n);
    }
    if (at.isEmpty()) fail("BAD_ARGUMENT", "give 'at' (times), 'every' (interval) or 'count' (evenly spread)");
    if (at.size() > 100) fail("BAD_ARGUMENT", QString("too many frames (%1, max 100)").arg(at.size()));
    const int maxEdge = a.contains("width") ? std::clamp(a["width"].toInt(), 64, 3840) : (ctx.mcp ? 480 : 640);
    const QString outDir = a["out"].toString();
    const bool asSheet = a["sheet"].toBool();
    if (outDir.isEmpty() && !ctx.mcp) fail("BAD_ARGUMENT", "missing 'out' (folder for the images, or the sheet file with --sheet)");

    const Timeline tl = session ? session->project.renderTimeline() : Timeline{};
    QVector<QImage> images;
    QStringList labels;
    QJsonArray frames;
    for (int i = 0; i < at.size(); ++i) {
        const int fr = std::clamp(at[i], 0, length - 1);
        QImage img = session ? Snapshot::timeline(f, tl, fr, maxEdge) : Snapshot::media(f, media.info.path, fr, maxEdge);
        if (img.isNull()) fail("DECODE_FAILED", QString("could not decode frame %1").arg(fr));
        images << img;
        labels << tc(fr, f);
        QJsonObject o{{"frame", fr}, {"tc", tc(fr, f)}, {"s", seconds(fr, f)}};
        if (!asSheet && !outDir.isEmpty()) {
            QDir().mkpath(outDir);
            const QString file = QDir(outDir).absoluteFilePath(QString("frame_%1.jpg").arg(fr, 6, 10, QChar('0')));
            if (!img.save(file, "JPG", 85)) fail("WRITE_FAILED", "cannot write " + file);
            o["file"] = file;
        }
        frames << o;
        if (ctx.progress) ctx.progress(double(i + 1) / at.size());
    }
    QJsonObject result{{"source", absolute(src)}, {"frames", frames}};
    if (asSheet) {
        const QImage s = sheet(images, labels);
        if (!outDir.isEmpty()) {
            QString file = absolute(outDir);
            if (QFileInfo(file).isDir()) file = QDir(file).filePath("sheet.jpg");
            if (!s.save(file, "JPG", 85)) fail("WRITE_FAILED", "cannot write " + file);
            result["sheet"] = file;
        }
        if (ctx.mcp) ctx.images = {s};
    } else if (ctx.mcp) {
        ctx.images = images;
    }
    return result;
}

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
    return projectJson(s.project, s.path);
}

QJsonObject cmdImport(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString());
    for (const QJsonValue& v : a["files"].toArray()) s.media(v.toString());
    const QString backup = s.save();
    QJsonObject o = projectJson(s.project, s.path);
    o.remove("timeline");
    if (!backup.isEmpty()) o["backup"] = backup;
    return o;
}

QJsonObject cmdEdit(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString());
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
    QJsonObject o{{"project", s.path}, {"ops", ops.size()}, {"saved", !dry}};
    if (!dry) {
        const QString backup = s.save();
        if (!backup.isEmpty()) o["backup"] = backup;
    }
    o["timeline"] = timelineJson(s.project);
    return o;
}

QJsonObject cmdRender(const QJsonObject& a, Context& ctx)
{
    Session s(a["project"].toString());
    const QString formatId = a.contains("format") ? a["format"].toString() : "h264";
    const auto& formats = renderFormats();
    if (std::none_of(formats.begin(), formats.end(), [&](const RenderFormatInfo& i) { return formatId == i.id; }))
        fail("BAD_ARGUMENT", "unknown format " + formatId + " (h264, h265, prores, aac, mp3, wav, aiff, alac)");
    QString out = absolute(a["out"].toString());
    if (QFileInfo(out).suffix().isEmpty()) out += "." + QString(renderFormat(formatId).extension);
    if (QFileInfo::exists(out) && !a["overwrite"].toBool())
        fail("EXISTS", "output exists (pass overwrite to replace it): " + out);
    RenderJob job;
    job.path = out;
    job.settings.format = formatId;
    job.settings.quality = std::clamp(a["quality"].toInt(), 0, 2);
    if (a.contains("height")) job.settings.shortSide = a["height"].toInt();
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
    return {{"out", out}, {"format", formatId}, {"render_s", std::round(timer.elapsed() / 100.0) / 10},
            {"width", job.size.width()}, {"height", job.size.height()}};
}

QJsonObject cmdHelp(const QJsonObject& a, Context&);

const QString kEditHelp = QStringLiteral(
    "List of operations, applied in order, saved once (nothing is saved if one fails). Times: frames (integer), "
    "\"4.5s\" or \"HH:MM:SS:FF\"; ranges are half-open [from, to). Tracks: \"V1\", \"A2\" (a media clip goes to "
    "V<n> and A<n>). Clip ids come from `info` and change when clips are split. Operations: "
    "{op:'append', media, in?, out?, track?} put a source range at the end of the timeline; "
    "{op:'place', media, at, in?, out?, track?} overwrite at a position; "
    "{op:'keep', media, ranges:[[from,to],...], at?, track?} put several source ranges one after another (e.g. the "
    "'sound' ranges of `silence`); "
    "{op:'split', at, clip?} cut all clips under `at` (or one clip); "
    "{op:'delete', clips:[ids], ripple?:false}; "
    "{op:'delete_range', from, to, ripple?:true} remove a time range on all tracks; "
    "{op:'move', clips, by|to, track_delta?}; "
    "{op:'trim', clip, edge:'start'|'end', by|to}; "
    "{op:'title', text, at?, duration?, track?, size?, y?}; "
    "{op:'fade', clip, in?, out?} fade lengths; "
    "{op:'volume', clips, db}; "
    "{op:'speed', clips, speed, reverse?, ripple?:true}; "
    "{op:'transition', at} cross dissolve at the cut nearest to `at`; "
    "{op:'marker', at}; {op:'enable'|'disable', clips}; {op:'clear'} empty the timeline. "
    "Linked audio/video partners are included unless linked:false.");

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
        {"info", "Show a project: format, media, tracks and clips (ids, positions, source ranges).",
         {{"project", T::Path, "project file", true, true}}, cmdInfo},
        {"import", "Add media files to a project's media pool.",
         {{"project", T::Path, "project file", true, true}, {"files", T::Paths, "media files", true, true}}, cmdImport},
        {"edit", "Apply edit operations to a project and save it (a backup of the previous state is kept).",
         {{"project", T::Path, "project file", true, true},
          {"ops", T::Json, kEditHelp, true},
          {"dry_run", T::Boolean, "only show the resulting timeline, do not save"}},
         cmdEdit},
        {"render", "Render the project timeline to a video or audio file.",
         {{"project", T::Path, "project file", true, true},
          {"out", T::Path, "output file (extension added if missing)", true},
          {"format", T::String, "h264 (default), h265, prores, aac, mp3, wav, aiff, alac"},
          {"quality", T::Integer, "0 = high (default), 1 = medium, 2 = small (H.264/H.265)"},
          {"height", T::Integer, "shorter image edge, e.g. 720 (default: timeline resolution)"},
          {"from", T::Time, "start of the range (default: timeline start)"},
          {"to", T::Time, "end of the range, exclusive (default: timeline end)"},
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
        {"frames", "Still images of a media file or a project timeline at given times, as files or one contact "
                   "sheet with timecodes (lets you look at the material).",
         {{"source", T::Path, "media file or project (.schneidi)", true, true},
          {"at", T::Times, "times to show"},
          {"every", T::Time, "one frame every … (e.g. 10s)"},
          {"count", T::Integer, "this many frames spread evenly"},
          {"out", T::Path, "folder for the images (with sheet: image file)"},
          {"sheet", T::Boolean, "one contact sheet image instead of single images"},
          {"width", T::Integer, "longer image edge in pixels (default 640, MCP 480)"},
          {"project", T::Path, "for a media file: count frames in this project's frame rate"}},
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
        list << QJsonObject{{"name", c.name}, {"summary", c.summary}, {"positional", positional},
                            {"params", inputSchema(c)}};
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
