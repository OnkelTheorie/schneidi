#pragma once
// Tonanalyse für "Audiopegel normalisieren" (wie DaVinci Normalize Audio Levels):
// Spitzenpegel (Sample Peak Program) oder Lautheit nach ITU-R BS.1770-4 (LUFS)

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Mlt {
class Profile;
}
class TimelineBuilder;

namespace AudioAnalysis {

// Spitzenpegel (Sample Peak, dBFS) des Clip-Tons zwischen In und Out, so wie die Timeline ihn liest
// (Geschwindigkeit/Rückwärts eingerechnet), aber ohne Clip-Lautstärke, Fades, Spur- und Master-Fader.
// Stille = -200. nullopt = kein Ton (Titel, Standbild, Datei fehlt) oder abgebrochen.
// progress(0..1) wird regelmäßig aufgerufen; false bricht ab. Blockiert (Aufrufer zeigt Fortschritt).
std::optional<double> clipPeakDb(const ProjectFormat& format, const Clip& clip,
                                 const std::function<bool(double)>& progress = {});

struct Loudness {
    double integrated = -200; // LUFS (Stille = -200)
    double range = 0;         // Loudness Range (LU)
    double peakDb = -200;     // Sample Peak (dBFS)
    double truePeakDb = -200;   // True Peak (dBTP, 4x oversampled like BS.1770-4)
    double maxMomentary = -200; // loudest 400 ms (LUFS)
    double maxShortTerm = -200; // loudest 3 s (LUFS)
    std::vector<double> blocks; // 400-ms-Blöcke (LoudnessMeter::integratedOf über mehrere Clips)
};
// Lautheit (ITU-R BS.1770-4, integriert mit Gates) des Clip-Tons, gleiche Regeln wie clipPeakDb
std::optional<Loudness> clipLoudness(const ProjectFormat& format, const Clip& clip,
                                     const std::function<bool(double)>& progress = {});

// Loudness of the timeline mix [from, to) as it is exported (clip/track/master levels, mute/solo, limiter)
std::optional<Loudness> timelineLoudness(const ProjectFormat& format, const Timeline& timeline, int from, int to,
                                         const std::function<bool(double)>& progress = {});

// The same for measuring one timeline several times (fader changes): keeps the opened files between the
// measurements (opening the producers of an edit with speed ramps takes seconds)
class MixMeter {
public:
    explicit MixMeter(const ProjectFormat& format);
    ~MixMeter();
    std::optional<Loudness> measure(const Timeline& timeline, int from, int to,
                                    const std::function<bool(double)>& progress = {});

private:
    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<TimelineBuilder> m_builder;
};

// Sample peak (linear, all channels) of every timeline frame of the clip, same rules as clipPeakDb
// (silence detection in schneidi-cli). Index 0 = first frame of the clip.
std::optional<std::vector<float>> clipFramePeaks(const ProjectFormat& format, const Clip& clip,
                                                 const std::function<bool(double)>& progress = {});

// Sample peak (linear) of every frame of the timeline mix [from, to) as it is exported. Index 0 = frame `from`.
std::optional<std::vector<float>> timelineFramePeaks(const ProjectFormat& format, const Timeline& timeline, int from,
                                                     int to, const std::function<bool(double)>& progress = {});

} // namespace AudioAnalysis
