// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Denoise.h"

#include "core/EffectRegistry.h"
#include "core/Fft.h"

#include <Mlt.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>

namespace {

constexpr double kOverSubtract = 2.0;    // subtract more than the mean noise (fewer remaining noise peaks)
constexpr double kMinimumBias = 2.0;     // the minimum of the long-smoothed spectrum lies below its mean
constexpr double kNoiseRiseDbPerSec = 4; // tracked noise may rise this fast (falls at once)

int windowSize(int sampleRate)
{
    // ~10.7 ms (512 at 48 kHz): enough frequency resolution for hiss/hum, little delay
    int n = 128;
    while (n * 2 <= 4096 && std::abs(n * 2 - sampleRate * 0.0107) < std::abs(n - sampleRate * 0.0107)) n *= 2;
    return n;
}

// Filter state: one Processor per channel, continuous over the clip's cuts
struct State {
    std::mutex mutex;
    int channels = 0, frequency = 0;
    std::vector<std::unique_ptr<Denoise::Processor>> processors;
    mlt_position last = -10;
    std::vector<float> lastOut; // the same frame asked again (preview refresh) -> same sound
};

int getAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels, int* samples)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    const mlt_audio_format wanted = *format;
    *format = mlt_audio_float;
    const int error = mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    if (error || !*buffer || *channels <= 0 || *samples <= 0 || *frequency <= 0) return error;
    if (*format != mlt_audio_float
        && (!frame->convert_audio || frame->convert_audio(frame, buffer, format, mlt_audio_float) || *format != mlt_audio_float))
        return 0; // unknown format: unchanged
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    auto* st = static_cast<State*>(mlt_properties_get_data(props, "_state", nullptr));
    if (!st) return 0;
    const double reduction = mlt_properties_get_int(props, "reduction") / 100.0;
    const double floor = mlt_properties_get_int(props, "floor") / 100.0;
    const bool track = mlt_properties_get_int(props, "track") != 0;
    {
        const std::lock_guard<std::mutex> lock(st->mutex);
        const int ch = *channels, n = *samples;
        auto* pcm = static_cast<float*>(*buffer);
        const size_t total = size_t(ch) * size_t(n);
        const mlt_position pos = mlt_frame_get_position(frame);
        if (pos == st->last && st->lastOut.size() == total) {
            std::copy(st->lastOut.begin(), st->lastOut.end(), pcm);
        } else {
            if (st->channels != ch || st->frequency != *frequency) {
                st->processors.clear();
                for (int c = 0; c < ch; ++c) st->processors.push_back(std::make_unique<Denoise::Processor>(*frequency));
                st->channels = ch;
                st->frequency = *frequency;
            } else if (pos != st->last + 1) { // jumped: start over (not with the sound of another place)
                for (auto& p : st->processors) p->reset();
            }
            for (int c = 0; c < ch; ++c) {
                st->processors[size_t(c)]->setParams(reduction, floor, track);
                st->processors[size_t(c)]->process(pcm + size_t(c) * size_t(n), n);
            }
            st->lastOut.assign(pcm, pcm + total);
            st->last = pos;
        }
    }
    if (wanted != mlt_audio_none && wanted != *format && frame->convert_audio)
        frame->convert_audio(frame, buffer, format, wanted);
    return 0;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(getAudio));
    return frame;
}

} // namespace

namespace Denoise {

Processor::Processor(int sampleRate) : m_size(windowSize(sampleRate)), m_hop(m_size / 4)
{
    m_window.resize(size_t(m_size));
    for (int i = 0; i < m_size; ++i)
        m_window[size_t(i)] = float(std::sqrt(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / m_size))); // periodic
    m_re.resize(size_t(m_size));
    m_im.resize(size_t(m_size));
    m_rate = sampleRate;
    reset();
}

Processor::~Processor() = default;

void Processor::setParams(double reductionDb, double floorDb, bool track)
{
    m_reduction = std::clamp(reductionDb, 0.0, 60.0);
    m_floor = std::clamp(floorDb, -100.0, 0.0);
    m_track = track;
}

void Processor::reset()
{
    m_in.assign(size_t(m_size), 0.f);
    m_ola.assign(size_t(m_size), 0.f);
    m_out.assign(size_t(m_hop), 0.f); // with the window this makes the delay exactly m_size samples
    m_smooth.assign(size_t(m_size / 2 + 1), -1.0);
    m_long.assign(size_t(m_size / 2 + 1), -1.0);
    m_noise.assign(size_t(m_size / 2 + 1), -1.0);
    m_gain.assign(size_t(m_size / 2 + 1), 1.0);
    m_pending = 0;
    m_seen = 0;
}

void Processor::process(float* data, int n)
{
    size_t read = 0;
    for (int i = 0; i < n; ++i) {
        m_in[size_t(m_size - m_hop + m_pending)] = data[i];
        ++m_seen;
        if (++m_pending == m_hop) hop();
        if (read < m_out.size()) data[i] = m_out[read++];
        else data[i] = 0.f; // cannot happen (m_out starts with one hop)
    }
    m_out.erase(m_out.begin(), m_out.begin() + std::ptrdiff_t(read));
}

void Processor::hop()
{
    const size_t N = size_t(m_size), H = size_t(m_hop), bins = N / 2 + 1;
    for (size_t i = 0; i < N; ++i) {
        m_re[i] = m_in[i] * m_window[i];
        m_im[i] = 0;
    }
    Fft::transform(m_re, m_im, false);

    const double gmin = std::pow(10.0, -m_reduction / 20.0);
    const bool full = m_seen >= long(N); // the window holds real sound only (tracking starts then)
    const double up = std::pow(10.0, kNoiseRiseDbPerSec / 10.0 * double(H) / m_rate);
    double sumW2 = 0;
    for (float w : m_window) sumW2 += double(w) * w;
    const double fixedNoise = std::pow(10.0, m_floor / 10.0) * sumW2; // white noise of that RMS level, per bin
    std::vector<double> g(bins);
    for (size_t k = 0; k < bins; ++k) {
        const double p = m_re[k] * m_re[k] + m_im[k] * m_im[k];
        double& s = m_smooth[k];   // short smoothing: what the gain follows
        double& l = m_long[k];     // long smoothing: what the noise tracking follows (small spread)
        s = s < 0 ? p : 0.5 * s + 0.5 * p;
        l = l < 0 ? p : 0.95 * l + 0.05 * p;
        double noise = fixedNoise;
        if (m_track) {
            double& tracked = m_noise[k]; // minimum statistics: falls at once, rises slowly
            if (full) tracked = tracked < 0 || l < tracked ? l : tracked * up;
            noise = tracked < 0 ? -1.0 : tracked * kMinimumBias;
        }
        double gain = 1.0;
        if (noise >= 0 && s > 1e-20) gain = std::sqrt(std::max(gmin * gmin, 1.0 - kOverSubtract * noise / s));
        else if (noise >= 0) gain = gmin;
        // open at once, close smoothly (less "musical noise")
        gain = gain > m_gain[k] ? gain : 0.5 * m_gain[k] + 0.5 * gain;
        m_gain[k] = gain;
        g[k] = gain;
    }
    for (size_t k = 0; k < bins; ++k) { // a little smoothing across frequencies
        const double l = g[k > 0 ? k - 1 : k], r = g[k + 1 < bins ? k + 1 : k];
        const double gain = std::max(gmin, 0.25 * l + 0.5 * g[k] + 0.25 * r);
        m_re[k] *= gain;
        m_im[k] *= gain;
        if (k > 0 && k < N / 2) {
            m_re[N - k] *= gain;
            m_im[N - k] *= gain;
        }
    }
    Fft::transform(m_re, m_im, true);
    // sqrt-Hann twice = Hann; at 75 % overlap the Hann windows add up to 2
    const double scale = 0.5 / double(N);
    for (size_t i = 0; i < N; ++i) m_ola[i] += float(m_re[i] * scale * m_window[i]);
    m_out.insert(m_out.end(), m_ola.begin(), m_ola.begin() + std::ptrdiff_t(H));
    std::move(m_ola.begin() + std::ptrdiff_t(H), m_ola.end(), m_ola.begin());
    std::fill(m_ola.end() - std::ptrdiff_t(H), m_ola.end(), 0.f);
    std::move(m_in.begin() + std::ptrdiff_t(H), m_in.end(), m_in.begin());
    m_pending = 0;
}

std::shared_ptr<Mlt::Filter> make(Mlt::Profile& profile, const Clip& c)
{
    const EffectInstance* e = EffectRegistry::instance(c, EffectId);
    if (!e || !e->enabled) return nullptr;
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    mlt_service_set_profile(MLT_FILTER_SERVICE(f), profile.get_profile());
    f->process = process;
    mlt_properties p = MLT_FILTER_PROPERTIES(f);
    auto hundredths = [&](const char* key) {
        return int(std::lround(EffectRegistry::value(c, EffectId, key).toDouble() * 100.0));
    };
    mlt_properties_set_int(p, "reduction", hundredths("reduction"));
    mlt_properties_set_int(p, "floor", hundredths("floor"));
    mlt_properties_set_int(p, "track", EffectRegistry::value(c, EffectId, "track").toBool() ? 1 : 0);
    mlt_properties_set_data(p, "_state", new State, 0, [](void* s) { delete static_cast<State*>(s); }, nullptr);
    auto filter = std::make_shared<Mlt::Filter>(f); // holds its own reference
    mlt_filter_close(f);
    return filter;
}

} // namespace Denoise
