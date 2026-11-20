// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/ScreenAwake.h"

#include "core/I18n.h"

#include <QCoreApplication>

#ifdef SCHNEIDI_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

#ifdef SCHNEIDI_HAVE_DBUS
constexpr const char* kService = "org.freedesktop.ScreenSaver";
constexpr const char* kPath = "/org/freedesktop/ScreenSaver";
constexpr const char* kInterface = "org.freedesktop.ScreenSaver";

void uninhibit(uint cookie)
{
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kInterface, "UnInhibit");
    m << cookie;
    QDBusConnection::sessionBus().asyncCall(m);
}
#endif

} // namespace

ScreenAwake::ScreenAwake(QObject* parent) : QObject(parent) {}

ScreenAwake::~ScreenAwake()
{
    release();
}

bool ScreenAwake::holding() const
{
    return m_cookie != 0 || m_windowsHeld;
}

void ScreenAwake::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    if (!active) {
        release();
        return;
    }
#ifdef Q_OS_WIN
    // The thread's state: the GUI thread calls this for both directions
    m_windowsHeld = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) != 0;
#elif defined(SCHNEIDI_HAVE_DBUS)
    if (m_waiting || m_cookie != 0) return; // the answer to an earlier request is on its way, or one is held
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kInterface, "Inhibit");
    // The reason is shown by the desktop (e.g. KDE's power applet)
    m << QCoreApplication::applicationName() << T("Video wird abgespielt");
    m_waiting = true;
    // Asynchronous: a desktop without the service must not hold up play
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(m), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        m_waiting = false;
        const QDBusPendingReply<uint> reply = *w;
        if (reply.isError()) return; // no screen saver service: nothing to keep off
        const uint cookie = reply.value();
        if (m_active) m_cookie = cookie;
        else uninhibit(cookie); // stopped before the desktop answered
    });
#endif
}

void ScreenAwake::release()
{
#ifdef Q_OS_WIN
    if (m_windowsHeld) SetThreadExecutionState(ES_CONTINUOUS);
    m_windowsHeld = false;
#elif defined(SCHNEIDI_HAVE_DBUS)
    if (m_cookie != 0) uninhibit(m_cookie);
    m_cookie = 0;
#endif
}
