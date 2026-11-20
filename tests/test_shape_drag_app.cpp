// SPDX-License-Identifier: AGPL-3.0-only
// The whole app: dragging a shape's corner in the viewer, the picture follows the box at once - it stayed a step
// behind for a while, the right edge short of the box (user report 2026-11-20)
#include "check.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/ViewerOverlay.h"
#include "ui/FrameView.h"
#include "ui/ShapeOverlay.h"
#include "ui/Viewer.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QTest>
#include <algorithm>
#include <clocale>
#include <vector>
#include <cmath>
#include <cstdio>

namespace {

void settle(int ms = 0)
{
    QElapsedTimer t;
    t.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (ms) QTest::qWait(1);
    } while (t.elapsed() < ms);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("shape-drag-app");
    Theme::apply(app);
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    const bool realScreen = qEnvironmentVariableIsSet("SHAPE_SCREEN"); // a real display: what reaches the screen
    if (realScreen) w.move(0, 0);
    w.show();
    CHECK(QTest::qWaitForWindowExposed(&w));
    w.showPage(MainWindow::Page::Edit);
    w.editor()->addTitle(0, -1, 300, TitleKind::Shape);
    settle(500);
    auto* shape = w.findChild<ShapeOverlay*>();
    FrameView* screen = nullptr;
    for (Viewer* v : w.findChildren<Viewer*>())
        if (v->isVisible()) screen = v->screen();
    if (!CHECK(shape && screen)) return Check::result();
    settle(500);
    const QRectF frame = screen->frameRect();
    std::printf("frame %.0f,%.0f %.0fx%.0f\n", frame.x(), frame.y(), frame.width(), frame.height());
    int worst = 0, stale = 0;
    for (int round = 0; round < 6; ++round) {
        const QPointF corner = shape->handlePos(4, frame);
        QMouseEvent press(QEvent::MouseButtonPress, corner, corner, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        if (!CHECK(shape->mousePress(&press, frame))) break;
        QPointF at = corner;
        const double dir = round % 2 ? -1 : 1;
        for (int i = 0; i < 30; ++i) {
            at += QPointF(dir * 3, dir * 1);
            QMouseEvent move(QEvent::MouseMove, at, at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            shape->mouseMove(&move, frame);
            settle(8);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        shape->mouseRelease(&release, frame);
        // The box's right edge in widget pixels; the picture must be white just inside and black just outside
        const QPointF right = shape->handlePos(3, frame);
        QElapsedTimer t;
        t.start();
        bool ok = false;
        while (t.elapsed() < 3000 && !ok) {
            settle(5);
            QImage g;
            if (realScreen) { // what the compositor shows (a QRhiWidget's picture included), in widget pixels
                const QPoint o = screen->mapToGlobal(QPoint(0, 0));
                g = QGuiApplication::primaryScreen()->grabWindow(0, o.x(), o.y(), screen->width(), screen->height()).toImage();
                if (g.size() != screen->size()) g = g.scaled(screen->size());
            } else {
                g = screen->grab().toImage();
            }
            const int y = int(std::lround(right.y())) + 12; // below the handle; the ellipse is ~2 px in there
            ok = qGray(g.pixel(int(right.x()) - 7, y)) > 150 && qGray(g.pixel(int(right.x()) + 6, y)) < 60;
        }
        if (!ok && qEnvironmentVariableIsSet("SHAPE_DUMP")) {
            screen->grab().toImage().save(QString("%1/round%2.png").arg(qEnvironmentVariable("SHAPE_DUMP")).arg(round));
            std::printf("box right %.1f,%.1f\n", right.x(), right.y());
        }
        std::printf("round %d: picture matches the box after %lld ms%s\n", round, t.elapsed(), ok ? "" : " (never)");
        if (t.elapsed() > 100) ++stale;
        worst = std::max<int>(worst, int(t.elapsed()));
        CHECK(ok);
    }
    std::printf("slowest %d ms, %d of 6 late\n", worst, stale);
    // While dragging (a mouse move every 7 ms, a 144 Hz mouse): how far the picture's right edge trails the box
    {
        const QPointF corner = shape->handlePos(4, frame);
        QMouseEvent press(QEvent::MouseButtonPress, corner, corner, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        CHECK(shape->mousePress(&press, frame));
        QPointF at = corner;
        double sum = 0;
        int n = 0;
        for (int i = 0; i < 60; ++i) {
            at += QPointF(1, 0);
            QMouseEvent move(QEvent::MouseMove, at, at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            shape->mouseMove(&move, frame);
            settle(7);
            if (i < 10) continue; // under way
            const QPointF right = shape->handlePos(3, frame);
            const QImage g = screen->grab().toImage();
            const int y = int(std::lround(right.y())) + 12;
            int edge = int(right.x()) - 2; // left of the box's dashed line (growing: the picture is inside the box)
            while (edge > 0 && qGray(g.pixel(edge, y)) < 100) --edge;
            sum += right.x() - edge;
            ++n;
        }
        QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        shape->mouseRelease(&release, frame);
        std::printf("while dragging: the picture trails the box by %.1f px on average (1 px per 7 ms)\n", sum / n);
    }
    CHECK(worst < 150);
    // Latency in ms (SHAPE_LATENCY): each step of a drag, from the mouse move until a picture with the new right
    // edge reaches the engine's consumers, and how long the move itself kept the UI thread
    if (qEnvironmentVariableIsSet("SHAPE_LATENCY")) {
        QImage latest;
        QElapsedTimer clock;
        clock.start();
        qint64 latestAt = 0;
        QObject::connect(&engine, &Engine::frameReady, &w, [&](const QImage& img) {
            latest = img;
            latestAt = clock.nsecsElapsed();
        });
        const QPointF corner = shape->handlePos(4, frame);
        QMouseEvent press(QEvent::MouseButtonPress, corner, corner, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        CHECK(shape->mousePress(&press, frame));
        QPointF at = corner;
        std::vector<double> lat, ui;
        for (int i = 0; i < 40; ++i) {
            at += QPointF(2, 0);
            QMouseEvent move(QEvent::MouseMove, at, at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            const qint64 t0 = clock.nsecsElapsed();
            shape->mouseMove(&move, frame);
            ui.push_back((clock.nsecsElapsed() - t0) / 1e6);
            // The box's right edge as a fraction of the frame; wait for a picture whose edge is there
            const double want = (shape->handlePos(3, frame).x() - frame.left()) / frame.width();
            while (clock.nsecsElapsed() - t0 < 1'000'000'000LL) {
                QApplication::processEvents(QEventLoop::AllEvents, 1);
                if (latest.isNull() || latestAt < t0) continue;
                const int y = latest.height() / 2;
                int x = latest.width() - 1;
                while (x > 0 && qGray(latest.pixel(x, y)) < 100) --x;
                if (std::abs(double(x + 1) / latest.width() - want) < 2.5 / frame.width()) break;
            }
            lat.push_back((latestAt - t0) / 1e6);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        shape->mouseRelease(&release, frame);
        settle(1000); // the viewer's latency report (log)
        std::sort(lat.begin(), lat.end());
        std::sort(ui.begin(), ui.end());
        std::printf("latency move -> picture: median %.1f ms, max %.1f ms; the move itself on the UI thread: median %.1f "
                    "ms, max %.1f ms\n", lat[lat.size() / 2], lat.back(), ui[ui.size() / 2], ui.back());
    }
    return Check::result();
}
