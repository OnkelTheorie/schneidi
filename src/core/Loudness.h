#pragma once
// Lautheitsmessung nach ITU-R BS.1770-4 / EBU R128 (EBU Tech 3341/3342):
// K-Filter (Hochregal + Hochpass) je Kanal, Energie in 100-ms-Häppchen, daraus
//   Momentary  = letzte 400 ms, Short-term = letzte 3 s,
//   Integrated = alle 400-ms-Blöcke (75 % Überlappung) mit absolutem (-70 LUFS) und relativem (-10 LU) Gate,
//   Loudness Range (LRA) = 10.–95. Perzentil der Short-term-Werte (Gates -70 LUFS / -20 LU).
// Reines C++ ohne MLT/Qt -> gleich unter Linux und Windows, testbar. Nicht thread-sicher (Aufrufer sperrt).

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

class LoudnessMeter {
public:
    static constexpr double kSilence = -200.0; // "−∞" (noch nichts gemessen bzw. Stille)
    static constexpr int kMaxChannels = 8;

    void reset();

    // Samples anhängen; sample(c, i) liefert Kanal c, Sample i als float (±1 = 0 dBFS).
    // Wechselt Samplerate oder Kanalzahl, beginnt die Filterung neu (Blöcke bleiben erhalten).
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
    // Planar float (MLT mlt_audio_float) bzw. verschachtelt (f32le)
    void addPlanar(const float* data, int frames, int channels, int rate)
    {
        add(frames, channels, rate, [&](int c, int i) { return data[c * frames + i]; });
    }
    void addInterleaved(const float* data, int frames, int channels, int rate)
    {
        add(frames, channels, rate, [&](int c, int i) { return data[i * channels + c]; });
    }

    double momentary() const;  // LUFS, kSilence solange < 400 ms
    double shortTerm() const;  // LUFS, kSilence solange < 3 s (wie EBU: erst dann gültig)
    double integrated() const; // LUFS, kSilence ohne Block über dem Gate
    double range() const;      // LU (0 ohne genug Werte)
    double measuredSeconds() const { return m_subBlocks * 0.1; }

    // 400-ms-Blöcke (mittlere gewichtete Energie) – mehrere Messungen lassen sich damit gemeinsam gaten
    const std::vector<double>& blocks() const { return m_blocks; }
    static double integratedOf(const std::vector<double>& blocks);
    static double lufs(double power); // -0.691 + 10·log10(power), Stille = kSilence

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    void configure(int channels, int rate);
    double filter(int c, double x)
    {
        // Direktform II transponiert, zwei Stufen (Hochregal, dann Hochpass)
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
    double windowPower(int subBlocks) const; // Mittel der letzten n Häppchen

    int m_rate = 0, m_channels = 0;
    int m_blockSamples = 4800; // 100 ms
    Biquad m_shelf, m_hp;
    std::array<std::array<double, 4>, kMaxChannels> m_state{};
    std::array<double, kMaxChannels> m_weight{};
    double m_acc = 0;
    int m_accCount = 0;
    std::deque<double> m_recent; // Energie je 100-ms-Häppchen (höchstens 30 = 3 s)
    long long m_subBlocks = 0;
    std::vector<double> m_blocks;     // 400-ms-Blöcke (alle 100 ms)
    std::vector<double> m_shortTerms; // 3-s-Fenster (alle 100 ms) für LRA
};

// Live-Messung in der Vorschau (Loudness-Meter im Mixer): der Audio-Thread des Players füttert den Messer
// (nur solange active), die Oberfläche liest unter demselben Mutex.
struct SharedLoudness {
    std::mutex mutex;
    LoudnessMeter meter;
    std::atomic<bool> active{false};
};
