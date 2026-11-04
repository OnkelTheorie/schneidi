// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SfxSynth.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

namespace SfxSynth {

namespace {

constexpr double kPi = std::numbers::pi;

struct KindInfo {
    Kind kind;
    const char* name;
    const char* description;
    double length; // default, seconds
    double peakDb;
};

// Peak levels: a whoosh or click sits under the game sound, a hit or impact may be heard a little more
constexpr KindInfo kInfo[] = {
    {Kind::Whoosh, "whoosh", "filtered noise that swells to its peak and falls away (peak on the cut)", 0.9, -10},
    {Kind::Riser, "riser", "noise and a rising tone that build up and end on the moment (a drop)", 2.0, -9},
    {Kind::Hit, "hit", "short punch: a falling low tone with a noise attack (a kill, a hit)", 0.5, -6},
    {Kind::Impact, "impact", "deep boom with a long low tail (the one big moment of a video)", 1.8, -4},
    {Kind::Click, "click", "tiny tick (a counter, a menu, a pop-up)", 0.06, -14},
    {Kind::Swish, "swish", "short, bright whoosh (a fast wipe, a title flying in)", 0.35, -12},
};

const KindInfo& info(Kind k)
{
    for (const KindInfo& i : kInfo)
        if (i.kind == k) return i;
    return kInfo[0];
}

// Deterministic noise: splitmix64 (the same numbers on every system, unlike std:: distributions)
class Rng {
public:
    explicit Rng(std::uint64_t seed) : m_state(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    std::uint64_t next()
    {
        std::uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); } // [0, 1)
    double bipolar() { return unit() * 2.0 - 1.0; }                              // [-1, 1)
    double range(double lo, double hi) { return lo + (hi - lo) * unit(); }

private:
    std::uint64_t m_state;
};

// State variable filter (topology-preserving transform form, a textbook design): low, band and high pass at once
struct Svf {
    double ic1 = 0, ic2 = 0;
    double lp = 0, bp = 0, hp = 0;
    void run(double x, double fc, double q)
    {
        fc = std::clamp(fc, 10.0, kRate * 0.45);
        const double g = std::tan(kPi * fc / kRate);
        const double k = 1.0 / std::max(0.1, q);
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
    }
};

// Exponential glide from a to b over u in [0, 1]
double glide(double a, double b, double u) { return a * std::pow(b / a, std::clamp(u, 0.0, 1.0)); }

// Smooth curve 0..1 (no corner at either end): how a sweep moves
double smooth(double u)
{
    u = std::clamp(u, 0.0, 1.0);
    return u * u * (3 - 2 * u);
}

// Slow seeded wobble in [-1, 1]: noise through two one-pole low passes at `hz` (filter drift, the air moving)
struct Wobble {
    double a = 0, b = 0, k = 0;
    explicit Wobble(double hz) : k(1 - std::exp(-2 * kPi * hz / kRate)) {}
    double next(Rng& rng)
    {
        a += k * (rng.bipolar() * 6 - a); // ~unit spread after the double smoothing
        b += k * (a - b);
        return std::clamp(b, -1.0, 1.0);
    }
};

struct SweepShape {
    double f0, f1, fEnd; // centre at the start, at the peak, at the end (Hz)
    double peakPart;     // where the peak lies (part of the length)
    double rumble;       // mix of a faint low layer (0 = none)
    double air;          // mix of the high "air" layer
};

// A whoosh or swish: air moving, not a tone. Three layered noise bands of low resonance (Q below 1: a high Q rings
// and sounds metallic), each a little early or late and higher or lower, whose centre and width move on a smooth
// curve with a slow random drift; a soft (raised cosine) swell to the peak and a longer exponential tail; a faint
// high air layer and, for a whoosh, a low rumble under it.
void sweep(std::vector<float>& out, Rng& rng, double length, const SweepShape& shape, double* anchor)
{
    const double peak = length * shape.peakPart;
    *anchor = peak;
    const double fall = std::max(0.02, length - peak);
    struct Band {
        Svf f;
        double ratio, shift, gain;
    };
    Band bands[3] = {{{}, rng.range(0.55, 0.7), rng.range(-0.06, -0.02), 0.6},
                     {{}, 1.0, 0.0, 1.0},
                     {{}, rng.range(1.5, 1.9), rng.range(0.02, 0.06), 0.45}};
    Svf air, rumble;
    Wobble drift(rng.range(3, 6)), level(rng.range(5, 9));
    const double tail = fall * rng.range(0.3, 0.4);
    const size_t n = out.size();
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / kRate;
        const double d = drift.next(rng), lv = level.next(rng);
        double v = 0;
        for (Band& band : bands) {
            const double tb = t - band.shift * length; // this band's own timing
            const double up = smooth(tb / peak), down = smooth((tb - peak) / fall);
            const double fc = (tb < peak ? shape.f0 * std::pow(shape.f1 / shape.f0, up)
                                         : shape.f1 * std::pow(shape.fEnd / shape.f1, down))
                              * band.ratio * std::pow(2.0, 0.2 * d);
            // Wider at the peak (more air rushing past), narrower at the edges; never resonant
            const double q = 0.55 + 0.25 * (tb < peak ? 1 - up : down);
            band.f.run(rng.bipolar(), fc, q);
            const double env = tb <= 0 ? 0
                               : tb < peak ? std::pow(std::sin(0.5 * kPi * tb / peak), 2.0)
                                           : std::exp(-(tb - peak) / tail);
            v += band.gain * env * band.f.bp;
        }
        const double env = t < peak ? std::pow(std::sin(0.5 * kPi * t / peak), 2.0) : std::exp(-(t - peak) / tail);
        air.run(rng.bipolar(), 7000, 0.5);
        rumble.run(rng.bipolar(), 140, 0.5);
        v += shape.air * std::pow(env, 1.5) * air.hp + shape.rumble * env * rumble.lp;
        out[i] = float(v * (1 + 0.12 * lv));
    }
}

void makeWhoosh(Sound& s, Rng& rng)
{
    const double f0 = 220 * rng.range(0.8, 1.25);
    sweep(s.samples, rng, s.length,
          {f0, 1600 * rng.range(0.75, 1.3), f0 * rng.range(1.3, 2.0), 0.6 * rng.range(0.92, 1.08), rng.range(0.6, 1.0),
           rng.range(0.08, 0.14)},
          &s.anchor);
}

void makeSwish(Sound& s, Rng& rng)
{
    const double f0 = 1100 * rng.range(0.8, 1.25);
    sweep(s.samples, rng, s.length,
          {f0, 4500 * rng.range(0.8, 1.2), f0 * rng.range(1.4, 2.0), 0.45 * rng.range(0.9, 1.1), 0.0, rng.range(0.15, 0.22)},
          &s.anchor);
}

void makeRiser(Sound& s, Rng& rng)
{
    const double L = s.length;
    s.anchor = L;
    const double f0 = 300 * rng.range(0.8, 1.2), f1 = 7000 * rng.range(0.8, 1.15);
    const double tone0 = 110 * rng.range(0.8, 1.25);
    const double octaves = rng.range(1.6, 2.4);
    const double toneMix = rng.range(0.25, 0.4);
    Svf band;
    double phase = 0, phase2 = 0;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        const double t = double(i) / kRate;
        const double u = t / L;
        band.run(rng.bipolar(), glide(f0, f1, u), 0.9); // low Q: a resonant sweep sounds metallic
        const double freq = tone0 * std::pow(2.0, octaves * u * u); // slow at first, faster towards the end
        phase += 2 * kPi * freq / kRate;
        phase2 += 2 * kPi * freq * 1.5 / kRate; // a fifth above
        const double tone = std::sin(phase) + 0.35 * std::sin(phase2);
        s.samples[i] = float(std::pow(u, 2.0) * band.bp + toneMix * std::pow(u, 1.5) * tone);
    }
}

void makeHit(Sound& s, Rng& rng)
{
    const double scale = s.length / info(Kind::Hit).length;
    const double fHigh = 180 * rng.range(0.85, 1.2), fLow = 65 * rng.range(0.85, 1.15);
    const double decay = 0.11 * scale * rng.range(0.85, 1.15);
    const double noiseMix = rng.range(0.45, 0.7);
    s.anchor = 0.002;
    Svf noiseFilter;
    double phase = 0;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        const double t = double(i) / kRate;
        const double freq = fLow + (fHigh - fLow) * std::exp(-t / (0.03 * scale));
        phase += 2 * kPi * freq / kRate;
        const double attack = std::min(1.0, t / 0.0015);
        noiseFilter.run(rng.bipolar(), 3500, 0.7);
        const double body = std::sin(phase) * std::exp(-t / decay);
        const double crack = noiseFilter.hp * std::exp(-t / (0.012 * scale)) * noiseMix;
        s.samples[i] = float(std::tanh(1.4 * attack * (body + crack)));
    }
}

void makeImpact(Sound& s, Rng& rng)
{
    const double scale = s.length / info(Kind::Impact).length;
    const double subHigh = 60 * rng.range(0.9, 1.15), subLow = 32 * rng.range(0.9, 1.1);
    const double tail = 0.45 * scale * rng.range(0.85, 1.2);
    s.anchor = 0.003;
    Svf burst, rumble;
    double sub = 0, body = 0;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        const double t = double(i) / kRate;
        const double glideTo = std::exp(-t / (0.15 * scale));
        sub += 2 * kPi * (subLow + (subHigh - subLow) * glideTo) / kRate;
        body += 2 * kPi * (60 + 60 * glideTo) / kRate;
        const double x = rng.bipolar();
        burst.run(x, 900, 0.7);
        rumble.run(x, 300, 0.7);
        const double attack = std::min(1.0, t / 0.002);
        const double v = std::sin(sub) * std::exp(-t / tail) + 0.4 * std::sin(body) * std::exp(-t / (0.12 * scale))
                         + 0.5 * burst.lp * std::exp(-t / (0.05 * scale))
                         + 0.25 * rumble.lp * std::exp(-t / (0.6 * scale));
        s.samples[i] = float(std::tanh(1.6 * attack * v));
    }
}

void makeClick(Sound& s, Rng& rng)
{
    const double scale = s.length / info(Kind::Click).length;
    const double freq = 2400 * rng.range(0.85, 1.2);
    s.anchor = 0.0005;
    Svf tick;
    for (size_t i = 0; i < s.samples.size(); ++i) {
        const double t = double(i) / kRate;
        tick.run(rng.bipolar(), 5000, 0.7);
        const double attack = std::min(1.0, t / 0.0003);
        s.samples[i] = float(attack * (std::sin(2 * kPi * freq * t) * std::exp(-t / (0.006 * scale))
                                       + 0.3 * tick.hp * std::exp(-t / (0.002 * scale))));
    }
}

} // namespace

QString kindName(Kind k) { return QString::fromLatin1(info(k).name); }

std::optional<Kind> kindFrom(const QString& name)
{
    for (const KindInfo& i : kInfo)
        if (name.compare(QLatin1String(i.name), Qt::CaseInsensitive) == 0) return i.kind;
    if (name.compare("boom", Qt::CaseInsensitive) == 0) return Kind::Impact;
    return std::nullopt;
}

QStringList kindNames()
{
    QStringList out;
    for (const KindInfo& i : kInfo) out << QString::fromLatin1(i.name);
    return out;
}

QString description(Kind k) { return QString::fromLatin1(info(k).description); }
double defaultLength(Kind k) { return info(k).length; }
double minLength(Kind k) { return k == Kind::Riser ? 0.5 : info(k).length * 0.5; }
double maxLength(Kind k) { return k == Kind::Riser ? 8.0 : info(k).length * 2.0; }
double peakLevelDb(Kind k) { return info(k).peakDb; }

Sound make(Kind kind, unsigned seed, double length)
{
    Sound s;
    s.kind = kind;
    s.seed = seed;
    s.length = std::clamp(length > 0 ? length : defaultLength(kind), minLength(kind), maxLength(kind));
    s.samples.assign(size_t(std::lround(s.length * kRate)), 0.f);
    // The seed picks the variant; the kind is mixed in so whoosh 1 and swish 1 differ in their noise too
    Rng rng(std::uint64_t(seed) * 16 + std::uint64_t(kind) + 1);
    switch (kind) {
    case Kind::Whoosh: makeWhoosh(s, rng); break;
    case Kind::Swish: makeSwish(s, rng); break;
    case Kind::Riser: makeRiser(s, rng); break;
    case Kind::Hit: makeHit(s, rng); break;
    case Kind::Impact: makeImpact(s, rng); break;
    case Kind::Click: makeClick(s, rng); break;
    }
    // Fade out the last 5 ms (no click at the end; a riser ends on the moment)
    const size_t n = s.samples.size();
    const size_t fade = std::min(n, size_t(kRate / 200));
    for (size_t i = 0; i < fade; ++i) s.samples[n - 1 - i] *= float(double(i) / double(fade));
    // Peak at the kind's level
    float peak = 0;
    for (float v : s.samples) peak = std::max(peak, std::abs(v));
    if (peak > 0) {
        const float gain = float(std::pow(10.0, peakLevelDb(kind) / 20.0) / peak);
        for (float& v : s.samples) v *= gain;
    }
    return s;
}

double anchorOf(Kind kind, unsigned seed, double length) { return make(kind, seed, length).anchor; }

double peakDb(const std::vector<float>& samples)
{
    float peak = 0;
    for (float v : samples) peak = std::max(peak, std::abs(v));
    return peak > 0 ? 20.0 * std::log10(double(peak)) : -std::numeric_limits<double>::infinity();
}

bool writeWav(const QString& path, const std::vector<float>& samples, int rate)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const auto u32 = [&](std::uint32_t v) {
        char b[4];
        qToLittleEndian(v, b);
        f.write(b, 4);
    };
    const auto u16 = [&](std::uint16_t v) {
        char b[2];
        qToLittleEndian(v, b);
        f.write(b, 2);
    };
    const std::uint32_t bytes = std::uint32_t(samples.size() * 2);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(std::uint32_t(rate));
    u32(std::uint32_t(rate) * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    QByteArray data(qsizetype(bytes), Qt::Uninitialized);
    for (size_t i = 0; i < samples.size(); ++i) {
        const long v = std::clamp(std::lround(double(samples[i]) * 32767.0), -32767L, 32767L);
        qToLittleEndian(std::int16_t(v), data.data() + i * 2);
    }
    f.write(data);
    return f.commit();
}

QString cachedFile(Kind kind, unsigned seed, double length)
{
    const Sound probe{kind, seed, std::clamp(length > 0 ? length : defaultLength(kind), minLength(kind), maxLength(kind))};
    const QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                         + QString("/sfx/%1-%2-%3-v%4.wav")
                               .arg(kindName(kind))
                               .arg(seed)
                               .arg(std::lround(probe.length * 1000))
                               .arg(kVersion);
    if (QFileInfo(path).size() > 44) return path;
    const Sound s = make(kind, seed, length);
    return writeWav(path, s.samples) ? path : QString();
}

} // namespace SfxSynth
