// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Finger gestures for a widget that draws itself (see Touch.h for the whole touch layer). Installs itself as an event
// filter on the widget and turns on Qt::WA_AcceptTouchEvents; touchpads and pens are left alone.
//  - one finger: nothing happens until it moves past Touch::slopPx() (then press + moves + release at the widget, as
//    mouse events with the touch device: the widget's mouse code runs unchanged, Touch::isTouch() tells it a finger)
//    or lifts (a tap = press + release; two taps close together = a double click, or onDoubleTap)
//  - held still for Touch::longPressMs(): onLongPress, default a QContextMenuEvent at the finger (like a right click;
//    reason Other, so a widget that opens some menus on the mouse release instead still opens them for a finger)
//  - a second finger before the first one moved: pinch. onPinch gets the previous and the new centre of the two
//    fingers and the scale since the last update (distance ratio): zoom around the fingers and pan in one
//  - a running TouchDrag (started by the widget's mouse code for a touch press) gets the finger's moves and drop
//  - panAt (optional) true for the press point: one finger moving pans (onPan with each step's distance, and after
//    the lift a decaying glide like a flicked list). The widget's own drag (moving clips, a rubber band) then needs
//    a hold first: held still for longPressMs (onHoldCue shows it) and then moved = press + moves as above; held and
//    lifted without moving = the long press (context menu)

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <functional>

class QPointingDevice;
class QTimer;
class QTouchEvent;
class QWidget;

class TouchGestures : public QObject {
public:
    explicit TouchGestures(QWidget* target);

    std::function<void(const QPointF& from, const QPointF& to, double scale)> onPinch;
    std::function<void()> onPinchEnd;
    std::function<bool(const QPointF& pos)> onDoubleTap; // true = handled (else a double click goes to the widget)
    std::function<bool(const QPointF& pos)> onLongPress; // true = handled (else the context menu event)
    std::function<bool(const QPointF& pos)> panAt;      // one finger pans here (see above)
    std::function<void(const QPointF& delta)> onPan;    // move the view's content by delta (finger direction)
    std::function<void(const QPointF& pos)> onHoldCue;  // the hold is ready: the next move works the widget itself

    enum class State { Idle, Pending, Pressed, Pinch, Done, Panning, Held };
    State state() const { return m_state; }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    bool touch(QTouchEvent* e);
    void longPress();
    void sendMouse(int type, const QPointF& pos, bool down);
    void glide(); // one step of the glide after a pan

    QWidget* m_target;
    QTimer* m_hold;
    State m_state = State::Idle;
    QPointer<const QPointingDevice> m_device;
    int m_mods = 0;
    QPointF m_pressPos, m_lastPos, m_lastGlobal;
    QPointF m_pinchCentre;
    double m_pinchDist = 0;
    QPointF m_lastTapPos{-1e9, -1e9};
    QElapsedTimer m_lastTap;
    bool m_pan = false;        // this touch pans (panAt said so at the press)
    QPointF m_velocity;        // px per ms, smoothed, while panning
    QElapsedTimer m_moveClock; // time since the last pan step
    QTimer* m_glide = nullptr;
};
