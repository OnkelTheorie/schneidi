#include "engine/AudioAnalysis.h"

#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <memory>

namespace AudioAnalysis {

std::optional<double> clipPeakDb(const ProjectFormat& format, const Clip& clip,
                                 const std::function<bool(double)>& progress)
{
    if (clip.isTitle() || clip.freeze || !QFileInfo::exists(clip.mediaPath)) return std::nullopt;
    auto profile = makeProfile(format);
    TimelineBuilder builder(*profile);
    Mlt::Producer* src = builder.clipAudioSource(clip);
    if (!src || !src->is_valid()) return std::nullopt;
    const int last = std::min(clip.out, src->get_length() - 1);
    if (last < clip.in) return std::nullopt;
    const double fps = profile->fps();
    src->seek(clip.in);
    float peak = 0.f;
    bool anyAudio = false;
    for (int pos = clip.in; pos <= last; ++pos) {
        if (progress && (pos - clip.in) % 25 == 0 && !progress(double(pos - clip.in) / (last - clip.in + 1)))
            return std::nullopt;
        std::unique_ptr<Mlt::Frame> f(src->get_frame());
        if (!f) break;
        mlt_audio_format fmt = mlt_audio_float; // planar float, nicht auf ±1 begrenzt
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(fps), freq, pos);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (!pcm || samples <= 0 || channels <= 0 || fmt != mlt_audio_float) continue;
        anyAudio = true;
        for (int i = 0, n = samples * channels; i < n; ++i) peak = std::max(peak, std::abs(pcm[i]));
    }
    if (progress) progress(1.0);
    if (!anyAudio) return std::nullopt;
    return peak > 1e-10f ? 20.0 * std::log10(double(peak)) : -200.0;
}

} // namespace AudioAnalysis
