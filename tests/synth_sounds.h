// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Synthetic sounds for the audio event tests (test_audio_events, test_audio_events_yamnet): speech (formant
// synthesis with a gliding pitch, syllables and pauses of irregular length), music (held chords, a melody, bass, kick,
// hi-hats at 120 BPM), applause (dense random claps), cheering (many gliding voices plus noise), laughter ("ha" bursts
// at a steady pace), a shot (noise bang with a low thump), a scream (a high held voice). Deterministic (seeded).

#include <QFile>
#include <QString>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace Synth {

constexpr double kPi = 3.14159265358979323846;

struct Vowel {
    double f1, f2, f3;
};
inline Vowel vowel(int i)
{
    static const Vowel v[] = {{800, 1200, 2500}, {500, 1800, 2500}, {300, 2200, 3000}, {500, 900, 2500}, {350, 800, 2300}};
    return v[i % 5];
}

// Gain of a harmonic at f through three formants
inline double formant(double f, const Vowel& v)
{
    const auto res = [f](double c, double bw) { return 1.0 / (1.0 + ((f - c) / bw) * ((f - c) / bw)); };
    return res(v.f1, 90) + 0.6 * res(v.f2, 110) + 0.3 * res(v.f3, 160) + 0.02;
}

class Builder {
public:
    Builder(int rate, double seconds, unsigned seed = 1) : rate(rate), s(size_t(seconds * rate), 0.f), rng(seed) {}
    int rate;
    std::vector<float> s;
    std::mt19937 rng;

    double uni(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }
    double noise() { return uni(-1, 1); }
    size_t at(double t) const { return size_t(std::max(0.0, t) * rate); }

    // A voiced sound from t0, `len` s long: pitch from f0a to f0b (+ vibrato), through vowel v, `breath` noise share
    void voice(double t0, double len, double f0a, double f0b, const Vowel& v, double amp, double breath = 0,
               double vibHz = 0, double vibDepth = 0, double attack = 0.015, double release = 0.03)
    {
        const size_t a = at(t0), n = size_t(len * rate);
        double phase = 0;
        for (size_t i = 0; i < n && a + i < s.size(); ++i) {
            const double t = double(i) / rate;
            const double f0 = (f0a + (f0b - f0a) * t / len) * (1 + vibDepth * std::sin(2 * kPi * vibHz * t));
            phase += 2 * kPi * f0 / rate;
            double x = 0;
            for (int k = 1; k * f0 < std::min(5000.0, rate / 2.0); ++k) x += formant(k * f0, v) / k * std::sin(k * phase);
            x = 0.6 * x + breath * noise();
            const double env = std::min({1.0, t / attack, (len - t) / release});
            s[a + i] += float(amp * std::max(0.0, env) * x);
        }
    }
    // Noise with an exponential decay (a bang, a clap, a consonant), optionally through a two-pole resonator
    void burst(double t0, double len, double amp, double tau, double centre = 0, double q = 1.5)
    {
        const size_t a = at(t0), n = size_t(len * rate);
        double y1 = 0, y2 = 0;
        const double w = 2 * kPi * centre / rate, r = centre > 0 ? std::exp(-w / (2 * q)) : 0;
        for (size_t i = 0; i < n && a + i < s.size(); ++i) {
            const double t = double(i) / rate;
            double x = noise() * std::exp(-t / tau);
            if (centre > 0) {
                const double y = x + 2 * r * std::cos(w) * y1 - r * r * y2;
                y2 = y1;
                y1 = y;
                x = y * (1 - r);
            }
            s[a + i] += float(amp * x);
        }
    }
    void tone(double t0, double len, double f, double amp, int partials = 4, double attack = 0.02, double release = 0.05)
    {
        const size_t a = at(t0), n = size_t(len * rate);
        for (size_t i = 0; i < n && a + i < s.size(); ++i) {
            const double t = double(i) / rate;
            double x = 0;
            for (int k = 1; k <= partials; ++k) x += std::sin(2 * kPi * f * k * t) / (k * k);
            s[a + i] += float(amp * std::max(0.0, std::min({1.0, t / attack, (len - t) / release})) * x);
        }
    }
    // A floor of quiet noise everywhere (-66 dBFS RMS), like a real recording
    void floor(double amp = 0.0009)
    {
        for (float& v : s) v += float(amp * noise());
    }
};

// Speech from t0 for len seconds (words of 1-4 syllables, pauses 0.1-0.45 s)
inline void speech(Builder& b, double t0, double len, double base = 115)
{
    double t = t0;
    while (t < t0 + len - 0.4) {
        const int syllables = 1 + int(b.uni(0, 4));
        const double f = base * b.uni(0.92, 1.15);
        for (int k = 0; k < syllables && t < t0 + len - 0.3; ++k) {
            if (b.uni(0, 1) < 0.4) { // a consonant before the vowel
                b.burst(t, b.uni(0.03, 0.06), 0.05, 0.03, 3500, 0.8);
                t += 0.04;
            }
            const double d = b.uni(0.09, 0.25);
            b.voice(t, d, f * b.uni(0.95, 1.12), f * b.uni(0.88, 1.02), vowel(int(b.uni(0, 5))), b.uni(0.15, 0.35));
            t += d + b.uni(0.01, 0.06);
        }
        t += b.uni(0.1, 0.45);
    }
}

// Music at 120 BPM: a held chord per bar, a melody note per beat, bass and kick on the beat, hi-hats between
inline void music(Builder& b, double t0, double len)
{
    const double beat = 0.5;
    static const double chords[4][3] = {{261.6, 329.6, 392.0}, {220.0, 261.6, 329.6}, {174.6, 220.0, 261.6}, {196.0, 246.9, 293.7}};
    static const double scale[] = {523.3, 587.3, 659.3, 698.5, 784.0, 880.0};
    for (int bar = 0; bar * 4 * beat < len; ++bar) {
        const double tb = t0 + bar * 4 * beat, barLen = std::min(4 * beat, t0 + len - tb);
        for (double f : chords[bar % 4]) b.tone(tb, barLen, f, 0.06, 4, 0.05, 0.1);
        for (int k = 0; k < 4; ++k) {
            const double t = tb + k * beat;
            if (t + beat > t0 + len + 1e-9) break;
            b.tone(t, beat, scale[size_t(b.uni(0, 6))], 0.07, 3, 0.02, 0.06);
            b.tone(t, beat * 0.9, chords[bar % 4][0] / 4, 0.12, 2, 0.01, 0.1);
            // Kick: a sine falling from 150 to 50 Hz
            const size_t a = b.at(t);
            for (size_t i = 0; i < size_t(0.2 * b.rate) && a + i < b.s.size(); ++i) {
                const double x = double(i) / b.rate;
                b.s[a + i] += float(0.25 * std::exp(-x / 0.06) * std::sin(2 * kPi * (50 * x + 3 * (1 - std::exp(-x / 0.03)))));
            }
            b.burst(t + beat / 2, 0.04, 0.03, 0.01, 7000, 1.0);
        }
    }
}

// Applause: random claps (about 50 per second), each a short bright noise burst with a little room; rises and dies
// away
inline void applause(Builder& b, double t0, double len)
{
    double t = t0;
    while (t < t0 + len) {
        const double env = std::min({1.0, (t - t0) / 0.3, (t0 + len - t) / 0.8});
        b.burst(t, 0.08, 0.6 * b.uni(0.4, 1.0) * std::max(0.05, env), b.uni(0.006, 0.015), b.uni(1200, 3000), 1.2);
        t += -std::log(1 - b.uni(0, 0.999)) / 50.0;
    }
}

// Cheering: 14 voices gliding and wobbling at 220-480 Hz, overlapping, plus crowd noise
inline void cheering(Builder& b, double t0, double len)
{
    for (int v = 0; v < 14; ++v) {
        const double start = t0 + b.uni(0, 0.4), end = t0 + len - b.uni(0, 0.4);
        // in pieces of 0.3-0.8 s with a new glide each (people shout, breathe, shout again)
        for (double t = start; t < end - 0.2;) {
            const double d = std::min(end - t, b.uni(0.3, 0.8));
            const double f = b.uni(220, 480);
            b.voice(t, d, f, f * b.uni(0.85, 1.2), vowel(int(b.uni(0, 2)) * 3), b.uni(0.02, 0.045), 0.2, b.uni(4, 7),
                    0.03, 0.05, 0.08);
            t += d + b.uni(0.0, 0.15);
        }
    }
    const size_t a = b.at(t0), n = size_t(len * b.rate);
    double lp = 0;
    for (size_t i = 0; i < n && a + i < b.s.size(); ++i) {
        const double t = double(i) / b.rate;
        lp += 0.3 * (b.noise() - lp);
        b.s[a + i] += float(0.04 * std::min({1.0, t / 0.3, (len - t) / 0.5}) * lp);
    }
}

// Laughter: `count` "ha" bursts every ~0.21 s (breath, then a breathy voiced vowel at ~270 Hz falling)
inline void laughter(Builder& b, double t0, int count = 6)
{
    double t = t0;
    for (int k = 0; k < count; ++k) {
        const double amp = 0.38 - 0.02 * k;
        b.burst(t, 0.03, 0.06, 0.02, 1500, 0.7);
        b.voice(t + 0.02, 0.08, 285 - 3 * k, 255 - 3 * k, vowel(0), amp, 0.25, 0, 0, 0.01, 0.035);
        t += 0.21 + b.uni(-0.01, 0.01);
    }
}

// A shot: a loud noise bang with a low thump
inline void shot(Builder& b, double t0, double amp = 0.8)
{
    b.burst(t0, 0.6, amp, 0.05);
    const size_t a = b.at(t0);
    for (size_t i = 0; i < size_t(0.6 * b.rate) && a + i < b.s.size(); ++i) {
        const double x = double(i) / b.rate;
        b.s[a + i] += float(0.3 * amp * std::exp(-x / 0.15) * std::sin(2 * kPi * 60 * x));
    }
}

// A scream: a loud, high held voice with vibrato
inline void scream(Builder& b, double t0, double len)
{
    b.voice(t0, len, 720, 690, Vowel{1000, 1500, 2800}, 0.5, 0.05, 6, 0.025, 0.06, 0.15);
}

inline bool writeWav(const QString& path, const std::vector<float>& mono, int rate, int channels = 1)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 data = quint32(mono.size() * 2 * size_t(channels));
    const auto le32 = [&](quint32 v) {
        char c[4];
        qToLittleEndian(v, c);
        f.write(c, 4);
    };
    const auto le16 = [&](quint16 v) {
        char c[2];
        qToLittleEndian(v, c);
        f.write(c, 2);
    };
    f.write("RIFF");
    le32(36 + data);
    f.write("WAVEfmt ");
    le32(16);
    le16(1);
    le16(quint16(channels));
    le32(quint32(rate));
    le32(quint32(rate * 2 * channels));
    le16(quint16(2 * channels));
    le16(16);
    f.write("data");
    le32(data);
    for (float v : mono)
        for (int c = 0; c < channels; ++c) le16(quint16(qint16(std::lround(std::clamp(v, -1.f, 1.f) * 32767))));
    return true;
}

} // namespace Synth
