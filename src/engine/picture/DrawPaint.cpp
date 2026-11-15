// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/DrawPaint.h"

#include "engine/video/Compose.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Picture::DrawPaint {

namespace {

// Signed area (> 0 = one turning direction): every piece of a varying-width stroke turns the same way, so the winding
// fill covers their union once (no piece cancels another out)
double area(const QPolygonF& p)
{
    double a = 0;
    for (int i = 0, n = int(p.size()); i < n; ++i) {
        const QPointF& u = p[i];
        const QPointF& v = p[(i + 1) % n];
        a += u.x() * v.y() - v.x() * u.y();
    }
    return a / 2;
}

void addPiece(QPainterPath& path, QPolygonF poly)
{
    if (poly.size() < 3) return;
    if (area(poly) < 0) std::reverse(poly.begin(), poly.end());
    poly << poly.first();
    path.addPolygon(poly);
}

QPolygonF circle(QPointF c, double r)
{
    QPolygonF p;
    const int n = std::clamp(int(std::ceil(2 * std::numbers::pi * r / 1.5)), 8, 96);
    for (int i = 0; i < n; ++i) {
        const double a = 2 * std::numbers::pi * i / n;
        p << c + QPointF(std::cos(a) * r, std::sin(a) * r);
    }
    return p;
}

// The outline of a stroke whose width changes along it: a disc at every point and a band between neighbours
QPainterPath varyingStroke(const Drawing::Placed& s)
{
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    double drawnR = -1; // the last disc's radius
    for (int i = 0; i < s.points.size(); ++i) {
        const double r = std::max(0.0, s.widths.value(i)) / 2;
        // A disc where the width changed, the line turns, and at both ends; the bands cover the rest
        bool disc = i == 0 || i + 1 == s.points.size() || std::abs(r - drawnR) > 0.05 * std::max(r, 0.5);
        if (!disc) {
            const QPointF u = s.points[i] - s.points[i - 1], v = s.points[i + 1] - s.points[i];
            const double lu = std::hypot(u.x(), u.y()), lv = std::hypot(v.x(), v.y());
            disc = lu < 1e-9 || lv < 1e-9 || (u.x() * v.x() + u.y() * v.y()) / (lu * lv) < 0.996; // > ~5 degrees
        }
        if (disc && r > 0.05) {
            addPiece(path, circle(s.points[i], r));
            drawnR = r;
        }
        if (i == 0) continue;
        const QPointF a = s.points[i - 1], b = s.points[i], d = b - a;
        const double len = std::hypot(d.x(), d.y());
        if (len < 1e-9) continue;
        const QPointF n(-d.y() / len, d.x() / len);
        const double ra = std::max(0.0, s.widths.value(i - 1)) / 2, rb = r;
        addPiece(path, QPolygonF{a + n * ra, b + n * rb, b - n * rb, a - n * ra});
    }
    return path;
}

bool uniform(const Drawing::Placed& s)
{
    for (const double w : s.widths)
        if (std::abs(w - s.widths.value(0)) > 1e-3) return false;
    return true;
}

} // namespace

Painted render(const QVector<Drawing::Placed>& strokes, const QRect& reach)
{
    Painted out;
    if (strokes.isEmpty()) return out;
    const QRect box = Drawing::bounds(strokes).adjusted(-2, -2, 2, 2).intersected(QRectF(reach)).toAlignedRect();
    if (box.isEmpty() || box.width() > 16384 || box.height() > 16384) return out;
    QImage img(box.width(), box.height(), QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-box.topLeft());
        for (const Drawing::Placed& s : strokes) {
            if (s.points.isEmpty() || s.opacity <= 0 || s.color.alpha() == 0) continue;
            p.setOpacity(std::clamp(s.opacity, 0.0, 1.0));
            if (s.points.size() == 1) { // a dot
                const double r = std::max(0.0, s.widths.value(0)) / 2;
                p.setPen(Qt::NoPen);
                p.setBrush(s.color);
                p.drawEllipse(s.points[0], r, r);
            } else if (uniform(s)) { // one pen along the whole line: no double blending where it crosses itself
                const double width = std::max(0.0, s.widths.value(0));
                if (width <= 0) continue;
                QPainterPath path(s.points[0]);
                for (int i = 1; i < s.points.size(); ++i) path.lineTo(s.points[i]);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(s.color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawPath(path);
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(s.color);
                p.drawPath(varyingStroke(s));
            }
        }
    }
    QImage straight = img.convertToFormat(QImage::Format_RGBA8888);
    if (straight.bytesPerLine() != box.width() * 4) return out;
    out.straight = straight;
    out.box = box;
    return out;
}

void put(uint8_t* rgba, int w, int h, QPoint origin, const Painted& painted, double opacity)
{
    if (painted.straight.isNull() || opacity <= 0 || w <= 0 || h <= 0) return;
    Compose::Affine m;
    m.c = painted.box.x() - origin.x();
    m.f = painted.box.y() - origin.y();
    Compose::draw(rgba, w, h, painted.straight.constBits(), painted.box.width(), painted.box.height(), m, opacity);
}

void paint(uint8_t* rgba, int w, int h, QPoint origin, const QVector<Drawing::Placed>& strokes, double opacity)
{
    if (strokes.isEmpty() || opacity <= 0 || w <= 0 || h <= 0) return;
    put(rgba, w, h, origin, render(strokes, QRect(origin, QSize(w, h))), opacity);
}

} // namespace Picture::DrawPaint
