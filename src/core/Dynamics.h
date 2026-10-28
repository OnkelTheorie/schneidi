// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Track compressor (Mixer "DYN", like Fairlight Dynamics): static curve with soft knee and make-up gain, a peak
// detector in the log domain with attack/release (smooth decoupled peak detector, Giannoulis/Massberg/Reiss 2012),
// stereo-linked (one gain for all channels). No lookahead, no latency. Times are in ms, so the result does not depend
// on the sample rate or on the block size. Works on float (levels above 0 dBFS are fine).
// Plain C++ without MLT/Qt widgets -> same on Linux and Windows, testable. Not thread-safe; copyable (state snapshot).

#include "core/Types.h"

class Compressor {
public:
    // Output level for a steady input level (dB), make-up included
    static double curveDb(const TrackDynamics& d, double inDb);
    // Values in their ranges (project file, dialogs, CLI)
    static TrackDynamics clamped(TrackDynamics d);
    static constexpr double kMinThreshold = -60, kMaxThreshold = 0, kMinRatio = 1, kMaxRatio = 100;
    static constexpr double kMinAttack = 0.1, kMaxAttack = 300, kMinRelease = 5, kMaxRelease = 5000;
    static constexpr double kMaxKnee = 24, kMinMakeup = -24, kMaxMakeup = 24;

    void setParams(const TrackDynamics& d);
    const TrackDynamics& params() const { return m_d; }
    // Forget the level so far (after a jump)
    void reset();
    // Planar float in place (channel c at data[c * samples + i]); off = untouched
    void processPlanar(float* data, int samples, int channels, int rate);
    // Current gain change of the compression (<= 0 dB, without make-up)
    double gainReductionDb() const { return -m_reduction; }

private:
    void configure(int rate);

    TrackDynamics m_d;
    int m_rate = 0;
    double m_attack = 0, m_release = 0; // one-pole coefficients
    double m_peak = 0, m_reduction = 0; // dB (>= 0): peak detector, smoothed reduction
};
