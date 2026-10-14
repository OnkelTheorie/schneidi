#include "core/Equalizer.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace {
constexpr double kPi = 3.14159265358979323846;
}

bool Equalizer::coefs(const EqBand& band, int rate, Coefs* out)
{
    using K = EqBand::Kind;
    const bool filter = band.kind == K::HighPass || band.kind == K::LowPass;
    if (!band.on || rate <= 0 || (!filter && band.gainDb == 0) || !std::isfinite(band.freq) || !std::isfinite(band.gainDb)
        || !std::isfinite(band.q))
        return false;
    // Stay clear of 0 Hz and Nyquist (the bilinear transform gets unstable/meaningless there)
    const double f = std::clamp(band.freq, 10.0, 0.45 * rate);
    const double q = std::clamp(band.q, kMinQ, kMaxQ);
    const double w = 2 * kPi * f / rate, cw = std::cos(w), alpha = std::sin(w) / (2 * q);
    const double A = std::pow(10.0, std::clamp(band.gainDb, kMinGain, kMaxGain) / 40), sa = 2 * std::sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    switch (band.kind) {
    case K::HighPass:
        b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2;
        a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case K::LowPass:
        b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2;
        a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case K::LowShelf:
        b0 = A * ((A + 1) - (A - 1) * cw + sa), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - sa;
        break;
    case K::HighShelf:
        b0 = A * ((A + 1) + (A - 1) * cw + sa), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - sa;
        break;
    case K::Bell:
    default:
        b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A;
        a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A;
        break;
    }
    *out = {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    return true;
}

double Equalizer::responseDb(const TrackEq& eq, double hz, int rate)
{
    if (!eq.on || rate <= 0) return 0;
    const std::complex<double> z1 = std::polar(1.0, -2 * kPi * std::clamp(hz, 0.0, rate / 2.0) / rate), z2 = z1 * z1;
    double db = 0;
    for (const EqBand& b : eq.bands) {
        Coefs c;
        if (!coefs(b, rate, &c)) continue;
        const double num = std::abs(c.b0 + c.b1 * z1 + c.b2 * z2), den = std::abs(1.0 + c.a1 * z1 + c.a2 * z2);
        db += 20 * std::log10(std::max(num, 1e-12) / std::max(den, 1e-12));
    }
    return db;
}

TrackEq Equalizer::clamped(TrackEq eq)
{
    const TrackEq def;
    for (size_t i = 0; i < eq.bands.size(); ++i) {
        EqBand& b = eq.bands[i];
        const EqBand& d = def.bands[i];
        b.kind = d.kind;
        b.freq = std::isfinite(b.freq) ? std::clamp(b.freq, kMinFreq, kMaxFreq) : d.freq;
        b.gainDb = std::isfinite(b.gainDb) ? std::clamp(b.gainDb, kMinGain, kMaxGain) : d.gainDb;
        b.q = std::isfinite(b.q) ? std::clamp(b.q, kMinQ, kMaxQ) : d.q;
    }
    return eq;
}

void Equalizer::setEq(const TrackEq& eq)
{
    m_eq = eq;
    if (m_rate > 0 && m_channels > 0) configure(m_channels, m_rate);
}

void Equalizer::reset()
{
    for (auto& s : m_state) s = {0, 0};
}

void Equalizer::configure(int channels, int rate)
{
    std::vector<Coefs> active;
    if (m_eq.on)
        for (const EqBand& b : m_eq.bands) {
            Coefs c;
            if (coefs(b, rate, &c)) active.push_back(c);
        }
    // Same bands and format: keep the filter state (live change while playing, no click)
    const bool keep = channels == m_channels && rate == m_rate && active.size() == m_active.size();
    m_active = std::move(active);
    m_channels = channels;
    m_rate = rate;
    if (!keep) m_state.assign(m_active.size() * size_t(channels), {0, 0});
}

void Equalizer::processPlanar(float* data, int samples, int channels, int rate)
{
    if (!m_eq.on || !data || samples <= 0 || channels <= 0 || rate <= 0) return;
    if (channels != m_channels || rate != m_rate) configure(channels, rate);
    for (size_t b = 0; b < m_active.size(); ++b) {
        const Coefs& k = m_active[b];
        for (int c = 0; c < channels; ++c) {
            std::array<double, 2>& s = m_state[b * size_t(channels) + size_t(c)];
            float* x = data + size_t(c) * samples;
            double s1 = s[0], s2 = s[1];
            for (int i = 0; i < samples; ++i) {
                const double in = x[i];
                const double out = k.b0 * in + s1;
                s1 = k.b1 * in - k.a1 * out + s2;
                s2 = k.b2 * in - k.a2 * out;
                x[i] = float(out);
            }
            // Denormals/blow-ups never stay in the state
            s[0] = std::isfinite(s1) && std::abs(s1) > 1e-30 ? s1 : 0;
            s[1] = std::isfinite(s2) && std::abs(s2) > 1e-30 ? s2 : 0;
        }
    }
}
