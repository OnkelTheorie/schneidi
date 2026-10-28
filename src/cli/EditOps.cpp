// schneidi-cli: the operations of the `edit` command (one JSON object each, applied through the Editor like in the app)
#include "cli/CommandsDetail.h"

#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/Retime.h"
#include "core/StreamPresets.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Cli::detail {

const Clip& clipOf(Session& s, const QJsonObject& op, const char* key)
{
    QJsonValue v = op.value(key);
    // A list of one under 'clips' (the key of ops for several; gemma4 sent trim clips:[1]): that clip, with a note
    if (v.isUndefined() && QString(key) == "clip" && op.value("clips").isArray()) {
        const QJsonArray list = op.value("clips").toArray();
        if (list.size() != 1)
            fail("BAD_ARGUMENT", QString("%1 takes one clip: 'clip' (a clip id); for several clips one op each")
                                     .arg(op.value("op").toString()));
        v = list.first();
        s.notes << QString("%1: clips:[%2] read as clip:%2")
                       .arg(op.value("op").toString())
                       .arg(v.toVariant().toString());
    }
    if (v.isUndefined()) fail("BAD_ARGUMENT", QString("missing '%1' (clip id)").arg(key));
    bool number = v.isDouble();
    const int id = number ? v.toInt() : v.toString().trimmed().toInt(&number); // "7" is 7
    if (!number)
        fail("BAD_ARGUMENT",
             QString("'%1': %2 is no clip id (a number, as `info` lists them)").arg(key, v.toVariant().toString()));
    const Clip* c = TimelineOps::findClip(s.project.timeline(), id);
    if (!c) fail("NOT_FOUND", QString("no clip with id %1").arg(id));
    return *c;
}

// Clip ids of `clips` (or `clip`); cues = subtitle ids allowed too (delete, move)
QVector<int> clipIds(Session& s, const QJsonObject& op, bool cues)
{
    QVector<int> ids;
    const QJsonValue v = op.value("clips");
    const QJsonArray arr = v.isArray() ? v.toArray() : QJsonArray{op.value("clip")};
    for (const QJsonValue& x : arr) {
        const int id = x.toInt();
        if (!TimelineOps::findClip(s.project.timeline(), id) && !(cues && Subtitles::find(s.project.timeline(), id)))
            fail("NOT_FOUND", QString("no %1 with id %2").arg(cues ? "clip or subtitle" : "clip").arg(id));
        ids << id;
    }
    if (ids.isEmpty()) fail("BAD_ARGUMENT", "missing 'clips' (list of clip ids)");
    // Linked partners (video + its audio) go along, like clicking a clip in the timeline; subtitles have none
    if (!op.value("linked").toBool(true)) return ids;
    QVector<int> clips, subtitles;
    for (int id : ids) (s.editor.isSubtitle(id) ? subtitles : clips) << id; // keeps the order ('move … to' uses the first)
    return s.editor.withLinked(clips) + subtitles;
}

namespace {

// Timeline frame where clip c plays source frame `source` (file frames in the project rate, as `probe` with a
// project counts them); speed, reverse and speed ramps included. -1 = the clip does not play that frame.
int timelineAtSource(const Session& s, const Clip& c, int source)
{
    const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
    const RetimeMap map(c, m ? m->length : 0);
    const int t = c.start + int(std::lround(map.materialAtFile(source))) - c.in; // first frame showing it
    return t >= c.start && t < c.end() ? t : -1;
}

QString sourceSpan(const Session& s, const Clip& c)
{
    const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
    const RetimeMap map(c, m ? m->length : 0);
    const double a = map.fileFrameAt(c.in), b = map.fileFrameAt(c.out);
    return QString("clip %1 plays source %2..%3").arg(c.id).arg(tc(int(std::min(a, b)), s.format()), tc(int(std::max(a, b)) + 1, s.format()));
}

// A time given relative to a clip or to the source material instead of the timeline:
// {clip, offset?} (from the clip start), {clip, from_end} (back from its end), {clip, source} (where the clip plays
// this source time) or {media, source} (the first clip on the timeline that plays it). They are resolved when the op
// runs, so earlier ops that move clips (speed ramps, ripple) do not shift them.
int timeRef(Session& s, const QJsonObject& r, const char* key)
{
    const ProjectFormat& f = s.format();
    const QString what = QString(key);
    if (r.contains("clip")) {
        const Clip& c = clipOf(s, r);
        if (r.contains("source")) {
            if (c.kind != ClipKind::Media) fail("BAD_ARGUMENT", QString("%1: clip %2 has no source material").arg(what).arg(c.id));
            const int t = timelineAtSource(s, c, parseTime(r["source"], f, what + ".source"));
            if (t < 0) fail("BAD_ARGUMENT", QString("%1: source time is not inside the clip (%2)").arg(what, sourceSpan(s, c)));
            return t;
        }
        if (r.contains("from_end")) return c.end() - parseTime(r["from_end"], f, what + ".from_end");
        return c.start + (r.contains("offset") ? parseTime(r["offset"], f, what + ".offset") : 0);
    }
    if (r.contains("media") && r.contains("source")) {
        const QString path = absolute(r["media"].toString());
        const int source = parseTime(r["source"], f, what + ".source");
        const Timeline& tl = s.project.timeline();
        QStringList used;
        int best = -1;
        for (const QVector<Track>* tracks : {&tl.video, &tl.audio})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips) {
                    if (c.kind != ClipKind::Media || c.mediaPath != path) continue;
                    const int at = timelineAtSource(s, c, source);
                    if (at >= 0 && (best < 0 || at < best)) best = at;
                    if (used.size() < 12) used << sourceSpan(s, c);
                }
        if (best >= 0) return best;
        fail("BAD_ARGUMENT", used.isEmpty() ? QString("%1: %2 is not on the timeline").arg(what, path)
                                            : QString("%1: no clip plays that source time (%2)").arg(what, used.join("; ")));
    }
    fail("BAD_ARGUMENT", QString("%1: a time object needs {clip, offset?|from_end|source} or {media, source}").arg(what));
}

} // namespace

int timeOf(Session& s, const QJsonObject& op, const char* key, int fallback)
{
    if (!op.contains(key)) {
        if (fallback >= 0) return fallback;
        fail("BAD_ARGUMENT", QString("missing '%1'").arg(key));
    }
    if (op[key].isObject()) return timeRef(s, op[key].toObject(), key);
    return parseTime(op[key], s.format(), key);
}

QVector<int> onTracks(Session& s, const QVector<int>& ids, TrackKind kind, const char* what)
{
    QVector<int> out;
    for (int id : ids)
        if (TrackRef r; TimelineOps::findClip(s.project.timeline(), id, &r) && r.kind == kind) out << id;
    if (out.isEmpty())
        fail("BAD_ARGUMENT", QString("%1 go on %2 clips; none of the clips is on a %2 track")
                                 .arg(what, kind == TrackKind::Video ? "video" : "audio"));
    return out;
}

QVector<int> clipsOn(Session& s, const QJsonObject& op, TrackKind kind, const char* what)
{
    QJsonObject alone = op;
    alone["linked"] = false;
    if (op.contains("linked")) // true: all partners, false: strictly the given clips
        return onTracks(s, clipIds(s, op.value("linked").toBool() ? op : alone), kind, what);
    const Timeline& tl = s.project.timeline();
    QVector<int> out;
    for (int id : clipIds(s, alone)) {
        TrackRef r;
        const QVector<int> ids = TimelineOps::findClip(tl, id, &r) && r.kind == kind ? QVector<int>{id}
                                                                                     : s.editor.withLinked({id});
        for (int x : ids)
            if (TrackRef xr; !out.contains(x) && TimelineOps::findClip(tl, x, &xr) && xr.kind == kind) out << x;
    }
    if (out.isEmpty())
        fail("BAD_ARGUMENT", QString("%1 go on %2 clips; none of the clips is on a %2 track")
                                 .arg(what, kind == TrackKind::Video ? "video" : "audio"));
    return out;
}

double numberOf(const QJsonObject& op, const char* key, double lo, double hi)
{
    const QJsonValue v = op.value(key);
    if (!v.isDouble()) fail("BAD_ARGUMENT", QString("'%1' must be a number").arg(key));
    const double d = v.toDouble();
    if (d < lo || d > hi) fail("BAD_ARGUMENT", QString("'%1' must be between %2 and %3, got %4").arg(key).arg(lo).arg(hi).arg(d));
    return d;
}

QColor colorOf(const QJsonValue& v, const QString& what)
{
    const QColor c(v.toString());
    if (!v.isString() || !c.isValid()) fail("BAD_ARGUMENT", what + " must be a color like \"#ff8800\" or \"#80000000\" (with alpha)");
    return c;
}

namespace {

// Source range [in, out) of a media file: defaults to the whole file
void sourceRange(Session& s, const QJsonObject& op, const MediaInfo& m, int* in, int* out)
{
    *in = timeOf(s, op, "in", 0);
    *out = op.contains("out") ? timeOf(s, op, "out") : m.length;
    if (op.contains("duration")) *out = *in + timeOf(s, op, "duration");
    *out = std::min(*out, m.length);
    if (*in >= *out) fail("BAD_ARGUMENT", QString("empty source range %1..%2 (media has %3 frames)").arg(*in).arg(*out).arg(m.length));
}

// "ST1" or 1 -> subtitle track index; missing = -1 (the visible track)
int parseSubtitleTrack(const QJsonValue& v)
{
    if (v.isUndefined() || v.isNull()) return -1;
    QString str = v.isDouble() ? QString::number(v.toInt()) : v.toString().trimmed().toUpper();
    if (str.startsWith("ST")) str = str.mid(2);
    bool ok = false;
    const int n = str.toInt(&ok);
    if (!ok || n < 1 || n > 99) fail("BAD_ARGUMENT", "subtitle track must look like ST1 or 1, got " + v.toVariant().toString());
    return n - 1;
}

// JSON value -> effect parameter value (checked against the effect's description)
QVariant effectValue(const EffectDescriptor& d, const QString& key, const QJsonValue& v)
{
    const auto it = std::find_if(d.params.begin(), d.params.end(), [&](const EffectParam& p) { return p.key == key; });
    if (it == d.params.end()) {
        QStringList keys;
        for (const EffectParam& p : d.params)
            if (!p.sparse) keys << p.key;
        fail("BAD_ARGUMENT", QString("effect %1 has no parameter '%2' (has: %3)").arg(d.id, key, keys.join(", ")));
    }
    const EffectParam& p = *it;
    switch (p.type) {
    case EffectParam::Double:
        if (!v.isDouble()) fail("BAD_ARGUMENT", QString("%1.%2 must be a number").arg(d.id, key));
        return std::clamp(v.toDouble(), p.min, p.max);
    case EffectParam::Bool:
        if (!v.isBool()) fail("BAD_ARGUMENT", QString("%1.%2 must be true or false").arg(d.id, key));
        return v.toBool();
    case EffectParam::Color: {
        const QColor c(v.toString());
        if (!c.isValid()) fail("BAD_ARGUMENT", QString("%1.%2 must be a color like \"#00ff00\"").arg(d.id, key));
        return c;
    }
    case EffectParam::Path: {
        const QString path = absolute(v.toString());
        if (!path.isEmpty() && !QFileInfo::exists(path)) fail("NOT_FOUND", "file not found: " + path);
        return path;
    }
    case EffectParam::Choice:
        if (!p.choices.contains(v.toString()))
            fail("BAD_ARGUMENT", QString("%1.%2 must be one of: %3").arg(d.id, key, p.choices.join(", ")));
        return v.toString();
    }
    return {};
}

} // namespace


namespace {

int endOf(Session& s) { return TimelineOps::endFrame(s.project.timeline()); }

// presets:false = every audio stream of the file is placed, the import presets of the app are left aside for this op
struct PresetsScope {
    Editor& editor;
    StreamPresets saved;
    PresetsScope(Session& s, const QJsonObject& op) : editor(s.editor), saved(s.editor.streamPresets())
    {
        if (!op.value("presets").toBool(true)) editor.setStreamPresets(StreamPresets::none());
    }
    ~PresetsScope() { editor.setStreamPresets(saved); }
};

// A recording with a Mix stream plus its single streams (OBS, game clip tools) placed with all of them playing in
// [from, to): everything is heard twice, too loud - say so with the Mix clips to turn off (the user's import presets
// or the game template mute the Mix; the default plays every stream)
void noteDoubledMix(Session& s, const MediaInfo& m, int from, int to)
{
    if (m.audioStreamCount() < 2) return;
    QJsonArray mixClips;
    QStringList singles;
    for (const Track& t : s.project.timeline().audio) {
        if (t.muted) continue;
        for (const Clip& c : t.clips) {
            if (!c.enabled || c.mediaPath != m.path || c.end() <= from || c.start >= to) continue;
            const QString title = m.audioStreamName(c.audioStream);
            if (StreamPresets::isMixTitle(title)) mixClips << c.id;
            else if (!singles.contains(title)) singles << title;
        }
    }
    if (mixClips.isEmpty() || singles.isEmpty()) return;
    s.notes << QString("%1: its Mix stream plays together with its single streams (%2) - everything is heard twice; "
                       "{op:'disable', clips:%3} turns the Mix off (or `new` with template game mutes it on import)")
                   .arg(QFileInfo(m.path).fileName(), singles.join(", "),
                        QString::fromUtf8(QJsonDocument(mixClips).toJson(QJsonDocument::Compact)));
}

void opPlace(Session& s, const QJsonObject& op, bool append)
{
    const PresetsScope presets(s, op);
    const MediaInfo m = s.media(op.value("media").toString());
    int in = 0, out = 0;
    sourceRange(s, op, m, &in, &out);
    const int at = append ? timeOf(s, op, "at", endOf(s)) : timeOf(s, op, "at");
    s.editor.placeSourceRange(m.path, in, out - 1, at, parseTrack(op.value("track")));
    noteDoubledMix(s, m, at, at + out - in);
}

void opKeep(Session& s, const QJsonObject& op)
{
    const PresetsScope presets(s, op);
    const MediaInfo m = s.media(op.value("media").toString());
    int at = timeOf(s, op, "at", endOf(s));
    const int start = at;
    const int track = parseTrack(op.value("track"));
    QJsonArray ranges = op.value("ranges").toArray();
    // One range written another way (small models): a flat pair, 'range', or from/to on the op itself
    const auto pair = [](const QJsonArray& a) { return a.size() == 2 && !a[0].isArray() && !a[0].isObject(); };
    QString other;
    if (pair(ranges)) other = "ranges:[from, to]";
    else if (ranges.isEmpty() && op.value("range").isArray()) other = "range";
    else if (ranges.isEmpty() && op.contains("from") && op.contains("to")) other = "from/to";
    if (!other.isEmpty()) {
        ranges = QJsonArray{other == "range"     ? op.value("range").toArray()
                            : other == "from/to" ? QJsonArray{op.value("from"), op.value("to")}
                                                 : ranges};
        s.notes << QString("keep: %1 read as ranges:[[from, to]] (one range; several: [[from, to], [from, to]])")
                       .arg(other);
    }
    if (ranges.isEmpty())
        fail("BAD_ARGUMENT", "missing 'ranges': the parts of the source to keep in order, e.g. "
                             "{\"op\":\"keep\",\"media\":\"/path/to/clip.mkv\",\"ranges\":[[\"2s\",\"5.5s\"],[\"10s\",\"12s\"]]}");
    for (const QJsonValue& r : ranges) {
        QJsonObject sub;
        const QJsonObject o = r.toObject();
        if (r.isArray()) sub = {{"in", r.toArray().at(0)}, {"out", r.toArray().at(1)}};
        else if (o.contains("start")) sub = {{"in", o.value("start")}, {"out", o.value("end")}}; // as answers name ranges
        else sub = {{"in", o.value("from")}, {"out", o.value("to")}};
        int in = 0, out = 0;
        sourceRange(s, sub, m, &in, &out);
        s.editor.placeSourceRange(m.path, in, out - 1, at, track);
        at += out - in;
    }
    noteDoubledMix(s, m, start, at);
}

void opDelete(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipIds(s, op, true);
    s.selection.set(QSet<int>(ids.begin(), ids.end()));
    if (op.value("ripple").toBool()) s.editor.rippleDeleteSelection();
    else s.editor.deleteSelection();
}

void opMove(Session& s, const QJsonObject& op)
{
    const QJsonValue by = op.value("by");
    int delta = 0;
    if (by.isDouble()) delta = by.toInt();
    else if (by.isString() && by.toString().startsWith('-')) delta = -parseTime(by.toString().mid(1), s.format(), "by");
    else if (by.isString()) delta = parseTime(by, s.format(), "by");
    if (op.contains("to")) delta = timeOf(s, op, "to") - clipOf(s, {{"clip", clipIds(s, op).first()}}).start;
    s.editor.moveClips(clipIds(s, op), delta, TrackKind::Video, op.value("track_delta").toInt());
}

// Fade handles of the clips; the linked partners (the sound of a video clip) fade along unless linked:false
void opFade(Session& s, const QJsonObject& op)
{
    if (!op.contains("in") && !op.contains("out")) fail("BAD_ARGUMENT", "missing 'in' and/or 'out' (fade lengths)");
    for (int id : clipIds(s, op)) {
        if (s.editor.isSubtitle(id)) continue;
        if (op.contains("in")) s.editor.setClipFade(id, TimelineOps::Edge::Start, timeOf(s, op, "in"));
        if (op.contains("out")) s.editor.setClipFade(id, TimelineOps::Edge::End, timeOf(s, op, "out"));
    }
}

void opSubtitle(Session& s, const QJsonObject& op)
{
    // New cue at `at`, or (with id) change text/timing of an existing one
    Editor& ed = s.editor;
    int id = op.value("id").toInt();
    if (id) {
        if (!Subtitles::find(s.project.timeline(), id)) fail("NOT_FOUND", QString("no subtitle with id %1").arg(id));
        if (op.contains("text")) ed.setSubtitleText(id, op.value("text").toString());
    } else {
        if (!op.contains("text")) fail("BAD_ARGUMENT", "missing 'text'");
        id = ed.addSubtitle(timeOf(s, op, "at"), parseSubtitleTrack(op.value("track")), op.value("text").toString());
        if (!id) fail("FAILED", "a subtitle already starts there or the subtitle track is locked");
    }
    const SubtitleCue c = *Subtitles::find(s.project.timeline(), id);
    const int start = op.contains("at") ? timeOf(s, op, "at") : c.start;
    int stop = op.contains("to") ? timeOf(s, op, "to") : start + c.length();
    if (op.contains("duration")) stop = start + timeOf(s, op, "duration");
    if (stop <= start) fail("BAD_ARGUMENT", "'to' must be after 'at'");
    if (start == c.start && stop == c.end) return;
    ed.setSubtitleTiming(id, start, stop); // stops at the neighbours
    // Cues never overlap: say where it stopped (a merge has to delete the next cue before it extends this one)
    const SubtitleCue* now = Subtitles::find(s.project.timeline(), id);
    if (now && (now->start != start || now->end != stop))
        s.notes << QString("subtitle %1 is %2-%3 instead of %4-%5: cues on a track do not overlap, it stops at its "
                           "neighbour (delete or move that one first)")
                       .arg(id)
                       .arg(now->start)
                       .arg(now->end)
                       .arg(start)
                       .arg(stop);
}

void opSubtitles(Session& s, const QJsonObject& op)
{
    // A whole new subtitle track: from an SRT file or a list of cues
    QVector<SubtitleCue> cues;
    QString trackName = op.value("name").toString();
    if (op.contains("srt")) {
        const QString path = absolute(op.value("srt").toString());
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) fail("NOT_FOUND", "cannot read " + path);
        cues = Subtitles::parseSrt(f.readAll(), s.project.frameRate());
        if (trackName.isEmpty()) trackName = QFileInfo(path).completeBaseName();
    } else {
        for (const QJsonValue& v : op.value("cues").toArray()) {
            const QJsonObject o = v.toObject();
            const int from = timeOf(s, o, "from"), to = timeOf(s, o, "to");
            if (to <= from) fail("BAD_ARGUMENT", "cue 'to' must be after 'from'");
            cues << SubtitleCue{0, from, to, o.value("text").toString()};
        }
        std::sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    }
    if (cues.isEmpty()) fail("BAD_ARGUMENT", "no cues (give 'srt' (file) or 'cues' ([{from, to, text}, …]))");
    s.editor.importSubtitles(cues, trackName);
}

// Effect order ({op:'effect', clips, effect, move}): 'up'/'down' one place, 'first'/'last', or a place (1 = rendered
// first) among the movable effects of each clip; the fixed stages keep theirs
void opMoveEffect(Session& s, const QJsonObject& op, const QString& effectId, const EffectDescriptor* d)
{
    if (effectId.isEmpty()) fail("BAD_ARGUMENT", "missing 'effect' (id of the effect to move, as `info` lists it)");
    if (d && d->fixed)
        fail("BAD_ARGUMENT", QString("'%1' is a fixed stage and cannot be moved: stabilize, grade (Color page) and "
                                     "chromakey (Green Screen) always render first, in that order; denoise belongs "
                                     "to the audio clip. Only the other effects follow the clip's order")
                                 .arg(effectId));
    for (const char* other : {"params", "enabled", "remove", "preset"})
        if (op.contains(other))
            fail("BAD_ARGUMENT", QString("'move' only changes the order; put '%1' into its own effect op").arg(other));
    const QJsonValue v = op.value("move");
    const QString where = v.toString();
    const bool place = v.isDouble();
    if (place ? v.toDouble() != std::floor(v.toDouble()) || v.toDouble() < 1 || v.toDouble() > 1000
              : !QStringList{"up", "down", "first", "last"}.contains(where))
        fail("BAD_ARGUMENT", "'move' is 'up' (one place earlier), 'down' (one later), 'first', 'last' or a place "
                             "among the clip's movable effects (1 = rendered first)");
    const QVector<int> targets = d ? clipsOn(s, op, d->video ? TrackKind::Video : TrackKind::Audio,
                                             d->video ? "video effects" : "audio effects")
                                   : clipIds(s, op); // plugin missing here: any clip that has it
    const Timeline& tl = s.project.timeline();
    QVector<int> having;
    for (int id : targets)
        if (const Clip* c = TimelineOps::findClip(tl, id); c && EffectRegistry::has(*c, effectId)) having << id;
    if (having.isEmpty())
        fail("NOT_FOUND", QString("none of the clips has the effect '%1' (`info` lists the effects of each clip)")
                              .arg(effectId));
    bool moved = false;
    if (place) moved = s.editor.moveEffect(having, effectId, v.toInt() - 1);
    else if (where == "up" || where == "down") moved = s.editor.moveEffectBy(having, effectId, where == "up" ? -1 : 1);
    else moved = s.editor.moveEffect(having, effectId, where == "first" ? 0 : std::numeric_limits<int>::max());
    if (!moved) {
        s.notes << QString("'%1' is already at that place (or its track is locked): nothing changed").arg(effectId);
        return;
    }
    // The new order, so the next step needs no `info`
    QStringList orders;
    for (int id : having.mid(0, 8))
        if (const Clip* c = TimelineOps::findClip(s.project.timeline(), id))
            orders << QString("clip %1: %2").arg(id).arg(EffectRegistry::order(*c).join(", "));
    s.notes << "effect order now (rendered first to last, after the fixed stages): " + orders.join("; ")
                   + (having.size() > 8 ? QString("; … %1 more clips").arg(having.size() - 8) : QString());
}

void opEffect(Session& s, const QJsonObject& op)
{
    const QString effectId = op.value("effect").toString();
    const EffectDescriptor* d = EffectRegistry::find(effectId);
    if (op.contains("move")) {
        opMoveEffect(s, op, effectId, d);
        return;
    }
    if (op.contains("preset")) {
        // Shotcut/Kdenlive preset from the effects folder: adds its effects with their values
        const QString path = findAsset(Asset::Preset, op.value("preset").toString());
        s.editor.addEffect(clipsOn(s, op, TrackKind::Video, "presets"), Presets::Prefix + path);
        return;
    }
    if (op.value("remove").toBool()) {
        s.editor.removeEffect(d ? clipsOn(s, op, d->video ? TrackKind::Video : TrackKind::Audio, "effects") : clipIds(s, op),
                              effectId);
        return;
    }
    if (!d) fail("NOT_FOUND", QString("no effect '%1' (see the `effects` command)").arg(effectId));
    // video effects on video clips, audio effects (kind "audio" in `effects`) on audio clips
    const QVector<int> targets = clipsOn(s, op, d->video ? TrackKind::Video : TrackKind::Audio,
                                          d->video ? "video effects" : "audio effects");
    QVariantMap values;
    const QJsonObject params = op.value("params").toObject();
    for (auto it = params.begin(); it != params.end(); ++it) values[it.key()] = effectValue(*d, it.key(), it.value());
    s.editor.addEffect(targets, effectId); // clips that have it already keep it
    if (values.isEmpty() && !op.contains("enabled")) return;
    s.editor.modifyClips(targets, d->name, [&](Clip& c) {
        EffectInstance* e = EffectRegistry::instance(c, effectId);
        if (!e) return;
        for (auto it = values.begin(); it != values.end(); ++it) e->params[it.key()] = it.value();
        if (op.contains("enabled")) e->enabled = op.value("enabled").toBool();
    });
}

void opAddTrack(Session& s, const QJsonObject& op)
{
    const QString kind = op.value("kind").toString("video");
    if (kind != "video" && kind != "audio") fail("BAD_ARGUMENT", "kind must be 'video' or 'audio'");
    const TrackKind k = kind == "audio" ? TrackKind::Audio : TrackKind::Video;
    const int n = s.project.timeline().tracks(k).size();
    s.editor.addTrack(k, op.contains("at") ? parseTrack(op.value("at")) : n); // default: on top / at the bottom
}

void opRemoveTrack(Session& s, const QJsonObject& op)
{
    if (const QString st = op.value("track").toString().trimmed().toUpper(); st.startsWith("ST")) {
        const int index = st.mid(2).toInt() - 1;
        if (index < 0 || index >= s.project.timeline().subtitles.size()) fail("NOT_FOUND", "no subtitle track " + st);
        s.editor.removeSubtitleTrack(index);
        return;
    }
    TrackKind k = TrackKind::Video;
    const int index = parseTrack(op.value("track"), &k);
    if (!s.editor.canRemoveTrack({k, index}))
        fail("FAILED", "cannot remove that track (missing, locked or the last one of its kind)");
    s.editor.removeTrack({k, index});
}

// Details of the ops that place source ranges (import presets, core/StreamPresets.h)
const char* const kPresetsDetails =
    "track: V2 puts the picture on V2 and the sound from A2 on. Files with several audio streams (OBS) get one "
    "track per stream, named after it, and follow the user's import presets from the app (by default every stream "
    "plays; users can e.g. mute the Mix stream when its single streams are there, or set rules per stream title; "
    "presets:false places every stream as it is). A recording with a Mix stream plus its single streams then plays "
    "everything twice: turn off the Mix or the single streams with 'disable' (check with `loudness`). A stream never "
    "lands on a track named after another stream of the file: then the streams take free tracks with their names, "
    "or new tracks below. Sound that already plays there (a second copy of the same moment, e.g. picture-in-picture; "
    "a blurred background needs no copy: op 'transform' fit:'blur') comes in disabled: 'enable' turns it on";

QVector<OpDef> makeOps()
{
    QVector<OpDef> ops{
        {"append", "{op:'append', media, in?, out?, track?, presets?}", "put a source range at the end of the timeline",
         kPresetsDetails,
         [](Session& s, const QJsonObject& op) { opPlace(s, op, true); }},
        {"place", "{op:'place', media, at, in?, out?, track?, presets?}", "overwrite at a position", kPresetsDetails,
         [](Session& s, const QJsonObject& op) { opPlace(s, op, false); }},
        {"keep", "{op:'keep', media, ranges:[[from,to],...], at?, track?, presets?}",
         "put several source ranges one after another (e.g. the 'sound' ranges of `silence`)", kPresetsDetails, opKeep},
        {"split", "{op:'split', at, clip?}", "cut all clips under `at` (or one clip)", {},
         [](Session& s, const QJsonObject& op) {
             const int at = timeOf(s, op, "at");
             if (op.contains("clip")) s.editor.bladeAt(clipOf(s, op).id, at);
             else s.editor.splitAtPlayhead(at); // no selection: every clip under `at`
         }},
        {"delete", "{op:'delete', clips:[ids], ripple?:false}", "clips or subtitles", {}, opDelete},
        {"delete_range", "{op:'delete_range', from, to, ripple?:true} or {op:'delete_range', ranges:[[from,to],...]}",
         "remove time ranges on all tracks; `ranges` are all timeline times before the cut (e.g. words of "
         "`transcribe` with words:true), applied together", {},
         [](Session& s, const QJsonObject& op) {
             QVector<QPair<int, int>> ranges;
             if (op.contains("ranges")) {
                 for (const QJsonValue& v : op.value("ranges").toArray()) {
                     const QJsonArray r = v.toArray();
                     if (r.size() != 2) fail("BAD_ARGUMENT", "ranges are [from, to] pairs");
                     QJsonObject pair{{"from", r[0]}, {"to", r[1]}};
                     ranges << qMakePair(timeOf(s, pair, "from"), timeOf(s, pair, "to"));
                 }
                 if (ranges.isEmpty()) fail("BAD_ARGUMENT", "'ranges' is empty");
             } else {
                 ranges << qMakePair(timeOf(s, op, "from"), timeOf(s, op, "to"));
             }
             for (const auto& r : ranges)
                 if (r.second <= r.first) fail("BAD_ARGUMENT", "'to' must be after 'from'");
             const bool ripple = op.value("ripple").toBool(true);
             s.editor.deleteRanges(ranges, ripple, ripple ? "Delete ranges with ripple" : "Delete ranges");
         }},
        {"move", "{op:'move', clips, by|to, track_delta?}", {}, {}, opMove},
        {"fade", "{op:'fade', clips, in?, out?, linked?}", "fade handles (video: to the track below/black, audio: volume); "
         "a video clip's sound fades along unless linked:false", {}, opFade},
        {"enable", "{op:'enable'|'disable', clips}", {}, {},
         [](Session& s, const QJsonObject& op) {
             s.editor.modifyClips(clipIds(s, op), "Enable", [](Clip& c) { c.enabled = true; });
         }},
        {"disable", {}, {}, {},
         [](Session& s, const QJsonObject& op) {
             s.editor.modifyClips(clipIds(s, op), "Disable", [](Clip& c) { c.enabled = false; });
         }},
        {"clear", "{op:'clear'}", "empty the timeline", {},
         [](Session& s, const QJsonObject&) { s.editor.deleteRange(0, std::max(1, endOf(s)), false); }},
        {"subtitle", "{op:'subtitle', text, at, to?|duration?, track?:'ST1'}",
         "add a subtitle (default 3s, stops before the next one), or {op:'subtitle', id, text?, at?, to?} change one",
         {}, opSubtitle},
        {"subtitles", "{op:'subtitles', srt?:file, cues?:[{from, to, text}], name?}",
         "new subtitle track from an SRT file or a cue list", {}, opSubtitles},
        {"effect", "{op:'effect', clips, effect, params?:{key: value}, enabled?, remove?} or {op:'effect', clips, preset} "
                   "or {op:'effect', clips, effect, move:'up'|'down'|'first'|'last'|place}",
         "add/set/remove an effect (ids and parameters: `effects`); preset = effect preset from `effects`; move = "
         "change the order the clip's effects render in",
         "Effects render in the order `info` lists them (a new one comes last), which matters: blur then pixelate "
         "gives soft blocks, pixelate then blur a smooth picture. move:'up' renders the effect one place "
         "earlier, 'down' one later, 'first'/'last', or a number = its place among the clip's movable effects (1 = "
         "rendered first). It works per clip on every given clip that has the effect. Fixed stages cannot move and "
         "always render before the others: stabilize, then grade (Color page), then chromakey (Green Screen, so "
         "effects work on the keyed picture); denoise is per audio clip. "
         "chromakey (Green Screen): color = the background's color, distance = tolerance 0..1 (how far from it still "
         "becomes transparent; gray is at ~0.6, skin ~0.75), softness = soft edge, spill = removes the key color's "
         "fringe near the edge (1 = fully). No effect blurs faces by itself: a `window` with fill 'blur', "
         "`track_window` makes it follow the face. "
         "Video effects go on video clips, audio effects (kind 'audio' in `effects`, e.g. denoise) on audio clips. "
         "stabilize needs an analysis pass of the clip: the app runs it in the background (the clip plays "
         "unstabilized until then), `render` runs missing analyses before it encodes. Changing shakiness/accuracy "
         "means a new analysis; smoothing, zoom and border do not. Not possible for speed ramps, freeze frames, "
         "titles and compound clips. denoise (noise reduction for voice recordings, ~11 ms delay): reduction = how far "
         "the noise is lowered in dB (default 12), track = measure the noise in the recording (default true), floor "
         "= fixed noise level in dBFS used when track is false.",
         opEffect},
        {"add_track", "{op:'add_track', kind:'video'|'audio', at?}", "new track (default: above/below the others)",
         {}, opAddTrack},
        {"remove_track", "{op:'remove_track', track}", "remove a track with its clips (not the last one; ST1 … = subtitle tracks)", {},
         opRemoveTrack},
    };
    addTimelineOps(ops);
    addLookOps(ops);
    addAudioOps(ops);
    addEditOps(ops);
    addMulticamOps(ops);
    return ops;
}

} // namespace

const QVector<OpDef>& editOps()
{
    static const QVector<OpDef> ops = makeOps();
    return ops;
}

void applyOp(Session& s, const QJsonObject& given)
{
    QJsonObject op = given;
    const QString name = op.value("op").toString();
    // The file under the name other commands give it ("file", "path"; no op has such a key): taken as 'media'
    if (!op.contains("media"))
        for (const char* alias : {"file", "path"})
            if (op.value(alias).isString()) {
                op["media"] = op.value(alias);
                s.notes << QString("%1: '%2' read as 'media' (the name of the file key in edit ops)").arg(name, alias);
                break;
            }
    s.selection.clear();
    for (const OpDef& d : editOps())
        if (d.name == name) return d.run(s, op);
    fail("BAD_ARGUMENT", QString("unknown op '%1' (see `help edit`)").arg(name));
}

QString editHelp()
{
    QStringList lines;
    for (const OpDef& d : editOps()) {
        if (d.usage.isEmpty()) continue;
        lines << (d.summary.isEmpty() ? d.usage : d.usage + " " + d.summary);
    }
    return QStringLiteral(
               "List of operations, applied in order, saved once (nothing is saved if one fails). Times: frames "
               "(integer), \"4.5s\", timecode \"HH:MM:SS:FF\" (frames below the frame rate) or clock time \"MM:SS\" "
               "/ \"HH:MM:SS.sss\"; ranges are half-open [from, to). A timeline position (at, to, "
               "from, cue times …) can also be relative, resolved when the op runs so earlier ops that move clips do "
               "not shift it: {clip:id, offset?} (from the clip start), {clip:id, from_end}, {clip:id, source} (where "
               "the clip plays that source time; speed and ramps included) or {media:file, source} (first clip that "
               "plays it); source times in seconds (\"12.4s\") are safest. Tracks: \"V1\", \"A2\" (a "
               "media clip goes to V<n> and A<n>). Clip ids come from `info` and change when clips are split. "
               "Linked audio/video partners are included unless linked:false; ops for one kind of clip (effect, color, "
               "transform, volume, keyframe …) take only the given clips of that kind, a clip of the other kind "
               "stands for its linked partners (linked:true = all partners, false = only the given ones). Subtitle ids work with 'delete' too. "
               "`help` with command 'edit' and op '<name>' has the details of an op. Operations: ")
        + lines.join("; ") + ".";
}

QJsonArray editOpsHelp()
{
    QJsonArray list;
    for (const OpDef& d : editOps()) {
        if (d.usage.isEmpty()) continue;
        QJsonObject o{{"op", d.name}, {"usage", d.usage}};
        if (!d.summary.isEmpty()) o["summary"] = d.summary;
        if (!d.details.isEmpty()) o["details"] = d.details;
        list << o;
    }
    return list;
}

} // namespace Cli::detail
