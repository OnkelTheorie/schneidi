// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/touch/TouchGestures.h"

#include "ui/touch/Touch.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QStyleHints>
#include <QTimer>
#include <QTouchEvent>
#include <QWidget>
#include <cmath>

TouchGestures::TouchGestures(QWidget* target) : QObject(target), m_target(target)
{
    target->setAttribute(Qt::WA_AcceptTouchEvents);
    target->installEventFilter(this);
    m_hold = new QTimer(this);
    m_hold->setSingleShot(true);
    m_hold->setInterval(Touch::longPressMs());
    connect(m_hold, &QTimer::timeout, this, &TouchGestures::longPress);
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
    const QPointF global = m_target->mapToGlobal(pos);
    const QPointF scene = m_target->window() ? m_target->mapTo(m_target->window(), pos) : pos;
    QMouseEvent me(t, pos, scene, global, button, down ? Qt::LeftButton : Qt::NoButton,
                   Qt::KeyboardModifiers(m_mods), m_device ? m_device.data() : QPointingDevice::primaryPointingDevice());
    QApplication::sendEvent(m_target, &me);
}

bool TouchGestures::touch(QTouchEvent* e)
{
    const auto& pts = e->points();
    const QPointer<TouchGestures> alive(this);
    e->accept();
    if (e->type() == QEvent::TouchBegin) {
        m_device = e->pointingDevice();
        m_mods = int(e->modifiers());
        m_state = State::Pending;
        m_pressPos = m_lastPos = pts.isEmpty() ? QPointF() : pts.first().position();
        m_lastGlobal = pts.isEmpty() ? QPointF() : pts.first().globalPosition();
        m_hold->start();
        if (pts.size() < 2) return true;
        // two fingers at once: straight into the pinch below
    }
    if (e->type() == QEvent::TouchCancel) {
        m_hold->stop();
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
        m_state = State::Pressed;
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
    m_state = State::Done; // the rest of this touch does nothing
    m_lastTap.invalidate();
    if (onLongPress && onLongPress(m_pressPos)) return;
    QContextMenuEvent ce(QContextMenuEvent::Mouse, m_pressPos.toPoint(), m_target->mapToGlobal(m_pressPos).toPoint(),
                         Qt::KeyboardModifiers(m_mods));
    QApplication::sendEvent(m_target, &ce);
}
