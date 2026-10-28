// SPDX-License-Identifier: AGPL-3.0-only
// Playback stops at the end of the timeline, and stays stopped (user report 2026-10-24: everything deleted during
// playback, then a new import – playback went on by itself). The engine kept its speed when the producer paused at
// its end, so the next rebuild of the timeline started it again. Played: to the end; then deleted while playing
// (the playhead lies behind the new end); then something new on the timeline: nothing may play. Shortened while
// playing: plays on to the new end. Play at the end: from the front.
#include "check.h"

#include "engine/Engine.h"

#include <QApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
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

Timeline timeline(const QString& path, int length)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    if (length > 0) {
        tl.video[0].clips << mk(1, path, 0, length);
        tl.audio[0].clips << mk(2, path, 0, length);
    }
    return tl;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Waits until the engine stopped (at most ms); true if it did
bool stopsWithin(Engine& engine, int ms)
{
    for (int t = 0; t < ms && engine.speed() != 0.0; t += 20) wait(20);
    return engine.speed() == 0.0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("play_end");
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
    QVector<double> speeds;
    QObject::connect(&engine, &Engine::speedChanged, &engine, [&](double s) { speeds << s; });

    // 1. To the end: stops there, reports it, the playhead on the last frame
    engine.updateTimeline(timeline(media, 30));
    wait(800);
    engine.showTimeline(0);
    wait(300);
    speeds.clear();
    engine.play();
    CHECK(stopsWithin(engine, 4000));
    CHECK(!speeds.isEmpty() && speeds.last() == 0.0);
    std::printf("       Ende: Position %d (letztes Bild 29)\n", engine.position());
    CHECK(engine.position() >= 27 && engine.position() <= 29);
    // A rebuild afterwards does not start it again
    engine.updateTimeline(timeline(media, 200));
    wait(1000);
    CHECK_EQ(engine.speed(), 0.0);
    const int still = engine.position();
    wait(500);
    CHECK_EQ(engine.position(), still);

    // 2. Everything deleted while playing, then something new: stays stopped
    engine.seek(0);
    wait(300);
    engine.play();
    wait(1000);
    CHECK(engine.speed() != 0.0);
    engine.updateTimeline(timeline(media, 0));
    CHECK(stopsWithin(engine, 2000));
    engine.updateTimeline(timeline(media, 250)); // the import
    wait(1500);
    CHECK_EQ(engine.speed(), 0.0);
    const int after = engine.position();
    wait(500);
    std::printf("       nach Import: Position %d, dann %d\n", after, engine.position());
    CHECK_EQ(engine.position(), after);

    // 3. Shortened while playing, the playhead still inside: plays on to the new end
    engine.seek(0);
    wait(300);
    engine.play();
    wait(500);
    engine.updateTimeline(timeline(media, 60));
    wait(300);
    CHECK(engine.speed() != 0.0);
    CHECK(stopsWithin(engine, 4000));
    CHECK(engine.position() >= 57 && engine.position() <= 59);

    // 4. Play at the end starts from the front
    engine.play();
    wait(400);
    CHECK(engine.speed() != 0.0);
    std::printf("       Play am Ende: Position %d\n", engine.position());
    CHECK(engine.position() < 30);
    engine.pause();

    return Check::result();
}
