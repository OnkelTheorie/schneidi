// SPDX-License-Identifier: AGPL-3.0-only
// ScreenAwake keeps the monitor on while the timeline plays: on Linux it asks org.freedesktop.ScreenSaver for an
// inhibition and gives it back when playback stops. The test runs its own private session bus (dbus-daemon) with a
// fake screen saver service, so the real desktop is never touched. Skipped without D-Bus support or dbus-daemon.

#include "check.h"

#include "ui/ScreenAwake.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>

#include <cstdio>
#include <functional>
#include <memory>

#ifdef SCHNEIDI_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusConnectionInterface>

class FakeScreenSaver : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    int inhibits = 0;
    QList<uint> held;
    QString app, reason;

public slots:
    uint Inhibit(const QString& application, const QString& why)
    {
        ++inhibits;
        app = application;
        reason = why;
        const uint cookie = 100 + uint(inhibits);
        held << cookie;
        return cookie;
    }
    void UnInhibit(uint cookie) { held.removeAll(cookie); }
};

namespace {

void spin(const std::function<bool()>& done, int ms = 2000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

} // namespace
#endif

int main(int argc, char** argv)
{
    Check::initEnv();
#ifndef SCHNEIDI_HAVE_DBUS
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    std::puts("skipped: built without D-Bus");
    return 77;
#else
    const QString daemon = QStandardPaths::findExecutable("dbus-daemon");
    if (daemon.isEmpty()) {
        std::puts("skipped: no dbus-daemon");
        return 77;
    }
    // A private session bus: its address must be set before the first use of QDBusConnection::sessionBus()
    QProcess bus;
    bus.start(daemon, {"--session", "--nofork", "--print-address"});
    if (!bus.waitForStarted(3000) || !bus.waitForReadyRead(3000)) {
        std::puts("skipped: dbus-daemon did not start");
        return 77;
    }
    const QByteArray address = bus.readLine().trimmed();
    qputenv("DBUS_SESSION_BUS_ADDRESS", address);

    QCoreApplication app(argc, argv);
    Check::initApp("screen_awake");

    QDBusConnection session = QDBusConnection::sessionBus();
    if (!CHECK(session.isConnected())) return Check::result();

    // 1. No screen saver on the bus: playing must not fail or hold anything
    {
        ScreenAwake awake;
        awake.setActive(true);
        spin([] { return false; }, 300);
        CHECK(!awake.holding());
        awake.setActive(false);
    }

    FakeScreenSaver saver;
    CHECK(session.registerObject("/org/freedesktop/ScreenSaver", &saver, QDBusConnection::ExportAllSlots));
    CHECK(session.registerService("org.freedesktop.ScreenSaver"));

    // 2. Play: one inhibition, with the app's name and a reason; stop: given back
    {
        ScreenAwake awake;
        awake.setActive(true);
        awake.setActive(true); // a speed change while playing: no second request
        spin([&] { return awake.holding(); });
        CHECK(awake.holding());
        CHECK_EQ(saver.inhibits, 1);
        CHECK_EQ(saver.held.size(), 1);
        CHECK(!saver.reason.isEmpty());
        awake.setActive(false);
        spin([&] { return saver.held.isEmpty(); });
        CHECK(!awake.holding());
        CHECK(saver.held.isEmpty());
    }

    // 3. Stopped before the desktop answered: the late cookie is given back too
    {
        ScreenAwake awake;
        awake.setActive(true);
        awake.setActive(false);
        spin([&] { return saver.inhibits == 2 && saver.held.isEmpty(); });
        CHECK_EQ(saver.inhibits, 2);
        CHECK(saver.held.isEmpty());
        CHECK(!awake.holding());
    }

    // 4. Closed while playing (the app quits): given back
    {
        auto awake = std::make_unique<ScreenAwake>();
        awake->setActive(true);
        spin([&] { return awake->holding(); });
        CHECK_EQ(saver.held.size(), 1);
        awake.reset();
        spin([&] { return saver.held.isEmpty(); });
        CHECK(saver.held.isEmpty());
    }

    session.unregisterService("org.freedesktop.ScreenSaver");
    bus.terminate();
    bus.waitForFinished(3000);
    return Check::result();
#endif
}

#include "test_screen_awake.moc"
