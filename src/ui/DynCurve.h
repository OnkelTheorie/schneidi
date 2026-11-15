// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QPointF>
#include <QWidget>

#include <functional>

class Editor;
struct TrackDynamics;

// The compressor's static curve of one audio track (compressor window, Studio page): input -60..0 dB across, output
// up. The threshold point slides sideways, the end point at 0 dB input up and down sets the ratio. A drag switches the
// compressor on; one undo step per drag. Double click = onOpen (when set).
class DynCurve : public QWidget {
    Q_OBJECT
public:
    DynCurve(Editor* editor, int track, QWidget* parent = nullptr);

    // The window's track moved (tracks added/removed/reordered): draw and edit that one
    void setTrack(int track)
    {
        m_track = track;
        m_drag = 0;
        update();
    }
    std::function<void()> onOpen;
    QSize sizeHint() const override { return QSize(360, 200); }
    // Where the threshold / ratio point is drawn (tests)
    QPointF thresholdPoint() const;
    QPointF endPoint() const;
    // The reading in the top left corner ("-20 dB  4,0:1") and its box (drawn on the background, no grid line through it)
    QString labelText() const;
    QRectF labelRect() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    TrackDynamics dyn() const;
    QRectF plot() const;
    QPointF at(double in, double out) const;

    Editor* m_editor;
    int m_track;
    int m_drag = 0; // 1 = threshold, 2 = ratio
};
