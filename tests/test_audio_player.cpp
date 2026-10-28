// SPDX-License-Identifier: AGPL-3.0-only
// Own audio engine, step 5 (docs/archiv/plan-0.10-ton.md): Audio::Player plays live. The "speaker" is the file device
// (SCHNEIDI_AUDIO_DEVICE=file:…, 16-bit stereo in real time). A sine timeline is played, jumped in, dragged, changed
// live (clip volume = new mix, fader = live value), played at 2x, backwards and silent at 8x, then stopped – what
// the speaker got must have no step in the wave anywhere, the stop fades out, and nothing runs dry. The clock
// (position()) must say which sample the speaker is playing right now.

#include "check.h"

#include "core/Loudness.h"
#include "engine/audio/Player.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kRate = 48000;
const FrameRate kFps{30, 1};

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

int64_t frameStart(int f) { return int64_t(f) * kRate / 30; }

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Watches the speaker file: when each block was handed over (file size, ns)
struct Speaker {
    QString path;
    QElapsedTimer clock;
    std::atomic<bool> stop{false};
    std::vector<std::pair<qint64, qint64>> writes;
    std::thread thread;
    void start()
    {
        clock.start();
        thread = std::thread([this] {
            qint64 last = -1;
            while (!stop) {
                const qint64 size = QFileInfo(path).size();
                if (size != last) {
                    writes.emplace_back(size, clock.nsecsElapsed());
                    last = size;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }
    void finish()
    {
        stop = true;
        thread.join();
    }
    // Sample of the file heard at `ns` (a block counts as heard from its handing over, sample by sample)
    double sampleAt(qint64 ns) const
    {
        for (size_t i = writes.size(); i-- > 1;)
            if (writes[i].second <= ns) {
                const double start = writes[i - 1].first / 4.0, end = writes[i].first / 4.0;
                return std::min(end, start + (ns - writes[i].second) * 1e-9 * kRate);
            }
        return 0;
    }
};

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QTemporaryDir tmp;
    Speaker speaker;
    speaker.path = tmp.filePath("speaker.raw");
    qputenv("SCHNEIDI_AUDIO_DEVICE", ("file:" + QDir::toNativeSeparators(speaker.path)).toLocal8Bit());
    QCoreApplication app(argc, argv);
    Check::initApp("audio_player");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    if (!CHECK(tmp.isValid())) return Check::result();

    QStringList tones;
    for (int f : {440, 620}) {
        tones << Check::makeMedia(tmp.filePath(QString("sine%1.wav").arg(f)),
                                  {"-f", "lavfi", "-i",
                                   QString("aevalsrc=0.5*sin(2*PI*%1*t)|0.5*sin(2*PI*%1*t):s=48000:d=12").arg(f),
                                   "-c:a", "pcm_s16le"});
        if (!CHECK(!tones.last().isEmpty())) return Check::result();
    }
    // 0-4 s 440 Hz, 4-8 s 620 Hz, 8-12 s 440 Hz (odd in points)
    Timeline tl;
    tl.audio.resize(1);
    tl.audio[0].clips << mk(1, tones[0], 0, 7, 126) << mk(2, tones[1], 120, 33, 152) << mk(3, tones[0], 240, 201, 320);

    SharedLoudness loudness;
    loudness.active = true;
    Audio::Player::Options opt;
    opt.buffer = 960; // two periods of exactly 10 ms
    auto player = std::make_unique<Audio::Player>(opt);
    player->setLoudness(&loudness);
    player->setTimeline(tl, kFps);
    CHECK(!player->playing());

    // 1. Play from the start; the clock against the speaker
    speaker.start();
    player->play(0, 1.0);
    sleepMs(40);
    const double held = player->position();
    CHECK_EQ(held, 0.0); // stands until the picture is ready
    player->release();
    std::vector<std::pair<qint64, double>> readings; // (ns, clock)
    for (int i = 0; i < 12; ++i) {
        sleepMs(40);
        readings.emplace_back(speaker.clock.nsecsElapsed(), player->position());
    }
    const std::vector<float> levels = player->takeLevels();
    // 2. Jumps
    player->play(double(frameStart(200)), 1.0);
    player->release();
    sleepMs(500);
    player->play(double(frameStart(40)), 1.0);
    player->release();
    sleepMs(400);
    // 3. Dragging the playhead while playing: silent until released
    player->setMuted(true);
    for (int i = 0; i < 12; ++i) {
        player->play(double(frameStart(150 + i * 2)), 1.0);
        player->release();
        sleepMs(30);
    }
    player->setMuted(false);
    sleepMs(500);
    // 4. Inspector: clip volume (a new mix), 5. mixer fader (live value)
    tl.audio[0].clips[1].volumeDb = -6;
    player->setTimeline(tl, kFps);
    sleepMs(300);
    tl.audio[0].volumeDb = -3;
    player->setTimeline(tl, kFps);
    sleepMs(300);
    // 6. Shuttle: 2x, backwards, 2x backwards, 8x (silent)
    player->play(player->position(), 2.0);
    player->release();
    sleepMs(400);
    player->play(player->position(), -1.0);
    player->release();
    sleepMs(700);
    player->play(player->position(), -2.0);
    player->release();
    sleepMs(300);
    const double before8 = player->position();
    player->play(before8, 8.0);
    player->release();
    sleepMs(300);
    const double after8 = player->position();
    // 7. Normal again, then stop
    player->play(player->position(), 1.0);
    player->release();
    sleepMs(400);
    player->pause();
    const double stopped = player->position();
    sleepMs(300);
    CHECK_EQ(player->position(), stopped); // the clock stands while stopped
    const Audio::Player::Stats stats = player->stats();
    player.reset();
    speaker.finish();

    std::printf("Gerät: Datei, %d Mischer gebaut, %d Live-Änderungen, %d Ausblendungen, %d Aussetzer\n", stats.mixers,
                stats.liveUpdates, stats.fadeOuts, stats.underruns);
    CHECK_EQ(stats.underruns, 0);
    CHECK_EQ(stats.mixers, 2);      // the first and the clip volume
    CHECK_EQ(stats.liveUpdates, 1); // the fader
    CHECK(stats.fadeOuts >= 8);
    // 8x ran silent with the clock: ~0.3 s at 8x = ~2.4 s
    std::printf("8x: Uhr %.2f s weiter\n", (after8 - before8) / kRate);
    CHECK(after8 - before8 > 1.5 * kRate && after8 - before8 < 3.5 * kRate);

    // Levels of the first stretch: track and master ~0.5 (sine at -6 dB)
    if (CHECK_EQ(int(levels.size()), 4)) {
        std::printf("Pegel: Spur %.3f/%.3f, Master %.3f/%.3f\n", levels[0], levels[1], levels[2], levels[3]);
        CHECK(std::abs(levels[0] - 0.5f) < 0.01f && std::abs(levels[1] - 0.5f) < 0.01f);
        CHECK(levels[2] > 0.49f && levels[2] < 0.53f);
    }
    {
        std::lock_guard<std::mutex> g(loudness.mutex);
        std::printf("Lautheit (nur Tempo 1): %.1f LUFS integriert über %.1f s\n", loudness.meter.integrated(),
                    loudness.meter.measuredSeconds());
        CHECK(loudness.meter.measuredSeconds() > 1.0);
    }

    QFile f(speaker.path);
    if (!CHECK(f.open(QIODevice::ReadOnly))) return Check::result();
    const QByteArray raw = f.readAll();
    const auto* s = reinterpret_cast<const int16_t*>(raw.constData());
    const qsizetype frames = raw.size() / 4;
    std::printf("Lautsprecher: %.2f s\n", frames / double(kRate));

    // The clock: the speaker plays sample n of the file = timeline sample n - first (playback 1 starts at 0 and
    // runs without a jump); readings within 1 ms (+ polling of the file). Now and then a thread of the file device or
    // of the polling wakes late (seen once in 10 runs: 5 ms) – the median must be exact, the worst within 8 ms
    qsizetype first = 0;
    while (first < frames && s[first * 2] == 0) ++first;
    --first; // the sine starts with 0
    double worst = 0;
    std::vector<double> devs;
    for (const auto& [ns, clock] : readings) {
        const double heard = speaker.sampleAt(ns) - double(first);
        devs.push_back(std::abs(clock - heard));
        worst = std::max(worst, devs.back());
        if (qEnvironmentVariableIsSet("SCHNEIDI_TEST_VERBOSE"))
            std::printf("  %.1f ms: Uhr %.0f, gehört %.0f\n", ns / 1e6, clock, heard);
    }
    std::sort(devs.begin(), devs.end());
    const double median = devs[devs.size() / 2];
    std::printf("Uhr: Ton beginnt nach %.1f ms Stille, Abweichung Median %.2f ms, größte %.2f ms\n",
                first * 1000.0 / kRate, median * 1000 / kRate, worst * 1000 / kRate);
    CHECK(median < kRate / 1000.0);
    CHECK(worst < kRate / 1000.0 * 8);

    // No step: a 620-Hz sine at 0.5 at 2x changes its slope by < 450 per sample (s16); clicks are > 5000
    int clicks = 0, largest = 0;
    qsizetype largestAt = 0;
    for (qsizetype i = 2; i < frames; ++i)
        for (int c = 0; c < 2; ++c) {
            const int d2 = std::abs(s[i * 2 + c] - 2 * s[(i - 1) * 2 + c] + s[(i - 2) * 2 + c]);
            if (d2 > largest) {
                largest = d2;
                largestAt = i;
            }
            if (d2 > 1500) {
                if (clicks < 5) std::printf("       Sprung in der Welle bei %.3f s: %d\n", i / double(kRate), d2);
                ++clicks;
                i += 480;
            }
        }
    std::printf("Größte Krümmung %d bei %.3f s\n", largest, largestAt / double(kRate));
    CHECK_EQ(clicks, 0);

    // Stop: fades out within 5 ms (the last sounding 2 ms are much quieter than the sine before them)
    qsizetype last = frames - 1;
    while (last > 0 && std::abs(s[last * 2]) < 300) --last;
    auto peak = [&](qsizetype from, qsizetype to) {
        int p = 0;
        for (qsizetype i = std::max<qsizetype>(0, from); i <= to; ++i) p = std::max(p, std::abs(int(s[i * 2])));
        return p;
    };
    const int tail = peak(last - 96, last), before = peak(last - 2400, last - 1440);
    std::printf("Stopp: letzte 2 ms Spitze %d, 30–50 ms davor %d\n", tail, before);
    CHECK(before > 8000 && tail < before / 2);

    // 8. The device vanishes while playing (it stops calling back without a word, like a chosen PulseAudio output
    // whose card goes away): the watchdog reopens and the clock runs on; new settings reopen at once.
    player.reset();
    qputenv("SCHNEIDI_AUDIO_DEVICE", "null:stall=500");
    auto lost = std::make_unique<Audio::Player>(opt);
    lost->setTimeline(tl, kFps);
    lost->play(0, 1.0);
    lost->release();
    sleepMs(1200);
    const double afterLoss = lost->position();
    sleepMs(500);
    const double later = lost->position();
    std::printf("Gerät weg: Uhr nach 1,2 s bei %.3f s, 0,5 s später +%.3f s, %d× neu geöffnet\n", afterLoss / kRate,
                (later - afterLoss) / kRate, lost->stats().reopens);
    CHECK_EQ(lost->stats().reopens, 1);
    CHECK(afterLoss > 0.7 * kRate && afterLoss < 1.2 * kRate); // stood ~0.25 s
    CHECK(std::abs(later - afterLoss - 0.5 * kRate) < 0.05 * kRate);
    lost->setDevice(1920, QString(), QString());
    sleepMs(500);
    const double changed = lost->position();
    CHECK(std::abs(changed - later - 0.5 * kRate) < 0.1 * kRate);
    CHECK_EQ(lost->stats().reopens, 1); // a settings change is no vanished device
    CHECK(lost->playing());

    // 9. A slow driver (headphones plugged in: the watchdog reopens for a second) never blocks play() – it runs on the
    // UI thread at every start, jump and scrub step
    lost.reset();
    qputenv("SCHNEIDI_AUDIO_DEVICE", "null:slow=1500");
    auto slow = std::make_unique<Audio::Player>(opt);
    slow->setTimeline(tl, kFps);
    slow->play(0, 1.0); // the first open: once
    slow->release();
    slow->setDevice(1920, QString(), QString()); // the watchdog reopens (1.5 s)
    sleepMs(300);
    QElapsedTimer jump;
    jump.start();
    slow->play(48000, 1.0);
    const double jumpMs = jump.nsecsElapsed() / 1e6;
    std::printf("Sprung während das Gerät 1,5 s lang öffnet: %.1f ms\n", jumpMs);
    CHECK(jumpMs < 200);
    CHECK(slow->playing());
    return Check::result();
}
