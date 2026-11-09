// SPDX-License-Identifier: AGPL-3.0-only
// Loop playback in the In/Out range (Engine::setLoopRange): playing forward wraps from Out back to In again and again
// - the playhead and the sound clock stay inside the range, no frame after Out is shown, the sound has no click at
// the seams (the engine's file device writes what the speaker would get). Off: plays on to the end and stops there.
// Switched off while looping (the range removed): plays on to the end. Playback that starts behind the range does not
// loop; a range reaching past the end wraps at the end.
#include "check.h"

#include "engine/Engine.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <climits>
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

bool stopsWithin(Engine& engine, int ms)
{
    for (int t = 0; t < ms && engine.speed() != 0.0; t += 20) wait(20);
    return engine.speed() == 0.0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QTemporaryDir tmp;
    const QString speaker = tmp.filePath("speaker.raw");
    qputenv("SCHNEIDI_AUDIO_DEVICE", ("file:" + QDir::toNativeSeparators(speaker)).toLocal8Bit());
    QApplication app(argc, argv);
    Check::initApp("play_loop");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    if (!CHECK(tmp.isValid())) return Check::result();
    // A sine: a jump in the wave at a seam is a click
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
    tl.video[0].clips << mk(1, media, 0, 150);
    tl.audio[0].clips << mk(2, media, 0, 150);
    engine.updateTimeline(tl);
    wait(800);
    engine.showTimeline(0);
    wait(300);
    QVector<int> shown;
    QObject::connect(&engine, &Engine::positionChanged, &engine, [&](int f) { shown << f; });
    QVector<bool> loopSignals;
    QObject::connect(&engine, &Engine::loopChanged, &engine, [&](bool on) { loopSignals << on; });

    // 1. Loop 50..74 (1 s) from 40: reaches the range, then wraps In <- Out several times
    engine.setLoopRange(50, 74);
    CHECK(engine.looping());
    CHECK(loopSignals == QVector<bool>{true});
    engine.seek(40);
    wait(300);
    shown.clear();
    engine.play();
    int wraps = 0, prev = 40, maxSeen = 0;
    double soundMin = 1e9, soundMax = -1e9;
    bool reached = false;
    QStringList wrapTimes; // ms after play: a loop of 1 s plus the moment each jump takes
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 4600) {
        wait(5);
        const double s = engine.soundFrame();
        if (reached && !std::isnan(s)) {
            soundMin = std::min(soundMin, s);
            soundMax = std::max(soundMax, s);
        }
        const int p = engine.position();
        if (p < prev) {
            ++wraps;
            wrapTimes << QString::number(t.elapsed());
        }
        if (p >= 50) reached = true;
        maxSeen = std::max(maxSeen, p);
        prev = p;
    }
    CHECK(engine.speed() != 0.0); // still playing
    int outside = 0;
    for (int i = 0; i < shown.size(); ++i)
        if (shown[i] > 74 || (shown[i] < 50 && std::find(shown.begin(), shown.begin() + i, 50) != shown.begin() + i))
            ++outside;
    std::printf("       Schleife: %d Sprünge zurück, höchstes Bild %d, %d Bilder außerhalb, Ton %.2f … %.2f\n", wraps,
                maxSeen, outside, soundMin, soundMax);
    std::printf("       Sprünge bei %s ms\n", qPrintable(wrapTimes.join(' ')));
    CHECK(wraps >= 3); // 4.6 s from 40: 0.4 s to In, then a wrap each second (and the moment each jump takes)
    CHECK(maxSeen <= 74);
    CHECK_EQ(outside, 0);
    // The sound clock follows: inside the range, at most a moment past Out's end until the jump lands
    CHECK(soundMin >= 49.5);
    CHECK(soundMax <= 76.0);

    // 2. Switched off while looping (the range removed): plays on past Out to the end and stops there
    engine.setLoopRange(-1, -1);
    CHECK(!engine.looping());
    CHECK(loopSignals == (QVector<bool>{true, false}));
    CHECK(stopsWithin(engine, 6000));
    std::printf("       aus: Ende bei %d\n", engine.position());
    CHECK(engine.position() >= 147 && engine.position() <= 149);
    engine.pause();

    // The sound of parts 1 and 2: no step in the wave at the seams (the jump fades out and in)
    wait(400);
    {
        QFile f(speaker);
        if (CHECK(f.open(QIODevice::ReadOnly))) {
            const QByteArray raw = f.readAll();
            const auto* s = reinterpret_cast<const int16_t*>(raw.constData());
            const qsizetype frames = raw.size() / 4 - 48000 / 2; // without the stop at the end
            int clicks = 0, largest = 0;
            for (qsizetype i = 2; i < frames; ++i) {
                const int d2 = std::abs(s[i * 2] - 2 * s[(i - 1) * 2] + s[(i - 2) * 2]);
                largest = std::max(largest, d2);
                if (d2 > 1500) {
                    if (clicks < 5) std::printf("       Sprung in der Welle bei %.3f s: %d\n", i / 48000.0, d2);
                    ++clicks;
                    i += 480;
                }
            }
            std::printf("       %.2f s Ton, größte Krümmung %d\n", raw.size() / 4 / 48000.0, largest);
            CHECK_EQ(clicks, 0);
        }
    }

    // 3. Off from the start: plays through Out to the end as before
    engine.seek(60);
    wait(300);
    engine.play();
    CHECK(stopsWithin(engine, 6000));
    CHECK(engine.position() >= 147);

    // 4. Playback that starts behind the range does not loop
    engine.setLoopRange(20, 40);
    engine.seek(100);
    wait(300);
    shown.clear();
    engine.play();
    const bool stopped4 = CHECK(stopsWithin(engine, 4000));
    CHECK(engine.position() >= 147);
    int before = 0;
    for (int f : shown) before += f < 100;
    if (!CHECK_EQ(before, 0) || !stopped4) {
        QStringList l;
        for (int i = 0; i < std::min<int>(shown.size(), 40); ++i) l << QString::number(shown[i]);
        std::printf("       hinter dem Bereich: %s\n", qPrintable(l.join(' ')));
    }

    // 5. A range reaching past the end wraps at the end; play at the end starts at In
    engine.setLoopRange(120, INT_MAX);
    engine.seek(130);
    wait(300);
    wraps = 0;
    prev = 130;
    engine.play();
    t.start();
    while (t.elapsed() < 2200) {
        wait(5);
        const int p = engine.position();
        if (p < prev) ++wraps;
        CHECK(p >= 120); // never in front of In
        prev = p;
    }
    std::printf("       bis zum Ende: %d Sprünge\n", wraps);
    CHECK(wraps >= 1);
    CHECK(engine.speed() != 0.0);
    engine.pause();
    engine.setLoopRange(-1, -1);
    return Check::result();
}
