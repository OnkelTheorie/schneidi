#pragma once
// True-peak measurement per ITU-R BS.1770-4 Annex 2 (EBU Tech 3341, dBTP):
// 4x oversampling with the 48-tap polyphase FIR from the recommendation (4 phases of 12 taps), peak of the
// magnitude of the interpolated values. The original samples count too (true peak >= sample peak).
// The filter history is kept across calls (chunks = one continuous stream).
// Pure C++ without MLT/Qt -> same on Linux and Windows, testable. Not thread-safe (caller locks).

#include <algorithm>
#include <array>
#include <cmath>

class TruePeakMeter {
public:
    static constexpr double kSilence = -200.0; // "−∞"
    static constexpr int kMaxChannels = 8;
    static constexpr int kPhases = 4, kTaps = 12;

    // Polyphase coefficients from BS.1770-4 Annex 2 (phase p, tap k: h[4k + p])
    static const double kCoef[kPhases][kTaps];

    // Reset peaks and filter history
    void reset();
    // Reset only the peaks (history stays -> measurement per chunk without a jump at the boundaries)
    void resetPeaks() { m_peak.fill(0.0); }

    // Append samples; sample(c, i) returns channel c, sample i (+-1 = 0 dBFS).
    // If sample rate or channel count changes, filtering starts anew (peaks stay).
    template <typename SampleFn>
    void add(int frames, int channels, int rate, SampleFn sample)
    {
        if (frames <= 0 || channels <= 0 || rate <= 0) return;
        channels = std::min(channels, kMaxChannels);
        if (channels != m_channels || rate != m_rate) {
            m_channels = channels;
            m_rate = rate;
            for (auto& h : m_hist) h.fill(0.0);
            m_pos = 0;
        }
        for (int i = 0; i < frames; ++i) {
            m_pos = (m_pos + kTaps - 1) % kTaps; // newest sample at m_pos, older ones behind it
            for (int c = 0; c < m_channels; ++c) {
                auto& h = m_hist[c];
                const double x = double(sample(c, i));
                h[m_pos] = x;
                double peak = std::abs(x);
                for (int p = 0; p < kPhases; ++p) {
                    double y = 0;
                    for (int k = 0; k < kTaps; ++k) y += kCoef[p][k] * h[(m_pos + k) % kTaps];
                    peak = std::max(peak, std::abs(y));
                }
                m_peak[c] = std::max(m_peak[c], peak);
            }
        }
    }
    void addPlanar(const float* data, int frames, int channels, int rate)
    {
        add(frames, channels, rate, [&](int c, int i) { return data[c * frames + i]; });
    }
    void addInterleaved(const float* data, int frames, int channels, int rate)
    {
        add(frames, channels, rate, [&](int c, int i) { return data[i * channels + c]; });
    }

    int channels() const { return m_channels; }
    double peak(int c) const { return c >= 0 && c < kMaxChannels ? m_peak[c] : 0.0; } // linear
    double peakDb(int c) const { return toDb(peak(c)); }                               // dBTP
    double maxDb() const { return toDb(*std::max_element(m_peak.begin(), m_peak.end())); }
    static double toDb(double linear) { return linear > 1e-10 ? 20.0 * std::log10(linear) : kSilence; }

private:
    int m_rate = 0, m_channels = 0, m_pos = 0;
    std::array<std::array<double, kTaps>, kMaxChannels> m_hist{};
    std::array<double, kMaxChannels> m_peak{};
};
