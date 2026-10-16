// schneidi-cli multicam (docs/archiv/plan-0.8.md): edit operations to make multicam clips from synced recordings, set
// source markers, cut/switch angles (one or a whole list), flatten, resync and rename angles, all through the
// Editor like the app; plus the `sync` command that measures the offsets of files by their sound. Angles are
// 1-based here (like the grid and the keys 1-9), 0-based in the model.
#include "cli/CommandsDetail.h"

#include "core/Multicam.h"
#include "core/TimelineOps.h"
#include "engine/Profiles.h"
#include "engine/SoundSync.h"

#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <climits>
#include <cmath>

namespace Cli::detail {

QString syncMethodName(Multicam::SyncMethod m)
{
    switch (m) {
    case Multicam::SyncMethod::Sound: return "sound";
    case Multicam::SyncMethod::Marker: return "marker";
    case Multicam::SyncMethod::InPoint: return "in";
    case Multicam::SyncMethod::Start: return "start";
    }
    return {};
}

QJsonArray multicamAnglesJson(const Sequence& s, const QVector<Multicam::Angle>* sync)
{
    QJsonArray angles;
    for (int i = 0; i < s.angles.size(); ++i) {
        const MulticamAngle& a = s.angles[i];
        // The recordings of the angle: its picture track, else its first sound track (one clip per file, unless
        // it was split inside the multicam clip). start = where frame 0 of the file lies in the multicam clip.
        const Track* t = a.videoTrack >= 0 && a.videoTrack < s.timeline.video.size() ? &s.timeline.video[a.videoTrack]
                         : !a.audioTracks.isEmpty() && a.audioTracks.first() < s.timeline.audio.size()
                             ? &s.timeline.audio[a.audioTracks.first()]
                             : nullptr;
        QJsonArray files;
        QString lastPath;
        int lastStart = INT_MIN;
        int fileIndex = 0;
        for (const Clip& c : t ? t->clips : QVector<Clip>{}) {
            const int start = c.start - c.in;
            if (c.mediaPath == lastPath && start == lastStart) continue;
            lastPath = c.mediaPath;
            lastStart = start;
            QJsonObject f{{"path", c.mediaPath}, {"start", start}};
            // How it was synced: known in the answer of the edit that made the multicam clip
            if (sync && i < sync->size()) {
                const QVector<Multicam::FilePlacement>& placed = (*sync)[i].files;
                for (int k = fileIndex; k < placed.size(); ++k)
                    if (placed[k].path == c.mediaPath) {
                        f["synced"] = placed[k].synced;
                        f["via"] = syncMethodName(placed[k].via);
                        if (placed[k].via == Multicam::SyncMethod::Sound)
                            f["confidence"] = std::round(placed[k].confidence * 1000) / 1000;
                        fileIndex = k + 1;
                        break;
                    }
            }
            files << f;
        }
        QJsonObject o{{"angle", i + 1}, {"name", a.name}, {"files", files}};
        if (a.videoTrack < 0) o["video"] = false; // sound only (e.g. the caster)
        if (a.audioTracks.isEmpty()) o["audio"] = false;
        angles << o;
    }
    return angles;
}

namespace {

using Multicam::Mode;
using Multicam::SyncMethod;

// ---------- parsing ----------

// The recordings of a multicam op: files (one angle each) or angles [{name?, files}]; absolute paths
QVector<Multicam::Source> sourcesOf(const QJsonObject& op)
{
    QVector<Multicam::Source> out;
    const auto paths = [](const QJsonValue& v, const QString& what) {
        if (!v.isArray()) fail("BAD_ARGUMENT", what + ": 'files' must be a list of media files");
        QStringList list;
        for (const QJsonValue& f : v.toArray()) {
            if (!f.isString() || f.toString().trimmed().isEmpty()) fail("BAD_ARGUMENT", what + ": files are paths");
            list << absolute(f.toString());
        }
        if (list.isEmpty()) fail("BAD_ARGUMENT", what + ": 'files' is empty");
        return list;
    };
    if (op.contains("angles")) {
        const QJsonArray angles = op.value("angles").toArray();
        if (angles.isEmpty()) fail("BAD_ARGUMENT", "'angles' must be a non-empty list of {name?, files}");
        for (int i = 0; i < angles.size(); ++i) {
            const QJsonObject a = angles[i].toObject();
            const QString what = QString("angle %1").arg(i + 1);
            if (!a.contains("files")) fail("BAD_ARGUMENT", what + ": missing 'files' (the recording, several = split recording)");
            out << Multicam::Source{a.value("name").toString(), paths(a.value("files"), what)};
        }
    } else if (op.contains("files")) {
        for (const QString& f : paths(op.value("files"), "multicam")) out << Multicam::Source{{}, {f}};
    } else {
        fail("BAD_ARGUMENT", "missing 'files' (one recording per angle) or 'angles' ([{name?, files}])");
    }
    return out;
}

SyncMethod syncOf(const QJsonObject& op)
{
    const QString v = op.value("sync").toString("sound");
    const int i = QStringList{"sound", "marker", "in", "start"}.indexOf(v);
    if (i < 0) fail("BAD_ARGUMENT", "sync must be sound (default), marker, in or start, got '" + v + "'");
    return SyncMethod(i);
}

int streamOf(const QJsonObject& op)
{
    if (!op.contains("stream")) return 0;
    const QJsonValue v = op.value("stream");
    if (!v.isDouble() || v.toDouble() < 0 || v.toDouble() != std::floor(v.toDouble()))
        fail("BAD_ARGUMENT", "'stream' must be an audio stream number >= 0 (0 = first; `probe` lists them)");
    return v.toInt();
}

QVector<SoundSync::Input> soundInputs(const QVector<Multicam::Source>& sources, int stream)
{
    QVector<SoundSync::Input> in;
    for (const Multicam::Source& src : sources)
        for (const QString& f : src.files) in << SoundSync::Input{f, stream};
    return in;
}

QJsonArray matchesJson(const QVector<AudioSync::Match>& list)
{
    QJsonArray out;
    for (const AudioSync::Match& m : list)
        out << QJsonObject{{"found", m.found}, {"offset_s", m.offsetS}, {"confidence", m.confidence},
                           {"drift_ms", m.driftMs}, {"via", m.via}};
    return out;
}

QVector<AudioSync::Match> matchesFrom(const QJsonArray& list)
{
    QVector<AudioSync::Match> out;
    for (const QJsonValue& v : list) {
        AudioSync::Match m;
        m.found = v["found"].toBool();
        m.offsetS = v["offset_s"].toDouble();
        m.confidence = v["confidence"].toDouble();
        m.driftMs = v["drift_ms"].toDouble();
        m.via = v["via"].toInt(-1);
        out << m;
    }
    return out;
}

SoundSync::Progress progressOf(Context* ctx)
{
    if (!ctx || !ctx->progress) return {};
    return [ctx](double p) {
        ctx->progress(p);
        return true;
    };
}

// Multicam sequence of the op: `timeline` (id or name) or `clip` (a segment of it)
const Sequence& multicamOf(Session& s, const QJsonObject& op)
{
    int id = 0;
    if (op.contains("timeline")) id = s.sequenceOf(op.value("timeline"));
    else if (op.contains("clip")) {
        const Clip& c = clipOf(s, op);
        if (!c.isMulticam()) fail("BAD_ARGUMENT", QString("clip %1 is not a multicam clip").arg(c.id));
        id = c.sequenceId;
    } else {
        fail("BAD_ARGUMENT", "missing 'timeline' (id or name of the multicam clip) or 'clip' (a segment of it)");
    }
    const Sequence* q = s.project.sequence(id);
    if (!q || !q->multicam) fail("BAD_ARGUMENT", QString("timeline '%1' is not a multicam clip").arg(s.project.sequenceName(id)));
    return *q;
}

QString angleList(const Sequence& q)
{
    QStringList names;
    for (int i = 0; i < q.angles.size(); ++i) names << QString("%1 %2").arg(i + 1).arg(q.angles[i].name);
    return names.join(", ");
}

// Angle by number (1-based) or name -> index in Sequence::angles
int angleOf(const Sequence& q, const QJsonValue& v)
{
    const int n = q.angles.size();
    if (v.isString()) {
        const QString name = v.toString().trimmed();
        for (int i = 0; i < n; ++i)
            if (q.angles[i].name.compare(name, Qt::CaseInsensitive) == 0) return i;
        bool ok = false;
        const int number = name.toInt(&ok);
        if (!ok)
            fail("BAD_ARGUMENT", QString("no angle '%1' in multicam clip '%2' (angles: %3)").arg(name, q.name, angleList(q)));
        return angleOf(q, number);
    }
    if (!v.isDouble()) fail("BAD_ARGUMENT", "missing 'angle' (number from 1 or name)");
    const double d = v.toDouble();
    if (d < 1 || d > n || d != std::floor(d))
        fail("BAD_ARGUMENT", QString("angle must be 1..%1: multicam clip '%2' has %1 angle(s) (%3), got %4")
                                 .arg(n).arg(q.name, angleList(q)).arg(d));
    return int(d) - 1;
}

Mode modeOf(const QJsonObject& op)
{
    const QString v = op.value("mode").toString("both");
    const int i = QStringList{"both", "video", "audio"}.indexOf(v);
    if (i < 0) fail("BAD_ARGUMENT", "mode must be both (default: picture and sound), video or audio, got '" + v + "'");
    return Mode(i);
}

// Signed time: frames, "-12", "1.5s", "-0.5s", timecode
int signedTime(Session& s, const QJsonValue& v, const char* what)
{
    if (v.isDouble()) {
        if (v.toDouble() != std::floor(v.toDouble())) fail("BAD_ARGUMENT", QString("'%1' must be whole frames").arg(what));
        return v.toInt();
    }
    const QString str = v.toString().trimmed();
    if (str.isEmpty()) fail("BAD_ARGUMENT", QString("missing '%1'").arg(what));
    if (str.startsWith('-')) return -parseTime(str.mid(1), s.format(), what);
    return parseTime(str, s.format(), what);
}

// ---------- targets of cuts ----------

// Which multicam segments a cut acts on: `clip` given = the segments of its multicam clip on its track, otherwise
// what the app's keys act on (Multicam::clipAt: top-most picture, else sound)
struct CutTarget {
    bool fixed = false;
    TrackRef ref;
    int sequenceId = 0;
};

CutTarget cutTargetOf(Session& s, const QJsonObject& op)
{
    CutTarget t;
    if (!op.contains("clip")) return t;
    const Clip& c = clipOf(s, op);
    if (!c.isMulticam()) fail("BAD_ARGUMENT", QString("clip %1 is not a multicam clip").arg(c.id));
    TimelineOps::findClip(s.project.timeline(), c.id, &t.ref);
    t.fixed = true;
    t.sequenceId = c.sequenceId;
    return t;
}

// The multicam sequence cut at `frame`; selects the segment for the editor when the target is fixed
const Sequence& sequenceAt(Session& s, const CutTarget& t, int frame)
{
    const Timeline& tl = s.project.timeline();
    const Clip* seg = nullptr;
    if (t.fixed) {
        for (const Clip& c : tl.track(t.ref).clips)
            if (c.isMulticam() && c.sequenceId == t.sequenceId && c.start <= frame && frame < c.end()) seg = &c;
        if (!seg) fail("NOT_FOUND", QString("the multicam clip on %1 does not cover %2").arg(trackName(t.ref), tc(frame, s.format())));
        s.selection.set({seg->id});
    } else {
        seg = Multicam::clipAt(tl, frame);
        if (!seg) fail("NOT_FOUND", QString("no multicam clip at %1 (place one with {op:'multicam', …, at})").arg(tc(frame, s.format())));
        s.selection.clear();
    }
    const Sequence* q = s.project.sequence(seg->sequenceId);
    if (!q) fail("NOT_FOUND", "the multicam clip's content is missing");
    return *q;
}

void checkAngleMode(const Sequence& q, int angle, Mode mode)
{
    if (mode == Mode::Video && !Multicam::angleHas(q, angle, TrackKind::Video))
        fail("BAD_ARGUMENT", QString("angle %1 (%2) has no picture").arg(angle + 1).arg(q.angles[angle].name));
    if (mode == Mode::Audio && !Multicam::angleHas(q, angle, TrackKind::Audio))
        fail("BAD_ARGUMENT", QString("angle %1 (%2) has no sound").arg(angle + 1).arg(q.angles[angle].name));
}

void applyCut(Session& s, const CutTarget& t, int frame, int angle, Mode mode, bool cut)
{
    const QString name = sequenceAt(s, t, frame).angles.value(angle).name;
    const bool changed = cut ? s.editor.multicamCut(frame, angle, mode) : s.editor.multicamSwitch(frame, angle, mode);
    if (!changed)
        s.notes << QString("%1: already angle %2 (%3), nothing changed").arg(tc(frame, s.format())).arg(angle + 1).arg(name);
    s.selection.clear();
}

// ---------- ops ----------

void opMulticam(Session& s, const QJsonObject& op)
{
    const QVector<Multicam::Source> sources = sourcesOf(op);
    const SyncMethod method = syncOf(op);
    const int stream = streamOf(op);
    const QString name = op.value("name").toString().trimmed();
    for (const Sequence& q : s.project.sequences())
        if (!name.isEmpty() && q.name.compare(name, Qt::CaseInsensitive) == 0)
            fail("EXISTS", QString("there is a timeline '%1' already (id %2)").arg(q.name).arg(q.id));
    for (const Multicam::Source& src : sources)
        for (const QString& f : src.files) s.media(f); // imported into the Media Pool (fails if unreadable)

    QVector<AudioSync::Match> sound;
    if (method == SyncMethod::Sound) {
        // Live: schneidi-cli measured it already (withSoundSync), the app does not decode for minutes
        sound = op.contains("_sound") ? matchesFrom(op.value("_sound").toArray())
                                      : SoundSync::align(soundInputs(sources, stream));
    }
    const QVector<Multicam::Angle> angles = Multicam::resolve(s.project, sources, method, sound);
    const int id = s.editor.createMulticamClip(name, angles);
    if (!id) fail("FAILED", "none of the files can be used for a multicam clip (stills or no picture and sound)");
    s.multicamSync.insert(id, angles);

    // Tell which recordings are not in sync (placed at the start of their angle / after the previous file)
    const char* why = method == SyncMethod::Sound    ? "its sound did not match and there are no source markers"
                      : method == SyncMethod::Marker ? "no source marker in it or in the reference"
                                                     : "a later file of a split recording";
    int index = 0;
    for (int i = 0; i < angles.size(); ++i)
        for (const Multicam::FilePlacement& f : angles[i].files) {
            const AudioSync::Match m = sound.value(index++);
            if (!f.synced)
                s.notes << QString("angle %1 (%2): %3 is not synced (%4); placed %5. Fix with multicam_nudge or "
                                   "source markers")
                               .arg(i + 1)
                               .arg(angles[i].name, QFileInfo(f.path).fileName(), why,
                                    f.offset == 0 ? "at the start" : "right after the previous file");
            else if (m.found && f.via == SyncMethod::Sound && std::abs(m.driftMs) * s.project.frameRate() >= 500)
                s.notes << QString("angle %1 (%2): %3 drifts by %4 ms over the shared part (different clocks); "
                                   "not corrected")
                               .arg(i + 1)
                               .arg(angles[i].name, QFileInfo(f.path).fileName())
                               .arg(std::lround(m.driftMs));
        }
    if (op.contains("at") && !s.editor.addSequenceAt(id, timeOf(s, op, "at"), parseTrack(op.value("track"))))
        fail("FAILED", "cannot place the multicam clip there (locked track?)");
}

void opSourceMarker(Session& s, const QJsonObject& op)
{
    if (!op.contains("media")) fail("BAD_ARGUMENT", "missing 'media' (file in the Media Pool)");
    const MediaInfo m = s.media(op.value("media").toString());
    if (!op.contains("at")) fail("BAD_ARGUMENT", "missing 'at' (source time in the file)");
    const int at = parseTime(op.value("at"), s.format(), "at");
    if (at >= m.length) fail("BAD_ARGUMENT", QString("'at' is after the end of %1 (%2 frames)").arg(m.path).arg(m.length));
    const bool remove = op.value("remove").toBool();
    if (m.markers.contains(at) != remove) return; // already as wanted
    s.editor.toggleSourceMarker(m.path, at);
}

void opAngle(Session& s, const QJsonObject& op)
{
    const int at = timeOf(s, op, "at");
    const Mode mode = modeOf(op);
    const CutTarget t = cutTargetOf(s, op);
    const Sequence& q = sequenceAt(s, t, at);
    const int angle = angleOf(q, op.value("angle"));
    checkAngleMode(q, angle, mode);
    applyCut(s, t, at, angle, mode, op.value("cut").toBool(true));
}

void opAngleCuts(Session& s, const QJsonObject& op)
{
    const Mode mode = modeOf(op);
    const CutTarget t = cutTargetOf(s, op);
    const QJsonArray list = op.value("cuts").toArray();
    if (list.isEmpty()) fail("BAD_ARGUMENT", "'cuts' must be a non-empty list of [at, angle] or {at, angle}");
    struct Cut {
        int index, at, angle;
    };
    QVector<Cut> cuts;
    // All checked first: a bad cut applies none of them
    for (int i = 0; i < list.size(); ++i) {
        try {
            QJsonObject c;
            if (list[i].isArray() && list[i].toArray().size() == 2) c = {{"at", list[i][0]}, {"angle", list[i][1]}};
            else if (list[i].isObject() && list[i].toObject().contains("at") && list[i].toObject().contains("angle"))
                c = list[i].toObject();
            else fail("BAD_ARGUMENT", "a cut is [at, angle] or {at, angle}");
            const int at = timeOf(s, c, "at");
            const Sequence& q = sequenceAt(s, t, at);
            const int angle = angleOf(q, c.value("angle"));
            checkAngleMode(q, angle, mode);
            cuts << Cut{i, at, angle};
        } catch (const Error& e) {
            throw Error{e.code, QString("cut %1: %2").arg(i).arg(e.message)};
        }
    }
    std::stable_sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.at < b.at; });
    for (int i = 1; i < cuts.size(); ++i)
        if (cuts[i].at == cuts[i - 1].at)
            fail("BAD_ARGUMENT", QString("cut %1 and cut %2 are both at %3")
                                     .arg(cuts[i - 1].index).arg(cuts[i].index).arg(tc(cuts[i].at, s.format())));
    for (const Cut& c : cuts) applyCut(s, t, c.at, c.angle, mode, true);
}

void opFlatten(Session& s, const QJsonObject& op)
{
    QVector<int> ids;
    if (op.contains("clips") || op.contains("clip")) {
        ids = clipIds(s, op);
    } else { // all multicam clips of the timeline
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : s.project.timeline().tracks(k))
                for (const Clip& c : t.clips)
                    if (c.isMulticam()) ids << c.id;
    }
    if (!s.editor.flattenMulticamClips(ids)) fail("FAILED", "none of the clips is a multicam clip (or they are on locked tracks)");
}

void opMulticamNudge(Session& s, const QJsonObject& op)
{
    const Sequence& q = multicamOf(s, op);
    const int seq = q.id;
    const int angle = angleOf(q, op.value("angle"));
    if (!op.contains("by")) fail("BAD_ARGUMENT", "missing 'by' (frames or seconds, negative = earlier)");
    const int by = signedTime(s, op.value("by"), "by");
    const auto firstStart = [&] {
        const Sequence* x = s.project.sequence(seq);
        const MulticamAngle& a = x->angles[angle];
        int first = INT_MAX;
        QVector<const Track*> tracks;
        if (a.videoTrack >= 0 && a.videoTrack < x->timeline.video.size()) tracks << &x->timeline.video[a.videoTrack];
        for (int i : a.audioTracks)
            if (i >= 0 && i < x->timeline.audio.size()) tracks << &x->timeline.audio[i];
        for (const Track* t : tracks)
            for (const Clip& c : t->clips) first = std::min(first, c.start);
        return first;
    };
    const int before = firstStart();
    s.editor.nudgeMulticamAngle(seq, angle, by);
    if (const int moved = firstStart() - before; before != INT_MAX && moved != by)
        s.notes << QString("angle %1 moved by %2 frame(s) only: it cannot start before the multicam clip").arg(angle + 1).arg(moved);
}

void opAngleName(Session& s, const QJsonObject& op)
{
    const Sequence& q = multicamOf(s, op);
    const int seq = q.id;
    const int angle = angleOf(q, op.value("angle"));
    if (!op.contains("name")) fail("BAD_ARGUMENT", "missing 'name' (empty = file name)");
    s.editor.renameMulticamAngle(seq, angle, op.value("name").toString());
}

} // namespace

QJsonObject withSoundSync(const QJsonObject& editArgs, Context& ctx)
{
    QJsonArray ops = editArgs.value("ops").isObject() ? QJsonArray{editArgs.value("ops")} : editArgs.value("ops").toArray();
    bool any = false;
    for (int i = 0; i < ops.size(); ++i) {
        QJsonObject op = ops[i].toObject();
        if (op.value("op").toString() != "multicam" || op.contains("_sound")) continue;
        try {
            if (syncOf(op) != SyncMethod::Sound) continue;
            const QVector<Multicam::Source> sources = sourcesOf(op);
            op["_sound"] = matchesJson(SoundSync::align(soundInputs(sources, streamOf(op)), {}, progressOf(&ctx)));
        } catch (const Error&) {
            continue; // the op reports it where it runs
        }
        ops[i] = op;
        any = true;
    }
    if (!any) return editArgs;
    QJsonObject out = editArgs;
    out["ops"] = ops;
    return out;
}

QJsonObject cmdSync(const QJsonObject& a, Context& ctx)
{
    const QJsonArray list = a["files"].toArray();
    if (list.size() < 2) fail("BAD_ARGUMENT", "give at least two 'files' (the first is the reference)");
    QVector<SoundSync::Input> in;
    int stream = 0;
    if (a.contains("stream")) {
        stream = a["stream"].toInt();
        if (stream < 0) fail("BAD_ARGUMENT", "'stream' must be >= 0 (0 = first audio stream)");
    }
    for (const QJsonValue& v : list) {
        const QString path = absolute(v.toString());
        if (!QFileInfo::exists(path)) fail("NOT_FOUND", "file not found: " + path);
        in << SoundSync::Input{path, stream};
    }
    // Frames count in the project's frame rate, else in the reference file's own
    double fps = 25;
    if (a.contains("project")) {
        Session s(a["project"].toString());
        fps = s.format().rate.fps();
    } else if (const ClipFormat cf = detectClipFormat(in.first().path); cf.ok) {
        fps = cf.suggested.fps();
    }
    AudioSync::Options options;
    if (a.contains("max_offset_s")) options.maxOffsetS = std::max(0.0, a["max_offset_s"].toDouble());
    const QVector<AudioSync::Match> matches = SoundSync::align(in, options, progressOf(&ctx));
    if (matches.size() != in.size()) fail("FAILED", "sync was canceled");
    QJsonArray files;
    for (int i = 0; i < in.size(); ++i) {
        const AudioSync::Match& m = matches[i];
        QJsonObject o{{"path", in[i].path}, {"found", m.found}};
        if (m.found) {
            o["offset_s"] = std::round(m.offsetS * 10000) / 10000;
            o["offset_frames"] = int(std::lround(m.offsetS * fps));
            o["confidence"] = std::round(m.confidence * 1000) / 1000;
            o["drift_ms"] = std::round(m.driftMs * 10) / 10;
            if (m.via > 0 && m.via < in.size()) o["via"] = in[m.via].path; // matched over another file (chain)
        }
        files << o;
    }
    return {{"reference", in.first().path}, {"fps", std::round(fps * 1000) / 1000}, {"files", files}};
}

void addMulticamOps(QVector<OpDef>& ops)
{
    ops << OpDef{"multicam", "{op:'multicam', files|angles, sync?:'sound', name?, stream?, at?, track?}",
                 "multicam clip from recordings of the same event (one angle per camera/POV), synced; at = also "
                 "place it in this timeline",
                 "files:[paths] = one angle per file; angles:[{name?, files:[paths]}] = several files for a recording "
                 "split after a restart (each synced on its own, gaps stay black/silent). Angle names default to the "
                 "file name; name = the multicam clip's name (default \"Multicam-Clip N\"). sync: sound (default: by the "
                 "shared sounds like round start, announcements; stream = audio stream used for it, default 0), marker "
                 "(first source markers on top of each other, set them with source_marker), in (source In points) or "
                 "start. The reference is the first file of the first angle; a file the sound does not find falls back "
                 "to source markers, else to the start of its angle (a note names it). The multicam clip is a timeline "
                 "(`info` timelines: multicam, angles with each file's start and how it was synced). Its segments in a "
                 "timeline show one angle each (clip kind 'multicam', angle 1-based); cut with 'angle'/'angle_cuts'.",
                 opMulticam}
        << OpDef{"source_marker", "{op:'source_marker', media, at, remove?}",
                 "source marker in a media file (M in the source viewer), for sync:'marker'",
                 "at = source time in the file (frames in the project rate or \"12.4s\"); remove:true removes the marker "
                 "there. `info` lists them per media file (markers).",
                 opSourceMarker}
        << OpDef{"angle", "{op:'angle', at, angle, cut?:true, mode?:'both', clip?}",
                 "cut the multicam clip at `at` to an angle (like keys 1-9), or cut:false = switch the segment there "
                 "(Alt+1-9)",
                 "angle = number from 1 or the angle's name. mode: both (picture and sound), video or audio (the other "
                 "part is cut too but keeps its angle). Acts on the top-most multicam clip at `at` (picture, else "
                 "sound), or on the multicam clip of segment `clip` on its track. A cut on an existing edit only "
                 "switches.",
                 opAngle}
        << OpDef{"angle_cuts", "{op:'angle_cuts', cuts:[[at, angle], …], mode?, clip?}",
                 "many angle cuts at once (e.g. switch to the POV of each kill), applied in time order",
                 "cuts: [at, angle] or {at, angle} in any order, angle = number from 1 or name. All are checked first: "
                 "a bad cut (its index is named) or two cuts at the same time apply none of them. mode and clip as in "
                 "'angle'.",
                 opAngleCuts}
        << OpDef{"flatten", "{op:'flatten', clips?}",
                 "replace multicam segments by normal clips of their angle (default: all in this timeline)",
                 "Keeps transform, effects, volume and fades of the segments; moves up to a free track where one is "
                 "taken; gaps of a recording stay empty.",
                 opFlatten}
        << OpDef{"multicam_nudge", "{op:'multicam_nudge', timeline|clip, angle, by}",
                 "resync: move an angle inside the multicam clip by frames or seconds (negative = earlier)",
                 "timeline = id or name of the multicam clip, or clip = a segment of it. The angle cannot start before "
                 "the multicam clip.",
                 opMulticamNudge}
        << OpDef{"angle_name", "{op:'angle_name', timeline|clip, angle, name}",
                 "rename an angle of a multicam clip (empty = file name)", {}, opAngleName};
}

} // namespace Cli::detail
