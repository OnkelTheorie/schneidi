// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): playback of a real project in the preview Engine, like pressing play in the app.
// Reports shown frames per second and the longest gaps (stutter) per second of playback; with the own audio engine
// (the default; not with SCHNEIDI_AUDIO=mlt) also how far the shown frame is behind the sound clock (picture follows sound).
// Run: xvfb-run -a env SDL_AUDIODRIVER=dummy ./build-tests/tests/bench_playback <project.schneidi> [seconds] [start]
//      (start in seconds of the timeline; default 10 s from 0)
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SDL_AUDIODRIVER", "dummy");
    QApplication app(argc, argv);
    Check::initApp("bench-playback");
    if (argc < 2) {
        std::printf("usage: bench_playback <project.schneidi> [seconds] [start]\n");
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
    const double seconds = argc > 2 ? atof(argv[2]) : 10;
    const int start = int((argc > 3 ? atof(argv[3]) : 0) * project.format().rate.fps());

    Engine engine;
    if (!engine.init(&error)) {
        std::printf("Engine init failed: %s\n", qPrintable(error));
        return 77;
    }
    engine.setFormat(project.format());
    engine.updateTimeline(project.timeline());
    engine.showTimeline(start);
    {
        QEventLoop loop;
        QObject::connect(&engine, &Engine::frameReady, &loop, &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    // Timestamps of the shown frames (context &app: the lambda runs in the GUI thread)
    QVector<qint64> shown;
    QVector<int> positions;
    QVector<double> behind; // sound clock minus shown frame (frames) at every new position
    QElapsedTimer clock;
    QObject::connect(&engine, &Engine::frameReady, &app, [&] { shown << clock.nsecsElapsed(); });
    QObject::connect(&engine, &Engine::positionChanged, &app, [&](int f) {
        positions << f;
        if (const double sound = engine.soundFrame(); !std::isnan(sound)) behind << sound - f;
    });
    clock.start();
    engine.play();
    QEventLoop loop;
    QTimer::singleShot(int(seconds * 1000), &loop, &QEventLoop::quit);
    loop.exec();
    const double soundAtEnd = engine.soundFrame();
    const int shownAtEnd = engine.position();
    engine.pause();

    const double fps = project.format().rate.fps();
    std::printf("project %s, %.3f fps, %d frames shown in %.1f s = %.1f fps (positions %d..%d)\n", argv[1], fps,
                int(shown.size()), seconds, shown.size() / seconds, positions.isEmpty() ? -1 : positions.first(),
                positions.isEmpty() ? -1 : positions.last());
    if (!behind.isEmpty()) {
        double sum = 0, worst = 0;
        for (const double b : behind) {
            sum += b;
            worst = std::max(worst, b);
        }
        std::printf("sound clock at the end %.0f, shown frame %d (%.2f s behind); picture behind the sound: mean %.1f, "
                    "max %.1f frames\n", soundAtEnd, shownAtEnd, (soundAtEnd - shownAtEnd) / fps, sum / behind.size(),
                    worst);
    }
    // Per second: shown frames and the longest gap between two frames
    for (int s = 0; s < int(seconds); ++s) {
        int n = 0;
        double gap = 0;
        for (int i = 0; i < shown.size(); ++i) {
            if (shown[i] < s * 1e9 || shown[i] >= (s + 1) * 1e9) continue;
            ++n;
            if (i > 0) gap = std::max(gap, (shown[i] - shown[i - 1]) / 1e6);
        }
        std::printf("  %2d s: %3d frames, longest gap %6.1f ms%s\n", s, n, gap, gap > 2.5 * 1000 / fps ? "  <- stutter" : "");
    }
    return 0;
}
