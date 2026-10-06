#pragma once
// True-Peak-Messung nach ITU-R BS.1770-4 Annex 2 (EBU Tech 3341, dBTP):
// 4-fach Überabtastung mit dem 48-Tap-Polyphasen-FIR aus der Empfehlung (4 Phasen à 12 Taps), Spitzenwert des
// Betrags der interpolierten Werte. Die Original-Samples zählen mit (True Peak >= Sample Peak).
// Der Filterverlauf bleibt über Aufrufe hinweg erhalten (Häppchen = ein durchgehender Strom).
// Reines C++ ohne MLT/Qt -> gleich unter Linux und Windows, testbar. Nicht thread-sicher (Aufrufer sperrt).

#include <algorithm>
#include <array>
#include <cmath>

class TruePeakMeter {
public:
    static constexpr double kSilence = -200.0; // "−∞"
    static constexpr int kMaxChannels = 8;
    static constexpr int kPhases = 4, kTaps = 12;

    // Polyphasen-Koeffizienten aus BS.1770-4 Annex 2 (Phase p, Tap k: h[4k + p])
    static const double kCoef[kPhases][kTaps];

    // Spitzenwerte und Filterverlauf zurücksetzen
    void reset();
    // Nur die Spitzenwerte zurücksetzen (Verlauf bleibt -> Messung je Häppchen ohne Sprung an den Grenzen)
    void resetPeaks() { m_peak.fill(0.0); }

    // Samples anhängen; sample(c, i) liefert Kanal c, Sample i (±1 = 0 dBFS).
    // Wechselt Samplerate oder Kanalzahl, beginnt die Filterung neu (Spitzenwerte bleiben).
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
            m_pos = (m_pos + kTaps - 1) % kTaps; // neuestes Sample an m_pos, ältere dahinter
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
