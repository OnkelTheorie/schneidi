#pragma once
// On-screen controls drawn over the viewer picture (like DaVinci's window outline on the Color page). The viewer
// calls paint() after the picture and offers mouse events first; `frame` is the area of the project frame in widget
// coordinates (the picture without the black bars).

#include <QObject>
#include <QRectF>

class QMouseEvent;
class QPainter;

class ViewerOverlay : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void paint(QPainter& p, const QRectF& frame) = 0;
    // true = the overlay took the event (no drag of the picture into the timeline)
    virtual bool mousePress(QMouseEvent*, const QRectF&) { return false; }
    virtual bool mouseMove(QMouseEvent*, const QRectF&) { return false; }
    virtual bool mouseRelease(QMouseEvent*, const QRectF&) { return false; }
    // Cursor shape over a point (no button pressed); Qt::ArrowCursor = nothing there
    virtual Qt::CursorShape cursorAt(const QPointF&, const QRectF&) const { return Qt::ArrowCursor; }

signals:
    void changed(); // repaint
};
