// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/touch/TouchGestures.h"

#include "ui/touch/Touch.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QStyleHints>
#include <QTimer>
#include <QTouchEvent>
#include <QWidget>
#include <cmath>

TouchGestures::TouchGestures(QWidget* target) : QObject(target), m_target(target)
{
    m_clock.start();
    target->setAttribute(Qt::WA_AcceptTouchEvents);
    target->installEventFilter(this);
    m_hold = new QTimer(this);
    m_hold->setSingleShot(true);
    m_hold->setInterval(Touch::longPressMs());
    connect(m_hold, &QTimer::timeout, this, &TouchGestures::longPress);
    m_glide = new QTimer(this);
    m_glide->setInterval(16);
    connect(m_glide, &QTimer::timeout, this, &TouchGestures::glide);
}

namespace {
// The glide's time constant (ms): its speed falls to 1/e in this time, its whole way is speed * tau
double glideTau(TouchGestures::Momentum m) { return m == TouchGestures::Momentum::Normal ? 325.0 : 140.0; }
// The finger's speed at the lift that counts as a flick (px per ms); slower = the view stops where the finger left it
double flickSpeed(TouchGestures::Momentum m) { return m == TouchGestures::Momentum::Normal ? 0.35 : 0.7; }
double maxGlideSpeed(TouchGestures::Momentum m) { return m == TouchGestures::Momentum::Normal ? 6.0 : 3.0; }
constexpr qint64 kVelocityWindowMs = 80; // the speed of a lift: the finger's way over this time
} // namespace

TouchGestures::Momentum TouchGestures::momentumFromSetting(const QString& value)
{
    if (value == QLatin1String("off")) return Momentum::Off;
    if (value == QLatin1String("normal")) return Momentum::Normal;
    return Momentum::Low;
}

QPointF TouchGestures::axisOnly(const QPointF& d) const
{
    if (m_axis == Axis::X) return {d.x(), 0};
    if (m_axis == Axis::Y) return {0, d.y()};
    return d;
}

QPointF TouchGestures::recentVelocity() const
{
    if (m_samples.size() < 2) return {};
    const Sample& last = m_samples.last();
    // A finger that stood still before the lift: no speed (only moves of the last window count)
    if (m_clock.elapsed() - last.ms > 50) return {};
    const Sample* from = &m_samples.first();
    for (const Sample& s : m_samples)
        if (last.ms - s.ms <= kVelocityWindowMs) {
            from = &s;
            break;
        }
    const double ms = double(last.ms - from->ms);
    if (ms < 8) return {}; // too short to tell a speed (one event)
    return axisOnly((last.pos - from->pos) / ms);
}

double TouchGestures::panSpeed() const
{
    if (m_state != State::Panning) return 0;
    const QPointF v = recentVelocity();
    return std::hypot(v.x(), v.y());
}

bool TouchGestures::gliding() const { return m_glide->isActive(); }

void TouchGestures::startGlide()
{
    if (m_momentum == Momentum::Off || !onPan) return;
    QPointF v = recentVelocity();
    const double speed = std::hypot(v.x(), v.y());
    if (speed < flickSpeed(m_momentum)) return; // no flick: it stops where the finger left it
    if (speed > maxGlideSpeed(m_momentum)) v *= maxGlideSpeed(m_momentum) / speed;
    m_glideV = v;
    m_glideStart = m_clock.elapsed();
    m_glideDone = 0;
    m_glide->start();
}

void TouchGestures::glide()
{
    // Time-based friction (the same way on a slow or a busy device, whatever the timer's ticks): the speed falls off
    // exponentially, the way gone is v * tau * (1 - e^(-t / tau))
    const double tau = glideTau(m_momentum);
    const double t = double(m_clock.elapsed() - m_glideStart);
    const double part = 1.0 - std::exp(-t / tau);
    const double speedNow = std::hypot(m_glideV.x(), m_glideV.y()) * std::exp(-t / tau);
    const QPointF step = m_glideV * tau * (part - m_glideDone);
    m_glideDone = part;
    if (!onPan || speedNow < 0.02) {
        m_glide->stop();
        if (onPan && !step.isNull()) onPan(step);
        return;
    }
    if (!step.isNull()) onPan(step);
}

bool TouchGestures::eventFilter(QObject* obj, QEvent* e)
{
    if (obj != m_target) return false;
    switch (e->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        auto* te = static_cast<QTouchEvent*>(e);
        if (!Touch::isTouchDevice(te->pointingDevice())) return false; // touchpad: the usual way
        return touch(te);
    }
    default: return false;
    }
}

void TouchGestures::sendMouse(int type, const QPointF& pos, bool down)
{
    const auto t = QEvent::Type(type);
    const Qt::MouseButton button = t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    // A click's focus (Qt gives it only for a spontaneous press): keys go where the finger tapped
    if (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonDblClick) Touch::clickFocus(m_target);
    const QPointF global = m_target->mapToGlobal(pos);
    const QPointF scene = m_target->window() ? m_target->mapTo(m_target->window(), pos) : pos;
    QMouseEvent me(t, pos, scene, global, button, down ? Qt::LeftButton : Qt::NoButton,
                   Qt::KeyboardModifiers(m_mods), m_device ? m_device.data() : QPointingDevice::primaryPointingDevice());
    me.setTimestamp(m_time); // (the finger's: a made-up event carries 0, Touch.cpp sendMouse)
    QApplication::sendEvent(m_target, &me);
}

bool TouchGestures::touch(QTouchEvent* e)
{
    m_time = e->timestamp();
    const auto& pts = e->points();
    const QPointer<TouchGestures> alive(this);
    if (e->type() == QEvent::TouchBegin) {
        // A finger stops a gliding view at once (and that touch is no tap: it only stopped it)
        m_stopper = m_glide->isActive();
        m_glide->stop();
    }
    // The finger landed on a child widget (the track name's text field, an overlay button): Qt offers the touch to
    // the target because the child takes none. Not ours: left unaccepted, Qt makes the child's own mouse events of
    // it (found in the Android emulator: a tap into the open name field closed it instead of placing the cursor)
    if (e->type() == QEvent::TouchBegin && !pts.isEmpty() && m_target->childAt(pts.first().position().toPoint())) {
        m_state = State::Idle;
        m_hold->stop();
        e->ignore();
        return true;
    }
    e->accept();
    if (e->type() == QEvent::TouchBegin) {
        m_device = e->pointingDevice();
        m_mods = int(e->modifiers());
        m_state = State::Pending;
        m_pressPos = m_lastPos = pts.isEmpty() ? QPointF() : pts.first().position();
        m_lastGlobal = pts.isEmpty() ? QPointF() : pts.first().globalPosition();
        m_hold->start();
        m_pan = panAt && onPan && panAt(m_pressPos);
        m_samples.clear();
        m_axis = Axis::Both;
        if (pts.size() < 2) return true;
        // two fingers at once: straight into the pinch below
    }
    if (e->type() == QEvent::TouchCancel) {
        m_hold->stop();
        m_glide->stop();
        // The view drops what the finger had begun (a held reorder, a drag) before the release below reaches it:
        // a cancelled touch never commits anything
        if (onCancel) onCancel();
        if (!alive) return true;
        if (TouchDrag::active()) TouchDrag::finish(m_lastGlobal, false);
        if (m_state == State::Pressed) sendMouse(QEvent::MouseButtonRelease, m_lastPos, false);
        if (alive && m_state == State::Pinch && onPinchEnd) onPinchEnd();
        if (alive) m_state = State::Idle;
        return true;
    }
    // The finger that pressed first leads; points that lifted are still listed in this event
    int live = 0;
    for (const auto& p : pts)
        if (p.state() != QEventPoint::Released) ++live;
    const bool end = e->type() == QEvent::TouchEnd;
    const QEventPoint* first = pts.isEmpty() ? nullptr : &pts.first();
    if (first) {
        m_lastPos = first->position();
        m_lastGlobal = first->globalPosition();
    }

    if (TouchDrag::active()) {
        m_hold->stop();
        if (end) {
            TouchDrag::finish(m_lastGlobal, true);
            if (alive && m_state == State::Pressed) sendMouse(QEvent::MouseButtonRelease, m_lastPos, false);
            if (alive) m_state = State::Idle;
        } else {
            TouchDrag::move(m_lastGlobal);
        }
        return true;
    }

    // A second finger before the first one did anything: pinch (zoom + pan)
    if (m_state == State::Pending && live >= 2) {
        m_hold->stop();
        m_state = State::Pinch;
        m_pinchCentre = (pts[0].position() + pts[1].position()) / 2;
        m_pinchDist = QLineF(pts[0].position(), pts[1].position()).length();
        return true;
    }
    switch (m_state) {
    case State::Pending:
        if (end) { // a tap
            m_hold->stop();
            const bool second = m_lastTap.isValid()
                                && m_lastTap.elapsed() < QGuiApplication::styleHints()->mouseDoubleClickInterval()
                                && QLineF(m_lastTapPos, m_pressPos).length() < 3 * Touch::slopPx();
            m_state = State::Idle;
            if (m_stopper) { // it only stopped the glide
                m_stopper = false;
                return true;
            }
            if (second) {
                m_lastTap.invalidate();
                if (onDoubleTap && onDoubleTap(m_pressPos)) return true;
                sendMouse(QEvent::MouseButtonDblClick, m_pressPos, true);
                if (alive) sendMouse(QEvent::MouseButtonRelease, m_pressPos, false);
                return true;
            }
            m_lastTap.start();
            m_lastTapPos = m_pressPos;
            sendMouse(QEvent::MouseButtonPress, m_pressPos, true);
            if (alive) sendMouse(QEvent::MouseButtonRelease, m_pressPos, false);
            return true;
        }
        if (QLineF(m_pressPos, m_lastPos).length() < Touch::slopPx()) return true;
        m_hold->stop();
        m_lastTap.invalidate();
        if (m_pan) { // one finger moves the view
            m_state = State::Panning;
            const QPointF d = m_lastPos - m_pressPos;
            // The axis is chosen once: sideways (time) unless the swipe is clearly vertical (steeper than ~60 deg)
            if (m_axisLock) m_axis = std::abs(d.y()) > 1.7 * std::abs(d.x()) ? Axis::Y : Axis::X;
            if (onPanStart) onPanStart();
            if (alive && onPan) onPan(axisOnly(d));
            if (!alive) return true;
            m_samples.clear();
            m_samples.append({m_clock.elapsed(), m_lastPos});
            m_pinchCentre = m_lastPos; // (the last pan point)
            return true;
        }
        m_state = State::Pressed;
        sendMouse(QEvent::MouseButtonPress, m_pressPos, true);
        if (alive && !TouchDrag::active()) sendMouse(QEvent::MouseMove, m_lastPos, true);
        return true;
    case State::Panning: {
        if (!onPan) { // the view gave up panning meanwhile (a layout switch): nothing more of this touch
            m_state = end ? State::Idle : State::Done;
            return true;
        }
        if (live >= 2) { // a second finger joins: from here a pinch
            m_state = State::Pinch;
            m_pinchCentre = (pts[0].position() + pts[1].position()) / 2;
            m_pinchDist = QLineF(pts[0].position(), pts[1].position()).length();
            return true;
        }
        const QPointF step = axisOnly(m_lastPos - m_pinchCentre);
        m_pinchCentre = m_lastPos;
        const qint64 now = m_clock.elapsed();
        if (!step.isNull()) {
            m_samples.append({now, m_lastPos});
            while (m_samples.size() > 2 && now - m_samples.first().ms > 4 * kVelocityWindowMs) m_samples.removeFirst();
        }
        if (!step.isNull()) onPan(step);
        if (!alive) return true;
        if (end) {
            m_state = State::Idle;
            startGlide(); // a real flick glides on (see Momentum), else the view stops with the finger
        }
        return true;
    }
    case State::Held:
        if (end) { // held and lifted without moving: the long press as before (the menu)
            m_state = State::Idle;
            if (onLongPress && onLongPress(m_pressPos)) return true;
            Touch::longPressAt(m_target, m_pressPos, Qt::KeyboardModifiers(m_mods));
            return true;
        }
        if (QLineF(m_pressPos, m_lastPos).length() < Touch::slopPx()) return true;
        m_state = State::Pressed; // held, then moved: the widget's own drag from the press point
        sendMouse(QEvent::MouseButtonPress, m_pressPos, true);
        if (alive && !TouchDrag::active()) sendMouse(QEvent::MouseMove, m_lastPos, true);
        return true;
    case State::Pressed:
        if (end) {
            m_state = State::Idle;
            sendMouse(QEvent::MouseButtonRelease, m_lastPos, false);
        } else {
            sendMouse(QEvent::MouseMove, m_lastPos, true);
        }
        return true;
    case State::Pinch: {
        if (end || live < 2) {
            if (end) m_state = State::Idle;
            else m_state = State::Done; // one finger left: nothing more until all are up
            if (onPinchEnd) onPinchEnd();
            return true;
        }
        // the two fingers that are down
        QPointF p[2];
        int n = 0;
        for (const auto& pt : pts)
            if (pt.state() != QEventPoint::Released && n < 2) p[n++] = pt.position();
        const QPointF centre = (p[0] + p[1]) / 2;
        const double dist = QLineF(p[0], p[1]).length();
        const double scale = m_pinchDist > 1 && dist > 1 ? dist / m_pinchDist : 1.0;
        const QPointF from = m_pinchCentre;
        m_pinchCentre = centre;
        m_pinchDist = dist;
        if (onPinch && (scale != 1.0 || from != centre)) onPinch(from, centre, scale);
        return true;
    }
    case State::Done:
    case State::Idle:
        if (end) m_state = State::Idle;
        return true;
    }
    return true;
}

void TouchGestures::longPress()
{
    if (m_state != State::Pending) return;
    if (m_pan) { // a panning surface: the hold arms the widget's own drag (the menu comes on a lift without moving)
        m_state = State::Held;
        m_lastTap.invalidate();
        if (onHoldCue) onHoldCue(m_pressPos);
        return;
    }
    m_state = State::Done; // the rest of this touch does nothing
    m_lastTap.invalidate();
    if (onLongPress && onLongPress(m_pressPos)) return;
    Touch::longPressAt(m_target, m_pressPos, Qt::KeyboardModifiers(m_mods)); // the menu, else a hint
}
