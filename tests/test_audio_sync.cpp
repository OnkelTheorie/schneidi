// Audio sync for multicam (docs/plan-0.8.md): onset envelope, offset between two recordings that only share a few
// sounds (round start, spike, announcements) while each player hears his own sounds, cases where it must not guess
// (silence, nothing shared, only periodic beeps), drift between two clocks, chains of recordings that do not all
// overlap, real files (sample rates, several streams, no audio, cancel), sync by source marker / In point and the
// fallback from sound to marker. Source markers in the Media Pool and the project file.
#include "check.h"

#include "core/AudioSync.h"
#include "core/Editor.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "engine/SoundSync.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace {

constexpr int kRate = 8000; // synthetic recordings: low rate keeps hours of audio fast
constexpr double kPi = 3.14159265358979323846;
constexpr double kEventLength = 0.3; // seconds

// A sound everyone who hears it hears the same way (same kind = same waveform): a few partials with a fast attack
// and an exponential decay, defined over time so it is the same at any sample rate
struct Event {
    double t = 0; // seconds in the reference recording
    int kind = 0;
    double gain = 1;
};

uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

double eventSample(int kind, double t)
{
    if (t < 0 || t >= kEventLength) return 0;
    const double env = std::min(1.0, t / 0.005) * std::exp(-t / 0.06);
    double s = 0;
    for (int k = 0; k < 4; ++k) {
        const uint32_t h = hash(uint32_t(kind) * 4u + uint32_t(k) + 1u);
        const double f = 200 + double(h % 2800);
        const double phase = double((h >> 12) % 628) / 100.0;
        s += std::sin(2 * kPi * f * t + phase);
    }
    return env * s / 4;
}

double noiseSample(uint32_t seed, int64_t n)
{
    return double(hash(seed * 2654435761U ^ uint32_t(n) ^ uint32_t(n >> 32))) / 2147483648.0 - 1.0;
}

// One recording of the event (one player's POV): shares `shared` events, has its own sounds and noise
struct Recording {
    double length = 60;    // seconds
    double offset = 0;     // starts `offset` s after the reference: a shared event at reference time T plays at T - offset
    double sharedGain = 0.5;
    double ownGain = 0.5;
    double noise = 0.01;
    double ppm = 0;        // clock runs this much faster than the reference
    int ownSeed = 1;       // own sounds (nobody else hears them)
    double ownEvery = 2.0; // mean gap between own sounds (s)
    double lowpassHz = 0;  // 0 = none (other mix/position in the game: shared sounds duller)
};

QVector<Event> ownEvents(const Recording& r)
{
    QVector<Event> e;
    std::mt19937 rng{uint32_t(r.ownSeed)};
    std::exponential_distribution<double> gap(1.0 / r.ownEvery);
    for (double t = gap(rng); t < r.length; t += gap(rng)) e << Event{t, 100000 + r.ownSeed * 1000 + int(e.size()), 1};
    return e;
}

// Renders the recording in blocks of one second (hours of audio never sit in memory)
void renderBlocks(const Recording& r, const QVector<Event>& shared, int rate,
                  const std::function<void(const float*, int)>& sink)
{
    struct Placed { double t; int kind; double gain; };
    std::vector<Placed> events;
    for (const Event& e : shared) {
        const double local = (e.t - r.offset) * (1.0 + r.ppm * 1e-6);
        if (local > -kEventLength && local < r.length) events.push_back({local, e.kind, r.sharedGain * e.gain});
    }
    if (r.ownGain > 0)
        for (const Event& e : ownEvents(r)) events.push_back({e.t, e.kind, r.ownGain});
    std::sort(events.begin(), events.end(), [](const Placed& a, const Placed& b) { return a.t < b.t; });

    const int64_t total = int64_t(std::llround(r.length * rate));
    const double a = r.lowpassHz > 0 ? 1.0 - std::exp(-2 * kPi * r.lowpassHz / rate) : 1.0;
    double lp = 0;
    size_t first = 0;
    std::vector<float> block;
    for (int64_t n0 = 0; n0 < total; n0 += rate) {
        const int n = int(std::min<int64_t>(rate, total - n0));
        block.assign(size_t(n), 0.f);
        for (int i = 0; i < n; ++i) {
            const double t = double(n0 + i) / rate;
            while (first < events.size() && events[first].t + kEventLength <= t) ++first;
            double s = r.noise * noiseSample(uint32_t(r.ownSeed) + 77u, n0 + i);
            for (size_t k = first; k < events.size() && events[k].t <= t; ++k)
                s += events[k].gain * eventSample(events[k].kind, t - events[k].t);
            lp += a * (s - lp);
            block[size_t(i)] = float(lp);
        }
        sink(block.data(), n);
    }
}

std::vector<float> render(const Recording& r, const QVector<Event>& shared, int rate = kRate)
{
    std::vector<float> all;
    renderBlocks(r, shared, rate, [&](const float* d, int n) { all.insert(all.end(), d, d + n); });
    return all;
}

std::vector<float> env(const Recording& r, const QVector<Event>& shared, int rate = kRate)
{
    AudioSync::Envelope e(rate);
    renderBlocks(r, shared, rate, [&](const float* d, int n) { e.add(d, n); });
    return e.take();
}

// Shared moments of a match: round starts, spike, announcements – each a different sound, every ~`every` seconds
QVector<Event> sharedEvents(double from, double to, double every, int seed = 7)
{
    QVector<Event> e;
    std::mt19937 rng{uint32_t(seed)};
    std::uniform_real_distribution<double> jitter(-0.3 * every, 0.3 * every);
    int kind = 1;
    for (double t = from + every; t < to; t += every) e << Event{t + jitter(rng), kind++, 1};
    return e;
}

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

MediaInfo media(const QString& path, int length, bool video = true, bool audio = true)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = length;
    m.hasVideo = video;
    m.hasAudio = audio;
    return m;
}

bool writeWav(const QString& path, const std::vector<float>& mono, int rate)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 data = quint32(mono.size() * 2);
    auto le32 = [&](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        f.write(b, 4);
    };
    auto le16 = [&](quint16 v) {
        char b[2];
        qToLittleEndian(v, b);
        f.write(b, 2);
    };
    f.write("RIFF");
    le32(36 + data);
    f.write("WAVEfmt ");
    le32(16);
    le16(1);
    le16(1);
    le32(quint32(rate));
    le32(quint32(rate) * 2);
    le16(2);
    le16(16);
    f.write("data");
    le32(data);
    for (float s : mono) le16(quint16(qint16(std::clamp(s, -1.f, 1.f) * 32767)));
    return true;
}

// ---- Envelope ----

void testEnvelopeRateAndLength()
{
    const QVector<Event> shared = sharedEvents(0, 20, 2);
    Recording r;
    r.length = 20;
    for (int rate : {8000, 44100, 48000}) {
        const std::vector<float> e = env(r, shared, rate);
        CHECK(std::abs(int(e.size()) - 20 * AudioSync::kEnvelopeRate) <= 1);
    }
    CHECK_EQ(AudioSync::kEnvelopeRate, 200);
    CHECK(AudioSync::envelope({}, kRate).empty());
}

void testEnvelopeBlockSizeIndependent()
{
    Recording r;
    r.length = 10;
    const std::vector<float> audio = render(r, sharedEvents(0, 10, 1.5));
    const std::vector<float> whole = AudioSync::envelope(audio, kRate);
    for (int block : {1, 7, 333, 4096}) {
        AudioSync::Envelope e(kRate);
        for (size_t i = 0; i < audio.size(); i += size_t(block))
            e.add(audio.data() + i, int(std::min<size_t>(size_t(block), audio.size() - i)));
        const std::vector<float> parts = e.take();
        bool same = parts.size() == whole.size();
        for (size_t i = 0; same && i < parts.size(); ++i) same = std::abs(parts[i] - whole[i]) < 1e-4f;
        CHECK(same);
    }
}

// ---- Two recordings ----

void testFindsOffsetWithSharedEventsOnly()
{
    // Reference 3 min; the other POV starts 12.34 s later and hears its own (louder) sounds plus the shared ones
    const QVector<Event> shared = sharedEvents(0, 200, 8);
    Recording ref;
    ref.length = 180;
    Recording other;
    other.length = 150;
    other.offset = 12.34;
    other.ownSeed = 2;
    other.ownGain = 0.8;
    other.sharedGain = 0.4;
    const AudioSync::Match m = AudioSync::match(env(ref, shared), env(other, shared));
    CHECK(m.found);
    CHECK(near(m.offsetS, 12.34, 0.002));
    CHECK(m.confidence > 0 && m.confidence <= 1);
    CHECK(near(m.driftMs, 0, 5));
}

void testNegativeOffset()
{
    // The other recording started 41.5 s before the reference
    const QVector<Event> shared = sharedEvents(-60, 120, 6);
    Recording ref;
    ref.length = 100;
    Recording other;
    other.length = 120;
    other.offset = -41.5;
    other.ownSeed = 3;
    const AudioSync::Match m = AudioSync::match(env(ref, shared), env(other, shared));
    CHECK(m.found);
    CHECK(near(m.offsetS, -41.5, 0.002));
}

void testDifferentLevelsAndColoring()
{
    // Shared sounds ~18 dB quieter than the own ones and dull (far away in the game, other headset mix)
    const QVector<Event> shared = sharedEvents(0, 120, 6);
    Recording ref;
    ref.length = 120;
    Recording other;
    other.length = 100;
    other.offset = 7.75;
    other.ownSeed = 4;
    other.sharedGain = 0.08;
    other.ownGain = 0.6;
    other.lowpassHz = 1200;
    const AudioSync::Match m = AudioSync::match(env(ref, shared), env(other, shared));
    CHECK(m.found);
    CHECK(near(m.offsetS, 7.75, 0.003));
}

void testPartialOverlap()
{
    // Only the last 30 s of the reference overlap the other recording (about 12 shared moments)
    const QVector<Event> shared = sharedEvents(0, 300, 2.5);
    Recording ref;
    ref.length = 180;
    ref.ownEvery = 4;
    Recording other;
    other.length = 90;
    other.offset = 150;
    other.ownSeed = 5;
    other.ownEvery = 4;
    const AudioSync::Match m = AudioSync::match(env(ref, shared), env(other, shared));
    CHECK(m.found);
    CHECK(near(m.offsetS, 150, 0.002));
}

void testDifferentSampleRatesMatch()
{
    // The same sound recorded at 44.1 and 48 kHz: envelopes line up at offset 0
    const QVector<Event> shared = sharedEvents(0, 40, 2);
    Recording r;
    r.length = 40;
    const AudioSync::Match m = AudioSync::match(env(r, shared, 44100), env(r, shared, 48000));
    CHECK(m.found);
    CHECK(near(m.offsetS, 0, 0.002));
}

void testLongRecordingLargeOffset()
{
    // A whole tournament: reference 2 h, the other POV 40 min from minute 47; must stay fast (no O(n²) search)
    const QVector<Event> shared = sharedEvents(0, 7300, 20);
    Recording ref;
    ref.length = 7200;
    ref.ownEvery = 4;
    Recording other;
    other.length = 2400;
    other.offset = 2820.48;
    other.ownSeed = 6;
    other.ownEvery = 4;
    const std::vector<float> a = env(ref, shared), b = env(other, shared);
    QElapsedTimer t;
    t.start();
    const AudioSync::Match m = AudioSync::match(a, b);
    CHECK(t.elapsed() < 30000);
    CHECK(m.found);
    CHECK(near(m.offsetS, 2820.48, 0.003));
}

void testSilenceIsNotFound()
{
    const QVector<Event> shared = sharedEvents(0, 60, 4);
    Recording ref;
    ref.length = 60;
    Recording silent;
    silent.length = 60;
    silent.sharedGain = 0;
    silent.ownGain = 0;
    silent.noise = 0;
    const AudioSync::Match m = AudioSync::match(env(ref, shared), env(silent, shared));
    CHECK(!m.found);
    CHECK(AudioSync::match(env(silent, shared), env(silent, shared)).found == false);
    CHECK(!AudioSync::match({}, env(ref, shared)).found);
}

void testUnrelatedRecordingsNotFound()
{
    // Two recordings of different matches: no shared sound, only own sounds and noise
    Recording a;
    a.length = 120;
    a.ownSeed = 11;
    Recording b;
    b.length = 120;
    b.ownSeed = 12;
    const AudioSync::Match m = AudioSync::match(env(a, {}), env(b, {}));
    CHECK(!m.found);
}

void testPeriodicBeepsAloneAreAmbiguous()
{
    // Only a beep every 2 s is shared (identical each time): every shift by 2 s fits about as well -> no guess
    QVector<Event> beeps;
    for (double t = 1; t < 80; t += 2) beeps << Event{t, 42, 0.25};
    Recording ref;
    ref.length = 60;
    ref.ownGain = 0.2;
    Recording other;
    other.length = 60;
    other.offset = 7.3;
    other.ownSeed = 13;
    other.ownGain = 0.2;
    CHECK(!AudioSync::match(env(ref, beeps), env(other, beeps)).found);

    // Distinct shared moments (announcements, round start) between the beeps make it clear again
    QVector<Event> plus = beeps;
    int kind = 7;
    for (double t : {10.6, 14.1, 19.9, 24.3, 30.4, 33.8, 39.2, 45.7, 50.1, 56.6}) plus << Event{t, kind++, 1};
    const AudioSync::Match m = AudioSync::match(env(ref, plus), env(other, plus));
    CHECK(m.found);
    CHECK(near(m.offsetS, 7.3, 0.003));
}

void testMaxOffsetLimitsSearch()
{
    const QVector<Event> shared = sharedEvents(0, 300, 5);
    Recording ref;
    ref.length = 240;
    Recording other;
    other.length = 100;
    other.offset = 100;
    other.ownSeed = 14;
    AudioSync::Options o;
    o.maxOffsetS = 30;
    CHECK(!AudioSync::match(env(ref, shared), env(other, shared), o).found);
    other.offset = 20;
    const AudioSync::Match m = AudioSync::match(env(ref, shared), env(other, shared), o);
    CHECK(m.found);
    CHECK(near(m.offsetS, 20, 0.002));
}

void testDriftIsReported()
{
    // The other PC's clock runs 100 ppm fast: after 20 min its sound is 120 ms early. Still found, offset between the
    // values at the start and at the end, drift reported (offset at the end minus at the start)
    const QVector<Event> shared = sharedEvents(0, 1300, 6);
    Recording ref;
    ref.length = 1250;
    Recording other;
    other.length = 1200;
    other.offset = 30;
    other.ppm = 100;
    other.ownSeed = 15;
    const AudioSync::Match m = AudioSync::match(env(ref, shared), env(other, shared));
    CHECK(m.found);
    CHECK(m.offsetS <= 30.005 && m.offsetS >= 29.875);
    CHECK(near(m.driftMs, -120, 25));
}

// ---- Several recordings ----

void testAlignAllChainsThroughOtherRecordings()
{
    // A 0–600 s (reference), B 400–1000 s, C 900–1500 s: C never overlaps A, but B links them.
    // D is from another match.
    const QVector<Event> shared = sharedEvents(0, 1600, 8);
    Recording a;
    a.length = 600;
    Recording b;
    b.length = 600;
    b.offset = 400;
    b.ownSeed = 21;
    Recording c;
    c.length = 600;
    c.offset = 900;
    c.ownSeed = 22;
    Recording d;
    d.length = 300;
    d.ownSeed = 23;
    const QVector<AudioSync::Match> m = AudioSync::alignAll({env(a, shared), env(b, shared), env(c, shared), env(d, {})});
    if (!CHECK_EQ(m.size(), 4)) return;
    CHECK(m[0].found && m[0].offsetS == 0 && m[0].via == -1);
    CHECK(m[1].found && near(m[1].offsetS, 400, 0.003) && m[1].via == 0);
    CHECK(m[2].found && near(m[2].offsetS, 900, 0.005) && m[2].via == 1);
    CHECK(!m[3].found);
}

void testAlignAllEdgeCases()
{
    CHECK(AudioSync::alignAll({}).isEmpty());
    const QVector<Event> shared = sharedEvents(0, 60, 3);
    Recording a;
    a.length = 60;
    const QVector<AudioSync::Match> one = AudioSync::alignAll({env(a, shared)});
    CHECK(one.size() == 1 && one[0].found && one[0].offsetS == 0);
    // Silent reference: the others match each other but not the reference -> not found (offsets are relative to it)
    Recording silent;
    silent.length = 60;
    silent.sharedGain = silent.ownGain = silent.noise = 0;
    Recording b = a;
    b.offset = 5;
    b.ownSeed = 31;
    Recording c = a;
    c.offset = 9;
    c.ownSeed = 32;
    const QVector<AudioSync::Match> m = AudioSync::alignAll({env(silent, shared), env(b, shared), env(c, shared)});
    CHECK(m.size() == 3 && m[0].found && !m[1].found && !m[2].found);
}

// ---- Real files (decoding) ----

void testSoundSyncOnFiles(const QString& dir)
{
    if (!Check::haveFfmpeg()) return;
    const QVector<Event> shared = sharedEvents(-10, 60, 2.5);
    Recording r1;
    r1.length = 30;
    Recording r2;
    r2.length = 20;
    r2.offset = 3.48;
    r2.ownSeed = 41;
    Recording game3; // stream 1 of the mkv: game sound with the shared moments
    game3.length = 25;
    game3.offset = -2.2;
    game3.ownSeed = 42;
    Recording mic3; // stream 0: only the player's voice
    mic3.length = 25;
    mic3.sharedGain = 0;
    mic3.ownSeed = 43;
    const QString w1 = dir + "/a1.wav", w3a = dir + "/a3mic.wav", w3b = dir + "/a3game.wav";
    const QString wav2 = dir + "/angle2.wav";
    if (!CHECK(writeWav(w1, render(r1, shared, 48000), 48000) && writeWav(wav2, render(r2, shared, 44100), 44100)
               && writeWav(w3a, render(mic3, shared, 48000), 48000) && writeWav(w3b, render(game3, shared, 48000), 48000)))
        return;
    const QString mp4 = Check::makeMedia(dir + "/angle1.mp4", {"-f", "lavfi", "-i", "color=red:size=160x90:rate=25:duration=30",
                                                               "-i", w1, "-c:v", "libx264", "-preset", "ultrafast",
                                                               "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    const QString mkv = Check::makeMedia(dir + "/angle3.mkv", {"-f", "lavfi", "-i", "color=blue:size=160x90:rate=25:duration=25",
                                                               "-i", w3a, "-i", w3b, "-map", "0:v", "-map", "1:a", "-map", "2:a",
                                                               "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                                               "-c:a", "pcm_s16le", "-shortest"});
    const QString silent = Check::makeMedia(dir + "/silent.mp4", {"-f", "lavfi", "-i", "color=green:size=160x90:rate=25:duration=10",
                                                                  "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!mp4.isEmpty() && !mkv.isEmpty() && !silent.isEmpty())) return;

    CHECK(SoundSync::fileEnvelope(mp4, 0).has_value());
    CHECK(!SoundSync::fileEnvelope(silent, 0).has_value());
    CHECK(!SoundSync::fileEnvelope(dir + "/missing.mp4", 0).has_value());
    CHECK(!SoundSync::fileEnvelope(mkv, 5).has_value()); // no such stream

    double lastProgress = -1;
    bool monotonic = true;
    const QVector<AudioSync::Match> m = SoundSync::align(
        {{mp4, 0}, {wav2, 0}, {mkv, 1}, {silent, 0}, {dir + "/missing.mp4", 0}}, {}, [&](double p) {
            monotonic = monotonic && p >= lastProgress && p <= 1.0;
            lastProgress = p;
            return true;
        });
    if (!CHECK_EQ(m.size(), 5)) return;
    // Within half a frame at 25 fps (AAC priming must not shift the result)
    CHECK(m[0].found && m[0].offsetS == 0);
    CHECK(m[1].found && near(m[1].offsetS, 3.48, 0.02));
    CHECK(m[2].found && near(m[2].offsetS, -2.2, 0.02));
    CHECK(!m[3].found);
    CHECK(!m[4].found);
    CHECK(monotonic && lastProgress > 0.99);

    // The mic stream of the same file hears nothing shared
    const QVector<AudioSync::Match> mic = SoundSync::align({{mp4, 0}, {mkv, 0}});
    CHECK(mic.size() == 2 && !mic[1].found);
}

void testSoundSyncCancel(const QString& dir)
{
    if (!Check::haveFfmpeg()) return;
    const QString a = Check::makeMedia(dir + "/cancel.wav", {"-f", "lavfi", "-i", "sine=frequency=440:duration=20"});
    if (!CHECK(!a.isEmpty())) return;
    int calls = 0;
    const QVector<AudioSync::Match> m = SoundSync::align({{a, 0}, {a, 0}}, {}, [&](double) { return ++calls < 2; });
    CHECK(m.isEmpty());
    CHECK(calls >= 2);
}

// ---- Sync without sound ----

void testResolveByMarker()
{
    Project p;
    MediaInfo a = media("/x/pov1.mp4", 1000);
    a.markers = {100, 400};
    MediaInfo b = media("/x/pov2.mp4", 1000);
    b.markers = {30};
    MediaInfo c = media("/x/pov3.mp4", 1000); // no marker
    p.addMedia(a);
    p.addMedia(b);
    p.addMedia(c);
    using M = Multicam::SyncMethod;
    const QVector<Multicam::Angle> angles =
        Multicam::resolve(p, {{"Spieler 1", {"/x/pov1.mp4"}}, {"", {"/x/pov2.mp4"}}, {"", {"/x/pov3.mp4"}}}, M::Marker);
    if (!CHECK_EQ(angles.size(), 3)) return;
    CHECK_EQ(angles[0].name, QString("Spieler 1"));
    CHECK_EQ(angles[1].name, QString("pov2")); // default: file name without extension
    const Multicam::FilePlacement& r = angles[0].files.value(0);
    CHECK(r.path == "/x/pov1.mp4" && r.offset == 0 && r.synced && r.via == M::Marker);
    // First markers line up: pov2 frame 30 lies on pov1 frame 100
    const Multicam::FilePlacement& f2 = angles[1].files.value(0);
    CHECK(f2.offset == 70 && f2.synced && f2.via == M::Marker);
    const Multicam::FilePlacement& f3 = angles[2].files.value(0);
    CHECK(f3.offset == 0 && !f3.synced && f3.via == M::Start);

    // Reference without a marker: nobody can be synced by marker
    Project q;
    q.addMedia(c);
    q.addMedia(b);
    const QVector<Multicam::Angle> none = Multicam::resolve(q, {{"", {"/x/pov3.mp4"}}, {"", {"/x/pov2.mp4"}}}, M::Marker);
    CHECK(none.size() == 2 && !none[1].files.value(0).synced);
}

void testResolveByInPointAndStart()
{
    Project p;
    MediaInfo a = media("/x/a.mp4", 500);
    a.markIn = 50;
    MediaInfo b = media("/x/b.mp4", 500);
    b.markIn = 10;
    MediaInfo c = media("/x/c.mp4", 500); // no In point = frame 0
    p.addMedia(a);
    p.addMedia(b);
    p.addMedia(c);
    using M = Multicam::SyncMethod;
    const QVector<Multicam::Angle> in = Multicam::resolve(p, {{"", {"/x/a.mp4"}}, {"", {"/x/b.mp4"}}, {"", {"/x/c.mp4"}}}, M::InPoint);
    if (!CHECK_EQ(in.size(), 3)) return;
    CHECK(in[1].files.value(0).offset == 40 && in[1].files.value(0).synced);
    CHECK(in[2].files.value(0).offset == 50 && in[2].files.value(0).synced);
    const QVector<Multicam::Angle> st = Multicam::resolve(p, {{"", {"/x/a.mp4"}}, {"", {"/x/b.mp4"}}}, M::Start);
    CHECK(st.size() == 2 && st[1].files.value(0).offset == 0 && st[1].files.value(0).via == M::Start);
}

void testResolveSplitRecordingPlacedEndToEnd()
{
    // One POV in two files (OBS restarted): without sync info the second file follows the first (no overlap)
    Project p;
    p.addMedia(media("/x/ref.mp4", 1000));
    p.addMedia(media("/x/part1.mp4", 300));
    p.addMedia(media("/x/part2.mp4", 400));
    const QVector<Multicam::Angle> a =
        Multicam::resolve(p, {{"", {"/x/ref.mp4"}}, {"POV", {"/x/part1.mp4", "/x/part2.mp4"}}}, Multicam::SyncMethod::Start);
    if (!CHECK(a.size() == 2 && a[1].files.size() == 2)) return;
    CHECK_EQ(a[1].files[0].offset, 0);
    CHECK_EQ(a[1].files[1].offset, 300);
    CHECK(!a[1].files[1].synced);
}

void testResolveSoundFallsBackToMarker()
{
    Project p;
    MediaInfo ref = media("/x/ref.mp4", 2500);
    ref.markers = {500};
    MediaInfo b = media("/x/b.mp4", 2500);
    MediaInfo c = media("/x/c.mp4", 2500);
    c.markers = {125};
    MediaInfo d = media("/x/d.mp4", 2500); // neither sound nor marker
    p.addMedia(ref);
    p.addMedia(b);
    p.addMedia(c);
    p.addMedia(d);
    AudioSync::Match ok;
    ok.found = true;
    ok.offsetS = 2.0;
    ok.confidence = 0.8;
    AudioSync::Match self;
    self.found = true;
    QVector<AudioSync::Match> sound{self, ok, AudioSync::Match{}, AudioSync::Match{}};
    using M = Multicam::SyncMethod;
    const QVector<Multicam::Angle> a = Multicam::resolve(
        p, {{"", {"/x/ref.mp4"}}, {"", {"/x/b.mp4"}}, {"", {"/x/c.mp4"}}, {"", {"/x/d.mp4"}}}, M::Sound, sound);
    if (!CHECK_EQ(a.size(), 4)) return;
    const Multicam::FilePlacement& fb = a[1].files.value(0);
    CHECK(fb.offset == 50 && fb.synced && fb.via == M::Sound && near(fb.confidence, 0.8, 1e-9)); // 2 s at 25 fps
    const Multicam::FilePlacement& fc = a[2].files.value(0);
    CHECK(fc.offset == 375 && fc.synced && fc.via == M::Marker);
    const Multicam::FilePlacement& fd = a[3].files.value(0);
    CHECK(!fd.synced && fd.via == M::Start);
    // Without the fallback the marker is not used
    const QVector<Multicam::Angle> strict = Multicam::resolve(
        p, {{"", {"/x/ref.mp4"}}, {"", {"/x/b.mp4"}}, {"", {"/x/c.mp4"}}, {"", {"/x/d.mp4"}}}, M::Sound, sound, false);
    CHECK(strict.size() == 4 && !strict[2].files.value(0).synced);
    // Rounded to whole frames (video and sound of an angle move together)
    sound[1].offsetS = 2.019; // 50.475 frames
    CHECK_EQ(Multicam::resolve(p, {{"", {"/x/ref.mp4"}}, {"", {"/x/b.mp4"}}}, M::Sound, sound.mid(0, 2))
                 .value(1).files.value(0).offset, 50);
}

// ---- Source markers (M in the source viewer) ----

void testSourceMarkers()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia(media("/x/a.mp4", 100));
    p.markSaved();
    const int steps = p.undoStack()->count();
    ed.toggleSourceMarker("/x/a.mp4", 40);
    ed.toggleSourceMarker("/x/a.mp4", 10);
    ed.toggleSourceMarker("/x/a.mp4", 70);
    CHECK_EQ(p.mediaInfo("/x/a.mp4")->markers, QVector<int>({10, 40, 70}));
    ed.toggleSourceMarker("/x/a.mp4", 40); // again = remove
    CHECK_EQ(p.mediaInfo("/x/a.mp4")->markers, QVector<int>({10, 70}));
    ed.toggleSourceMarker("/x/a.mp4", -1);  // outside the material
    ed.toggleSourceMarker("/x/a.mp4", 100);
    ed.toggleSourceMarker("/x/missing.mp4", 5);
    CHECK_EQ(p.mediaInfo("/x/a.mp4")->markers, QVector<int>({10, 70}));
    CHECK_EQ(p.undoStack()->count(), steps); // like source In/Out: not in the undo history …
    CHECK(p.isModified());                   // … but a change of the project

    // Project file: saved, loaded, saved again the same; old files without markers read as none
    const QByteArray json = ProjectFile::toJson(p.data(), "/x/p.schneidi");
    ProjectData back;
    QString err;
    if (!CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err))) return;
    CHECK_EQ(back.media.value(0).markers, QVector<int>({10, 70}));
    Project q;
    q.load(back);
    CHECK_EQ(ProjectFile::toJson(q.data(), "/x/p.schneidi"), json);
    ProjectData old = back;
    old.media[0].markers.clear();
    const QJsonObject oldMedia =
        QJsonDocument::fromJson(ProjectFile::toJson(old, "/x/p.schneidi")).object().value("media").toArray().at(0).toObject();
    CHECK(!oldMedia.isEmpty() && !oldMedia.contains("markers")); // optional field, like markIn
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("audio-sync");
    QTemporaryDir dir;

    testEnvelopeRateAndLength();
    testEnvelopeBlockSizeIndependent();
    testFindsOffsetWithSharedEventsOnly();
    testNegativeOffset();
    testDifferentLevelsAndColoring();
    testPartialOverlap();
    testDifferentSampleRatesMatch();
    testLongRecordingLargeOffset();
    testSilenceIsNotFound();
    testUnrelatedRecordingsNotFound();
    testPeriodicBeepsAloneAreAmbiguous();
    testMaxOffsetLimitsSearch();
    testDriftIsReported();
    testAlignAllChainsThroughOtherRecordings();
    testAlignAllEdgeCases();
    testResolveByMarker();
    testResolveByInPointAndStart();
    testResolveSplitRecordingPlacedEndToEnd();
    testResolveSoundFallsBackToMarker();
    testSourceMarkers();

    // Decoding real files (skipped inside without ffmpeg)
    Check::initMlt();
    testSoundSyncOnFiles(dir.path());
    testSoundSyncCancel(dir.path());
    return Check::result();
}
