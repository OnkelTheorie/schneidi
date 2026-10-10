#include "engine/AudioAnalysis.h"

#include "core/Loudness.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <memory>

namespace AudioAnalysis {

namespace {

// Pass the clip audio frame by frame to sink as planar float (48 kHz, stereo); a frame that cannot be decoded
// arrives as pcm == nullptr. false = no audio or canceled.
bool decode(const ProjectFormat& format, const Clip& clip, const std::function<bool(double)>& progress,
            const std::function<void(const float*, int samples, int channels, int freq)>& sink)
{
    if (clip.isTitle() || clip.freeze || !QFileInfo::exists(clip.mediaPath)) return false;
    auto profile = makeProfile(format);
    TimelineBuilder builder(*profile);
    Mlt::Producer* src = builder.clipAudioSource(clip);
    if (!src || !src->is_valid()) return false;
    const int last = std::min(clip.out, src->get_length() - 1);
    if (last < clip.in) return false;
    const double fps = profile->fps();
    src->seek(clip.in);
    bool anyAudio = false;
    for (int pos = clip.in; pos <= last; ++pos) {
        if (progress && (pos - clip.in) % 25 == 0 && !progress(double(pos - clip.in) / (last - clip.in + 1)))
            return false;
        std::unique_ptr<Mlt::Frame> f(src->get_frame());
        if (!f) break;
        mlt_audio_format fmt = mlt_audio_float; // planar float, nicht auf ±1 begrenzt
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(fps), freq, pos);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (!pcm || samples <= 0 || channels <= 0 || fmt != mlt_audio_float) {
            sink(nullptr, 0, 0, freq); // keeps frame counting sinks in step
            continue;
        }
        anyAudio = true;
        sink(pcm, samples, channels, freq);
    }
    if (progress) progress(1.0);
    return anyAudio;
}

double toDb(float peak) { return peak > 1e-10f ? 20.0 * std::log10(double(peak)) : -200.0; }

// Loudness of planar float audio blocks (meters fed by measure())
struct Measure {
    LoudnessMeter meter;
    TruePeakMeter truePeak;
    float peak = 0.f;
    double maxMomentary = LoudnessMeter::kSilence, maxShortTerm = LoudnessMeter::kSilence;
    void add(const float* pcm, int samples, int channels, int freq)
    {
        meter.addPlanar(pcm, samples, channels, freq);
        truePeak.addPlanar(pcm, samples, channels, freq);
        for (int i = 0, n = samples * channels; i < n; ++i) peak = std::max(peak, std::abs(pcm[i]));
        maxMomentary = std::max(maxMomentary, meter.momentary());
        maxShortTerm = std::max(maxShortTerm, meter.shortTerm());
    }
    Loudness result() const
    {
        Loudness l;
        l.integrated = meter.integrated();
        l.range = meter.range();
        l.peakDb = toDb(peak);
        l.truePeakDb = truePeak.maxDb();
        l.maxMomentary = maxMomentary;
        l.maxShortTerm = maxShortTerm;
        l.blocks = meter.blocks();
        return l;
    }
};

} // namespace

std::optional<double> clipPeakDb(const ProjectFormat& format, const Clip& clip,
                                 const std::function<bool(double)>& progress)
{
    float peak = 0.f;
    const bool ok = decode(format, clip, progress, [&](const float* pcm, int samples, int channels, int) {
        for (int i = 0, n = samples * channels; i < n; ++i) peak = std::max(peak, std::abs(pcm[i]));
    });
    if (!ok) return std::nullopt;
    return toDb(peak);
}

std::optional<Loudness> clipLoudness(const ProjectFormat& format, const Clip& clip,
                                     const std::function<bool(double)>& progress)
{
    Measure m;
    const bool ok = decode(format, clip, progress, [&](const float* pcm, int samples, int channels, int freq) {
        if (pcm) m.add(pcm, samples, channels, freq);
    });
    if (!ok) return std::nullopt;
    return m.result();
}

std::optional<Loudness> timelineLoudness(const ProjectFormat& format, const Timeline& timeline, int from, int to,
                                         const std::function<bool(double)>& progress)
{
    if (to <= from) return std::nullopt;
    auto profile = makeProfile(format);
    TimelineBuilder builder(*profile);
    builder.setSubtitles(false);
    const std::unique_ptr<Mlt::Tractor> tractor = builder.build(timeline);
    if (!tractor || !tractor->is_valid()) return std::nullopt;
    const double fps = profile->fps();
    Measure m;
    tractor->seek(from);
    for (int pos = from; pos < to; ++pos) {
        if (progress && (pos - from) % 25 == 0 && !progress(double(pos - from) / (to - from))) return std::nullopt;
        std::unique_ptr<Mlt::Frame> f(tractor->get_frame());
        if (!f) break;
        mlt_audio_format fmt = mlt_audio_float;
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(fps), freq, pos);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (pcm && samples > 0 && channels > 0 && fmt == mlt_audio_float) m.add(pcm, samples, channels, freq);
    }
    if (progress) progress(1.0);
    return m.result();
}

std::optional<std::vector<float>> clipFramePeaks(const ProjectFormat& format, const Clip& clip,
                                                 const std::function<bool(double)>& progress)
{
    std::vector<float> peaks;
    const bool ok = decode(format, clip, progress, [&](const float* pcm, int samples, int channels, int) {
        float peak = 0.f;
        for (int i = 0, n = samples * channels; i < n; ++i) peak = std::max(peak, std::abs(pcm[i]));
        peaks.push_back(peak);
    });
    if (!ok) return std::nullopt;
    return peaks;
}

} // namespace AudioAnalysis
