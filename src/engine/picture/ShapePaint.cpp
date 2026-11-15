// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/ShapePaint.h"

#include "engine/MaskRaster.h"
#include "engine/VideoEffects.h"
#include "engine/video/Compose.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace Picture::ShapePaint {

namespace {

QRectF pointBounds(const QVector<QPointF>& pts)
{
    if (pts.isEmpty()) return {};
    double x0 = pts[0].x(), x1 = x0, y0 = pts[0].y(), y1 = y0;
    for (const QPointF& p : pts) {
        x0 = std::min(x0, p.x());
        x1 = std::max(x1, p.x());
        y0 = std::min(y0, p.y());
        y1 = std::max(y1, p.y());
    }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

Compose::Affine shift(double x, double y)
{
    Compose::Affine m;
    m.c = x;
    m.f = y;
    return m;
}

// The stroke along `pts` (a closed outline drawn whole is a closed polygon: no seam at its start)
void strokePath(QPainter& p, const QVector<QPointF>& pts, bool closed, double reveal, const QPen& pen)
{
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    if (closed && reveal >= 1) {
        p.drawPolygon(QPolygonF(pts));
        return;
    }
    const QVector<QPointF> part = Shapes::partial(pts, closed, reveal);
    if (part.size() >= 2) p.drawPolyline(QPolygonF(part));
}

// rgb of every pixel set to `color`, alpha times `opacity`; then blurred by sigma (a drop shadow of a picture). Only
// the alpha is blurred (16-bit, the values VideoEffects::gaussian gives that channel): the colour is the same
// everywhere, so blurring it too only cost time (four channels instead of one)
void shadowOf(uint8_t* rgba, int w, int h, const QColor& color, double opacity, double sigma)
{
    const uint8_t r = uint8_t(color.red()), g = uint8_t(color.green()), b = uint8_t(color.blue());
    const double k = std::clamp(opacity, 0.0, 1.0) * color.alphaF();
    uint8_t times[256];
    for (int v = 0; v < 256; ++v) times[v] = uint8_t(std::lround(v * k));
    const size_t n = size_t(w) * h;
    if (sigma < 0.5) { // VideoEffects::gaussian blurs nothing below half a pixel
        for (size_t i = 0; i < n; ++i) {
            uint8_t* px = rgba + i * 4;
            px[0] = r;
            px[1] = g;
            px[2] = b;
            px[3] = times[px[3]];
        }
        return;
    }
    std::vector<uint16_t> alpha(n);
    for (size_t i = 0; i < n; ++i) alpha[i] = uint16_t(times[rgba[i * 4 + 3]] * 257);
    VideoEffects::gaussianPlane(alpha.data(), w, h, sigma);
    Compose::forRows(h, 64, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * w, end = size_t(y1) * w; i < end; ++i) {
            uint8_t* px = rgba + i * 4;
            const uint8_t a = uint8_t((alpha[i] + 128) / 257);
            px[0] = a ? r : 0;
            px[1] = a ? g : 0;
            px[2] = a ? b : 0;
            px[3] = a;
        }
    });
}

// dst = dst + (src - dst) * weight / 255 (all four channels), weight = mask times k
inline void lerp(uint8_t* dst, const uint8_t* src, int weight)
{
    for (int c = 0; c < 4; ++c) {
        const int d = (int(src[c]) - int(dst[c])) * weight; // rounded to the nearest on both sides of zero
        dst[c] = uint8_t(dst[c] + (d >= 0 ? (d + 127) / 255 : -((127 - d) / 255)));
    }
}

// A blurred copy of the region `reg` of a picture (w pixels wide) with sigma: at full size for a small blur; for a
// larger one at 1/d of the size (d x d blocks averaged, blurred with what is left of sigma, read back bilinearly):
// the same picture to the eye at a fraction of the cost (a big blur shape on a 1080p frame: ~40 ms -> a few)
struct Blurred {
    int d = 1, w = 0, h = 0; // scale, size of the (small) picture
    std::vector<uint8_t> px;
    std::vector<int> xs;     // d > 1: for each region column, the left small column (<< 8) + weight of the right one
    std::vector<int> ys;
    // Region row y, columns x .. x + n - 1 as RGBA into out (tmp: room for one small row, b.w pixels)
    void row(int y, int x, int n, uint8_t* out, std::vector<uint8_t>& tmp) const
    {
        if (d == 1) {
            std::memcpy(out, px.data() + (size_t(y) * w + x) * 4, size_t(n) * 4);
            return;
        }
        // Between the two small rows first (once per small pixel), then along the row
        const int sy = ys[size_t(y)] >> 8, fy = ys[size_t(y)] & 255;
        const uint8_t* r0 = px.data() + size_t(sy) * w * 4;
        const uint8_t* r1 = px.data() + size_t(std::min(sy + 1, h - 1)) * w * 4;
        const int s0 = xs[size_t(x)] >> 8, s1 = std::min(w - 1, (xs[size_t(x + n - 1)] >> 8) + 1);
        tmp.resize(size_t(w) * 4);
        for (int i = s0 * 4; i < (s1 + 1) * 4; ++i) tmp[size_t(i)] = uint8_t((r0[i] * (256 - fy) + r1[i] * fy + 128) >> 8);
        for (int i = 0; i < n; ++i) {
            const int sx = xs[size_t(x + i)] >> 8, fx = xs[size_t(x + i)] & 255;
            const uint8_t* a = tmp.data() + sx * 4;
            const uint8_t* b = tmp.data() + std::min(sx + 1, w - 1) * 4;
            for (int k = 0; k < 4; ++k) out[i * 4 + k] = uint8_t((a[k] * (256 - fx) + b[k] * fx + 128) >> 8);
        }
    }
};

Blurred blurredRegion(const uint8_t* rgba, int w, const QRect& reg, double sigma)
{
    Blurred b;
    // 1/d of the size leaves at least ~3 small pixels of sigma (below that the blocks would show)
    b.d = std::max(1, int(sigma / 3));
    const int d = b.d;
    if (d == 1) {
        b.w = reg.width();
        b.h = reg.height();
        b.px.resize(size_t(b.w) * b.h * 4);
        for (int y = 0; y < b.h; ++y)
            std::memcpy(b.px.data() + size_t(y) * b.w * 4, rgba + (size_t(reg.top() + y) * w + reg.left()) * 4, size_t(b.w) * 4);
        VideoEffects::gaussian(b.px.data(), b.w, b.h, sigma);
        return b;
    }
    b.w = (reg.width() + d - 1) / d;
    b.h = (reg.height() + d - 1) / d;
    b.px.resize(size_t(b.w) * b.h * 4);
    Compose::forRows(b.h, 8, [&](int y0, int y1) {
        const size_t line = size_t(reg.width()) * 4;
        std::vector<uint32_t> col(line); // the block's rows added up (a plain loop over the row: vectorized)
        for (int sy = y0; sy < y1; ++sy) {
            std::fill(col.begin(), col.end(), 0u);
            const int top = sy * d, bottom = std::min(reg.height(), top + d);
            for (int y = top; y < bottom; ++y) {
                const uint8_t* __restrict src = rgba + (size_t(reg.top() + y) * w + reg.left()) * 4;
                uint32_t* __restrict acc = col.data();
                for (size_t i = 0; i < line; ++i) acc[i] += src[i];
            }
            uint8_t* dst = b.px.data() + size_t(sy) * b.w * 4;
            for (int sx = 0; sx < b.w; ++sx) {
                const int x0 = sx * d, x1 = std::min(reg.width(), x0 + d);
                uint32_t sum[4] = {0, 0, 0, 0};
                for (int x = x0; x < x1; ++x)
                    for (int k = 0; k < 4; ++k) sum[k] += col[size_t(x) * 4 + k];
                const uint32_t n = uint32_t(bottom - top) * uint32_t(x1 - x0);
                for (int k = 0; k < 4; ++k) dst[sx * 4 + k] = uint8_t((sum[k] + n / 2) / n);
            }
        }
    });
    // Averaging d x d blocks and reading back bilinearly blur by themselves (variances (d^2 - 1) / 12 and d^2 / 6 per
    // axis, in full pixels): the small picture gets the rest
    const double rest = sigma * sigma - (d * d - 1) / 12.0 - d * d / 6.0;
    if (rest > 0) VideoEffects::gaussian(b.px.data(), b.w, b.h, std::sqrt(rest) / d);
    const auto map = [d](int n, int limit, std::vector<int>& out) {
        out.resize(size_t(n));
        for (int i = 0; i < n; ++i) {
            const double u = std::clamp((i + 0.5) / d - 0.5, 0.0, double(limit - 1));
            int s = int(u), f = int(std::lround((u - s) * 256));
            if (f >= 256) ++s, f = 0; // rounds up to the next small pixel (never past the last: u <= limit - 1)
            out[size_t(i)] = (s << 8) | f;
        }
    };
    map(reg.width(), b.w, b.xs);
    map(reg.height(), b.h, b.ys);
    return b;
}

} // namespace

QRect coverage(const QVector<QPointF>& points, double soft, int w, int h, uint8_t* plane)
{
    const Masks::Geometry g = Shapes::geometry(points, soft);
    if (g.empty || w <= 0 || h <= 0) return {};
    return MaskRaster::render(g, w, h, plane, true);
}

namespace {

// A shape title painted (its pixels and its shadow, straight RGBA) for one set of inputs. Kept while they stay the
// same: a shape that does not move or change is painted once, not on every frame (the drop shadow's blur was most of
// a big shape's cost)
struct Painted {
    // The inputs
    QVector<QPointF> points;
    bool closed = true;
    ShapeStyle shape;
    bool outlineOn = false;
    double outlineWidth = 0;
    QColor outlineColor;
    bool shadowOn = false;
    double shadowX = 0, shadowY = 0, shadowSoft = 0, shadowOpacity = 0;
    QColor shadowColor;
    double reveal = 1, scale = 1;
    QColor fill;
    QPoint origin;
    int w = 0, h = 0;
    // The result: nothing drawn (an empty shape), or the shape's pixels at `box` (output pixels relative to origin)
    // and its shadow at `shadowShift` from there
    bool empty = true;
    QRect box;
    QImage straight, shadow;
    QPointF shadowShift;

    bool same(const Shapes::Outline& o, const TitleStyle& t, double rv, double sc, const QColor& f, QPoint org, int ww,
              int hh) const
    {
        const TitleLook& l = t.look;
        return w == ww && h == hh && origin == org && reveal == rv && scale == sc && fill == f && closed == o.closed
               && outlineOn == t.outlineOn && outlineWidth == t.outlineWidth && outlineColor == t.outlineColor
               && shadowOn == l.shadowOn && (!shadowOn || (shadowX == l.shadowX && shadowY == l.shadowY
                                                           && shadowSoft == l.shadowSoft && shadowOpacity == l.shadowOpacity
                                                           && shadowColor == l.shadowColor))
               && shape == t.shape && points == o.points;
    }
    size_t bytes() const { return size_t(straight.sizeInBytes() + shadow.sizeInBytes()) + size_t(points.size()) * sizeof(QPointF); }
};

std::shared_ptr<const Painted> paintShape(const Shapes::Outline& o, const TitleStyle& t, double reveal, double scale,
                                          const QColor& fill, QPoint origin, int w, int h)
{
    auto out = std::make_shared<Painted>();
    Painted& r = *out;
    r.points = o.points;
    r.closed = o.closed;
    r.shape = t.shape;
    r.outlineOn = t.outlineOn;
    r.outlineWidth = t.outlineWidth;
    r.outlineColor = t.outlineColor;
    r.shadowOn = t.look.shadowOn;
    r.shadowX = t.look.shadowX;
    r.shadowY = t.look.shadowY;
    r.shadowSoft = t.look.shadowSoft;
    r.shadowOpacity = t.look.shadowOpacity;
    r.shadowColor = t.look.shadowColor;
    r.reveal = reveal;
    r.scale = scale;
    r.fill = fill;
    r.origin = origin;
    r.w = w;
    r.h = h;
    const ShapeStyle& s = t.shape;
    const bool line = s.kind == ShapeKind::Line;
    const double lineWidth = line ? std::max(0.0, s.height) * scale : 0.0;
    const double outline = t.outlineOn ? std::max(0.0, t.outlineWidth) * scale : 0.0;
    const double feather = std::max(0.0, s.feather) * scale;
    const double sigma = t.look.shadowOn ? std::max(0.0, t.look.shadowSoft) * scale / 2 : 0.0;
    const QPointF shadowAt = t.look.shadowOn ? QPointF(t.look.shadowX, t.look.shadowY) * scale : QPointF();
    // Nothing to draw (a blur or pixelate shape without a stroke: `below` did its part): no pixels, no shadow of them
    const bool fills = !line && o.closed && fill.alpha() > 0;
    const bool strokes = line ? (outline > 0 || (lineWidth > 0 && fill.alpha() > 0)) : (outline > 0 && reveal > 0);
    if (!fills && !strokes) return out;
    // The shape's own pixels (with room for its stroke, soft edge and the blur of its shadow), only where they can
    // reach the picture drawn on
    const double margin = std::max({feather, outline / 2 + lineWidth / 2 + (line ? outline : 0.0), 1.0}) + 2 + 3 * sigma;
    QRectF reach(origin, QSizeF(w, h));
    reach = reach.adjusted(-std::abs(shadowAt.x()) - 3 * sigma, -std::abs(shadowAt.y()) - 3 * sigma,
                           std::abs(shadowAt.x()) + 3 * sigma, std::abs(shadowAt.y()) + 3 * sigma);
    const QRect box = pointBounds(o.points).adjusted(-margin, -margin, margin, margin).intersected(reach).toAlignedRect();
    if (box.isEmpty() || box.width() > 16384 || box.height() > 16384) return out;
    QVector<QPointF> pts = o.points;
    for (QPointF& p : pts) p -= box.topLeft();
    const int bw = box.width(), bh = box.height();
    QImage img(bw, bh, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    // Fill: the outline's coverage with its soft edge, in the fill colour
    if (fills) {
        std::vector<uint8_t> plane(size_t(bw) * bh, 0);
        const QRect area = coverage(pts, feather, bw, bh, plane.data());
        const double a = fill.alphaF();
        const int pr = int(std::lround(fill.red() * a)), pg = int(std::lround(fill.green() * a)),
                  pb = int(std::lround(fill.blue() * a)), pa = fill.alpha();
        for (int y = area.top(); y <= area.bottom() && !area.isEmpty(); ++y) {
            uint8_t* row = img.scanLine(y);
            const uint8_t* m = plane.data() + size_t(y) * bw;
            for (int x = area.left(); x <= area.right(); ++x) {
                const int v = m[x];
                if (!v) continue;
                uint8_t* px = row + x * 4;
                px[0] = uint8_t((pr * v + 127) / 255);
                px[1] = uint8_t((pg * v + 127) / 255);
                px[2] = uint8_t((pb * v + 127) / 255);
                px[3] = uint8_t((pa * v + 127) / 255);
            }
        }
    }
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        if (line) { // a line is its stroke: the outline (when on) around it, then the line in the fill colour
            if (outline > 0)
                strokePath(p, pts, false, reveal, QPen(t.outlineColor, lineWidth + 2 * outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (lineWidth > 0 && fill.alpha() > 0)
                strokePath(p, pts, false, reveal, QPen(fill, lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        } else if (outline > 0 && reveal > 0) {
            strokePath(p, pts, o.closed, reveal, QPen(t.outlineColor, outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
    }
    QImage straight = img.convertToFormat(QImage::Format_RGBA8888);
    if (straight.bytesPerLine() != bw * 4) return out; // always packed for RGBA8888 (4-byte pixels)
    r.empty = false;
    r.box = box.translated(-origin);
    if (t.look.shadowOn && t.look.shadowOpacity > 0) {
        r.shadow = straight.copy();
        shadowOf(r.shadow.bits(), bw, bh, t.look.shadowColor, t.look.shadowOpacity, sigma);
        r.shadowShift = shadowAt;
    }
    r.straight = std::move(straight);
    return out;
}

// The last shapes painted (all render threads): a few, within a byte budget
class PaintCache {
public:
    std::shared_ptr<const Painted> get(const Shapes::Outline& o, const TitleStyle& t, double reveal, double scale,
                                       const QColor& fill, QPoint origin, int w, int h)
    {
        {
            std::lock_guard lock(m_mutex);
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
                if ((*it)->same(o, t, reveal, scale, fill, origin, w, h)) {
                    std::shared_ptr<const Painted> hit = *it;
                    m_entries.erase(it);
                    m_entries.insert(m_entries.begin(), hit); // most recent first
                    return hit;
                }
        }
        std::shared_ptr<const Painted> made = paintShape(o, t, reveal, scale, fill, origin, w, h);
        std::lock_guard lock(m_mutex);
        m_entries.insert(m_entries.begin(), made);
        size_t total = 0;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            total += m_entries[i]->bytes();
            if (i > 0 && (i >= kEntries || total > kBudget)) {
                m_entries.resize(i);
                break;
            }
        }
        return made;
    }

private:
    static constexpr size_t kEntries = 8;               // shapes on screen at once, each painted once
    static constexpr size_t kBudget = 64ull * 1024 * 1024; // ~4 full-frame 1080p shapes with their shadows
    std::mutex m_mutex;
    std::vector<std::shared_ptr<const Painted>> m_entries;
};

PaintCache& paintCache()
{
    static auto* cache = new PaintCache; // never destroyed: frames may still render while statics are torn down
    return *cache;
}

} // namespace

void paint(uint8_t* rgba, int w, int h, QPoint origin, const Shapes::Outline& o, const TitleStyle& t, double reveal,
           double scale, double opacity, const QColor& fill)
{
    if (o.points.isEmpty() || opacity <= 0 || w <= 0 || h <= 0) return;
    const std::shared_ptr<const Painted> p = paintCache().get(o, t, reveal, scale, fill, origin, w, h);
    if (p->empty) return;
    const QRect& box = p->box;
    const int bw = box.width(), bh = box.height();
    if (!p->shadow.isNull())
        Compose::draw(rgba, w, h, p->shadow.constBits(), bw, bh,
                      shift(box.x() + p->shadowShift.x(), box.y() + p->shadowShift.y()), opacity);
    Compose::draw(rgba, w, h, p->straight.constBits(), bw, bh, shift(box.x(), box.y()), opacity);
}

void below(uint8_t* rgba, int w, int h, const Shapes::Outline& o, const ShapeStyle& s, double scale, double k)
{
    if (w <= 0 || h <= 0 || k <= 0 || !o.closed || s.kind == ShapeKind::Line) return;
    k = std::min(1.0, k);
    std::vector<uint8_t> plane(size_t(w) * h, 0);
    const QRect area = coverage(o.points, std::max(0.0, s.feather) * scale, w, h, plane.data());
    const int K = int(std::lround(k * 255));
    if (!area.isEmpty() && s.fill == ShapeFill::Blur && s.amount > 0) {
        // Strength as the blur effect: 100 = 1/20 of the frame height
        const double sigma = s.amount * h / 2000.0;
        const int R = int(std::ceil(3 * sigma)) + 1;
        const QRect reg = area.adjusted(-R, -R, R, R).intersected(QRect(0, 0, w, h));
        const Blurred blurred = blurredRegion(rgba, w, reg, sigma);
        Compose::forRows(area.height(), 32, [&](int a, int b) {
            std::vector<uint8_t> px(size_t(area.width()) * 4), tmp;
            for (int y = area.top() + a; y < area.top() + b; ++y) {
                const uint8_t* m = plane.data() + size_t(y) * w;
                int x0 = area.left(), x1 = area.right();
                while (x0 <= x1 && !m[x0]) ++x0;
                while (x1 >= x0 && !m[x1]) --x1;
                if (x0 > x1) continue;
                blurred.row(y - reg.top(), x0 - reg.left(), x1 - x0 + 1, px.data(), tmp);
                uint8_t* dst = rgba + (size_t(y) * w + x0) * 4;
                for (int x = x0; x <= x1; ++x, dst += 4) {
                    const int v = m[x] * K / 255;
                    const uint8_t* src = px.data() + size_t(x - x0) * 4;
                    if (v == 255) std::memcpy(dst, src, 4); // all of it (what lerp gives at 255)
                    else if (v) lerp(dst, src, v);
                }
            }
        });
    } else if (!area.isEmpty() && s.fill == ShapeFill::Pixelate && s.amount > 0) {
        // Block size: 100 = 1/10 of the frame height, blocks on a grid from the picture's top left (they stay put
        // while the shape moves)
        const int block = std::max(2, int(std::lround(s.amount / 100.0 * h / 10.0)));
        const int bx0 = area.left() / block, bx1 = area.right() / block;
        const int by0 = area.top() / block, by1 = area.bottom() / block;
        Compose::forRows(by1 - by0 + 1, 1, [&](int a, int b) {
            for (int by = by0 + a; by < by0 + b; ++by)
                for (int bx = bx0; bx <= bx1; ++bx) {
                    const QRect cell = QRect(bx * block, by * block, block, block).intersected(QRect(0, 0, w, h));
                    const QRect hit = cell.intersected(area);
                    if (hit.isEmpty()) continue;
                    long sum[4] = {0, 0, 0, 0};
                    for (int y = cell.top(); y <= cell.bottom(); ++y)
                        for (int x = cell.left(); x <= cell.right(); ++x)
                            for (int c = 0; c < 4; ++c) sum[c] += rgba[(size_t(y) * w + x) * 4 + c];
                    const long n = long(cell.width()) * cell.height();
                    const uint8_t avg[4] = {uint8_t(sum[0] / n), uint8_t(sum[1] / n), uint8_t(sum[2] / n), uint8_t(sum[3] / n)};
                    for (int y = hit.top(); y <= hit.bottom(); ++y)
                        for (int x = hit.left(); x <= hit.right(); ++x)
                            if (const int m = plane[size_t(y) * w + x]) lerp(rgba + (size_t(y) * w + x) * 4, avg, m * K / 255);
                }
        });
    }
    if (s.spotlight > 0) { // outside darker by `spotlight` percent, the soft edge in between
        const double d = std::clamp(s.spotlight / 100.0, 0.0, 1.0) * k;
        const int D = int(std::lround(d * 256));
        Compose::forRows(h, 32, [&](int a, int b) {
            for (int y = a; y < b; ++y) {
                uint8_t* row = rgba + size_t(y) * w * 4;
                const uint8_t* m = plane.data() + size_t(y) * w;
                for (int x = 0; x < w; ++x) {
                    const int keep = 256 - D * (255 - m[x]) / 255; // 256 inside, 256 - D outside
                    uint8_t* px = row + x * 4;
                    for (int c = 0; c < 3; ++c) px[c] = uint8_t((px[c] * keep + 128) >> 8);
                }
            }
        });
    }
}

void clipShape(uint8_t* rgba, int w, int h, const Shapes::ClipShape& cs, double scale)
{
    if (w <= 0 || h <= 0) return;
    const Shapes::Outline o = Shapes::clipShapeOutline(cs, w, h);
    std::vector<uint8_t> plane(size_t(w) * h, 0);
    coverage(o.points, cs.feather * scale, w, h, plane.data());
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
        uint8_t* a = rgba + i * 4 + 3;
        *a = uint8_t((*a * plane[i] + 127) / 255);
    }
    if (cs.strokeWidth > 0 && cs.strokeColor.alpha() > 0) {
        QImage img(rgba, w, h, w * 4, QImage::Format_RGBA8888);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        strokePath(p, o.points, true, 1.0, QPen(cs.strokeColor, cs.strokeWidth * scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    if (cs.shadow && cs.shadowOpacity > 0) {
        // Down and to the right by a third of its softness, behind the picture (inside the clip's own picture)
        std::vector<uint8_t> shadow(rgba, rgba + size_t(w) * h * 4);
        shadowOf(shadow.data(), w, h, Qt::black, cs.shadowOpacity, cs.shadowSoft * scale / 2);
        std::vector<uint8_t> out(size_t(w) * h * 4, 0);
        const double off = std::max(1.0, cs.shadowSoft * scale / 3);
        Compose::draw(out.data(), w, h, shadow.data(), w, h, shift(off, off), 1.0);
        Compose::over(out.data(), rgba, w, h, 1.0);
        std::memcpy(rgba, out.data(), out.size());
    }
}

} // namespace Picture::ShapePaint
