// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/text3d/Text3d.h"

#include "engine/video/Compose.h"

#include <QPolygonF>

#include <algorithm>
#include <array>
#include <cmath>

namespace Text3d {

namespace {

using Contour = std::vector<QPointF>;

// The outline's closed contours, without the closing point and repeated points
std::vector<Contour> contoursOf(const QPainterPath& path)
{
    std::vector<Contour> out;
    for (const QPolygonF& poly : path.toSubpathPolygons()) {
        Contour c;
        for (const QPointF& p : poly)
            if (c.empty() || std::abs(p.x() - c.back().x()) + std::abs(p.y() - c.back().y()) > 1e-4) c.push_back(p);
        while (c.size() > 1 && std::abs(c.front().x() - c.back().x()) + std::abs(c.front().y() - c.back().y()) <= 1e-4)
            c.pop_back();
        if (c.size() >= 3) out.push_back(std::move(c));
    }
    return out;
}

// ---------- caps: trapezoids by a sweep over the edges ----------

struct Edge {
    double y0, y1; // y0 < y1
    double a, s;   // x = a + s * y
    int dir;       // +1 = the contour runs down here
    double x(double y) const { return a + s * y; }
};

struct Trapezoid {
    double y0, y1, l0, l1, r0, r1; // left/right x at y0 and y1
};

// The filled area of the contours (fill rule) as trapezoids between horizontal lines: the sweep stops at every
// vertex y and every crossing of two edges, an interval between two edges that stays inside over several stops is one
// trapezoid. Exact: the trapezoids cover the area once.
std::vector<Trapezoid> trapezoids(const std::vector<Contour>& contours, Qt::FillRule rule)
{
    std::vector<Edge> edges;
    std::vector<double> ys;
    for (const Contour& c : contours)
        for (size_t i = 0; i < c.size(); ++i) {
            const QPointF p = c[i], q = c[(i + 1) % c.size()];
            if (p.y() == q.y()) continue;
            Edge e;
            const bool down = q.y() > p.y();
            const QPointF top = down ? p : q, bot = down ? q : p;
            e.y0 = top.y();
            e.y1 = bot.y();
            e.s = (bot.x() - top.x()) / (bot.y() - top.y());
            e.a = top.x() - e.s * top.y();
            e.dir = down ? 1 : -1;
            edges.push_back(e);
            ys.push_back(e.y0);
            ys.push_back(e.y1);
        }
    std::vector<Trapezoid> out;
    if (edges.empty()) return out;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    struct Open {
        int l, r;
        double y;
        bool kept;
    };
    std::vector<Open> open;
    std::vector<int> active;
    std::vector<std::pair<int, int>> intervals;
    size_t next = 0;
    auto close = [&](const Open& o, double y) {
        const Edge &l = edges[size_t(o.l)], &r = edges[size_t(o.r)];
        if (y > o.y) out.push_back({o.y, y, l.x(o.y), l.x(y), r.x(o.y), r.x(y)});
    };
    for (size_t ei = 0; ei + 1 < ys.size(); ++ei) {
        double ya = ys[ei];
        const double yEnd = ys[ei + 1];
        while (ya < yEnd) {
            active.erase(std::remove_if(active.begin(), active.end(), [&](int i) { return edges[size_t(i)].y1 <= ya; }),
                         active.end());
            while (next < edges.size() && edges[next].y0 <= ya) active.push_back(int(next++));
            double yb = yEnd;
            for (int guard = 0; guard < 64; ++guard) {
                const double mid = (ya + yb) / 2;
                std::sort(active.begin(), active.end(), [&](int i, int j) {
                    const double xi = edges[size_t(i)].x(mid), xj = edges[size_t(j)].x(mid);
                    return xi != xj ? xi < xj : i < j;
                });
                double cross = yb;
                for (size_t k = 0; k + 1 < active.size(); ++k) {
                    const Edge &e = edges[size_t(active[k])], &f = edges[size_t(active[k + 1])];
                    if ((e.x(ya) > f.x(ya) + 1e-9 || e.x(yb) > f.x(yb) + 1e-9) && e.s != f.s) {
                        const double y = (f.a - e.a) / (e.s - f.s);
                        if (y > ya + 1e-9 && y < cross - 1e-9) cross = y;
                    }
                }
                if (cross >= yb) break;
                yb = cross;
            }
            // Inside intervals of this slab
            intervals.clear();
            int wind = 0, left = -1;
            for (int i : active) {
                const bool was = rule == Qt::WindingFill ? wind != 0 : (wind & 1) != 0;
                wind += edges[size_t(i)].dir;
                const bool is = rule == Qt::WindingFill ? wind != 0 : (wind & 1) != 0;
                if (!was && is) left = i;
                else if (was && !is) intervals.push_back({left, i});
            }
            for (Open& o : open) o.kept = false;
            std::vector<Open> now;
            for (const auto& [l, r] : intervals) {
                auto it = std::find_if(open.begin(), open.end(), [&](const Open& o) { return o.l == l && o.r == r; });
                if (it != open.end()) {
                    it->kept = true;
                    now.push_back(*it);
                } else {
                    now.push_back({l, r, ya, true});
                }
            }
            for (const Open& o : open)
                if (!o.kept) close(o, ya);
            open = std::move(now);
            ya = yb;
        }
    }
    for (const Open& o : open) close(o, ys.back());
    return out;
}

// ---------- mesh building ----------

struct Vec {
    double x = 0, y = 0, z = 0;
};
Vec norm(Vec v)
{
    const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 0 ? Vec{v.x / l, v.y / l, v.z / l} : v;
}

void addVertex(Mesh& m, double x, double y, double z, Vec n)
{
    m.pos.insert(m.pos.end(), {float(x), float(y), float(z)});
    m.nrm.insert(m.nrm.end(), {float(n.x), float(n.y), float(n.z)});
}

void addTri(Mesh& m, Mesh::Part part, const std::array<Vec, 3>& p, const std::array<Vec, 3>& n)
{
    for (int i = 0; i < 3; ++i) addVertex(m, p[i].x, p[i].y, p[i].z, n[i]);
    m.part.push_back(part);
}

void addCap(Mesh& m, const std::vector<Trapezoid>& traps, double z, Mesh::Part part)
{
    const Vec n{0, 0, part == Mesh::Front ? -1.0 : 1.0};
    const std::array<Vec, 3> ns{n, n, n};
    for (const Trapezoid& t : traps) {
        const Vec tl{t.l0, t.y0, z}, tr{t.r0, t.y0, z}, br{t.r1, t.y1, z}, bl{t.l1, t.y1, z};
        if (t.r0 - t.l0 > 1e-9) addTri(m, part, {tl, tr, br}, ns);
        if (t.r1 - t.l1 > 1e-9) addTri(m, part, {tl, br, bl}, ns);
    }
}

// Unit normal of each edge (i -> i+1) pointing out of the filled area (`inward` = which side is inside: +1 = the
// left normal (-dy, dx))
std::vector<QPointF> edgeNormals(const Contour& c, int inward)
{
    std::vector<QPointF> n(c.size());
    for (size_t i = 0; i < c.size(); ++i) {
        const QPointF d = c[(i + 1) % c.size()] - c[i];
        const double l = std::hypot(d.x(), d.y());
        const QPointF left = l > 0 ? QPointF(-d.y() / l, d.x() / l) : QPointF(0, 0);
        n[i] = -inward * left;
    }
    return n;
}

// Which side of the contour is filled (+1 = left of its direction, -1 = right), tested beside its longest edge
int inwardSide(const Contour& c, const QPainterPath& path)
{
    size_t best = 0;
    double bestLen = -1;
    for (size_t i = 0; i < c.size(); ++i) {
        const QPointF d = c[(i + 1) % c.size()] - c[i];
        const double l = std::hypot(d.x(), d.y());
        if (l > bestLen) bestLen = l, best = i;
    }
    const QPointF a = c[best], d = c[(best + 1) % c.size()] - a;
    const QPointF mid = a + d / 2, left(-d.y() / bestLen, d.x() / bestLen);
    const double eps = std::min(0.05, bestLen / 4);
    return path.contains(mid + left * eps) ? 1 : -1;
}

} // namespace

Mesh extrude(const QPainterPath& path, double depth, double bevel)
{
    Mesh m;
    const std::vector<Contour> outer = contoursOf(path);
    if (outer.empty()) return m;
    depth = std::max(0.0, depth);
    bevel = std::clamp(bevel, 0.0, depth / 2);
    const Qt::FillRule rule = path.fillRule();
    // Outward normals per contour edge; smooth vertex normals where neighbouring edges turn by less than ~35°
    constexpr double kSmooth = 0.82;
    std::vector<std::vector<QPointF>> normals;
    std::vector<Contour> inset;
    for (const Contour& c : outer) {
        const int inward = inwardSide(c, path);
        const std::vector<QPointF> n = edgeNormals(c, inward);
        normals.push_back(n);
        if (bevel <= 0) continue;
        Contour in(c.size());
        for (size_t i = 0; i < c.size(); ++i) {
            const QPointF a = -n[(i + c.size() - 1) % c.size()], b = -n[i]; // inward normals around vertex i
            QPointF mm = a + b;
            const double l = std::hypot(mm.x(), mm.y());
            mm = l > 1e-6 ? mm / l : b;
            const double d = std::max(0.5, QPointF::dotProduct(mm, b));
            in[i] = c[i] + mm * (bevel / d);
        }
        inset.push_back(std::move(in));
    }
    const std::vector<Contour>& capContours = bevel > 0 ? inset : outer;
    const std::vector<Trapezoid> traps = trapezoids(capContours, rule);
    addCap(m, traps, 0, Mesh::Front);
    if (depth <= 0) return m;
    addCap(m, traps, depth, Mesh::Back);
    for (size_t k = 0; k < outer.size(); ++k) {
        const Contour& c = outer[k];
        const std::vector<QPointF>& n = normals[k];
        const size_t cnt = c.size();
        auto smooth = [&](size_t edge, size_t vertexOfEdge) { // normal at the start (0) or end (1) of an edge
            const size_t other = vertexOfEdge == 0 ? (edge + cnt - 1) % cnt : (edge + 1) % cnt;
            const QPointF a = n[edge], b = n[other];
            if (QPointF::dotProduct(a, b) < kSmooth) return Vec{a.x(), a.y(), 0};
            return norm(Vec{a.x() + b.x(), a.y() + b.y(), 0});
        };
        auto band = [&](const Contour& p0, double z0, const Contour& p1, double z1, double tilt0, double tilt1) {
            // Quads between p0 at z0 and p1 at z1; normal = outward + tilt along z (bevel bands lean toward the cap)
            for (size_t i = 0; i < cnt; ++i) {
                const size_t j = (i + 1) % cnt;
                const Vec ni = smooth(i, 0), nj = smooth(i, 1);
                const Vec n0i = norm({ni.x, ni.y, tilt0}), n0j = norm({nj.x, nj.y, tilt0});
                const Vec n1i = norm({ni.x, ni.y, tilt1}), n1j = norm({nj.x, nj.y, tilt1});
                const Vec a{p0[i].x(), p0[i].y(), z0}, b{p0[j].x(), p0[j].y(), z0};
                const Vec cc{p1[j].x(), p1[j].y(), z1}, d{p1[i].x(), p1[i].y(), z1};
                addTri(m, Mesh::Side, {a, b, cc}, {n0i, n0j, n1j});
                addTri(m, Mesh::Side, {a, cc, d}, {n0i, n1j, n1i});
            }
        };
        if (bevel > 0) {
            band(inset[k], 0, c, bevel, -1, -1);
            band(c, bevel, c, depth - bevel, 0, 0);
            band(c, depth - bevel, inset[k], depth, 1, 1);
        } else {
            band(c, 0, c, depth, 0, 0);
        }
    }
    return m;
}

double area(const Mesh& m, Mesh::Part part)
{
    double sum = 0;
    for (int t = 0; t < m.triangles(); ++t) {
        if (m.part[size_t(t)] != part) continue;
        const float* p = m.pos.data() + size_t(t) * 9;
        sum += std::abs((p[3] - p[0]) * double(p[7] - p[1]) - (p[6] - p[0]) * double(p[4] - p[1])) / 2;
    }
    return sum;
}

// ---------- rendering ----------

namespace {

constexpr int kBandRows = 16; // output rows per band

struct Plane {
    float a = 0, b = 0, c = 0; // v = a * x + b * y + c
    float at(float x, float y) const { return a * x + b * y + c; }
};

struct Tri {
    float e[3][3];      // edge functions (a, b, c), inside >= 0
    float minY, maxY;   // sample rows
    Plane q, r, g, b;   // 1/depth (larger = nearer) and colour
};

Plane planeOf(const float x[3], const float y[3], const float v[3], double area2)
{
    Plane p;
    const double dx1 = x[1] - x[0], dy1 = y[1] - y[0], dx2 = x[2] - x[0], dy2 = y[2] - y[0];
    const double dv1 = v[1] - v[0], dv2 = v[2] - v[0];
    const double a = (dv1 * dy2 - dv2 * dy1) / area2, b = (dv2 * dx1 - dv1 * dx2) / area2;
    p.a = float(a);
    p.b = float(b);
    p.c = float(v[0] - a * x[0] - b * y[0]);
    return p;
}

} // namespace

QRect render(const Mesh& m, const View& v, uint8_t* rgba, int w, int h)
{
    if (m.empty() || w <= 0 || h <= 0) return {};
    const int ss = std::clamp(v.aa, 1, 4);
    const double fov = std::clamp(v.fov, 1.0, 150.0) * M_PI / 180;
    const double f = (h / 2.0) / std::tan(fov / 2);
    const double D = f * std::max(0.05, v.distance);
    const double near = f * 0.02;
    // Rotation: roll (z), then tilt (x), then turn (y)
    const double rx = v.rotX * M_PI / 180, ry = v.rotY * M_PI / 180, rz = v.rotZ * M_PI / 180;
    const double cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry), cz = std::cos(rz),
                 sz = std::sin(rz);
    auto rotate = [&](double x, double y, double z) {
        double x1 = x * cz - y * sz, y1 = x * sz + y * cz, z1 = z;     // roll clockwise
        double y2 = y1 * cx + z1 * sx, z2 = -y1 * sx + z1 * cx;        // top away
        double x3 = x1 * cy - z2 * sy, z3 = x1 * sy + z2 * cy;         // right side away
        return Vec{x3, y2, z3};
    };
    const double la = v.lightAngle * M_PI / 180, le = std::clamp(v.lightHeight, -90.0, 90.0) * M_PI / 180;
    const Vec L = norm({std::cos(le) * std::cos(la), std::cos(le) * std::sin(la), -std::sin(le)});
    const double amb = std::clamp(v.ambient, 0.0, 1.0), shine = std::clamp(v.shine, 0.0, 1.0),
                 metal = std::clamp(v.metal, 0.0, 1.0);
    const QColor colors[3] = {v.front, v.back.isValid() ? v.back : v.front, v.side};

    // Vertices to the picture (pixels) and their lit colour
    const int T = m.triangles();
    std::vector<float> sxv(size_t(T) * 3), syv(size_t(T) * 3), qv(size_t(T) * 3), col(size_t(T) * 9);
    std::vector<uint8_t> ok(size_t(T), 1);
    Compose::forRows(T, 4096, [&](int from, int to) {
        for (int t = from; t < to; ++t) {
            const QColor& base = colors[m.part[size_t(t)]];
            const double br = base.red(), bg = base.green(), bb = base.blue();
            for (int k = 0; k < 3; ++k) {
                const size_t i = size_t(t) * 3 + size_t(k);
                const float* p = m.pos.data() + i * 3;
                const float* n = m.nrm.data() + i * 3;
                const Vec pv = rotate(p[0] - v.pivot.x(), p[1] - v.pivot.y(), p[2] - v.depth / 2);
                const double zc = D + pv.z;
                if (zc < near) {
                    ok[size_t(t)] = 0;
                    break;
                }
                sxv[i] = float(v.at.x() + f * pv.x / zc);
                syv[i] = float(v.at.y() + f * pv.y / zc);
                qv[i] = float(f / zc);
                Vec nv = rotate(n[0], n[1], n[2]);
                const Vec toCam = norm({-pv.x, -pv.y, -zc});
                if (nv.x * toCam.x + nv.y * toCam.y + nv.z * toCam.z < 0) nv = {-nv.x, -nv.y, -nv.z};
                const double diff = std::max(0.0, nv.x * L.x + nv.y * L.y + nv.z * L.z);
                const Vec H = norm({L.x + toCam.x, L.y + toCam.y, L.z + toCam.z});
                double sp = std::max(0.0, nv.x * H.x + nv.y * H.y + nv.z * H.z);
                for (int s = 0; s < 5; ++s) sp *= sp; // ^32
                sp *= shine * 255;
                const double lit = amb + (1 - amb) * diff;
                const double hr = sp * (1 - metal + metal * br / 255), hg = sp * (1 - metal + metal * bg / 255),
                             hb = sp * (1 - metal + metal * bb / 255);
                col[i * 3] = float(std::min(255.0, br * lit + hr));
                col[i * 3 + 1] = float(std::min(255.0, bg * lit + hg));
                col[i * 3 + 2] = float(std::min(255.0, bb * lit + hb));
            }
        }
    });
    double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
    for (int t = 0; t < T; ++t) {
        if (!ok[size_t(t)]) continue;
        for (int k = 0; k < 3; ++k) {
            const size_t i = size_t(t) * 3 + size_t(k);
            minX = std::min(minX, double(sxv[i]));
            maxX = std::max(maxX, double(sxv[i]));
            minY = std::min(minY, double(syv[i]));
            maxY = std::max(maxY, double(syv[i]));
        }
    }
    if (minX > maxX) return {};
    const QRect box = QRect(QPoint(int(std::floor(minX)), int(std::floor(minY))),
                            QPoint(int(std::ceil(maxX)), int(std::ceil(maxY)))) & QRect(0, 0, w, h);
    if (box.isEmpty()) return {};
    const int bw = box.width() * ss, bands = (box.height() + kBandRows - 1) / kBandRows;

    // Triangle setup in sample coordinates of the box, bucketed by band
    std::vector<Tri> tris;
    std::vector<std::vector<int>> inBand(size_t(bands));
    tris.reserve(size_t(T));
    for (int t = 0; t < T; ++t) {
        if (!ok[size_t(t)]) continue;
        float X[3], Y[3], q[3], r[3], g[3], b[3];
        for (int k = 0; k < 3; ++k) {
            const size_t i = size_t(t) * 3 + size_t(k);
            X[k] = (sxv[i] - box.left()) * ss;
            Y[k] = (syv[i] - box.top()) * ss;
            q[k] = qv[i];
            r[k] = col[i * 3];
            g[k] = col[i * 3 + 1];
            b[k] = col[i * 3 + 2];
        }
        const double area2 = (double(X[1]) - X[0]) * (double(Y[2]) - Y[0]) - (double(X[2]) - X[0]) * (double(Y[1]) - Y[0]);
        if (std::abs(area2) < 1e-9) continue;
        Tri tr;
        const float sgn = area2 > 0 ? 1.f : -1.f;
        for (int k = 0; k < 3; ++k) {
            const int a = (k + 1) % 3, c = (k + 2) % 3; // edge a -> c, opposite vertex k
            const double ea = -(double(Y[c]) - Y[a]), eb = double(X[c]) - X[a];
            tr.e[k][0] = float(ea) * sgn;
            tr.e[k][1] = float(eb) * sgn;
            tr.e[k][2] = float(-(ea * X[a] + eb * Y[a])) * sgn;
        }
        tr.minY = std::min({Y[0], Y[1], Y[2]});
        tr.maxY = std::max({Y[0], Y[1], Y[2]});
        if (tr.maxY < 0 || tr.minY > box.height() * ss) continue;
        tr.q = planeOf(X, Y, q, area2);
        tr.r = planeOf(X, Y, r, area2);
        tr.g = planeOf(X, Y, g, area2);
        tr.b = planeOf(X, Y, b, area2);
        const int id = int(tris.size());
        tris.push_back(tr);
        const int b0 = std::clamp(int(tr.minY) / (kBandRows * ss), 0, bands - 1),
                  b1 = std::clamp(int(tr.maxY) / (kBandRows * ss), 0, bands - 1);
        for (int k = b0; k <= b1; ++k) inBand[size_t(k)].push_back(id);
    }

    Compose::forRows(bands, 1, [&](int from, int to) {
        const int rows = kBandRows * ss;
        std::vector<float> depth(size_t(bw) * rows);
        std::vector<uint32_t> px(size_t(bw) * rows);
        for (int band = from; band < to; ++band) {
            const int y0 = band * rows; // first sample row of the band
            const int bandRows = std::min(rows, box.height() * ss - y0);
            std::fill(depth.begin(), depth.begin() + ptrdiff_t(bw) * bandRows, 0.f);
            std::fill(px.begin(), px.begin() + ptrdiff_t(bw) * bandRows, 0u);
            for (int id : inBand[size_t(band)]) {
                const Tri& tr = tris[size_t(id)];
                const int ya = std::max(y0, int(std::ceil(tr.minY - 0.5f))),
                          yb = std::min(y0 + bandRows - 1, int(std::floor(tr.maxY - 0.5f)));
                for (int y = ya; y <= yb; ++y) {
                    const float yc = y + 0.5f;
                    float xl = 0, xr = float(bw);
                    bool empty = false;
                    for (int k = 0; k < 3; ++k) {
                        const float a = tr.e[k][0], rest = tr.e[k][1] * yc + tr.e[k][2];
                        if (a > 1e-12f) xl = std::max(xl, -rest / a - 1e-4f);
                        else if (a < -1e-12f) xr = std::min(xr, -rest / a + 1e-4f);
                        else if (rest < 0) empty = true;
                    }
                    if (empty) continue;
                    const int xa = std::max(0, int(std::ceil(xl - 0.5f))), xb = std::min(bw - 1, int(std::floor(xr - 0.5f)));
                    if (xa > xb) continue;
                    float* drow = depth.data() + size_t(y - y0) * bw;
                    uint32_t* prow = px.data() + size_t(y - y0) * bw;
                    float q = tr.q.at(xa + 0.5f, yc), r = tr.r.at(xa + 0.5f, yc), g = tr.g.at(xa + 0.5f, yc),
                          b = tr.b.at(xa + 0.5f, yc);
                    for (int x = xa; x <= xb; ++x) {
                        if (q > drow[x]) {
                            drow[x] = q;
                            const int ri = std::min(255, std::max(0, int(r + 0.5f))),
                                      gi = std::min(255, std::max(0, int(g + 0.5f))),
                                      bi = std::min(255, std::max(0, int(b + 0.5f)));
                            prow[x] = 0xff000000u | uint32_t(ri) | (uint32_t(gi) << 8) | (uint32_t(bi) << 16);
                        }
                        q += tr.q.a;
                        r += tr.r.a;
                        g += tr.g.a;
                        b += tr.b.a;
                    }
                }
            }
            // Resolve: the samples of each pixel -> coverage (alpha) and their mean colour
            const int n = ss * ss;
            for (int oy = 0; oy * ss < bandRows; ++oy) {
                uint8_t* out = rgba + (size_t(box.top() + band * kBandRows + oy) * w + box.left()) * 4;
                for (int ox = 0; ox < box.width(); ++ox, out += 4) {
                    uint32_t cnt = 0, sr = 0, sg = 0, sb = 0;
                    for (int j = 0; j < ss; ++j) {
                        const uint32_t* s = px.data() + size_t(oy * ss + j) * bw + size_t(ox) * ss;
                        for (int i = 0; i < ss; ++i) {
                            const uint32_t c = s[i];
                            if (!c) continue;
                            ++cnt;
                            sr += c & 0xff;
                            sg += (c >> 8) & 0xff;
                            sb += (c >> 16) & 0xff;
                        }
                    }
                    if (!cnt) continue;
                    const uint32_t r = (sr + cnt / 2) / cnt, g = (sg + cnt / 2) / cnt, b = (sb + cnt / 2) / cnt;
                    const uint32_t a = (cnt * 255 + uint32_t(n) / 2) / uint32_t(n);
                    const uint32_t da = out[3];
                    if (a == 255 || da == 0) {
                        out[0] = uint8_t(r);
                        out[1] = uint8_t(g);
                        out[2] = uint8_t(b);
                        out[3] = uint8_t(a);
                        continue;
                    }
                    // Over what is there (straight alpha): an edge over a box
                    const uint32_t rest = da * (255 - a); // / 255
                    const uint32_t ao = a * 255 + rest;   // alpha * 255
                    out[0] = uint8_t((r * a * 255 + out[0] * rest + ao / 2) / ao);
                    out[1] = uint8_t((g * a * 255 + out[1] * rest + ao / 2) / ao);
                    out[2] = uint8_t((b * a * 255 + out[2] * rest + ao / 2) / ao);
                    out[3] = uint8_t((ao + 127) / 255);
                }
            }
        }
    });
    return box;
}

} // namespace Text3d
