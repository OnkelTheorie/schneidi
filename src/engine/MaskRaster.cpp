// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/MaskRaster.h"

#include "engine/video/Compose.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

namespace MaskRaster {

namespace {

inline uint8_t code(float v) { return uint8_t(v * 255.0f + 0.5f); } // v in 0..1

void rows(int y0, int y1, bool threads, const std::function<void(int, int)>& fn)
{
    if (y1 <= y0) return;
    if (!threads) {
        fn(y0, y1);
        return;
    }
    Compose::forRows(y1 - y0, 48, [&](int a, int b) { fn(y0 + a, y0 + b); });
}

QRect pixelRect(const QRectF& r, int w, int h)
{
    if (r.isEmpty()) return {};
    const double x0 = std::max(-1.0, std::floor(r.left())), y0 = std::max(-1.0, std::floor(r.top()));
    const double x1 = std::min(double(w), std::ceil(r.right())), y1 = std::min(double(h), std::ceil(r.bottom()));
    if (x1 <= x0 || y1 <= y0) return {};
    return QRect(QPoint(int(x0), int(y0)), QPoint(int(x1), int(y1))).intersected(QRect(0, 0, w, h));
}

// Rect / ellipse: the Color page's window mask. Both shapes are convex, so per row only the pixels up to the first
// fully inside one from each side are computed; between them everything is 255
QRect renderWindow(const Masks::Geometry& g, int w, int h, uint8_t* plane, bool threads)
{
    const GradeWindows::Mask m(Masks::toWindow(g, QSize(w, h)), w, h);
    const QRect r = m.bounds();
    if (r.isEmpty()) return {};
    rows(r.top(), r.bottom() + 1, threads, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            uint8_t* row = plane + size_t(y) * w;
            int xl = r.left();
            for (; xl <= r.right(); ++xl) {
                const float v = m.weight(xl, y);
                if (v >= 1) break;
                row[xl] = code(v);
            }
            if (xl > r.right()) continue;
            int xr = r.right();
            for (; xr > xl; --xr) {
                const float v = m.weight(xr, y);
                if (v >= 1) break;
                row[xr] = code(v);
            }
            std::memset(row + xl, 255, size_t(xr - xl + 1));
        }
    });
    return r;
}

struct Edge {
    double ax, ay, bx, by, dx, dy, invLen2, nx, ny, c, ymin, ymax;
};

// Polygon (and flattened bezier): inside by the non-zero winding rule on each row (pixel centers), the soft edge
// outside from the distance to the nearest outline segment - per row only the pixels a segment's band reaches
QRect renderPolygon(const Masks::Geometry& g, int w, int h, uint8_t* plane, bool threads)
{
    const QRect r = pixelRect(Masks::bounds(g), w, h);
    if (r.isEmpty()) return {};
    const int n = int(g.path.size());
    std::vector<Edge> edges;
    edges.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const QPointF a = g.path[i], b = g.path[(i + 1) % n];
        Edge e;
        e.ax = a.x();
        e.ay = a.y();
        e.bx = b.x();
        e.by = b.y();
        e.dx = e.bx - e.ax;
        e.dy = e.by - e.ay;
        const double len2 = e.dx * e.dx + e.dy * e.dy;
        e.invLen2 = len2 > 1e-12 ? 1 / len2 : 0;
        const double len = std::sqrt(len2);
        e.nx = len > 1e-9 ? -e.dy / len : 0;
        e.ny = len > 1e-9 ? e.dx / len : 0;
        e.c = e.nx * e.ax + e.ny * e.ay;
        e.ymin = std::min(e.ay, e.by);
        e.ymax = std::max(e.ay, e.by);
        edges.push_back(e);
    }
    const double R = g.soft, R2 = R * R;
    const int bx0 = r.left(), bw = r.width();
    rows(r.top(), r.bottom() + 1, threads, [&](int ya, int yb) {
        std::vector<float> d2(static_cast<size_t>(bw));
        std::vector<std::pair<double, int>> cross;
        for (int y = ya; y < yb; ++y) {
            const double yc = y + 0.5;
            std::fill(d2.begin(), d2.end(), float(R2));
            cross.clear();
            for (const Edge& e : edges) {
                if (yc >= e.ymin && yc < e.ymax) // half-open: a vertex on the row counts once
                    cross.emplace_back(e.ax + (yc - e.ay) * e.dx / e.dy, e.dy > 0 ? 1 : -1);
                if (yc < e.ymin - R || yc > e.ymax + R) continue;
                double lo = std::min(e.ax, e.bx) - R, hi = std::max(e.ax, e.bx) + R;
                if (std::abs(e.nx) > 1e-9) { // the band |n.p - c| <= R on this row
                    const double v = e.c - e.ny * yc;
                    const double x1 = (v - R) / e.nx, x2 = (v + R) / e.nx;
                    lo = std::max(lo, std::min(x1, x2));
                    hi = std::min(hi, std::max(x1, x2));
                }
                const int i0 = std::max(bx0, int(std::ceil(lo - 0.5)));
                const int i1 = std::min(bx0 + bw - 1, int(std::floor(hi - 0.5)));
                const double py = yc - e.ay;
                float* d = d2.data() - bx0;
                for (int x = i0; x <= i1; ++x) {
                    const double px = x + 0.5 - e.ax;
                    double t = (px * e.dx + py * e.dy) * e.invLen2;
                    t = t < 0 ? 0 : (t > 1 ? 1 : t);
                    const double qx = px - t * e.dx, qy = py - t * e.dy;
                    const float dd = float(qx * qx + qy * qy);
                    if (dd < d[x]) d[x] = dd;
                }
            }
            uint8_t* row = plane + size_t(y) * w;
            for (int i = 0; i < bw; ++i) {
                const float dd = d2[size_t(i)];
                if (dd >= float(R2)) continue;
                const float f = 1 - std::sqrt(dd) / float(R);
                row[bx0 + i] = code(f * f * (3 - 2 * f));
            }
            std::sort(cross.begin(), cross.end());
            int wind = 0;
            for (size_t k = 0; k + 1 < cross.size(); ++k) {
                wind += cross[k].second;
                if (wind == 0) continue;
                const int x0 = std::max(bx0, int(std::ceil(cross[k].first - 0.5)));
                const int x1 = std::min(bx0 + bw, int(std::ceil(cross[k + 1].first - 0.5)));
                if (x1 > x0) std::memset(row + x0, 255, size_t(x1 - x0));
            }
        }
    });
    return r;
}

QRect renderGradient(const Masks::Geometry& g, int w, int h, uint8_t* plane, bool threads)
{
    const QRect r = pixelRect(Masks::bounds(g), w, h);
    if (r.isEmpty()) return {};
    const double dx = g.b.x() - g.a.x(), dy = g.b.y() - g.a.y();
    const double len2 = dx * dx + dy * dy;
    const bool linear = g.shape == Masks::Shape::Linear;
    const double invLen2 = 1 / len2, invLen = 1 / std::sqrt(len2);
    rows(r.top(), r.bottom() + 1, threads, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            uint8_t* row = plane + size_t(y) * w;
            const double py = y + 0.5 - g.a.y();
            if (linear) { // 1 at a, 0 at b, steps of a constant per pixel
                double t = ((r.left() + 0.5 - g.a.x()) * dx + py * dy) * invLen2;
                const double step = dx * invLen2;
                for (int x = r.left(); x <= r.right(); ++x, t += step) {
                    const double v = 1 - t;
                    row[x] = v <= 0 ? 0 : (v >= 1 ? 255 : uint8_t(v * 255 + 0.5));
                }
            } else {
                const double py2 = py * py;
                for (int x = r.left(); x <= r.right(); ++x) {
                    const double px = x + 0.5 - g.a.x();
                    const double v = 1 - std::sqrt(px * px + py2) * invLen;
                    row[x] = v <= 0 ? 0 : uint8_t(v * 255 + 0.5);
                }
            }
        }
    });
    return r;
}

} // namespace

QRect render(const Masks::Geometry& g, int w, int h, uint8_t* plane, bool threads)
{
    if (g.empty || !plane || w <= 0 || h <= 0) return {};
    switch (g.shape) {
    case Masks::Shape::Rect:
    case Masks::Shape::Ellipse: return renderWindow(g, w, h, plane, threads);
    case Masks::Shape::Polygon:
    case Masks::Shape::Bezier: return renderPolygon(g, w, h, plane, threads);
    case Masks::Shape::Linear:
    case Masks::Shape::Radial: return renderGradient(g, w, h, plane, threads);
    }
    return {};
}

void invert(uint8_t* plane, int w, int h)
{
    const size_t n = size_t(std::max(0, w)) * std::max(0, h);
    for (size_t i = 0; i < n; ++i) plane[i] = uint8_t(255 - plane[i]);
}

QRect combine(uint8_t* dst, QRect dstArea, const uint8_t* src, QRect srcArea, int w, int h, Masks::Combine mode)
{
    // dst holds the shape (0 outside dstArea), src the mask it goes together with
    const QRect all(0, 0, w, h);
    dstArea &= all;
    srcArea &= all;
    const QRect u = dstArea | srcArea;
    QRect result;
    switch (mode) {
    case Masks::Combine::Add: result = u; break;
    case Masks::Combine::Subtract: result = srcArea; break;
    case Masks::Combine::Intersect: result = dstArea & srcArea; break;
    }
    if (u.isEmpty()) return {};
    for (int y = u.top(); y <= u.bottom(); ++y) {
        uint8_t* d = dst + size_t(y) * w;
        const uint8_t* s = src + size_t(y) * w;
        const bool rowIn = y >= srcArea.top() && y <= srcArea.bottom();
        const int s0 = rowIn ? srcArea.left() : u.right() + 1, s1 = rowIn ? srcArea.right() : u.right();
        if (mode != Masks::Combine::Add) { // no src there: 0
            for (int x = u.left(); x < s0; ++x) d[x] = 0;
            for (int x = s1 + 1; x <= u.right(); ++x) d[x] = 0;
        }
        for (int x = s0; x <= s1; ++x) {
            const uint8_t a = s[x], b = d[x];
            switch (mode) {
            case Masks::Combine::Add: d[x] = std::max(a, b); break;
            case Masks::Combine::Subtract: d[x] = std::min<uint8_t>(a, uint8_t(255 - b)); break;
            case Masks::Combine::Intersect: d[x] = std::min(a, b); break;
            }
        }
    }
    return result;
}

} // namespace MaskRaster
