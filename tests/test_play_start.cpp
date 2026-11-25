// SPDX-License-Identifier: AGPL-3.0-only
// Play starts at once (the user's rule: instant play matters). The sound's clock stands after play() until the picture
// is there (Player::release) or 0.3 s have passed. Paused, the still at the playhead is already there: the pump shows
// it again at once when told to play - before Engine::setSpeed reached Player::play on the first play of a session
// (play() still opens the device), and that release() was lost: picture and sound stood 0.3 s. Measured here: the time
// from play() (and from a jump while playing back to a frame shown moments ago) until the sound clock moves.
#include "check.h"

#include "engine/Engine.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr int kFps = 25;

Clip mk(int id, const QString& path, int start, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = 0;
    c.out = length - 1;
    return c;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Milliseconds until the sound clock passed `from` + half a frame (-1: not within 1.5 s)
double clockMoves(Engine& engine, double from, const QElapsedTimer& since)
{
    while (since.elapsed() < 1500) {
        if (const double s = engine.soundFrame(); !std::isnan(s) && s > from + 0.5) return since.nsecsElapsed() / 1e6;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("play_start");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QString media = Check::makeMedia(
        tmp.filePath("media.mov"),
        {"-f", "lavfi", "-i", QString("testsrc2=size=320x180:rate=%1:duration=10").arg(kFps), "-f", "lavfi", "-i",
         "aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t):s=48000:d=10", "-c:v", "libx264", "-preset", "ultrafast",
         "-g", "25", "-c:a", "pcm_s16le"});
    if (!CHECK(!media.isEmpty())) return Check::result();

    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("Engine: %s\n", qPrintable(error));
        return Check::skip("Engine startet nicht");
    }
    engine.setOpenInBackground(true); // like the app
    engine.setFormat(ProjectFormat{320, 180, {kFps, 1}});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << mk(1, media, 0, 250);
    tl.audio[0].clips << mk(2, media, 0, 250);
    engine.updateTimeline(tl);
    wait(800);
    engine.showTimeline(0);
    engine.seek(40);
    wait(500); // the still at 40 is shown (and in the pump's recent frames)

    // The hold lasts 300 ms; a start the picture released takes a frame or two (the null device's timer, the pump)
    constexpr double kMaxStartMs = 200;
    QElapsedTimer t;
    t.start();
    engine.play(); // first play of the session: opens the sound device
    const double first = clockMoves(engine, 40, t);
    std::printf("first play: clock moves after %.1f ms\n", first);
    CHECK(first >= 0 && first < kMaxStartMs);

    double worst = 0;
    for (int i = 0; i < 4; ++i) {
        wait(300);
        engine.togglePlay(); // pause
        wait(400);
        const double at = engine.position();
        t.restart();
        engine.togglePlay();
        const double ms = clockMoves(engine, at, t);
        std::printf("play %d: clock moves after %.1f ms\n", i + 2, ms);
        worst = ms < 0 ? 1e9 : std::max(worst, ms);
    }
    CHECK(worst < kMaxStartMs);

    // A jump while playing back to a frame shown moments ago (J-K-L back, a loop's In point)
    worst = 0;
    for (int i = 0; i < 4; ++i) {
        const int back = std::max(0, engine.position() - 3);
        wait(200);
        t.restart();
        engine.seek(back);
        const double ms = clockMoves(engine, back, t);
        std::printf("jump %d: clock moves after %.1f ms\n", i + 1, ms);
        worst = ms < 0 ? 1e9 : std::max(worst, ms);
    }
    CHECK(worst < kMaxStartMs);
    engine.pause();

    // Fractional frame rates: frame -> sound position -> frame is not exact in double (at 30000/1001 the clock of a held
    // start reads 3.6e-15 below 4.5 % of the frames), and a start frame "not yet due" by that much was never shown, so
    // never released the clock: 0.3 s of standing picture and sound. A jump while playing to such frames, not shown
    // before (they must be rendered), has to start as fast as any other.
    {
        const FrameRate ntsc{30000, 1001};
        engine.setFormat(ProjectFormat{320, 180, ntsc});
        engine.updateTimeline(tl);
        wait(500);
        engine.seek(5);
        wait(300);
        engine.play();
        wait(300);
        worst = 0;
        int tried = 0;
        for (int f = 30; f < 240 && tried < 6; ++f) {
            if (double(f) * 48000.0 / ntsc.fps() * ntsc.fps() / 48000.0 >= double(f)) continue; // the clock reads f exactly
            ++tried;
            t.restart();
            engine.seek(f);
            const double ms = clockMoves(engine, f, t);
            std::printf("29.97 jump to %d: clock moves after %.1f ms\n", f, ms);
            worst = ms < 0 ? 1e9 : std::max(worst, ms);
            wait(150);
            f += 8; // not a neighbour of what was just shown
        }
        CHECK(tried >= 3);
        CHECK(worst < kMaxStartMs);
        engine.pause();
    }
    return Check::result();
}
