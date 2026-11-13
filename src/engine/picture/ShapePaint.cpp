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

// rgb of every pixel set to `color`, alpha times `opacity`; then blurred by sigma (a drop shadow of a picture)
void shadowOf(uint8_t* rgba, int w, int h, const QColor& color, double opacity, double sigma)
{
    const uint8_t r = uint8_t(color.red()), g = uint8_t(color.green()), b = uint8_t(color.blue());
    const double k = std::clamp(opacity, 0.0, 1.0) * color.alphaF();
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
        uint8_t* px = rgba + i * 4;
        px[0] = r;
        px[1] = g;
        px[2] = b;
        px[3] = uint8_t(std::lround(px[3] * k));
    }
    if (sigma > 0.3) VideoEffects::gaussian(rgba, w, h, sigma);
}

// dst = dst + (src - dst) * weight / 255 (all four channels), weight = mask times k
inline void lerp(uint8_t* dst, const uint8_t* src, int weight)
{
    for (int c = 0; c < 4; ++c) dst[c] = uint8_t(dst[c] + ((int(src[c]) - int(dst[c])) * weight + 127) / 255);
}

} // namespace

QRect coverage(const QVector<QPointF>& points, double soft, int w, int h, uint8_t* plane)
{
    const Masks::Geometry g = Shapes::geometry(points, soft);
    if (g.empty || w <= 0 || h <= 0) return {};
    return MaskRaster::render(g, w, h, plane, true);
}

void paint(uint8_t* rgba, int w, int h, QPoint origin, const Shapes::Outline& o, const TitleStyle& t, double reveal,
           double scale, double opacity, const QColor& fill)
{
    if (o.points.isEmpty() || opacity <= 0 || w <= 0 || h <= 0) return;
    const ShapeStyle& s = t.shape;
    const bool line = s.kind == ShapeKind::Line;
    const double lineWidth = line ? std::max(0.0, s.height) * scale : 0.0;
    const double outline = t.outlineOn ? std::max(0.0, t.outlineWidth) * scale : 0.0;
    const double feather = std::max(0.0, s.feather) * scale;
    const double sigma = t.look.shadowOn ? std::max(0.0, t.look.shadowSoft) * scale / 2 : 0.0;
    const QPointF shadowAt = t.look.shadowOn ? QPointF(t.look.shadowX, t.look.shadowY) * scale : QPointF();
    // The shape's own pixels (with room for its stroke, soft edge and the blur of its shadow), only where they can
    // reach the picture drawn on
    const double margin = std::max({feather, outline / 2 + lineWidth / 2 + (line ? outline : 0.0), 1.0}) + 2 + 3 * sigma;
    QRectF reach(origin, QSizeF(w, h));
    reach = reach.adjusted(-std::abs(shadowAt.x()) - 3 * sigma, -std::abs(shadowAt.y()) - 3 * sigma,
                           std::abs(shadowAt.x()) + 3 * sigma, std::abs(shadowAt.y()) + 3 * sigma);
    const QRect box = pointBounds(o.points).adjusted(-margin, -margin, margin, margin).intersected(reach).toAlignedRect();
    if (box.isEmpty() || box.width() > 16384 || box.height() > 16384) return;
    QVector<QPointF> pts = o.points;
    for (QPointF& p : pts) p -= box.topLeft();
    const int bw = box.width(), bh = box.height();
    QImage img(bw, bh, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    // Fill: the outline's coverage with its soft edge, in the fill colour
    if (!line && o.closed && fill.alpha() > 0) {
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
    if (straight.bytesPerLine() != bw * 4) return; // always packed for RGBA8888 (4-byte pixels)
    const double ox = box.x() - origin.x(), oy = box.y() - origin.y();
    if (t.look.shadowOn && t.look.shadowOpacity > 0) {
        QImage shadow = straight.copy();
        shadowOf(shadow.bits(), bw, bh, t.look.shadowColor, t.look.shadowOpacity, sigma);
        Compose::draw(rgba, w, h, shadow.constBits(), bw, bh, shift(ox + shadowAt.x(), oy + shadowAt.y()), opacity);
    }
    Compose::draw(rgba, w, h, straight.constBits(), bw, bh, shift(ox, oy), opacity);
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
        std::vector<uint8_t> blurred(size_t(reg.width()) * reg.height() * 4);
        for (int y = 0; y < reg.height(); ++y)
            std::memcpy(blurred.data() + size_t(y) * reg.width() * 4,
                        rgba + (size_t(reg.top() + y) * w + reg.left()) * 4, size_t(reg.width()) * 4);
        VideoEffects::gaussian(blurred.data(), reg.width(), reg.height(), sigma);
        Compose::forRows(area.height(), 32, [&](int a, int b) {
            for (int y = area.top() + a; y < area.top() + b; ++y)
                for (int x = area.left(); x <= area.right(); ++x) {
                    const int m = plane[size_t(y) * w + x];
                    if (!m) continue;
                    lerp(rgba + (size_t(y) * w + x) * 4,
                         blurred.data() + (size_t(y - reg.top()) * reg.width() + (x - reg.left())) * 4, m * K / 255);
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
