// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Loudness measurement per ITU-R BS.1770-4 / EBU R128 (EBU Tech 3341/3342):
// K-filter (high shelf + high pass) per channel, energy in 100 ms chunks, from which
//   Momentary  = last 400 ms, Short-term = last 3 s,
//   Integrated = all 400 ms blocks (75 % overlap) with absolute (-70 LUFS) and relative (-10 LU) gate,
//   Loudness Range (LRA) = 10th-95th percentile of the short-term values (gates -70 LUFS / -20 LU).
// Pure C++ without MLT/Qt -> same on Linux and Windows, testable. Not thread-safe (caller locks).

#include "core/TruePeak.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

class LoudnessMeter {
public:
    static constexpr double kSilence = -200.0; // "-inf" (nothing measured yet or silence)
    static constexpr int kMaxChannels = 8;

    void reset();

    // Append samples; sample(c, i) returns channel c, sample i as float (+-1 = 0 dBFS).
    // If sample rate or channel count changes, filtering starts anew (blocks are kept).
    template <typename SampleFn>
    void add(int frames, int channels, int rate, SampleFn sample)
    {
        if (frames <= 0 || channels <= 0 || rate <= 0) return;
        configure(std::min(channels, kMaxChannels), rate);
        for (int i = 0; i < frames; ++i) {
            double sum = 0;
            for (int c = 0; c < m_channels; ++c) {
                const double y = filter(c, double(sample(c, i)));
                sum += m_weight[c] * y * y;
            }
            m_acc += sum;
            if (++m_accCount >= m_blockSamples) finishSubBlock();
        }
    }
    // Planar float (MLT mlt_audio_float) or interleaved (f32le)
    void addPlanar(const float* data, int frames, int channels, int rate)
    {
        add(frames, channels, rate, [&](int c, int i) { return data[c * frames + i]; });
    }
    void addInterleaved(const float* data, int frames, int channels, int rate)
    {
        add(frames, channels, rate, [&](int c, int i) { return data[i * channels + c]; });
    }

    double momentary() const;  // LUFS, kSilence while < 400 ms
    double shortTerm() const;  // LUFS, kSilence while < 3 s (as in EBU: only valid then)
    double integrated() const; // LUFS, kSilence without a block above the gate
    double range() const;      // LU (0 without enough values)
    double measuredSeconds() const { return m_subBlocks * 0.1; }

    // 400 ms blocks (mean weighted energy) - several measurements can thus be gated together
    const std::vector<double>& blocks() const { return m_blocks; }
    static double integratedOf(const std::vector<double>& blocks);
    static double lufs(double power); // -0.691 + 10*log10(power), silence = kSilence

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    void configure(int channels, int rate);
    double filter(int c, double x)
    {
        // Direct form II transposed, two stages (high shelf, then high pass)
        auto& s = m_state[c];
        const double y1 = m_shelf.b0 * x + s[0];
        s[0] = m_shelf.b1 * x - m_shelf.a1 * y1 + s[1];
        s[1] = m_shelf.b2 * x - m_shelf.a2 * y1;
        const double y2 = m_hp.b0 * y1 + s[2];
        s[2] = m_hp.b1 * y1 - m_hp.a1 * y2 + s[3];
        s[3] = m_hp.b2 * y1 - m_hp.a2 * y2;
        return y2;
    }
    void finishSubBlock();
    double windowPower(int subBlocks) const; // mean of the last n chunks

    int m_rate = 0, m_channels = 0;
    int m_blockSamples = 4800; // 100 ms
    Biquad m_shelf, m_hp;
    std::array<std::array<double, 4>, kMaxChannels> m_state{};
    std::array<double, kMaxChannels> m_weight{};
    double m_acc = 0;
    int m_accCount = 0;
    std::deque<double> m_recent; // energy per 100 ms chunk (at most 30 = 3 s)
    long long m_subBlocks = 0;
    std::vector<double> m_blocks;     // 400 ms blocks (every 100 ms)
    std::vector<double> m_shortTerms; // 3 s windows (every 100 ms) for LRA
};

// Live measurement in the preview (loudness meter in the mixer): the player's audio thread feeds the meter
// (only while active), the UI reads under the same mutex. truePeak = highest true peak since reset.
struct SharedLoudness {
    std::mutex mutex;
    LoudnessMeter meter;
    TruePeakMeter truePeak;
    std::atomic<bool> active{false};
};
