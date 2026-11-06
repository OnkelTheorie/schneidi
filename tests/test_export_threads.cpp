// SPDX-License-Identifier: AGPL-3.0-only
// Export with several threads: frames in parallel (except with stabilize border "keep"; the own Green Screen keyer runs
// in parallel too), same result as with one thread.
// Prints the render times (1 thread / all threads) so that the gain can be measured afterwards.

#include "check.h"

#include "core/EffectRegistry.h"
#include "core/SystemMemory.h"
#include "engine/Exporter.h"
#include "engine/ClipFormat.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cstring>
#include <memory>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace {

QString g_media;
ProjectFormat kFmt{1920, 1080, {25, 1}};
constexpr int kFrames = 100;

Clip effectClip(bool chromakey)
{
    Clip c;
    c.id = 1;
    c.mediaPath = g_media;
    c.in = 0;
    c.out = kFrames - 1;
    EffectRegistry::add(c, "color");
    EffectRegistry::instance(c, "color")->params["saturation"] = 40.0;
    EffectRegistry::add(c, "blur");
    if (chromakey) EffectRegistry::add(c, "chromakey");
    return c;
}

Timeline one(const Clip& c)
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    return tl;
}

void testParallelFrames()
{
    const Timeline plain = one(effectClip(false));
    CHECK_EQ(Exporter::parallelFrames(plain, 0), Exporter::availableThreads());
    CHECK_EQ(Exporter::parallelFrames(plain, 3), 3);
    CHECK_EQ(Exporter::parallelFrames(one(effectClip(true)), 3), 3); // own keyer: thread-safe
    // Stabilization with border "keep" needs the frames in order (the other borders do not)
    Clip st = effectClip(false);
    EffectRegistry::add(st, "stabilize");
    CHECK_EQ(Exporter::parallelFrames(one(st), 4), 4);
    EffectRegistry::instance(st, "stabilize")->params["border"] = QString("keep");
    CHECK_EQ(Exporter::parallelFrames(one(st), 4), 1);
}

// Automatic threads against the RAM available: unchanged on a normal machine, fewer on a small board
void testMemoryThreads()
{
    constexpr qint64 MB = 1024 * 1024;
    const QSize hd(1920, 1080), vertical(1080, 1920), uhd(3840, 2160);
    // Unknown memory: every processor
    CHECK_EQ(Exporter::automaticThreads(12, uhd, 0).threads, 12);
    CHECK(!Exporter::automaticThreads(12, uhd, 0).lowMemory);
    // A machine with 4 GB (about 2.5 GB free) or more: 1080p and 4K keep all threads
    for (qint64 free : {2500 * MB, 12000 * MB}) {
        for (QSize size : {hd, vertical})
            for (int cores : {4, 8, 12, 16}) {
                CHECK_EQ(Exporter::automaticThreads(cores, size, free).threads, cores);
                CHECK(!Exporter::automaticThreads(cores, size, free).lowMemory);
            }
        CHECK_EQ(Exporter::automaticThreads(8, uhd, free).threads, 8);
    }
    CHECK_EQ(Exporter::automaticThreads(12, uhd, 12000 * MB).threads, 12);
    // Raspberry Pi Zero 2 W (415 MB, about 280 MB available) with a 1080x1920 short: 2 of 4
    const Exporter::ThreadChoice pi = Exporter::automaticThreads(4, vertical, 280 * MB);
    CHECK_EQ(pi.threads, 2);
    CHECK(pi.lowMemory);
    // 720p there has room for more; nearly nothing free still renders with one
    CHECK(Exporter::automaticThreads(4, QSize(1280, 720), 280 * MB).threads > 2);
    CHECK_EQ(Exporter::automaticThreads(4, vertical, 50 * MB).threads, 1);
    CHECK(Exporter::automaticThreads(4, vertical, 50 * MB).lowMemory);
    CHECK(!Exporter::automaticThreads(1, vertical, 50 * MB).lowMemory); // one processor: nothing was lowered

    // x264's lookahead against the RAM: 8K on a machine with 11 GB free (40 frames = 4 GB) short, 4K and 1080p
    // there not; a 4 GB machine (2.5 GB free) shortens it at 4K, not at 1080p
    CHECK(Exporter::shortLookahead(QSize(7680, 4320), 11000 * MB));
    CHECK(!Exporter::shortLookahead(uhd, 11000 * MB));
    CHECK(!Exporter::shortLookahead(hd, 11000 * MB));
    CHECK(Exporter::shortLookahead(uhd, 2500 * MB));
    CHECK(!Exporter::shortLookahead(hd, 2500 * MB));
    CHECK(!Exporter::shortLookahead(QSize(7680, 4320), 0)); // unknown: no limit

    // /proc/meminfo as Linux writes it
    const SystemMemory::Info info = SystemMemory::parseMeminfo("MemTotal:         425364 kB\nMemFree:           87212 kB\n"
                                                               "MemAvailable:     286720 kB\nBuffers:            1234 kB\n");
    CHECK_EQ(info.total, qint64(425364) * 1024);
    CHECK_EQ(info.available, qint64(286720) * 1024);
    CHECK_EQ(SystemMemory::parseMeminfo("garbage\n").available, qint64(0));
    // The real machine: something known on the systems schneidi runs on
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    CHECK(SystemMemory::read().total > 0);
    CHECK(SystemMemory::read().available > 0);
#endif
}

// A memory cap of the process's control group (container, systemd scope) counts, not only the machine's free RAM:
// an 8K render in a 5 GB scope on a machine with 11 GB free chose 9 threads and was killed by the cap
void testCgroupRoom(const QString& dir)
{
    constexpr qint64 MB = 1024 * 1024;
    auto put = [&](const QString& rel, const QByteArray& text) {
        QDir().mkpath(QFileInfo(dir + "/cg" + rel).path());
        QFile f(dir + "/cg" + rel);
        if (f.open(QIODevice::WriteOnly)) f.write(text);
    };
    const QString root = dir + "/cg";
    const QByteArray self = "0::/user.slice/app.slice/run-1.scope\n";
    // No limit files at all (no cgroup v2, or no memory controller): unknown
    CHECK_EQ(SystemMemory::cgroupRoom(self, root), qint64(0));
    // "max" everywhere: no limit
    put("/user.slice/memory.max", "max\n");
    put("/user.slice/app.slice/run-1.scope/memory.max", "max\n");
    CHECK_EQ(SystemMemory::cgroupRoom(self, root), qint64(0));
    // The scope: 5 GB cap, 1 GB used of which 200 MB is droppable file cache -> 4.2 GB room
    put("/user.slice/app.slice/run-1.scope/memory.max", QByteArray::number(5120 * MB) + "\n");
    put("/user.slice/app.slice/run-1.scope/memory.current", QByteArray::number(1024 * MB) + "\n");
    put("/user.slice/app.slice/run-1.scope/memory.stat", "anon 1\nfile 2\ninactive_file " + QByteArray::number(200 * MB) + "\n");
    CHECK_EQ(SystemMemory::cgroupRoom(self, root), 4296 * MB);
    // A parent with less room wins (memory.high counts like a limit: above it the kernel throttles and reclaims)
    put("/user.slice/memory.high", QByteArray::number(3000 * MB) + "\n");
    put("/user.slice/memory.current", QByteArray::number(2000 * MB) + "\n");
    CHECK_EQ(SystemMemory::cgroupRoom(self, root), 1000 * MB);
    // cgroup v1 or garbage: unknown, never a path outside the mount
    CHECK_EQ(SystemMemory::cgroupRoom("12:memory:/user.slice\n", root), qint64(0));
    CHECK_EQ(SystemMemory::cgroupRoom("0::/../../etc\n", root), qint64(0));
    CHECK_EQ(SystemMemory::cgroupRoom("", root), qint64(0));
    // The 8K case of the 5 GB scope with 4.2 GB room: a handful of threads instead of all
    put("/user.slice/memory.high", "max\n");
    const Exporter::ThreadChoice uhd8k =
        Exporter::automaticThreads(10, QSize(7680, 4320), SystemMemory::cgroupRoom(self, root));
    CHECK(uhd8k.lowMemory);
    CHECK(uhd8k.threads <= 4);
}

// A render that leaves the threads to schneidi says when the memory lowered them; a number asked for stays
void testMemoryRender(const QString& dir)
{
    Timeline tl = one(effectClip(false));
    tl.video[0].clips[0].out = 9;
    auto run = [&](int threads, int* used, bool* low) {
        Exporter ex;
        ExportSettings s;
        s.path = dir + "/mem.mp4";
        s.format = kFmt;
        s.preset = "medium"; // looks 40 frames ahead
        s.threads = threads;
        QEventLoop loop;
        QObject::connect(&ex, &Exporter::finished, &loop, [&](bool, const QString&) { loop.quit(); });
        QString error;
        if (!CHECK(ex.start(tl, s, &error))) return;
        *used = ex.threads();
        *low = ex.threadsLowMemory();
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
    };
    // x264 writes its settings into the stream (SEI): the lookahead it used
    auto lookahead = [&] {
        QFile f(dir + "/mem.mp4");
        if (!f.open(QIODevice::ReadOnly)) return QByteArray("no file");
        const QByteArray data = f.readAll();
        const qsizetype at = data.indexOf("rc_lookahead=");
        return at < 0 ? QByteArray("none") : data.mid(at + 13, data.indexOf(' ', at) - at - 13);
    };
    qputenv("SCHNEIDI_AVAILABLE_MB", "100");
    int used = 0;
    bool low = false;
    run(0, &used, &low);
    CHECK_EQ(used, 1);
    CHECK_EQ(low, Exporter::availableThreads() > 1);
    const bool x264 = lookahead() != "none"; // the GPL-free build writes OpenH264
    if (low && x264) CHECK_EQ(lookahead(), QByteArray("10"));
    run(3, &used, &low);
    CHECK_EQ(used, 3);
    CHECK(!low);
    // Threads asked for stay, but the lookahead alone (40 frames of 1080p, ~250 MB) does not fit into 100 MB: short
    // (8K: a render with --threads 2 was killed in a 5 GB cap by x264's lookahead)
    if (x264) CHECK_EQ(lookahead(), QByteArray("10"));
    qputenv("SCHNEIDI_AVAILABLE_MB", "4000");
    run(3, &used, &low);
    if (x264) CHECK_EQ(lookahead(), QByteArray("40")); // room enough: the preset's own
    qunsetenv("SCHNEIDI_AVAILABLE_MB");
    run(0, &used, &low); // this machine, as it is
    CHECK_EQ(used, Exporter::availableThreads());
    CHECK(!low);
}

// CPU time of the process (all threads) in s; 0 = unknown
double cpuSeconds()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto s = [](const FILETIME& f) { return (double(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) / 1e7; };
    return s(kernel) + s(user);
#else
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
#endif
}
double g_cpuLoad = 0; // last render(): CPU time / run time = average number of processors busy

// Renders and returns the duration in ms (-1 = failed)
qint64 render(const Timeline& tl, const QString& path, int threads)
{
    Exporter ex;
    ExportSettings s;
    s.path = path;
    s.format = kFmt;
    s.crf = qEnvironmentVariableIsSet("SCHNEIDI_BENCH_CLIP") ? 20 : 23; // measurement: like the Deliver page (quality High, medium)
    s.preset = qEnvironmentVariableIsSet("SCHNEIDI_BENCH_CLIP") ? "medium" : "veryfast";
    s.threads = threads;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    QElapsedTimer t;
    t.start();
    const double cpu0 = cpuSeconds();
    QString error;
    if (!CHECK(ex.start(tl, s, &error))) return -1;
    QTimer::singleShot(900000, &loop, &QEventLoop::quit);
    loop.exec();
    g_cpuLoad = (cpuSeconds() - cpu0) * 1000.0 / std::max<qint64>(1, t.elapsed());
    return CHECK(ok) ? t.elapsed() : -1;
}

QImage grab(const QString& path, int pos)
{
    return Check::mediaFrame(path, kFmt, pos);
}

// mean deviation per channel (0..255), sample
double diff(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull()) return 255;
    double sum = 0;
    int n = 0;
    for (int y = 0; y < a.height(); y += 7)
        for (int x = 0; x < a.width(); x += 7) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            sum += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
            n += 3;
        }
    return sum / n;
}

void testRender(const QString& dir)
{
    const Timeline tl = one(effectClip(false));
    const QString single = dir + "/one.mp4", all = dir + "/all.mp4";
    const qint64 t1 = render(tl, single, 1);
    const qint64 tn = render(tl, all, 0);
    std::printf("Export 1080p, %d Bilder, Farbkorrektur + Unschärfe: 1 Thread %lld ms, alle (%d) %lld ms\n", kFrames,
                t1, Exporter::availableThreads(), tn);
    // same images in the same order (parallel must not swap anything)
    for (int pos : {0, 37, kFrames - 1}) {
        const double d = diff(grab(single, pos), grab(all, pos));
        if (!CHECK(d < 2.0)) std::printf("       Bild %d: Abweichung %.2f\n", pos, d);
    }
    CHECK_EQ(Check::mediaLength(all, kFmt), kFrames);

    // Green Screen: frames in parallel, the same pictures as with one thread
    Timeline key = one(effectClip(true));
    key.video[0].clips[0].out = 24;
    const qint64 tk1 = render(key, dir + "/key1.mp4", 1);
    const qint64 tk = render(key, dir + "/key.mp4", 0);
    std::printf("Export mit Green Screen (25 Bilder): 1 Thread %lld ms, alle %lld ms\n", tk1, tk);
    for (int pos : {0, 12, 24}) CHECK(diff(grab(dir + "/key1.mp4", pos), grab(dir + "/key.mp4", pos)) < 2.0);
}

// Measurement with a real file (not part of the normal test run):
//   SCHNEIDI_BENCH_CLIP=<file> [SCHNEIDI_BENCH_CORES=1,0] [SCHNEIDI_BENCH_FPS=60] test_export_threads
// Clip entirely on V1 + A1 (cut without effects) and once more with color correction
int bench(const QString& clip)
{
    kFmt = ProjectFormat{1920, 1080, {qEnvironmentVariableIntValue("SCHNEIDI_BENCH_FPS") > 0
                                          ? qEnvironmentVariableIntValue("SCHNEIDI_BENCH_FPS") : 60, 1}};
    const int length = Check::mediaLength(clip, kFmt);
    if (!CHECK(length > 0)) return Check::result();
    Clip c;
    c.id = 1;
    c.mediaPath = clip;
    c.out = length - 1;
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    Timeline graded = tl;
    EffectRegistry::add(graded.video[0].clips[0], "color");
    EffectRegistry::instance(graded.video[0].clips[0], "color")->params["saturation"] = 40.0;
    QTemporaryDir tmp;
    const QStringList threadList = qEnvironmentVariable("SCHNEIDI_BENCH_THREADS", qEnvironmentVariable("SCHNEIDI_BENCH_CORES", "1,0")).split(',');
    for (const auto& [name, t] : {std::pair<const char*, const Timeline*>{"Schnitt", &tl}, {"Farbkorrektur", &graded}})
        for (const QString& n : threadList) {
            const qint64 ms = render(*t, tmp.filePath("bench.mp4"), n.toInt());
            std::printf("%s, %d Bilder, Threads %s: %lld ms (%.1f Bilder/s), im Mittel %.1f Prozessoren belegt\n", name, c.out + 1,
                        n == "0" ? qPrintable(QString("alle (%1)").arg(Exporter::availableThreads())) : qPrintable(n), ms,
                        ms > 0 ? (c.out + 1) * 1000.0 / ms : 0.0, g_cpuLoad);
            std::fflush(stdout);
        }
    return Check::result();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_threads");
    if (qEnvironmentVariableIsSet("SCHNEIDI_BENCH_CLIP")) {
        Check::initEngine();
        std::setlocale(LC_NUMERIC, "C");
        return bench(qEnvironmentVariable("SCHNEIDI_BENCH_CLIP"));
    }
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25:duration=4", "-c:v", "libx264",
                                "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();

    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testParallelFrames();
    testMemoryThreads();
    testCgroupRoom(tmp.path());
    testMemoryRender(tmp.path());
    testRender(tmp.path());
    return Check::result();
}
