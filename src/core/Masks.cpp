// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Masks.h"

#include <algorithm>
#include <cmath>

namespace Masks {

namespace {

constexpr const char* kShapeIds[] = {"rect", "ellipse", "polygon", "bezier", "linear", "radial"};
constexpr const char* kCombineIds[] = {"add", "subtract", "intersect"};

// Clockwise on screen (y downward)
QPointF rotated(QPointF d, double cosA, double sinA)
{
    return {d.x() * cosA - d.y() * sinA, d.x() * sinA + d.y() * cosA};
}

QPointF middle(const QVector<QPointF>& pts)
{
    double x0 = pts[0].x(), x1 = x0, y0 = pts[0].y(), y1 = y0;
    for (const QPointF& p : pts) {
        x0 = std::min(x0, p.x());
        x1 = std::max(x1, p.x());
        y0 = std::min(y0, p.y());
        y1 = std::max(y1, p.y());
    }
    return {(x0 + x1) / 2, (y0 + y1) / 2};
}

bool finite(QPointF p) { return std::isfinite(p.x()) && std::isfinite(p.y()); }

} // namespace

const char* shapeId(Shape s) { return kShapeIds[int(s)]; }

bool shapeFromId(const QString& id, Shape* s)
{
    for (int i = 0; i < int(std::size(kShapeIds)); ++i)
        if (id == QLatin1String(kShapeIds[i])) {
            if (s) *s = Shape(i);
            return true;
        }
    return false;
}

const char* combineId(Combine c) { return kCombineIds[int(c)]; }

bool combineFromId(const QString& id, Combine* c)
{
    for (int i = 0; i < int(std::size(kCombineIds)); ++i)
        if (id == QLatin1String(kCombineIds[i])) {
            if (c) *c = Combine(i);
            return true;
        }
    return false;
}

QVector<QPointF> flattenBezier(const QVector<QPointF>& pts, const QVector<QPointF>& in, const QVector<QPointF>& out,
                               double tolerance)
{
    QVector<QPointF> line;
    const int n = int(pts.size());
    if (n < 2) return pts;
    tolerance = std::max(tolerance, 1e-3);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const QPointF p0 = pts[i], p1 = i < out.size() ? out[i] : pts[i];
        const QPointF p2 = j < in.size() ? in[j] : pts[j], p3 = pts[j];
        // Deviation of n uniform steps <= 3/4 * max second difference / n^2
        const QPointF d1 = p0 - 2 * p1 + p2, d2 = p1 - 2 * p2 + p3;
        const double dd = std::max(std::hypot(d1.x(), d1.y()), std::hypot(d2.x(), d2.y()));
        const int steps = std::clamp(int(std::ceil(std::sqrt(0.75 * dd / tolerance))), 1, 128);
        line.append(p0);
        for (int k = 1; k < steps; ++k) {
            const double t = double(k) / steps, u = 1 - t;
            line.append(u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3);
        }
    }
    return line;
}

Geometry toPixels(const Spec& s, QSize size)
{
    Geometry g;
    g.shape = s.shape;
    g.invert = s.invert;
    g.soft = std::max(1.0, std::isfinite(s.soft) ? s.soft : 0.0);
    g.rotation = std::isfinite(s.rotation) ? s.rotation : 0;
    const double W = size.width(), H = size.height();
    if (W <= 0 || H <= 0) return g;
    const auto px = [W, H](QPointF p) { return QPointF(p.x() * W, p.y() * H); };
    switch (s.shape) {
    case Shape::Rect:
    case Shape::Ellipse:
        g.center = px(s.center);
        g.halfW = s.width * W / 2;
        g.halfH = s.height * W / 2;
        g.empty = !(g.halfW > 0 && g.halfH > 0) || !finite(g.center) || !std::isfinite(g.halfW + g.halfH);
        return g;
    case Shape::Polygon:
    case Shape::Bezier: {
        if (s.points.size() < 3) return g;
        QVector<QPointF> pts, in, out;
        for (int i = 0; i < s.points.size(); ++i) {
            pts.append(px(s.points[i]));
            out.append(px(s.points[i] + (i < s.handlesOut.size() ? s.handlesOut[i] : QPointF())));
            in.append(px(s.points[i] + (i < s.handlesIn.size() ? s.handlesIn[i] : QPointF())));
        }
        if (g.rotation != 0) {
            const double r = g.rotation * M_PI / 180, c = std::cos(r), sn = std::sin(r);
            const QPointF m = middle(pts);
            for (QVector<QPointF>* v : {&pts, &in, &out})
                for (QPointF& p : *v) p = m + rotated(p - m, c, sn);
        }
        g.path = s.shape == Shape::Bezier ? flattenBezier(pts, in, out, 0.25) : pts;
        // A polygon without area (all corners on a line or on one point) draws nothing. Unsigned triangle fan: the
        // wings of a self-intersecting bow-tie cancel out in the signed area but still have one
        double area = 0;
        for (int i = 0; i < g.path.size(); ++i) {
            if (!finite(g.path[i])) return g;
            if (i < 2) continue;
            const QPointF a = g.path[i - 1] - g.path[0], b = g.path[i] - g.path[0];
            area += std::abs(a.x() * b.y() - b.x() * a.y());
        }
        g.empty = !(area > 1e-9);
        return g;
    }
    case Shape::Linear:
    case Shape::Radial: {
        if (s.points.size() < 2) return g;
        g.a = px(s.points[0]);
        g.b = px(s.points[1]);
        if (g.rotation != 0) {
            const double r = g.rotation * M_PI / 180, c = std::cos(r), sn = std::sin(r);
            const QPointF m = (g.a + g.b) / 2;
            g.a = m + rotated(g.a - m, c, sn);
            g.b = m + rotated(g.b - m, c, sn);
        }
        const QPointF d = g.b - g.a;
        g.empty = !finite(g.a) || !finite(g.b) || !(std::hypot(d.x(), d.y()) > 1e-9);
        return g;
    }
    }
    return g;
}

Geometry followed(Geometry g, const Follow& f)
{
    if (g.empty) return g;
    const double scale = f.scale > 0 && std::isfinite(f.scale) ? f.scale : 1;
    const double angle = std::isfinite(f.angle) ? f.angle : 0;
    const double r = angle * M_PI / 180, c = std::cos(r), s = std::sin(r);
    const auto map = [&](QPointF p) { return f.to + scale * rotated(p - f.from, c, s); };
    g.center = map(g.center);
    g.halfW *= scale;
    g.halfH *= scale;
    g.rotation += angle;
    for (QPointF& p : g.path) p = map(p);
    g.a = map(g.a);
    g.b = map(g.b);
    return g;
}

QRectF bounds(const Geometry& g)
{
    if (g.empty) return {};
    switch (g.shape) {
    case Shape::Rect:
    case Shape::Ellipse: {
        const double r = g.rotation * M_PI / 180, c = std::abs(std::cos(r)), s = std::abs(std::sin(r));
        const double ex = g.halfW * c + g.halfH * s + g.soft, ey = g.halfW * s + g.halfH * c + g.soft;
        return QRectF(g.center.x() - ex, g.center.y() - ey, 2 * ex, 2 * ey);
    }
    case Shape::Polygon:
    case Shape::Bezier: {
        double x0 = g.path[0].x(), x1 = x0, y0 = g.path[0].y(), y1 = y0;
        for (const QPointF& p : g.path) {
            x0 = std::min(x0, p.x());
            x1 = std::max(x1, p.x());
            y0 = std::min(y0, p.y());
            y1 = std::max(y1, p.y());
        }
        return QRectF(QPointF(x0 - g.soft, y0 - g.soft), QPointF(x1 + g.soft, y1 + g.soft));
    }
    case Shape::Linear: return QRectF(-1e9, -1e9, 2e9, 2e9); // 1 on the whole side before a
    case Shape::Radial: {
        const QPointF d = g.b - g.a;
        const double rad = std::hypot(d.x(), d.y());
        return QRectF(g.a.x() - rad, g.a.y() - rad, 2 * rad, 2 * rad);
    }
    }
    return {};
}

GradeWindows::Geometry toWindow(const Geometry& g, QSize size)
{
    GradeWindows::Geometry w;
    const double W = std::max(1, size.width()), H = std::max(1, size.height());
    w.shape = g.shape == Shape::Ellipse ? GradeWindows::Shape::Ellipse : GradeWindows::Shape::Rectangle;
    w.x = g.center.x() / W * 100;
    w.y = g.center.y() / H * 100;
    w.width = 2 * g.halfW / W * 100;
    w.height = 2 * g.halfH / H * 100;
    w.rotation = -g.rotation; // a window turns counterclockwise
    w.softness = g.soft / H * 100;
    w.invert = false;
    return w;
}

} // namespace Masks
