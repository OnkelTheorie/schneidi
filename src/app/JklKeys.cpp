// SPDX-License-Identifier: AGPL-3.0-only
#include "app/JklKeys.h"

#include "ui/timeline/TimelineOptions.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>

JklKeys::JklKeys(QAction* back, QAction* stop, QAction* forward, Hooks hooks, QObject* parent)
    : QObject(parent), m_back(back), m_stop(stop), m_forward(forward), m_hooks(std::move(hooks))
{
    m_hold.setSingleShot(true);
    m_hold.setInterval(kHoldMs);
    connect(&m_hold, &QTimer::timeout, this, [this] {
        if (!m_stopHeld || m_heldDir == 0) return;
        m_slow = true;
        m_hooks.setSpeed(0.5 * m_heldDir);
    });
    qApp->installEventFilter(this);
    // A key let go outside the app never arrives: leaving the app lets go of everything
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) releaseAll();
    });
}

JklKeys::~JklKeys()
{
    if (qApp) qApp->removeEventFilter(this);
}

void JklKeys::stopPressed()
{
    // (Held or not comes from the key events, eventFilter: K from the menu or an AI's "stop" only stops.) The key's
    // own repeat while held only stops; a running K+J/L half speed goes on until a key is let go
    if (m_slow) return;
    m_hooks.pause();
}

void JklKeys::shuttlePressed(int direction)
{
    if (m_stopHeld) {
        if (m_heldDir == direction) return; // the held key's repeat
        if (m_slow) { // the other direction while half speed plays: that way
            m_heldDir = direction;
            m_hooks.setSpeed(0.5 * direction);
            return;
        }
        m_heldDir = direction;
        m_hooks.step(direction); // a tap: one frame
        m_hold.start();          // still down after kHoldMs: half speed
        return;
    }
    const double next = TimelineOptions::shuttleSpeed(m_hooks.speed(), direction, TimelineOptions::shuttleSteps());
    m_hooks.setSpeed(next);
}

bool JklKeys::pressMatches(QKeyCombination bound, const QKeyEvent* e)
{
    // As Qt's shortcut map matches: the number pad's flag does not count, and Shift does not where it only types the
    // symbol ('<' is Shift+, on a US keyboard, unshifted on a German one). Shift+letter stays a key of its own
    const Qt::KeyboardModifiers ignore = Qt::KeypadModifier;
    const Qt::KeyboardModifiers want = bound.keyboardModifiers() & ~ignore;
    const Qt::KeyboardModifiers got = e->modifiers() & ~ignore;
    const int key = e->key();
    if (bound.key() != Qt::Key(key)) return false;
    if (want == got) return true;
    const bool symbol = key < 0x01000000 && !(key >= Qt::Key_A && key <= Qt::Key_Z) && !(key >= Qt::Key_0 && key <= Qt::Key_9);
    return symbol && got.testFlag(Qt::ShiftModifier) && !want.testFlag(Qt::ShiftModifier) && want == (got & ~Qt::ShiftModifier);
}

bool JklKeys::isKeyOf(const QAction* action, const QKeyEvent* e, bool press)
{
    if (!action) return false;
    for (const QKeySequence& seq : action->shortcuts())
        if (seq.count() == 1 && (press ? pressMatches(seq[0], e) : seq[0].key() == Qt::Key(e->key()))) // released: modifiers may go first
            return true;
    return false;
}

void JklKeys::releaseShuttle()
{
    m_hold.stop();
    m_heldDir = 0;
    if (m_slow) {
        m_slow = false;
        m_hooks.pause();
    }
}

void JklKeys::releaseAll()
{
    m_stopHeld = false;
    releaseShuttle();
}

bool JklKeys::eventFilter(QObject* watched, QEvent* event)
{
    // A press reaches the focus widget as ShortcutOverride before the shortcut fires (the KeyPress itself never comes
    // when an action takes it); a release as KeyRelease. Either may pass here more than once (the window, the focus
    // widget, its parents): holding and letting go are idempotent
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* e = static_cast<const QKeyEvent*>(event);
        if (!e->isAutoRepeat() && isKeyOf(m_stop, e, true)) m_stopHeld = true;
    } else if (event->type() == QEvent::KeyRelease) {
        const auto* e = static_cast<const QKeyEvent*>(event);
        if (!e->isAutoRepeat()) {
            if (isKeyOf(m_stop, e, false)) releaseAll();
            else if ((m_heldDir < 0 && isKeyOf(m_back, e, false)) || (m_heldDir > 0 && isKeyOf(m_forward, e, false)))
                releaseShuttle();
        }
    }
    return QObject::eventFilter(watched, event);
}
