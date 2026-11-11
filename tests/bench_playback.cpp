// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): playback of a real project in the preview Engine, like pressing play in the app.
// Reports shown frames per second and the longest gaps (stutter) per second of playback; with the own audio engine
// (the default; not with SCHNEIDI_AUDIO=mlt) also how far the shown frame is behind the sound clock (picture follows sound).
// Run: xvfb-run -a env SDL_AUDIODRIVER=dummy ./build-tests/tests/bench_playback <project.schneidi> [seconds] [start]
//      [--proxies] [--edge <px>]
//      (start in seconds of the timeline; default 10 s from 0; --proxies: first create the proxies of all video files
//      in the test cache, report time and size, then play from the proxies; --edge: the preview's longer edge like
//      the app's preview resolution, Engine::setPreviewEdge, 0 = the engine's default 960, 1920 = Full HD)
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SDL_AUDIODRIVER", "dummy");
    QApplication app(argc, argv);
    Check::initApp("bench-playback");
    bool useProxies = false;
    int edge = 0;
    for (int i = 1; i < argc;) {
        const std::string a = argv[i];
        int take = 0;
        if (a == "--proxies") {
            useProxies = true;
            take = 1;
        } else if (a == "--edge" && i + 1 < argc) {
            edge = atoi(argv[i + 1]);
            take = 2;
        }
        if (take == 0) {
            ++i;
            continue;
        }
        for (int j = i; j + take < argc; ++j) argv[j] = argv[j + take];
        argc -= take;
    }
    if (argc < 2) {
        std::printf("usage: bench_playback <project.schneidi> [seconds] [start] [--proxies] [--edge <px>]\n");
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
    if (useProxies) {
        QStringList files;
        for (const Track& t : project.timeline().video)
            for (const Clip& c : t.clips)
                if (!c.mediaPath.isEmpty() && !files.contains(c.mediaPath)) files << c.mediaPath;
        ProxyManager* pm = engine.proxies();
        pm->remove(files); // measure the creation, not an older proxy
        QElapsedTimer took;
        took.start();
        pm->generate(files);
        QEventLoop wait;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &wait, [&] {
            if (pm->pendingCount() == 0) wait.quit();
        });
        QObject::connect(pm, &ProxyManager::failed, &app, [](const QString& f, const QString& m) {
            std::printf("proxy failed for %s: %s\n", qPrintable(f), qPrintable(m));
        });
        poll.start(50);
        wait.exec();
        for (const QString& f : files)
            std::printf("proxy %s: %s, %.1f MB (original %.1f MB)\n", qPrintable(QFileInfo(f).fileName()),
                        pm->hasProxy(f) ? "made" : "MISSING", QFileInfo(ProxyManager::proxyPath(f)).size() / 1e6,
                        QFileInfo(f).size() / 1e6);
        std::printf("proxies created in %.1f s\n", took.elapsed() / 1000.0);
    }
    engine.setFormat(project.format());
    engine.setPreviewEdge(edge);
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
    std::printf("preview %dx%d\n", engine.previewSize().width(), engine.previewSize().height());
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
