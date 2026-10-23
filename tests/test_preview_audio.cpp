// Preview sound without clicks (user 2026-10-21): the real preview Engine plays a sine timeline while the test seeks,
// drags the playhead and changes the clip volume and the mixer – what the speaker gets (SDL's "disk" driver writes
// it to a file) must have no step in the wave and must come from one opened audio device.
// Clicks found this way (docs/testdaten/knacken): a full purge on seek threw away the fade-out (SeekFade), every seek
// of a drag played 5 ms of sound, clip volume 0 -> x dB and mixer changes from the AI rebuilt the timeline
// (audio device closed and reopened). Stopping breaks off on the MLT sound path (sdl2_audio drops its buffer) – checked
// as known there (SCHNEIDI_AUDIO=mlt); the own audio engine (the default, its file device as the speaker) fades out.
#include "check.h"

#include "engine/Engine.h"
#include "engine/audio/Path.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstdio>

namespace {

Clip mk(int id, const QString& path, int start, int in, int out)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = out;
    return c;
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
    QTemporaryDir tmp;
    const QString out = tmp.filePath("speaker.raw");
    qputenv("SDL_AUDIODRIVER", "disk"); // the "speaker": SDL writes what it would play, in real time
    qputenv("SDL_DISKAUDIOFILE", QDir::toNativeSeparators(out).toLocal8Bit());
    qputenv("SCHNEIDI_AUDIO_DEVICE", ("file:" + QDir::toNativeSeparators(out)).toLocal8Bit()); // own audio engine
    QApplication app(argc, argv);
    Check::initApp("preview_audio");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    if (!CHECK(tmp.isValid())) return Check::result();
    // Two tones (-6 dB) with a small picture, every cut and every seek is a jump in the wave unless faded. MOV: sample-exact timestamps
    // (MKV has 1 ms, MLT < 7.36 then drops 48 samples after a seek – a click of its own, see dev/mlt.md)
    QStringList tones;
    for (int f : {440, 620}) {
        tones << Check::makeMedia(tmp.filePath(QString("sine%1.mov").arg(f)),
                                  {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=30:duration=12", "-f", "lavfi", "-i",
                                   QString("aevalsrc=0.5*sin(2*PI*%1*t)|0.5*sin(2*PI*%1*t):s=48000:d=12").arg(f),
                                   "-c:v", "libx264", "-preset", "ultrafast", "-g", "30", "-c:a", "pcm_s16le"});
        if (!CHECK(!tones.last().isEmpty())) return Check::result();
    }

    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("Engine: %s\n", qPrintable(error));
        return Check::skip("kein MLT-Audio-Consumer");
    }
    ProjectFormat fmt;
    fmt.width = 320;
    fmt.height = 180;
    fmt.rate = nearestFrameRate(30);
    engine.setFormat(fmt);
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    // 0-4 s 440 Hz, 4-8 s 620 Hz, 8-12 s 440 Hz (odd in points)
    // with picture like a real project (rendering it ahead is what a full purge threw away together with the fade)
    tl.video[0].clips << mk(11, tones[0], 0, 7, 126) << mk(12, tones[1], 120, 33, 152) << mk(13, tones[0], 240, 201, 320);
    tl.audio[0].clips << mk(1, tones[0], 0, 7, 126) << mk(2, tones[1], 120, 33, 152) << mk(3, tones[0], 240, 201, 320);
    engine.updateTimeline(tl);
    engine.showTimeline(0);
    wait(800);

    QElapsedTimer playing;
    playing.start();
    engine.play();
    wait(1000);
    engine.seek(200); // jumps
    wait(700);
    engine.seek(40);
    wait(700);
    for (int i = 0; i < 25; ++i) { // dragging the playhead: a seek every 30 ms
        engine.seek(150 + i * 2);
        wait(30);
    }
    wait(700);
    tl.audio[0].clips[1].volumeDb = -6; // Inspector: clip volume (was 0 dB = no filter before)
    engine.updateTimeline(tl);
    wait(700);
    tl.audio[0].volumeDb = -3; // mixer fader (from the AI: no "mixer only" hint)
    engine.updateTimeline(tl);
    wait(700);
    const double played = playing.elapsed() / 1000.0;
    engine.pause();
    wait(300);

    QFile f(out);
    if (!CHECK(f.open(QIODevice::ReadOnly))) return Check::result();
    const QByteArray raw = f.readAll();
    const auto* s = reinterpret_cast<const int16_t*>(raw.constData());
    const qsizetype frames = raw.size() / 4; // s16 stereo
    const double seconds = frames / 48000.0;
    // One device session: the file holds (nearly) all of the playback (reopening the device starts it anew)
    if (!CHECK(seconds > played - 0.6)) std::printf("       %.2f s Ton, %.2f s gespielt\n", seconds, played);
    // No step: a 620-Hz sine at 0.5 changes its slope by < 110 per sample (s16); clicks were > 5000
    const qsizetype end = frames - 48000 * 3 / 10; // without the stop
    int clicks = 0, largest = 0;
    qsizetype largestAt = 0;
    for (qsizetype i = 2; i < end; ++i) {
        const int d2 = std::abs(s[i * 2] - 2 * s[(i - 1) * 2] + s[(i - 2) * 2]);
        if (d2 > largest) {
            largest = d2;
            largestAt = i;
        }
        if (d2 > 1500) {
            if (clicks < 5) std::printf("       Sprung in der Welle bei %.3f s: %d\n", i / 48000.0, d2);
            ++clicks;
            i += 480;
        }
    }
    std::printf("       %.2f s Ton, größte Krümmung %d bei %.3f s\n", seconds, largest, largestAt / 48000.0);
    CHECK_EQ(clicks, 0);

    // Stop: the sound fades out within a few ms instead of breaking off (the last sounding 2 ms are much quieter than
    // the sine before them). sdl2_audio closes the device at once and drops its buffer (dev/mlt.md) – known until the
    // own audio engine (docs/archiv/plan-0.10-ton.md), which fades out and keeps the device open.
    qsizetype last = frames - 1;
    while (last > 0 && std::abs(s[last * 2]) < 1000) --last;
    auto peak = [&](qsizetype from, qsizetype to) {
        int p = 0;
        for (qsizetype i = std::max<qsizetype>(0, from); i <= to; ++i) p = std::max(p, std::abs(int(s[i * 2])));
        return p;
    };
    const int tail = peak(last - 96, last), before = peak(last - 2400, last - 1440);
    const bool cut = before > 0 && tail > before / 2;
    const bool kMltAudio = !Audio::ownPath();
    std::printf("       Stopp: letzte 2 ms Spitze %d, 30–50 ms davor %d%s\n", tail, before,
                cut ? (kMltAudio ? " – bricht ab (bekannt: MLT-Ton-Weg)" : " – bricht ab") : " – blendet aus");
    if (kMltAudio) {
        if (!CHECK(cut)) std::printf("       Stopp knackt nicht mehr: Test auf die strenge Prüfung umstellen\n");
    } else {
        CHECK(!cut);
    }
    return Check::result();
}
