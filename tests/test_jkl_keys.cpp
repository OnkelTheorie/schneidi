// SPDX-License-Identifier: AGPL-3.0-only
// J/K/L (app/JklKeys) through real key presses and releases on window shortcuts (offscreen): J/L faster with every
// press in both step tables (doubling as before, gentle 1-2-3-5-8 by choice), K stops; K held + J/L tapped = one
// frame back/forward, K held + J/L held = half speed backwards/forwards until a key is let go; a rebound K works the
// same; leaving the app lets go. Plus the step table itself (TimelineOptions::shuttleSpeed).

#include "check.h"

#include "app/JklKeys.h"
#include "ui/timeline/TimelineOptions.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QTest>
#include <QWidget>
#include <cmath>

namespace {

using TimelineOptions::ShuttleSteps;

void testSpeedTable()
{
    using TimelineOptions::shuttleSpeed;
    // Doubling (as before): 1 2 4 8 16 16; the other way or paused starts at 1x; half speed goes to 1x
    double s = 0;
    for (double want : {1.0, 2.0, 4.0, 8.0, 16.0, 16.0}) CHECK_EQ(s = shuttleSpeed(s, 1, ShuttleSteps::Double), want);
    CHECK_EQ(shuttleSpeed(16, -1, ShuttleSteps::Double), -1.0);
    CHECK_EQ(shuttleSpeed(-4, -1, ShuttleSteps::Double), -8.0);
    CHECK_EQ(shuttleSpeed(0.5, 1, ShuttleSteps::Double), 1.0);
    CHECK_EQ(shuttleSpeed(-0.5, -1, ShuttleSteps::Double), -1.0);
    // Gentle: 1 2 3 5 8 8
    s = 0;
    for (double want : {-1.0, -2.0, -3.0, -5.0, -8.0, -8.0}) CHECK_EQ(s = shuttleSpeed(s, -1, ShuttleSteps::Gentle), want);
    CHECK_EQ(shuttleSpeed(-8, 1, ShuttleSteps::Gentle), 1.0);
    CHECK_EQ(shuttleSpeed(4, 1, ShuttleSteps::Gentle), 5.0); // from a doubling speed: the next level up
    CHECK_EQ(shuttleSpeed(16, 1, ShuttleSteps::Gentle), 8.0);
}

struct Window {
    QWidget w;
    QAction* back = new QAction(&w);
    QAction* stop = new QAction(&w);
    QAction* forward = new QAction(&w);
    double speed = 0;
    int position = 100;
    int steps = 0;
    JklKeys* keys = nullptr;

    Window()
    {
        back->setShortcut(QKeySequence("J"));
        stop->setShortcut(QKeySequence("K"));
        forward->setShortcut(QKeySequence("L"));
        w.addActions({back, stop, forward});
        keys = new JklKeys(back, stop, forward,
                           {[this] { return speed; }, [this](double s) { speed = s; },
                            [this](int frames) {
                                speed = 0;
                                position += frames;
                                ++steps;
                            },
                            [this] { speed = 0; }},
                           &w);
        QObject::connect(back, &QAction::triggered, keys, [this] { keys->shuttlePressed(-1); });
        QObject::connect(stop, &QAction::triggered, keys, [this] { keys->stopPressed(); });
        QObject::connect(forward, &QAction::triggered, keys, [this] { keys->shuttlePressed(1); });
        w.resize(300, 200);
        w.show();
        w.activateWindow();
        w.setFocus();
        QApplication::processEvents();
    }
    void press(Qt::Key k) { QTest::keyPress(&w, k); }
    void release(Qt::Key k) { QTest::keyRelease(&w, k); }
    void tap(Qt::Key k)
    {
        press(k);
        release(k);
    }
};

void testShuttle()
{
    QSettings().remove("timeline");
    Window win;
    // L L L: 1x 2x 4x (doubling, the default); J: 1x backwards; K stops
    win.tap(Qt::Key_L);
    CHECK_EQ(win.speed, 1.0);
    win.tap(Qt::Key_L);
    win.tap(Qt::Key_L);
    CHECK_EQ(win.speed, 4.0);
    win.tap(Qt::Key_J);
    CHECK_EQ(win.speed, -1.0);
    win.tap(Qt::Key_K);
    CHECK_EQ(win.speed, 0.0);
    CHECK(!win.keys->stopHeld());
    CHECK_EQ(win.steps, 0);
    // Gentle by choice: 1 2 3 5
    TimelineOptions::setShuttleSteps(ShuttleSteps::Gentle);
    for (double want : {1.0, 2.0, 3.0, 5.0}) {
        win.tap(Qt::Key_L);
        CHECK_EQ(win.speed, want);
    }
    win.tap(Qt::Key_K);
    QSettings().remove("timeline");
}

void testHoldStop()
{
    Window win;
    // K held, L tapped twice: two frames forward, paused; J tapped: one back
    win.press(Qt::Key_K);
    CHECK(win.keys->stopHeld());
    win.tap(Qt::Key_L);
    CHECK_EQ(win.position, 101);
    win.tap(Qt::Key_L);
    CHECK_EQ(win.position, 102);
    win.tap(Qt::Key_J);
    CHECK_EQ(win.position, 101);
    CHECK_EQ(win.speed, 0.0);
    QTest::qWait(JklKeys::kHoldMs + 100); // taps only: nothing plays afterwards
    CHECK_EQ(win.speed, 0.0);
    CHECK_EQ(win.steps, 3);
    // K + L held: one frame, then half speed forwards; letting go of L stops
    win.press(Qt::Key_L);
    CHECK_EQ(win.position, 102);
    QTest::qWait(JklKeys::kHoldMs + 100);
    CHECK_EQ(win.speed, 0.5);
    win.press(Qt::Key_L); // the held key's repeat changes nothing
    CHECK_EQ(win.speed, 0.5);
    CHECK_EQ(win.position, 102);
    win.press(Qt::Key_K); // nor does K's
    CHECK_EQ(win.speed, 0.5);
    win.release(Qt::Key_L);
    CHECK_EQ(win.speed, 0.0);
    // K + J held: half speed backwards; letting go of K stops
    win.press(Qt::Key_J);
    QTest::qWait(JklKeys::kHoldMs + 100);
    CHECK_EQ(win.speed, -0.5);
    win.release(Qt::Key_K);
    CHECK_EQ(win.speed, 0.0);
    CHECK(!win.keys->stopHeld());
    win.release(Qt::Key_J);
    // K let go: J/L shuttle again (from paused: 1x)
    win.tap(Qt::Key_L);
    CHECK_EQ(win.speed, 1.0);
    // While it plays, K stops (and is held: L then steps)
    win.press(Qt::Key_K);
    CHECK_EQ(win.speed, 0.0);
    const int at = win.position;
    win.tap(Qt::Key_L);
    CHECK_EQ(win.position, at + 1);
    win.release(Qt::Key_K);
}

void testRebound()
{
    // K on another key (the user's binding): its press and release count
    Window win;
    win.stop->setShortcut(QKeySequence("S"));
    win.press(Qt::Key_K); // no longer the stop key
    CHECK(!win.keys->stopHeld());
    win.release(Qt::Key_K);
    win.press(Qt::Key_S);
    CHECK(win.keys->stopHeld());
    win.release(Qt::Key_K); // K is no longer the stop key
    CHECK(win.keys->stopHeld());
    win.tap(Qt::Key_L);
    CHECK_EQ(win.position, 101);
    win.release(Qt::Key_S);
    CHECK(!win.keys->stopHeld());
    win.tap(Qt::Key_L);
    CHECK_EQ(win.speed, 1.0);
    // Shift+S is not the stop key: not held
    win.press(Qt::Key_K);
    QTest::keyPress(&win.w, Qt::Key_S, Qt::ShiftModifier);
    CHECK(!win.keys->stopHeld());
    QTest::keyRelease(&win.w, Qt::Key_S, Qt::ShiftModifier);
}

void testTriggeredWithoutKey()
{
    // The stop action from the menu (or an AI's "stop"): it stops, but K is not held - L shuttles afterwards
    Window win;
    win.tap(Qt::Key_L);
    win.stop->trigger();
    CHECK_EQ(win.speed, 0.0);
    CHECK(!win.keys->stopHeld());
    win.tap(Qt::Key_L);
    CHECK_EQ(win.speed, 1.0);
    CHECK_EQ(win.steps, 0);
    // Leaving the app lets go of a held K
    win.press(Qt::Key_K);
    CHECK(win.keys->stopHeld());
    emit qApp->applicationStateChanged(Qt::ApplicationInactive); // (as the platform reports it)
    CHECK(!win.keys->stopHeld());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("jkl_keys");
    testSpeedTable();
    testShuttle();
    testHoldStop();
    testRebound();
    testTriggeredWithoutKey();
    return Check::result();
}
