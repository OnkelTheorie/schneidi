// Benchmark (not a CTest test): what the multicam grid (docs/plan-0.8.md) costs in the real preview Engine.
// Opens a project with a multicam clip, shows its grid, plays it (shown frames per second) and flips the pages
// back and forth while stopped and while playing (memory after each step: does it settle or keep growing?).
// Run: QT_QPA_PLATFORM=offscreen SDL_AUDIODRIVER=dummy ./build-tests/tests/bench_multicam <project.schneidi> [cycles]
#include "check.h"

#include "core/Multicam.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"
#include "engine/TimelineBuilder.h"
#include "ui/MulticamGrid.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#include <cstdio>

namespace {

long rssMb()
{
    QFile f("/proc/self/status");
    if (!f.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray& line : f.readAll().split('\n'))
        if (line.startsWith("VmRSS:")) return line.mid(6).trimmed().split(' ').value(0).toLong() / 1024;
    return -1;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-multicam");
    if (argc < 2) {
        std::fprintf(stderr, "usage: bench_multicam <project.schneidi> [cycles]\n");
        return 2;
    }
    const int cycles = argc > 2 ? QByteArray(argv[2]).toInt() : 4;
    ProjectData data;
    QString err;
    if (!ProjectFile::load(QString::fromLocal8Bit(argv[1]), &data, &err)) {
        std::fprintf(stderr, "load: %s\n", qPrintable(err));
        return 1;
    }
    Project project;
    project.load(data);
    const Sequence* seq = nullptr;
    for (const Sequence& s : project.sequences())
        if (!s.angles.isEmpty()) seq = &s;
    if (!seq) {
        std::fprintf(stderr, "no multicam clip in the project\n");
        return 1;
    }
    Engine engine;
    if (!engine.init(&err)) {
        std::fprintf(stderr, "engine: %s\n", qPrintable(err));
        return 1;
    }
    engine.setFormat(project.format());
    // BENCH_PROXIES=1: with proxy media (created first, in the test cache), otherwise the originals
    engine.proxies()->setEnabled(qEnvironmentVariableIntValue("BENCH_PROXIES"));
    if (engine.proxies()->enabled()) {
        QStringList files;
        for (const MediaInfo& m : project.media()) files << m.path;
        engine.proxies()->generate(files);
        QElapsedTimer t;
        t.start();
        while (engine.proxies()->pendingCount() > 0) wait(200);
        std::printf("proxies for %d files in %.1f s\n", int(files.size()), t.elapsed() / 1000.0);
    }
    const Timeline content = project.renderTimeline(seq->id);
    const int pages = MulticamGrid::pages(seq->angles.size());
    std::printf("%d angles, %d per page, %d pages; start: %ld MB\n", int(seq->angles.size()),
                MulticamGrid::perPage(seq->angles.size()), pages, rssMb());

    int shown = 0;
    QObject::connect(&engine, &Engine::frameReady, &app, [&] { ++shown; });
    auto show = [&](int page, int position) {
        MulticamGridOptions o;
        o.page = page;
        QElapsedTimer t;
        t.start();
        engine.showMulticam(content, seq->angles, o, 0, position);
        return t.elapsed();
    };
    const qint64 build = show(0, 0);
    wait(1500);
    std::printf("page 1 built in %lld ms: %ld MB\n", build, rssMb());

    auto play = [&](const char* what) {
        engine.seek(0);
        wait(500);
        shown = 0;
        QElapsedTimer t;
        t.start();
        engine.play();
        wait(4000);
        const int frames = engine.position();
        engine.pause();
        std::printf("%s: %d frames shown, position %d after %.1f s (%.1f fps), %ld MB\n", what, shown, frames,
                    t.elapsed() / 1000.0, frames / (t.elapsed() / 1000.0), rssMb());
    };
    play("play page 1");

    for (int c = 1; c <= cycles && pages > 1; ++c) {
        const qint64 a = show(1, engine.position());
        wait(1000);
        const qint64 b = show(0, engine.position());
        wait(1000);
        std::printf("stopped flip %d: page 2 %lld ms, page 1 %lld ms: %ld MB\n", c, a, b, rssMb());
    }
    for (int c = 1; c <= cycles && pages > 1; ++c) {
        engine.play();
        wait(1000);
        const int before = engine.position();
        const qint64 a = show(1, before);
        wait(1000);
        const int mid = engine.position();
        const qint64 b = show(0, mid);
        wait(1000);
        const int after = engine.position();
        engine.pause();
        std::printf("playing flip %d: page 2 %lld ms (from %d), page 1 %lld ms (from %d), now %d: %ld MB\n", c, a,
                    before, b, mid, after, rssMb());
    }
    wait(1000);
    std::printf("end: %ld MB\n", rssMb());
    return 0;
}
