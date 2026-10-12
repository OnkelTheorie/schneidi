#include "core/Limiter.h"

#include "core/TruePeak.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kTaps = TruePeakMeter::kTaps, kPhases = TruePeakMeter::kPhases;
// Below the ceiling by 0.18 dB: the gain changes a little within the interpolation filter's span, and other
// true peak meters (ffmpeg ebur128) interpolate a bit differently
constexpr double kMargin = 0.98;
} // namespace

void PeakLimiter::setCeilingDb(double db)
{
    m_ceiling = std::clamp(std::pow(10.0, db / 20.0), 0.01, 1.0);
}

void PeakLimiter::reset()
{
    const int channels = m_channels, rate = m_rate;
    m_channels = m_rate = 0;
    if (channels > 0 && rate > 0) configure(channels, rate);
}

void PeakLimiter::configure(int channels, int rate)
{
    m_channels = channels;
    m_rate = rate;
    m_lookahead = std::max(1, int(std::lround(kLookaheadS * rate)));
    // A requirement found at input sample j covers the signal from j - 7 to j (the interpolation filter is
    // 12 taps long): hold it 7 samples longer and delay by 6 more, then the ramp has reached it in time
    m_window = m_lookahead + 7;
    m_delay = m_lookahead + 6;
    m_release = 1.0 - std::exp(-1.0 / (kReleaseS * rate));
    m_n = 0;
    m_fir.assign(channels, std::vector<double>(kTaps, 0.0));
    m_firPos = 0;
    m_line.assign(channels, std::vector<float>(m_delay, 0.f));
    m_linePos = 0;
    m_hold.clear();
    m_box.assign(m_lookahead, 1.0);
    m_boxPos = 0;
    m_boxSum = m_lookahead;
    m_released = 1.0;
}

void PeakLimiter::processPlanar(float* data, int samples, int channels, int rate)
{
    if (!data || samples <= 0 || channels <= 0 || rate <= 0) return;
    if (channels != m_channels || rate != m_rate) configure(channels, rate);
    const double target = m_ceiling * kMargin;
    const float ceiling = float(m_ceiling);
    for (int i = 0; i < samples; ++i) {
        // Highest sample or interpolated value (true peak) that just came in, over all channels
        m_firPos = (m_firPos + kTaps - 1) % kTaps;
        double peak = 0;
        for (int c = 0; c < channels; ++c) {
            std::vector<double>& h = m_fir[c];
            const double x = data[c * samples + i];
            h[m_firPos] = x;
            peak = std::max(peak, std::abs(x));
            for (int p = 0; p < kPhases; ++p) {
                double y = 0;
                for (int k = 0; k < kTaps; ++k) y += TruePeakMeter::kCoef[p][k] * h[(m_firPos + k) % kTaps];
                peak = std::max(peak, std::abs(y));
            }
        }
        const double required = peak > target ? target / peak : 1.0;

        // Lowest required gain within the window (monotonic queue)
        while (!m_hold.empty() && m_hold.back().second >= required) m_hold.pop_back();
        m_hold.emplace_back(m_n, required);
        while (m_hold.front().first <= m_n - m_window) m_hold.pop_front();
        const double held = m_hold.front().second;
        // Release: rises slowly back to the held value, never above it
        m_released = held < m_released ? held : m_released + (held - m_released) * m_release;
        // Moving average over the lookahead: a smooth ramp that is at or below every held value it spans
        m_boxSum += m_released - m_box[m_boxPos];
        m_box[m_boxPos] = m_released;
        m_boxPos = (m_boxPos + 1) % m_lookahead;
        if (m_boxPos == 0) { // against rounding drift of the running sum
            m_boxSum = 0;
            for (double v : m_box) m_boxSum += v;
        }
        const float gain = float(std::min(1.0, m_boxSum / m_lookahead));

        for (int c = 0; c < channels; ++c) {
            float& s = data[c * samples + i];
            const float delayed = m_line[c][m_linePos];
            m_line[c][m_linePos] = s;
            s = std::clamp(delayed * gain, -ceiling, ceiling);
        }
        m_linePos = (m_linePos + 1) % m_delay;
        ++m_n;
    }
}
