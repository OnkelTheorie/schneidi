// SPDX-License-Identifier: AGPL-3.0-only
#include "core/MusicBed.h"

#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/TruePeak.h"

#include <QByteArray>
#include <QFile>
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

enum class Timbre { PadDark, PadBright, HypeBright, Bass, BassDrive, BassHype, KeysSoft, KeysBright, PluckDark, PluckBright };

// Amplitude of harmonic k of a note at f Hz
double harmonic(Timbre t, int k, double f)
{
    const double saw = 1.0 / k, x = k * f;
    const auto lp = [x](double fc) { return 1.0 / (1.0 + std::pow(x / fc, 4)); };
    switch (t) {
    case Timbre::PadDark: return saw * lp(650);
    case Timbre::PadBright: return saw * lp(2200);
    case Timbre::HypeBright: return saw * lp(4200);
    case Timbre::Bass: {
        static const double a[] = {1, 0.4, 0.16, 0.07, 0.03};
        return k <= 5 ? a[k - 1] : 0;
    }
    case Timbre::BassDrive: return (k == 1 ? 1.4 : 0) + saw * lp(520);
    case Timbre::BassHype: return (k == 1 ? 1.6 : 0) + saw * lp(420);
    case Timbre::KeysSoft: {
        static const double a[] = {1, 0.07, 0.025};
        return k <= 3 ? a[k - 1] : 0;
    }
    case Timbre::KeysBright: {
        static const double a[] = {1, 0.55, 0.3, 0.2, 0.12, 0.08, 0.05, 0.03};
        return k <= 8 ? a[k - 1] : 0;
    }
    case Timbre::PluckDark: return saw * lp(800);
    case Timbre::PluckBright: return saw * lp(4800);
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

struct Chord {
    int offset = 0; // semitones above the tonic
    bool minor = false;
};
using Progression = std::array<Chord, 4>;

// Per mood and mode, the first one is the mood's own (seed 1); one chord per bar
const std::vector<Progression>& progressions(Mood m, bool minor)
{
    static const std::vector<Progression> calmMajor{{{{0, false}, {7, false}, {9, true}, {5, false}}},
                                                    {{{0, false}, {9, true}, {5, false}, {7, false}}},
                                                    {{{5, false}, {0, false}, {7, false}, {9, true}}}};
    static const std::vector<Progression> calmMinor{{{{0, true}, {8, false}, {3, false}, {10, false}}},
                                                    {{{0, true}, {5, true}, {8, false}, {7, true}}},
                                                    {{{8, false}, {3, false}, {10, false}, {0, true}}}};
    static const std::vector<Progression> driveMinor{{{{0, true}, {8, false}, {3, false}, {10, false}}},
                                                     {{{0, true}, {10, false}, {8, false}, {10, false}}},
                                                     {{{0, true}, {5, true}, {8, false}, {7, false}}}};
    static const std::vector<Progression> driveMajor{{{{0, false}, {7, false}, {9, true}, {5, false}}},
                                                     {{{9, true}, {5, false}, {0, false}, {7, false}}},
                                                     {{{0, false}, {5, false}, {9, true}, {7, false}}}};
    static const std::vector<Progression> hypeMinor{{{{0, true}, {8, false}, {3, false}, {10, false}}},
                                                    {{{8, false}, {10, false}, {0, true}, {0, true}}},
                                                    {{{0, true}, {8, false}, {5, true}, {10, false}}}};
    static const std::vector<Progression> hypeMajor{{{{9, true}, {5, false}, {0, false}, {7, false}}},
                                                    {{{0, false}, {7, false}, {9, true}, {5, false}}},
                                                    {{{5, false}, {7, false}, {9, true}, {0, false}}}};
    switch (m) {
    case Mood::Calm: return minor ? calmMinor : calmMajor;
    case Mood::Drive: return minor ? driveMinor : driveMajor;
    case Mood::Hype: return minor ? hypeMinor : hypeMajor;
    }
    return driveMinor;
}

const char* const kSharps[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* const kFlats[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

bool flatKey(const Key& k)
{
    const int relMajor = k.minor ? (k.root + 3) % 12 : k.root;
    return relMajor == 5 || relMajor == 10 || relMajor == 3 || relMajor == 8 || relMajor == 1;
}

QString noteName(int pc, bool flats) { return QString::fromLatin1((flats ? kFlats : kSharps)[((pc % 12) + 12) % 12]); }

// Pitch classes of a chord (tonic first) with the mood's colour: add9 / m7 for calm, the octave for the others
std::array<int, 4> chordTones(Mood m, const Key& key, const Chord& c)
{
    const int r = key.root + c.offset, third = r + (c.minor ? 3 : 4), fifth = r + 7;
    const int fourth = m == Mood::Calm ? (c.minor ? r + 10 : r + 14) : r + 12;
    return {r % 12, third % 12, fifth % 12, fourth % 12};
}

// The chord in the pad's range, with the least movement from the previous voicing
std::array<int, 4> voice(const std::array<int, 4>& pcs, const std::array<int, 4>& previous)
{
    std::array<int, 4> best{};
    int bestCost = 1 << 30;
    for (int rot = 0; rot < 4; ++rot) {
        for (int base = 52; base < 64; ++base) {
            if ((base - pcs[size_t(rot)]) % 12 != 0) continue;
            std::array<int, 4> v{};
            int note = base;
            for (int i = 0; i < 4; ++i) {
                const int pc = pcs[size_t((rot + i) % 4)];
                while ((note - pc) % 12 != 0) ++note;
                v[size_t(i)] = note;
                ++note;
            }
            if (v[3] > 76) continue;
            int cost = 0;
            for (int i = 0; i < 4; ++i) cost += std::abs(v[size_t(i)] - previous[size_t(i)]);
            if (cost < bestCost) bestCost = cost, best = v;
        }
    }
    return best;
}

// ---------- Plan: tempo, sections, chords per bar ----------

struct Plan {
    Mood mood = Mood::Drive;
    double bpm = 0, beat = 0, bar = 0, length = 0;
    long long frames = 0;
    Key key;
    unsigned seed = 1;
    int variant = 0;
    Progression prog{};
    int bars = 0;     // bars that start before the end
    int intro = 0;    // bars
    int endBar = -1;  // the final hit
    std::vector<Section> sections;
    std::vector<Section::Kind> kind; // per bar (bars + 1 entries)
    std::vector<bool> lift, fill;    // per bar: the second eight bars of a main part; the bar before a new part
    std::vector<Chord> chord;        // per bar
    std::vector<std::array<int, 4>> voicing;

    Section::Kind kindAt(int b) const { return b >= 0 && b < int(kind.size()) ? kind[size_t(b)] : Section::Ending; }
    double barTime(int b) const { return b * bar; }
};

double minTail(Mood m) { return m == Mood::Calm ? 3.5 : 2.5; }

Plan makePlan(const Options& o)
{
    Plan p;
    p.mood = o.mood;
    p.bpm = o.bpm > 0 ? std::clamp(o.bpm, kMinBpm, kMaxBpm) : defaultBpm(o.mood);
    p.beat = 60.0 / p.bpm;
    p.bar = 4 * p.beat;
    p.frames = frameCount(std::clamp(o.length, kMinLength, kMaxLength));
    p.length = double(p.frames) / SR;
    p.key = o.key ? *o.key : defaultKey(o.mood);
    p.seed = std::max(1u, o.seed);
    const std::vector<Progression>& list = progressions(p.mood, p.key.minor);
    p.variant = int((p.seed - 1) % list.size());
    p.prog = list[size_t(p.variant)];
    p.bars = int(std::ceil(p.length / p.bar - 1e-9));

    const int h = int(std::floor((p.length - minTail(p.mood)) / p.bar));
    p.endBar = h >= 2 ? h : -1;
    const int body = p.endBar > 0 ? p.endBar : p.bars;
    p.intro = p.endBar < 0 ? 0 : body >= 12 ? 4 : body >= 6 ? 2 : 0;
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

    // Chords: the progression from the first main bar on (an intro of two plays its last two chords)
    p.chord.resize(size_t(p.bars + 1));
    p.voicing.resize(size_t(p.bars + 1));
    std::array<int, 4> previous{57, 60, 64, 67};
    for (int b = 0; b <= p.bars; ++b) {
        const bool ending = p.kindAt(b) == Section::Ending;
        p.chord[size_t(b)] = ending ? Chord{0, p.key.minor} : p.prog[size_t((((b - p.intro) % 4) + 4) % 4)];
        previous = voice(chordTones(p.mood, p.key, p.chord[size_t(b)]), previous);
        p.voicing[size_t(b)] = previous;
    }
    return p;
}

// ---------- Rendering ----------

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
    Brightness br;
    br.bar = p.bar;
    br.target.assign(size_t(p.bars + 1), 0.5);
    br.ramp.assign(size_t(p.bars + 1), 0.2);
    for (int b = 0; b <= p.bars; ++b) {
        const Section::Kind k = p.kindAt(b);
        double& t = br.target[size_t(b)];
        double& r = br.ramp[size_t(b)];
        const bool lastOfPart = p.fill[size_t(b)];
        switch (p.mood) {
        case Mood::Calm:
            t = 0.45 + 0.15 * ((b / 4) % 2);
            r = 1.0;
            break;
        case Mood::Drive:
            t = k == Section::Intro ? 0.25 + 0.35 * (b + 1) / std::max(1, p.intro)
                : k == Section::Break ? 0.3
                : k == Section::Ending ? 0.55
                                       : (p.lift[size_t(b)] ? 0.8 : 0.65);
            r = k == Section::Intro ? 1.0 : 0.3;
            break;
        case Mood::Hype:
            if (k == Section::Intro) t = 0.05 + 0.4 * (b + 1) / std::max(1, p.intro), r = 1.0;
            else if (k == Section::Break) t = 0.12, r = 0.25;
            else if (k == Section::Ending) t = 0.8, r = 0.2;
            else t = 1.0, r = 0.1;
            if (lastOfPart && k != Section::Main) t = 0.9, r = 1.0; // the riser bar opens the filter
            break;
        }
    }
    return br;
}

// What the voices of one thread share (the tables and controls are the same for all threads)
struct Context {
    const Plan& p;
    Bus bus;
    Tables& tables;
    const Control& bright; // 0..1 (Brightness)
    const Control& duck;   // side-chain gain (Duck)
    bool ducking = false;
    Noise noise;
    Context(const Plan& plan, const Bus& b, Tables& t, const Control& br, const Control& d, bool ducks)
        : p(plan), bus(b), tables(t), bright(br), duck(d), ducking(ducks), noise(plan.seed * 7919u + 17u)
    {
    }
};

// ---------- Voices ----------

// Pad: per chord two detuned oscillators per note (left/right), a dark and a bright table blended by the brightness
void pad(Context& c, double t0, double gate, const std::array<int, 4>& notes, const Env& e, double gain, int rootMidi)
{
    const Plan& p = c.p;
    const bool hype = p.mood == Mood::Hype;
    const Timbre brightT = hype ? Timbre::HypeBright : Timbre::PadBright;
    const double detune = p.mood == Mood::Calm ? 6 : hype ? 11 : 8;
    const double send = p.mood == Mood::Calm ? 0.55 : hype ? 0.35 : 0.3;
    const Control* duck = c.ducking ? &c.duck : nullptr;
    for (int i = 0; i < 4; ++i) {
        const int midi = notes[size_t(i)];
        const Table& dark = c.tables.get(Timbre::PadDark, midi);
        const Table& bright = c.tables.get(brightT, midi);
        const double f = midiHz(midi);
        // A centre voice in both channels and a detuned one on each side, lower: the sides give the width, the centre
        // keeps the mono sum steady (two equal detuned voices cancel each other once per beating period)
        Osc m{c.noise.unit(), f / SR}, l{c.noise.unit(), f * cents(-detune) / SR}, r{c.noise.unit(), f * cents(detune) / SR};
        const Control& br = c.bright;
        note(c.bus, t0, gate, e, gain, 0, send, duck, [&](long long, long long frame) {
            const float b = br.at(frame);
            const float vm = 0.72f * lookup2(dark, bright, m.phase, b);
            const float vl = vm + 0.48f * lookup2(dark, bright, l.phase, b), vr = vm + 0.48f * lookup2(dark, bright, r.phase, b);
            for (Osc* o : {&m, &l, &r}) {
                o->phase += o->inc;
                if (o->phase >= 1) o->phase -= 1;
            }
            return std::pair<float, float>{vl, vr};
        });
    }
    if (rootMidi > 0) { // the chord's root below the voicing, warm and centred
        const int root = rootMidi;
        const Table& dark = c.tables.get(Timbre::PadDark, root);
        Osc o{c.noise.unit(), midiHz(root) / SR};
        note(c.bus, t0, gate, e, gain * 0.5, 0, send * 0.5, duck, [&](long long, long long) {
            const float v = o.next(dark);
            return std::pair<float, float>{v, v};
        });
    }
}

void bassNote(Context& c, double t0, double gate, int midi, const Env& e, double gain, Timbre t)
{
    const Table& table = c.tables.get(t, midi);
    Osc o{0, midiHz(midi) / SR};
    const Control* duck = c.p.mood == Mood::Hype ? &c.duck : nullptr;
    note(c.bus, t0, gate, e, gain, 0, 0.0, duck, [&](long long, long long) {
        const float v = o.next(table);
        return std::pair<float, float>{v, v};
    });
}

// A plucked or struck tone: bright at the attack, darker as it rings (keys, plucks, the arp)
void pluck(Context& c, double t0, double gate, int midi, const Env& e, double gain, double pan, double send,
           Timbre darkT, Timbre brightT, double brightDecay, double brightness, bool followBrightness)
{
    const Table& dark = c.tables.get(darkT, midi);
    const Table& bright = c.tables.get(brightT, midi);
    Osc o{0, midiHz(midi) / SR};
    const double k = std::exp(-1.0 / (brightDecay * SR));
    double env = brightness;
    const Control& br = c.bright;
    note(c.bus, t0, gate, e, gain, pan, send, nullptr, [&](long long, long long frame) {
        const float b = followBrightness ? float(env) * (0.15f + 0.85f * br.at(frame)) : float(env);
        const float v = lookup2(dark, bright, o.phase, b);
        o.phase += o.inc;
        if (o.phase >= 1) o.phase -= 1;
        env *= k;
        return std::pair<float, float>{v, v};
    });
}

// ---------- Drums ----------

void kick(Context& c, double t0, double vel)
{
    const Mood m = c.p.mood;
    const double fEnd = m == Mood::Calm ? 54 : m == Mood::Drive ? 50 : 47;
    const double fSweep = m == Mood::Calm ? 55 : m == Mood::Drive ? 85 : 115;
    const double pitchTau = m == Mood::Calm ? 0.045 : m == Mood::Drive ? 0.035 : 0.03;
    const double decay = m == Mood::Calm ? 0.22 : m == Mood::Drive ? 0.26 : 0.32;
    const double clickLevel = m == Mood::Calm ? 0.0 : 0.06;
    const Table& sine = sineTable();
    double phase = 0;
    Biquad click = Biquad::make(Biquad::Low, 2500, 0.7);
    Noise n(std::uint32_t(t0 * 1000) + 3);
    Env e{0.0015, decay, 0.0, 0.03};
    const double kSweep = std::exp(-1.0 / (pitchTau * SR)), kClick = std::exp(-1.0 / (0.002 * SR));
    double sweep = fSweep, clickEnv = clickLevel;
    note(c.bus, t0, decay * 3.2, e, vel, 0, 0.02, nullptr, [&](long long i, long long) {
        float v = lookup(sine, phase);
        phase += (fEnd + sweep) / SR;
        if (phase >= 1) phase -= 1;
        sweep *= kSweep;
        if (clickLevel > 0 && i < 480) {
            v += float(clickEnv) * click.run(n.next());
            clickEnv *= kClick;
        }
        return std::pair<float, float>{v, v};
    });
}

void snare(Context& c, double t0, double vel)
{
    const bool calm = c.p.mood == Mood::Calm;
    Biquad band = Biquad::make(Biquad::Band, calm ? 2600 : 1900, 0.8), high = Biquad::make(Biquad::High, 500, 0.7);
    Noise n(std::uint32_t(t0 * 977) + 11);
    const Table& sine = sineTable();
    double phase = 0;
    const double toneTau = 0.05, noiseTau = calm ? 0.09 : 0.13;
    Env e{0.001, 0, 1, 0.03};
    const double kTone = std::exp(-1.0 / (toneTau * SR)), kNoise = std::exp(-1.0 / (noiseTau * SR));
    double toneEnv = calm ? 0.25 : 0.5, noiseEnv = 2.2;
    note(c.bus, t0, noiseTau * 3.5, e, vel, -0.05, calm ? 0.6 : 0.3, nullptr, [&](long long, long long) {
        const float tone = lookup(sine, phase) * float(toneEnv);
        phase += 185.0 / SR;
        if (phase >= 1) phase -= 1;
        const float noise = high.run(band.run(n.next())) * float(noiseEnv);
        toneEnv *= kTone;
        noiseEnv *= kNoise;
        const float v = tone + noise;
        return std::pair<float, float>{v, v};
    });
}

void clap(Context& c, double t0, double vel)
{
    Biquad band = Biquad::make(Biquad::Band, 1150, 1.1);
    Noise n(std::uint32_t(t0 * 733) + 29);
    Env e{0.0008, 0, 1, 0.03};
    // Three quick bursts (9 ms apart, 4 ms each) and a tail from 22 ms on
    const double kBurst = std::exp(-1.0 / (0.004 * SR)), kTail = std::exp(-1.0 / (0.11 * SR));
    const long long b1 = std::llround(0.009 * SR), b2 = std::llround(0.018 * SR), tail = std::llround(0.022 * SR);
    double burst = 0, tailEnv = 0.7;
    note(c.bus, t0, 0.42, e, vel, 0.08, 0.4, nullptr, [&](long long i, long long) {
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

void hat(Context& c, double t0, double vel, bool open)
{
    Biquad high = Biquad::make(Biquad::High, 7200, 0.7), band = Biquad::make(Biquad::Band, 10500, 0.9);
    Noise n(std::uint32_t(t0 * 1543) + 41);
    const double tau = open ? 0.2 : 0.032;
    Env e{0.0007, tau, 0.0, 0.02};
    note(c.bus, t0, tau * 4, e, vel, 0.25, 0.06, nullptr, [&](long long, long long) {
        const float x = n.next();
        const float v = 0.9f * high.run(x) + 0.6f * band.run(x);
        return std::pair<float, float>{v, v};
    });
}

void shaker(Context& c, double t0, double vel)
{
    Biquad band = Biquad::make(Biquad::Band, 5600, 1.4);
    Noise n(std::uint32_t(t0 * 1201) + 53);
    Env e{0.012, 0.05, 0.0, 0.03};
    note(c.bus, t0, 0.14, e, vel, -0.3, 0.15, nullptr, [&](long long, long long) {
        const float v = 1.8f * band.run(n.next());
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

// ---------- Parts ----------

// The chord's root in 40..51 (E2 .. D#3), under the pad's voicing
int rootMidi(const Plan& p, const Chord& ch)
{
    const int pc = (p.key.root + ch.offset) % 12;
    return 40 + ((pc - 40) % 12 + 12) % 12;
}

void renderPads(Context& c)
{
    const Plan& p = c.p;
    Env e;
    switch (p.mood) {
    case Mood::Calm: e = {0.9, 0, 1, 1.4}; break;
    case Mood::Drive: e = {0.25, 0, 1, 0.6}; break;
    case Mood::Hype: e = {0.04, 0, 1, 0.3}; break;
    }
    const double gain = p.mood == Mood::Calm ? 0.085 : p.mood == Mood::Drive ? 0.09 : 0.11;
    const int last = p.endBar > 0 ? p.endBar : p.bars;
    for (int b = 0; b < last; ++b) {
        const Section::Kind k = p.kindAt(b);
        const double g = gain * (k == Section::Intro ? 0.85 : k == Section::Break && p.mood == Mood::Hype ? 1.15 : 1.0);
        const double gate = p.mood == Mood::Hype ? p.bar - 0.02 : p.bar;
        pad(c, p.barTime(b), gate, p.voicing[size_t(b)], e, g, p.mood != Mood::Hype ? rootMidi(p, p.chord[size_t(b)]) : -1);
    }
    if (p.endBar > 0) { // the final chord rings out to the end
        Env end{p.mood == Mood::Calm ? 0.2 : 0.02, p.mood == Mood::Calm ? 4.0 : 2.5, 0.35, 0.01};
        pad(c, p.barTime(p.endBar), p.length - p.barTime(p.endBar), p.voicing[size_t(p.endBar)], end, gain * 1.05,
            rootMidi(p, p.chord[size_t(p.endBar)]));
    }
}

int bassMidi(const Plan& p, const Chord& ch)
{
    const int pc = (p.key.root + ch.offset) % 12;
    const int low = p.mood == Mood::Hype ? 33 : 36;
    int m = low + ((pc - low) % 12 + 12) % 12;
    return m;
}

void renderBass(Context& c)
{
    const Plan& p = c.p;
    const int last = p.endBar > 0 ? p.endBar : p.bars;
    const double eighth = p.beat / 2;
    for (int b = 0; b < last; ++b) {
        const Section::Kind k = p.kindAt(b);
        const int m = bassMidi(p, p.chord[size_t(b)]);
        const double t0 = p.barTime(b);
        switch (p.mood) {
        case Mood::Calm:
            if (k == Section::Intro && b < p.intro / 2) break;
            bassNote(c, t0, p.bar - 0.04, m, {0.06, 3.0, 0.6, 0.3}, 0.25, Timbre::Bass);
            break;
        case Mood::Drive:
            if (k == Section::Intro) break;
            if (k == Section::Break) {
                bassNote(c, t0, p.bar - 0.05, m, {0.03, 2.0, 0.5, 0.2}, 0.32, Timbre::BassDrive);
                break;
            }
            for (int i = 0; i < 8; ++i) {
                const bool up = i == 7;
                bassNote(c, t0 + i * eighth, eighth * 0.82, up ? m + 12 : m, {0.004, 0.22, 0.6, 0.03},
                         (i % 2 == 0 ? 0.34 : 0.27) * (up ? 0.8 : 1.0), Timbre::BassDrive);
            }
            break;
        case Mood::Hype:
            if (k != Section::Main) break;
            for (int i = 0; i < 4; ++i)
                bassNote(c, t0 + (i + 0.5) * p.beat, p.beat * 0.42, m, {0.003, 0.18, 0.55, 0.04}, 0.4, Timbre::BassHype);
            break;
        }
    }
    if (p.endBar > 0) {
        const int m = bassMidi(p, p.chord[size_t(p.endBar)]);
        bassNote(c, p.barTime(p.endBar), p.length - p.barTime(p.endBar), m, {0.005, 0.9, 0.0, 0.01}, 0.45,
                 p.mood == Mood::Calm ? Timbre::Bass : p.mood == Mood::Drive ? Timbre::BassDrive : Timbre::BassHype);
    }
}

// Calm: soft keys breaking the chord on the beats (a pick-up in lift bars); drive: plucked stabs; hype: a 16th arp
// with two echoes (dotted eighths, the other side)
void renderMelodic(Context& c)
{
    const Plan& p = c.p;
    const int last = p.endBar > 0 ? p.endBar : p.bars;
    Noise vel(p.seed * 31 + 5);
    if (p.mood == Mood::Calm) {
        static const int patterns[][5] = {{0, 2, 3, 1, -1}, {0, 3, 2, 4, -1}, {3, 2, 0, 1, -1}};
        const int* pat = patterns[p.variant % 3];
        for (int b = 0; b < last; ++b) {
            std::array<int, 4> v = p.voicing[size_t(b)];
            int tones[6] = {v[0] + 12, v[1] + 12, v[2] + 12, v[3] + 12, v[0] + 24, v[1] + 24};
            for (int i = 0; i < 4; ++i) {
                const int idx = pat[i];
                const double t = p.barTime(b) + i * p.beat;
                const double g = 0.24 * (i == 0 ? 1.0 : 0.8) * (0.92 + 0.08 * vel.unit());
                pluck(c, t, p.beat * 1.6, tones[idx], {0.004, 0.9, 0.0, 0.25}, g, (i % 2 ? 0.25 : -0.25), 0.5,
                      Timbre::KeysSoft, Timbre::KeysBright, 0.5, 1.0, false);
            }
            if (p.lift[size_t(b)])
                pluck(c, p.barTime(b) + 3.5 * p.beat, p.beat * 0.8, tones[4], {0.004, 0.6, 0.0, 0.2}, 0.1, 0.3,
                      0.5, Timbre::KeysSoft, Timbre::KeysBright, 0.4, 0.9, false);
        }
        if (p.endBar > 0) { // the final chord, rolled
            const std::array<int, 4> v = p.voicing[size_t(p.endBar)];
            for (int i = 0; i < 4; ++i)
                pluck(c, p.barTime(p.endBar) + i * 0.045, p.length - p.barTime(p.endBar), v[size_t(i)],
                      {0.004, 1.6, 0.0, 0.01}, 0.09, i % 2 ? 0.2 : -0.2, 0.55, Timbre::KeysSoft, Timbre::KeysBright, 0.3,
                      0.8, false);
        }
    } else if (p.mood == Mood::Drive) {
        static const double hits[][5] = {{0, 1.5, 3, -1, -1}, {0, 0.75, 1.5, 2.5, 3}, {0.5, 1.5, 2.5, 3.5, -1}};
        for (int b = 0; b < last; ++b) {
            if (p.kindAt(b) == Section::Intro && b < p.intro / 2) continue;
            const double* pat = hits[p.lift[size_t(b)] ? 1 : (p.variant == 2 ? 2 : 0)];
            const std::array<int, 4> v = p.voicing[size_t(b)];
            for (int i = 0; i < 5 && pat[i] >= 0; ++i)
                for (int n = 1; n < 4; ++n)
                    pluck(c, p.barTime(b) + pat[i] * p.beat, 0.22, v[size_t(n)], {0.002, 0.16, 0.0, 0.05},
                          0.07 * (0.9 + 0.1 * vel.unit()), n == 1 ? -0.35 : n == 3 ? 0.35 : 0, 0.25, Timbre::PluckDark,
                          Timbre::PluckBright, 0.07, 1.0, true);
        }
    } else {
        static const int patterns[][16] = {{0, 1, 2, 3, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 4, 3},
                                           {0, 2, 4, 2, 1, 3, 5, 3, 0, 2, 4, 6, 4, 2, 1, 2},
                                           {4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1}};
        const int* pat = patterns[p.variant % 3];
        const double sixteenth = p.beat / 4, echo = 0.75 * p.beat;
        for (int b = 0; b < last; ++b) {
            const std::array<int, 4> v = p.voicing[size_t(b)];
            const int up = p.lift[size_t(b)] ? 12 : 0;
            const int tones[7] = {v[0] + up, v[1] + up, v[2] + up, v[3] + up, v[1] + 12 + up, v[2] + 12 + up, v[3] + 12 + up};
            for (int i = 0; i < 16; ++i) {
                const double t = p.barTime(b) + i * sixteenth;
                const int m = tones[pat[i]];
                const double g = 0.07 * (i % 4 == 0 ? 1.0 : 0.78);
                const double pan = (i % 2 ? 0.15 : -0.15);
                const Env e{0.002, 0.09, 0.0, 0.04};
                pluck(c, t, 0.12, m, e, g, pan, 0.2, Timbre::PluckDark, Timbre::PluckBright, 0.05, 1.0, true);
                pluck(c, t + echo, 0.12, m, e, g * 0.32, -pan * 4, 0.3, Timbre::PluckDark, Timbre::PluckBright, 0.04, 0.5, true);
                pluck(c, t + 2 * echo, 0.12, m, e, g * 0.13, pan * 4, 0.3, Timbre::PluckDark, Timbre::PluckBright, 0.04, 0.35, true);
            }
        }
    }
}

void renderDrums(Context& c)
{
    const Plan& p = c.p;
    const int last = p.endBar > 0 ? p.endBar : p.bars;
    Noise vel(p.seed * 13 + 1);
    const auto human = [&](double v) { return v * (0.93 + 0.07 * vel.unit()); };
    for (int b = 0; b < last; ++b) {
        const Section::Kind k = p.kindAt(b);
        const double t0 = p.barTime(b);
        const bool lift = p.lift[size_t(b)], fill = p.fill[size_t(b)] && b + 1 < p.bars;
        const auto at = [&](double beats) { return t0 + beats * p.beat; };
        const bool newPart = b > 0 && p.kindAt(b - 1) != k && k == Section::Main;
        switch (p.mood) {
        case Mood::Calm:
            if (k == Section::Intro || k == Section::Break) break;
            kick(c, at(0), human(0.7));
            kick(c, at(2), human(0.55));
            if (lift) kick(c, at(2.5), human(0.3));
            snare(c, at(1), human(0.3));
            snare(c, at(3), human(0.32));
            for (int i = 0; i < 8; ++i) shaker(c, at(i * 0.5), human(i % 2 ? 0.06 : 0.1));
            break;
        case Mood::Drive:
            if (newPart) crash(c, at(0), 0.07);
            if (k == Section::Main) {
                kick(c, at(0), human(0.75));
                kick(c, at(2), human(0.7));
                kick(c, at(2.5), human(0.5));
                if (lift) kick(c, at(3.75), human(0.4));
                if (fill) {
                    snare(c, at(1), human(0.32));
                    for (int i = 0; i < 8; ++i) snare(c, at(2 + i * 0.25), 0.12 + 0.22 * i / 7.0);
                } else {
                    snare(c, at(1), human(0.42));
                    snare(c, at(3), human(0.42));
                }
            }
            if (k == Section::Main || k == Section::Break || (k == Section::Intro && b >= p.intro / 2))
                for (int i = 0; i < 8; ++i) hat(c, at(i * 0.5), human(i % 2 ? 0.075 : 0.11), lift && i == 7);
            if (fill && k != Section::Main) riser(c, t0, p.bar, 0.035);
            break;
        case Mood::Hype:
            if (newPart) crash(c, at(0), 0.09);
            if (k == Section::Main) {
                for (int i = 0; i < 4; ++i) kick(c, at(i), human(0.85));
                clap(c, at(1), human(0.2));
                clap(c, at(3), human(0.2));
                snare(c, at(1), human(0.1));
                snare(c, at(3), human(0.1));
                for (int i = 0; i < 4; ++i) hat(c, at(i + 0.5), human(0.06), true);
                for (int i = 0; i < 16; ++i)
                    if (i % 2 == 1 || lift) hat(c, at(i * 0.25), human(i % 4 == 2 ? 0.045 : 0.028), false);
                if (fill)
                    for (int i = 0; i < 8; ++i) snare(c, at(2 + i * 0.25), 0.06 + 0.16 * i / 7.0);
            } else if (k == Section::Intro) {
                for (int i = 0; i < 8; ++i) hat(c, at(i * 0.5), human(i % 2 ? 0.035 : 0.022), false);
            }
            if (fill) riser(c, t0, p.bar, k == Section::Main ? 0.03 : 0.05);
            break;
        }
    }
    if (p.endBar > 0) { // the final hit
        const double t = p.barTime(p.endBar);
        kick(c, t, p.mood == Mood::Calm ? 0.55 : 0.85);
        if (p.mood != Mood::Calm) crash(c, t, p.mood == Mood::Hype ? 0.1 : 0.08);
        if (p.mood == Mood::Hype) clap(c, t, 0.18);
    }
}

// Small reverb (four damped feedback combs and two all-passes per side, own delays), mono send -> stereo
// Runs at half the rate (the send averaged in pairs, the reverb linearly interpolated back): a reverb has little above
// 10 kHz, and it takes half the time
void reverb(float* out, const float* send, long long frames, Mood m)
{
    const double wet = m == Mood::Calm ? 0.42 : m == Mood::Drive ? 0.3 : 0.32;
    const float fb = m == Mood::Calm ? 0.84f : 0.8f, damp = 0.3f;
    struct Comb {
        std::vector<float> buf;
        size_t pos = 0;
        float filt = 0;
    };
    struct AllPass {
        std::vector<float> buf;
        size_t pos = 0;
    };
    const int combL[] = {843, 801, 933, 889}, apL[] = {281, 121}; // at 24 kHz
    const int spread = 19;
    const long long pre = std::llround(0.018 * SR / 2), half = frames / 2 + 1;
    const float in = 0.06f, outGain = float(wet);
    // The two sides are independent: one thread each
    const auto side = [&](int ch) {
        Comb combs[4];
        AllPass aps[2];
        for (int i = 0; i < 4; ++i) combs[i].buf.assign(size_t(combL[i] + ch * spread), 0.f);
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
    std::thread right(side, 1);
    side(0);
    right.join();
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
    const int parts = int(std::clamp<long long>(frames / (kRate * 20), 1, 4));
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
    std::vector<std::thread> threads;
    for (int i = 1; i < parts; ++i) threads.emplace_back(work, i);
    work(0);
    for (std::thread& t : threads) t.join();
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
    case Mood::Drive: return QStringLiteral("drive");
    case Mood::Hype: return QStringLiteral("hype");
    }
    return {};
}

std::optional<Mood> moodFrom(const QString& s)
{
    const QString t = s.trimmed().toLower();
    if (t == "calm" || t == "ruhig") return Mood::Calm;
    if (t == "drive" || t == "driving" || t == "treibend") return Mood::Drive;
    if (t == "hype") return Mood::Hype;
    return std::nullopt;
}

QStringList moodIds() { return {"calm", "drive", "hype"}; }

const char* moodLabel(Mood m)
{
    switch (m) {
    case Mood::Calm: return N_("Ruhig");
    case Mood::Drive: return N_("Treibend");
    case Mood::Hype: return N_("Hype");
    }
    return "";
}

QString description(Mood m)
{
    switch (m) {
    case Mood::Calm:
        return "calm: warm pad, soft keys breaking the chords, round bass, a gentle half-time groove with a shaker "
               "(vlog, tutorial, product, under speech)";
    case Mood::Drive:
        return "drive: pad, pulsing eighth-note bass, plucked chord stabs, a pop/rock beat with hi-hats and fills "
               "(travel, sport, montage, ads)";
    case Mood::Hype:
        return "hype: pumping saw pad, off-beat bass, a 16th arp with echoes, four on the floor, risers into the drops "
               "(gaming highlights, shorts, hype edits)";
    }
    return {};
}

double defaultBpm(Mood m) { return m == Mood::Calm ? 84 : m == Mood::Drive ? 112 : 128; }

Key defaultKey(Mood m)
{
    switch (m) {
    case Mood::Calm: return {5, false}; // F major
    case Mood::Drive: return {9, true}; // A minor
    case Mood::Hype: return {4, true};  // E minor
    }
    return {};
}

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
    const bool flats = flatKey(p.key);
    for (const Chord& ch : p.prog) r.chords << noteName(p.key.root + ch.offset, flats) + (ch.minor ? "m" : "");
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
    b.phraseBars = 4;
    b.phraseOffset = p.intro % 4;
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
    const double depth = p.mood == Mood::Hype ? 0.55 : p.mood == Mood::Drive ? 0.18 : 0.0;
    {
        const Brightness br = brightnessOf(p);
        bright.build(n, [&](long long f) { return br.at(double(f) / SR); });
        Duck duck;
        duck.setup(depth, p.beat, p.mood == Mood::Hype ? 0.09 : 0.07);
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
        renderMelodic(c);
        renderDrums(c);
    };
    const int threads = int(std::clamp<long long>(n / (kRate * 8), 1, std::clamp(int(std::thread::hardware_concurrency()), 1, 8)));
    {
        std::vector<std::thread> pool;
        for (int t = 1; t < threads; ++t) pool.emplace_back(render, n * t / threads, n * (t + 1) / threads);
        render(0, n / threads);
        for (std::thread& t : pool) t.join();
    }
    reverb(out.data(), send.data(), n, p.mood);
    send = {};

    // Master: DC blocker (one pole at ~18 Hz), a short fade-in, the end faded to silence at the exact length
    std::vector<float>& s = out;
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

    // Level: integrated loudness to the target, the true peak under the ceiling; at the same time the level and bass
    // per whole bar (before the gain, which is added to them afterwards)
    double lufs = LoudnessMeter::kSilence;
    std::thread loudness([&] {
        LoudnessMeter meter;
        meter.addInterleaved(s.data(), int(n), 2, kRate);
        lufs = meter.integrated();
    });
    std::vector<double> barEnergy, barBass;
    std::thread bars([&] {
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
    });
    const double tp = truePeakDb(s, n);
    loudness.join();
    bars.join();
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
    QByteArray bytes(int(44 + data), Qt::Uninitialized);
    char* b = bytes.data();
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
    for (float v : r.samples) {
        const float x = std::clamp(v, -1.0f, 1.0f) * 8388607.0f;
        const std::int32_t q = std::int32_t(x >= 0 ? x + 0.5f : x - 0.5f);
        *b++ = char(q & 0xFF);
        *b++ = char((q >> 8) & 0xFF);
        *b++ = char((q >> 16) & 0xFF);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(bytes) == bytes.size() && f.flush();
}

QString fileName(const Options& o)
{
    const Plan p = makePlan(o); // cheap: no sound
    QString key = keyName(p.key).toLower().replace('#', 's').replace(' ', '-');
    return QString("music-bed-%1-%2bpm-%3-s%4-%5ms-v%6.wav")
        .arg(moodId(p.mood), QString::number(p.bpm, 'g', 6), key)
        .arg(p.seed)
        .arg(std::llround(p.length * 1000))
        .arg(kVersion);
}

} // namespace MusicBed
