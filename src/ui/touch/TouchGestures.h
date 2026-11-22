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
//    the lift a glide, only after a real flick: see Momentum). The widget's own drag (moving clips, a rubber band) then needs
//    a hold first: held still for longPressMs (onHoldCue shows it) and then moved = press + moves as above; held and
//    lifted without moving = the long press (context menu). setAxisLock: a pan follows one axis only, chosen at its
//    start (sideways unless the swipe is clearly vertical). A finger landing on a gliding view only stops it (no tap)

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QVector>
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

    std::function<void()> onPanStart;                   // a pan begins (before its first onPan)

    // The glide after a pan: Off = none, Low = short (a quick flick only), Normal = like a flicked list
    enum class Momentum { Off, Low, Normal };
    void setMomentum(Momentum m) { m_momentum = m; }
    Momentum momentum() const { return m_momentum; }
    static Momentum momentumFromSetting(const QString& value); // "" / "low" = Low, "normal", "off"
    void setAxisLock(bool on) { m_axisLock = on; }
    // The finger's speed over the last ~80 ms (px per ms, along the pan's axis); 0 when not panning
    double panSpeed() const;
    bool gliding() const;

    enum class State { Idle, Pending, Pressed, Pinch, Done, Panning, Held };
    State state() const { return m_state; }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    bool touch(QTouchEvent* e);
    void longPress();
    void sendMouse(int type, const QPointF& pos, bool down);
    void glide(); // one step of the glide after a pan
    QPointF axisOnly(const QPointF& d) const;
    QPointF recentVelocity() const; // px per ms over the last samples
    void startGlide();

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
    struct Sample {
        qint64 ms;
        QPointF pos;
    };
    QVector<Sample> m_samples; // the pan's recent finger positions (time from m_clock)
    QElapsedTimer m_clock;     // runs from construction
    QPointF m_glideV;          // the glide's start speed (px per ms)
    qint64 m_glideStart = 0;
    double m_glideDone = 0;    // the part of the glide's distance already handed out (0..1 of v * tau)
    QTimer* m_glide = nullptr;
    Momentum m_momentum = Momentum::Normal;
    bool m_axisLock = false;
    enum class Axis { Both, X, Y } m_axis = Axis::Both;
    bool m_stopper = false; // this touch stopped a glide: a tap does nothing
};
