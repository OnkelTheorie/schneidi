// SPDX-License-Identifier: AGPL-3.0-only
// Own time stretcher (engine/audio/Stretch): the pitch stays at every speed the app allows, sounds land where the
// map puts them (no drift, also on a ramp) without pre-echo, no clicks (the output does not depend on the block size, a sine stays a
// clean sine), the channels keep their phase and level difference, onsets are found in hits but not in a tone, every
// window reads source the stretcher fetched.
// Quality against Rubber Band and the speed: tests/bench_stretch.

#include "check.h"

#include "core/Retime.h"
#include "engine/audio/Stretch.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr double kPi = std::numbers::pi;
using Signal = std::function<float(int channel, int64_t sample)>;
using Map = std::function<double(double)>;

std::vector<std::vector<float>> stretch(int channels, const Signal& sig, const Map& map, int64_t length,
                                        const std::vector<int>& blocks = {512}, int* transients = nullptr,
                                        int* refetches = nullptr)
{
    Audio::Stretch st(channels, kRate,
                      [&](int64_t from, int count, float* const* out) {
                          for (int c = 0; c < channels; ++c)
                              for (int i = 0; i < count; ++i) out[c][i] = sig(c, from + i);
                      },
                      map);
    std::vector<std::vector<float>> out(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(length)));
    std::vector<float*> p(static_cast<size_t>(channels));
    size_t k = 0;
    for (int64_t pos = 0; pos < length;) {
        const int n = int(std::min<int64_t>(blocks[k++ % blocks.size()], length - pos));
        for (int c = 0; c < channels; ++c) p[size_t(c)] = out[size_t(c)].data() + pos;
        st.read(pos, n, p.data());
        pos += n;
    }
    if (transients) *transients = st.transients();
    if (refetches) *refetches = st.refetches();
    return out;
}

Signal sine(double hz, double amp = 0.5, double phase = 0)
{
    return [=](int, int64_t i) { return float(amp * std::sin(2 * kPi * hz * double(i) / kRate + phase)); };
}

// Frequency from zero crossings over [a, b)
double frequency(const std::vector<float>& x, size_t a, size_t b)
{
    int n = 0;
    double first = -1, last = -1;
    for (size_t i = std::max<size_t>(a, 1); i < std::min(b, x.size()); ++i)
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = double(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t;
            last = t;
            ++n;
        }
    return n > 1 ? (n - 1) / (last - first) * kRate : 0;
}

// Amplitude and phase of `hz` in x[a, b) (correlation)
std::pair<double, double> tone(const std::vector<float>& x, size_t a, size_t b, double hz)
{
    double re = 0, im = 0;
    for (size_t i = a; i < b; ++i) {
        re += x[i] * std::cos(2 * kPi * hz * double(i) / kRate);
        im += x[i] * std::sin(2 * kPi * hz * double(i) / kRate);
    }
    return {2 * std::hypot(re, im) / double(b - a), std::atan2(re, im)};
}

double worstStep(const std::vector<float>& x, size_t a, size_t b)
{
    double worst = 0;
    for (size_t i = a + 1; i + 1 < b; ++i) worst = std::max(worst, double(std::abs(x[i + 1] - 2 * x[i] + x[i - 1])));
    return worst;
}

// Pitch, level and clicks of a sine at the speeds of the app's range
void testSine()
{
    for (double speed : {0.01, 0.1, 0.5, 0.75, 1.5, 2.0, 4.0, 10.0, 100.0}) {
        const int64_t length = 2 * kRate;
        const auto out = stretch(1, sine(440), [speed](double t) { return t * speed; }, length, {333, 512, 7, 4096, 1919});
        const double hz = frequency(out[0], kRate / 4, size_t(length) - kRate / 4);
        const double amp = tone(out[0], kRate / 4, size_t(length) - kRate / 4, 440).first;
        // a clean sine of 0.5 at 440 Hz: second difference 0.5 * (2 pi 440 / rate)^2
        const double step = worstStep(out[0], kRate / 4, size_t(length) - kRate / 4) / (0.5 * std::pow(2 * kPi * 440 / kRate, 2));
        std::printf("  Sinus %6.2fx: %8.3f Hz  Pegel %+.2f dB  Sprung %.2f\n", speed, hz, 20 * std::log10(amp / 0.5), step);
        CHECK(std::abs(hz / 440 - 1) < 0.001);
        CHECK(std::abs(20 * std::log10(amp / 0.5)) < 0.5);
        CHECK(step < 1.1);
    }
}

// The output does not depend on how it is read (block sizes), and a jump reads the same as a fresh start
void testBlocks()
{
    const Signal sig = [](int c, int64_t i) {
        return float(0.3 * std::sin(0.031 * double(i) + c) + 0.2 * std::sin(0.0071 * double(i) * (1 + 1e-6 * double(i))));
    };
    for (double speed : {0.6, 1.7}) {
        const Map map = [speed](double t) { return t * speed; };
        const auto a = stretch(2, sig, map, kRate, {kRate}), b = stretch(2, sig, map, kRate, {1, 4096, 333, 17, 1920});
        CHECK(a == b);
        // A fresh stretcher started at 0.5 s gives what one started there again gives (deterministic priming)
        Audio::Stretch s1(2, kRate, [&](int64_t f, int n, float* const* o) { for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) o[c][i] = sig(c, f + i); }, map);
        std::vector<float> x(4800), y(4800), z(4800), w(4800);
        float* p1[2] = {x.data(), y.data()};
        float* p2[2] = {z.data(), w.data()};
        s1.read(24000, 4800, p1);
        s1.read(100, 4800, p2); // jump back
        s1.read(24000, 4800, p2); // and forth: like the first read
        CHECK(x == z && y == w);
    }
}

// Sounds land where the map puts them: short clicks at known source times, also over a ramp 0.5x -> 2x
void testTiming()
{
    constexpr int kClicks = 20;
    auto clickAt = [](int k) { return int64_t(0.25 * kRate + k * 0.37 * kRate); };
    const Signal sig = [&](int, int64_t i) {
        for (int k = 0; k < kClicks; ++k) {
            const int64_t d = i - clickAt(k);
            if (d >= 0 && d < 96) return float(0.8 * std::sin(2 * kPi * 3000 * double(d) / kRate) * std::sin(kPi * double(d) / 96));
        }
        return 0.f;
    };
    struct Case {
        const char* name;
        Map map;
        std::function<double(double)> inverse; // source -> output
    };
    // Ramp: speed v(t) = 0.5 + 1.5 t / T over T = 4 s, then 2x; s(t) = 0.5 t + 0.75 t^2 / T
    const double T = 4.0 * kRate;
    const Map ramp = [T](double t) { return t < T ? 0.5 * t + 0.75 * t * t / T : 0.5 * T + 0.75 * T + 2 * (t - T); };
    const auto rampInv = [T](double s) {
        const double sT = 1.25 * T;
        if (s >= sT) return T + (s - sT) / 2;
        return (-0.5 + std::sqrt(0.25 + 3 * s / T)) / (1.5 / T);
    };
    const std::vector<Case> cases = {
        {"0.5x", [](double t) { return 0.5 * t; }, [](double s) { return s / 0.5; }},
        {"0.75x", [](double t) { return 0.75 * t; }, [](double s) { return s / 0.75; }},
        {"1.5x", [](double t) { return 1.5 * t; }, [](double s) { return s / 1.5; }},
        {"2x", [](double t) { return 2.0 * t; }, [](double s) { return s / 2.0; }},
        {"Rampe 0.5x-2x", ramp, rampInv},
    };
    for (const Case& c : cases) {
        const int64_t length = int64_t(c.inverse(double(clickAt(kClicks - 1) + kRate / 4)));
        int found = 0;
        const auto out = stretch(1, sig, c.map, length, {512}, &found);
        // Energy centroid around each expected place (+-20 ms)
        double worst = 0, lo = 1e9, hi = -1e9, preEcho = 0;
        int seen = 0;
        for (int k = 0; k < kClicks; ++k) {
            const double want = c.inverse(double(clickAt(k)) + 48);
            double e = 0, et = 0;
            for (int64_t i = int64_t(want) - kRate / 50; i < int64_t(want) + kRate / 50; ++i) {
                if (i < 0 || i >= length) continue;
                const double v = double(out[0][size_t(i)]) * out[0][size_t(i)];
                e += v;
                et += v * double(i);
            }
            if (e <= 0) continue;
            ++seen;
            // Pre-echo: level 25..3 ms before the click against the click (onsets cut the windows before them)
            double pre = 0;
            for (int64_t i = int64_t(want) - kRate / 40; i < int64_t(want) - 3 * kRate / 1000; ++i)
                if (i >= 0) pre += double(out[0][size_t(i)]) * out[0][size_t(i)];
            preEcho += pre / e;
            const double off = et / e - want;
            worst = std::max(worst, std::abs(off));
            lo = std::min(lo, off);
            hi = std::max(hi, off);
        }
        preEcho = 10 * std::log10(preEcho / std::max(1, seen) + 1e-20); // mean energy
        std::printf("  Klicks %-14s %2d/%d  größte Abweichung %6.1f Samples (%+.1f..%+.1f), Einsätze %d, Vorecho %.1f dB\n",
                    c.name, seen, kClicks, worst, lo, hi, found, preEcho);
        CHECK(seen == kClicks);
        CHECK(worst < 0.006 * kRate); // RB placed them within 15 ms
        CHECK(found >= kClicks / 2);
        // mean over the clicks; Rubber Band (up to 0.13) had -9..-31 dB before drum hits at these speeds (bench_stretch)
        CHECK(preEcho < -30);
    }
    // A steady tone has no onsets
    int found = -1;
    stretch(1, sine(440), [](double t) { return 0.7 * t; }, 2 * kRate, {512}, &found);
    CHECK_EQ(found, 0);
}

// Every window reads source the stretcher fetched (its cache): faster than 1x a reset frame's centre moves back
// towards its onset and its second analysis reads a hop before that. That once reached before the cache, into memory
// that was not the source: the output depended on the heap (offsets that changed from run to run, NaN that silenced
// the rest of a clip).
void testCache()
{
    const Signal sig = [](int c, int64_t i) {
        const int64_t d = i % 17713; // clicks over a quiet tone, the click at every place relative to the frame grid
        const double click = d < 96 ? 0.8 * std::sin(2 * kPi * 3000 * double(d) / kRate) * std::sin(kPi * double(d) / 96) : 0;
        return float(click + 0.05 * std::sin(0.02 * double(i) + c));
    };
    const double T = 2.0 * kRate;
    const Map ramp = [T](double t) { return t < T ? 0.5 * t + 1.75 * t * t / T : 2.25 * T + 4 * (t - T); }; // 0.5x -> 4x
    const std::vector<std::pair<const char*, Map>> cases = {
        {"0.5x", [](double t) { return 0.5 * t; }}, {"1.5x", [](double t) { return 1.5 * t; }},
        {"2x", [](double t) { return 2.0 * t; }},   {"4x", [](double t) { return 4.0 * t; }},
        {"10x", [](double t) { return 10.0 * t; }}, {"100x", [](double t) { return 100.0 * t; }},
        {"Rampe 0.5x-4x", ramp},
    };
    for (const auto& [name, map] : cases) {
        int found = 0, refetches = -1;
        const auto out = stretch(2, sig, map, 3 * kRate, {512, 1919}, &found, &refetches);
        bool finite = true;
        for (const auto& ch : out) finite = finite && std::all_of(ch.begin(), ch.end(), [](float v) { return std::isfinite(v); });
        std::printf("  Quelle %-14s Einsätze %3d, außerhalb des Zwischenspeichers %d%s\n", name, found, refetches,
                    finite ? "" : ", NICHT ENDLICH");
        CHECK(found > 0);
        CHECK_EQ(refetches, 0);
        CHECK(finite);
    }
}

// Phase and level difference between the channels stay; mono and 6 channels work
void testChannels()
{
    for (double speed : {0.5, 1.5}) {
        const Signal sig = [](int c, int64_t i) {
            if (i < 0) return 0.f;
            const double t = double(i) / kRate;
            return c == 0 ? float(0.5 * std::sin(2 * kPi * 330 * t) + 0.2 * std::sin(2 * kPi * 1250 * t))
                          : float(0.25 * std::sin(2 * kPi * 330 * t + kPi / 3) + 0.2 * std::sin(2 * kPi * 1250 * t - kPi / 2));
        };
        const auto out = stretch(2, sig, [speed](double t) { return t * speed; }, 2 * kRate, {700});
        for (const auto& [hz, ratio, phase] : {std::tuple{330.0, 0.5, kPi / 3}, std::tuple{1250.0, 1.0, -kPi / 2}}) {
            const auto l = tone(out[0], kRate / 2, 3 * kRate / 2, hz), r = tone(out[1], kRate / 2, 3 * kRate / 2, hz);
            const double dphi = std::remainder(r.second - l.second - phase, 2 * kPi);
            std::printf("  Stereo %.1fx %6.0f Hz: Pegel R/L %.3f (%.3f), Phase %+.2f° daneben\n", speed, hz, r.first / l.first,
                        ratio, dphi * 180 / kPi);
            CHECK(std::abs(r.first / l.first / ratio - 1) < 0.03);
            CHECK(std::abs(dphi) < 2 * kPi / 180);
        }
    }
    const auto six = stretch(6, [](int c, int64_t i) { return float(0.1 * (c + 1) * std::sin(0.05 * double(i))); },
                             [](double t) { return 0.8 * t; }, kRate, {1024});
    for (int c = 0; c < 6; ++c) {
        const double amp = tone(six[size_t(c)], kRate / 4, 3 * kRate / 4, 0.05 * kRate / (2 * kPi)).first;
        CHECK(std::abs(amp / (0.1 * (c + 1)) - 1) < 0.05);
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testSine();
    testBlocks();
    testTiming();
    testCache();
    testChannels();
    return Check::result();
}
