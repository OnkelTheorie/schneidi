// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Dynamics.h"

#include <algorithm>
#include <cmath>

double Compressor::curveDb(const TrackDynamics& d, double x)
{
    const double t = d.thresholdDb, r = std::max(1.0, d.ratio), w = std::max(0.0, d.kneeDb);
    double y;
    if (w > 0 && std::abs(x - t) <= w / 2) {
        const double over = x - t + w / 2;
        y = x + (1 / r - 1) * over * over / (2 * w);
    } else if (x <= t) {
        y = x;
    } else {
        y = t + (x - t) / r;
    }
    return y + d.makeupDb;
}

TrackDynamics Compressor::clamped(TrackDynamics d)
{
    auto fix = [](double v, double lo, double hi, double fallback) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    const TrackDynamics def;
    d.thresholdDb = fix(d.thresholdDb, kMinThreshold, kMaxThreshold, def.thresholdDb);
    d.ratio = fix(d.ratio, kMinRatio, kMaxRatio, def.ratio);
    d.attackMs = fix(d.attackMs, kMinAttack, kMaxAttack, def.attackMs);
    d.releaseMs = fix(d.releaseMs, kMinRelease, kMaxRelease, def.releaseMs);
    d.kneeDb = fix(d.kneeDb, 0, kMaxKnee, def.kneeDb);
    d.makeupDb = fix(d.makeupDb, kMinMakeup, kMaxMakeup, def.makeupDb);
    return d;
}

void Compressor::setParams(const TrackDynamics& d)
{
    const bool wasOn = m_d.on;
    m_d = d;
    if (!wasOn && d.on) reset(); // switched on: start without an old reduction
    if (m_rate > 0) configure(m_rate);
}

void Compressor::reset()
{
    m_peak = 0;
    m_reduction = 0;
}

void Compressor::configure(int rate)
{
    m_rate = rate;
    auto coef = [rate](double ms) { return ms > 0 ? std::exp(-1.0 / (ms / 1000.0 * rate)) : 0.0; };
    m_attack = coef(m_d.attackMs);
    m_release = coef(m_d.releaseMs);
}

void Compressor::processPlanar(float* data, int samples, int channels, int rate)
{
    if (!m_d.on || !data || samples <= 0 || channels <= 0 || rate <= 0) return;
    if (rate != m_rate) configure(rate);
    const double makeup = m_d.makeupDb;
    for (int i = 0; i < samples; ++i) {
        float level = 0.f;
        for (int c = 0; c < channels; ++c) level = std::max(level, std::abs(data[size_t(c) * samples + i]));
        double over = 0; // wanted reduction (dB) for this sample
        if (level > 1e-10f && std::isfinite(level)) {
            const double x = 20 * std::log10(double(level));
            over = std::max(0.0, x - (curveDb(m_d, x) - makeup));
        }
        m_peak = std::max(over, m_release * m_peak + (1 - m_release) * over);
        m_reduction = m_attack * m_reduction + (1 - m_attack) * m_peak;
        const float gain = float(std::pow(10.0, (makeup - m_reduction) / 20));
        for (int c = 0; c < channels; ++c) data[size_t(c) * samples + i] *= gain;
    }
}
