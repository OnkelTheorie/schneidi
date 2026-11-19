// SPDX-License-Identifier: AGPL-3.0-only
#include "core/MusicBed.h"

#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/TruePeak.h"

#include <QByteArray>
#include <QSaveFile>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <thread>
#include <atomic>
#include <exception>
#include <system_error>

namespace MusicBed {

namespace {

constexpr double SR = kRate;
constexpr double kPi = std::numbers::pi;

double smooth(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3 - 2 * x);
}
double midiHz(double m) { return 440.0 * std::pow(2.0, (m - 69) / 12); }
double cents(double c) { return std::pow(2.0, c / 1200); }

// ---------- Threads ----------

std::atomic<int> g_threads{0};            // 0 = automatic (setThreadsForTests)
std::atomic<bool> g_failStarts{false};    // tests: every thread start fails

int threadsFor(long long frames, long long framesPerThread, int most)
{
    const int forced = g_threads.load();
    if (forced > 0) return forced;
    const int cores = std::clamp(int(std::thread::hardware_concurrency()), 1, most);
    return int(std::clamp<long long>(frames / framesPerThread, 1, cores));
}

// Jobs 0..n-1, job 0 here and the others on threads. A thread that cannot start (no resources) leaves its job to
// this thread; a job that throws on its thread (no memory) is reset and run again here, where an error reaches the
// caller instead of ending the program
template <typename Job, typename Reset>
void parallel(int n, Job&& job, Reset&& reset)
{
    std::vector<std::thread> threads;
    threads.reserve(size_t(std::max(0, n - 1)));
    std::vector<int> state(size_t(std::max(1, n)), 0); // 1 = threw on its thread, 2 = did not start
    for (int i = 1; i < n; ++i) {
        try {
            if (g_failStarts.load()) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            threads.emplace_back([&job, &state, i] {
                try {
                    job(i);
                } catch (...) {
                    state[size_t(i)] = 1;
                }
            });
        } catch (const std::system_error&) {
            state[size_t(i)] = 2;
        }
    }
    std::exception_ptr error;
    try {
        job(0);
    } catch (...) {
        error = std::current_exception();
    }
    for (std::thread& t : threads) t.join();
    if (error) std::rethrow_exception(error);
    for (int i = 1; i < n; ++i) {
        if (state[size_t(i)] == 1) reset(i);
        if (state[size_t(i)] != 0) job(i);
    }
}

// ---------- Noise and filters ----------

// Deterministic white noise in [-1, 1) (xorshift32)
struct Noise {
    std::uint32_t s;
    explicit Noise(std::uint32_t seed) : s(seed * 2654435761u + 0x9E3779B9u) { s |= 1; }
    float next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return float(std::int32_t(s)) * (1.0f / 2147483648.0f);
    }
    double unit() { return 0.5 + 0.5 * double(next()); } // [0, 1)
};

// Biquad of the audio EQ cookbook formulas (direct form II transposed)
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float run(float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    enum Type { Low, High, Band };
    static Biquad make(Type type, double f, double q)
    {
        const double w = 2 * kPi * std::clamp(f, 10.0, 0.45 * SR) / SR, cw = std::cos(w), alpha = std::sin(w) / (2 * q);
        double b[3], a0 = 1 + alpha;
        if (type == Low) b[0] = (1 - cw) / 2, b[1] = 1 - cw, b[2] = (1 - cw) / 2;
        else if (type == High) b[0] = (1 + cw) / 2, b[1] = -(1 + cw), b[2] = (1 + cw) / 2;
        else b[0] = alpha, b[1] = 0, b[2] = -alpha; // band pass, 0 dB at the centre
        Biquad r;
        r.b0 = float(b[0] / a0);
        r.b1 = float(b[1] / a0);
        r.b2 = float(b[2] / a0);
        r.a1 = float(-2 * cw / a0);
        r.a2 = float((1 - alpha) / a0);
        return r;
    }
    // New coefficients, the state stays (sweeps)
    void retune(Type type, double f, double q)
    {
        const Biquad n = make(type, f, q);
        b0 = n.b0, b1 = n.b1, b2 = n.b2, a1 = n.a1, a2 = n.a2;
    }
};

// ---------- Wavetables ----------

constexpr int kTable = 2048;

enum class Timbre {
    PadDark, PadBright, HypeBright, Bass, BassDrive, BassHype, PluckDark, PluckBright,
    StringsDark, StringsBright, PulseDark, PulseBright, BrassDark, BrassBright, SlapDark, SlapBright, Sine
};

// Amplitude of harmonic k of a note at f Hz
double harmonic(Timbre t, int k, double f)
{
    const double saw = 1.0 / k, x = k * f;
    const double pulse = std::abs(std::sin(kPi * k * 0.22)) / k; // a narrow pulse (22 %): hollow and nasal
    const auto lp = [x](double fc) { return 1.0 / (1.0 + std::pow(x / fc, 4)); };
    const auto bump = [x](double fc) { return 1.0 / (1.0 + std::pow((x - fc) / (0.35 * fc), 2)); };
    switch (t) {
    case Timbre::PadDark: return saw * lp(650);
    case Timbre::PadBright: return saw * lp(2200);
    case Timbre::HypeBright: return saw * lp(6000);
    case Timbre::Bass: {
        static const double a[] = {1, 0.4, 0.16, 0.07, 0.03};
        return k <= 5 ? a[k - 1] : 0;
    }
    case Timbre::BassDrive: return (k == 1 ? 1.4 : 0) + saw * lp(520);
    case Timbre::BassHype: return (k == 1 ? 1.6 : 0) + saw * lp(420);
    case Timbre::PluckDark: return saw * lp(800);
    case Timbre::PluckBright: return saw * lp(4800);
    // Bowed strings: a saw shaped by a wooden body (resonances near 280 Hz, 1 kHz and 2.6 kHz, a dip between)
    case Timbre::StringsDark: return saw * lp(900) * (1 + 1.5 * bump(280));
    case Timbre::StringsBright: return saw * lp(5200) * (0.5 + 1.4 * bump(280) + 0.9 * bump(1000) + 1.5 * bump(2600));
    case Timbre::PulseDark: return pulse * lp(1300);
    case Timbre::PulseBright: return pulse * lp(6500);
    case Timbre::BrassDark: return saw * lp(450);
    case Timbre::BrassBright: return saw * lp(2600) * (1 + 0.6 * lp(1600)); // a formant-ish bump
    case Timbre::SlapDark: return (k == 1 ? 1.5 : 0) + saw * lp(380);
    case Timbre::SlapBright: return (k == 1 ? 1.0 : 0) + saw * lp(3200);
    case Timbre::Sine: return k == 1 ? 1 : 0;
    }
    return 0;
}


using Table = std::array<float, kTable + 1>; // one period, plus the first value again (interpolation)

const Table& sineTable()
{
    static const Table t = [] {
        Table s;
        for (int i = 0; i <= kTable; ++i) s[size_t(i)] = float(std::sin(2 * kPi * i / kTable));
        return s;
    }();
    return t;
}

// The tables of one bed (band-limited per note: harmonics below 0.45 x the rate), normalized to the same RMS so
// blending a dark and a bright one keeps the level
class Tables {
public:
    const Table& get(Timbre t, int midi)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        const int key = int(t) * 256 + std::clamp(midi, 0, 255);
        auto it = m_tables.find(key);
        if (it != m_tables.end()) return *it->second;
        auto table = std::make_unique<Table>();
        std::vector<double> v(kTable, 0.0);
        const double f = midiHz(midi);
        const int harmonics = std::clamp(int(0.45 * SR / f), 1, 96);
        const Table& sine = sineTable();
        double power = 0;
        for (int k = 1; k <= harmonics; ++k) {
            const double a = harmonic(t, k, f);
            if (a == 0) continue;
            power += a * a / 2;
            for (int i = 0; i < kTable; ++i) v[size_t(i)] += a * sine[size_t((k * i) & (kTable - 1))];
        }
        const double norm = power > 0 ? 0.5 / std::sqrt(power) : 0;
        for (int i = 0; i < kTable; ++i) (*table)[size_t(i)] = float(v[size_t(i)] * norm);
        (*table)[kTable] = (*table)[0];
        return *(m_tables[key] = std::move(table));
    }

private:
    std::map<int, std::unique_ptr<Table>> m_tables;
    std::mutex m_mutex;
};

inline float lookup(const Table& t, double phase) // phase in [0, 1)
{
    const double p = phase * kTable;
    const int i = int(p);
    const float f = float(p - i);
    return t[size_t(i)] + f * (t[size_t(i) + 1] - t[size_t(i)]);
}

// Two tables at the same phase, blended: a + b * (b - a)
inline float lookup2(const Table& a, const Table& b, double phase, float mix)
{
    const double p = phase * kTable;
    const int i = int(p);
    const float f = float(p - i);
    const float va = a[size_t(i)] + f * (a[size_t(i) + 1] - a[size_t(i)]);
    const float vb = b[size_t(i)] + f * (b[size_t(i) + 1] - b[size_t(i)]);
    return va + mix * (vb - va);
}

// An oscillator over a table, the phase running on
struct Osc {
    double phase = 0, inc = 0;
    float next(const Table& t)
    {
        const float v = lookup(t, phase);
        phase += inc;
        if (phase >= 1) phase -= 1;
        return v;
    }
};

// ---------- Harmony ----------

enum class Q { Maj, Min, Maj7, Min7, Dom7, Add9, MinAdd9, Sus2, Sus4, Maj9, Min9, Dom9, Dim7 };

struct Chord {
    int offset = 0; // semitones above the tonic
    Q q = Q::Maj;
    bool minor() const { return q == Q::Min || q == Q::Min7 || q == Q::MinAdd9 || q == Q::Min9 || q == Q::Dim7; }
};
using Progression = std::vector<Chord>;

// Intervals above the root of a chord's four voices
std::array<int, 4> intervals(Q q)
{
    switch (q) {
    case Q::Maj: return {0, 4, 7, 12};
    case Q::Min: return {0, 3, 7, 12};
    case Q::Maj7: return {0, 4, 7, 11};
    case Q::Min7: return {0, 3, 7, 10};
    case Q::Dom7: return {0, 4, 7, 10};
    case Q::Add9: return {0, 4, 7, 14};
    case Q::MinAdd9: return {0, 3, 7, 14};
    case Q::Sus2: return {0, 2, 7, 12};
    case Q::Sus4: return {0, 5, 7, 12};
    case Q::Maj9: return {4, 7, 11, 14}; // rootless (the bass has the root)
    case Q::Min9: return {3, 7, 10, 14};
    case Q::Dom9: return {4, 10, 14, 7};
    case Q::Dim7: return {0, 3, 6, 9};
    }
    return {0, 4, 7, 12};
}

const char* suffix(Q q)
{
    switch (q) {
    case Q::Maj: return "";
    case Q::Min: return "m";
    case Q::Maj7: return "maj7";
    case Q::Min7: return "m7";
    case Q::Dom7: return "7";
    case Q::Add9: return "add9";
    case Q::MinAdd9: return "m(add9)";
    case Q::Sus2: return "sus2";
    case Q::Sus4: return "sus4";
    case Q::Maj9: return "maj9";
    case Q::Min9: return "m9";
    case Q::Dom9: return "9";
    case Q::Dim7: return "dim7";
    }
    return "";
}

// Per mood and mode, the first one is the mood's own (seed 1). Each chord lasts Style::chordBeats
const std::vector<Progression>& progressions(Mood m, bool minor)
{
    using enum Q;
    static const std::map<std::pair<Mood, bool>, std::vector<Progression>> all{
        // calm: soft colours (maj7, add9), two bars a chord
        {{Mood::Calm, false}, {{{0, Maj7}, {9, Min7}, {5, Maj7}, {7, Sus4}},
                               {{5, Maj7}, {0, Add9}, {9, Min7}, {7, Add9}},
                               {{0, Add9}, {4, Min7}, {5, Maj7}, {5, Min}}}},
        {{Mood::Calm, true}, {{{0, MinAdd9}, {8, Maj7}, {3, Maj7}, {10, Add9}},
                              {{0, Min7}, {5, Min7}, {8, Maj7}, {7, Sus4}},
                              {{8, Maj7}, {3, Add9}, {10, Sus4}, {0, MinAdd9}}}},
        // hype: minor EDM loop, a bar a chord
        {{Mood::Hype, true}, {{{0, Min}, {8, Maj}, {10, Maj}, {7, Min}},
                              {{0, Min}, {8, Maj}, {3, Maj}, {10, Maj}},
                              {{8, Maj}, {10, Maj}, {0, Min}, {0, Min}}}},
        {{Mood::Hype, false}, {{{9, Min}, {5, Maj}, {0, Maj}, {7, Maj}},
                               {{0, Maj}, {7, Maj}, {9, Min}, {5, Maj}},
                               {{5, Maj}, {7, Maj}, {9, Min}, {0, Maj}}}},
        // lofi: jazz ninths, ii-V-I, a bar a chord
        {{Mood::Lofi, false}, {{{2, Min9}, {7, Dom9}, {0, Maj9}, {9, Min9}},
                               {{0, Maj9}, {9, Min9}, {2, Min9}, {7, Dom9}},
                               {{5, Maj9}, {4, Min9}, {2, Min9}, {0, Maj9}}}},
        {{Mood::Lofi, true}, {{{0, Min9}, {5, Min9}, {10, Dom9}, {3, Maj9}},
                              {{8, Maj9}, {7, Dom9}, {0, Min9}, {0, Min9}},
                              {{0, Min9}, {3, Maj9}, {5, Min9}, {7, Dom9}}}},
        // epic: heroic minor triads (i VI III VII, or a major V), two bars a chord
        {{Mood::Epic, true}, {{{0, Min}, {8, Maj}, {3, Maj}, {10, Maj}},
                              {{0, Min}, {8, Maj}, {5, Min}, {7, Maj}},
                              {{8, Maj}, {10, Maj}, {0, Min}, {3, Maj}}}},
        {{Mood::Epic, false}, {{{0, Maj}, {7, Maj}, {9, Min}, {5, Maj}},
                               {{5, Maj}, {0, Maj}, {7, Sus4}, {7, Maj}},
                               {{0, Maj}, {4, Min}, {5, Maj}, {7, Maj}}}},
        // funk: a dorian vamp of sevenths and ninths, two bars a chord
        {{Mood::Funk, true}, {{{0, Min7}, {5, Dom9}},
                              {{0, Min9}, {5, Dom9}, {3, Maj7}, {2, Min7}},
                              {{0, Min7}, {10, Dom7}}}},
        {{Mood::Funk, false}, {{{0, Dom9}, {5, Dom9}},
                               {{0, Dom9}, {0, Dom9}, {5, Dom9}, {7, Dom9}},
                               {{2, Min7}, {7, Dom9}}}},
    };
    const auto it = all.find({m, minor});
    return it != all.end() ? it->second : all.at({Mood::Hype, minor});
}

const char* const kSharps[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kFlats[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

bool flatKey(const Key& k)
{
    const int relMajor = k.minor ? (k.root + 3) % 12 : k.root;
    return relMajor == 5 || relMajor == 10 || relMajor == 3 || relMajor == 8 || relMajor == 1;
}

QString noteName(int pc, bool flats) { return QString::fromLatin1((flats ? kFlats : kSharps)[((pc % 12) + 12) % 12]); }

// Pitch classes of a chord's four voices
std::array<int, 4> chordTones(const Key& key, const Chord& c)
{
    const std::array<int, 4> iv = intervals(c.q);
    std::array<int, 4> pcs{};
    for (int i = 0; i < 4; ++i) pcs[size_t(i)] = (key.root + c.offset + iv[size_t(i)]) % 12;
    return pcs;
}

// The chord in the range low..low+24, with the least movement from the previous voicing
std::array<int, 4> voice(const std::array<int, 4>& pcs, const std::array<int, 4>& previous, int low)
{
    std::array<int, 4> best{};
    int bestCost = 1 << 30;
    for (int rot = 0; rot < 4; ++rot) {
        for (int base = low; base < low + 12; ++base) {
            if ((base - pcs[size_t(rot)]) % 12 != 0) continue;
            std::array<int, 4> v{};
            int note = base;
            for (int i = 0; i < 4; ++i) {
                const int pc = pcs[size_t((rot + i) % 4)];
                while ((note - pc) % 12 != 0) ++note;
                v[size_t(i)] = note;
                ++note;
            }
            if (v[3] > low + 24) continue;
            int cost = 0;
            for (int i = 0; i < 4; ++i) cost += std::abs(v[size_t(i)] - previous[size_t(i)]);
            if (cost < bestCost) bestCost = cost, best = v;
        }
    }
    return best;
}

int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// ---------- Style: what makes a mood ----------

struct Style {
    double bpm = 112;
    Key key;
    int chordBeats = 4;     // how long a chord lasts
    double swing = 0;       // the off-beat eighths (swing16: sixteenths) this much of their length late
    bool swing16 = false;
    double minTail = 2.5;   // seconds after the final hit at least
    // Pad (gain 0: none)
    double padGain = 0;
    Timbre padDark = Timbre::PadDark, padBright = Timbre::PadBright;
    double padAttack = 0.25, padRelease = 0.6, padDetune = 8, padSend = 0.3;
    int padLow = 52;        // the voicing's range: padLow .. padLow + 24
    bool padRoot = true;    // the chord's root under the voicing
    double padVibrato = 0;  // cents (an ensemble)
    bool padWide = false;   // five voices (a supersaw)
    // Brightness of the pads and plucks per part (0..1) and how fast it follows a new part (bars)
    double bIntroFrom = 0.3, bIntroTo = 0.6, bMain = 0.65, bLift = 0.8, bBreak = 0.3, bEnd = 0.55, bRamp = 0.3;
    // Space: reverb level, feedback (length), damping (darkness), size (delay lengths) and pre-delay (s)
    double wet = 0.3, feedback = 0.8, damp = 0.3, size = 1.0, preDelay = 0.018;
    // Side chain on the beat in main parts (0: none)
    double duck = 0, duckRelease = 0.07;
    // Master: low pass (0: none), soft saturation (0: none)
    double masterLowpass = 0, masterDrive = 0;
};

Style styleOf(Mood m)
{
    Style s;
    switch (m) {
    case Mood::Calm:
        s.bpm = 72, s.key = {5, false}, s.chordBeats = 8, s.minTail = 3.5;
        s.padGain = 0.035, s.padAttack = 1.2, s.padRelease = 1.6, s.padDetune = 6, s.padSend = 0.6, s.padLow = 52,
        s.padRoot = false;
        s.bIntroFrom = 0.25, s.bIntroTo = 0.35, s.bMain = 0.35, s.bLift = 0.45, s.bBreak = 0.25, s.bEnd = 0.35, s.bRamp = 1.0;
        s.wet = 0.5, s.feedback = 0.86, s.damp = 0.45, s.size = 1.35, s.preDelay = 0.03;
        break;
    case Mood::Hype:
        s.bpm = 128, s.key = {4, true}, s.chordBeats = 4;
        s.padGain = 0.1, s.padDark = Timbre::PadDark, s.padBright = Timbre::HypeBright, s.padAttack = 0.04,
        s.padRelease = 0.3, s.padDetune = 18, s.padSend = 0.35, s.padRoot = false, s.padWide = true;
        s.bIntroFrom = 0.05, s.bIntroTo = 0.45, s.bMain = 1.0, s.bLift = 1.0, s.bBreak = 0.12, s.bEnd = 0.8, s.bRamp = 0.1;
        s.wet = 0.32, s.feedback = 0.8, s.damp = 0.3, s.size = 1.0;
        s.duck = 0.55, s.duckRelease = 0.09;
        break;
    case Mood::Lofi:
        s.bpm = 84, s.key = {3, false}, s.chordBeats = 4, s.swing = 0.16, s.swing16 = true, s.minTail = 3.0;
        s.padGain = 0;
        s.bIntroFrom = 0.4, s.bIntroTo = 0.5, s.bMain = 0.5, s.bLift = 0.6, s.bBreak = 0.4, s.bEnd = 0.5, s.bRamp = 1.0;
        s.wet = 0.18, s.feedback = 0.7, s.damp = 0.55, s.size = 0.7, s.preDelay = 0.008;
        s.duck = 0.12, s.duckRelease = 0.12, s.masterLowpass = 6000, s.masterDrive = 0.35;
        break;
    case Mood::Epic:
        s.bpm = 90, s.key = {2, true}, s.chordBeats = 8, s.minTail = 4.0;
        s.padGain = 0.1, s.padDark = Timbre::StringsDark, s.padBright = Timbre::StringsBright, s.padAttack = 0.5,
        s.padRelease = 1.2, s.padDetune = 7, s.padSend = 0.6, s.padLow = 50, s.padVibrato = 14;
        s.bIntroFrom = 0.2, s.bIntroTo = 0.7, s.bMain = 0.9, s.bLift = 1.0, s.bBreak = 0.35, s.bEnd = 0.85, s.bRamp = 1.0;
        s.wet = 0.5, s.feedback = 0.9, s.damp = 0.3, s.size = 1.6, s.preDelay = 0.04;
        break;
    case Mood::Funk:
        s.bpm = 104, s.key = {4, true}, s.chordBeats = 8, s.swing = 0.1, s.swing16 = true, s.minTail = 2.0;
        s.padGain = 0;
        s.bIntroFrom = 0.6, s.bIntroTo = 0.7, s.bMain = 0.75, s.bLift = 0.9, s.bBreak = 0.5, s.bEnd = 0.7, s.bRamp = 0.2;
        s.wet = 0.1, s.feedback = 0.66, s.damp = 0.3, s.size = 0.6, s.preDelay = 0.005;
        break;
    }
    return s;
}

// ---------- Plan: tempo, sections, chords, the tune ----------

struct LeadNote {
    double beat = 0, len = 0; // beats from the bed's start
    int midi = 60;
    double vel = 1;
};

struct Plan {
    Mood mood = Mood::Hype;
    Style style;
    double bpm = 0, beat = 0, bar = 0, length = 0;
    long long frames = 0;
    Key key;
    unsigned seed = 1;
    int variant = 0;
    Progression prog;
    Chord tonic;      // the final chord
    int bars = 0;     // bars that start before the end
    int intro = 0;    // bars
    int endBar = -1;  // the final hit
    std::vector<Section> sections;
    std::vector<Section::Kind> kind; // per bar (bars + 1 entries)
    std::vector<bool> lift, fill;    // per bar: the second eight bars of a main part; the bar before a new part
    // Per beat ((bars + 1) * 4 entries): the chord, its voicing, the index of the chord span it belongs to
    std::vector<Chord> chord;
    std::vector<std::array<int, 4>> voicing;
    struct Span {
        int from = 0, to = 0; // beats
        Section::Kind kind = Section::Main;
    };
    std::vector<Span> spans;   // one chord in one part
    std::vector<LeadNote> lead; // the tune (moods with one)

    Section::Kind kindAt(int b) const { return b >= 0 && b < int(kind.size()) ? kind[size_t(b)] : Section::Ending; }
    double barTime(int b) const { return b * bar; }
    double beatTime(double k) const { return k * beat; }
    int last() const { return endBar > 0 ? endBar : bars; }
    const Chord& chordAt(int beatIndex) const { return chord[size_t(std::clamp(beatIndex, 0, int(chord.size()) - 1))]; }
    const std::array<int, 4>& voicingAt(int beatIndex) const
    {
        return voicing[size_t(std::clamp(beatIndex, 0, int(voicing.size()) - 1))];
    }
    // A position in beats within the bar, swung: the off-beat eighths (sixteenths) later
    double swung(double pos) const
    {
        if (style.swing <= 0) return pos;
        const double unit = style.swing16 ? 0.25 : 0.5;
        const double q = pos / unit;
        const long long i = std::llround(q);
        if (std::abs(q - double(i)) < 1e-6 && (i % 2) != 0) return pos + style.swing * unit;
        return pos;
    }
    double at(int b, double pos) const { return barTime(b) + swung(pos) * beat; }
};

int pcIn(int pc, int low) { return low + ((pc - low) % 12 + 12) % 12; } // the pitch class pc in low..low+11

// The tune of moods with one (epic: the horns): a four-bar phrase from the seed (rhythm from the mood's patterns,
// chord tones on strong beats, steps of the scale between), repeated, another one in lift bars; the phrase's last
// note lands on the chord's root
void makeLead(Plan& p)
{
    struct Hit {
        double at, len;
    };
    using Rhythm = std::vector<Hit>;
    std::vector<Rhythm> rhythms, slow; // slow: main bars before the lift (epic)
    Rhythm closing;
    int lo = 60, hi = 79;
    switch (p.mood) {
    case Mood::Epic:
        rhythms = {{{0, 2}, {2, 2}}, {{0, 3}, {3, 1}}, {{0, 1}, {1, 1}, {2, 2}}, {{0, 1.5}, {1.5, 0.5}, {2, 2}}};
        slow = {{{0, 4}}, {{0, 2}, {2, 2}}, {{0, 3}, {3, 1}}};
        closing = {{0, 4}};
        lo = 55, hi = 72;
        break;
    default: return;
    }
    const bool minorScale = p.key.minor;
    static const int major[] = {0, 2, 4, 5, 7, 9, 11}, minor[] = {0, 2, 3, 5, 7, 8, 10};
    const int* scale = minorScale ? minor : major;
    const auto inScale = [&](int m) {
        const int d = ((m - p.key.root) % 12 + 12) % 12;
        for (int i = 0; i < 7; ++i)
            if (scale[i] == d) return true;
        return false;
    };
    const auto nearest = [&](int target, const auto& ok) {
        int best = std::clamp(target, lo, hi);
        for (int d = 0; d <= 12; ++d) {
            if (target - d >= lo && target - d <= hi && ok(target - d)) return target - d;
            if (target + d >= lo && target + d <= hi && ok(target + d)) return target + d;
        }
        return best;
    };
    for (int b = 0; b < p.last(); ++b) {
        if (p.kindAt(b) != Section::Main) continue;
        const int inPhrase = ((b - p.intro) % 4 + 4) % 4;
        const int ab = p.lift[size_t(b)] ? 1 : 0;
        // The same bar of the phrase and the same chord give the same notes: the phrase repeats
        const int chordIndex = int(p.chord[size_t(b * 4)].offset * 16 + int(p.chord[size_t(b * 4)].q));
        Noise rng(p.seed * 7907u + unsigned(inPhrase) * 131u + unsigned(ab) * 17u + unsigned(chordIndex) * 3u + 5u);
        Noise rhythmRng(p.seed * 4231u + unsigned(inPhrase) * 61u + unsigned(ab) * 11u + 1u);
        const std::vector<Rhythm>& set = !slow.empty() && !p.lift[size_t(b)] ? slow : rhythms;
        const Rhythm& r = inPhrase == 3 ? closing : set[size_t(rhythmRng.unit() * double(set.size())) % set.size()];
        int prev = (lo + hi) / 2 + int(rng.unit() * 5) - 2;
        for (size_t i = 0; i < r.size(); ++i) {
            const int beatIndex = b * 4 + int(r[i].at);
            const std::array<int, 4> pcs = chordTones(p.key, p.chordAt(beatIndex));
            const auto isChordTone = [&](int m) {
                for (int pc : pcs)
                    if (((m - pc) % 12 + 12) % 12 == 0) return true;
                return false;
            };
            const bool strong = std::abs(r[i].at - std::round(r[i].at)) < 1e-6 && int(std::round(r[i].at)) % 2 == 0;
            const bool lastNote = inPhrase == 3 && i + 1 == r.size();
            int m;
            if (lastNote) {
                const int root = (p.key.root + p.chordAt(beatIndex).offset) % 12;
                m = nearest(prev, [&](int x) { return ((x - root) % 12 + 12) % 12 == 0; });
            } else if (strong) {
                const int jump = int(rng.unit() * 7) - 3;
                m = nearest(prev + jump, isChordTone);
            } else {
                const int steps = rng.unit() < 0.5 ? -1 : 1;
                m = prev + steps;
                while (!inScale(m)) m += steps;
                if (m < lo || m > hi) {
                    m = prev - steps;
                    while (!inScale(m)) m -= steps;
                }
            }
            prev = m;
            const double vel = (strong ? 1.0 : 0.82) * (0.92 + 0.08 * rng.unit());
            p.lead.push_back({b * 4 + r[i].at, r[i].len, m, vel});
        }
    }
}

Plan makePlan(const Options& o)
{
    Plan p;
    p.mood = o.mood;
    p.style = styleOf(o.mood);
    p.bpm = o.bpm > 0 ? std::clamp(o.bpm, kMinBpm, kMaxBpm) : p.style.bpm;
    p.beat = 60.0 / p.bpm;
    p.bar = 4 * p.beat;
    p.frames = frameCount(std::clamp(o.length, kMinLength - kLengthSlack, kMaxLength + kLengthSlack));
    p.length = double(p.frames) / SR;
    p.key = o.key ? *o.key : p.style.key;
    p.seed = std::max(1u, o.seed);
    const std::vector<Progression>& list = progressions(p.mood, p.key.minor);
    p.variant = int((p.seed - 1) % list.size());
    p.prog = list[size_t(p.variant)];
    // The final chord: the progression's own tonic chord when it has one, else the plain triad
    p.tonic = Chord{0, p.key.minor ? Q::Min : Q::Maj};
    for (const Chord& c : p.prog)
        if (c.offset == 0) {
            p.tonic = c;
            break;
        }
    p.bars = int(std::ceil(p.length / p.bar - 1e-9));

    const int h = int(std::floor((p.length - p.style.minTail) / p.bar));
    p.endBar = h >= 2 ? h : -1;
    const int body = p.endBar > 0 ? p.endBar : p.bars;
    // An intro of four bars in long beds, two in shorter ones (at most about a sixth of the bed: a 30 s bed starts
    // its groove within a few seconds)
    p.intro = p.endBar < 0 ? 0 : body >= 12 && 4 * p.bar <= 0.18 * p.length ? 4 : body >= 6 ? 2 : 0;
    if (p.intro > 0) p.sections.push_back({Section::Intro, 0, p.intro});
    // Main parts of 16 bars with a break of 4 between them, as long as a main part of 8 still follows the break
    constexpr int kMain = 16, kBreak = 4;
    int cur = p.intro;
    while (cur < body) {
        if (body - cur >= kMain + kBreak + 8) {
            p.sections.push_back({Section::Main, cur, cur + kMain});
            p.sections.push_back({Section::Break, cur + kMain, cur + kMain + kBreak});
            cur += kMain + kBreak;
        } else {
            p.sections.push_back({Section::Main, cur, body});
            cur = body;
        }
    }
    if (p.endBar > 0) p.sections.push_back({Section::Ending, p.endBar, p.bars});

    p.kind.assign(size_t(p.bars + 1), Section::Main);
    p.lift.assign(size_t(p.bars + 1), false);
    p.fill.assign(size_t(p.bars + 1), false);
    for (const Section& s : p.sections)
        for (int b = s.fromBar; b < s.toBar && b <= p.bars; ++b) {
            p.kind[size_t(b)] = s.kind;
            if (s.kind == Section::Main) p.lift[size_t(b)] = ((b - s.fromBar) / 8) % 2 == 1;
        }
    if (p.endBar > 0)
        for (int b = p.endBar; b <= p.bars; ++b) p.kind[size_t(b)] = Section::Ending;
    for (size_t i = 1; i < p.sections.size(); ++i)
        if (p.sections[i].fromBar > 0) p.fill[size_t(p.sections[i].fromBar - 1)] = true;

    // Chords per beat: the progression from the first main bar on (an intro plays the end of it), the tonic chord from
    // the final hit on; a new voicing where the chord changes
    const int beats = (p.bars + 1) * 4, n = int(p.prog.size()), cb = p.style.chordBeats;
    p.chord.resize(size_t(beats));
    p.voicing.resize(size_t(beats));
    std::array<int, 4> previous{p.style.padLow + 5, p.style.padLow + 8, p.style.padLow + 12, p.style.padLow + 15};
    int lastIndex = -1 << 20;
    for (int k = 0; k < beats; ++k) {
        const bool ending = p.kindAt(k / 4) == Section::Ending;
        const int slot = floorDiv(k - p.intro * 4, cb);
        const int index = ending ? -1000000 : slot;
        p.chord[size_t(k)] = ending ? p.tonic : p.prog[size_t(((slot % n) + n) % n)];
        if (index != lastIndex) previous = voice(chordTones(p.key, p.chord[size_t(k)]), previous, p.style.padLow);
        p.voicing[size_t(k)] = previous;
        const Section::Kind kind = p.kindAt(k / 4);
        if (index != lastIndex || p.spans.empty() || p.spans.back().kind != kind) p.spans.push_back({k, k + 1, kind});
        else p.spans.back().to = k + 1;
        lastIndex = index;
    }
    makeLead(p);
    return p;
}

struct Env {
    double attack = 0.005; // s, raised curve from 0
    double decay = 0;      // s, time constant towards sustain (0 = none)
    double sustain = 1;
    double release = 0.05; // s after the gate, down to 0
};

// A slow control signal (brightness, side chain) sampled every kStep frames and interpolated: computed once per
// bed instead of per voice and sample
struct Control {
    static constexpr int kShift = 5, kStep = 1 << kShift;
    std::vector<float> v;
    template <typename F>
    void build(long long frames, F&& f)
    {
        v.resize(size_t(frames / kStep + 2));
        for (size_t i = 0; i < v.size(); ++i) v[i] = float(f((long long)i * kStep));
    }
    float at(long long frame) const
    {
        const size_t i = size_t(frame >> kShift);
        const float x = float(frame & (kStep - 1)) * (1.0f / kStep);
        return v[i] + x * (v[i + 1] - v[i]);
    }
};

// Gain of the side chain: the sound dips on every beat of bars with a four-on-the-floor kick and comes back
struct Duck {
    double depth = 0;
    double beatFrames = 1;
    std::vector<bool> bars; // bars where it pumps
    std::array<float, 513> curve{};
    void setup(double depthIn, double beatS, double release)
    {
        depth = depthIn;
        beatFrames = beatS * SR;
        for (int i = 0; i <= 512; ++i) {
            const double x = i / 512.0 * beatS;
            const double down = smooth(x / 0.006) * std::exp(-x / release) * (1 - smooth((x - 0.8 * beatS) / (0.2 * beatS)));
            curve[size_t(i)] = float(1 - depthIn * down);
        }
    }
    float at(long long frame) const
    {
        if (depth <= 0) return 1;
        const double b = frame / beatFrames;
        const long long beat = (long long)b;
        const size_t bar = size_t(beat / 4);
        if (bar >= bars.size() || !bars[bar]) return 1;
        return curve[size_t((b - double(beat)) * 512)];
    }
};

// Where the voices write: the whole bed's buffers, of which this thread writes frames [lo, hi). Every thread computes
// each note from its start (noise, filters and phases run the same), so the sum is bit for bit the one of a single
// thread, whatever the windows are
struct Bus {
    float* out = nullptr;  // interleaved stereo
    float* send = nullptr; // mono, into the reverb
    long long frames = 0;
    long long lo = 0, hi = 0;
};

// One note into the bus: voice(i, frame) gives the dry (left, right) sample i of the note; gain/pan/send and the
// envelope come on top (pan -1..1, linear balance)
template <typename Voice>
void note(Bus& bus, double t0, double gate, const Env& e, double gain, double pan, double send, const Control* duck,
          Voice&& voiceFn)
{
    const long long s0 = std::llround(t0 * SR);
    const long long nGate = std::max<long long>(1, std::llround(gate * SR));
    const long long nRel = std::max<long long>(1, std::llround(e.release * SR));
    const long long nAtt = std::min(nGate, std::max<long long>(1, std::llround(e.attack * SR)));
    const long long total = nGate + nRel;
    if (s0 + total <= bus.lo || s0 >= bus.hi) return; // not in this thread's window
    const long long end = std::min(total, bus.hi - s0);
    const float k = e.decay > 0 ? float(std::exp(-1.0 / (e.decay * SR))) : 1.0f;
    const float sustain = float(e.sustain), rise = 1.0f - sustain;
    const float gl = float(gain * (pan > 0 ? 1 - pan : 1.0)), gr = float(gain * (pan < 0 ? 1 + pan : 1.0));
    const float gs = float(gain * send * 0.5);
    float dec = 1, atGate = 0;
    const float invAtt = 1.0f / float(nAtt), invRel = 1.0f / float(nRel);
    const auto put = [&](long long i, float v) {
        const long long frame = s0 + i;
        const auto [l, r] = voiceFn(i, frame);
        if (frame < bus.lo) return; // computed for the state, written by another thread
        if (duck) v *= duck->at(frame);
        bus.out[2 * frame] += l * v * gl;
        bus.out[2 * frame + 1] += r * v * gr;
        if (gs != 0) bus.send[frame] += (l + r) * v * gs;
    };
    long long i = 0;
    for (; i < std::min(nAtt, end); ++i) { // attack: a smooth rise
        const float x = float(i) * invAtt;
        atGate = x * x * (3 - 2 * x) * (sustain + rise * dec);
        dec *= k;
        put(i, atGate);
    }
    for (; i < std::min(nGate, end); ++i) { // decay towards the sustain level
        atGate = sustain + rise * dec;
        dec *= k;
        put(i, atGate);
    }
    for (; i < end; ++i) { // release: a smooth fall to 0
        const float x = float(i - nGate) * invRel;
        put(i, atGate * (1 - x * x * (3 - 2 * x)));
    }
}

// Brightness 0..1 of the pads and plucks over time: a target per bar, reached by a smooth ramp (quick at a new part,
// over the whole bar where it builds up)
struct Brightness {
    std::vector<double> target, ramp; // per bar
    double bar = 1;
    double at(double t) const
    {
        const double x = t / bar;
        const int b = std::clamp(int(x), 0, int(target.size()) - 1);
        const double from = b > 0 ? target[size_t(b - 1)] : target[0];
        return from + (target[size_t(b)] - from) * smooth((x - b) / ramp[size_t(b)]);
    }
};

Brightness brightnessOf(const Plan& p)
{
    const Style& s = p.style;
    Brightness br;
    br.bar = p.bar;
    br.target.assign(size_t(p.bars + 1), 0.5);
    br.ramp.assign(size_t(p.bars + 1), 0.2);
    const bool opens = p.mood == Mood::Hype || p.mood == Mood::Epic;
    for (int b = 0; b <= p.bars; ++b) {
        const Section::Kind k = p.kindAt(b);
        double& t = br.target[size_t(b)];
        double& r = br.ramp[size_t(b)];
        if (k == Section::Intro) t = s.bIntroFrom + (s.bIntroTo - s.bIntroFrom) * (b + 1) / std::max(1, p.intro), r = 1.0;
        else if (k == Section::Break) t = s.bBreak, r = s.bRamp;
        else if (k == Section::Ending) t = s.bEnd, r = s.bRamp;
        else t = p.lift[size_t(b)] ? s.bLift : s.bMain, r = s.bRamp;
        if (p.mood == Mood::Calm) t += 0.1 * ((b / 4) % 2); // breathing every four bars
        if (opens && p.fill[size_t(b)] && k != Section::Main) t = 0.9, r = 1.0; // the riser bar opens the filter
    }
    return br;
}

// What the voices of one thread share (the tables and controls are the same for all threads)
struct Context {
    const Plan& p;
    const Style& s;
    Bus bus;
    Tables& tables;
    const Control& bright; // 0..1 (Brightness)
    const Control& duck;   // side-chain gain (Duck)
    bool ducking = false;
    Noise noise;
    Context(const Plan& plan, const Bus& b, Tables& t, const Control& br, const Control& d, bool ducks)
        : p(plan), s(plan.style), bus(b), tables(t), bright(br), duck(d), ducking(ducks), noise(plan.seed * 7919u + 17u)
    {
    }
    const Control* duckOrNull() const { return ducking ? &duck : nullptr; }
};

inline double frac(double x) { return x - std::floor(x); }
// Fast soft clip (tanh-like, exact 1 at the limit)
inline float soft(float x)
{
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27 + x * x) / (27 + 9 * x * x);
}

// ---------- Voices ----------

// Pad: per note a centre voice in both channels and a detuned one on each side (the width; the centre keeps the
// mono sum steady), wide: two more closer in (a supersaw); a dark and a bright table blended by the brightness;
// vib: a vibrato of that many cents, each voice its own rate (an ensemble)
void pad(Context& c, double t0, double gate, const std::vector<int>& notes, const Env& e, double gain, int rootMidi,
         Timbre darkT, Timbre brightT, double detune, double send, double vib = 0, bool wide = false)
{
    const Control* duck = c.duckOrNull();
    const Table& sine = sineTable();
    for (const int midi : notes) {
        const Table& dark = c.tables.get(darkT, midi);
        const Table& bright = c.tables.get(brightT, midi);
        const double f = midiHz(midi);
        Osc o[5] = {{c.noise.unit(), f / SR},
                    {c.noise.unit(), f * cents(-detune) / SR},
                    {c.noise.unit(), f * cents(detune) / SR},
                    {c.noise.unit(), f * cents(-0.4 * detune) / SR},
                    {c.noise.unit(), f * cents(0.43 * detune) / SR}};
        Osc lfo[5];
        for (Osc& l : lfo) l = Osc{c.noise.unit(), (4.6 + 1.2 * c.noise.unit()) / SR};
        const double depth = vib * 0.000578;
        const int count = wide ? 5 : 3;
        const Control& br = c.bright;
        note(c.bus, t0, gate, e, gain, 0, send, duck, [&](long long, long long frame) {
            const float b = br.at(frame);
            float v[5] = {0, 0, 0, 0, 0};
            for (int i = 0; i < count; ++i) {
                v[i] = lookup2(dark, bright, o[i].phase, b);
                double inc = o[i].inc;
                if (depth > 0) {
                    inc *= 1 + depth * lookup(sine, lfo[i].phase);
                    lfo[i].phase += lfo[i].inc;
                    if (lfo[i].phase >= 1) lfo[i].phase -= 1;
                }
                o[i].phase += inc;
                if (o[i].phase >= 1) o[i].phase -= 1;
            }
            const float vm = 0.72f * v[0];
            float vl = vm + 0.48f * v[1], vr = vm + 0.48f * v[2];
            if (wide) vl += 0.3f * v[3] + 0.12f * v[4], vr += 0.3f * v[4] + 0.12f * v[3];
            return std::pair<float, float>{vl, vr};
        });
    }
    if (rootMidi > 0) { // the chord's root below the voicing, warm and centred
        const Table& dark = c.tables.get(Timbre::PadDark, rootMidi);
        Osc o{c.noise.unit(), midiHz(rootMidi) / SR};
        note(c.bus, t0, gate, e, gain * 0.5, 0, send * 0.5, duck, [&](long long, long long) {
            const float v = o.next(dark);
            return std::pair<float, float>{v, v};
        });
    }
}

// A tone of one or two oscillators over two tables: its brightness moves from bStart to bEnd (time constant bTau;
// a pluck falls, a brass swell rises), optionally scaled by the bed's brightness; a vibrato that sets in after
// vibDelay; with detune two voices, one per side
struct ToneP {
    Timbre dark = Timbre::PluckDark, bright = Timbre::PluckBright;
    double bStart = 1, bEnd = 0, bTau = 0.1;
    bool follow = false;
    double detune = 0;
    double vibRate = 0, vibCents = 0, vibDelay = 0.15;
};

void tone(Context& c, double t0, double gate, int midi, const Env& e, double gain, double pan, double send, const ToneP& tp,
          bool ducked = false)
{
    const Table& dark = c.tables.get(tp.dark, midi);
    const Table& bright = c.tables.get(tp.bright, midi);
    const Table& sine = sineTable();
    const double f = midiHz(midi);
    const bool pair = tp.detune > 0;
    Osc a{0, f * (pair ? cents(-tp.detune) : 1.0) / SR}, b{0.37, f * cents(tp.detune) / SR};
    const double k = std::exp(-1.0 / (std::max(0.001, tp.bTau) * SR));
    double benv = tp.bStart - tp.bEnd, vibPhase = 0;
    const double vibInc = tp.vibRate / SR, vibDepth = tp.vibCents * 0.000578;
    const long long vibStart = std::llround(tp.vibDelay * SR), vibRamp = std::llround(0.25 * SR);
    const Control& br = c.bright;
    note(c.bus, t0, gate, e, gain, pan, send, ducked ? c.duckOrNull() : nullptr, [&](long long i, long long frame) {
        float bv = float(tp.bEnd + benv);
        if (tp.follow) bv *= 0.15f + 0.85f * br.at(frame);
        bv = std::clamp(bv, 0.0f, 1.0f);
        benv *= k;
        double w = 1;
        if (vibDepth > 0 && i > vibStart) {
            w = 1 + vibDepth * smooth(double(i - vibStart) / double(vibRamp)) * lookup(sine, vibPhase);
            vibPhase += vibInc;
            if (vibPhase >= 1) vibPhase -= 1;
        }
        const float va = lookup2(dark, bright, a.phase, bv);
        a.phase += a.inc * w;
        if (a.phase >= 1) a.phase -= 1;
        if (!pair) return std::pair<float, float>{va, va};
        const float vb = lookup2(dark, bright, b.phase, bv);
        b.phase += b.inc * w;
        if (b.phase >= 1) b.phase -= 1;
        return std::pair<float, float>{0.75f * va + 0.25f * vb, 0.75f * vb + 0.25f * va};
    });
}

// FM (phase modulation of one sine by another): the electric piano of lofi (ratio 1, a bright attack that fades).
// Optional tremolo (a slow auto-pan) and wow (a slow pitch drift, by the bed's
// clock so all notes drift together, like a worn tape)
struct FmP {
    double ratio = 1, index = 1.5, indexEnd = 0.2, indexTau = 0.2;
    double tremolo = 0, tremRate = 4;
    double wow = 0; // cents
};

void fm(Context& c, double t0, double gate, int midi, const Env& e, double gain, double pan, double send, const FmP& f)
{
    const Table& sine = sineTable();
    const double hz = midiHz(midi), ic = hz / SR, im = hz * f.ratio / SR;
    double pc = 0, pm = 0, idx = f.index - f.indexEnd;
    const double k = std::exp(-1.0 / (f.indexTau * SR)), inv2pi = 1.0 / (2 * kPi);
    note(c.bus, t0, gate, e, gain, pan, send, nullptr, [&](long long, long long frame) {
        double w = 1;
        if (f.wow > 0) w = 1 + f.wow * 0.000578 * lookup(sine, frac(0.55 * double(frame) / SR));
        const double ph = frac(pc + (f.indexEnd + idx) * lookup(sine, pm) * inv2pi);
        const float v = lookup(sine, ph);
        pc += ic * w;
        if (pc >= 1) pc -= 1;
        pm += im * w;
        if (pm >= 1) pm -= 1;
        idx *= k;
        if (f.tremolo <= 0) return std::pair<float, float>{v, v};
        const float a = float(f.tremolo) * lookup(sine, frac(f.tremRate * double(frame) / SR));
        return std::pair<float, float>{v * (1 - a), v * (1 + a)};
    });
}

// A plucked string (Karplus-Strong): a delay line of one period, filled with a burst of filtered noise (the pick),
// fed back through an averaging filter (the string loses its highs and level each period)
struct KsString {
    std::vector<float> buf;
    size_t pos = 0;
    float loss = 0.996f, prev = 0, c = 0, apIn = 0, apOut = 0;
    // The loop: N samples, half a sample of the averaging filter and an all-pass for the rest (in tune to a cent)
    void init(double hz, Noise& n, float lossIn, float pick)
    {
        const double total = SR / hz;
        const long whole = std::max(2L, long(std::floor(total - 0.6)));
        const double d = total - 0.5 - double(whole);
        c = float((1 - d) / (1 + d));
        buf.assign(size_t(whole), 0.f);
        loss = lossIn;
        float lp = 0;
        for (float& v : buf) {
            lp += pick * (n.next() - lp);
            v = lp;
        }
        pos = 0;
        prev = apIn = apOut = 0;
    }
    float next()
    {
        const float out = buf[pos];
        const float avg = loss * 0.5f * (out + prev);
        prev = out;
        apOut = c * avg + apIn - c * apOut;
        apIn = avg;
        buf[pos] = apOut;
        if (++pos == buf.size()) pos = 0;
        return out;
    }
};

// A nylon string (calm): a plucked string through a small guitar body (a resonance near 200 Hz, the highs rolled off)
void nylon(Context& c, double t0, double gate, int midi, double gain, double pan, double send)
{
    KsString str;
    Noise pick(std::uint32_t(t0 * 2711) + std::uint32_t(midi) * 13u + 7u);
    str.init(midiHz(midi), pick, 0.9975f, 0.45f);
    Biquad body = Biquad::make(Biquad::Band, 210, 1.4), air = Biquad::make(Biquad::Low, 5200, 0.7);
    const Env e{0.001, 0, 1, 0.15};
    note(c.bus, t0, gate, e, gain, pan, send, nullptr, [&](long long, long long) {
        const float x = str.next();
        const float v = air.run(x + 0.8f * body.run(x));
        return std::pair<float, float>{v, v};
    });
}

void bassNote(Context& c, double t0, double gate, int midi, const Env& e, double gain, Timbre t, bool ducked = false)
{
    const Table& table = c.tables.get(t, midi);
    Osc o{0, midiHz(midi) / SR};
    note(c.bus, t0, gate, e, gain, 0, 0.0, ducked ? c.duckOrNull() : nullptr, [&](long long, long long) {
        const float v = o.next(table);
        return std::pair<float, float>{v, v};
    });
}

// ---------- Drums ----------

struct KickP {
    double fEnd = 50, fSweep = 85, pitchTau = 0.035, decay = 0.26, click = 0.06, send = 0.02;
    double beater = 0; // an acoustic kick: the beater's slap (a band of noise near 3.5 kHz) and the shell's knock
};

void kick(Context& c, double t0, double vel, const KickP& k)
{
    const Table& sine = sineTable();
    double phase = 0;
    Biquad click = Biquad::make(Biquad::Low, 2500, 0.7), slap = Biquad::make(Biquad::Band, 3500, 1.2);
    Biquad knock = Biquad::make(Biquad::Band, 320, 2.0);
    Noise n(std::uint32_t(t0 * 1000) + 3);
    Env e{0.0015, k.decay, 0.0, 0.03};
    const double kSweep = std::exp(-1.0 / (k.pitchTau * SR)), kClick = std::exp(-1.0 / (0.002 * SR));
    const double kSlap = std::exp(-1.0 / (0.004 * SR)), kKnock = std::exp(-1.0 / (0.025 * SR));
    double sweep = k.fSweep, clickEnv = k.click, slapEnv = k.beater * 3, knockEnv = k.beater * 2;
    note(c.bus, t0, k.decay * 3.2, e, vel, 0, k.send, nullptr, [&](long long i, long long) {
        float v = lookup(sine, phase);
        phase += (k.fEnd + sweep) / SR;
        if (phase >= 1) phase -= 1;
        sweep *= kSweep;
        if (k.click > 0 && i < 480) {
            v += float(clickEnv) * click.run(n.next());
            clickEnv *= kClick;
        }
        if (k.beater > 0 && i < 4800) {
            const float x = n.next();
            v += float(slapEnv) * slap.run(x) + float(knockEnv) * knock.run(x);
            slapEnv *= kSlap;
            knockEnv *= kKnock;
        }
        return std::pair<float, float>{v, v};
    });
}

KickP kickOf(Mood m)
{
    switch (m) {
    case Mood::Calm: return {54, 45, 0.045, 0.2, 0.0, 0.05};
    case Mood::Hype: return {47, 115, 0.03, 0.32, 0.06, 0.02};
    case Mood::Lofi: return {52, 60, 0.04, 0.24, 0.0, 0.02};
    case Mood::Epic: return {42, 70, 0.06, 0.5, 0.02, 0.3};
    case Mood::Funk: return {62, 45, 0.012, 0.12, 0.0, 0.0, 0.45};
    }
    return {};
}

struct SnareP {
    // No gated tail any more: retro's gated snare (0.25 trial) was a far too loud "psssh" over the bed
    double band = 1900, noiseTau = 0.13, tone = 0.5, toneHz = 185, send = 0.3;
    double lowpass = 0;
};

void snare(Context& c, double t0, double vel, const SnareP& sp)
{
    Biquad band = Biquad::make(Biquad::Band, sp.band, 0.8), high = Biquad::make(Biquad::High, 500, 0.7);
    Biquad lp = Biquad::make(Biquad::Low, sp.lowpass > 0 ? sp.lowpass : 18000, 0.7);
    Noise n(std::uint32_t(t0 * 977) + 11);
    const Table& sine = sineTable();
    double phase = 0;
    const double toneTau = 0.05;
    const Env e{0.001, 0, 1, 0.03};
    const double kTone = std::exp(-1.0 / (toneTau * SR)), kNoise = std::exp(-1.0 / (sp.noiseTau * SR));
    double toneEnv = sp.tone, noiseEnv = 2.2;
    note(c.bus, t0, sp.noiseTau * 3.5, e, vel, -0.05, sp.send, nullptr, [&](long long, long long) {
        const float tone = lookup(sine, phase) * float(toneEnv);
        phase += sp.toneHz / SR;
        if (phase >= 1) phase -= 1;
        float noise = high.run(band.run(n.next())) * float(noiseEnv);
        if (sp.lowpass > 0) noise = lp.run(noise);
        toneEnv *= kTone;
        noiseEnv *= kNoise;
        const float v = tone + noise;
        return std::pair<float, float>{v, v};
    });
}

void clap(Context& c, double t0, double vel, double pan = 0.08, double freq = 1150)
{
    Biquad band = Biquad::make(Biquad::Band, freq, 1.1);
    Noise n(std::uint32_t(t0 * 733) + 29);
    Env e{0.0008, 0, 1, 0.03};
    // Three quick bursts (9 ms apart, 4 ms each) and a tail from 22 ms on
    const double kBurst = std::exp(-1.0 / (0.004 * SR)), kTail = std::exp(-1.0 / (0.11 * SR));
    const long long b1 = std::llround(0.009 * SR), b2 = std::llround(0.018 * SR), tail = std::llround(0.022 * SR);
    double burst = 0, tailEnv = 0.7;
    note(c.bus, t0, 0.42, e, vel, pan, 0.4, nullptr, [&](long long i, long long) {
        if (i == 0 || i == b1 || i == b2) burst += 1;
        double a = burst;
        burst *= kBurst;
        if (i >= tail) {
            a += tailEnv;
            tailEnv *= kTail;
        }
        const float v = band.run(n.next()) * float(2.6 * a);
        return std::pair<float, float>{v, v};
    });
}

void hat(Context& c, double t0, double vel, bool open, double cut = 7200, double pan = 0.25)
{
    Biquad high = Biquad::make(Biquad::High, cut, 0.7), band = Biquad::make(Biquad::Band, cut * 1.45, 0.9);
    Noise n(std::uint32_t(t0 * 1543) + 41);
    const double tau = open ? 0.2 : 0.032;
    Env e{0.0007, tau, 0.0, 0.02};
    note(c.bus, t0, tau * 4, e, vel, pan, 0.06, nullptr, [&](long long, long long) {
        const float x = n.next();
        const float v = 0.9f * high.run(x) + 0.6f * band.run(x);
        return std::pair<float, float>{v, v};
    });
}

void shaker(Context& c, double t0, double vel, double pan = -0.3)
{
    Biquad band = Biquad::make(Biquad::Band, 5600, 1.4);
    Noise n(std::uint32_t(t0 * 1201) + 53);
    Env e{0.012, 0.05, 0.0, 0.03};
    note(c.bus, t0, 0.14, e, vel, pan, 0.15, nullptr, [&](long long, long long) {
        const float v = 1.8f * band.run(n.next());
        return std::pair<float, float>{v, v};
    });
}

// Scratch: a muted guitar strum (a short burst of two noise bands, the strings damped)
void scratch(Context& c, double t0, double vel, double pan)
{
    Biquad b1 = Biquad::make(Biquad::Band, 2100, 1.8), b2 = Biquad::make(Biquad::Band, 3400, 2.5);
    Noise n(std::uint32_t(t0 * 877) + 131);
    Env e{0.001, 0.012, 0.0, 0.012};
    note(c.bus, t0, 0.03, e, vel, pan, 0.05, nullptr, [&](long long, long long) {
        const float x = n.next();
        const float v = 2.5f * (b1.run(x) + 0.6f * b2.run(x));
        return std::pair<float, float>{v, v};
    });
}

// Tom or taiko: a pitched sine falling a little, with a skin of filtered noise
void tom(Context& c, double t0, double vel, double hz, double decay, double pan, double send)
{
    const Table& sine = sineTable();
    Biquad skin = Biquad::make(Biquad::Low, 900, 0.7);
    Noise n(std::uint32_t(t0 * 457) + 101);
    double phase = 0, sweep = hz * 0.6, skinEnv = 0.5;
    const double kSweep = std::exp(-1.0 / (0.04 * SR)), kSkin = std::exp(-1.0 / (0.03 * SR));
    Env e{0.002, decay, 0.0, 0.05};
    note(c.bus, t0, decay * 3, e, vel, pan, send, nullptr, [&](long long, long long) {
        const float v = lookup(sine, phase) + float(skinEnv) * skin.run(n.next());
        phase += (hz + sweep) / SR;
        if (phase >= 1) phase -= 1;
        sweep *= kSweep;
        skinEnv *= kSkin;
        return std::pair<float, float>{v, v};
    });
}

// Boom: a deep sine falling from 70 to 32 Hz over a second (cinematic hits, section starts)
void boom(Context& c, double t0, double vel)
{
    const Table& sine = sineTable();
    double phase = 0, sweep = 38;
    const double kSweep = std::exp(-1.0 / (0.35 * SR));
    Env e{0.004, 0.9, 0.0, 0.2};
    note(c.bus, t0, 2.2, e, vel, 0, 0.3, nullptr, [&](long long, long long) {
        const float v = lookup(sine, phase);
        phase += (32 + sweep) / SR;
        if (phase >= 1) phase -= 1;
        sweep *= kSweep;
        return std::pair<float, float>{v, v};
    });
}

// Crash: two noise streams (left/right) through a high pass, ringing over a second
void crash(Context& c, double t0, double vel)
{
    Biquad hl = Biquad::make(Biquad::High, 3800, 0.7), hr = Biquad::make(Biquad::High, 3800, 0.7);
    Biquad bl = Biquad::make(Biquad::Band, 6500, 0.8), bri = Biquad::make(Biquad::Band, 6500, 0.8);
    Noise nl(std::uint32_t(t0 * 331) + 61), nr(std::uint32_t(t0 * 337) + 67);
    Env e{0.003, 0.9, 0.0, 0.3};
    note(c.bus, t0, 3.0, e, vel, 0, 0.3, nullptr, [&](long long, long long) {
        const float xl = nl.next(), xr = nr.next();
        return std::pair<float, float>{0.8f * hl.run(xl) + 0.5f * bl.run(xl), 0.8f * hr.run(xr) + 0.5f * bri.run(xr)};
    });
}

// Riser: noise through a band pass sweeping up, swelling to the bar one at t0 + len (it is gone there: the drop)
void riser(Context& c, double t0, double len, double vel)
{
    Biquad bl, br;
    Noise nl(std::uint32_t(t0 * 211) + 71), nr(std::uint32_t(t0 * 223) + 73);
    const long long n = std::llround(len * SR);
    Env e{0.01, 0, 1, 0.012};
    note(c.bus, t0, len - 0.012, e, vel, 0, 0.45, nullptr, [&](long long i, long long) {
        const double x = std::clamp(double(i) / double(n), 0.0, 1.0);
        if (i % 32 == 0) {
            const double f = 350 * std::pow(8000.0 / 350, x);
            bl.retune(Biquad::Band, f, 1.6);
            br.retune(Biquad::Band, f * 1.07, 1.6);
        }
        const float a = float(1.6 * x * x);
        return std::pair<float, float>{a * bl.run(nl.next()), a * br.run(nr.next())};
    });
}

// Vinyl: a bar of soft dust crackle and a little hiss (lofi); its own noise per bar
void vinyl(Context& c, int bar, double vel)
{
    Noise n(std::uint32_t(bar) * 2909u + 113u);
    Biquad pop = Biquad::make(Biquad::Band, 2600, 0.9), hiss = Biquad::make(Biquad::Band, 5000, 0.5);
    const double t0 = c.p.barTime(bar);
    Env e{0.02, 0, 1, 0.02};
    double target = 0, env = 0;
    const double kPop = std::exp(-1.0 / (0.0015 * SR));
    note(c.bus, t0, c.p.bar, e, vel, 0, 0.0, nullptr, [&](long long, long long) {
        const float x = n.next(), y = n.next();
        if (x > 0.9993f) target += 0.5 + 0.5 * double(y > 0 ? y : -y);
        env += (target - env) * 0.06; // a soft edge (no click)
        target *= kPop;
        const float v = float(env) * pop.run(x) + 0.05f * hiss.run(y);
        return std::pair<float, float>{v, v * 0.9f};
    });
}

// ---------- Parts ----------

int rootPc(const Plan& p, const Chord& ch) { return (p.key.root + ch.offset) % 12; }

void renderPads(Context& c)
{
    const Plan& p = c.p;
    const Style& s = c.s;
    if (s.padGain <= 0) return;
    const Env e{s.padAttack, 0, 1, s.padRelease};
    for (const Plan::Span& sp : p.spans) {
        if (sp.kind == Section::Ending) continue;
        double g = s.padGain;
        if (sp.kind == Section::Intro) g *= p.mood == Mood::Epic ? 0.8 : 0.85;
        if (sp.kind == Section::Break) g *= p.mood == Mood::Hype ? 1.15 : 1.0;
        const double t0 = p.beatTime(sp.from), gate = p.beatTime(sp.to - sp.from) - (p.mood == Mood::Hype ? 0.02 : 0);
        const std::array<int, 4>& v = p.voicingAt(sp.from);
        const int root = rootPc(p, p.chordAt(sp.from));
        const std::vector<int> notes(v.begin(), v.end());
        pad(c, t0, gate, notes, e, g, s.padRoot ? pcIn(root, 40) : -1, s.padDark, s.padBright, s.padDetune, s.padSend,
            s.padVibrato, s.padWide);
    }
    if (p.endBar > 0) { // the final chord rings out to the end
        const bool slow = p.mood == Mood::Calm || p.mood == Mood::Epic;
        const Env end{slow ? 0.2 : 0.02, slow ? 4.0 : 2.5, 0.35, 0.01};
        const std::array<int, 4>& v = p.voicingAt(p.endBar * 4);
        const int root = rootPc(p, p.tonic);
        const std::vector<int> notes(v.begin(), v.end());
        pad(c, p.barTime(p.endBar), p.length - p.barTime(p.endBar), notes, end, s.padGain * 1.05,
            s.padRoot ? pcIn(root, 40) : -1, s.padDark, s.padBright, s.padDetune, s.padSend, s.padVibrato, s.padWide);
    }
}

void renderBass(Context& c)
{
    const Plan& p = c.p;
    const int last = p.last();
    const auto bassOf = [&](int beatIndex, int low) { return pcIn(rootPc(p, p.chordAt(beatIndex)), low); };
    // Moods whose bass holds the chord: one note per chord span
    if (p.mood == Mood::Calm || p.mood == Mood::Epic) {
        for (const Plan::Span& sp : p.spans) {
            if (sp.kind == Section::Ending) continue;
            const int b = sp.from / 4;
            if (sp.kind == Section::Intro && b < p.intro / 2) continue;
            const double t0 = p.beatTime(sp.from), gate = p.beatTime(sp.to - sp.from) - 0.05;
            if (p.mood == Mood::Calm) {
                bassNote(c, t0, gate, bassOf(sp.from, 36), {0.12, 3.5, 0.6, 0.5}, 0.22, Timbre::Bass);
            } else {
                const int m = bassOf(sp.from, 33);
                bassNote(c, t0, gate, m, {0.3, 0, 1, 0.8}, sp.kind == Section::Main ? 0.1 : 0.08, Timbre::StringsDark);
                if (sp.kind == Section::Main) bassNote(c, t0, gate, m - 12, {0.3, 0, 1, 0.8}, 0.06, Timbre::Sine);
            }
        }
    }
    Noise vel(p.seed * 53 + 7);
    for (int b = 0; b < last; ++b) {
        const Section::Kind k = p.kindAt(b);
        const double t0 = p.barTime(b);
        const bool lift = p.lift[size_t(b)];
        switch (p.mood) {
        case Mood::Calm:
        case Mood::Epic: break;
        case Mood::Hype: {
            if (k != Section::Main) break;
            const int m = bassOf(b * 4, 33);
            for (int i = 0; i < 4; ++i)
                bassNote(c, t0 + (i + 0.5) * p.beat, p.beat * 0.42, m, {0.003, 0.18, 0.55, 0.04}, 0.4, Timbre::BassHype, true);
            break;
        }
        case Mood::Lofi: {
            if (k == Section::Intro && b < p.intro / 2) break;
            const int m = bassOf(b * 4, 36);
            const Env e{0.012, 0.7, 0.45, 0.12};
            if (k == Section::Break) {
                bassNote(c, t0, p.bar * 0.9, m, e, 0.3, Timbre::Bass);
                break;
            }
            bassNote(c, p.at(b, 0), p.beat * 1.4, m, e, 0.32, Timbre::Bass);
            bassNote(c, p.at(b, 1.75), p.beat * 0.5, m, e, 0.22, Timbre::Bass);
            const int m2 = bassOf(b * 4 + 2, 36);
            bassNote(c, p.at(b, 2.5), p.beat * 1.2, lift ? m2 + 7 : m2, e, 0.28, Timbre::Bass);
            break;
        }
        case Mood::Funk: {
            if (k == Section::Intro) break;
            const int m = bassOf(b * 4, 33);
            if (k == Section::Break) {
                bassNote(c, t0, p.bar * 0.5, m, {0.005, 0.5, 0.3, 0.08}, 0.34, Timbre::SlapDark);
                break;
            }
            // {sixteenth, interval (-1: a dead note), length in sixteenths, velocity}
            struct Hit {
                int at, iv;
                double len, vel;
            };
            static const std::vector<Hit> pats[] = {
                {{0, 0, 2, 1}, {3, -1, 0.5, 0.5}, {4, 12, 1, 0.8}, {6, 0, 1, 0.7}, {7, -1, 0.5, 0.5}, {8, 0, 1.5, 0.9},
                 {10, 12, 1, 0.8}, {11, 10, 1, 0.7}, {13, 7, 1, 0.7}, {14, 12, 1, 0.85}},
                {{0, 0, 1.5, 1}, {2, -1, 0.5, 0.5}, {3, 0, 1, 0.75}, {6, 12, 1, 0.85}, {8, 0, 1, 0.9}, {9, -1, 0.5, 0.5},
                 {10, 7, 1, 0.75}, {12, 10, 1, 0.8}, {14, 12, 0.8, 0.9}, {15, 0, 1, 0.7}}};
            const std::vector<Hit>& pat = pats[(p.variant + (lift ? 1 : 0)) % 2];
            for (const Hit& h : pat) {
                const double t = p.at(b, h.at * 0.25);
                const double len = h.len * p.beat / 4;
                const int beatIndex = b * 4 + h.at / 4;
                const int root = bassOf(beatIndex, 33);
                const double g = 0.36 * h.vel * (0.94 + 0.06 * vel.unit());
                if (h.iv < 0) tone(c, t, len, root, {0.001, 0.02, 0.0, 0.01}, g * 0.8, 0, 0, ToneP{Timbre::SlapDark, Timbre::SlapBright, 0.8, 0.1, 0.01});
                else tone(c, t, len, root + h.iv, {0.001, 0.25, 0.55, 0.03}, g, 0, 0.02, ToneP{Timbre::SlapDark, Timbre::SlapBright, 1.0, 0.1, 0.06});
            }
            break;
        }
        }
    }
    if (p.endBar > 0) {
        const double t = p.barTime(p.endBar), len = p.length - t;
        const int low = p.mood == Mood::Hype || p.mood == Mood::Epic || p.mood == Mood::Funk ? 33 : 36;
        const int m = pcIn(rootPc(p, p.tonic), low);
        Timbre t0 = Timbre::Bass;
        switch (p.mood) {
        case Mood::Hype: t0 = Timbre::BassHype; break;
        case Mood::Epic: t0 = Timbre::Sine; break;
        case Mood::Funk: t0 = Timbre::SlapDark; break;
        default: break;
        }
        bassNote(c, t, len, p.mood == Mood::Epic ? m - 12 : m, {0.005, 0.9, 0.0, 0.01}, 0.45, t0);
    }
}

// The chord instrument of each mood
void renderComp(Context& c)
{
    const Plan& p = c.p;
    const int last = p.last();
    Noise vel(p.seed * 31 + 5);
    const auto human = [&](double v) { return v * (0.92 + 0.08 * vel.unit()); };
    for (int b = 0; b < last; ++b) {
        const Section::Kind k = p.kindAt(b);
        const bool lift = p.lift[size_t(b)];
        const double t0 = p.barTime(b);
        switch (p.mood) {
        case Mood::Calm: {
            // A nylon-string guitar picked in eighths: the root on one and three, the chord's tones between
            static const int patterns[][8] = {{-1, 1, 2, 3, -1, 2, 3, 1}, {-1, 2, 1, 3, -1, 3, 2, 1}, {-1, 1, 3, 2, -1, 1, 2, 3}};
            const int* pat = patterns[p.variant % 3];
            for (int i = 0; i < 8; ++i) {
                if (k == Section::Break && i % 2) continue;
                const std::array<int, 4> v = p.voicingAt(b * 4 + i / 2);
                const bool bass = pat[i] < 0;
                const int m = bass ? pcIn(rootPc(p, p.chordAt(b * 4 + i / 2)), 45)
                                   : v[size_t(pat[i])] + (v[size_t(pat[i])] < 57 ? 12 : 0);
                const double g = (bass ? 0.42 : 0.3) * (lift ? 1.0 : 0.92);
                nylon(c, t0 + i * p.beat / 2, p.beat * (bass ? 1.9 : 1.4), m, human(g), bass ? -0.1 : (i % 4 == 1 ? 0.3 : -0.3),
                      0.45);
            }
            break;
        }
        case Mood::Hype: {
            // A 16th arp with two echoes (dotted eighths, the other side)
            static const int patterns[][16] = {{0, 1, 2, 3, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 4, 3},
                                               {0, 2, 4, 2, 1, 3, 5, 3, 0, 2, 4, 6, 4, 2, 1, 2},
                                               {4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1}};
            const int* pat = patterns[p.variant % 3];
            const double sixteenth = p.beat / 4, echo = 0.75 * p.beat;
            const std::array<int, 4> v = p.voicingAt(b * 4);
            const int up = lift ? 12 : 0;
            const int tones[7] = {v[0] + up, v[1] + up, v[2] + up, v[3] + up, v[1] + 12 + up, v[2] + 12 + up, v[3] + 12 + up};
            const ToneP tp{Timbre::PluckDark, Timbre::HypeBright, 1.0, 0.1, 0.06, true};
            for (int i = 0; i < 16; ++i) {
                const double t = t0 + i * sixteenth;
                const int m = tones[pat[i]];
                const double g = 0.07 * (i % 4 == 0 ? 1.0 : 0.78);
                const double pan = (i % 2 ? 0.15 : -0.15);
                const Env e{0.002, 0.09, 0.0, 0.04};
                tone(c, t, 0.12, m, e, g, pan, 0.2, tp);
                tone(c, t + echo, 0.12, m, e, g * 0.32, -pan * 4, 0.3, ToneP{Timbre::PluckDark, Timbre::PluckBright, 0.5, 0, 0.04, true});
                tone(c, t + 2 * echo, 0.12, m, e, g * 0.13, pan * 4, 0.3, ToneP{Timbre::PluckDark, Timbre::PluckBright, 0.35, 0, 0.04, true});
            }
            break;
        }
        case Mood::Lofi: {
            // Electric piano comping: the chord on one, a short push on the and of two, the chord again on three
            const FmP ep{1.0, 1.8, 0.25, 0.3, 0.22, 3.6, 9};
            const auto chordAt = [&](double pos, double len, double g) {
                const std::array<int, 4> v = p.voicingAt(b * 4 + int(pos));
                const double t = p.at(b, pos);
                for (int n = 0; n < 4; ++n)
                    fm(c, t + n * 0.012, len * p.beat, v[size_t(n)], {0.003, 1.4, 0.25, 0.25}, human(g), n % 2 ? 0.2 : -0.2, 0.25, ep);
            };
            if (k == Section::Break) {
                chordAt(0, 3.6, 0.075);
                break;
            }
            chordAt(0, 1.4, 0.08);
            chordAt(1.75, 0.45, 0.055);
            chordAt(2.5, lift ? 0.9 : 1.3, 0.065);
            if (lift) chordAt(3.5, 0.45, 0.05);
            break;
        }
        case Mood::Epic: {
            // String ostinato in eighths (root, octave, fifth) in main parts
            if (k != Section::Main) break;
            static const int iv[8] = {0, 12, 7, 12, 0, 12, 7, 15};
            for (int i = 0; i < 8; ++i) {
                const Chord& ch = p.chordAt(b * 4 + i / 2);
                const int root = pcIn(rootPc(p, ch), 45);
                const int interval = iv[i] == 15 ? (ch.minor() ? 15 : 16) : iv[i];
                tone(c, t0 + i * p.beat / 2, p.beat / 2 * 0.55, root + interval, {0.004, 0.12, 0.3, 0.06},
                     human((i % 2 ? 0.1 : 0.15) * (lift ? 1.1 : 1.0)), i % 2 ? 0.3 : -0.3, 0.4,
                     ToneP{Timbre::StringsDark, Timbre::StringsBright, 0.95, 0.45, 0.08, true});
            }
            break;
        }
        case Mood::Funk: {
            // Clavinet stabs on syncopated sixteenths (upper three notes of the chord)
            if (k == Section::Intro) break;
            static const int pats[][8] = {{0, 3, 6, 8, 10, 11, 14, -1}, {2, 3, 6, 7, 10, 12, 14, 15}};
            const int* pat = pats[(p.variant + (lift ? 1 : 0)) % 2];
            for (int i = 0; i < 8 && pat[i] >= 0; ++i) {
                if (k == Section::Break && pat[i] % 4 != 0) continue;
                const std::array<int, 4> v = p.voicingAt(b * 4 + pat[i] / 4);
                const double t = p.at(b, pat[i] * 0.25);
                for (int n = 1; n < 4; ++n)
                    tone(c, t, p.beat * 0.12, v[size_t(n)] + 12, {0.001, 0.045, 0.0, 0.02}, human(0.06), n == 1 ? -0.3 : 0.3, 0.05,
                         ToneP{Timbre::PulseDark, Timbre::PulseBright, 1.0, 0.15, 0.025});
            }
            // A muted guitar scratching sixteenths, accents with the snare
            if (k == Section::Main)
                for (int i = 0; i < 16; ++i)
                    if (i % 2 || i == 4 || i == 12) scratch(c, p.at(b, i * 0.25), human(i == 4 || i == 12 ? 0.09 : 0.035), 0.45);
            // Horn stabs: the and of four every second bar, in lift bars also the and of two
            for (const double pos : {1.5, 3.5}) {
                if (k == Section::Break || (pos < 2 && !lift) || (pos > 2 && b % 2 == 0)) continue;
                const std::array<int, 4> v = p.voicingAt(b * 4 + int(pos));
                for (int n = 1; n < 4; ++n)
                    tone(c, p.at(b, pos), p.beat * 0.3, v[size_t(n)] + 12, {0.012, 0.12, 0.5, 0.06}, human(0.06),
                         n == 1 ? -0.2 : n == 3 ? 0.2 : 0, 0.15, ToneP{Timbre::BrassDark, Timbre::BrassBright, 0.95, 0.45, 0.1});
            }
            break;
        }
        }
    }
    // The final chord
    if (p.endBar > 0) {
        const double t = p.barTime(p.endBar), len = p.length - t;
        const std::array<int, 4> v = p.voicingAt(p.endBar * 4);
        switch (p.mood) {
        case Mood::Calm: // the final chord strummed slowly
            nylon(c, t, len, pcIn(rootPc(p, p.tonic), 45), 0.4, -0.1, 0.5);
            for (int i = 0; i < 4; ++i)
                nylon(c, t + (i + 1) * 0.05, len, v[size_t(i)] + (v[size_t(i)] < 57 ? 12 : 0), 0.28, i % 2 ? 0.25 : -0.25, 0.5);
            break;
        case Mood::Lofi:
            for (int i = 0; i < 4; ++i)
                fm(c, t + i * 0.03, len, v[size_t(i)], {0.003, 1.6, 0.0, 0.01}, 0.08, i % 2 ? 0.2 : -0.2, 0.3,
                   FmP{1.0, 1.8, 0.25, 0.3, 0.22, 3.6, 9});
            break;
        case Mood::Funk:
            for (int n = 1; n < 4; ++n)
                tone(c, t, 0.3, v[size_t(n)] + 12, {0.001, 0.2, 0.0, 0.05}, 0.06, n == 1 ? -0.3 : 0.3, 0.1,
                     ToneP{Timbre::PulseDark, Timbre::PulseBright, 1.0, 0.35, 0.08});
            break;
        default: break;
        }
    }
}

// The tune (epic: horns, an octave below doubling them)
void renderLead(Context& c)
{
    const Plan& p = c.p;
    for (const LeadNote& n : p.lead) {
        const double t = p.beatTime(n.beat), len = n.len * p.beat;
        switch (p.mood) {
        case Mood::Epic: {
            // The brightness swells (bStart below bEnd), a vibrato sets in
            const ToneP swell{Timbre::BrassDark, Timbre::BrassBright, 0.1, 0.65, 0.25, false, 5, 4.8, 10, 0.3};
            const Env e{0.09, 0, 1, 0.3};
            tone(c, t, len * 0.95, n.midi, e, 0.14 * n.vel, -0.15, 0.55, swell);
            tone(c, t, len * 0.95, n.midi - 12, e, 0.07 * n.vel, 0.15, 0.55, swell);
            break;
        }
        default: break;
        }
    }
}

void renderDrums(Context& c)
{
    const Plan& p = c.p;
    const int last = p.last();
    const KickP kp = kickOf(p.mood);
    Noise vel(p.seed * 13 + 1);
    const auto human = [&](double v) { return v * (0.93 + 0.07 * vel.unit()); };
    for (int b = 0; b < last; ++b) {
        const Section::Kind k = p.kindAt(b);
        const double t0 = p.barTime(b);
        const bool lift = p.lift[size_t(b)], fill = p.fill[size_t(b)] && b + 1 < p.bars;
        const auto at = [&](double beats) { return p.at(b, beats); };
        const bool newPart = b > 0 && p.kindAt(b - 1) != k && k == Section::Main;
        switch (p.mood) {
        case Mood::Calm: {
            // Brushed half time: a soft kick on one, a rim click on three, a quiet shaker
            if (k == Section::Intro || k == Section::Break) break;
            // Brushes: a swish on two and four, a shaker (no kick)
            const SnareP brush{3800, 0.11, 0.0, 200, 0.6, 7000};
            if (lift) tom(c, at(0), human(0.25), 70, 0.25, 0, 0.3); // a soft floor tom on one in lift bars
            snare(c, at(1), human(0.11), brush);
            snare(c, at(3), human(0.12), brush);
            for (int i = 0; i < 8; ++i) shaker(c, at(i * 0.5), human(i % 2 ? 0.05 : 0.085));
            break;
        }
        case Mood::Hype: {
            const SnareP sn{1900, 0.13, 0.5, 185, 0.3};
            if (newPart) crash(c, at(0), 0.09);
            if (k == Section::Main) {
                for (int i = 0; i < 4; ++i) kick(c, at(i), human(0.85), kp);
                clap(c, at(1), human(0.2));
                clap(c, at(3), human(0.2));
                snare(c, at(1), human(0.1), sn);
                snare(c, at(3), human(0.1), sn);
                for (int i = 0; i < 4; ++i) hat(c, at(i + 0.5), human(0.06), true);
                for (int i = 0; i < 16; ++i)
                    if (i % 2 == 1 || lift) hat(c, at(i * 0.25), human(i % 4 == 2 ? 0.045 : 0.028), false);
                if (fill)
                    for (int i = 0; i < 8; ++i) snare(c, at(2 + i * 0.25), 0.06 + 0.16 * i / 7.0, sn);
            } else if (k == Section::Intro) {
                for (int i = 0; i < 8; ++i) hat(c, at(i * 0.5), human(i % 2 ? 0.035 : 0.022), false);
            }
            if (fill) riser(c, t0, p.bar, k == Section::Main ? 0.03 : 0.05);
            break;
        }
        case Mood::Lofi: {
            // Boom-bap on swung sixteenths: a dusty kick, a fat low snare, soft dark hats; crackle all along
            vinyl(c, b, 0.05);
            if (k == Section::Intro || k == Section::Break) break;
            const SnareP sn{1400, 0.16, 0.6, 160, 0.2, 3800};
            kick(c, at(0), human(0.7), kp);
            kick(c, at(1.75), human(0.4), kp);
            kick(c, at(2.5), human(0.6), kp);
            snare(c, at(1), human(0.36), sn);
            snare(c, at(3), human(0.38), sn);
            for (int i = 0; i < 8; ++i) hat(c, at(i * 0.5), human(i % 2 ? 0.045 : 0.07), false, 5200, 0.15);
            if (lift) hat(c, at(3.75), human(0.035), false, 5200, 0.15);
            break;
        }
        case Mood::Epic: {
            // Taiko and big toms, an ensemble snare on three in lift bars, cymbal swells into new parts
            if (newPart) boom(c, at(0), 0.55), crash(c, at(0), 0.06);
            if (k == Section::Main) {
                tom(c, at(0), human(0.95), 55, 0.45, 0, 0.3);
                tom(c, at(1), human(0.6), 92, 0.25, -0.35, 0.3);
                tom(c, at(2), human(0.9), 55, 0.45, 0, 0.3);
                tom(c, at(3), human(0.6), 92, 0.25, 0.35, 0.3);
                tom(c, at(3.5), human(0.4), 92, 0.2, 0.35, 0.3);
                if (lift) snare(c, at(2), human(0.28), SnareP{1500, 0.2, 0.4, 170, 0.7});
                if (fill)
                    for (int i = 0; i < 8; ++i) tom(c, at(2 + i * 0.25), 0.25 + 0.3 * i / 7.0, i % 2 ? 92 : 75, 0.2, i % 2 ? 0.3 : -0.3, 0.45);
            } else if (k == Section::Intro && b >= p.intro / 2) {
                tom(c, at(0), human(0.4), 62, 0.4, 0, 0.5);
            }
            if (fill && k != Section::Main) riser(c, t0, p.bar, 0.03);
            break;
        }
        case Mood::Funk: {
            // Tight kick on syncopations, snare on two and four with ghost notes, swung 16th hats with accents
            const SnareP sn{2300, 0.1, 0.55, 210, 0.08};
            const bool groove = k == Section::Main || k == Section::Intro;
            if (!groove) {
                for (int i = 0; i < 8; ++i) hat(c, at(i * 0.5), human(0.05), false, 8000, 0.2);
                break;
            }
            kick(c, at(0), human(0.75), kp);
            kick(c, at(0.75), human(0.5), kp);
            kick(c, at(2.5), human(0.7), kp);
            if (lift || b % 2) kick(c, at(3.25), human(0.45), kp);
            snare(c, at(1), human(0.42), sn);
            snare(c, at(3), human(0.42), sn);
            for (const double g : {1.75, 2.25, 3.75}) snare(c, at(g), human(0.07), sn);
            for (int i = 0; i < 16; ++i) {
                if (i == 14) {
                    hat(c, at(3.5), human(0.075), true, 7600, 0.2);
                    continue;
                }
                hat(c, at(i * 0.25), human(i % 4 == 0 ? 0.08 : i % 2 ? 0.035 : 0.055), false, 7600, 0.2);
            }
            break;
        }
        }
    }
    if (p.endBar > 0) { // the final hit
        const double t = p.barTime(p.endBar);
        switch (p.mood) {
        case Mood::Calm: kick(c, t, 0.5, kp); break;
        case Mood::Lofi:
            kick(c, t, 0.7, kp);
            for (int b = p.endBar; b < p.bars; ++b) vinyl(c, b, 0.05);
            break;
        case Mood::Epic:
            boom(c, t, 0.65);
            tom(c, t, 0.75, 62, 0.5, 0, 0.5);
            crash(c, t, 0.08);
            break;
        case Mood::Funk:
            kick(c, t, 0.8, kp);
            hat(c, t, 0.08, true, 7600, 0.2);
            break;
        default:
            kick(c, t, 0.85, kp);
            crash(c, t, p.mood == Mood::Hype ? 0.1 : 0.08);
            if (p.mood == Mood::Hype) clap(c, t, 0.18);
            break;
        }
    }
}

// Reverb (four damped feedback combs and two all-passes per side, own delays), mono send -> stereo
// Runs at half the rate (the send averaged in pairs, the reverb linearly interpolated back): a reverb has little above
// 10 kHz, and it takes half the time. Its size, length, darkness and level are the mood's
void reverb(float* out, const float* send, long long frames, const Style& st)
{
    const float fb = float(st.feedback), damp = float(st.damp);
    struct Comb {
        std::vector<float> buf;
        size_t pos = 0;
        float filt = 0;
    };
    struct AllPass {
        std::vector<float> buf;
        size_t pos = 0;
    };
    const int combL[] = {843, 801, 933, 889}, apL[] = {281, 121}; // at 24 kHz, size 1
    const int spread = 19;
    const long long pre = std::llround(st.preDelay * SR / 2), half = frames / 2 + 1;
    // The input level so that a longer tail does not get louder (the combs' gain grows with 1 / (1 - fb))
    const float in = float(0.06 * (1 - fb) / 0.2), outGain = float(st.wet);
    const auto side = [&](int ch) {
        Comb combs[4];
        AllPass aps[2];
        for (int i = 0; i < 4; ++i) combs[i].buf.assign(size_t(std::lround(combL[i] * st.size)) + size_t(ch * spread), 0.f);
        for (int i = 0; i < 2; ++i) aps[i].buf.assign(size_t(apL[i] + ch * (spread / 2)), 0.f);
        float previous = 0;
        for (long long h = 0; h < half; ++h) {
            const long long g = h - pre; // the send, averaged over frames 2g and 2g + 1
            float x = 0;
            if (g >= 0 && 2 * g < frames) x = 0.5f * (send[2 * g] + (2 * g + 1 < frames ? send[2 * g + 1] : 0.f)) * in;
            float y = 0;
            for (Comb& c : combs) {
                const float o = c.buf[c.pos];
                c.filt = o * (1 - damp) + c.filt * damp;
                c.buf[c.pos] = x + c.filt * fb;
                if (++c.pos == c.buf.size()) c.pos = 0;
                y += o;
            }
            for (AllPass& a : aps) {
                const float b = a.buf[a.pos];
                a.buf[a.pos] = y + b * 0.5f;
                y = b - y;
                if (++a.pos == a.buf.size()) a.pos = 0;
            }
            // Back to the full rate: frame 2h gets the mean of the previous value and this one, 2h + 1 this one
            if (2 * h < frames) out[4 * h + ch] += 0.5f * (previous + y) * outGain;
            if (2 * h + 1 < frames) out[4 * h + 2 + ch] += y * outGain;
            previous = y;
        }
    };
    parallel(2, side, [](int) {}); // a side allocates before it writes: nothing to undo
}

// True peak of interleaved stereo (BS.1770 4x oversampling, the coefficients of core/TruePeak), in parallel parts.
// The interpolation is computed only where the samples around it reach half the sample peak: far below that it cannot
// come near the top (TruePeakMeter computes it everywhere, too slow for minutes of music)
double truePeakDb(const std::vector<float>& s, long long frames)
{
    float samplePeak = 0;
    for (float v : s) samplePeak = std::max(samplePeak, std::abs(v));
    if (samplePeak <= 0) return TruePeakMeter::kSilence;
    const float threshold = 0.5f * samplePeak;
    const int parts = threadsFor(frames, kRate * 20LL, 4);
    std::vector<double> peaks(size_t(parts), 0.0);
    const auto x = [&](long long f, int c) { return f >= 0 && f < frames ? double(s[size_t(2 * f + c)]) : 0.0; };
    const auto work = [&](int part) {
        const long long from = frames * part / parts, to = frames * (part + 1) / parts;
        double peak = 0;
        for (int c = 0; c < 2; ++c)
            for (long long f = from; f < to + TruePeakMeter::kTaps; ++f) {
                if (std::max(std::abs(x(f - 5, c)), std::abs(x(f - 6, c))) < threshold) continue;
                for (int ph = 0; ph < TruePeakMeter::kPhases; ++ph) {
                    double y = 0;
                    for (int k = 0; k < TruePeakMeter::kTaps; ++k) y += TruePeakMeter::kCoef[ph][k] * x(f - k, c);
                    peak = std::max(peak, std::abs(y));
                }
            }
        peaks[size_t(part)] = peak;
    };
    parallel(parts, work, [](int) {});
    const double peak = std::max(double(samplePeak), *std::max_element(peaks.begin(), peaks.end()));
    return 20 * std::log10(peak);
}

double dbOf(double meanSquare) { return 10 * std::log10(meanSquare + 1e-12); }

double meanOf(const std::vector<double>& db, int from, int to)
{
    double sum = 0;
    int n = 0;
    for (int i = std::max(0, from); i < std::min(int(db.size()), to); ++i) sum += std::pow(10.0, db[size_t(i)] / 10), ++n;
    return n > 0 ? dbOf(sum / n) : -120;
}

double median(std::vector<double> v)
{
    if (v.empty()) return -120;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace
// ---------- Public ----------

QString moodId(Mood m)
{
    switch (m) {
    case Mood::Calm: return QStringLiteral("calm");
    case Mood::Hype: return QStringLiteral("hype");
    case Mood::Lofi: return QStringLiteral("lofi");
    case Mood::Epic: return QStringLiteral("epic");
    case Mood::Funk: return QStringLiteral("funk");
    }
    return {};
}

std::optional<Mood> moodFrom(const QString& s)
{
    const QString t = s.trimmed().toLower();
    for (const Mood m : kMoods)
        if (t == moodId(m)) return m;
    static const std::map<QString, Mood> other{
        // drive (public before 0.25: guide, projects) and its words: the closest mood left is hype (energetic, steady
        // eighths, travel/sport/action); tense, retro and happy were tried and dropped in 0.25 (see MusicBed.h)
        {"drive", Mood::Hype},       {"driving", Mood::Hype},    {"treibend", Mood::Hype},   {"rock", Mood::Hype},
        {"ruhig", Mood::Calm},
        {"edm", Mood::Hype},         {"lo-fi", Mood::Lofi},      {"chill", Mood::Lofi},      {"chillhop", Mood::Lofi},
        {"cinematic", Mood::Epic},   {"episch", Mood::Epic},     {"dark", Mood::Epic},       {"düster", Mood::Epic},
        {"funky", Mood::Funk},       {"groove", Mood::Funk},     {"upbeat", Mood::Funk},     {"fröhlich", Mood::Funk}};
    const auto it = other.find(t);
    if (it != other.end()) return it->second;
    return std::nullopt;
}

QStringList moodIds()
{
    QStringList ids;
    for (const Mood m : kMoods) ids << moodId(m);
    return ids;
}

const char* moodLabel(Mood m)
{
    switch (m) {
    case Mood::Calm: return N_("Ruhig");
    case Mood::Hype: return N_("Hype");
    case Mood::Lofi: return N_("Lo-Fi");
    case Mood::Epic: return N_("Episch");
    case Mood::Funk: return N_("Funky");
    }
    return "";
}

QString description(Mood m)
{
    switch (m) {
    case Mood::Calm:
        return "calm: a fingerpicked nylon guitar on soft chords (maj7, add9; two bars each), a quiet warm pad, round "
               "bass, brushes and a shaker (no kick), a wide hall (vlog, tutorial, product, under speech)";
    case Mood::Hype:
        return "hype: pumping supersaw, off-beat bass, a 16th arp with echoes, four on the floor, risers into the "
               "drops (gaming highlights, shorts, hype edits)";
    case Mood::Lofi:
        return "lofi: wobbly electric piano on jazz ninths, swung boom-bap drums, vinyl crackle, a dark warm master "
               "(study, chill vlog, cooking, cozy b-roll)";
    case Mood::Epic:
        return "epic: string ensemble and an eighth-note ostinato, taiko and big toms, a horn theme, deep booms, a huge "
               "hall (trailer, intro, big reveal, sports finale)";
    case Mood::Funk:
        return "funk: slap bass, clavinet and horn stabs, a scratching guitar on swung 16ths, a tight dry kit with "
               "ghost notes, a dorian vamp (fashion, food, dance, playful product)";
    }
    return {};
}

double defaultBpm(Mood m) { return styleOf(m).bpm; }

Key defaultKey(Mood m) { return styleOf(m).key; }

std::optional<Key> keyFrom(const QString& text)
{
    static const QRegularExpression re(
        "^\\s*([a-gA-G])\\s*(#|b|is|es|s)?\\s*[- ]?\\s*(m|min|minor|moll|maj|major|dur)?\\s*$",
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch()) return std::nullopt;
    static const int base[] = {9, 11, 0, 2, 4, 5, 7}; // a b c d e f g
    const QString letter = match.captured(1);
    Key k;
    k.root = base[letter.toLower()[0].unicode() - 'a'];
    const QString acc = match.captured(2).toLower();
    if (acc == "#" || acc == "is") k.root += 1;
    else if (acc == "b" || acc == "es" || acc == "s") k.root -= 1;
    k.root = (k.root + 12) % 12;
    const QString mode = match.captured(3).toLower();
    // "Am", "a minor", "a-moll"; a lower-case letter alone is minor too (the German way: "a" = A minor)
    k.minor = mode == "m" || mode == "min" || mode == "minor" || mode == "moll" || (mode.isEmpty() && letter[0].isLower());
    return k;
}

QString keyName(const Key& k) { return noteName(k.root, flatKey(k)) + (k.minor ? " minor" : " major"); }

QString sectionName(Section::Kind k)
{
    switch (k) {
    case Section::Intro: return QStringLiteral("intro");
    case Section::Main: return QStringLiteral("main");
    case Section::Break: return QStringLiteral("break");
    case Section::Ending: return QStringLiteral("ending");
    }
    return {};
}

void setThreadsForTests(int count, bool failStarts)
{
    g_threads = std::max(0, count);
    g_failStarts = failStarts;
}

long long frameCount(double seconds) { return std::max(1LL, std::llround(seconds * SR)); }

namespace {

// Everything but the sound and what is measured on it
Result describe(const Plan& p)
{
    Result r;
    r.mood = p.mood;
    r.bpm = p.bpm;
    r.key = p.key;
    r.seed = p.seed;
    r.length = p.length;
    r.endBar = p.endBar;
    r.sections = p.sections;
    r.chordBeats = p.style.chordBeats;
    const bool flats = flatKey(p.key);
    for (const Chord& ch : p.prog) r.chords << noteName(p.key.root + ch.offset, flats) + QString::fromLatin1(suffix(ch.q));
    // The grid as core/Beats gives it
    Beats::Result& b = r.beats;
    b.found = true;
    b.bpm = p.bpm;
    b.confidence = 1;
    b.period = p.beat;
    b.phase = 0;
    b.beats = std::max(0, int(std::floor((p.length - 1e-6) / p.beat)));
    b.downbeat = 0;
    b.downbeatConfidence = 1;
    const int cycleBars = int(p.prog.size()) * p.style.chordBeats / 4;
    b.phraseBars = cycleBars == 8 ? 8 : 4;
    b.phraseOffset = p.intro % b.phraseBars;
    b.phraseConfidence = 1;
    return r;
}

} // namespace

Result describe(const Options& o) { return describe(makePlan(o)); }

Result make(const Options& o)
{
    const Plan p = makePlan(o);
    const long long n = p.frames;
    std::vector<float> out(size_t(2 * n), 0.f), send(size_t(n), 0.f);
    Control bright, duckGain;
    const double depth = p.style.duck;
    {
        const Brightness br = brightnessOf(p);
        bright.build(n, [&](long long f) { return br.at(double(f) / SR); });
        Duck duck;
        duck.setup(depth, p.beat, p.style.duckRelease);
        duck.bars.assign(size_t(p.bars + 1), false);
        for (int b = 0; b <= p.bars; ++b) duck.bars[size_t(b)] = p.kindAt(b) == Section::Main;
        duckGain.build(n, [&](long long f) { return duck.at(f); });
    }

    // The voices in time windows, one thread each (the result does not depend on how many)
    Tables tables;
    const auto render = [&](long long lo, long long hi) {
        Context c(p, Bus{out.data(), send.data(), n, lo, hi}, tables, bright, duckGain, depth > 0);
        renderPads(c);
        renderBass(c);
        renderComp(c);
        renderLead(c);
        renderDrums(c);
    };
    const int threads = threadsFor(n, kRate * 8LL, 8);
    parallel(
        threads, [&](int t) { render(n * t / threads, n * (t + 1) / threads); },
        [&](int t) { // a window that failed half way: its frames start again from silence
            const long long lo = n * t / threads, hi = n * (t + 1) / threads;
            std::fill(out.begin() + 2 * lo, out.begin() + 2 * hi, 0.f);
            std::fill(send.begin() + lo, send.begin() + hi, 0.f);
        });
    reverb(out.data(), send.data(), n, p.style);
    send = {};

    // Master: the mood's colour (a low pass for a worn sound, a soft saturation), a DC blocker (one pole at ~18 Hz), a
    // short fade-in, the end faded to silence at the exact length
    std::vector<float>& s = out;
    if (p.style.masterLowpass > 0 || p.style.masterDrive > 0) {
        const auto colour = [&](int ch) {
            Biquad a = Biquad::make(Biquad::Low, p.style.masterLowpass > 0 ? p.style.masterLowpass : 20000, 0.54);
            Biquad b = Biquad::make(Biquad::Low, p.style.masterLowpass > 0 ? p.style.masterLowpass : 20000, 1.31);
            const bool lowpass = p.style.masterLowpass > 0;
            const float g = float(1 + 2 * p.style.masterDrive);
            for (long long f = 0; f < n; ++f) {
                float x = s[size_t(2 * f + ch)];
                if (lowpass) x = b.run(a.run(x));
                if (g > 1) x = soft(x * g) / g;
                s[size_t(2 * f + ch)] = x;
            }
        };
        parallel(2, colour, [](int) {}); // the channels are independent; a side cannot fail half way
    }
    {
        const double a = std::exp(-2 * kPi * 18 / SR);
        double xl = 0, yl = 0, xr = 0, yr = 0;
        for (long long f = 0; f < n; ++f) {
            const double l = s[size_t(2 * f)], r = s[size_t(2 * f + 1)];
            yl = l - xl + a * yl;
            xl = l;
            yr = r - xr + a * yr;
            xr = r;
            s[size_t(2 * f)] = float(yl);
            s[size_t(2 * f + 1)] = float(yr);
        }
    }
    const long long fadeIn = std::min<long long>(n / 4, std::llround(0.01 * SR));
    for (long long f = 0; f < fadeIn; ++f) {
        const float g = float(smooth(double(f) / double(fadeIn)));
        s[size_t(2 * f)] *= g;
        s[size_t(2 * f + 1)] *= g;
    }
    double fadeFrom;
    if (p.endBar > 0) {
        const double hit = p.barTime(p.endBar), tail = p.length - hit;
        fadeFrom = std::max(hit + 0.4, p.length - std::min(3.0, tail * 0.8));
    } else {
        fadeFrom = p.length - std::min(2.0, p.length * 0.3);
    }
    const long long fadeStart = std::llround(fadeFrom * SR);
    for (long long f = std::max(0LL, fadeStart); f < n; ++f) {
        // Raised cosine from 1 to exactly 0 on the last sample
        const double x = double(f - fadeStart) / double(std::max(1LL, n - 1 - fadeStart));
        const float g = float(0.5 + 0.5 * std::cos(kPi * std::clamp(x, 0.0, 1.0)));
        s[size_t(2 * f)] *= g;
        s[size_t(2 * f + 1)] *= g;
    }

    // Peaks: a look-ahead limiter catches the few drum hits that would hold the bed below its loudness target (a big
    // taiko, a boom, a gated snare). Its threshold sits just under the ceiling at the level the bed will get; offline,
    // so the look-ahead is a backward pass (the gain ramps down 2 ms before a peak) and the release a forward one
    {
        LoudnessMeter meter;
        meter.addInterleaved(s.data(), int(n), 2, kRate);
        const double before = meter.integrated();
        if (before > LoudnessMeter::kSilence) {
            const float threshold = float(std::pow(10.0, (kCeilingDbtp - 1.0 - (kTargetLufs - before)) / 20));
            std::vector<float> g(size_t(n), 1.f);
            bool any = false;
            for (long long f = 0; f < n; ++f) {
                const float peak = std::max(std::abs(s[size_t(2 * f)]), std::abs(s[size_t(2 * f + 1)]));
                if (peak > threshold) g[size_t(f)] = threshold / peak, any = true;
            }
            if (any) {
                const float release = float(1 - std::exp(-1.0 / (0.08 * SR))), attack = float(1 - std::exp(-1.0 / (0.002 * SR)));
                for (long long f = 1; f < n; ++f) g[size_t(f)] = std::min(g[size_t(f)], g[size_t(f - 1)] + (1 - g[size_t(f - 1)]) * release);
                for (long long f = n - 2; f >= 0; --f) g[size_t(f)] = std::min(g[size_t(f)], g[size_t(f + 1)] + (1 - g[size_t(f + 1)]) * attack);
                for (long long f = 0; f < n; ++f) s[size_t(2 * f)] *= g[size_t(f)], s[size_t(2 * f + 1)] *= g[size_t(f)];
            }
        }
    }

    // Level: integrated loudness to the target, the true peak under the ceiling; at the same time the level and bass
    // per whole bar (before the gain, which is added to them afterwards)
    double lufs = LoudnessMeter::kSilence, tp = TruePeakMeter::kSilence;
    std::vector<double> barEnergy, barBass;
    const auto loudness = [&] {
        LoudnessMeter meter;
        meter.addInterleaved(s.data(), int(n), 2, kRate);
        lufs = meter.integrated();
    };
    const auto bars = [&] {
        barEnergy.clear();
        barBass.clear();
        Biquad lp1 = Biquad::make(Biquad::Low, 120, 0.541), lp2 = Biquad::make(Biquad::Low, 120, 1.307);
        const double barFrames = p.bar * SR;
        const int wholeBars = int(std::floor(p.length / p.bar + 1e-9));
        for (int bar = 0; bar < wholeBars; ++bar) {
            const long long from = std::llround(bar * barFrames), to = std::min(n, std::llround((bar + 1) * barFrames));
            double e = 0, bass = 0;
            for (long long f = from; f < to; ++f) {
                const float m = 0.5f * (s[size_t(2 * f)] + s[size_t(2 * f + 1)]);
                const float lo = lp2.run(lp1.run(m));
                e += double(m) * m;
                bass += double(lo) * lo;
            }
            const double count = double(std::max(1LL, to - from));
            barEnergy.push_back(dbOf(e / count));
            barBass.push_back(dbOf(bass / count));
        }
    };
    parallel(
        g_threads.load() == 1 ? 1 : 3,
        [&](int job) {
            if (job == 0) tp = truePeakDb(s, n);
            else if (job == 1) loudness();
            else bars();
        },
        [](int) {});
    if (g_threads.load() == 1) loudness(), bars();
    double gainDb = lufs > LoudnessMeter::kSilence ? kTargetLufs - lufs : 0;
    if (tp > TruePeakMeter::kSilence && tp + gainDb > kCeilingDbtp - 0.2) gainDb = kCeilingDbtp - 0.2 - tp;
    const float gain = float(std::pow(10.0, gainDb / 20));
    for (float& v : s) v *= gain;

    Result r = describe(p);
    r.lufs = lufs + gainDb;
    r.truePeakDb = tp + gainDb;
    Beats::Result& b = r.beats;
    for (double& v : barEnergy) v += gainDb;
    for (double& v : barBass) v += gainDb;
    b.barEnergyDb = std::move(barEnergy);
    b.barBassDb = std::move(barBass);
    const double usualEnergy = median(b.barEnergyDb), usualBass = median(b.barBassDb);
    for (size_t i = 0; i < p.sections.size(); ++i) {
        const Section& sec = p.sections[i];
        if (sec.kind != Section::Intro && sec.kind != Section::Break) continue;
        Beats::Break br;
        br.from = sec.fromBar * 4;
        br.to = sec.toBar * 4;
        br.bassDb = meanOf(b.barBassDb, sec.fromBar, sec.toBar) - usualBass;
        br.energyDb = meanOf(b.barEnergyDb, sec.fromBar, sec.toBar) - usualEnergy;
        br.atStart = sec.fromBar == 0;
        b.breaks.push_back(br);
        if (i + 1 < p.sections.size() && p.sections[i + 1].kind == Section::Main) {
            Beats::Drop d;
            d.beat = sec.toBar * 4;
            d.bassJumpDb = meanOf(b.barBassDb, sec.toBar, sec.toBar + 2) - meanOf(b.barBassDb, sec.toBar - 2, sec.toBar);
            d.energyJumpDb = meanOf(b.barEnergyDb, sec.toBar, sec.toBar + 2) - meanOf(b.barEnergyDb, sec.toBar - 2, sec.toBar);
            d.breakS = (sec.toBar - sec.fromBar) * p.bar;
            d.breakIndex = int(b.breaks.size()) - 1;
            d.score = std::max(0.0, d.bassJumpDb) + 0.5 * std::max(0.0, d.energyJumpDb) + 0.2 * d.breakS;
            b.drops.push_back(d);
        }
    }
    std::stable_sort(b.drops.begin(), b.drops.end(), [](const Beats::Drop& x, const Beats::Drop& y) { return x.score > y.score; });
    r.samples = std::move(s);
    return r;
}
bool writeWav(const QString& path, const Result& r)
{
    const long long frames = (long long)r.samples.size() / 2;
    const long long data = frames * 2 * 3;
    if (data > 0xFFFFFFF0LL - 36) return false;
    // Atomic (a half-written bed never lies under the final name) and in blocks (no second copy of the whole bed)
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    char head[44];
    char* b = head;
    const auto put32 = [&b](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) *b++ = char((v >> (8 * i)) & 0xFF);
    };
    const auto put16 = [&b](std::uint16_t v) {
        *b++ = char(v & 0xFF);
        *b++ = char(v >> 8);
    };
    std::memcpy(b, "RIFF", 4), b += 4;
    put32(std::uint32_t(36 + data));
    std::memcpy(b, "WAVEfmt ", 8), b += 8;
    put32(16);
    put16(1); // PCM
    put16(2);
    put32(kRate);
    put32(kRate * 2 * 3);
    put16(2 * 3);
    put16(24);
    std::memcpy(b, "data", 4), b += 4;
    put32(std::uint32_t(data));
    if (f.write(head, 44) != 44) return false;
    constexpr long long kBlock = 65536; // frames
    QByteArray block(int(kBlock * 2 * 3), Qt::Uninitialized);
    for (long long from = 0; from < frames; from += kBlock) {
        const long long n = std::min(kBlock, frames - from);
        char* o = block.data();
        for (long long i = 2 * from; i < 2 * (from + n); ++i) {
            const float x = std::clamp(r.samples[size_t(i)], -1.0f, 1.0f) * 8388607.0f;
            const std::int32_t q = std::int32_t(x >= 0 ? x + 0.5f : x - 0.5f);
            *o++ = char(q & 0xFF);
            *o++ = char((q >> 8) & 0xFF);
            *o++ = char((q >> 16) & 0xFF);
        }
        if (f.write(block.constData(), n * 6) != n * 6) {
            f.cancelWriting();
            return false;
        }
    }
    return f.commit();
}

QString fileName(const Options& o)
{
    const Plan p = makePlan(o); // cheap: no sound
    QString key = keyName(p.key).toLower().replace('#', 's').replace(' ', '-');
    return QString("music-bed-%1-%2bpm-%3-s%4-%5smp-v%6.wav")
        .arg(moodId(p.mood), QString::number(p.bpm, 'g', 6), key)
        .arg(p.seed)
        .arg(p.frames)
        .arg(kVersion);
}

} // namespace MusicBed
