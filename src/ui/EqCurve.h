// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QPointF>
#include <QWidget>

class Editor;

// EQ curve of one audio track (EQ dialog, like the Fairlight EQ): frequency logarithmic 20 Hz – 20 kHz, gain ±24 dB.
// Every band is a point: drag = frequency (sideways) and gain (up/down; high/low pass only sideways), one undo
// step per drag; double click = back to 0 dB. Dragging switches the EQ and the band on.
class EqCurve : public QWidget {
    Q_OBJECT
public:
    EqCurve(Editor* editor, int track, QWidget* parent = nullptr);

    // Where band `band` (0..5) is drawn (widget coordinates)
    QPointF pointFor(int band) const;
    QSize sizeHint() const override { return {480, 220}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QRectF plot() const;
    double xOf(double hz) const;
    double hzAt(double x) const;
    double yOf(double db) const;
    double dbAt(double y) const;
    int bandAt(const QPointF& p) const; // -1 = none

    Editor* m_editor;
    int m_track;
    int m_drag = -1;  // band being dragged
    QPointF m_grab;   // mouse minus band point at the press (the point stays under the mouse)
};
