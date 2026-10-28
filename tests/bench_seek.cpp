// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): what a jump costs in the preview Engine while stopped (clicking into the timeline):
// time from seek() until the picture there is shown, for jumps to positions spread over the project.
// Run: QT_QPA_PLATFORM=offscreen ./build-tests/tests/bench_seek <project.schneidi> [jumps]
//      (on one core: taskset -c 0 …)
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <algorithm>
#include <cstdio>
#include <random>

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SDL_AUDIODRIVER", "dummy");
    QApplication app(argc, argv);
    Check::initApp("bench-seek");
    if (argc < 2) {
        std::printf("usage: bench_seek <project.schneidi> [jumps]\n");
        return 2;
    }
    ProjectData data;
    QString error;
    if (!ProjectFile::load(QString::fromLocal8Bit(argv[1]), &data, &error)) {
        std::printf("cannot load: %s\n", qPrintable(error));
        return 1;
    }
    Project project;
    project.load(data);
    const int jumps = argc > 2 ? atoi(argv[2]) : 30;
    Engine engine;
    if (!engine.init(&error)) {
        std::printf("Engine init failed: %s\n", qPrintable(error));
        return 77;
    }
    engine.setFormat(project.format());
    engine.updateTimeline(project.timeline());
    // Waits until the picture of `frame` is shown (or 10 s); ms
    int shownAt = -1;
    QObject::connect(&engine, &Engine::positionChanged, &app, [&](int f) { shownAt = f; });
    auto showAndWait = [&](int frame, auto&& start) {
        QElapsedTimer t;
        QEventLoop loop;
        auto c = QObject::connect(&engine, &Engine::frameReady, &loop, [&] {
            if (shownAt == frame) loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        t.start();
        start();
        loop.exec();
        QObject::disconnect(c);
        return t.nsecsElapsed() / 1e6;
    };
    showAndWait(0, [&] { engine.showTimeline(0); });
    int length = 0;
    for (const Track& t : project.timeline().video)
        for (const Clip& c : t.clips) length = std::max(length, c.end());
    std::mt19937 random(7); // the same jumps every run
    std::vector<double> ms;
    for (int i = 0; i < jumps; ++i) {
        const int to = int(random() % uint32_t(std::max(1, length - 1)));
        ms.push_back(showAndWait(to, [&] { engine.seek(to); }));
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double m : ms) sum += m;
    std::printf("project %s, %d jumps: median %.0f ms, mean %.0f ms, 90 %% %.0f ms, max %.0f ms\n", argv[1], jumps,
                ms[ms.size() / 2], sum / ms.size(), ms[ms.size() * 9 / 10], ms.back());
    return 0;
}
