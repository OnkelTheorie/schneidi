// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): what seeking costs in the preview Engine while stopped: jumps (clicking into the
// timeline), frame steps back/forward after a jump (arrow keys) and scrubbing (60 seeks in 1 s, then the time until the
// exact picture is there); always until the exact picture is shown.
// Run: QT_QPA_PLATFORM=offscreen ./build-tests/tests/bench_seek <project.schneidi> [jumps]
//      (on one core: taskset -c 0 …)
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QCoreApplication>
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
    // Pictures shown exactly (a close picture shown while scrubbing does not count)
    int shown = 0;
    QObject::connect(&engine, &Engine::frameReady, &app, [&] { ++shown; });
    auto exactShown = [&] {
#ifdef BENCH_BASELINE
        return shown;
#else
        const Picture::Pump::Stats s = engine.pictureStats();
        return s.shown - s.nearest;
#endif
    };
    // Waits until the exact picture after `start` is shown (or 10 s); ms
    auto showAndWait = [&](auto&& start) {
        const int before = exactShown(); // the picture before is there already (each step waits for it)
        QElapsedTimer t;
        t.start();
        start();
        while (exactShown() <= before && t.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        return t.nsecsElapsed() / 1e6;
    };
    auto pause = [&](int ms) { // seeks further apart than scrubbing
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    };
    auto report = [](const char* what, std::vector<double> ms) {
        std::sort(ms.begin(), ms.end());
        double sum = 0;
        for (double m : ms) sum += m;
        std::printf("%-12s n=%3zu: median %6.1f ms, mean %6.1f ms, 90 %% %6.1f ms, max %6.1f ms\n", what, ms.size(),
                    ms[ms.size() / 2], sum / ms.size(), ms[ms.size() * 9 / 10], ms.back());
    };
    showAndWait([&] { engine.showTimeline(0); });
    int length = 0;
    for (const Track& t : project.timeline().video)
        for (const Clip& c : t.clips) length = std::max(length, c.end());
    std::mt19937 random(7); // the same jumps every run
    std::printf("project %s\n", argv[1]);
    std::vector<double> ms;
    for (int i = 0; i < jumps; ++i) {
        const int to = int(random() % uint32_t(std::max(1, length - 1)));
        pause(200);
        ms.push_back(showAndWait([&] { engine.seek(to); }));
    }
    report("jump", ms);
    // Frame steps after a jump (arrow keys, one press at a time): back, then forward again over the same frames
    std::vector<double> back, fwd;
    for (int i = 0; i < 5; ++i) {
        const int at = 60 + int(random() % uint32_t(std::max(1, length - 120)));
        pause(200);
        showAndWait([&] { engine.seek(at); });
        for (int k = 1; k <= 12; ++k) {
            pause(200);
            back.push_back(showAndWait([&] { engine.seek(at - k); }));
        }
        for (int k = 11; k >= 0; --k) {
            pause(200);
            fwd.push_back(showAndWait([&] { engine.seek(at - k); }));
        }
    }
    report("step back", back);
    report("step fwd", fwd);
    // Scrubbing: 60 seeks in 1 s over 3 s of timeline (dragging the playhead), then the mouse stops: pictures shown
    // while dragging, and the time from the last seek until the exact picture is there
    std::vector<double> settle;
    std::vector<double> during;
    for (int i = 0; i < 6; ++i) {
        const int from = int(random() % uint32_t(std::max(1, length - 200)));
        const int dir = i % 2 ? -1 : 1;
        const int a = dir > 0 ? from : from + 180;
        pause(300);
        showAndWait([&] { engine.seek(a); });
        const int shownBefore = shown;
        QElapsedTimer t;
        t.start();
        for (int k = 1; k <= 60; ++k) {
            while (t.elapsed() < k * 1000 / 60) QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
            if (k < 60) engine.seek(a + dir * k * 3);
        }
        during.push_back(shown - shownBefore);
        settle.push_back(showAndWait([&] { engine.seek(a + dir * 180); }));
    }
    report("scrub shown", during);
    report("scrub settle", settle);
    return 0;
}
