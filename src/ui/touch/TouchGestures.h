// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Finger gestures for a widget that draws itself (see Touch.h for the whole touch layer). Installs itself as an event
// filter on the widget and turns on Qt::WA_AcceptTouchEvents; touchpads and pens are left alone.
//  - one finger: nothing happens until it moves past Touch::slopPx() (then press + moves + release at the widget, as
//    mouse events with the touch device: the widget's mouse code runs unchanged, Touch::isTouch() tells it a finger)
//    or lifts (a tap = press + release; two taps close together = a double click, or onDoubleTap)
//  - held still for Touch::longPressMs(): onLongPress, default a QContextMenuEvent at the finger (like a right click)
//  - a second finger before the first one moved: pinch. onPinch gets the previous and the new centre of the two
//    fingers and the scale since the last update (distance ratio): zoom around the fingers and pan in one
//  - a running TouchDrag (started by the widget's mouse code for a touch press) gets the finger's moves and drop

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

    enum class State { Idle, Pending, Pressed, Pinch, Done };
    State state() const { return m_state; }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    bool touch(QTouchEvent* e);
    void longPress();
    void sendMouse(int type, const QPointF& pos, bool down);

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
};
