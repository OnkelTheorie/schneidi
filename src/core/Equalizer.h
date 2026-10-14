#pragma once
// Track EQ (Mixer "EQ", like Fairlight): 6 biquad bands (RBJ cookbook) in series, high/low pass 12 dB/oct.
// Bells and shelves at 0 dB and bands that are off are skipped, so a flat EQ is exactly transparent.
// responseDb() is the magnitude response of the same filters (EQ curve, tests).
// Plain C++ without MLT/Qt widgets -> same on Linux and Windows, testable. Not thread-safe; copyable (state snapshot).

#include "core/Types.h"

#include <array>
#include <vector>

class Equalizer {
public:
    static constexpr double kMinFreq = 20, kMaxFreq = 20000, kMinGain = -24, kMaxGain = 24, kMinQ = 0.1, kMaxQ = 10;

    // Gain (dB) of the whole EQ at `hz`; EQ off = 0
    static double responseDb(const TrackEq& eq, double hz, int rate);
    // Values in their ranges (project file, dialogs, CLI); the band kinds stay as they belong
    static TrackEq clamped(TrackEq eq);

    void setEq(const TrackEq& eq);
    const TrackEq& eq() const { return m_eq; }
    void reset(); // filter state silent (after a jump)
    // Planar float in place (channel c at data[c * samples + i]); off = untouched
    void processPlanar(float* data, int samples, int channels, int rate);

    struct Coefs {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // normalized (a0 = 1)
    };
    // Filter of one band; false = the band does nothing (off, or a bell/shelf at 0 dB)
    static bool coefs(const EqBand& band, int rate, Coefs* out);

private:
    void configure(int channels, int rate);

    TrackEq m_eq;
    int m_rate = 0, m_channels = 0;
    std::vector<Coefs> m_active;                     // filters of the active bands
    std::vector<std::array<double, 2>> m_state;      // per active band and channel (transposed direct form II)
};
