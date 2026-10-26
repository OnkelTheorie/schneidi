#pragma once
// Audio analysis for "Normalize audio levels" (like DaVinci Normalize Audio Levels):
// peak level (Sample Peak Program) or loudness per ITU-R BS.1770-4 (LUFS), plus frame peaks for silence detection
// and ducking. The sound comes from the own audio engine (Audio::Mixer, the same mix as the export) or, as long as
// it is the chosen path (Audio::ownPath), through MLT.

#include "core/Ducking.h"
#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QHash>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Mlt {
class Profile;
}
class TimelineBuilder;

namespace AudioAnalysis {

// Peak level (sample peak, dBFS) of the clip audio between In and Out, as the timeline reads it
// (speed/reverse included), but without clip volume, fades, track and master faders.
// Silence = -200. nullopt = no audio (title, still, file missing) or canceled.
// progress(0..1) is called regularly; false cancels. Blocks (caller shows progress).
std::optional<double> clipPeakDb(const ProjectFormat& format, const Clip& clip,
                                 const std::function<bool(double)>& progress = {});

struct Loudness {
    double integrated = -200; // LUFS (silence = -200)
    double range = 0;         // Loudness Range (LU)
    double peakDb = -200;     // Sample Peak (dBFS)
    double truePeakDb = -200;   // True Peak (dBTP, 4x oversampled like BS.1770-4)
    double maxMomentary = -200; // loudest 400 ms (LUFS)
    double maxShortTerm = -200; // loudest 3 s (LUFS)
    std::vector<double> blocks; // 400 ms blocks (LoudnessMeter::integratedOf over several clips)
};
// Loudness (ITU-R BS.1770-4, integrated with gates) of the clip audio, same rules as clipPeakDb
std::optional<Loudness> clipLoudness(const ProjectFormat& format, const Clip& clip,
                                     const std::function<bool(double)>& progress = {});

// "Normalize audio levels": the level of every clip in `ids` (sample peak dBFS, or with loudness the integrated
// LUFS), without its volume; with loudness also the true peaks and the loudness of all clips together (the reference
// for "relative"). Clips without sound are missing. nullopt = canceled (progress over all clips returned false).
// App dialog and CLI op both measure with this.
struct ClipLevels {
    QHash<int, double> levels;     // clip id -> dBFS or LUFS
    QHash<int, double> truePeaks;  // loudness only: clip id -> dBTP
    std::optional<double> shared;  // loudness only: all clips as one
};
std::optional<ClipLevels> measureClips(const ProjectFormat& format, const Timeline& timeline, const QVector<int>& ids,
                                       bool loudness, const std::function<bool(double)>& progress = {});

// Loudness of the timeline mix [from, to) as it is exported (clip/track/master levels, mute/solo, limiter).
// framePeaks: also the sample peak of every frame like timelineFramePeaks, in the same pass (the check before export)
std::optional<Loudness> timelineLoudness(const ProjectFormat& format, const Timeline& timeline, int from, int to,
                                         const std::function<bool(double)>& progress = {},
                                         std::vector<float>* framePeaks = nullptr);

// The same for measuring one timeline several times (fader changes). MLT audio path: keeps the opened files between
// the measurements (opening the producers of an edit with speed ramps takes seconds); the own engine opens a file in
// about a millisecond and mixes each measurement afresh.
class MixMeter {
public:
    explicit MixMeter(const ProjectFormat& format);
    ~MixMeter();
    std::optional<Loudness> measure(const Timeline& timeline, int from, int to,
                                    const std::function<bool(double)>& progress = {},
                                    std::vector<float>* framePeaks = nullptr);

private:
    ProjectFormat m_format;
    std::unique_ptr<Mlt::Profile> m_profile; // MLT audio path only, made on first use
    std::unique_ptr<TimelineBuilder> m_builder;
};

// Sample peak (linear, all channels) of every timeline frame of the clip, same rules as clipPeakDb
// (silence detection in schneidi-cli). Index 0 = first frame of the clip.
std::optional<std::vector<float>> clipFramePeaks(const ProjectFormat& format, const Clip& clip,
                                                 const std::function<bool(double)>& progress = {});

// Sample peak (linear) of every frame of the timeline mix [from, to) as it is exported. Index 0 = frame `from`.
std::optional<std::vector<float>> timelineFramePeaks(const ProjectFormat& format, const Timeline& timeline, int from,
                                                     int to, const std::function<bool(double)>& progress = {});

// The sound itself, timeline frame by timeline frame, as planar float (48 kHz, stereo): of the clip as
// clipFramePeaks reads it, or of the timeline mix [from, to) as timelineFramePeaks (schneidi-cli `beats`).
// pcm == nullptr = a frame that could not be decoded (MLT path; keeps counting sinks in step). false = no audio,
// could not mix or canceled.
using SoundSink = std::function<void(const float* pcm, int samples, int channels, int rate)>;
bool clipSound(const ProjectFormat& format, const Clip& clip, const SoundSink& sink,
               const std::function<bool(double)>& progress = {});
bool timelineSound(const ProjectFormat& format, const Timeline& timeline, int from, int to, const SoundSink& sink,
                   const std::function<bool(double)>& progress = {});

// Ducking: where the audio tracks `under` (indices; the others are muted, solo ignored) have sound in the timeline
// mix (render timeline, see Project::renderTimeline). nullopt = could not mix or canceled; no tracks / empty = no range.
std::optional<Ducking::Ranges> soundRanges(const ProjectFormat& format, const Timeline& timeline, const QVector<int>& under,
                                           const Ducking::Params& params,
                                           const std::function<bool(double)>& progress = {});

} // namespace AudioAnalysis
