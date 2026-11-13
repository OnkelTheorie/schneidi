// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Shapes.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"

#include <QColor>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Shapes {

namespace {

constexpr double kPi = std::numbers::pi;

// Segments for an arc of `radius` turning by `turn` radians so the chords stay within `tolerance`
int arcSteps(double radius, double turn, double tolerance)
{
    if (radius <= tolerance) return 1;
    const double step = 2 * std::acos(std::max(-1.0, 1 - tolerance / radius));
    return std::clamp(int(std::ceil(std::abs(turn) / std::max(step, 1e-3))), 1, 720);
}

// A regular star or polygon in an ellipse of half sizes rx, ry: tips first at the top
QVector<QPointF> ring(int corners, double rx, double ry, double inner)
{
    QVector<QPointF> out;
    const int n = std::clamp(corners, 3, 32);
    const int steps = inner > 0 ? 2 * n : n;
    for (int i = 0; i < steps; ++i) {
        const double a = -kPi / 2 + 2 * kPi * i / steps;
        const double r = inner > 0 && i % 2 ? inner : 1.0;
        out << QPointF(std::cos(a) * rx * r, std::sin(a) * ry * r);
    }
    return out;
}

} // namespace

const char* kindId(ShapeKind k)
{
    for (const auto& i : kShapeKinds)
        if (i.kind == k) return i.id;
    return kShapeKinds[0].id;
}

bool kindFromId(const QString& id, ShapeKind* k)
{
    for (const auto& i : kShapeKinds)
        if (id == QLatin1String(i.id)) {
            *k = i.kind;
            return true;
        }
    return false;
}

const char* fillId(ShapeFill f)
{
    for (const auto& i : kShapeFills)
        if (i.fill == f) return i.id;
    return kShapeFills[0].id;
}

bool fillFromId(const QString& id, ShapeFill* f)
{
    for (const auto& i : kShapeFills)
        if (id == QLatin1String(i.id)) {
            *f = i.fill;
            return true;
        }
    return false;
}

Outline outline(const ShapeStyle& s, double w, double h, double tolerance)
{
    Outline o;
    w = std::max(0.0, w);
    h = std::max(0.0, h);
    const double rx = w / 2, ry = h / 2;
    tolerance = std::max(0.01, tolerance);
    switch (s.kind) {
    case ShapeKind::Ellipse: {
        const int n = std::max(8, arcSteps(std::max(rx, ry), 2 * kPi, tolerance));
        for (int i = 0; i < n; ++i) {
            const double a = -kPi / 2 + 2 * kPi * i / n;
            o.points << QPointF(std::cos(a) * rx, std::sin(a) * ry);
        }
        break;
    }
    case ShapeKind::Rect: {
        const double r = std::clamp(s.corner, 0.0, std::min(rx, ry));
        o.points << QPointF(0, -ry);
        // Corners clockwise from the top right: their centres and the angle each arc starts at
        const QPointF centres[4] = {{rx - r, -ry + r}, {rx - r, ry - r}, {-rx + r, ry - r}, {-rx + r, -ry + r}};
        const int steps = r > 0 ? arcSteps(r, kPi / 2, tolerance) : 0;
        for (int c = 0; c < 4; ++c) {
            const double a0 = -kPi / 2 + c * kPi / 2;
            if (steps == 0) { // a sharp corner
                o.points << QPointF(c == 0 || c == 1 ? rx : -rx, c == 1 || c == 2 ? ry : -ry);
                continue;
            }
            for (int i = 0; i <= steps; ++i) {
                const double a = a0 + kPi / 2 * i / steps;
                o.points << centres[c] + QPointF(std::cos(a) * r, std::sin(a) * r);
            }
        }
        break;
    }
    case ShapeKind::Triangle: o.points = {{0, -ry}, {rx, ry}, {-rx, ry}}; break;
    case ShapeKind::Arrow: {
        // Pointing right: the head is as long as it is wide (at most 60 % of the length), the shaft 40 % of its width
        const double head = std::min(h, 0.6 * w), shaft = 0.4 * ry;
        const double neck = rx - head;
        o.points = {{-rx, -shaft}, {neck, -shaft}, {neck, -ry}, {rx, 0}, {neck, ry}, {neck, shaft}, {-rx, shaft}};
        break;
    }
    case ShapeKind::Line:
        o.points = {{-rx, 0}, {rx, 0}};
        o.closed = false;
        break;
    case ShapeKind::Star: o.points = ring(s.points, rx, ry, std::clamp(s.inner, 0.05, 1.0)); break;
    case ShapeKind::Polygon: o.points = ring(s.points, rx, ry, 0); break;
    }
    return o;
}

Place placeAt(const Clip& c, double t, double PW, double PH)
{
    Place p;
    p.center = QPointF(PW / 2 + Keys::valueAt(c, AnimParam::TitlePosX, t), PH / 2 - Keys::valueAt(c, AnimParam::TitlePosY, t));
    p.w = std::max(0.0, Keys::valueAt(c, AnimParam::ShapeWidth, t));
    p.h = std::max(0.0, Keys::valueAt(c, AnimParam::ShapeHeight, t));
    p.angle = Keys::valueAt(c, AnimParam::ShapeRotation, t);
    p.reveal = std::clamp(Keys::valueAt(c, AnimParam::ShapeReveal, t) / 100.0, 0.0, 1.0);
    return p;
}

Outline placed(const ShapeStyle& s, const Place& p, const Masks::Follow* follow, double tolerance)
{
    Outline o = outline(s, p.w, p.h, tolerance);
    const double a = p.angle * kPi / 180, cs = std::cos(a), sn = std::sin(a);
    double fa = 0, fs = 1, fc = 1, fn = 0;
    if (follow) {
        fs = follow->scale > 0 && std::isfinite(follow->scale) ? follow->scale : 1.0;
        fa = std::isfinite(follow->angle) ? follow->angle * kPi / 180 : 0.0;
        fc = std::cos(fa);
        fn = std::sin(fa);
    }
    for (QPointF& q : o.points) {
        // y down: a clockwise turn by a is (x cos a - y sin a, x sin a + y cos a)
        QPointF v(q.x() * cs - q.y() * sn, q.x() * sn + q.y() * cs);
        v += p.center;
        if (follow) {
            const QPointF d = (v - follow->from) * fs;
            v = follow->to + QPointF(d.x() * fc - d.y() * fn, d.x() * fn + d.y() * fc);
        }
        q = v;
    }
    return o;
}

QVector<QPointF> partial(const QVector<QPointF>& points, bool closed, double fraction)
{
    QVector<QPointF> path = points;
    if (closed && !path.isEmpty()) path << path.first();
    if (path.size() < 2 || fraction >= 1) return path;
    if (fraction <= 0) return {};
    double total = 0;
    for (int i = 1; i < path.size(); ++i) total += std::hypot(path[i].x() - path[i - 1].x(), path[i].y() - path[i - 1].y());
    double left = total * fraction;
    QVector<QPointF> out{path.first()};
    for (int i = 1; i < path.size(); ++i) {
        const QPointF d = path[i] - path[i - 1];
        const double len = std::hypot(d.x(), d.y());
        if (len >= left) {
            if (len > 0) out << path[i - 1] + d * (left / len);
            return out;
        }
        left -= len;
        out << path[i];
    }
    return out;
}

QRectF bounds(const TitleStyle& t, const Outline& o)
{
    if (o.points.isEmpty()) return {};
    double x0 = o.points[0].x(), x1 = x0, y0 = o.points[0].y(), y1 = y0;
    for (const QPointF& p : o.points) {
        x0 = std::min(x0, p.x());
        x1 = std::max(x1, p.x());
        y0 = std::min(y0, p.y());
        y1 = std::max(y1, p.y());
    }
    QRectF r(QPointF(x0, y0), QPointF(x1, y1));
    double grow = std::max(1.0, t.shape.feather);
    if (t.outlineOn) grow = std::max(grow, t.outlineWidth / 2 + 1);
    // a line's pen: its thickness plus the outline on both sides (ShapePaint: lineWidth + 2 * outline)
    if (t.shape.kind == ShapeKind::Line)
        grow = std::max(grow, std::max(0.0, t.shape.height) / 2 + (t.outlineOn ? std::max(0.0, t.outlineWidth) : 0.0) + 1);
    r.adjust(-grow, -grow, grow, grow);
    if (t.look.shadowOn) {
        const double soft = 3 * std::max(0.0, t.look.shadowSoft);
        r |= r.translated(t.look.shadowX, t.look.shadowY).adjusted(-soft, -soft, soft, soft);
    }
    return r;
}

Masks::Geometry geometry(const QVector<QPointF>& points, double soft)
{
    Masks::Geometry g;
    g.shape = Masks::Shape::Polygon;
    g.path = points;
    g.soft = std::max(1.0, soft);
    g.empty = points.size() < 3;
    return g;
}

void remapFollow(Timeline& tl, const QHash<int, int>& ids)
{
    if (ids.isEmpty()) return;
    QSet<int> copies;
    for (const int id : ids) copies.insert(id);
    for (Track& t : tl.video)
        for (Clip& c : t.clips)
            if (copies.contains(c.id) && c.isTitle() && c.title.isShape() && ids.contains(c.title.shape.followClip))
                c.title.shape.followClip = ids.value(c.title.shape.followClip);
}

bool clipShapeAt(const Clip& c, double t, ClipShape* out)
{
    const EffectInstance* e = EffectRegistry::instance(c, kClipShapeEffect);
    if (!e || !e->enabled) return false;
    const auto num = [&](const char* key) {
        const AnimParam p = Keys::effectParam(QString::fromLatin1(kClipShapeEffect), QString::fromLatin1(key));
        return Keys::animated(c, p) ? Keys::valueAt(c, p, t) : EffectRegistry::value(c, kClipShapeEffect, key).toDouble();
    };
    ClipShape cs;
    kindFromId(EffectRegistry::value(c, kClipShapeEffect, "shape").toString(), &cs.shape.kind);
    cs.size = std::clamp(num("size"), 0.0, 400.0);
    cs.aspect = std::clamp(num("aspect"), 0.05, 20.0);
    cs.x = num("x");
    cs.y = num("y");
    cs.cornerPct = std::clamp(num("corner"), 0.0, 50.0);
    cs.shape.points = std::clamp(int(std::lround(num("points"))), 3, 32);
    cs.strokeWidth = std::max(0.0, num("stroke_width"));
    const QVariant sv = EffectRegistry::value(c, kClipShapeEffect, "stroke_color");
    const QColor sc = sv.typeId() == QMetaType::QColor ? sv.value<QColor>() : QColor(sv.toString());
    if (sc.isValid()) cs.strokeColor = sc;
    cs.shadow = EffectRegistry::value(c, kClipShapeEffect, "shadow").toBool();
    cs.shadowSoft = std::max(0.0, num("shadow_soft"));
    cs.shadowOpacity = std::clamp(num("shadow_opacity"), 0.0, 1.0);
    cs.feather = std::max(0.0, num("feather"));
    if (cs.shape.kind == ShapeKind::Line) cs.shape.kind = ShapeKind::Rect; // a line cuts nothing out
    *out = cs;
    return true;
}

Outline clipShapeOutline(const ClipShape& cs, double w, double h)
{
    const double side = std::min(w, h) * cs.size / 100;
    Place p;
    p.h = side;
    p.w = side * cs.aspect;
    p.center = QPointF(w / 2 + cs.x / 100 * w, h / 2 - cs.y / 100 * h);
    ShapeStyle s = cs.shape;
    s.corner = std::min(p.w, p.h) * cs.cornerPct / 100;
    return placed(s, p);
}

} // namespace Shapes
