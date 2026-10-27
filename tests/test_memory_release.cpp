// Giving memory back while idle (engine/MemoryRelease): MLT's frame pool keeps every buffer it handed out and glibc
// keeps freed heap memory, so the app grew by GBs over playback, exports and AI sessions. The policy: release only
// while nothing renders (no playback, no export, no render cache job) – never during playback. IdleRelease waits until
// nothing happened for its delay, tries again while something renders, releases once, and is reached from anywhere via
// MemoryRelease::requestRelease(). release() really hands the pool's buffers back (resident memory drops, Linux).
// The engine asks after stopping and after rebuilds, never while playing.
#include "check.h"

#include "engine/Engine.h"
#include "engine/MemoryRelease.h"

#include <framework/mlt.h>

#include <QApplication>
#include <QElapsedTimer>
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
    int signals_ = 0;
    QObject::connect(&idle, &IdleRelease::released, [&] { ++signals_; });

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
    constexpr int kBuffers = 40;
    constexpr int kSize = 8 * 1024 * 1024; // like a 1080p RGBA frame
    std::vector<void*> buffers;
    for (int i = 0; i < kBuffers; ++i) {
        void* b = mlt_pool_alloc(kSize);
        std::memset(b, i, kSize); // resident
        buffers.push_back(b);
    }
    for (void* b : buffers) mlt_pool_release(b); // back into MLT's pool: still resident
    const qint64 held = MemoryRelease::residentBytes();
    MemoryRelease::release();
    const qint64 after = MemoryRelease::residentBytes();
    std::printf("       resident: %lld MB, with %d pooled buffers %lld MB, after release %lld MB\n", base / 1048576,
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

    // Called directly while it plays (schneidi-cli releases after every MCP tool, whatever else runs): MLT's threads
    // keep taking and returning pool buffers meanwhile – no crash, the pictures keep coming
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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("memory_release");
    Check::initMlt();

    testPolicy();
    testIdleRelease();
    testRelease();

    if (Check::haveFfmpeg()) {
        QTemporaryDir tmp;
        const QString media = Check::makeMedia(
            tmp.filePath("media.mov"),
            {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=10", "-f", "lavfi", "-i",
             "sine=frequency=440:duration=10", "-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-c:a", "pcm_s16le"});
        if (CHECK(!media.isEmpty())) testEngine(media);
    } else {
        std::printf("       ffmpeg missing: engine part skipped\n");
    }
    return Check::result();
}
