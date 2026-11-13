// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli edit operation for drawings (0.22): op 'draw' places a drawing title (strokes as point lists: an arrow,
// a circle round a player, an underline) or adds strokes to one, removes strokes, makes it draw itself ("reveal") and
// lets it follow a tracker. What `info` shows of a drawing is drawingJson.
#include "cli/CommandsDetail.h"

#include "core/Drawing.h"
#include "core/TimelineOps.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

// One stroke of op draw: {points, color?, width?, opacity?, pressure?}; points [[x, y], …] or [x0, y0, x1, y1, …] in
// 0..1 of the frame (units 'frame') or pixels from the top left (units 'pixels'); width in pixels of the format.
// def = the op's color/width/opacity for strokes without their own
DrawStroke strokeOf(const QJsonValue& v, int index, bool pixels, const ProjectFormat& f, const DrawStroke& def)
{
    const QString where = QString("strokes[%1]").arg(index);
    QJsonObject o;
    if (v.isArray()) o["points"] = v; // just the points
    else if (v.isObject()) o = v.toObject();
    else fail("BAD_ARGUMENT", where + " is {points, color?, width?, opacity?} or a list of points");
    const QJsonArray raw = o.value("points").toArray();
    QVector<QPointF> pts;
    const auto add = [&](const QJsonValue& x, const QJsonValue& y) {
        if (!x.isDouble() || !y.isDouble()) fail("BAD_ARGUMENT", where + ".points must be numbers");
        QPointF p(x.toDouble(), y.toDouble());
        if (pixels) p = QPointF(p.x() / f.width, p.y() / f.height);
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || p.x() < -0.5 || p.x() > 1.5 || p.y() < -0.5 || p.y() > 1.5)
            fail("BAD_ARGUMENT", where + QString(".points: (%1, %2) lies far outside the picture (%3)")
                                     .arg(x.toDouble()).arg(y.toDouble())
                                     .arg(pixels ? QString("pixels %1 x %2").arg(f.width).arg(f.height) : QString("0..1 of the frame")));
        pts << p;
    };
    if (!raw.isEmpty() && raw[0].isArray()) {
        for (const QJsonValue& p : raw) {
            const QJsonArray xy = p.toArray();
            if (xy.size() != 2) fail("BAD_ARGUMENT", where + ".points: each point is [x, y]");
            add(xy[0], xy[1]);
        }
    } else {
        if (raw.size() % 2) fail("BAD_ARGUMENT", where + ".points: an even count of numbers [x0, y0, x1, y1, …]");
        for (int i = 0; i + 1 < raw.size(); i += 2) add(raw[i], raw[i + 1]);
    }
    if (pts.isEmpty()) fail("BAD_ARGUMENT", where + " has no points");
    DrawStroke s = def;
    s.points = pts;
    s.pressure.clear();
    if (o.contains("color")) s.color = colorOf(o.value("color"), where + ".color");
    if (o.contains("width")) s.width = numberOf(o, "width", 0.1, f.height) / f.height;
    if (o.contains("opacity")) s.opacity = numberOf(o, "opacity", 0, 100) / 100;
    if (o.contains("pressure")) {
        const QJsonArray pr = o.value("pressure").toArray();
        if (pr.size() != pts.size()) fail("BAD_ARGUMENT", where + ".pressure needs one value 0..1 per point");
        for (const QJsonValue& p : pr) s.pressure << float(std::clamp(p.toDouble(1), 0.0, 1.0));
    }
    return s;
}

void opDraw(Session& s, const QJsonObject& op)
{
    const bool created = !op.contains("clip");
    const ProjectFormat f = s.project.timelineFormat();
    const QString units = op.value("units").toString("frame");
    if (units != "frame" && units != "pixels") fail("BAD_ARGUMENT", "units must be frame (0..1, default) or pixels");
    DrawStroke def;
    def.width = std::max(2.0, std::round(f.height / 135.0)) / f.height; // 8 px at 1080
    def.color = QColor(255, 214, 0);
    if (op.contains("color")) def.color = colorOf(op.value("color"), "color");
    if (op.contains("width")) def.width = numberOf(op, "width", 0.1, f.height) / f.height;
    if (op.contains("opacity")) def.opacity = numberOf(op, "opacity", 0, 100) / 100;
    QVector<DrawStroke> strokes;
    if (op.contains("strokes")) {
        if (!op.value("strokes").isArray()) fail("BAD_ARGUMENT", "strokes is a list of strokes [{points:[…]}, …]");
        const QJsonArray a = op.value("strokes").toArray();
        for (int i = 0; i < a.size(); ++i) strokes << strokeOf(a[i], i, units == "pixels", f, def);
    }
    if (created && strokes.isEmpty()) fail("BAD_ARGUMENT", "a new drawing needs strokes:[{points:[x0, y0, x1, y1, …]}]");
    int at = -1, length = 0;
    if (op.contains("at") || op.contains("from")) at = timeOf(s, op, op.contains("at") ? "at" : "from");
    if (op.contains("duration")) length = timeOf(s, op, "duration");
    if (op.contains("until")) {
        const int until = timeOf(s, op, "until");
        const int from = at >= 0 ? at : (created ? 0 : clipOf(s, op).start);
        length = until - from;
        if (length <= 0) fail("BAD_ARGUMENT", "until must lie after from/at");
    }
    if ((op.contains("duration") || op.contains("until")) && length <= 0) fail("BAD_ARGUMENT", "duration must be at least one frame");
    double reveal = -1; // seconds; -1 = unchanged
    if (op.contains("reveal")) {
        const QJsonValue v = op.value("reveal");
        if ((v.isBool() && !v.toBool()) || (v.isDouble() && v.toDouble() == 0)) {
            reveal = 0;
        } else if (v.isBool()) {
            fail("BAD_ARGUMENT", "reveal is a time (\"2s\") or false");
        } else {
            const int frames = timeOf(s, op, "reveal");
            if (frames <= 0) fail("BAD_ARGUMENT", "reveal is a time (\"2s\") or false");
            reveal = frames / f.rate.fps();
        }
    }
    int id = 0;
    if (created) {
        const int track = op.contains("track") ? parseTrack(op.value("track")) : -1;
        const QHash<int, Clip> before =
            clipsIn(s, track, std::max(0, at), std::max(0, at) + (length > 0 ? length : 5 * s.project.fps()));
        s.selection.clear();
        s.editor.addTitle(std::max(0, at), track, length, TitleKind::Drawing);
        if (s.selection.ids().isEmpty()) fail("FAILED", "could not place the drawing (locked track?)");
        id = *s.selection.ids().begin();
        noteOverwritten(s, id, before);
    } else {
        const Clip& c = clipOf(s, op);
        if (!c.isTitle() || !c.title.isDrawing()) fail("BAD_ARGUMENT", QString("clip %1 is not a drawing").arg(c.id));
        id = c.id;
        if (at >= 0 && at != c.start) s.editor.moveClips({id}, at - c.start, TrackKind::Video, 0);
        if (length > 0) s.editor.setTitleLength(id, length);
    }
    TrackRef ref;
    const Clip* clip = TimelineOps::findClip(s.project.timeline(), id, &ref);
    if (!clip) fail("FAILED", "the drawing went missing");
    TitleStyle t = clip->title;
    DrawingStyle& d = t.drawing;
    if (op.value("clear").toBool()) d.strokes.clear();
    if (op.contains("remove")) {
        // stroke indices as `info` lists them (before the new strokes are added)
        QVector<int> gone;
        const QJsonValue rv = op.value("remove");
        const QJsonArray list = rv.isArray() ? rv.toArray() : QJsonArray{rv};
        for (const QJsonValue& v : list) {
            const int i = v.toInt(-1);
            if (!v.isDouble() || i < 0 || i >= d.strokes.size())
                fail("BAD_ARGUMENT", QString("remove: stroke %1 does not exist (the drawing has %2, counted from 0)")
                                         .arg(v.toVariant().toString()).arg(d.strokes.size()));
            gone << i;
        }
        std::sort(gone.begin(), gone.end());
        gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
        for (int k = int(gone.size()) - 1; k >= 0; --k) d.strokes.remove(gone[k]);
    }
    d.strokes += strokes;
    if (reveal >= 0) d.reveal = reveal;
    if (op.contains("x")) t.posX = numberOf(op, "x", -100000, 100000);
    if (op.contains("y")) t.posY = numberOf(op, "y", -100000, 100000);
    if (op.contains("follow")) applyFollow(s, t, op, *clip, ref.index, false);
    if (d.strokes.isEmpty()) s.notes << "the drawing has no strokes left: it draws nothing";
    const bool opacity = op.contains("clip_opacity");
    const double opacityValue = opacity ? numberOf(op, "clip_opacity", 0, 100) : 100;
    s.editor.modifyClips({id}, "Drawing", [&](Clip& c) {
        c.title = t;
        if (opacity) {
            c.transform.opacity = opacityValue;
            c.transform.compositeOn = true;
        }
    });
    if (created) s.notes << QJsonObject{{"drawing", id}};
}

} // namespace

QJsonObject drawingJson(const TitleStyle& t, const ProjectFormat& f)
{
    QJsonArray strokes;
    for (const DrawStroke& st : t.drawing.strokes) {
        QJsonObject o = Drawing::strokeJson(st);
        o["width"] = std::round(st.width * f.height * 100) / 100; // pixels, as op draw takes it
        if (o.contains("opacity")) o["opacity"] = std::round(st.opacity * 1000) / 10; // percent
        strokes << o;
    }
    QJsonObject o{{"strokes", strokes}, {"units", "frame"}};
    if (t.drawing.reveal > 0) o["reveal"] = QString("%1s").arg(t.drawing.reveal);
    if (t.posX != 0) o["x"] = t.posX;
    if (t.posY != 0) o["y"] = t.posY;
    if (t.shape.followClip > 0) o["follow"] = QJsonObject{{"clip", t.shape.followClip}, {"node", t.shape.followNode}};
    return o;
}

void addDrawOps(QVector<OpDef>& ops)
{
    ops << OpDef{"draw",
                 "{op:'draw', strokes?, clip?, at?|from?, duration?|until?, track?, units?, color?, width?, opacity?, "
                 "reveal?, clear?, remove?, x?, y?, clip_opacity?, follow?}",
                 "new drawing clip (strokes painted over the picture: arrows, circles, underlines; 5s on a free track "
                 "above) or with clip: add strokes to one, remove strokes, make it draw itself",
                 "strokes: a list of {points, color?, width?, opacity?, pressure?} (or just the points). points: "
                 "[[x, y], …] or [x0, y0, x1, y1, …]; units 'frame' (default: 0..1 of the picture, (0, 0) top left, "
                 "y down) or 'pixels' (of the timeline format, from the top left). One point = a dot. width in pixels "
                 "of the timeline format (default 8 at 1080 high), color \"#rrggbb\"/\"#aarrggbb\" (default yellow), "
                 "opacity 0..100, pressure: one value 0..1 per point (the width there; a pen). color/width/opacity on "
                 "the op = for its strokes without their own. Strokes are drawn with round ends in the given order; "
                 "the app smooths painted strokes, given points are kept as they are. With clip: strokes are added "
                 "after the drawing's own; clear:true removes all strokes first; remove: a stroke index or a list "
                 "(counted from 0 as `info` lists them; the app's eraser does the same). reveal: a time (\"2s\") = the "
                 "strokes draw themselves one after another from the clip's start over that time (each takes the share "
                 "its length has); reveal:false = all at once. x/y = moves the whole drawing (pixels, y up, like a "
                 "title). clip_opacity 0..100. Scale or turn it with op 'transform' like any title. follow: {clip, "
                 "node} = a tracker node of that clip's comp graph (op 'node' type tracker + 'track_node'), or just the "
                 "node id: the drawing moves (and scales/turns with a two-point tracker) with it from where it was "
                 "drawn at the tracker's start, and fades where the point is lost; follow:false ends it. `info` lists "
                 "a drawing under clip.drawing in the same form (round trip), `frames` shows it, `check` treats it "
                 "like a title (safe areas). Example: circle round a player drawing itself over 0.6 s: {op:'draw', "
                 "at:'3s', duration:'2s', reveal:'0.6s', width:10, color:'#ff3030', strokes:[{points:[0.5,0.3, "
                 "0.57,0.33, 0.6,0.4, 0.57,0.47, 0.5,0.5, 0.43,0.47, 0.4,0.4, 0.43,0.33, 0.5,0.3]}]}; an arrow: "
                 "strokes:[[[0.2,0.8],[0.45,0.55]], [[0.38,0.55],[0.45,0.55],[0.45,0.62]]].",
                 opDraw};
}

} // namespace Cli::detail
