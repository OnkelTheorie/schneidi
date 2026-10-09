// schneidi-cli: the operations of the `edit` command (one JSON object each, applied through the Editor like in the app)
#include "cli/CommandsDetail.h"

#include "core/EffectRegistry.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>

namespace Cli::detail {

namespace {

const Clip& clipOf(Session& s, const QJsonObject& op, const char* key = "clip")
{
    if (!op.contains(key)) fail("BAD_ARGUMENT", QString("missing '%1' (clip id)").arg(key));
    const int id = op[key].toInt();
    const Clip* c = TimelineOps::findClip(s.project.timeline(), id);
    if (!c) fail("NOT_FOUND", QString("no clip with id %1").arg(id));
    return *c;
}

// Clip ids of `clips` (or `clip`); cues = subtitle ids allowed too (delete, move)
QVector<int> clipIds(Session& s, const QJsonObject& op, bool cues = false)
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
        for (const EffectParam& p : d.params) keys << p.key;
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
        const QVector<int> ids = clipIds(s, op, true);
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
    } else if (name == "subtitle") {
        // New cue at `at`, or (with id) change text/timing of an existing one
        int id = op.value("id").toInt();
        if (id) {
            const SubtitleCue* c = Subtitles::find(s.project.timeline(), id);
            if (!c) fail("NOT_FOUND", QString("no subtitle with id %1").arg(id));
            if (op.contains("text")) ed.setSubtitleText(id, op.value("text").toString());
        } else {
            if (!op.contains("text")) fail("BAD_ARGUMENT", "missing 'text'");
            id = ed.addSubtitle(timeOf(s, op, "at"), parseSubtitleTrack(op.value("track")), op.value("text").toString());
            if (!id) fail("FAILED", "a subtitle already starts there or the subtitle track is locked");
        }
        const SubtitleCue c = *Subtitles::find(s.project.timeline(), id);
        int start = op.contains("at") ? timeOf(s, op, "at") : c.start;
        int stop = op.contains("to") ? timeOf(s, op, "to") : start + c.length();
        if (op.contains("duration")) stop = start + timeOf(s, op, "duration");
        if (stop <= start) fail("BAD_ARGUMENT", "'to' must be after 'at'");
        if (start != c.start || stop != c.end) ed.setSubtitleTiming(id, start, stop); // stops at the neighbours
    } else if (name == "subtitles") {
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
        ed.importSubtitles(cues, trackName);
    } else if (name == "effect") {
        const QString effectId = op.value("effect").toString();
        const QVector<int> ids = clipIds(s, op);
        if (op.value("remove").toBool()) {
            ed.removeEffect(ids, effectId);
            return;
        }
        const EffectDescriptor* d = EffectRegistry::find(effectId);
        if (!d || !d->video) fail("NOT_FOUND", QString("no video effect '%1' (see the `effects` command)").arg(effectId));
        QVector<int> video;
        for (int id : ids)
            if (TrackRef r; TimelineOps::findClip(s.project.timeline(), id, &r) && r.kind == TrackKind::Video) video << id;
        if (video.isEmpty()) fail("BAD_ARGUMENT", "effects go on video clips; none of the clips is on a video track");
        QVariantMap values;
        const QJsonObject params = op.value("params").toObject();
        for (auto it = params.begin(); it != params.end(); ++it) values[it.key()] = effectValue(*d, it.key(), it.value());
        ed.addEffect(video, effectId); // clips that have it already keep it
        if (values.isEmpty() && !op.contains("enabled")) return;
        ed.modifyClips(video, d->name, [&](Clip& c) {
            EffectInstance* e = EffectRegistry::instance(c, effectId);
            if (!e) return;
            for (auto it = values.begin(); it != values.end(); ++it) e->params[it.key()] = it.value();
            if (op.contains("enabled")) e->enabled = op.value("enabled").toBool();
        });
    } else if (name == "add_track") {
        const QString kind = op.value("kind").toString("video");
        if (kind != "video" && kind != "audio") fail("BAD_ARGUMENT", "kind must be 'video' or 'audio'");
        const TrackKind k = kind == "audio" ? TrackKind::Audio : TrackKind::Video;
        const int n = s.project.timeline().tracks(k).size();
        ed.addTrack(k, op.contains("at") ? parseTrack(op.value("at")) : n); // default: on top / at the bottom
    } else if (name == "remove_track") {
        TrackKind k = TrackKind::Video;
        const int index = parseTrack(op.value("track"), &k);
        if (!ed.canRemoveTrack({k, index}))
            fail("FAILED", "cannot remove that track (missing, locked or the last one of its kind)");
        ed.removeTrack({k, index});
    } else if (name == "clear") {
        ed.deleteRange(0, std::max(1, end()), false);
    } else {
        fail("BAD_ARGUMENT", QString("unknown op '%1'").arg(name));
    }
}

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
    "{op:'marker', at}; {op:'enable'|'disable', clips}; {op:'clear'} empty the timeline; "
    "{op:'subtitle', text, at, to?|duration?, track?:'ST1'} add a subtitle (default 3s, stops before the next one), "
    "or {op:'subtitle', id, text?, at?, to?} change one; "
    "{op:'subtitles', srt?:file, cues?:[{from, to, text}], name?} new subtitle track from an SRT file or a cue list; "
    "{op:'effect', clips, effect, params?:{key: value}, enabled?, remove?} add/set/remove a video effect (ids and "
    "parameters: `effects`); "
    "{op:'add_track', kind:'video'|'audio', at?} new track (default: above/below the others); "
    "{op:'remove_track', track} remove an empty or full track (not the last one). "
    "Subtitle ids work with 'delete' too. "
    "Linked audio/video partners are included unless linked:false.");

} // namespace Cli::detail
