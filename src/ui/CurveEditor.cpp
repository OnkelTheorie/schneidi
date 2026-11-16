// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/CurveEditor.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kHandle = 4.5; // handle radius
constexpr double kGrab = 8;     // grab distance of a handle
}

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setToolTip(T("Klicken = Punkt setzen, ziehen = verschieben (Shift = fein), Doppelklick/Rechtsklick auf einen Punkt = "
                 "entfernen"));
}

void CurveEditor::setKind(GradeCurves::Kind kind)
{
    if (kind == m_kind) return;
    m_kind = kind;
    m_drag = -1;
    update();
}

void CurveEditor::setHistograms(const QVector<QVector<float>>& h)
{
    m_hist = h;
    update();
}

QVector<QVector<float>> CurveEditor::histograms(const QImage& picture)
{
    QVector<QVector<float>> out(5, QVector<float>(kBins, 0.0f));
    if (picture.isNull()) return {};
    const QImage img = picture.format() == QImage::Format_RGB32 || picture.format() == QImage::Format_ARGB32
                           ? picture
                           : picture.convertToFormat(QImage::Format_RGB32);
    const int step = std::max(1, int(std::sqrt(double(img.width()) * img.height() / 40000.0)));
    for (int y = 0; y < img.height(); y += step) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += step) {
            const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            const auto bin = [](double v) { return std::clamp(int(v * kBins / 256.0), 0, kBins - 1); };
            out[0][bin(0.2126 * r + 0.7152 * g + 0.0722 * b)] += 1;
            out[1][bin(r)] += 1;
            out[2][bin(g)] += 1;
            out[3][bin(b)] += 1;
            const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
            if (mx - mn > 8) {
                const QColor c(r, g, b);
                out[4][std::clamp(int(c.hsvHueF() * kBins), 0, kBins - 1)] += float(mx - mn) / 255.0f;
            }
        }
    }
    for (QVector<float>& h : out) { // the tallest bin = 1; a single huge bin (black bars) clipped to 4x the mean
        float sum = 0;
        for (float v : h) sum += v;
        const float cap = sum / kBins * 4;
        float mx = 0;
        for (float& v : h) mx = std::max(mx, v = std::min(v, cap));
        if (mx > 0)
            for (float& v : h) v /= mx;
    }
    return out;
}

void CurveEditor::setPoints(const QVector<QPointF>& points)
{
    if (m_drag >= 0) return;
    const QVector<QPointF> p = GradeCurves::normalized(points);
    if (p == m_points) return;
    m_points = p;
    update();
}

QRectF CurveEditor::area() const
{
    const double strip = GradeCurves::tone(m_kind) ? 0 : 10; // hue / brightness strip below
    return QRectF(6, 6, std::max(10, width() - 12), std::max(10.0, height() - 12 - strip));
}

QPointF CurveEditor::toWidget(const QPointF& v) const
{
    const QRectF r = area();
    return {r.left() + v.x() * r.width(), r.bottom() - v.y() * r.height()};
}

QPointF CurveEditor::toValue(const QPointF& pos) const
{
    const QRectF r = area();
    return {(pos.x() - r.left()) / r.width(), (r.bottom() - pos.y()) / r.height()};
}

QColor CurveEditor::curveColor() const
{
    switch (m_kind) {
    case GradeCurves::Kind::R: return QColor(0xe8, 0x5a, 0x5a);
    case GradeCurves::Kind::G: return QColor(0x5a, 0xc8, 0x6a);
    case GradeCurves::Kind::B: return QColor(0x5a, 0x8a, 0xe8);
    case GradeCurves::Kind::Y: return Theme::text;
    default: return Theme::primary;
    }
}

int CurveEditor::pointAt(const QPointF& pos) const
{
    int best = -1;
    double bestD = kGrab;
    for (int i = 0; i < m_points.size(); ++i) {
        const double d = QLineF(toWidget(m_points[i]), pos).length();
        if (d <= bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void CurveEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = area();
    p.fillRect(rect(), Theme::lane);
    // Grid in quarters
    p.setPen(QPen(Theme::alpha(Theme::text, 22), 1));
    for (int i = 1; i < 4; ++i) {
        const double x = r.left() + r.width() * i / 4, y = r.top() + r.height() * i / 4;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.setPen(QPen(Theme::border, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    // What x means: the hue spectrum or the brightness ramp below the area
    if (!GradeCurves::tone(m_kind)) {
        const QRectF strip(r.left(), r.bottom() + 3, r.width(), 6);
        QLinearGradient g(strip.topLeft(), strip.topRight());
        if (m_kind == GradeCurves::Kind::LumSat) {
            g.setColorAt(0, Qt::black);
            g.setColorAt(1, Qt::white);
        } else {
            for (int i = 0; i <= 6; ++i) g.setColorAt(i / 6.0, QColor::fromHsvF(std::fmod(i / 6.0, 1.0), 0.75, 0.9));
        }
        p.fillRect(strip, g);
    }
    // Histogram of the shown picture behind everything
    {
        using K = GradeCurves::Kind;
        const int which = m_kind == K::R ? 1 : m_kind == K::G ? 2 : m_kind == K::B ? 3
                          : GradeCurves::periodic(m_kind) ? 4 : 0;
        if (which < m_hist.size() && m_hist[which].size() == kBins) {
            QPainterPath area;
            area.moveTo(r.bottomLeft());
            for (int i = 0; i < kBins; ++i) {
                const double x0 = r.left() + r.width() * i / kBins, x1 = r.left() + r.width() * (i + 1) / kBins;
                const double y = r.bottom() - m_hist[which][i] * r.height() * 0.9;
                area.lineTo(x0, y);
                area.lineTo(x1, y);
            }
            area.lineTo(r.bottomRight());
            area.closeSubpath();
            QColor fill = which >= 1 && which <= 3 ? curveColor() : Theme::text;
            fill.setAlpha(38);
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawPath(area);
        }
    }
    // The untouched line
    p.setPen(QPen(Theme::alpha(Theme::text, 50), 1, Qt::DashLine));
    if (GradeCurves::tone(m_kind)) p.drawLine(toWidget({0, 0}), toWidget({1, 1}));
    else p.drawLine(toWidget({0, 0.5}), toWidget({1, 0.5}));
    // The curve
    QPainterPath path;
    const int steps = std::max(32, int(r.width() / 2));
    const GradeCurves::Shape shape(m_points, m_kind);
    for (int i = 0; i <= steps; ++i) {
        const double x = double(i) / steps;
        const QPointF q = toWidget({x, shape(x)});
        if (i == 0) path.moveTo(q);
        else path.lineTo(q);
    }
    p.setPen(QPen(curveColor(), 1.6));
    p.drawPath(path);
    // Handles
    for (int i = 0; i < m_points.size(); ++i) {
        const bool active = i == m_drag;
        p.setPen(QPen(active ? Theme::primary : curveColor(), 1.5));
        p.setBrush(active ? Theme::primary : Theme::lane);
        p.drawEllipse(toWidget(m_points[i]), kHandle, kHandle);
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::RightButton) {
        if (const int i = pointAt(e->position()); i >= 0) removePoint(i);
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    int i = pointAt(e->position());
    if (i < 0) {
        const QPointF v = toValue(e->position());
        const double x = std::clamp(v.x(), 0.0, 1.0);
        // On the curve where it is near (it only bends there), at the mouse otherwise
        const double onCurve = GradeCurves::eval(m_points, m_kind, x);
        const double y = std::abs(toWidget({x, onCurve}).y() - e->position().y()) < 14 ? onCurve : std::clamp(v.y(), 0.0, 1.0);
        QVector<QPointF> pts = m_points;
        if (pts.isEmpty()) {
            if (GradeCurves::tone(m_kind)) {
                pts = {{0, 0}, {1, 1}}; // the ends of the straight line, so the rest stays where it was
            } else if (GradeCurves::periodic(m_kind)) {
                const double w = 1.0 / 6; // neutral neighbours one colour away: only this hue moves
                pts = {{std::fmod(x - w + 1, 1.0), 0.5}, {std::fmod(x + w, 1.0), 0.5}};
            } else {
                pts = {{0, 0.5}, {1, 0.5}};
            }
        }
        pts << QPointF(x, y);
        pts = GradeCurves::normalized(pts);
        // The new point (normalized may have merged it into one close by)
        int found = -1;
        for (int k = 0; k < pts.size(); ++k)
            if (std::abs(pts[k].x() - x) < GradeCurves::kMinGap) found = k;
        m_points = pts;
        i = found;
        if (i < 0) return;
        // Dragging from here on (the model's answer to `changed` must not replace the points: a point added on the
        // straight line leaves it straight, which the model stores as no curve at all)
        m_drag = i;
        m_last = e->position();
        m_dragValue = m_points[i];
        update();
        emit changed(m_points);
        return;
    }
    m_drag = i;
    m_last = e->position();
    m_dragValue = m_points[i];
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag < 0) {
        setCursor(pointAt(e->position()) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
        return;
    }
    const QRectF r = area();
    const double fine = e->modifiers() & Qt::ShiftModifier ? 0.15 : 1.0;
    const QPointF d = e->position() - m_last;
    m_last = e->position();
    m_dragValue += QPointF(d.x() / r.width() * fine, -d.y() / r.height() * fine);
    // Between its neighbours: the points keep their order
    const double lo = m_drag > 0 ? m_points[m_drag - 1].x() + GradeCurves::kMinGap : 0.0;
    const double hi = m_drag + 1 < m_points.size() ? m_points[m_drag + 1].x() - GradeCurves::kMinGap : 1.0;
    QPointF v(std::clamp(m_dragValue.x(), lo, std::max(lo, hi)), std::clamp(m_dragValue.y(), 0.0, 1.0));
    if (v == m_points[m_drag]) return;
    m_points[m_drag] = v;
    update();
    emit changed(m_points);
}

void CurveEditor::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || m_drag < 0) return;
    m_drag = -1;
    update();
    emit finished();
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    if (const int i = pointAt(e->position()); i >= 0) removePoint(i);
}

void CurveEditor::removePoint(int i)
{
    if (i < 0 || i >= m_points.size()) return;
    m_drag = -1;
    m_points.removeAt(i);
    // A tone curve with fewer than two points or a hue curve left on its neutral line: straight again
    if ((GradeCurves::tone(m_kind) && m_points.size() < 2) || GradeCurves::isIdentity(m_points, m_kind)) m_points.clear();
    update();
    emit changed(m_points);
    emit finished();
}
