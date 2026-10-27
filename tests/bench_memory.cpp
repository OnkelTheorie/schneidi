// Benchmark (not a CTest test): how much memory the preview engine keeps after playback, scrubbing, edits, an export,
// thumbnails and stills, how much of it is MLT's frame pool (mlt_pool keeps every image buffer it ever handed out), and
// what giving it back frees (mlt_pool_purge, then malloc_trim on glibc). Also the time from play() to the first new
// picture, with a warm pool and right after a purge (giving memory back must not make playback start slower).
// Default: the engine's IdleRelease is held back, each step is measured by hand. --auto: the app's own behaviour
// (IdleRelease after its delay), every phase measured after a few idle seconds.
// Run: env SDL_AUDIODRIVER=dummy QT_QPA_PLATFORM=offscreen ./build/tests/bench_memory [--seconds N] [--edits N]
//      [--format WxH@fps] [--no-export] [--auto] <video>...
//      (the files go one after the other on V1/A1, the same again half a clip later on V2 at 60 % opacity, zoomed)
#include "check.h"

#include "engine/Engine.h"
#include "engine/Exporter.h"
#include "engine/MediaCache.h"
#include "engine/MemoryRelease.h"
#include "engine/StillFetcher.h"

#include <framework/mlt.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// VmRSS / VmHWM in MB (Linux), -1 elsewhere
double procMb(const char* key)
{
    QFile f("/proc/self/status");
    if (!f.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray& line : f.readAll().split('\n'))
        if (line.startsWith(key)) return line.mid(int(qstrlen(key))).trimmed().split(' ').value(0).toDouble() / 1024;
    return -1;
}

// mlt_pool_stat() only logs (verbose level): our log callback catches its summary line and passes everything else on
// to stderr like MLT's own (it stays installed: MLT has no getter for the previous one)
qint64 g_poolAllocated = -1, g_poolUsed = -1;
bool g_catchPool = false;
void poolLog(void*, int level, const char* fmt, va_list args)
{
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, args);
    if (g_catchPool) {
        long long allocated = 0, used = 0;
        if (const char* at = std::strstr(buf, "allocated ");
            at && std::sscanf(at, "allocated %lld bytes, used %lld bytes", &allocated, &used) == 2) {
            g_poolAllocated = allocated;
            g_poolUsed = used;
        }
        return;
    }
    if (level <= mlt_log_get_level()) std::fputs(buf, stderr);
}

void poolStat(double* allocatedMb, double* usedMb)
{
    *allocatedMb = *usedMb = 0;
    if (!mlt_factory_repository()) return; // the pools exist only after Mlt::Factory::init (mlt_pool_stat crashes)
    const int level = mlt_log_get_level();
    mlt_log_set_callback(poolLog);
    g_catchPool = true;
    mlt_log_set_level(MLT_LOG_VERBOSE);
    mlt_pool_stat();
    mlt_log_set_level(level);
    g_catchPool = false;
    *allocatedMb = g_poolAllocated / 1048576.0;
    *usedMb = g_poolUsed / 1048576.0;
}

// glibc's heap: in use by allocations and free but still held by the process (malloc_trim gives the latter back)
void heapStat(double* usedMb, double* freeMb)
{
    *usedMb = *freeMb = -1;
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 33)
    const struct mallinfo2 mi = mallinfo2();
    *usedMb = (mi.uordblks + mi.hblkhd) / 1048576.0;
    *freeMb = mi.fordblks / 1048576.0;
#endif
}

bool g_auto = false; // --auto: the app's IdleRelease runs, every phase is measured after this many idle ms
constexpr int kAutoIdleMs = 6000;

void snap(const char* label)
{
    if (g_auto) wait(kAutoIdleMs);
    double alloc = 0, used = 0, heapUsed = 0, heapFree = 0;
    poolStat(&alloc, &used);
    heapStat(&heapUsed, &heapFree);
    std::printf("%-34s RSS %6.0f MB (peak %5.0f)  pool %4.0f MB (%2.0f in use)  heap %5.0f MB in use, %5.0f free\n", label,
                procMb("VmRSS:"), procMb("VmHWM:"), alloc, used, heapUsed, heapFree);
    std::fflush(stdout);
}

Clip mk(int id, const QString& path, int start, int in, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    return c;
}

void release(bool purge, bool trim, bool quiet = false);

// ms from play() until `frames` new pictures arrived (-1 = none within 5 s); then pauses. purge/trim: give the pool /
// the free heap back right before play() (what the app does a few seconds after stopping)
double playStart(Engine& engine, int at, int frames = 1, bool purge = false, bool trim = false)
{
    engine.seek(at);
    wait(400);
    if (purge || trim) release(purge, trim, true);
    QElapsedTimer t;
    int got = 0;
    double ms = -1;
    QEventLoop loop;
    const auto c = QObject::connect(&engine, &Engine::frameReady, &loop, [&] {
        if (++got == frames) {
            ms = t.nsecsElapsed() / 1e6;
            loop.quit();
        }
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    t.start();
    engine.play();
    loop.exec();
    QObject::disconnect(c);
    engine.pause();
    wait(300);
    return ms;
}

void release(bool purge, bool trim, bool quiet)
{
    QElapsedTimer t;
    t.start();
    if (purge) mlt_pool_purge();
#ifdef __GLIBC__
    if (trim) malloc_trim(0);
#else
    Q_UNUSED(trim);
#endif
    if (!quiet) std::printf("  (release took %.1f ms)\n", t.nsecsElapsed() / 1e6);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-memory");
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // lines reach a pipe even if it crashes
    double seconds = 20;
    int edits = 40;
    bool exportPhase = true;
    ProjectFormat format{1920, 1080, {30, 1}};
    QStringList files;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--edits" && i + 1 < argc) edits = atoi(argv[++i]);
        else if (a == "--no-export") exportPhase = false;
        else if (a == "--auto") g_auto = true;
        else if (a == "--format" && i + 1 < argc) {
            const QStringList p = QString(argv[++i]).split(QRegularExpression("[x@]"));
            format = ProjectFormat{p.value(0).toInt(), p.value(1).toInt(), {p.value(2).toInt(), 1}};
        } else files << a;
    }
    if (files.isEmpty()) {
        std::printf("usage: bench_memory [--seconds N] [--edits N] [--format WxH@fps] <video>...\n");
        return 2;
    }

    snap("start");
    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("Engine init failed: %s\n", qPrintable(error));
        return 77;
    }
    engine.setFormat(format);
    // By hand: the engine must not give memory back on its own between the measurements
    if (!g_auto) engine.idleRelease()->setDelayMs(24 * 3600 * 1000);
    std::printf("mode: %s\n", g_auto ? "automatic (the app's IdleRelease, phases measured after 6 s idle)"
                                       : "by hand (IdleRelease held back)");
    snap("engine ready");

    const int fps = format.rate.timebase();
    const int clipLen = 8 * fps;
    Timeline tl;
    tl.video.resize(2);
    tl.audio.resize(1);
    int id = 1;
    for (int i = 0; i < files.size(); ++i) {
        tl.video[0].clips << mk(id++, files[i], i * clipLen, 0, clipLen);
        tl.audio[0].clips << mk(id++, files[i], i * clipLen, 0, clipLen);
        Clip over = mk(id++, files[i], i * clipLen + clipLen / 2, fps, clipLen / 2);
        over.transform.opacity = 60;
        over.transform.zoomX = over.transform.zoomY = 0.6;
        tl.video[1].clips << over;
    }
    const int end = int(files.size()) * clipLen;
    engine.updateTimeline(tl);
    engine.showTimeline(0);
    wait(1500);
    snap("timeline built");

    std::printf("first picture after play (warm decoders, cold pool): %.1f ms\n", playStart(engine, 0));
    {
        engine.seek(0);
        wait(300);
        int shown = 0;
        const auto c = QObject::connect(&engine, &Engine::frameReady, &app, [&] { ++shown; });
        engine.play();
        // Playback stops at the end: start again from the front until the time is over
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < seconds * 1000) {
            wait(200);
            if (engine.speed() == 0.0) {
                engine.seek(0);
                engine.play();
            }
        }
        engine.pause();
        QObject::disconnect(c);
        std::printf("played %.0f s: %d pictures (%.1f fps)\n", seconds, shown, shown / seconds);
    }
    wait(500);
    snap("after playback");

    // Scrubbing: the playhead jumps around (dragging it, J/K/L, an AI seeking to look at frames)
    for (int i = 0; i < 120; ++i) {
        engine.seek((i * 97) % std::max(1, end));
        wait(40);
    }
    wait(500);
    snap("after scrubbing");

    // Edits like an AI session: every change rebuilds the timeline, the preview shows the playhead's picture
    for (int i = 0; i < edits; ++i) {
        Timeline e = tl;
        Clip& c = e.video[1].clips[i % e.video[1].clips.size()];
        c.start += (i % 2) ? 1 : 0;
        c.transform.posX = 10 * i;
        engine.updateTimeline(e);
        engine.seek((i * 37) % std::max(1, end));
        wait(120);
    }
    wait(800);
    snap("after edits");

    if (exportPhase) {
        // Export of the whole timeline (own producers, own MLT graph) while the preview stands still
        QTemporaryDir out;
        ExportSettings es;
        es.path = out.filePath("bench.mp4");
        es.format = format;
        es.preset = "ultrafast";
        Exporter ex;
        QEventLoop loop;
        bool ok = false;
        QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
            ok = success;
            loop.quit();
        });
        QElapsedTimer t;
        t.start();
        QString err;
        if (ex.start(tl, es, &err)) {
            QTimer::singleShot(600000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        std::printf("export: %s in %.1f s%s\n", ok ? "ok" : "FAILED", t.elapsed() / 1000.0,
                    err.isEmpty() ? "" : qPrintable(" – " + err));
        wait(500);
        snap("after export");
    }

    {
        MediaCache cache;
        cache.setFormat(format);
        int wanted = 0;
        for (const QString& f : files)
            for (int frame = 0; frame < clipLen; frame += fps / 2) {
                cache.thumbnail(f, frame, 0);
                ++wanted;
            }
        QElapsedTimer t;
        t.start();
        int have = 0;
        while (t.elapsed() < 60000) {
            wait(250);
            have = 0;
            for (const QString& f : files)
                for (int frame = 0; frame < clipLen; frame += fps / 2) have += !cache.cachedThumbnail(f, frame).isNull();
            if (have == wanted) break;
        }
        std::printf("thumbnails: %d of %d\n", have, wanted);
        snap("after thumbnails (cache alive)");

        StillFetcher stills;
        stills.setFormat(format);
        QVector<StillFetcher::Request> reqs;
        for (const QString& f : files)
            for (int frame = 0; frame < clipLen; frame += fps) reqs.push_back({f, frame});
        int ready = 0;
        QObject::connect(&stills, &StillFetcher::ready, &app, [&] { ++ready; });
        stills.request(reqs);
        t.restart();
        while (ready < reqs.size() && t.elapsed() < 60000) wait(100);
        std::printf("stills: %d of %d\n", ready, int(reqs.size()));
        snap("after stills (cache alive)");
        wait(6000);
        snap("idle 6 s (caches alive)");
    }
    wait(300);
    snap("thumbnail/still caches deleted");

    release(false, true);
    snap("malloc_trim only");
    release(true, false);
    snap("mlt_pool_purge");
    release(false, true);
    snap("+ malloc_trim");

    // Playback start: warm pool against giving back right before (pool, heap, both), in turns
    double warm = 0, purged = 0, trimmed = 0, both = 0;
    const int runs = 6;
    for (int i = 0; i < runs; ++i) {
        const int at = (i * 53) % std::max(1, end - 2 * fps);
        warm += playStart(engine, at, 3); // pool filled by the runs before
        purged += playStart(engine, at, 3, true, false);
        warm += playStart(engine, at, 3);
        trimmed += playStart(engine, at, 3, false, true);
        warm += playStart(engine, at, 3);
        both += playStart(engine, at, 3, true, true);
    }
    std::printf("3 pictures after play (average of %d): warm %.1f ms, pool purged %.1f ms, heap trimmed %.1f ms, "
                "both %.1f ms\n", runs, warm / (3 * runs), purged / runs, trimmed / runs, both / runs);
    snap("end");

    // The app's own release: play once more, stop, IdleRelease gives it back after its delay
    {
        engine.seek(0);
        wait(300);
        engine.play();
        wait(5000);
        engine.pause();
        wait(300);
        snap("played 5 s again");
        engine.idleRelease()->setDelayMs(1000);
        engine.idleRelease()->request();
        const int before = MemoryRelease::releases();
        QElapsedTimer t;
        t.start();
        while (MemoryRelease::releases() == before && t.elapsed() < 10000) wait(50);
        wait(200);
        snap("IdleRelease after stopping");
    }
    return 0;
}
