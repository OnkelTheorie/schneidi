// schneidi-cli: the operations of the `edit` command (one JSON object each, applied through the Editor like in the app)
#include "cli/CommandsDetail.h"

#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>

namespace Cli::detail {

const Clip& clipOf(Session& s, const QJsonObject& op, const char* key)
{
    if (!op.contains(key)) fail("BAD_ARGUMENT", QString("missing '%1' (clip id)").arg(key));
    const int id = op[key].toInt();
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

int timeOf(Session& s, const QJsonObject& op, const char* key, int fallback)
{
    if (!op.contains(key)) {
        if (fallback >= 0) return fallback;
        fail("BAD_ARGUMENT", QString("missing '%1'").arg(key));
    }
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


namespace {

int endOf(Session& s) { return TimelineOps::endFrame(s.project.timeline()); }

void opPlace(Session& s, const QJsonObject& op, bool append)
{
    const MediaInfo m = s.media(op.value("media").toString());
    int in = 0, out = 0;
    sourceRange(s, op, m, &in, &out);
    const int at = append ? timeOf(s, op, "at", endOf(s)) : timeOf(s, op, "at");
    s.editor.placeSourceRange(m.path, in, out - 1, at, parseTrack(op.value("track")));
}

void opKeep(Session& s, const QJsonObject& op)
{
    const MediaInfo m = s.media(op.value("media").toString());
    int at = timeOf(s, op, "at", endOf(s));
    const int track = parseTrack(op.value("track"));
    const QJsonArray ranges = op.value("ranges").toArray();
    if (ranges.isEmpty()) fail("BAD_ARGUMENT", "missing 'ranges' ([[from, to], …] in the source)");
    for (const QJsonValue& r : ranges) {
        QJsonObject sub;
        if (r.isArray()) sub = {{"in", r.toArray().at(0)}, {"out", r.toArray().at(1)}};
        else sub = {{"in", r.toObject().value("from")}, {"out", r.toObject().value("to")}};
        int in = 0, out = 0;
        sourceRange(s, sub, m, &in, &out);
        s.editor.placeSourceRange(m.path, in, out - 1, at, track);
        at += out - in;
    }
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

void opFade(Session& s, const QJsonObject& op)
{
    const int id = clipOf(s, op).id;
    if (op.contains("in")) s.editor.setClipFade(id, TimelineOps::Edge::Start, timeOf(s, op, "in"));
    if (op.contains("out")) s.editor.setClipFade(id, TimelineOps::Edge::End, timeOf(s, op, "out"));
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
    if (start != c.start || stop != c.end) ed.setSubtitleTiming(id, start, stop); // stops at the neighbours
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

void opEffect(Session& s, const QJsonObject& op)
{
    const QString effectId = op.value("effect").toString();
    const QVector<int> ids = clipIds(s, op);
    if (op.contains("preset")) {
        // Shotcut/Kdenlive preset from the effects folder: adds its effects with their values
        const QString path = findAsset(Asset::Preset, op.value("preset").toString());
        s.editor.addEffect(onTracks(s, ids, TrackKind::Video, "presets"), Presets::Prefix + path);
        return;
    }
    if (op.value("remove").toBool()) {
        s.editor.removeEffect(ids, effectId);
        return;
    }
    const EffectDescriptor* d = EffectRegistry::find(effectId);
    if (!d || !d->video) fail("NOT_FOUND", QString("no video effect '%1' (see the `effects` command)").arg(effectId));
    const QVector<int> video = onTracks(s, ids, TrackKind::Video, "effects");
    QVariantMap values;
    const QJsonObject params = op.value("params").toObject();
    for (auto it = params.begin(); it != params.end(); ++it) values[it.key()] = effectValue(*d, it.key(), it.value());
    s.editor.addEffect(video, effectId); // clips that have it already keep it
    if (values.isEmpty() && !op.contains("enabled")) return;
    s.editor.modifyClips(video, d->name, [&](Clip& c) {
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

QVector<OpDef> makeOps()
{
    QVector<OpDef> ops{
        {"append", "{op:'append', media, in?, out?, track?}", "put a source range at the end of the timeline", {},
         [](Session& s, const QJsonObject& op) { opPlace(s, op, true); }},
        {"place", "{op:'place', media, at, in?, out?, track?}", "overwrite at a position", {},
         [](Session& s, const QJsonObject& op) { opPlace(s, op, false); }},
        {"keep", "{op:'keep', media, ranges:[[from,to],...], at?, track?}",
         "put several source ranges one after another (e.g. the 'sound' ranges of `silence`)", {}, opKeep},
        {"split", "{op:'split', at, clip?}", "cut all clips under `at` (or one clip)", {},
         [](Session& s, const QJsonObject& op) {
             const int at = timeOf(s, op, "at");
             if (op.contains("clip")) s.editor.bladeAt(clipOf(s, op).id, at);
             else s.editor.splitAtPlayhead(at); // no selection: every clip under `at`
         }},
        {"delete", "{op:'delete', clips:[ids], ripple?:false}", "clips or subtitles", {}, opDelete},
        {"delete_range", "{op:'delete_range', from, to, ripple?:true}", "remove a time range on all tracks", {},
         [](Session& s, const QJsonObject& op) {
             const int from = timeOf(s, op, "from"), to = timeOf(s, op, "to");
             if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
             s.editor.deleteRange(from, to, op.value("ripple").toBool(true));
         }},
        {"move", "{op:'move', clips, by|to, track_delta?}", {}, {}, opMove},
        {"fade", "{op:'fade', clip, in?, out?}", "fade lengths", {}, opFade},
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
        {"effect", "{op:'effect', clips, effect, params?:{key: value}, enabled?, remove?} or {op:'effect', clips, preset}",
         "add/set/remove a video effect (ids and parameters: `effects`); preset = effect preset from `effects`", {},
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
    return ops;
}

} // namespace

const QVector<OpDef>& editOps()
{
    static const QVector<OpDef> ops = makeOps();
    return ops;
}

void applyOp(Session& s, const QJsonObject& op)
{
    const QString name = op.value("op").toString();
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
               "(integer), \"4.5s\" or \"HH:MM:SS:FF\"; ranges are half-open [from, to). Tracks: \"V1\", \"A2\" (a "
               "media clip goes to V<n> and A<n>). Clip ids come from `info` and change when clips are split. "
               "Linked audio/video partners are included unless linked:false. Subtitle ids work with 'delete' too. "
               "`help edit` has details. Operations: ")
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
