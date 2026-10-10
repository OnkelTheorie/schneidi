// schneidi-cli edit operations of the Edit page beyond the basics: titles with their full style, trim modes
// (ripple/roll/slip/slide), transitions with type and length, tracks and subtitle tracks, link/unlink, speed and
// speed ramps, markers, Media Pool organisation, copy and insert edits
#include "cli/CommandsDetail.h"

#include "core/EffectFolders.h"
#include "core/Retime.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QFileInfo>

namespace Cli::detail {

namespace {

// ---------- styles (titles, subtitle tracks) ----------

const QString kStyleFields = "font, size, color, bold, italic, align (left|center|right), x, y, outline, outline_color, "
                             "outline_width, box, box_color, box_padding";

bool applyStyle(TitleStyle& t, const QJsonObject& op)
{
    bool any = false;
    const auto num = [&](const char* k, double* v, double lo, double hi) {
        if (!op.contains(k)) return;
        *v = numberOf(op, k, lo, hi);
        any = true;
    };
    const auto color = [&](const char* k, QColor* v) {
        if (!op.contains(k)) return;
        *v = colorOf(op.value(k), k);
        any = true;
    };
    const auto flag = [&](const char* k, bool* v) {
        if (!op.contains(k)) return;
        *v = op.value(k).toBool();
        any = true;
    };
    if (op.contains("font")) {
        t.font = op.value("font").toString();
        any = true;
    }
    num("size", &t.size, 1, 1000);
    color("color", &t.color);
    flag("bold", &t.bold);
    flag("italic", &t.italic);
    if (op.contains("align")) {
        const int a = QStringList{"left", "center", "right"}.indexOf(op.value("align").toString());
        if (a < 0) fail("BAD_ARGUMENT", "align must be left, center or right");
        t.align = a;
        any = true;
    }
    num("x", &t.posX, -10000, 10000);
    num("y", &t.posY, -10000, 10000);
    flag("outline", &t.outlineOn);
    color("outline_color", &t.outlineColor);
    num("outline_width", &t.outlineWidth, 0, 200);
    flag("box", &t.boxOn);
    color("box_color", &t.boxColor);
    num("box_padding", &t.boxPad, 0, 500);
    return any;
}

void opTitle(Session& s, const QJsonObject& op)
{
    int id = 0;
    if (op.contains("clip")) {
        const Clip& c = clipOf(s, op);
        if (!c.isTitle()) fail("BAD_ARGUMENT", QString("clip %1 is not a title").arg(c.id));
        id = c.id;
    } else {
        s.editor.addTitle(timeOf(s, op, "at", 0), op.contains("track") ? parseTrack(op.value("track")) : -1);
        if (s.selection.ids().isEmpty()) fail("FAILED", "could not place the title (locked track?)");
        id = *s.selection.ids().begin();
    }
    const bool created = !op.contains("clip");
    const int length = op.contains("duration") ? timeOf(s, op, "duration") : (created ? 5 * s.project.fps() : 0);
    TitleStyle style = TimelineOps::findClip(s.project.timeline(), id)->title;
    if (created) style.text = "Title";
    if (op.contains("text")) style.text = op.value("text").toString();
    applyStyle(style, op);
    s.editor.modifyClips({id}, "Title", [&](Clip& c) {
        c.title = style;
        if (length > 0) c.out = c.in + length - 1; // longer titles overwrite nothing: see TimelineOps::modify
    });
}

// ---------- trim ----------

void opTrim(Session& s, const QJsonObject& op)
{
    const Clip& c = clipOf(s, op);
    const QString mode = op.value("mode").toString("normal");
    const QString edgeName = op.value("edge").toString("end");
    if (edgeName != "start" && edgeName != "end") fail("BAD_ARGUMENT", "edge must be 'start' or 'end'");
    const TimelineOps::Edge edge = edgeName == "start" ? TimelineOps::Edge::Start : TimelineOps::Edge::End;
    const QJsonValue by = op.value("by");
    int delta = 0;
    if (by.isDouble()) delta = by.toInt();
    else if (by.isString() && by.toString().startsWith('-')) delta = -parseTime(by.toString().mid(1), s.format(), "by");
    else if (by.isString()) delta = parseTime(by, s.format(), "by");
    if (mode == "normal") {
        if (op.contains("to")) delta = timeOf(s, op, "to") - (edge == TimelineOps::Edge::Start ? c.start : c.end());
        s.editor.trimClip(c.id, edge, delta);
        return;
    }
    TimelineOps::TrimKind kind;
    if (mode == "ripple") kind = TimelineOps::TrimKind::Ripple;
    else if (mode == "roll") kind = TimelineOps::TrimKind::Roll;
    else if (mode == "slip") kind = TimelineOps::TrimKind::Slip;
    else if (mode == "slide") kind = TimelineOps::TrimKind::Slide;
    else fail("BAD_ARGUMENT", "mode must be normal, ripple, roll, slip or slide");
    if (op.contains("to")) {
        if (kind == TimelineOps::TrimKind::Slip) fail("BAD_ARGUMENT", "slip moves the content: use 'by' (frames)");
        const int from = kind == TimelineOps::TrimKind::Slide ? c.start : (edge == TimelineOps::Edge::Start ? c.start : c.end());
        delta = timeOf(s, op, "to") - from;
    }
    const TimelineOps::TrimEdit e = s.editor.trimEdit(kind, c.id, edge);
    if (e.isNull()) fail("FAILED", QString("cannot %1 here (roll needs a neighbouring clip at the cut)").arg(mode));
    const int clamped = s.editor.clampTrimEdit(e, delta);
    s.editor.applyTrimEdit(e, clamped);
}

// ---------- transitions ----------

TransitionStyle parseTransitionStyle(const QJsonObject& op, TransitionStyle st)
{
    if (op.contains("type")) {
        const QString id = op.value("type").toString();
        const auto it = std::find_if(std::begin(kTransitionTypes), std::end(kTransitionTypes),
                                     [&](const TransitionTypeInfo& i) { return id == QLatin1String(i.id); });
        if (it == std::end(kTransitionTypes))
            fail("BAD_ARGUMENT", "type must be cross_dissolve, dip_color, wipe_right, wipe_left, wipe_down, wipe_up or luma");
        st.type = it->type;
    }
    if (op.contains("align")) {
        const int a = QStringList{"center", "start", "end"}.indexOf(op.value("align").toString());
        if (a < 0) fail("BAD_ARGUMENT", "align must be center, start or end (of the cut)");
        st.align = TransitionAlign(a);
    }
    if (op.contains("color")) st.color = colorOf(op.value("color"), "color");
    if (op.contains("softness")) st.softness = numberOf(op, "softness", 0, 100);
    if (op.contains("border")) st.border = numberOf(op, "border", 0, 1000);
    if (op.contains("border_color")) st.borderColor = colorOf(op.value("border_color"), "border_color");
    if (op.contains("invert")) st.invert = op.value("invert").toBool();
    if (op.contains("audio_curve")) {
        const QString id = op.value("audio_curve").toString();
        const auto it = std::find_if(std::begin(kAudioCurves), std::end(kAudioCurves),
                                     [&](const AudioCurveInfo& i) { return id == QLatin1String(i.id); });
        if (it == std::end(kAudioCurves)) fail("BAD_ARGUMENT", "audio_curve must be plus3, zero or minus3");
        st.audio = it->curve;
    }
    if (op.contains("luma")) {
        // Built-in name (`effects` lists them), a file in the effects folder or any image file
        const QString path = findAsset(Asset::Luma, op.value("luma").toString());
        st.luma = path;
        if (!op.contains("type")) st.type = TransitionType::Luma;
    }
    return st;
}

void opTransition(Session& s, const QJsonObject& op)
{
    Editor& ed = s.editor;
    const Timeline& cur = s.project.timeline();
    std::optional<TrackKind> only;
    if (op.contains("kind")) {
        const QString k = op.value("kind").toString();
        if (k != "video" && k != "audio") fail("BAD_ARGUMENT", "kind must be 'video' or 'audio'");
        only = k == "audio" ? TrackKind::Audio : TrackKind::Video;
    }
    // Which transitions: both edges of the clips, or the cut nearest to `at` (on every track)
    QSet<int> clips;
    int cut = -1;
    if (op.contains("clips") || op.contains("clip")) {
        const QVector<int> ids = clipIds(s, op);
        clips = QSet<int>(ids.begin(), ids.end());
    } else {
        const int at = timeOf(s, op, "at");
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : cur.tracks(k))
                for (const Clip& c : t.clips)
                    for (int f : {c.start, c.end()})
                        if (!t.locked && (!only || k == *only) && (cut < 0 || std::abs(f - at) < std::abs(cut - at))) cut = f;
        if (cut < 0) fail("FAILED", "no cut there (empty timeline?)");
    }
    const auto spans = [&] {
        QVector<TimelineOps::TransitionSpan> out;
        const Timeline& tl = s.project.timeline();
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
            if (only && k != *only) continue;
            for (int i = 0; i < tl.tracks(k).size(); ++i)
                for (const auto& sp : ed.transitions({k, i}))
                    if (clips.isEmpty() ? sp.cut == cut : (clips.contains(sp.leftId) || clips.contains(sp.rightId))) out << sp;
        }
        return out;
    };
    if (op.value("remove").toBool()) {
        for (const auto& sp : spans()) ed.removeTransition(sp.leftId, sp.rightId);
        return;
    }
    if (clips.isEmpty()) {
        ed.addTransitions(cut, std::nullopt, only);
    } else {
        s.selection.set(clips);
        ed.addTransitions(0, std::nullopt, only);
        s.selection.clear();
    }
    const auto found = spans();
    if (found.isEmpty()) fail("FAILED", "no transition possible there");
    const int length = op.contains("length") ? timeOf(s, op, "length") : 0;
    for (const auto& sp : found) {
        const TransitionStyle st = parseTransitionStyle(op, sp.style);
        if (st != sp.style) ed.setTransitionStyle(sp.leftId, sp.rightId, st);
        if (length > 0) ed.setTransitionLength(sp.leftId, sp.rightId, length);
    }
}

// ---------- tracks ----------

void opTrack(Session& s, const QJsonObject& op)
{
    Editor& ed = s.editor;
    const QString name = op.value("track").toString().trimmed().toUpper();
    if (name.startsWith("ST")) {
        // Subtitle track: name, visible (one at a time), lock, style
        const int index = name.mid(2).toInt() - 1;
        if (index < 0 || index >= s.project.timeline().subtitles.size()) fail("NOT_FOUND", "no subtitle track " + name);
        const SubtitleTrack& t = s.project.timeline().subtitles[index];
        if (op.contains("name")) ed.renameSubtitleTrack(index, op.value("name").toString());
        if (op.contains("enabled")) ed.setSubtitleTrackEnabled(index, op.value("enabled").toBool());
        if (op.contains("lock") && op.value("lock").toBool() != t.locked) ed.toggleSubtitleTrackLock(index);
        TitleStyle style = s.project.timeline().subtitles[index].style;
        if (applyStyle(style, op)) ed.setSubtitleStyle(index, "Subtitle style", [&](TitleStyle& x) { x = style; });
        return;
    }
    TrackKind k = TrackKind::Video;
    const int index = parseTrack(op.value("track"), &k);
    if (index >= s.project.timeline().tracks(k).size()) fail("NOT_FOUND", "no track " + name);
    const TrackRef ref{k, index};
    const auto state = [&] { return s.project.timeline().track(ref); };
    if (op.contains("name")) ed.renameTrack(ref, op.value("name").toString());
    if (op.contains("color")) {
        const QString c = op.value("color").toString();
        if (!c.isEmpty() && !trackColorInfo(c)) {
            QStringList ids;
            for (const auto& i : kTrackColors) ids << i.id;
            fail("BAD_ARGUMENT", "color must be one of " + ids.join(", ") + " or \"\"");
        }
        ed.setTrackColor(ref, c);
    }
    if (op.contains("mute") && op.value("mute").toBool() != state().muted) ed.toggleTrackMute(ref);
    if (op.contains("hidden") && op.value("hidden").toBool() != state().hidden) ed.toggleTrackHidden(ref);
    if (op.contains("move_to")) {
        TrackKind k2 = k;
        const int to = parseTrack(op.value("move_to"), &k2);
        if (k2 != k || to >= s.project.timeline().tracks(k).size()) fail("BAD_ARGUMENT", "move_to must be an existing track of the same kind");
        ed.moveTrack(k, index, to);
    }
    if (op.contains("lock") && op.value("lock").toBool() != state().locked) ed.toggleTrackLock(ref); // last: blocks the rest
}

void opAddSubtitleTrack(Session& s, const QJsonObject& op)
{
    const int index = s.editor.addSubtitleTrack();
    if (op.contains("name")) s.editor.renameSubtitleTrack(index, op.value("name").toString());
}

// ---------- link ----------

void opLink(Session& s, const QJsonObject& op, bool link)
{
    const QVector<int> ids = s.editor.editable(clipIds(s, QJsonObject{{"clips", op.value("clips")}, {"linked", link ? false : true}}));
    if (link && ids.size() < 2) fail("BAD_ARGUMENT", "link needs at least two clips");
    const int id = link ? s.project.newLinkId() : 0;
    s.editor.modifyClips(ids, link ? "Link clips" : "Unlink clips", [id](Clip& c) { c.linkId = id; });
}

// ---------- speed ----------

void opSpeed(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipIds(s, op);
    const Clip& first = *TimelineOps::findClip(s.project.timeline(), ids.first());
    Editor::Retime r{first.speed, first.reverse, first.freeze, first.keepPitch};
    if (op.contains("speed")) r.speed = numberOf(op, "speed", Retime::kMinSpeed, Retime::kMaxSpeed);
    if (op.contains("reverse")) r.reverse = op.value("reverse").toBool();
    if (op.contains("freeze")) r.freeze = op.value("freeze").toBool();
    if (op.contains("keep_pitch")) r.keepPitch = op.value("keep_pitch").toBool();
    s.editor.setClipSpeed(ids, r, op.value("ripple").toBool(true));
}

void opSpeedRamp(Session& s, const QJsonObject& op)
{
    Editor& ed = s.editor;
    const int id = clipOf(s, op).id;
    if (!ed.canRetime(id)) fail("BAD_ARGUMENT", "this clip cannot get a speed ramp (title, still or freeze frame)");
    const auto clip = [&] { return *TimelineOps::findClip(s.project.timeline(), id); };
    // Start over: remove the old points (last first)
    for (int i = clip().ramp.size() - 1; i >= 0; --i) ed.removeSpeedPoint(id, i);
    if (op.value("clear").toBool()) return;
    struct Point {
        int at;
        double speed;
        int smooth;
    };
    QVector<Point> points;
    for (const QJsonValue& v : op.value("points").toArray()) {
        const QJsonObject p = v.toObject();
        points << Point{timeOf(s, p, "at"), numberOf(p, "speed", Retime::kMinSpeed, Retime::kMaxSpeed),
                        p.contains("smooth") ? timeOf(s, p, "smooth") : 0};
    }
    if (points.isEmpty()) fail("BAD_ARGUMENT", "missing 'points' ([{at, speed, smooth?}, …]) or clear:true");
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.at < b.at; });
    // Points first (they keep the timing), then the speeds from the last segment on
    const Clip c = clip();
    for (const Point& p : points) {
        if (p.at <= c.start || p.at >= c.end()) fail("BAD_ARGUMENT", QString("speed point %1 is not inside the clip (%2..%3)").arg(p.at).arg(c.start).arg(c.end()));
        ed.addSpeedPoint(id, p.at);
    }
    if (clip().ramp.size() != points.size()) fail("FAILED", "speed points must be at least one frame apart");
    if (op.contains("speed")) ed.setSegmentSpeed(id, 0, numberOf(op, "speed", Retime::kMinSpeed, Retime::kMaxSpeed));
    for (int i = points.size() - 1; i >= 0; --i) {
        ed.setSegmentSpeed(id, i + 1, points[i].speed);
        if (points[i].smooth > 0) ed.setSpeedPointSmooth(id, i, points[i].smooth);
    }
}

// ---------- markers ----------

void opMarker(Session& s, const QJsonObject& op)
{
    const int at = timeOf(s, op, "at");
    const bool has = s.project.timeline().markers.contains(at);
    if (op.value("remove").toBool() ? has : !has) s.editor.toggleMarker(at);
}

void opMarks(Session& s, const QJsonObject& op)
{
    if (op.value("clear").toBool()) return s.editor.clearMarks();
    if (op.contains("in")) s.editor.setMarkIn(timeOf(s, op, "in"));
    if (op.contains("out")) s.editor.setMarkOut(timeOf(s, op, "out") - 1);
}

// ---------- Media Pool ----------

int binOf(Session& s, const QString& path)
{
    // "Name" or "Parent/Child"; created if missing
    int bin = 0;
    for (const QString& part : path.split('/', Qt::SkipEmptyParts)) {
        int found = -1;
        for (int child : s.project.childBins(bin))
            if (s.project.binName(child).compare(part, Qt::CaseInsensitive) == 0) found = child;
        bin = found >= 0 ? found : s.project.addBin(bin, part.trimmed());
    }
    return bin;
}

void opMedia(Session& s, const QJsonObject& op)
{
    QStringList paths;
    const QJsonValue v = op.value("media");
    for (const QJsonValue& x : v.isArray() ? v.toArray() : QJsonArray{v}) paths << s.media(x.toString()).path;
    if (op.contains("bin")) s.project.moveMediaToBin(paths, binOf(s, op.value("bin").toString()));
    if (op.contains("color")) {
        const QString c = op.value("color").toString();
        if (!c.isEmpty() && !trackColorInfo(c)) fail("BAD_ARGUMENT", "color: same names as track colors (orange, blue, …) or \"\"");
        s.project.setClipColor(paths, c);
    }
    if (op.contains("flags")) {
        s.project.clearFlags(paths);
        for (const QJsonValue& f : op.value("flags").toArray()) {
            if (!flagColorInfo(f.toString())) fail("BAD_ARGUMENT", "flags: blue, cyan, green, yellow, red, pink, purple, … got " + f.toString());
            s.project.setFlag(paths, f.toString(), true);
        }
    }
    if (op.contains("in") || op.contains("out")) {
        for (const QString& p : paths) {
            if (op.contains("in")) s.editor.setSourceMarkIn(p, timeOf(s, op, "in"));
            if (op.contains("out")) s.editor.setSourceMarkOut(p, timeOf(s, op, "out") - 1);
        }
    }
}

// ---------- copy, insert ----------

void opCopy(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipIds(s, op);
    s.selection.set(QSet<int>(ids.begin(), ids.end()));
    s.editor.copySelection();
    s.selection.clear();
    s.editor.paste(timeOf(s, op, "at"));
}

// Source edits like F9–F12 in the app; `at` (and for fit_to_fill `to`) instead of playhead and timeline In/Out
void opSourceEdit(Session& s, const QJsonObject& op, Editor::SourceEditMode mode)
{
    const MediaInfo m = s.media(op.value("media").toString());
    // Source In/Out of the clip for this edit only (the app's marks stay as they were)
    const int in = timeOf(s, op, "in", 0);
    const int out = std::min(op.contains("out") ? timeOf(s, op, "out") : m.length, m.length);
    if (in >= out) fail("BAD_ARGUMENT", "empty source range");
    const int track = parseTrack(op.value("track"));
    const int at = timeOf(s, op, "at");
    const bool fit = mode == Editor::SourceEditMode::FitToFill;
    const int to = fit ? timeOf(s, op, "to") : 0;
    if (fit && to <= at) fail("BAD_ARGUMENT", "'to' must be after 'at'");
    const int markIn = s.project.timeline().markIn, markOut = s.project.timeline().markOut;
    if (markIn >= 0 || markOut >= 0) s.editor.clearMarks();
    if (fit) {
        s.editor.setMarkIn(at);
        s.editor.setMarkOut(to - 1);
    }
    s.project.setMediaMarks(m.path, in, out - 1);
    s.editor.setTargetTracks(track, track);
    const int end = s.editor.sourceEdit(mode, m.path, in, at);
    s.project.setMediaMarks(m.path, m.markIn, m.markOut);
    s.editor.clearMarks();
    if (markIn >= 0) s.editor.setMarkIn(markIn);
    if (markOut >= 0) s.editor.setMarkOut(markOut);
    if (end < 0) fail("FAILED", "could not edit there (replace/ripple_overwrite need a clip under `at` on the track)");
}

} // namespace

QJsonObject transitionStyleJson(const TransitionStyle& st)
{
    const TransitionStyle def;
    QJsonObject o;
    if (st.type != def.type) o["type"] = transitionTypeInfo(st.type).id;
    if (st.align != def.align) o["align"] = QStringList{"center", "start", "end"}.value(int(st.align));
    if (st.type == TransitionType::DipToColor) o["color"] = st.color.name();
    if (st.softness != def.softness) o["softness"] = st.softness;
    if (st.border != def.border) o["border"] = st.border;
    if (st.isLuma()) {
        o["luma"] = EffectFolders::isBuiltin(st.luma) ? EffectFolders::builtinId(st.luma) : st.luma;
        if (st.invert) o["invert"] = true;
    }
    if (st.audio != def.audio) o["audio_curve"] = audioCurveInfo(st.audio).id;
    return o;
}

void addEditOps(QVector<OpDef>& ops)
{
    ops << OpDef{"title", "{op:'title', text?, at?, duration?, track?, clip?, <style>}",
                 "new title (5s, lowest free video track) or with clip: change one",
                 "Style (pixels of the timeline format, from the center, y up): " + kStyleFields
                     + ". Colors \"#rrggbb\" or \"#aarrggbb\". text may have several lines (\\n).",
                 opTitle}
        << OpDef{"trim", "{op:'trim', clip, edge?:'start'|'end', by|to, mode?}",
                 "move a clip edge; mode normal (default, no ripple), ripple, roll (the cut), slip (content, by only), "
                 "slide (clip between its neighbours)",
                 {}, opTrim}
        << OpDef{"transition", "{op:'transition', at|clips, type?, length?, align?, kind?, remove?, …}",
                 "transition at the cut nearest to `at` (all tracks) or at both edges of the clips (default cross "
                 "dissolve, 1s)",
                 "type: cross_dissolve, dip_color (color), wipe_right, wipe_left, wipe_down, wipe_up (softness 0..100, "
                 "border px, border_color), luma (luma: built-in name from `effects` or an image file, softness, "
                 "invert). align: center, start, end (relative to the cut). audio_curve: plus3, zero, minus3. "
                 "kind: only 'video' or 'audio' tracks. Alone at a clip edge it fades from/to black (silence). "
                 "remove:true removes them; existing ones change their style/length.",
                 opTransition}
        << OpDef{"track", "{op:'track', track, name?, lock?, mute?, hidden?, color?, move_to?}",
                 "track header settings; subtitle tracks (ST1 …) take name, enabled, lock and the title style",
                 "Video/audio track colors: orange, apricot, yellow, lime, olive, green, teal, navy, blue, purple, "
                 "violet, pink, tan, beige, brown, chocolate. Subtitle style: " + kStyleFields
                     + " (y = distance from the bottom edge).",
                 opTrack}
        << OpDef{"add_subtitle_track", "{op:'add_subtitle_track', name?}", {}, {}, opAddSubtitleTrack}
        << OpDef{"link", "{op:'link'|'unlink', clips}", "link clips (move/cut together) or unlink them", {},
                 [](Session& s, const QJsonObject& op) { opLink(s, op, true); }}
        << OpDef{"unlink", {}, {}, {}, [](Session& s, const QJsonObject& op) { opLink(s, op, false); }}
        << OpDef{"speed", "{op:'speed', clips, speed?, reverse?, freeze?, keep_pitch?, ripple?:true}",
                 "clip speed (1 = normal, 0.01..100), reverse, freeze frame; ripple: length follows, later clips move", {}, opSpeed}
        << OpDef{"speed_ramp", "{op:'speed_ramp', clip, points:[{at, speed, smooth?}], speed?} or {…, clear:true}",
                 "speed ramp: from each point on its own speed (speed = before the first point)",
                 "at = timeline frames inside the clip as it is now; smooth = frames of a soft change around the "
                 "point. The clip length follows the speeds, later clips move. Replaces earlier points.",
                 opSpeedRamp}
        << OpDef{"marker", "{op:'marker', at, remove?}", {}, {}, opMarker}
        << OpDef{"marks", "{op:'marks', in?, out?, clear?}", "timeline In/Out range (I/O)", {}, opMarks}
        << OpDef{"media", "{op:'media', media, bin?, color?, flags?, in?, out?}",
                 "Media Pool: move into a bin (\"Name\" or \"A/B\", created if missing), clip color, flags, source "
                 "In/Out",
                 {}, opMedia}
        << OpDef{"copy", "{op:'copy', clips, at}", "copy clips to `at` (same tracks, overwrites)", {}, opCopy}
        << OpDef{"insert", "{op:'insert'|'replace'|'place_on_top'|'ripple_overwrite', media, at, in?, out?, track?}",
                 "source edits like F9–F12: insert (later clips move right), replace (the clip under `at`, keeps its "
                 "length), place_on_top (first free track above), ripple_overwrite (replace, the rest moves by the "
                 "length difference)",
                 {}, [](Session& s, const QJsonObject& op) { opSourceEdit(s, op, Editor::SourceEditMode::Insert); }}
        << OpDef{"replace", {}, {}, {},
                 [](Session& s, const QJsonObject& op) { opSourceEdit(s, op, Editor::SourceEditMode::Replace); }}
        << OpDef{"place_on_top", {}, {}, {},
                 [](Session& s, const QJsonObject& op) { opSourceEdit(s, op, Editor::SourceEditMode::PlaceOnTop); }}
        << OpDef{"ripple_overwrite", {}, {}, {},
                 [](Session& s, const QJsonObject& op) { opSourceEdit(s, op, Editor::SourceEditMode::RippleOverwrite); }}
        << OpDef{"fit_to_fill", "{op:'fit_to_fill', media, at, to, in?, out?, track?}",
                 "source range sped up/slowed down to fill exactly [at, to) (overwrites)", {},
                 [](Session& s, const QJsonObject& op) { opSourceEdit(s, op, Editor::SourceEditMode::FitToFill); }};
}

} // namespace Cli::detail
