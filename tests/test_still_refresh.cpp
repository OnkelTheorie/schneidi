// SPDX-License-Identifier: AGPL-3.0-only
// Paused, the viewer follows edits at once: a shape sized in quick steps (dragging a handle) shows its last size
// soon after the last step - the picture must not stay a step behind the box drawn over it (user report 2026-11-20)
#include "check.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace {

const ProjectFormat kFmt{1920, 1080, {60, 1}};

Clip rect(double w)
{
    Clip c;
    c.id = 1;
    c.kind = ClipKind::Title;
    c.title.kind = TitleKind::Shape;
    c.title.text.clear();
    c.title.color = Qt::white;
    c.title.shape.kind = ShapeKind::Rect;
    c.title.shape.width = w;
    c.title.shape.height = 400;
    c.out = 299;
    return c;
}

Timeline timelineWith(double w, double x)
{
    Timeline tl;
    TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
    Clip c = rect(w);
    c.title.posX = x;
    tl.video[0].clips = {c};
    return tl;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Right edge of the white box on the middle row, as a fraction of the width (-1: none)
double rightEdge(const QImage& img)
{
    const int y = img.height() / 2;
    for (int x = img.width() - 1; x >= 0; --x)
        if (qGray(img.pixel(x, y)) > 128) return double(x + 1) / img.width();
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("still_refresh");
    Check::initEngine();
    std::mutex lock;
    QImage last;
    int frames = 0;
    Engine engine;
    QString error;
    if (!engine.init(&error)) return Check::skip(qPrintable(error));
    engine.setFormat(kFmt);
    QObject::connect(&engine, &Engine::frameReady, &engine, [&](const QImage& img) {
        std::lock_guard<std::mutex> g(lock);
        last = img;
        ++frames;
    }, Qt::DirectConnection);
    engine.updateTimeline(timelineWith(400, 0));
    engine.showTimeline(30);
    wait(800);
    // A drag of the right edge: the left one stays (x moves by half the width change), one step per 8 ms
    int worst = 0;
    for (int round = 0; round < 5; ++round) {
        double w = 400;
        for (int i = 0; i < 40; ++i) {
            w += 6;
            engine.updateTimeline(timelineWith(w, (w - 400) / 2));
            wait(8);
        }
        const double want = (960 + (w - 400) / 2 + w / 2) / 1920;
        QElapsedTimer t;
        t.start();
        double edge = -1;
        while (t.elapsed() < 3000) {
            {
                std::lock_guard<std::mutex> g(lock);
                if (!last.isNull()) edge = rightEdge(last);
            }
            if (std::abs(edge - want) < 0.004) break;
            wait(2);
        }
        std::printf("round %d: right edge %.4f, wanted %.4f, after %lld ms (%d frames shown)\n", round, edge, want,
                    t.elapsed(), frames);
        worst = std::max<int>(worst, int(t.elapsed()));
        CHECK(std::abs(edge - want) < 0.004);
        engine.updateTimeline(timelineWith(400, 0));
        wait(300);
    }
    std::printf("slowest: %d ms\n", worst);
    CHECK(worst < 250);
    return Check::result();
}
