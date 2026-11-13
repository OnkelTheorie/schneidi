// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli edit operations for shapes (0.22): op 'shape' places or changes a shape title (circle, rectangle, arrow,
// star …; fill colour, blur or pixelate, stroke that may draw itself, shadow, spotlight, following a tracker) and op
// 'clip_shape' cuts a clip's picture to a shape (effect "clip_shape": a face cam as a circle). What `info` shows of a
// shape is shapeJson.
#include "cli/CommandsDetail.h"

#include "core/BrandKit.h"
#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Shapes.h"
#include "core/TimelineOps.h"
#include "core/TitleLookJson.h"
#include "engine/picture/CompTrack.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

QString kindList()
{
    QStringList ids;
    for (const auto& k : kShapeKinds) ids << k.id;
    return ids.join(", ");
}

// A clip of the timeline with tracker node `node` in its compositing graph that is on screen in [from, to); the one on
// the highest track below `belowTrack` first (what the shape lies over), else any. nullptr = none
const Clip* trackerClip(const Timeline& tl, int node, int from, int to, int belowTrack)
{
    const Clip* best = nullptr;
    int bestTrack = -1;
    for (int i = 0; i < tl.video.size(); ++i)
        for (const Clip& c : tl.video[i].clips) {
            const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
            CompGraph g;
            if (!e || !::Comp::graphOf(*e, &g) || !g.node(node) || g.node(node)->type != QLatin1String(::Comp::kTrackerType))
                continue;
            if (c.end() <= from || c.start >= to) continue;
            const bool below = belowTrack < 0 || i < belowTrack;
            const bool bestBelow = best && (belowTrack < 0 || bestTrack < belowTrack);
            if (!best || (below && !bestBelow) || (below == bestBelow && i > bestTrack)) {
                best = &c;
                bestTrack = i;
            }
        }
    return best;
}

} // namespace

// follow: {clip, node} or a node id (the clip: one under the shape with that tracker), false/0 = none
void applyFollow(Session& s, TitleStyle& t, const QJsonObject& op, const Clip& shape, int shapeTrack, bool placeAtRef)
{
    const QJsonValue v = op.value("follow");
    if (v.isBool() && !v.toBool()) {
        t.shape.followClip = t.shape.followNode = 0;
        return;
    }
    int clip = 0, node = 0;
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        clip = o.value("clip").toInt();
        node = o.value("node").toInt();
    } else if (v.isDouble()) {
        node = v.toInt();
    } else {
        fail("BAD_ARGUMENT", "follow is {clip, node} (a tracker node of that clip's comp graph), a node id, or false");
    }
    if (node <= 0 && clip <= 0) {
        t.shape.followClip = t.shape.followNode = 0;
        return;
    }
    if (node <= 0) fail("BAD_ARGUMENT", "follow needs the tracker's node id (see `info` comp nodes of the clip)");
    const Timeline& tl = s.project.timeline();
    const Clip* tracked = nullptr;
    if (clip > 0) {
        tracked = TimelineOps::findClip(tl, clip);
        if (!tracked) fail("BAD_ARGUMENT", QString("follow: there is no clip %1").arg(clip));
        const EffectInstance* e = EffectRegistry::instance(*tracked, ::Comp::kEffectId);
        CompGraph g;
        if (!e || !::Comp::graphOf(*e, &g) || !g.node(node) || g.node(node)->type != QLatin1String(::Comp::kTrackerType))
            fail("BAD_ARGUMENT", QString("follow: clip %1 has no tracker node %2 (op 'node' with type tracker, then "
                                         "'track_node')").arg(clip).arg(node));
    } else {
        tracked = trackerClip(tl, node, shape.start, shape.end(), shapeTrack);
        if (!tracked)
            fail("BAD_ARGUMENT", QString("follow: no clip under the shape has a tracker node %1; give {clip, node}").arg(node));
    }
    t.shape.followClip = tracked->id;
    t.shape.followNode = node;
    if (::Comp::trackGaps(*tracked, node).isEmpty() && !tracked->keys.contains(Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(node, "x"))))
        s.notes << QString("tracker %1 of clip %2 is not analyzed yet: run {op:'track_node', clip:%2, node:%1}; until "
                           "then the shape stays where it is").arg(node).arg(tracked->id);
    if (!placeAtRef) return;
    // Not placed by hand: onto the tracked point (where the tracker starts), so it sits on what it follows
    Picture::Comp::FollowAt at;
    if (!Picture::Comp::followAt(*tracked, node, 0, s.project.fps(), &at)) return;
    const ProjectFormat f = s.project.timelineFormat();
    double aspect = tracked->isTitle() || tracked->isNested() ? 0.0 : MediaAspect::ofClip(*tracked);
    const QSizeF fit = MediaAspect::fit(aspect, QSizeF(f.width, f.height));
    const bool tr = tracked->transform.transformOn;
    const double zx = tr ? tracked->transform.zoomX : 1, zy = tr ? tracked->transform.zoomY : 1;
    const double px = tr ? tracked->transform.posX : 0, py = tr ? tracked->transform.posY : 0;
    t.posX = px + (at.from.x() - 0.5) * fit.width() * zx;
    t.posY = py - (at.from.y() - 0.5) * fit.height() * zy;
}

namespace {

void opShape(Session& s, const QJsonObject& op)
{
    const bool created = !op.contains("clip");
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
    int id = 0;
    if (created) {
        const int track = op.contains("track") ? parseTrack(op.value("track")) : -1;
        const QHash<int, Clip> before =
            clipsIn(s, track, std::max(0, at), std::max(0, at) + (length > 0 ? length : 5 * s.project.fps()));
        s.selection.clear();
        s.editor.addTitle(std::max(0, at), track, length, TitleKind::Shape);
        if (s.selection.ids().isEmpty()) fail("FAILED", "could not place the shape (locked track?)");
        id = *s.selection.ids().begin();
        noteOverwritten(s, id, before);
    } else {
        const Clip& c = clipOf(s, op);
        if (!c.isTitle() || !c.title.isShape()) fail("BAD_ARGUMENT", QString("clip %1 is not a shape").arg(c.id));
        id = c.id;
        if (at >= 0 && at != c.start) s.editor.moveClips({id}, at - c.start, TrackKind::Video, 0);
        if (length > 0) s.editor.setTitleLength(id, length);
    }
    TrackRef ref;
    const Clip* clip = TimelineOps::findClip(s.project.timeline(), id, &ref);
    if (!clip) fail("FAILED", "the shape went missing");
    TitleStyle t = clip->title;
    ShapeStyle& sh = t.shape;
    if (op.contains("kind") && !Shapes::kindFromId(op.value("kind").toString(), &sh.kind))
        fail("BAD_ARGUMENT", QString("kind must be one of %1").arg(kindList()));
    if (op.contains("size")) sh.width = sh.height = numberOf(op, "size", 0, 100000);
    if (op.contains("width")) sh.width = numberOf(op, "width", 0, 100000);
    if (op.contains("height")) sh.height = numberOf(op, "height", 0, 100000);
    if (op.contains("rotation")) sh.rotation = numberOf(op, "rotation", -36000, 36000);
    if (op.contains("corner")) sh.corner = numberOf(op, "corner", 0, 100000);
    if (op.contains("points")) sh.points = int(numberOf(op, "points", 3, 32));
    if (op.contains("inner")) sh.inner = numberOf(op, "inner", 0.05, 1);
    if (op.contains("x")) t.posX = numberOf(op, "x", -100000, 100000);
    if (op.contains("y")) t.posY = numberOf(op, "y", -100000, 100000);
    if (op.contains("fill")) {
        const QJsonValue f = op.value("fill");
        if (!Shapes::fillFromId(f.toString(), &sh.fill)) {
            // A colour as the fill: colour fill in it
            if (f.isString() && f.toString().startsWith('#')) {
                sh.fill = ShapeFill::Color;
                t.color = colorOf(f, "fill");
                t.colorRef = 0; // a fixed colour: no brand colour any more
            } else {
                fail("BAD_ARGUMENT", "fill must be color, none, blur, pixelate or a colour \"#rrggbb\"");
            }
        }
    }
    if (op.contains("color")) t.color = colorOf(op.value("color"), "color"), t.colorRef = 0;
    if (op.contains("amount")) sh.amount = numberOf(op, "amount", 0, 100);
    if (op.contains("feather")) sh.feather = numberOf(op, "feather", 0, 10000);
    if (op.contains("spotlight")) sh.spotlight = numberOf(op, "spotlight", 0, 100);
    if (op.contains("reveal")) sh.reveal = numberOf(op, "reveal", 0, 100);
    if (op.contains("stroke")) {
        const QJsonValue v = op.value("stroke");
        if (v.isBool()) t.outlineOn = v.toBool();
        else if (v.isString()) t.outlineOn = true, t.outlineColor = colorOf(v, "stroke"), t.outlineRef = 0;
        else if (v.isDouble()) t.outlineOn = v.toDouble() > 0, t.outlineWidth = numberOf(op, "stroke", 0, 2000);
        else if (v.isObject()) { // as `info` shows it: {color, width}
            const QJsonObject o = v.toObject();
            t.outlineOn = true;
            if (o.contains("color")) t.outlineColor = colorOf(o.value("color"), "stroke.color"), t.outlineRef = 0;
            if (o.contains("width")) t.outlineWidth = numberOf(o, "width", 0, 2000), t.outlineOn = t.outlineWidth > 0;
        } else fail("BAD_ARGUMENT", "stroke is true/false, a colour, a width or {color, width}");
    }
    if (op.contains("stroke_color")) t.outlineOn = true, t.outlineColor = colorOf(op.value("stroke_color"), "stroke_color"), t.outlineRef = 0;
    if (op.contains("stroke_width")) t.outlineWidth = numberOf(op, "stroke_width", 0, 2000), t.outlineOn = t.outlineWidth > 0;
    if (op.contains("shadow")) {
        QString err;
        if (!TitleLookJson::read(QJsonObject{{"shadow", op.value("shadow")}}, t.look, false, &err)) fail("BAD_ARGUMENT", err);
    }
    if (op.contains("follow")) applyFollow(s, t, op, *clip, ref.index, !op.contains("x") && !op.contains("y"));
    if (sh.kind == ShapeKind::Line && op.contains("kind") && !op.contains("height") && !op.contains("size"))
        sh.height = std::max(2.0, std::round(s.project.timelineFormat().height / 135.0)); // a line: 8 px at 1080
    if (sh.fill == ShapeFill::None && !t.outlineOn && sh.kind != ShapeKind::Line && sh.spotlight <= 0)
        s.notes << "fill none without a stroke draws nothing: give stroke:true (stroke_color, stroke_width) or spotlight";
    const bool opacity = op.contains("opacity");
    const double opacityValue = opacity ? numberOf(op, "opacity", 0, 100) : 100;
    // draw_on: the stroke draws itself from the shape's start over that time (keyframes 0 -> 100 % of shape_reveal)
    const int drawOn = op.contains("draw_on") ? timeOf(s, op, "draw_on") : 0;
    if (op.contains("draw_on") && drawOn <= 0) fail("BAD_ARGUMENT", "draw_on must be at least one frame");
    if (drawOn > 0 && !t.outlineOn && sh.kind != ShapeKind::Line) t.outlineOn = true;
    s.editor.modifyClips({id}, "Shape", [&](Clip& c) {
        c.title = t;
        Brand::resolve(c, s.project.effectiveBrand()); // a brand colour reference fills in its colour
        if (opacity) {
            c.transform.opacity = opacityValue;
            c.transform.compositeOn = true;
        }
        if (drawOn > 0) {
            Keys::clear(c, AnimParam::ShapeReveal);
            Keys::setKey(c, AnimParam::ShapeReveal, 0, 0); // clip frames
            Keys::setKey(c, AnimParam::ShapeReveal, std::min(drawOn, c.length() - 1), 100);
        }
    });
    if (created) s.notes << QJsonObject{{"shape", id}};
}

// The effect's values: given keys onto the instance
void opClipShape(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipsOn(s, op, TrackKind::Video, "a clip shape");
    const QString effect = QString::fromLatin1(Shapes::kClipShapeEffect);
    const QString kind = op.value("shape").toString("ellipse");
    const bool remove = kind == "none" || op.value("remove").toBool();
    ShapeKind k = ShapeKind::Ellipse;
    if (!remove && (!Shapes::kindFromId(kind, &k) || k == ShapeKind::Line))
        fail("BAD_ARGUMENT", "shape must be ellipse, rect, triangle, arrow, star, polygon or none");
    QVariantMap values;
    if (op.contains("shape") && !remove) values["shape"] = kind;
    const struct { const char* key; double lo, hi; } nums[] = {
        {"size", 0, 400}, {"aspect", 0.05, 20}, {"x", -100, 100}, {"y", -100, 100}, {"corner", 0, 50}, {"points", 3, 32},
        {"feather", 0, 500}, {"stroke_width", 0, 500}, {"shadow_soft", 0, 200}, {"shadow_opacity", 0, 1}};
    for (const auto& n : nums)
        if (op.contains(n.key)) values[n.key] = numberOf(op, n.key, n.lo, n.hi);
    if (op.contains("stroke_color")) {
        values["stroke_color"] = colorOf(op.value("stroke_color"), "stroke_color").name(QColor::HexArgb);
        if (!op.contains("stroke_width")) values["stroke_width"] = std::max(2.0, std::round(s.project.timelineFormat().height / 180.0));
    }
    if (op.contains("shadow")) values["shadow"] = op.value("shadow").toBool();
    s.editor.modifyClips(ids, remove ? "Remove clip shape" : "Clip shape", [&](Clip& c) {
        if (remove) {
            EffectRegistry::remove(c, effect);
            return;
        }
        if (!EffectRegistry::has(c, effect)) EffectRegistry::add(c, effect);
        EffectInstance* e = EffectRegistry::instance(c, effect);
        if (!e) return;
        e->enabled = true;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            e->params[it.key()] = it.value();
            Keys::clear(c, Keys::effectParam(effect, it.key())); // a value given replaces its animation
        }
    });
}

} // namespace

QJsonObject shapeJson(const TitleStyle& t)
{
    const ShapeStyle& sh = t.shape;
    QJsonObject o{{"kind", Shapes::kindId(sh.kind)}, {"width", sh.width}, {"height", sh.height},
                  {"x", t.posX}, {"y", t.posY}, {"fill", Shapes::fillId(sh.fill)}};
    if (sh.fill == ShapeFill::Color || sh.kind == ShapeKind::Line)
        o["color"] = t.color.alpha() == 255 ? t.color.name() : t.color.name(QColor::HexArgb);
    if (sh.fill == ShapeFill::Blur || sh.fill == ShapeFill::Pixelate) o["amount"] = sh.amount;
    if (sh.rotation != 0) o["rotation"] = sh.rotation;
    if (sh.kind == ShapeKind::Rect && sh.corner > 0) o["corner"] = sh.corner;
    if (sh.kind == ShapeKind::Star || sh.kind == ShapeKind::Polygon) o["points"] = sh.points;
    if (sh.kind == ShapeKind::Star) o["inner"] = sh.inner;
    if (sh.feather > 0) o["feather"] = sh.feather;
    if (sh.spotlight > 0) o["spotlight"] = sh.spotlight;
    if (t.outlineOn)
        o["stroke"] = QJsonObject{{"color", t.outlineColor.name(QColor::HexArgb)}, {"width", t.outlineWidth}};
    if (sh.reveal < 100) o["reveal"] = sh.reveal;
    if (sh.followClip > 0) o["follow"] = QJsonObject{{"clip", sh.followClip}, {"node", sh.followNode}};
    if (t.look.shadowOn) {
        QJsonObject look;
        TitleLookJson::write(look, t.look);
        o["shadow"] = look.value("shadow");
    }
    return o;
}

void addShapeOps(QVector<OpDef>& ops)
{
    ops << OpDef{"shape",
                 "{op:'shape', kind?, clip?, at?|from?, duration?|until?, track?, x?, y?, size?, width?, height?, "
                 "rotation?, corner?, points?, inner?, fill?, color?, amount?, feather?, stroke?, stroke_color?, "
                 "stroke_width?, shadow?, opacity?, spotlight?, reveal?, draw_on?, follow?}",
                 "new shape clip (circle, rectangle, arrow …; default 5s on a free track above) or with clip: change one",
                 "kind: " + kindList() + " (ellipse with width = height is a circle; arrow points right, turn it with "
                 "rotation; line = stroke only, height = its thickness, drawn in `color`). Pixels of the timeline "
                 "format: x/y = centre from the picture centre (y up), width/height (size = both), rotation degrees "
                 "clockwise, corner = rect corner radius, points = star tips / polygon corners (3..32), inner = star "
                 "inner radius (0.05..1). fill: color (default, in `color` \"#rrggbb\"/\"#aarrggbb\"), none (stroke "
                 "only), blur or pixelate (the picture below is blurred / a mosaic inside the shape: a face, a number "
                 "plate, a name; amount 0..100, blur 100 = 1/20 of the frame height, pixelate 100 = blocks 1/10 of it), "
                 "feather = soft edge in pixels. stroke true/false or a colour, stroke_color, stroke_width (drawn on the "
                 "outline). shadow true/false or {x, y, soft, color, opacity}. opacity 0..100. spotlight 0..100 darkens "
                 "the picture outside the shape (\"look here\"). reveal 0..100 = how much of the stroke is drawn; "
                 "draw_on:'1s' makes it draw itself from the shape's start (keyframes shape_reveal 0 -> 100; other "
                 "animations: op 'keyframe' with title_x, title_y, title_color, shape_width, shape_height, "
                 "shape_rotation, shape_reveal, opacity). follow: {clip, node} = a tracker node of that clip's comp graph "
                 "(op 'node' type tracker + 'track_node'), or just the node id (the clip under the shape with it); "
                 "without x/y the shape is put on the tracked point; it then moves (and scales/turns with a two-point "
                 "tracker) with it and fades where the point is lost; follow:false ends it. Example blurred face: "
                 "{op:'shape', kind:'ellipse', fill:'blur', amount:60, feather:20, follow:{clip:4, node:2}}; circle "
                 "around a player drawing itself: {op:'shape', fill:'none', stroke:'#ffd600', stroke_width:8, "
                 "draw_on:'0.5s', at:'3s', duration:'2s'}. Blur, pixelate and spotlight work on the shape's own "
                 "place (the clip transform's zoom/position/rotation apply, crop and effects not). `info` lists the "
                 "shape under clip.shape, `check` treats it like a title (safe areas).",
                 opShape}
        << OpDef{"clip_shape",
                 "{op:'clip_shape', clips, shape?, size?, aspect?, x?, y?, corner?, points?, feather?, stroke_width?, "
                 "stroke_color?, shadow?, shadow_soft?, shadow_opacity?}",
                 "cut a clip's picture to a shape (a face cam as a circle) with stroke and shadow; shape:'none' removes it",
                 "shape: ellipse (default; aspect 1 = a circle), rect (corner = rounding in %), triangle, arrow, star, "
                 "polygon (points), none. size = height of the shape in % of the picture's shorter side (default 90), "
                 "aspect = width / height, x/y = offset in % of the picture (y up), feather = soft edge, stroke_width "
                 "(pixels; given a stroke_color alone it gets a width), stroke_color, shadow true/false (shadow_soft, "
                 "shadow_opacity 0..1). It is the effect clip_shape: its values animate with op 'keyframe' "
                 "(param clip_shape.size, clip_shape.x …); place and scale the cut clip with op 'transform'. "
                 "Example: {op:'clip_shape', clips:[5], shape:'ellipse', size:80, stroke_color:'#ffffff', shadow:true} "
                 "then {op:'transform', clips:[5], zoom:0.35, x:700, y:-330}.",
                 opClipShape, kClipKeys}
        ;
}

} // namespace Cli::detail
