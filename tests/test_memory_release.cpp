// SPDX-License-Identifier: AGPL-3.0-only
// Giving memory back while idle (engine/MemoryRelease): the picture engine keeps frame buffers for reuse and glibc
// keeps freed heap memory, so the app grew by GBs over playback, exports and AI sessions. The policy: release only
// while nothing renders (no playback, no export, no render cache job) – never during playback. IdleRelease waits until
// nothing happened for its delay, tries again while something renders, releases once, and is reached from anywhere via
// MemoryRelease::requestRelease(). release() really hands freed memory back (resident memory drops, Linux).
// The engine asks after stopping and after rebuilds, never while playing.
#include "check.h"

#include "engine/Engine.h"
#include "engine/MemoryRelease.h"
#include "engine/picture/Renderer.h"

#include <cstdlib>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace {

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Waits until release() ran `count` times in total (at most ms); true if it did
bool releasedWithin(int count, int ms)
{
    QElapsedTimer t;
    t.start();
    while (MemoryRelease::releases() < count && t.elapsed() < ms) wait(10);
    wait(20); // the worker's finished() reaches the IdleRelease
    return MemoryRelease::releases() >= count;
}

void testPolicy()
{
    using A = MemoryRelease::Activity;
    CHECK(MemoryRelease::allowed(A{}));
    CHECK(!MemoryRelease::allowed(A{true, false, false}));  // playing
    CHECK(!MemoryRelease::allowed(A{false, true, false}));  // exporting
    CHECK(!MemoryRelease::allowed(A{false, false, true}));  // render cache job
    CHECK(!MemoryRelease::allowed(A{true, true, true}));
}

void testIdleRelease()
{
    MemoryRelease::Activity now;
    IdleRelease idle([&] { return now; });
    idle.setDelayMs(60);
    int signals_ = 0, releasing = 0;
    QObject::connect(&idle, &IdleRelease::released, [&] { ++signals_; });
    QObject::connect(&idle, &IdleRelease::releasing, [&] { ++releasing; }); // the engine drops the pump's recent frames

    // Nothing asked: nothing happens
    const int start = MemoryRelease::releases();
    wait(200);
    CHECK_EQ(MemoryRelease::releases(), start);

    // Playing: asked, but never released while it plays (it keeps trying)
    now.playing = true;
    idle.request();
    wait(400);
    CHECK_EQ(MemoryRelease::releases(), start);
    CHECK(idle.pending());
    // Stopped: released once, then nothing more until asked again
    now.playing = false;
    CHECK(releasedWithin(start + 1, 2000));
    wait(300);
    CHECK_EQ(MemoryRelease::releases(), start + 1);
    CHECK(!idle.pending());
    CHECK_EQ(signals_, 1);
    CHECK_EQ(releasing, 1);

    // Export or render cache running: waits for them too
    for (bool MemoryRelease::Activity::*busy : {&MemoryRelease::Activity::exporting, &MemoryRelease::Activity::caching}) {
        const int before = MemoryRelease::releases();
        now.*busy = true;
        idle.request();
        wait(400);
        CHECK_EQ(MemoryRelease::releases(), before);
        now.*busy = false;
        CHECK(releasedWithin(before + 1, 2000));
    }

    // Many requests in a row (an AI session's rebuilds): one release after the last
    {
        const int before = MemoryRelease::releases();
        for (int i = 0; i < 10; ++i) {
            idle.request();
            wait(20);
        }
        CHECK_EQ(MemoryRelease::releases(), before);
        CHECK(releasedWithin(before + 1, 2000));
        wait(200);
        CHECK_EQ(MemoryRelease::releases(), before + 1);
    }

    // From anywhere (export end, other threads): reaches the newest IdleRelease
    {
        const int before = MemoryRelease::releases();
        std::thread([] { MemoryRelease::requestRelease(); }).join();
        CHECK(releasedWithin(before + 1, 2000));
    }
}

// The pool's buffers really go back to the system
void testRelease()
{
    const qint64 base = MemoryRelease::residentBytes();
#ifdef Q_OS_LINUX
    CHECK(base > 0); // /proc/self/status (reported size 0: read to the end, not line by line until atEnd())
#endif
    if (base < 0) {
        std::printf("       resident memory unknown here: release() only checked for not crashing\n");
        MemoryRelease::release();
        return;
    }
    // Small blocks (below glibc's mmap threshold): freed, they stay in the heap until it is trimmed
    constexpr int kBuffers = 8000;
    constexpr int kSize = 16 * 1024;
    std::vector<void*> buffers;
    for (int i = 0; i < kBuffers; ++i) {
        void* b = std::malloc(kSize);
        std::memset(b, i, kSize); // resident
        buffers.push_back(b);
    }
    for (void* b : buffers) std::free(b); // freed: still resident in the heap
    const qint64 held = MemoryRelease::residentBytes();
    MemoryRelease::release();
    const qint64 after = MemoryRelease::residentBytes();
    std::printf("       resident: %lld MB, with %d freed blocks %lld MB, after release %lld MB\n", base / 1048576,
                kBuffers, held / 1048576, after / 1048576);
    CHECK(held - base > qint64(kBuffers) * kSize / 2);
    CHECK(held - after > qint64(kBuffers) * kSize / 2);
}

// The engine: asks after stopping and rebuilds, never releases while playing
void testEngine(const QString& media)
{
    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("       Engine: %s (skipped)\n", qPrintable(error));
        return;
    }
    engine.idleRelease()->setDelayMs(150);
    engine.setFormat(ProjectFormat{320, 180, {25, 1}});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.out = 249;
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    engine.updateTimeline(tl);
    engine.showTimeline(0);
    CHECK(engine.idleRelease()->pending()); // the rebuild asked
    const int afterBuild = MemoryRelease::releases();
    CHECK(releasedWithin(afterBuild + 1, 3000));

    // Playing: rebuilds ask, but nothing is released until it stops
    engine.play();
    wait(200);
    if (!CHECK(engine.speed() != 0.0)) return;
    CHECK(engine.activity().playing);
    const int playing = MemoryRelease::releases();
    for (int i = 0; i < 4; ++i) {
        tl.video[0].clips[0].transform.opacity = 90 - i;
        engine.updateTimeline(tl);
        wait(200);
    }
    CHECK(engine.speed() != 0.0);
    CHECK_EQ(MemoryRelease::releases(), playing);

    // Called directly while it plays (schneidi-cli releases after every MCP tool, whatever else runs): the render
    // threads keep taking and returning pool buffers meanwhile – no crash, the pictures keep coming
    {
        int shown = 0;
        const auto counter = QObject::connect(&engine, &Engine::frameReady, [&] { ++shown; });
        std::thread other([] {
            for (int i = 0; i < 20; ++i) {
                MemoryRelease::release();
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        });
        for (int i = 0; i < 10; ++i) {
            MemoryRelease::release();
            wait(30);
        }
        other.join();
        const int during = shown;
        wait(300);
        QObject::disconnect(counter);
        std::printf("       %d pictures while releasing, %d in the 300 ms after\n", during, shown - during);
        CHECK(during > 0);
        CHECK(shown > during);
        CHECK(engine.speed() != 0.0);
    }
    const int beforeStop = MemoryRelease::releases();
    engine.pause();
    CHECK(!engine.activity().playing);
    CHECK(releasedWithin(beforeStop + 1, 3000));
    CHECK(!engine.activity().exporting);
}

Timeline clipsOf(const QStringList& files, int each)
{
    Timeline tl;
    tl.video.resize(1);
    for (int i = 0; i < files.size(); ++i) {
        Clip c;
        c.id = i + 1;
        c.mediaPath = files[i];
        c.start = i * each;
        c.out = each - 1;
        tl.video[0].clips << c;
    }
    return tl;
}

// Sources of the preview renderer over long pauses, memory pressure, a closed project and many clips
void testSources(const QString& media, const QString& dir)
{
    using namespace std::chrono;
    MemoryRelease::setPressureForTests(0);
    Picture::Options o;
    o.format = ProjectFormat{320, 180, {25, 1}};
    o.mode = Picture::Mode::Preview;
    const int each = 25;
    {
        Picture::Renderer r(clipsOf({media}, 250), o);
        r.render(100);
        CHECK_EQ(r.stats().sourcesOpen, 1);
        // Paused for a long time (an hour): Play goes on with the open source
        r.trim(steady_clock::now() + hours(1));
        CHECK_EQ(r.stats().sourcesOpen, 1);
        // Memory pressure: unused for a few seconds is enough to close it
        MemoryRelease::setPressureForTests(1);
        r.trim(steady_clock::now() + seconds(1));
        CHECK_EQ(r.stats().sourcesOpen, 1); // in use right now
        r.trim(steady_clock::now() + seconds(10));
        CHECK_EQ(r.stats().sourcesOpen, 0);
        MemoryRelease::setPressureForTests(0);
        CHECK(!r.render(101).isNull()); // opened again
        CHECK_EQ(r.stats().sourcesOpen, 1);
        // Another project (or the clip deleted): no longer used by the timeline, closed after a while
        r.update(Timeline{});
        r.trim(steady_clock::now() + seconds(5)); // an undo right after finds it open
        CHECK_EQ(r.stats().sourcesOpen, 1);
        r.trim(steady_clock::now() + minutes(1));
        CHECK_EQ(r.stats().sourcesOpen, 0);
    }
    // Many clips of different files played through: the most recently used few stay, the timeline's other files
    // close by last use - the count does not grow with the clips played
    QStringList files;
    for (int i = 0; i < 20; ++i) {
        const QString copy = dir + QString("/source%1.mov").arg(i);
        if (QFile::exists(copy) || QFile::copy(media, copy)) files << copy;
    }
    if (!CHECK_EQ(files.size(), 20)) return;
    Picture::Renderer r(clipsOf(files, each), o);
    int most = 0;
    for (int round = 0; round < 2; ++round)
        for (int i = 0; i < files.size(); ++i) {
            r.render(i * each + 5);
            r.trim(steady_clock::now() + seconds(15)); // as if each clip had played for 15 s
            most = std::max(most, r.stats().sourcesOpen);
        }
    std::printf("       20 files played through twice: at most %d sources open, %d at the end\n", most,
                r.stats().sourcesOpen);
    CHECK(most <= Picture::Renderer::kKeepSources + 1);
    CHECK(r.stats().sourcesOpen >= 1);
    // All of them in use at once (all asked for within the last seconds): none closed
    for (int i = 0; i < files.size(); ++i) r.render(i * each + 6);
    r.trim(steady_clock::now());
    CHECK_EQ(r.stats().sourcesOpen, 20);
    // Paused on clip 3, then the other clips were touched later (prepared by another thread): beyond the count, the
    // least recently used go - never the one the paused frame shows
    {
        Picture::Renderer p(clipsOf(files, each), o);
        p.render(3 * each + 5);
        std::thread other([&] {
            for (int i = 4; i < files.size(); ++i) p.prepare(i * each);
        });
        other.join();
        CHECK(p.stats().sourcesOpen > Picture::Renderer::kKeepSources);
        p.trim(steady_clock::now() + minutes(1));
        CHECK_EQ(p.stats().sourcesOpen, Picture::Renderer::kKeepSources);
        p.update(clipsOf({files[3]}, each)); // the paused one alone: still open, nothing else
        p.trim(steady_clock::now() + minutes(1));
        CHECK_EQ(p.stats().sourcesOpen, 1);
    }
    // Still pictures (up to 4x the project's pixels as RGBA) close after 30 s unused, as before
    {
        const QString png = dir + "/still.png";
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(Qt::red);
        CHECK(img.save(png));
        Picture::Renderer p(clipsOf({png}, 100), o);
        p.render(10);
        CHECK_EQ(p.stats().sourcesOpen, 1);
        p.trim(steady_clock::now() + seconds(10));
        CHECK_EQ(p.stats().sourcesOpen, 1);
        p.trim(steady_clock::now() + minutes(1));
        CHECK_EQ(p.stats().sourcesOpen, 0);
    }
    MemoryRelease::setPressureForTests(-1);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("memory_release");
    Check::initEngine();

    testPolicy();
    testIdleRelease();
    testRelease();

    if (Check::haveFfmpeg()) {
        QTemporaryDir tmp;
        const QString media = Check::makeMedia(
            tmp.filePath("media.mov"),
            {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=10", "-f", "lavfi", "-i",
             "sine=frequency=440:duration=10", "-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-c:a", "pcm_s16le"});
        if (CHECK(!media.isEmpty())) {
            testEngine(media);
            testSources(media, tmp.path());
        }
    } else {
        std::printf("       ffmpeg missing: engine part skipped\n");
    }
    return Check::result();
}
