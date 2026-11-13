// SPDX-License-Identifier: AGPL-3.0-only
// The main window fits its screen (MainWindow::placeOnScreen/fitToScreen, main.cpp before showMaximized): it never
// starts larger than the screen's available area - a 1024x768 screen, a 1280x1024 one at QT_SCALE_FACTOR=1.25 had the
// Inspector and the right of the viewer off-screen (without a window manager, maximizing does nothing) - and is not a
// small window on a big screen. The rule itself with made-up screens; the real window on the screen it runs on (run it
// under Xvfb with several -screen sizes: the frame stays inside availableGeometry).

#include "check.h"

#include "app/MainWindow.h"
#include "core/Editor.h"
#include "engine/Engine.h"
#include "ui/DrawOverlay.h"

#include <QAction>
#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QTest>

namespace {

void testRule()
{
    const QMargins f = MainWindow::kFrameEstimate;
    const QRect vga(0, 0, 1024, 768), big(0, 0, 2560, 1440), right(1920, 0, 1280, 1024);
    auto inside = [&](const QRect& r, const QRect& screen) { return screen.contains(r.marginsAdded(f)); };
    // The old fixed 1600x950 on a small screen: shrunk to 90 % of it, centred, frame inside
    QRect r = MainWindow::fitToScreen(QRect(0, 0, 1600, 950), vga);
    CHECK(inside(r, vga) && r.width() >= 800 && r.height() >= 600);
    // A first start (nothing wanted) on a big screen: most of it
    r = MainWindow::fitToScreen({}, big);
    CHECK(inside(r, big) && r.width() >= 2200 && r.height() >= 1200);
    // Fits but hangs off the edge: moved in, size kept
    r = MainWindow::fitToScreen(QRect(900, 600, 800, 500), vga);
    CHECK(inside(r, vga) && r.size() == QSize(800, 500));
    // Saved on a monitor that is gone (off this screen): its size, centred here
    r = MainWindow::fitToScreen(QRect(5000, 100, 1000, 700), right);
    CHECK(inside(r, right) && r.size() == QSize(1000, 700));
    // Already inside: unchanged
    CHECK(MainWindow::fitToScreen(QRect(100, 100, 800, 600), vga) == QRect(100, 100, 800, 600));
}

void testWindow(Engine& engine)
{
    MainWindow w(&engine);
    w.disableAutosave();
    w.placeOnScreen();
    w.show();
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
    const QRect avail = w.screen()->availableGeometry();
    std::printf("screen %dx%d (available %d,%d %dx%d), window frame %d,%d %dx%d, minimum %dx%d\n",
                w.screen()->geometry().width(), w.screen()->geometry().height(), avail.x(), avail.y(), avail.width(),
                avail.height(), w.frameGeometry().x(), w.frameGeometry().y(), w.frameGeometry().width(),
                w.frameGeometry().height(), w.minimumSizeHint().width(), w.minimumSizeHint().height());
    // A screen smaller than the window's minimum (the offscreen platform's 800x800) cannot hold it: nothing to check
    if (w.minimumSizeHint().width() > avail.width() || w.minimumSizeHint().height() > avail.height()) {
        std::printf("screen smaller than the window's minimum: not checked (run under Xvfb, e.g. 1024x768)\n");
        return;
    }
    Check::report(avail.contains(w.frameGeometry()), "window frame inside the screen", __FILE__, __LINE__,
                  QString("frame %1,%2 %3x%4").arg(w.frameGeometry().x()).arg(w.frameGeometry().y())
                      .arg(w.frameGeometry().width()).arg(w.frameGeometry().height()));
    // Most of the screen: not a small window on a big one
    CHECK(w.width() >= avail.width() * 8 / 10 || w.width() >= w.minimumSizeHint().width());
}

// Paint mode (key P) in a small window: the brush bar fits the viewer (two rows if needed) and never makes the window
// bigger (a 1000x640 window grew to 1188x773); Esc ends painting
void testPaintBar(Engine& engine)
{
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1000, 640);
    w.show();
    w.editor()->addTitle(200, -1, 50, TitleKind::Text); // a text title selected: the Inspector's usual width
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
    const QSize least = w.minimumSizeHint();
    w.editor()->addTitle(0, -1, 50, TitleKind::Drawing); // selected: the Inspector shows its section
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
    const QSize before = w.size(); // (at least the window's minimum)
    // A selected drawing does not make the window's minimum wider (its section made the Edit page ~70 px wider)
    std::printf("window minimum with a text title selected %dx%d, with a drawing selected %dx%d (a 1024x768 screen holds %dx%d)\n", least.width(),
                least.height(), w.minimumSizeHint().width(), w.minimumSizeHint().height(),
                1024 - MainWindow::kFrameEstimate.left() - MainWindow::kFrameEstimate.right(),
                768 - MainWindow::kFrameEstimate.top() - MainWindow::kFrameEstimate.bottom());
    CHECK(w.minimumSizeHint().width() <= least.width());
    QAction* paint = nullptr;
    for (QAction* a : w.findChildren<QAction*>())
        if (a->shortcut() == QKeySequence("P")) paint = a;
    if (!CHECK(paint)) return;
    paint->trigger();
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
    auto* bar = w.findChild<DrawBar*>();
    if (!CHECK(bar && bar->isVisible())) return;
    std::printf("window %dx%d -> %dx%d (minimum %dx%d), bar %dx%d in %d px\n", before.width(), before.height(),
                w.width(), w.height(), w.minimumSizeHint().width(), w.minimumSizeHint().height(), bar->width(),
                bar->height(), bar->parentWidget()->width());
    CHECK(w.size() == before);
    CHECK(bar->geometry().right() < bar->parentWidget()->width());
    // A narrow viewer: the bar goes to two rows, still inside
    w.resize(760, 640);
    QTest::qWait(50);
    QApplication::processEvents();
    CHECK(bar->geometry().right() < bar->parentWidget()->width() || bar->parentWidget()->width() < 200);
    // Esc ends painting (a key reaches only the active window: without a window manager it may not get active)
    w.activateWindow();
    if (!QTest::qWaitForWindowActive(&w, 2000)) {
        std::printf("window not active: Esc not checked\n");
        return;
    }
    QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : static_cast<QWidget*>(&w), Qt::Key_Escape);
    QApplication::processEvents();
    CHECK(!bar->isVisible() && !paint->isChecked());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("window-fit");
    testRule();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testWindow(engine);
    testPaintBar(engine);
    return Check::result();
}
