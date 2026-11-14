// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/RegionPick.h"

#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Masks.h"
#include "core/Project.h"
#include "core/ProjectFormat.h"
#include "core/TimelineOps.h"
#include "core/Types.h"
#include "engine/Tracker.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CompTrack.h"
#include "engine/video/Compose.h"

#include <QUndoStack>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <numbers>
#include <vector>

namespace RegionPick {

namespace PC = Picture::Comp;

namespace {

// ---- Color ----

struct Lab {
    float L = 0, a = 0, b = 0;
};

float srgbToLinear(int v)
{
    const double c = v / 255.0;
    return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}

double labF(double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16) / 116; }

Lab toLab(float r, float g, float b) // linear sRGB (D65) -> CIE Lab
{
    const double X = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
    const double Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double Z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
    const double fx = labF(X), fy = labF(Y), fz = labF(Z);
    return {float(116 * fy - 16), float(500 * (fx - fy)), float(200 * (fy - fz))};
}

// Lightness counts less: a shadow across a wall changes it more than the color
double dist(const Lab& p, const Lab& q)
{
    const double dl = 0.7 * (p.L - q.L), da = p.a - q.a, db = p.b - q.b;
    return std::sqrt(dl * dl + da * da + db * db);
}

// ---- Outline ----

double segDist(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double len2 = d.x() * d.x() + d.y() * d.y();
    double t = len2 > 0 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF q = a + t * d - p;
    return std::hypot(q.x(), q.y());
}

void douglasPeucker(const QVector<QPointF>& pts, int i, int j, double eps, std::vector<char>& keep)
{
    if (j <= i + 1) return;
    double worst = -1;
    int at = -1;
    for (int k = i + 1; k < j; ++k)
        if (const double d = segDist(pts[k], pts[i], pts[j]); d > worst) worst = d, at = k;
    if (worst <= eps) return;
    keep[size_t(at)] = 1;
    douglasPeucker(pts, i, at, eps, keep);
    douglasPeucker(pts, at, j, eps, keep);
}

// The closed loop simplified: start at a corner sure to stay (the top-left-most point), split at the point farthest
// from it, each half by Douglas-Peucker
QVector<QPointF> simplify(QVector<QPointF> loop, double eps)
{
    const int n = int(loop.size());
    if (n <= 3) return loop;
    int s = 0;
    for (int i = 1; i < n; ++i)
        if (loop[i].x() + loop[i].y() < loop[s].x() + loop[s].y()) s = i;
    std::rotate(loop.begin(), loop.begin() + s, loop.end());
    loop.append(loop.first());
    int far = 1;
    for (int i = 1; i < n; ++i)
        if (std::hypot(loop[i].x() - loop[0].x(), loop[i].y() - loop[0].y())
            > std::hypot(loop[far].x() - loop[0].x(), loop[far].y() - loop[0].y()))
            far = i;
    std::vector<char> keep(size_t(n + 1), 0);
    keep[0] = keep[size_t(far)] = keep[size_t(n)] = 1;
    douglasPeucker(loop, 0, far, eps, keep);
    douglasPeucker(loop, far, n, eps, keep);
    QVector<QPointF> out;
    for (int i = 0; i < n; ++i)
        if (keep[size_t(i)]) out.append(loop[i]);
    return out;
}

bool segmentsCross(QPointF a, QPointF b, QPointF c, QPointF d)
{
    const auto side = [](QPointF p, QPointF q, QPointF r) {
        return (q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x());
    };
    const double d1 = side(c, d, a), d2 = side(c, d, b), d3 = side(a, b, c), d4 = side(a, b, d);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

// Two sides of the closed polygon that are not neighbours cross (a bow-tie)
bool selfCrossing(const QVector<QPointF>& p)
{
    const int n = int(p.size());
    for (int i = 0; i < n; ++i)
        for (int j = i + 2; j < n; ++j) {
            if (i == 0 && j == n - 1) continue; // neighbours over the start
            if (segmentsCross(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n])) return true;
        }
    return false;
}

double signedArea(const QVector<QPointF>& p)
{
    double a = 0;
    for (int i = 0, n = int(p.size()); i < n; ++i) {
        const QPointF& u = p[i];
        const QPointF& v = p[(i + 1) % n];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return a / 2;
}

// The outline of the region (1 in `in`, w x h) along the pixel edges: every edge between a region pixel and one
// outside, directed clockwise around the region (the region on the right in a y-down picture), joined into loops
// (turning left first where two parts touch at a corner: one loop around both). The loop with the largest area
QVector<QPointF> outline(const std::vector<uint8_t>& in, int w, int h)
{
    const int vw = w + 1;
    static constexpr int kDx[4] = {1, 0, -1, 0}, kDy[4] = {0, 1, 0, -1}; // E S W N
    std::vector<uint8_t> out(size_t(vw) * (h + 1), 0);                    // bit d: an edge leaves the vertex in direction d
    const auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && in[size_t(y) * w + x]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!in[size_t(y) * w + x]) continue;
            if (!inside(x, y - 1)) out[size_t(y) * vw + x] |= 1 << 0;           // top: (x, y) -> (x+1, y)
            if (!inside(x + 1, y)) out[size_t(y) * vw + x + 1] |= 1 << 1;       // right: (x+1, y) -> (x+1, y+1)
            if (!inside(x, y + 1)) out[size_t(y + 1) * vw + x + 1] |= 1 << 2;   // bottom: (x+1, y+1) -> (x, y+1)
            if (!inside(x - 1, y)) out[size_t(y + 1) * vw + x] |= 1 << 3;       // left: (x, y+1) -> (x, y)
        }
    QVector<QPointF> best;
    double bestArea = 0;
    for (size_t v0 = 0; v0 < out.size(); ++v0) {
        while (out[v0]) {
            int d0 = 0;
            while (!(out[v0] & (1 << d0))) ++d0;
            QVector<QPointF> loop;
            int x = int(v0 % vw), y = int(v0 / vw), d = d0;
            for (size_t guard = 0; guard <= out.size() * 4; ++guard) {
                out[size_t(y) * vw + x] &= uint8_t(~(1 << d));
                loop.append(QPointF(x, y));
                x += kDx[d], y += kDy[d];
                const uint8_t here = out[size_t(y) * vw + x];
                int next = -1;
                for (int turn : {3, 0, 1}) // left, straight, right
                    if (here & (1 << ((d + turn) % 4))) {
                        next = (d + turn) % 4;
                        break;
                    }
                if (next < 0) break; // back at the start (its edge used)
                d = next;
            }
            // Corners only: drop points in the middle of a straight run
            QVector<QPointF> corners;
            const int n = int(loop.size());
            for (int i = 0; i < n; ++i) {
                const QPointF a = loop[(i + n - 1) % n], b = loop[i], c = loop[(i + 1) % n];
                if ((b.x() - a.x()) * (c.y() - b.y()) - (b.y() - a.y()) * (c.x() - b.x()) != 0) corners.append(b);
            }
            const double area = std::abs(signedArea(corners));
            if (area > bestArea) bestArea = area, best = corners;
        }
    }
    return best;
}

// 3 x 3 square: erode (min) or dilate (max); outside the picture counts as `edge`
std::vector<uint8_t> morph(const std::vector<uint8_t>& in, int w, int h, bool dilate, uint8_t edge)
{
    std::vector<uint8_t> out(in.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = dilate ? 0 : 1;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    const uint8_t q = xx < 0 || yy < 0 || xx >= w || yy >= h ? edge : in[size_t(yy) * w + xx];
                    v = dilate ? std::max(v, q) : std::min(v, q);
                }
            out[size_t(y) * w + x] = v;
        }
    return out;
}

// The 4-connected part of `in` holding (sx, sy); empty when that pixel is not set
std::vector<uint8_t> partAt(const std::vector<uint8_t>& in, int w, int h, int sx, int sy)
{
    std::vector<uint8_t> out(in.size(), 0);
    if (!in[size_t(sy) * w + sx]) return out;
    std::vector<int> stack{sy * w + sx};
    out[size_t(sy) * w + sx] = 1;
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % w, y = i / w;
        const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
        for (const auto& q : nb) {
            if (q[0] < 0 || q[1] < 0 || q[0] >= w || q[1] >= h) continue;
            const size_t j = size_t(q[1]) * w + q[0];
            if (in[j] && !out[j]) out[j] = 1, stack.push_back(int(j));
        }
    }
    return out;
}

// Holes filled: what is not reachable from the picture's border without crossing the region belongs to it
void fillHoles(std::vector<uint8_t>& r, int w, int h)
{
    std::vector<uint8_t> outside(r.size(), 0);
    std::vector<int> stack;
    const auto seed = [&](int x, int y) {
        const size_t i = size_t(y) * w + x;
        if (!r[i] && !outside[i]) outside[i] = 1, stack.push_back(int(i));
    };
    for (int x = 0; x < w; ++x) seed(x, 0), seed(x, h - 1);
    for (int y = 0; y < h; ++y) seed(0, y), seed(w - 1, y);
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % w, y = i / w;
        if (x + 1 < w) seed(x + 1, y);
        if (x > 0) seed(x - 1, y);
        if (y + 1 < h) seed(x, y + 1);
        if (y > 0) seed(x, y - 1);
    }
    for (size_t i = 0; i < r.size(); ++i)
        if (!outside[i]) r[i] = 1;
}

QImage working(const QImage& img, int maxEdge)
{
    QImage im = img.convertToFormat(QImage::Format_RGB32);
    const int edge = std::max(im.width(), im.height());
    if (maxEdge > 0 && edge > maxEdge)
        im = im.scaled(im.size() * (double(maxEdge) / edge), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return im;
}

} // namespace

Result pick(const QImage& img, QPointF point, const Options& o)
{
    Result r;
    if (img.isNull() || img.width() < 2 || img.height() < 2) {
        r.error = "no picture to pick from";
        return r;
    }
    if (!(point.x() >= 0 && point.x() <= 1 && point.y() >= 0 && point.y() <= 1)) {
        r.error = "the point must lie in the picture (x and y 0..1)";
        return r;
    }
    const QImage im = working(img, o.maxEdge);
    const int w = im.width(), h = im.height();
    const size_t n = size_t(w) * h;
    // Lab of every pixel, then a 3 x 3 box over it (noise and compression blocks)
    static const std::array<float, 256> lin = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) t[size_t(i)] = srgbToLinear(i);
        return t;
    }();
    std::vector<Lab> raw(n), lab(n);
    for (int y = 0; y < h; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(im.constScanLine(y));
        for (int x = 0; x < w; ++x)
            raw[size_t(y) * w + x] = toLab(lin[size_t(qRed(line[x]))], lin[size_t(qGreen(line[x]))], lin[size_t(qBlue(line[x]))]);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Lab s;
            int k = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = std::clamp(x + dx, 0, w - 1), yy = std::clamp(y + dy, 0, h - 1);
                    const Lab& q = raw[size_t(yy) * w + xx];
                    s.L += q.L, s.a += q.a, s.b += q.b, ++k;
                }
            lab[size_t(y) * w + x] = {s.L / k, s.a / k, s.b / k};
        }
    const int sx = std::clamp(int(point.x() * w), 0, w - 1), sy = std::clamp(int(point.y() * h), 0, h - 1);
    // The color looked for: the mean around the click (5 x 5)
    Lab seed;
    {
        int k = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int xx = std::clamp(sx + dx, 0, w - 1), yy = std::clamp(sy + dy, 0, h - 1);
                const Lab& q = lab[size_t(yy) * w + xx];
                seed.L += q.L, seed.a += q.a, seed.b += q.b, ++k;
            }
        seed = {seed.L / k, seed.a / k, seed.b / k};
    }
    const double tol = std::clamp(o.tolerance, 0.0, 1.0);
    const double far = 4 + 36 * tol;  // distance to the color looked for
    const double step = 3 + 12 * tol; // between neighbours: a sharper step is an edge
    std::vector<uint8_t> region(n, 0);
    {
        std::deque<int> queue{sy * w + sx};
        region[size_t(sy) * w + sx] = 1;
        while (!queue.empty()) {
            const int i = queue.front();
            queue.pop_front();
            const int x = i % w, y = i / w;
            const Lab& p = lab[size_t(i)];
            const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            for (const auto& q : nb) {
                if (q[0] < 0 || q[1] < 0 || q[0] >= w || q[1] >= h) continue;
                const size_t j = size_t(q[1]) * w + q[0];
                if (region[j]) continue;
                const double ds = dist(lab[j], seed), dn = dist(lab[j], p);
                // Near the color and no edge between them. (A rule for gentle shading further away let real walls run
                // out into ground of the same sand color: a CS2 clip went from 2 % to 69 % of the picture.)
                if (ds <= far && dn <= step) {
                    region[j] = 1;
                    queue.push_back(int(j));
                }
            }
        }
    }
    // Thin leaks (a one-pixel bridge into the next surface) away: open with a 3 x 3 square, keep the clicked part;
    // a square keeps straight edges and corners where they are
    {
        std::vector<uint8_t> opened = partAt(morph(morph(region, w, h, false, 1), w, h, true, 0), w, h, sx, sy);
        if (std::count(opened.begin(), opened.end(), 1) > 16) region = std::move(opened);
    }
    fillHoles(region, w, h);
    const size_t count = size_t(std::count(region.begin(), region.end(), 1));
    r.area = double(count) / n;
    for (int x = 0; x < w && !r.touchesEdge; ++x) r.touchesEdge = region[size_t(x)] || region[size_t(h - 1) * w + x];
    for (int y = 0; y < h && !r.touchesEdge; ++y) r.touchesEdge = region[size_t(y) * w] || region[size_t(y) * w + w - 1];
    QVector<QPointF> loop = outline(region, w, h);
    if (loop.size() < 3) {
        r.error = "nothing picked: the region is too small (raise the tolerance)";
        return r;
    }
    const int maxPoints = std::clamp(o.maxPoints, 3, Masks::kMaxPoints);
    // The coarsest outline with at most maxPoints corners whose sides do not cross (the two halves are simplified
    // apart and could cross in a thin U); crossing there: the finest one without crossing that a mask still takes
    QVector<QPointF> poly, uncrossed;
    for (double eps = 0.7; eps < 1e5; eps *= 1.25) {
        poly = simplify(loop, eps);
        const bool crossed = selfCrossing(poly);
        if (!crossed && poly.size() <= Masks::kMaxPoints) uncrossed = poly;
        if (poly.size() <= maxPoints) {
            if (crossed && !uncrossed.isEmpty()) poly = uncrossed;
            break;
        }
    }
    if (signedArea(poly) < 0) std::reverse(poly.begin(), poly.end()); // clockwise on screen
    for (QPointF& p : poly) p = QPointF(p.x() / w, p.y() / h);
    r.polygon = poly;
    if (r.area > 0.6)
        r.warning = "the region spread over most of the picture: lower the tolerance or click nearer the middle of the surface";
    else if (count < 64)
        r.warning = "very small region: raise the tolerance or click on a larger even surface";
    return r;
}

QImage clipPicture(const ProjectFormat& format, const Clip& c, int frame, int maxEdge)
{
    if (c.isNested() || c.isTitle()) return {};
    const auto source = Tracker::clipPictures(format, c);
    return source(std::clamp(frame, 0, std::max(0, c.length() - 1)), std::max(maxEdge, 64));
}

Result pickClip(const ProjectFormat& format, const Clip& c, int frame, QPointF point, const Options& o)
{
    if (c.isNested() || c.isTitle()) {
        Result r;
        r.error = "picking works on video clips only (not on titles, compound or multicam clips)";
        return r;
    }
    return pick(clipPicture(format, c, frame, o.maxEdge), point, o);
}

QPointF trackablePoint(const QImage& img, const QVector<QPointF>& polygon, double side)
{
    if (polygon.isEmpty()) return {0.5, 0.5};
    const QImage g = working(img, 640);
    if (g.isNull()) return polygon.first();
    const int w = g.width(), h = g.height();
    const int r = std::max(2, int(side * w / 2));
    const auto gray = [&](int x, int y) {
        const QRgb p = reinterpret_cast<const QRgb*>(g.constScanLine(std::clamp(y, 0, h - 1)))[std::clamp(x, 0, w - 1)];
        return 0.299 * qRed(p) + 0.587 * qGreen(p) + 0.114 * qBlue(p);
    };
    // Smaller eigenvalue of the structure tensor (Shi-Tomasi): high only where it moves trackably in both directions
    const auto score = [&](QPointF at) {
        const int cx = int(at.x() * w), cy = int(at.y() * h);
        double xx = 0, yy = 0, xy = 0;
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) {
                const double gx = gray(x + 1, y) - gray(x - 1, y), gy = gray(x, y + 1) - gray(x, y - 1);
                xx += gx * gx, yy += gy * gy, xy += gx * gy;
            }
        const double tr = xx + yy, det = xx * yy - xy * xy;
        return tr / 2 - std::sqrt(std::max(0.0, tr * tr / 4 - det));
    };
    // Candidates: the corners and the middles of the sides, each moved inward by half the square's half side - three
    // quarters of the square on the surface (it moves with it), the outline still in it (an even surface has no
    // structure of its own; a square half on the background would follow the background's motion as much)
    QPolygonF poly;
    for (const QPointF& p : polygon) poly << QPointF(p.x() * w, p.y() * h);
    const int n = int(poly.size());
    const auto inward = [&](QPointF d) { // the side's normal into the polygon (either orientation)
        const double len = std::hypot(d.x(), d.y());
        return len > 0 ? QPointF(-d.y() / len, d.x() / len) : QPointF();
    };
    double area = 0;
    for (int i = 0; i < n; ++i) area += poly[i].x() * poly[(i + 1) % n].y() - poly[(i + 1) % n].x() * poly[i].y();
    const double sign = area >= 0 ? 1 : -1; // y down: positive = clockwise on screen, the inside on the right
    const double shift = r / 2.0;
    const auto fits = [&](QPointF p) { return p.x() >= r && p.y() >= r && p.x() <= w - 1 - r && p.y() <= h - 1 - r; };
    QPointF best(-1, -1);
    double bestScore = -1;
    for (int i = 0; i < n; ++i) {
        const QPointF a = poly[(i + n - 1) % n], b = poly[i], c = poly[(i + 1) % n];
        const QPointF nb = inward(b - a) + inward(c - b), nc = inward(c - b);
        const double nbl = std::hypot(nb.x(), nb.y());
        const QPointF cands[2] = {b + (nbl > 0 ? nb / nbl : QPointF()) * sign * shift, (b + c) / 2 + nc * sign * shift};
        for (const QPointF& p : cands) {
            if (!fits(p) || !poly.containsPoint(p, Qt::OddEvenFill)) continue;
            if (const double sc = score(QPointF(p.x() / w, p.y() / h)); sc > bestScore) bestScore = sc, best = p;
        }
    }
    if (bestScore < 0) { // nothing fits inside (a sliver at the border): the most textured point of the outline in the picture
        for (const QPointF& p : poly)
            if (fits(p))
                if (const double sc = score(QPointF(p.x() / w, p.y() / h)); sc > bestScore) bestScore = sc, best = p;
    }
    if (bestScore < 0) { // the middle of the polygon's corners, kept inside the picture
        QPointF m;
        for (const QPointF& p : poly) m += p / n;
        best = QPointF(std::clamp(m.x(), double(r), double(w - 1 - r)), std::clamp(m.y(), double(r), double(h - 1 - r)));
    }
    return QPointF(best.x() / w, best.y() / h);
}

bool combineFromId(const QString& id, Combine* out)
{
    Combine c;
    if (id.isEmpty() || id == QLatin1String("replace")) c = Combine::Replace;
    else if (id == QLatin1String("add")) c = Combine::Add;
    else if (id == QLatin1String("subtract")) c = Combine::Subtract;
    else return false;
    if (out) *out = c;
    return true;
}

namespace {

QVariantMap nodeValues(const Clip& c, int id)
{
    QVariantMap out;
    const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
    if (!e) return out;
    for (auto it = e->params.begin(); it != e->params.end(); ++it) {
        int node = 0;
        QString key;
        if (CompGraph::splitParamKey(it.key(), &node, &key) && node == id) out.insert(key, it.value());
    }
    return out;
}

AnimParam paramOf(int node, const QString& key) { return Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(node, key)); }

// A node's number at clip frame t (keyframes win)
double numberAt(const Clip& c, int node, const PC::NodeType& k, const QVariantMap& values, const QString& key, double t)
{
    if (Keys::animated(c, paramOf(node, key))) return Keys::valueAt(c, paramOf(node, key), t);
    return PC::numValue(k, values, key);
}

// The point in the tracker's reference that the follower draws at `p` at clip frame t (pixels of a W x H picture):
// the inverse of the follow rule, or of the four-corner homography. false = no inverse (a degenerate quad)
bool toReference(const Clip& c, int tracker, double t, double fps, double W, double H, QVector<QPointF>& pts)
{
    const PC::NodeType* k = PC::find(QString::fromLatin1(::Comp::kTrackerType));
    if (!k) return false;
    const QVariantMap values = nodeValues(c, tracker);
    const QString mode = PC::textValue(*k, values, QStringLiteral("mode"));
    const ::Comp::FollowTime ft = PC::followTime(&c, tracker, values, t, fps);
    const auto value = [&](const QString& key) {
        return ::Comp::followValue(
            key, mode, [&](const QString& q) { return values.contains(q) || Keys::animated(c, paramOf(tracker, q)); },
            [&](const QString& q) { return numberAt(c, tracker, *k, values, q, t); }, &ft);
    };
    if (value(QString::fromLatin1(::Comp::kQuadKey)) > 0.5) {
        Compose::Homography h, inv;
        if (!PC::quadMap(value, W, H, &h) || !Compose::invert(h, &inv)) return false;
        for (QPointF& p : pts) {
            double x = 0, y = 0;
            if (!Compose::mapPoint(inv, p.x() * W, p.y() * H, &x, &y)) return false;
            p = QPointF(x / W, y / H);
        }
        return true;
    }
    double s = value(QStringLiteral("scale"));
    s = s > 0 && std::isfinite(s) ? s : 1.0;
    double a = value(QStringLiteral("angle"));
    a = std::isfinite(a) ? a : 0.0;
    const QPointF from(value(QStringLiteral("rx")) * W, value(QStringLiteral("ry")) * H);
    const QPointF to(value(QStringLiteral("x")) * W, value(QStringLiteral("y")) * H);
    const double rad = -a * std::numbers::pi / 180, cs = std::cos(rad), sn = std::sin(rad);
    for (QPointF& p : pts) { // p = to + s R(a) (q - from)  =>  q = from + R(-a) (p - to) / s
        const QPointF d = (QPointF(p.x() * W, p.y() * H) - to) / s;
        const QPointF q = from + QPointF(cs * d.x() - sn * d.y(), sn * d.x() + cs * d.y());
        p = QPointF(q.x() / W, q.y() / H);
    }
    return true;
}

} // namespace

int chainEnd(const Clip& c, int node)
{
    CompGraph g;
    const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
    if (!e || !::Comp::graphOf(*e, &g)) return node;
    for (int guard = 0; guard < 64; ++guard) {
        int next = 0;
        for (const CompLink& l : g.links)
            if (l.from == node && l.input == QLatin1String("mask"))
                if (const CompNode* to = g.node(l.to); to && to->type == QLatin1String("mask")) next = to->id;
        if (!next) break;
        node = next;
    }
    return node;
}

int applyPick(Editor& editor, const ProjectFormat& format, int clipId, int node, Combine mode,
              const QVector<QPointF>& polygon, int at, const QImage* picture, QPointF* trackerPoint, QString* err)
{
    const auto failed = [err](const QString& why) {
        if (err) *err = why;
        return 0;
    };
    const Clip* c = TimelineOps::findClip(editor.project()->timeline(), clipId);
    if (!c) return failed(QString("no clip %1").arg(clipId));
    CompGraph g;
    const EffectInstance* e = EffectRegistry::instance(*c, ::Comp::kEffectId);
    if (!e || !::Comp::graphOf(*e, &g)) return failed(QString("clip %1 has no compositing graph").arg(clipId));
    const CompNode* n = g.node(node);
    if (!n || n->type != QLatin1String("mask")) return failed(QString("node %1 is no mask node").arg(node));
    if (polygon.size() < 3 || polygon.size() > Masks::kMaxPoints)
        return failed(QString("a polygon has 3 to %1 corners").arg(Masks::kMaxPoints));
    const PC::NodeType* mk = PC::find(QStringLiteral("mask"));
    const QVariantMap own = nodeValues(*c, node);
    const int t = std::clamp(at - c->start, 0, std::max(0, c->length() - 1));
    int follow = ::Comp::followId(numberAt(*c, node, *mk, own, QStringLiteral("follow"), t));
    if (follow > 0 && !(g.node(follow) && g.node(follow)->type == QLatin1String(::Comp::kTrackerType))) follow = 0;
    QVector<QPointF> pts = polygon;
    if (follow > 0) {
        const QRectF pic = Tracker::pictureRect(format, *c);
        if (!toReference(*c, follow, t, format.rate.fps(), std::max(1.0, pic.width()), std::max(1.0, pic.height()), pts))
            return failed(QString("tracker %1's corners make no quad at that frame: pick where it is tracked well").arg(follow));
    }
    // A fresh point tracker: never analyzed, its point never set (a recipe's default)
    bool place = false;
    if (follow > 0 && picture && !picture->isNull() && mode == Combine::Replace) {
        const PC::NodeType* tk = PC::find(QString::fromLatin1(::Comp::kTrackerType));
        const QVariantMap tv = nodeValues(*c, follow);
        place = tk && PC::textValue(*tk, tv, QStringLiteral("mode")) == QLatin1String("point") && !tv.contains("ax")
                && !Keys::animated(*c, paramOf(follow, QStringLiteral("ax")))
                && !Keys::animated(*c, paramOf(follow, QStringLiteral("x")));
    }
    QVariantMap values{{"shape", "polygon"}, {"points", int(pts.size())}};
    for (int i = 0; i < pts.size(); ++i) {
        values[QStringLiteral("p%1x").arg(i)] = std::round(pts[i].x() * 1e5) / 1e5;
        values[QStringLiteral("p%1y").arg(i)] = std::round(pts[i].y() * 1e5) / 1e5;
    }
    QString why;
    const CompGraph::KindInfo kinds = [](const QString& type, QStringList* inputs) { return PC::kindInfo(type, inputs); };
    QUndoStack* undo = editor.project()->undoStack();
    undo->beginMacro(T("Fläche wählen"));
    struct End {
        QUndoStack* u;
        ~End() { u->endMacro(); }
    } end{undo};
    // Failed after the mask changed: its values back (inside the macro, so it also works within a caller's macro)
    const QVector<EffectInstance> effectsBefore = c->effects;
    const auto keysBefore = c->keys;
    const auto rollback = [&] {
        editor.modifyClips({clipId}, T("Fläche wählen"), [&](Clip& x) {
            x.effects = effectsBefore;
            x.keys = keysBefore;
        });
    };
    int id = node;
    if (mode == Combine::Replace) {
        values["rotate"] = 0.0; // the corners are where they were picked
        if (!editor.setCompNodeValuesAt(clipId, node, values, at, {}, &why))
            return failed(why.isEmpty() ? QString("could not set the mask") : why);
    } else {
        values["combine"] = mode == Combine::Add ? "add" : "subtract";
        if (follow > 0) values["follow"] = follow;
        if (const double soft = numberAt(*c, node, *mk, own, QStringLiteral("soft"), t); soft > 0) values["soft"] = soft;
        id = editor.addCompNode(clipId, QStringLiteral("mask"), values, chainEnd(*c, node), QStringLiteral("mask"), kinds, &why);
        if (!id) return failed(why.isEmpty() ? QString("could not add the mask") : why);
    }
    if (place) { // the middle of an even wall has nothing to follow: its edge has
        const Clip* now = TimelineOps::findClip(editor.project()->timeline(), clipId);
        const PC::NodeType* tk = PC::find(QString::fromLatin1(::Comp::kTrackerType));
        const double side = now && tk ? PC::numValue(*tk, nodeValues(*now, follow), QStringLiteral("size")) : 0.05;
        const QPointF tp = trackablePoint(*picture, polygon, side);
        const QVariantMap v{{"ax", std::round(tp.x() * 1e4) / 1e4}, {"ay", std::round(tp.y() * 1e4) / 1e4}};
        if (!editor.setCompNodeValues(clipId, follow, v, &why)) {
            rollback();
            return failed(why.isEmpty() ? QString("could not move the tracker") : why);
        }
        if (trackerPoint) *trackerPoint = QPointF(v["ax"].toDouble(), v["ay"].toDouble());
    }
    return id;
}

} // namespace RegionPick
