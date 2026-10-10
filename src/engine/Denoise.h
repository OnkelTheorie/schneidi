#pragma once
// Noise reduction (effect "denoise", like DaVinci's Fairlight FX → Noise Reduction): spectral noise suppression
// (STFT with 75 % overlap, noise spectrum tracked by its minimum or set by a level, spectral subtraction with a gain
// floor and gain smoothing against "musical noise"). Real time, ~11 ms latency (48 kHz).
//
// Own implementation instead of FFmpeg's afftdn: MLT's avfilter wrapper (7.30) only works with filters that return
// as many samples as they get – afftdn/anlmdn buffer internally ("Unexpected return format", sound unchanged).
//
// One filter per clip, attached to all its cuts (like the sub-frame delay): its state runs on across cut
// boundaries (transitions), so the sound does not start over there. Parameters are integer properties (LiveUpdate
// copies them as text; MLT would write other numbers in the current locale) and apply live.

#include "core/Types.h"

#include <memory>
#include <vector>

namespace Mlt {
class Filter;
class Profile;
} // namespace Mlt

namespace Denoise {

constexpr const char* EffectId = "denoise";

// Filter for all audio cuts of clip c; nullptr = no enabled noise reduction
std::shared_ptr<Mlt::Filter> make(Mlt::Profile& profile, const Clip& c);

// The processing of one channel (also used by the tests)
class Processor {
public:
    explicit Processor(int sampleRate);
    ~Processor();
    // reductionDb: most the noise is lowered; floorDb: noise level (dBFS RMS) when not tracking
    void setParams(double reductionDb, double floorDb, bool track);
    void reset();
    // In place; the output is the input `latency()` samples later
    void process(float* data, int n);
    int latency() const { return m_size; }

private:
    void hop();

    int m_size, m_hop;
    int m_rate = 48000;
    double m_reduction = 12, m_floor = -50;
    bool m_track = true;
    std::vector<float> m_window;      // sqrt-Hann (analysis and synthesis)
    std::vector<float> m_in;          // last m_size input samples
    std::vector<float> m_ola;         // overlap-add accumulator
    std::vector<float> m_out;         // finished samples, waiting to be returned
    std::vector<double> m_re, m_im;   // FFT buffers
    std::vector<double> m_smooth;     // smoothed power per bin (short, for the gain)
    std::vector<double> m_long;       // smoothed power per bin (long, for the noise tracking)
    std::vector<double> m_noise;      // noise estimate per bin (< 0 = not known yet)
    std::vector<double> m_gain;       // last gain per bin
    int m_pending = 0;                // input samples since the last hop
    long m_seen = 0;                  // input samples since reset (noise tracking waits for a full window)
};

} // namespace Denoise
