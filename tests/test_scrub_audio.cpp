// SPDX-License-Identifier: AGPL-3.0-only
// Sound while scrubbing (Audio::Player::scrub, Engine::scrubTo, TimelineOptions::scrubAudio). The "speaker" is the file
// device (SCHNEIDI_AUDIO_DEVICE=file:…, 16-bit stereo in real time). The test sound carries its own time: the left
// channel rises evenly with the timeline time (t / 24), the right one is a 440-Hz sine (any step in it is a click).
//  1. Single scrubs play one slice each, 40-90 ms long, from the asked timeline time, then silence (no drone)
//  2. A slow drag at about real time continues the sound without a seam (joins)
//  3. A fast burst of requests: the stale ones are dropped, the newest position plays, no lag queues up
//  4. A muted track stays silent, a soloed other track is heard
//  5. Nowhere a step in the wave (fades at every slice edge)
//  6. Engine: scrubTo plays a slice, a plain seek does not, the setting off is silent; the setting is stored
#include "check.h"

#include "engine/Engine.h"
#include "engine/audio/Player.h"
#include "ui/timeline/TimelineOptions.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kRate = 48000;
const FrameRate kFps{30, 1};
constexpr int kSilence = 40; // s16: below this is silence

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

Clip mk(int id, const QString& path, int start, int frames)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = 0;
    c.out = frames - 1;
    return c;
}

struct Speaker {
    QString path;
    std::vector<int16_t> s; // interleaved L, R
    qsizetype frames() const { return qsizetype(s.size() / 2); }
    void load()
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return;
        const QByteArray raw = f.readAll();
        s.assign(reinterpret_cast<const int16_t*>(raw.constData()),
                 reinterpret_cast<const int16_t*>(raw.constData()) + raw.size() / 2);
    }
    qsizetype size() const { return QFileInfo(path).size() / 4; } // frames written so far
    bool loud(qsizetype i) const { return std::abs(s[size_t(i) * 2]) > kSilence || std::abs(s[size_t(i) * 2 + 1]) > kSilence; }
};

struct Seg {
    qsizetype from = 0, to = 0; // [from, to)
    qsizetype length() const { return to - from; }
};

// Stretches of sound in [a, b); gaps shorter than `merge` samples join them
std::vector<Seg> segments(const Speaker& sp, qsizetype a, qsizetype b, int merge = 96)
{
    std::vector<Seg> out;
    b = std::min(b, sp.frames());
    for (qsizetype i = a; i < b; ++i) {
        if (!sp.loud(i)) continue;
        if (!out.empty() && i - out.back().to <= merge) out.back().to = i + 1;
        else out.push_back({i, i + 1});
    }
    return out;
}

// The timeline time (samples) the left channel says at file sample i: value = t / 24 at full level
double timeAt(const Speaker& sp, qsizetype i) { return sp.s[size_t(i) * 2] / 32767.0 * 24.0 * kRate; }

// Largest step from one sample to the next in [a, b), both channels
int largestStep(const Speaker& sp, qsizetype a, qsizetype b, qsizetype* at)
{
    int best = 0;
    for (qsizetype i = std::max<qsizetype>(1, a); i < std::min(b, sp.frames()); ++i)
        for (int c = 0; c < 2; ++c) {
            const int d = std::abs(sp.s[size_t(i) * 2 + c] - sp.s[size_t(i - 1) * 2 + c]);
            if (d > best) {
                best = d;
                *at = i;
            }
        }
    return best;
}

// Waits until the speaker has been silent for `ms` (the slices are over); returns the file length then
qsizetype settle(Speaker& sp, int ms = 250)
{
    sleepMs(ms);
    return sp.size();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QTemporaryDir tmp;
    Speaker sp;
    sp.path = tmp.filePath("speaker.raw");
    qputenv("SCHNEIDI_AUDIO_DEVICE", ("file:" + QDir::toNativeSeparators(sp.path)).toLocal8Bit());
    QApplication app(argc, argv);
    Check::initApp("scrub_audio");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    if (!CHECK(tmp.isValid())) return Check::result();

    const QString tone = Check::makeMedia(tmp.filePath("tone.wav"),
                                          {"-f", "lavfi", "-i", "aevalsrc=t/24|0.5*sin(2*PI*440*t):s=48000:d=12",
                                           "-c:a", "pcm_s16le"});
    const QString other = Check::makeMedia(tmp.filePath("other.wav"),
                                           {"-f", "lavfi", "-i", "aevalsrc=0.3*sin(2*PI*300*t)|0.3*sin(2*PI*300*t):s=48000:d=12",
                                            "-c:a", "pcm_s16le"});
    if (!CHECK(!tone.isEmpty() && !other.isEmpty())) return Check::result();
    Timeline tl;
    tl.audio.resize(1);
    tl.audio[0].clips << mk(1, tone, 0, 360);

    Audio::Player::Options opt;
    opt.buffer = 960;
    auto player = std::make_unique<Audio::Player>(opt);
    player->setTimeline(tl, kFps);

    // 1. Single scrubs: one slice each from the asked time, then silence
    const double asked[] = {2.0, 7.5, 0.5, 11.0};
    std::vector<qsizetype> marks;
    player->scrub(0); // opens the device (the speaker file starts)
    marks.push_back(settle(sp, 400));
    for (double t : asked) {
        player->scrub(t * kRate);
        marks.push_back(settle(sp, 300));
    }
    const Audio::Player::Stats single = player->stats();

    // 2. A slow drag at about real time (a step of 40 ms every 40 ms) in 4-6 s: one sound, joined
    const qsizetype dragFrom = sp.size();
    for (int i = 0; i < 25; ++i) {
        player->scrub((4.0 + i * 0.04) * kRate);
        sleepMs(40);
    }
    const qsizetype dragTo = settle(sp, 300);
    const Audio::Player::Stats dragged = player->stats();

    // 3. A fast burst: 60 requests 1 ms apart across 1-3 s, the last one far away at 10 s
    const qsizetype burstFrom = sp.size();
    for (int i = 0; i < 60; ++i) {
        player->scrub((1.0 + i * 2.0 / 60) * kRate);
        sleepMs(1);
    }
    player->scrub(10.0 * kRate);
    const qsizetype burstTo = settle(sp, 400);
    const Audio::Player::Stats burst = player->stats();

    // 4. Mute and solo
    Timeline muted = tl;
    muted.audio[0].muted = true;
    player->setTimeline(muted, kFps);
    sleepMs(50);
    const qsizetype muteFrom = sp.size();
    player->scrub(3.0 * kRate);
    const qsizetype muteTo = settle(sp, 300);
    Timeline solo = tl;
    solo.audio.resize(2);
    solo.audio[1].clips << mk(2, other, 0, 360);
    solo.audio[1].solo = true;
    player->setTimeline(solo, kFps);
    sleepMs(50);
    const qsizetype soloFrom = sp.size();
    player->scrub(5.0 * kRate);
    const qsizetype soloTo = settle(sp, 300);
    // 4b. A held Left key (a frame back every 25 ms, a fast key repeat): every step plays from its own time, never on past the one before
    player->setTimeline(tl, kFps);
    sleepMs(50);
    const qsizetype backFrom = sp.size();
    std::vector<double> backAsked;
    for (int i = 0; i < 12; ++i) {
        backAsked.push_back((8.0 - i / 30.0) * kRate);
        player->scrub(backAsked.back());
        sleepMs(25);
    }
    const qsizetype backTo = settle(sp, 300);
    // Playing ends scrubbing: no scrub sound afterwards, and play/pause still work
    player->setTimeline(tl, kFps);
    player->play(6.0 * kRate, 1.0);
    player->release();
    sleepMs(150);
    player->pause();
    sleepMs(150);
    player.reset();

    sp.load();
    std::printf("Lautsprecher: %.2f s\n", sp.frames() / double(kRate));

    // 1.
    for (size_t k = 0; k < std::size(asked); ++k) {
        const auto segs = segments(sp, marks[k], marks[k + 1]);
        std::printf("Scrub bei %.2f s: %zu Stück(e)", asked[k], segs.size());
        if (!CHECK_EQ(int(segs.size()), 1)) {
            std::printf("\n");
            continue;
        }
        const Seg& g = segs[0];
        const qsizetype mid = (g.from + g.to) / 2;
        const double heard = timeAt(sp, mid) - double(mid - g.from); // where the slice started, by its own time code
        std::printf(", %.1f ms lang, beginnt bei %.4f s\n", g.length() * 1000.0 / kRate, heard / kRate);
        CHECK(g.length() >= kRate * 40 / 1000 && g.length() <= kRate * 95 / 1000);
        CHECK(std::abs(heard - asked[k] * kRate) < kRate * 0.002); // within 2 ms (time code resolution ~0.7 ms)
    }
    CHECK_EQ(single.underruns, 0); // the end of a slice is no dropout

    // 2.
    {
        const auto segs = segments(sp, dragFrom, dragTo);
        qsizetype longest = 0;
        for (const Seg& g : segs) longest = std::max(longest, g.length());
        const int slices = dragged.scrubSlices - single.scrubSlices, joins = dragged.scrubJoins - single.scrubJoins;
        std::printf("Langsames Ziehen: %zu Stück(e), längstes %.0f ms, %d Scheiben, davon %d nahtlos angehängt\n",
                    segs.size(), longest * 1000.0 / kRate, slices, joins);
        CHECK(joins >= slices / 2 && joins >= 5);
        CHECK(longest > kRate * 0.6); // one sound, not 25 bits
    }

    // 3.
    {
        const auto segs = segments(sp, burstFrom, burstTo);
        qsizetype total = 0;
        for (const Seg& g : segs) total += g.length();
        const int replaced = burst.scrubReplaced - dragged.scrubReplaced;
        const int slices = burst.scrubSlices - dragged.scrubSlices;
        std::printf("Schnelle Folge: 61 Anfragen, %d verworfen, %d Scheiben, %.0f ms Ton", replaced, slices,
                    total * 1000.0 / kRate);
        CHECK(replaced >= 45);
        CHECK(slices <= 8);
        CHECK(total < kRate * 0.35); // no queue (61 slices would be seconds)
        if (CHECK(!segs.empty())) {
            const Seg& g = segs.back();
            const double at = timeAt(sp, g.to - kRate / 50) / kRate; // inside the last slice (the slices before join it)
            std::printf(", zuletzt bei %.2f s\n", at);
            CHECK(at > 9.9 && at < 10.2); // the newest position wins
        } else {
            std::printf("\n");
        }
    }

    // 4.
    {
        const auto m = segments(sp, muteFrom, muteTo);
        const auto s = segments(sp, soloFrom, soloTo);
        int soloRight = 0, soloLeft = 0; // the soloed track: 300 Hz in both channels; the tone's time code muted
        if (!s.empty())
            for (qsizetype i = s[0].from; i < s[0].to; ++i) {
                soloLeft = std::max(soloLeft, std::abs(int(sp.s[size_t(i) * 2])));
                soloRight = std::max(soloRight, std::abs(int(sp.s[size_t(i) * 2 + 1])));
            }
        std::printf("Stumm: %zu Stück(e); Solo der anderen Spur: %zu Stück(e), Spitze %d/%d\n", m.size(), s.size(),
                    soloLeft, soloRight);
        CHECK(m.empty());
        CHECK_EQ(int(s.size()), 1);
        CHECK(soloLeft > 6000 && soloLeft < 11000 && std::abs(soloLeft - soloRight) < 600); // 0.3 alone, no time code
    }

    // 4b.
    {
        double highest = 0;
        for (qsizetype i = backFrom; i < std::min(backTo, sp.frames()); ++i)
            if (sp.loud(i)) highest = std::max(highest, timeAt(sp, i));
        // Each step's start: its own time 5-7 ms in (behind the fade-in) somewhere in what was heard
        int found = 0;
        for (double p : backAsked) {
            const double want = p + 0.006 * kRate;
            for (qsizetype i = backFrom; i < std::min(backTo, sp.frames()); ++i)
                if (std::abs(timeAt(sp, i) - want) < 0.001 * kRate) {
                    ++found;
                    break;
                }
        }
        std::printf("Rückwärts-Schritte: höchste gehörte Zeit %.4f s (erster Schritt 8,0 s), %d von %zu Schritten von "
                    "ihrer Zeit an gehört\n",
                    highest / kRate, found, backAsked.size());
        CHECK(highest < backAsked[0] + 0.090 * kRate); // never on past the first step's slice
        CHECK(found >= int(backAsked.size()) - 2);       // (a step may be replaced by the next before it is mixed)
    }

    // 5. No step anywhere (440 Hz at 0.5 changes by < 950 per sample; a cut of the time code or the sine: thousands)
    {
        qsizetype at = 0;
        const int step = largestStep(sp, 0, sp.frames(), &at);
        std::printf("Größter Sprung von Sample zu Sample: %d bei %.3f s\n", step, at / double(kRate));
        CHECK(step < 1300);
    }

    // Listening (optional, by hand): SCHNEIDI_SCRUB_SOURCE=<a media file> scrubs it like a user - a slow drag forward,
    // a fast one, back, frame steps, a pause - and copies what the speaker got (raw s16 stereo 48 kHz) to
    // SCHNEIDI_SCRUB_OUT, for listening and click checks outside (earshot)
    if (const QString source = qEnvironmentVariable("SCHNEIDI_SCRUB_SOURCE"); !source.isEmpty()) {
        const QString out = qEnvironmentVariable("SCHNEIDI_SCRUB_OUT");
        const QString raw = tmp.filePath("listen.raw");
        qputenv("SCHNEIDI_AUDIO_DEVICE", ("file:" + QDir::toNativeSeparators(raw)).toLocal8Bit());
        Timeline lt;
        lt.audio.resize(1);
        lt.audio[0].clips << mk(1, source, 0, 30 * 60);
        auto listen = std::make_unique<Audio::Player>(opt);
        listen->setTimeline(lt, kFps);
        double t = 5.0;
        const auto step = [&](double to, double seconds, int everyMs) { // a drag from t to `to`, mouse events every ms
            const int n = std::max(1, int(seconds * 1000 / everyMs));
            const double from = t;
            for (int i = 1; i <= n; ++i) {
                t = from + (to - from) * i / n;
                listen->scrub(t * kRate);
                sleepMs(everyMs);
            }
        };
        step(5.0, 0.3, 16);
        step(7.0, 2.0, 16);  // real time
        step(7.5, 1.5, 16);  // slow
        sleepMs(400);        // the mouse holds still
        step(12.0, 0.8, 8);  // fast
        step(9.0, 1.0, 16);  // back
        sleepMs(400);
        for (int i = 0; i < 20; ++i) { // frame steps (a held arrow key: 30 per second)
            t += 1.0 / 30;
            listen->scrub(t * kRate);
            sleepMs(33);
        }
        sleepMs(400);
        const Audio::Player::Stats ls = listen->stats();
        listen.reset();
        std::printf("Anhören: %d Anfragen, %d Scheiben, %d nahtlos, %d verworfen, %d gekürzt\n", ls.scrubRequests,
                    ls.scrubSlices, ls.scrubJoins, ls.scrubReplaced, ls.scrubCuts);
        if (!out.isEmpty()) {
            QFile::remove(out);
            CHECK(QFile::copy(raw, out));
        }
    }

    // 6. Engine and setting
    {
        CHECK(TimelineOptions::scrubAudio()); // on by default
        TimelineOptions::setScrubAudio(false);
        CHECK(!TimelineOptions::scrubAudio());
        CHECK_EQ(QSettings().value("timeline/scrubAudio").toString(), QString("off"));
        TimelineOptions::setScrubAudio(true);
        CHECK(TimelineOptions::scrubAudio());
        CHECK(!QSettings().contains("timeline/scrubAudio")); // the default is not stored

        Speaker sp2;
        sp2.path = tmp.filePath("engine.raw");
        qputenv("SCHNEIDI_AUDIO_DEVICE", ("file:" + QDir::toNativeSeparators(sp2.path)).toLocal8Bit());
        Engine engine;
        QString error;
        if (!engine.init(&error)) {
            std::printf("Engine: %s\n", qPrintable(error));
            return Check::result();
        }
        engine.setOpenInBackground(true);
        engine.setFormat(ProjectFormat{320, 180, kFps});
        Timeline etl = tl;
        engine.updateTimeline(etl);
        wait(800);
        engine.showTimeline(0);
        wait(300);
        CHECK(engine.scrubAudio());
        engine.scrubTo(60); // 2 s
        wait(400);
        const int after = engine.audioStats().scrubRequests;
        engine.seek(150); // a plain seek: silent
        wait(200);
        const int afterSeek = engine.audioStats().scrubRequests;
        engine.setScrubAudio(false);
        const qsizetype offFrom = sp2.size();
        engine.scrubTo(200);
        engine.scrubTo(201);
        wait(400);
        const qsizetype offTo = sp2.size();
        sp2.load();
        const auto on = segments(sp2, 0, offFrom);
        const auto off = segments(sp2, offFrom, offTo);
        std::printf("Engine: scrubTo %d Anfrage(n), seek +%d; an %zu Stück(e), aus %zu Stück(e)\n", after,
                    afterSeek - after, on.size(), off.size());
        CHECK_EQ(after, 1);
        CHECK_EQ(afterSeek, after);
        CHECK_EQ(engine.audioStats().scrubRequests, after);
        if (CHECK_EQ(int(on.size()), 1)) {
            const Seg& g = on[0];
            const qsizetype mid = (g.from + g.to) / 2;
            const double heard = (sp2.s[size_t(mid) * 2] / 32767.0 * 24.0 * kRate - double(mid - g.from)) / kRate;
            std::printf("Engine-Scheibe beginnt bei %.4f s (Frame 60 = 2 s)\n", heard);
            CHECK(std::abs(heard - 2.0) < 0.002);
        }
        CHECK(off.empty());
    }
    return Check::result();
}
