// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Curve of the Color page (core/GradeCurves): the curve over a grid, its points as handles. Click = new point on the
// curve (a straight hue curve gets two neutral neighbours with it, so only that hue moves), drag = move it (Shift =
// fine), double-click or right-click on a point = remove it. Tone curves show the straight line they started from,
// hue curves the hue spectrum below, brightness vs saturation a gray ramp.

#include "core/GradeCurves.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QVector>
#include <QWidget>

class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);

    void setKind(GradeCurves::Kind kind);
    GradeCurves::Kind kind() const { return m_kind; }
    // The model's points (not while a point is dragged: the drag owns them then)
    void setPoints(const QVector<QPointF>& points);
    QVector<QPointF> points() const { return m_points; }
    bool dragging() const { return m_drag >= 0; }
    // Histograms of the shown picture behind the curve: [0] luma, [1..3] R/G/B, [4] hue (weighted by chroma), each
    // kBins values 0..1 (empty = none). Tone curves show their channel (Y: luma), hue curves the hue, lum/sat the luma
    static constexpr int kBins = 64;
    void setHistograms(const QVector<QVector<float>>& h);
    // The five histograms of an RGBA/RGB32 picture (a few tens of thousands of pixels sampled)
    static QVector<QVector<float>> histograms(const QImage& picture);

    QSize sizeHint() const override { return {300, 190}; }
    QSize minimumSizeHint() const override { return {160, 110}; }

    // Curve area inside the widget and the mapping of curve values (0..1) to it
    QRectF area() const;
    QPointF toWidget(const QPointF& v) const;
    QPointF toValue(const QPointF& pos) const;

signals:
    void changed(const QVector<QPointF>& points); // every change (also while dragging)
    void finished();                              // a drag released, a point added or removed

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    int pointAt(const QPointF& pos) const; // index of the handle under pos, -1 = none
    void removePoint(int i);
    QColor curveColor() const;

    GradeCurves::Kind m_kind = GradeCurves::Kind::Y;
    QVector<QPointF> m_points;
    QVector<QVector<float>> m_hist;
    int m_drag = -1;       // index of the dragged point
    QPointF m_last;        // mouse position of the last move (relative dragging, Shift = fine)
    QPointF m_dragValue;   // the dragged point's value before clamping to its neighbours
};
