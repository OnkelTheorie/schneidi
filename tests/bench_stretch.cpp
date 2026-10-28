// SPDX-License-Identifier: AGPL-3.0-only
// Quality and speed of the own time stretcher (engine/audio/Stretch), and of Rubber Band when it is installed
// (only here, never in the app: tests/CMakeLists.txt finds it with pkg-config and defines SCHNEIDI_HAVE_RUBBERBAND).
//
// Generated material with a known ideal result (the generator run on a stretched time axis: notes, syllables and hits
// at scaled times, same frequencies): sine, music (chords with vibrato and bass, panned), a speech-like voice
// (glottal pulses through moving formants, syllables and pauses), a drum loop (kick, snare, hi-hat).
// Per speed 0.5, 0.75, 1.5, 2:
//   LSD       log-spectral distance to the ideal (dB, STFT 2048/512, frames with sound) - lower is better
//   pitch     sine 440 Hz: FFT peak of the middle of the output
//   click     sine: largest second difference over the one of a clean sine (1.0 = no click)
//   attack    drums: 1-ms peak level in the first 15 ms of each hit, against the ideal (dB, 0 = as sharp)
//   pre-echo  drums: level in the 25 ms before each hit against the hit (dB, ideal in brackets) - lower is better
//   length    output samples against the requested length
//   us/block  time per 512-sample stereo block (one core)
// Environment: STRETCH_WINDOW=<n> other FFT size for the own stretcher, STRETCH_OWN_ONLY=1 without Rubber Band.
// bench_stretch <file> [seconds]: real material (e.g. speech) - stretched by s and back by 1/s, LSD to the original
// (round trip, both stretchers the same way).

#include "engine/audio/Reader.h"
#include "engine/audio/Stretch.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <functional>
#include <numbers>
#include <random>
#include <vector>

#if SCHNEIDI_HAVE_RUBBERBAND
#include <rubberband/RubberBandStretcher.h>
#endif

namespace {

constexpr int kRate = 48000;
constexpr double kPi = std::numbers::pi;
using Stereo = std::vector<std::vector<float>>; // [channel][sample]

// ---- Generators: g(scale) renders the material on a time axis stretched by `scale` (1 = original) ----

Stereo make(double seconds) { return Stereo(2, std::vector<float>(size_t(seconds * kRate), 0.f)); }

Stereo sine(double scale, double seconds)
{
    Stereo s = make(seconds * scale);
    for (size_t i = 0; i < s[0].size(); ++i) s[0][i] = s[1][i] = float(0.5 * std::sin(2 * kPi * 440 * double(i) / kRate));
    return s;
}

Stereo music(double scale, double seconds)
{
    Stereo s = make(seconds * scale);
    const double chords[4][3] = {{261.6, 329.6, 392.0}, {220.0, 261.6, 329.6}, {174.6, 220.0, 261.6}, {196.0, 246.9, 293.7}};
    const double bass[4] = {65.4, 55.0, 87.3, 98.0};
    std::vector<double> ph(4 * 8, 0.0);
    for (size_t i = 0; i < s[0].size(); ++i) {
        const double t = double(i) / kRate / scale; // original time
        const int bar = int(t) % 4;
        const double vib = 1 + 0.004 * std::sin(2 * kPi * 5.5 * t);
        const double env = std::min(1.0, (t - std::floor(t)) * 50) * std::exp(-1.2 * (t - std::floor(t)));
        double l = 0, r = 0;
        for (int v = 0; v < 4; ++v) {
            const double f0 = v < 3 ? chords[bar][v] * vib : bass[bar];
            for (int h = 1; h <= 8; ++h) {
                double& p = ph[size_t(v * 8 + h - 1)];
                p += 2 * kPi * f0 * h / kRate;
                if (f0 * h > 20000) continue;
                const double a = 0.08 / h * env;
                const double pan = v == 3 ? 0.5 : 0.2 + 0.3 * v;
                l += a * (1 - pan) * std::sin(p);
                r += a * pan * std::sin(p + 0.3 * v);
            }
        }
        s[0][i] = float(l);
        s[1][i] = float(r);
    }
    return s;
}

// Resonator (two-pole) for the formants
struct Reso {
    double y1 = 0, y2 = 0;
    double run(double x, double f, double bw)
    {
        const double rr = std::exp(-kPi * bw / kRate), c = 2 * rr * std::cos(2 * kPi * f / kRate);
        const double y = (1 - rr) * x + c * y1 - rr * rr * y2;
        y2 = y1;
        y1 = y;
        return y;
    }
};

Stereo speech(double scale, double seconds)
{
    Stereo s = make(seconds * scale);
    const double vowels[5][3] = {{730, 1090, 2440}, {270, 2290, 3010}, {300, 870, 2240}, {530, 1840, 2480}, {570, 840, 2410}};
    Reso r[3];
    double phase = 0;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> noise(-1, 1);
    for (size_t i = 0; i < s[0].size(); ++i) {
        const double t = double(i) / kRate / scale;
        const int syl = int(t / 0.18);
        const double in = t / 0.18 - syl;
        const bool pause = syl % 7 == 6;
        const double env = pause ? 0 : std::sin(kPi * in) * (syl % 3 == 0 ? 1.0 : 0.7);
        const double f0 = 110 + 25 * std::sin(2 * kPi * 0.7 * t) + 10 * in;
        phase += f0 / kRate;
        double x = 0;
        if (phase >= 1) {
            phase -= 1;
            x = 1; // glottal pulse
        }
        if (in < 0.15 && syl % 2 == 1) x += 0.05 * noise(rng); // consonant noise at the syllable start
        const double* v = vowels[syl % 5];
        double y = 0;
        for (int k = 0; k < 3; ++k) y += r[k].run(x, v[k], 80 + 40 * k) * (k == 0 ? 1.0 : 0.5);
        s[0][i] = s[1][i] = float(0.6 * env * y);
    }
    return s;
}

struct Hit {
    double t; // original time
    int kind; // 0 kick, 1 snare, 2 hat
};

std::vector<Hit> hits(double seconds)
{
    std::vector<Hit> h;
    for (int k = 0; k * 0.25 + 0.1 < seconds - 0.3; ++k) {
        const double t = 0.1 + k * 0.25;
        h.push_back({t, k % 4 == 0 ? 0 : k % 4 == 2 ? 1 : 2});
    }
    return h;
}

Stereo drums(double scale, double seconds)
{
    Stereo s = make(seconds * scale);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> noise(-1, 1);
    for (const Hit& h : hits(seconds)) {
        const size_t at = size_t(std::llround(h.t * scale * kRate));
        double ph = 0, hp = 0, last = 0;
        for (size_t i = 0; i < size_t(0.2 * kRate) && at + i < s[0].size(); ++i) {
            const double u = double(i) / kRate;
            double v = 0;
            if (h.kind == 0) {
                ph += 2 * kPi * (50 + 100 * std::exp(-u * 30)) / kRate;
                v = 0.9 * std::sin(ph) * std::exp(-u * 12);
            } else if (h.kind == 1) {
                ph += 2 * kPi * 190 / kRate;
                v = (0.5 * noise(rng) + 0.3 * std::sin(ph)) * std::exp(-u * 25);
            } else {
                const double n = noise(rng);
                hp = n - last; // high pass
                last = n;
                v = 0.3 * hp * std::exp(-u * 90);
            }
            s[0][at + i] += float(v * (h.kind == 2 ? 0.6 : 1.0));
            s[1][at + i] += float(v * (h.kind == 2 ? 1.0 : 0.8));
        }
    }
    return s;
}

// ---- Stretchers: output of `length` samples at constant speed ----

Stereo ownStretch(const Stereo& in, double speed, size_t length, double* usPerBlock = nullptr)
{
    const int64_t size = int64_t(in[0].size());
    Audio::Stretch::Options options;
    options.window = qEnvironmentVariableIntValue("STRETCH_WINDOW");
    Audio::Stretch st(2, kRate,
                      [&](int64_t from, int count, float* const* out) {
                          for (int c = 0; c < 2; ++c)
                              for (int i = 0; i < count; ++i) {
                                  const int64_t k = from + i;
                                  out[c][i] = k >= 0 && k < size ? in[size_t(c)][size_t(k)] : 0.f;
                              }
                      },
                      [speed](double t) { return t * speed; }, options);
    Stereo out = make(0);
    out[0].resize(length);
    out[1].resize(length);
    QElapsedTimer timer;
    timer.start();
    int blocks = 0;
    for (size_t pos = 0; pos < length; pos += 512, ++blocks) {
        const int n = int(std::min<size_t>(512, length - pos));
        float* buf[2] = {out[0].data() + pos, out[1].data() + pos};
        st.read(int64_t(pos), n, buf);
    }
    if (usPerBlock) *usPerBlock = double(timer.nsecsElapsed()) / 1000.0 / blocks;
    if (qEnvironmentVariableIsSet("STRETCH_ONSETS")) std::printf("    (%d onsets at %.2fx)\n", st.transients(), speed);
    return out;
}

#if SCHNEIDI_HAVE_RUBBERBAND
// Like the app used it up to 0.13: real-time mode, R3, channels together; start pad and delay compensated
Stereo rubberBand(const Stereo& in, double speed, size_t length, double* usPerBlock = nullptr)
{
    using RB = RubberBand::RubberBandStretcher;
    RB rb(kRate, 2, RB::OptionProcessRealTime | RB::OptionEngineFiner | RB::OptionChannelsTogether, 1.0 / speed);
    const size_t pad = rb.getPreferredStartPad();
    int discard = int(rb.getStartDelay());
    Stereo out = make(0);
    out[0].resize(length);
    out[1].resize(length);
    std::vector<float> z(pad + 8192, 0.f), z2(pad + 8192, 0.f), t0(8192), t1(8192);
    size_t fed = 0, done = 0;
    QElapsedTimer timer;
    timer.start();
    {
        const float* p[2] = {z.data(), z2.data()};
        if (pad) rb.process(p, pad, false);
    }
    while (done < length) {
        if (rb.available() <= 0) {
            const size_t want = std::max<size_t>(1, rb.getSamplesRequired());
            std::vector<float> a(want, 0.f), b(want, 0.f);
            for (size_t i = 0; i < want; ++i)
                if (fed + i < in[0].size()) {
                    a[i] = in[0][fed + i];
                    b[i] = in[1][fed + i];
                }
            const float* p[2] = {a.data(), b.data()};
            rb.process(p, want, false);
            fed += want;
            continue;
        }
        if (discard > 0) {
            float* p[2] = {t0.data(), t1.data()};
            discard -= int(rb.retrieve(p, size_t(std::min(discard, std::min(rb.available(), 8192)))));
            continue;
        }
        float* p[2] = {out[0].data() + done, out[1].data() + done};
        done += rb.retrieve(p, std::min(length - done, size_t(rb.available())));
    }
    if (usPerBlock) *usPerBlock = double(timer.nsecsElapsed()) / 1000.0 / double((length + 511) / 512);
    return out;
}
#endif

// ---- Measures ----

// STFT magnitudes (dB) of the mid signal
std::vector<std::vector<float>> spectrogram(const Stereo& s)
{
    constexpr int n = 2048, hop = 512;
    std::vector<std::vector<float>> frames;
    std::vector<std::complex<double>> buf(n);
    for (size_t start = 0; start + n <= s[0].size(); start += hop) {
        std::vector<double> re(n), im(n, 0.0);
        for (int i = 0; i < n; ++i)
            re[size_t(i)] = 0.5 * (s[0][start + size_t(i)] + s[1][start + size_t(i)]) * (0.5 - 0.5 * std::cos(2 * kPi * i / n));
        // DFT via a simple radix-2 FFT
        for (size_t i = 1, j = 0; i < size_t(n); ++i) {
            size_t bit = size_t(n) >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (size_t len = 2; len <= size_t(n); len <<= 1)
            for (size_t i = 0; i < size_t(n); i += len)
                for (size_t k = 0; k < len / 2; ++k) {
                    const double a = -2 * kPi * double(k) / double(len), cr = std::cos(a), ci = std::sin(a);
                    const size_t x = i + k, y = i + k + len / 2;
                    const double tr = re[y] * cr - im[y] * ci, ti = re[y] * ci + im[y] * cr;
                    re[y] = re[x] - tr;
                    im[y] = im[x] - ti;
                    re[x] += tr;
                    im[x] += ti;
                }
        std::vector<float> f(n / 2);
        for (int b = 0; b < n / 2; ++b)
            f[size_t(b)] = float(10 * std::log10(re[size_t(b)] * re[size_t(b)] + im[size_t(b)] * im[size_t(b)] + 1e-10));
        frames.push_back(std::move(f));
    }
    return frames;
}

// Log-spectral distance (dB): RMS over bins up to 16 kHz of frames where the reference has sound
double lsd(const Stereo& got, const Stereo& ref)
{
    const auto a = spectrogram(got), b = spectrogram(ref);
    double sum = 0;
    int frames = 0;
    const int top = 16000 * 2048 / kRate;
    for (size_t k = 0; k < std::min(a.size(), b.size()); ++k) {
        const float peak = *std::max_element(b[k].begin(), b[k].end());
        if (peak < 0) continue; // silence
        double s = 0;
        for (int i = 1; i < top; ++i) {
            const double x = std::max(a[k][size_t(i)], peak - 80), y = std::max(b[k][size_t(i)], peak - 80);
            s += (x - y) * (x - y);
        }
        sum += std::sqrt(s / (top - 1));
        ++frames;
    }
    return frames ? sum / frames : 0;
}

double pitch(const Stereo& s)
{
    // Middle 1 s, Hann, DFT around 440 Hz with 0.05 Hz steps
    const size_t a = s[0].size() / 2 - kRate / 2, n = kRate;
    double best = 0, bestF = 0;
    for (double f = 400; f <= 480; f += 0.05) {
        double re = 0, im = 0;
        for (size_t i = 0; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(n)), x = s[0][a + i] * w;
            re += x * std::cos(2 * kPi * f * double(i) / kRate);
            im -= x * std::sin(2 * kPi * f * double(i) / kRate);
        }
        if (re * re + im * im > best) {
            best = re * re + im * im;
            bestF = f;
        }
    }
    return bestF;
}

// Largest second difference (after the first 0.1 s) over the one of a clean 440 Hz sine of amplitude 0.5
double clicks(const Stereo& s)
{
    double worst = 0;
    for (size_t i = kRate / 10; i + 1 < s[0].size() - kRate / 10; ++i)
        worst = std::max(worst, double(std::abs(s[0][i + 1] - 2 * s[0][i] + s[0][i - 1])));
    const double clean = 0.5 * std::pow(2 * kPi * 440 / kRate, 2);
    return worst / clean;
}

double levelDb(const Stereo& s, int64_t a, int64_t b)
{
    double e = 0;
    a = std::max<int64_t>(0, a);
    b = std::min<int64_t>(int64_t(s[0].size()), b);
    for (int64_t i = a; i < b; ++i) e += double(s[0][size_t(i)]) * s[0][size_t(i)];
    return 10 * std::log10(e / double(std::max<int64_t>(1, b - a)) + 1e-12);
}

// Attack and pre-echo of the hits at their ideal times
void transients(const Stereo& got, const Stereo& ideal, double scale, double seconds, double* attack, double* pre,
                double* preIdeal)
{
    double att = 0, pr = 0, pri = 0;
    int k = 0;
    for (const Hit& h : hits(seconds)) {
        const int64_t at = std::llround(h.t * scale * kRate);
        auto peak = [&](const Stereo& s) {
            double best = -200;
            for (int64_t i = at; i < at + int64_t(0.015 * kRate); i += 48) best = std::max(best, levelDb(s, i, i + 48));
            return best;
        };
        const double pg = peak(got), pi = peak(ideal);
        att += pg - pi;
        pr += levelDb(got, at - int64_t(0.025 * kRate), at - 48) - pg;
        pri += levelDb(ideal, at - int64_t(0.025 * kRate), at - 48) - pi;
        ++k;
    }
    *attack = att / k;
    *pre = pr / k;
    *preIdeal = pri / k;
}

using Stretcher = std::function<Stereo(const Stereo&, double, size_t, double*)>;

void report(const char* name, const Stretcher& st)
{
    constexpr double seconds = 6;
    std::printf("\n%s\n", name);
    std::printf("  speed  LSD sine  music  speech  drums   pitch Hz  click   attack dB  pre-echo dB (ideal)  length  us/block\n");
    for (double speed : {0.5, 0.75, 1.5, 2.0}) {
        const double scale = 1 / speed;
        double us = 0;
        auto run = [&](const std::function<Stereo(double, double)>& g, double* lsdOut) {
            const Stereo in = g(1, seconds), ideal = g(scale, seconds);
            const Stereo out = st(in, speed, ideal[0].size(), &us);
            if (lsdOut) *lsdOut = lsd(out, ideal);
            return std::pair{out, ideal};
        };
        double ls = 0, lm = 0, lv = 0, ld = 0;
        const auto [sOut, sIdeal] = run(sine, &ls);
        run(music, &lm);
        run(speech, &lv);
        const auto [dOut, dIdeal] = run(drums, &ld);
        double attack = 0, pre = 0, preIdeal = 0;
        transients(dOut, dIdeal, scale, seconds, &attack, &pre, &preIdeal);
        const Stereo m = music(1, seconds);
        const size_t length = size_t(double(m[0].size()) * scale);
        st(m, speed, length, &us);
        std::printf("  %4.2f   %6.2f  %6.2f  %6.2f  %6.2f   %7.2f  %5.2f   %7.2f    %6.1f (%6.1f)       %s    %6.1f\n", speed, ls,
                    lm, lv, ld, pitch(sOut), clicks(sOut), attack, pre, preIdeal, sOut[0].size() == sIdeal[0].size() ? "ok" : "BAD", us);
    }
}

// Real material: round trip s, 1/s
void roundTrip(const char* name, const Stretcher& st, const Stereo& in)
{
    std::printf("  %-12s", name);
    for (double speed : {0.5, 0.75, 1.5, 2.0}) {
        const Stereo a = st(in, speed, size_t(double(in[0].size()) / speed), nullptr);
        const Stereo b = st(a, 1 / speed, in[0].size(), nullptr);
        std::printf("  %4.2f: %5.2f", speed, lsd(b, in));
    }
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const Stretcher own = [](const Stereo& in, double s, size_t n, double* us) { return ownStretch(in, s, n, us); };
#if SCHNEIDI_HAVE_RUBBERBAND
    const Stretcher rb = [](const Stereo& in, double s, size_t n, double* us) { return rubberBand(in, s, n, us); };
#endif
    if (argc > 1) {
        auto reader = Audio::Reader::open(QString::fromLocal8Bit(argv[1]), 0, {kRate, 2});
        if (!reader) {
            std::printf("cannot read %s\n", argv[1]);
            return 1;
        }
        const double seconds = argc > 2 ? std::atof(argv[2]) : 20;
        Stereo in = make(seconds);
        float* p[2] = {in[0].data(), in[1].data()};
        reader->read(0, int(in[0].size()), p);
        std::printf("Round trip (LSD dB to the original, lower is better): %s\n", argv[1]);
        roundTrip("own", own, in);
#if SCHNEIDI_HAVE_RUBBERBAND
        if (!qEnvironmentVariableIsSet("STRETCH_OWN_ONLY")) roundTrip("Rubber Band", rb, in);
#endif
        return 0;
    }
    report("Own stretcher (engine/audio/Stretch)", own);
#if SCHNEIDI_HAVE_RUBBERBAND
    if (!qEnvironmentVariableIsSet("STRETCH_OWN_ONLY")) report("Rubber Band (R3, real-time, channels together)", rb);
#else
    std::printf("\nRubber Band not installed: no comparison\n");
#endif
    return 0;
}
