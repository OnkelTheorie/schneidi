#pragma once
// Tonanalyse für "Audiopegel normalisieren" (wie DaVinci Normalize Audio Levels):
// Spitzenpegel (Sample Peak Program) oder Lautheit nach ITU-R BS.1770-4 (LUFS)

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <functional>
#include <optional>
#include <vector>

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
    std::vector<double> blocks; // 400-ms-Blöcke (LoudnessMeter::integratedOf über mehrere Clips)
};
// Lautheit (ITU-R BS.1770-4, integriert mit Gates) des Clip-Tons, gleiche Regeln wie clipPeakDb
std::optional<Loudness> clipLoudness(const ProjectFormat& format, const Clip& clip,
                                     const std::function<bool(double)>& progress = {});

} // namespace AudioAnalysis
