// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Master limiter: lookahead brickwall limiter that holds a true-peak ceiling (BS.1770 4x oversampling, the same
// filter as TruePeakMeter). One gain for all channels. The gain reaches its value before a peak arrives (lookahead,
// smooth ramp) and recovers exponentially (release). The audio is delayed by latency() samples.
// Works on float, so levels above 0 dBFS before the limiter are not clipped (export and measurement agree).
// Plain C++ without MLT/Qt -> same on Linux and Windows, testable. Not thread-safe; copyable (state snapshot).

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

class PeakLimiter {
public:
    static constexpr double kLookaheadS = 0.005, kReleaseS = 0.08;

    void setCeilingDb(double db);
    // Forget the audio so far (after a jump): delay line silent, gain 1
    void reset();
    // Planar float in place (channel c at data[c * samples + i]); a new rate or channel count starts over
    void processPlanar(float* data, int samples, int channels, int rate);
    // Set up for this format ahead of the first block (latency() is known then); same format = no change
    void prepare(int channels, int rate)
    {
        if (channels > 0 && rate > 0 && (channels != m_channels || rate != m_rate)) configure(channels, rate);
    }
    int latency() const { return m_delay; } // samples at the current rate

private:
    void configure(int channels, int rate);

    double m_ceiling = 1.0;
    int m_rate = 0, m_channels = 0;
    int m_lookahead = 0, m_window = 0, m_delay = 0;
    double m_release = 0;
    int64_t m_n = 0;                                // samples processed
    std::vector<std::vector<double>> m_fir;          // per channel: last 12 samples (true peak interpolation)
    int m_firPos = 0;
    std::vector<std::vector<float>> m_line;          // per channel: delay line, m_delay samples
    int m_linePos = 0;
    std::deque<std::pair<int64_t, double>> m_hold;   // sliding minimum of the required gain over m_window
    std::vector<double> m_box;                       // last m_lookahead held gains (moving average)
    int m_boxPos = 0;
    double m_boxSum = 0;
    double m_released = 1.0;                         // held gain after the release
};
