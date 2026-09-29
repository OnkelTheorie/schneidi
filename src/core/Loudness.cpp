#include "core/Loudness.h"

#include <algorithm>
#include <cmath>

void LoudnessMeter::reset()
{
    *this = LoudnessMeter();
}

void LoudnessMeter::configure(int channels, int rate)
{
    if (channels == m_channels && rate == m_rate) return;
    m_channels = channels;
    m_rate = rate;
    m_blockSamples = std::max(1, int(std::lround(rate * 0.1)));
    m_state = {};
    m_acc = 0;
    m_accCount = 0;

    // K-Filter-Koeffizienten für beliebige Samplerate (aus den 48-kHz-Werten der Norm abgeleitet,
    // gleiche Herleitung wie libebur128/ffmpeg)
    const double pi = 3.14159265358979323846;
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(pi * f0 / rate);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        m_shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        m_shelf.b1 = 2.0 * (K * K - Vh) / a0;
        m_shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        m_shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        m_shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(pi * f0 / rate);
        const double a0 = 1.0 + K / Q + K * K;
        m_hp.b0 = 1.0;
        m_hp.b1 = -2.0;
        m_hp.b2 = 1.0;
        m_hp.a1 = 2.0 * (K * K - 1.0) / a0;
        m_hp.a2 = (1.0 - K / Q + K * K) / a0;
    }
    // Kanalgewichte: L/R/C = 1; bei 5.1 (L R C LFE Ls Rs) LFE ignoriert, Surround +1,5 dB
    for (int c = 0; c < kMaxChannels; ++c) m_weight[c] = c < channels ? 1.0 : 0.0;
    if (channels == 6) {
        m_weight[3] = 0.0;
        m_weight[4] = m_weight[5] = 1.41;
    }
}

void LoudnessMeter::finishSubBlock()
{
    m_recent.push_back(m_acc / m_accCount);
    if (m_recent.size() > 30) m_recent.pop_front();
    m_acc = 0;
    m_accCount = 0;
    ++m_subBlocks;
    if (m_recent.size() >= 4) m_blocks.push_back(windowPower(4));
    if (m_recent.size() >= 30) m_shortTerms.push_back(windowPower(30));
}

double LoudnessMeter::windowPower(int n) const
{
    if (int(m_recent.size()) < n) return 0.0;
    double sum = 0;
    for (auto it = m_recent.end() - n; it != m_recent.end(); ++it) sum += *it;
    return sum / n;
}

double LoudnessMeter::lufs(double power)
{
    return power > 1e-20 ? -0.691 + 10.0 * std::log10(power) : kSilence;
}

double LoudnessMeter::momentary() const
{
    return m_recent.size() >= 4 ? lufs(windowPower(4)) : kSilence;
}

double LoudnessMeter::shortTerm() const
{
    return m_recent.size() >= 30 ? lufs(windowPower(30)) : kSilence;
}

double LoudnessMeter::integratedOf(const std::vector<double>& blocks)
{
    // Absolutes Gate: -70 LUFS
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0;
    int n = 0;
    for (double p : blocks)
        if (p > absGate) {
            sum += p;
            ++n;
        }
    if (n == 0) return kSilence;
    // Relatives Gate: 10 LU unter dem Mittel der Blöcke über dem absoluten Gate
    const double relGate = sum / n * std::pow(10.0, -10.0 / 10.0);
    const double gate = std::max(absGate, relGate);
    sum = 0;
    n = 0;
    for (double p : blocks)
        if (p > gate) {
            sum += p;
            ++n;
        }
    return n ? lufs(sum / n) : kSilence;
}

double LoudnessMeter::integrated() const
{
    return integratedOf(m_blocks);
}

double LoudnessMeter::range() const
{
    // EBU Tech 3342: Short-term-Werte, absolutes Gate -70 LUFS, relatives -20 LU, dann 95. - 10. Perzentil
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    std::vector<double> kept;
    double sum = 0;
    for (double p : m_shortTerms)
        if (p > absGate) {
            kept.push_back(p);
            sum += p;
        }
    if (kept.size() < 2) return 0.0;
    const double gate = sum / kept.size() * std::pow(10.0, -20.0 / 10.0);
    std::vector<double> l;
    for (double p : kept)
        if (p > gate) l.push_back(lufs(p));
    if (l.size() < 2) return 0.0;
    std::sort(l.begin(), l.end());
    auto pct = [&](double q) {
        const double idx = q * (l.size() - 1);
        const size_t lo = size_t(std::floor(idx)), hi = std::min(l.size() - 1, lo + 1);
        return l[lo] + (l[hi] - l[lo]) * (idx - lo);
    };
    return std::max(0.0, pct(0.95) - pct(0.10));
}
