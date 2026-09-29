#pragma once
// Tonanalyse für "Audiopegel normalisieren" (wie DaVinci Normalize Audio Levels, Modus Sample Peak Program)

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <functional>
#include <optional>

namespace AudioAnalysis {

// Spitzenpegel (Sample Peak, dBFS) des Clip-Tons zwischen In und Out, so wie die Timeline ihn liest
// (Geschwindigkeit/Rückwärts eingerechnet), aber ohne Clip-Lautstärke, Fades, Spur- und Master-Fader.
// Stille = -200. nullopt = kein Ton (Titel, Standbild, Datei fehlt) oder abgebrochen.
// progress(0..1) wird regelmäßig aufgerufen; false bricht ab. Blockiert (Aufrufer zeigt Fortschritt).
std::optional<double> clipPeakDb(const ProjectFormat& format, const Clip& clip,
                                 const std::function<bool(double)>& progress = {});

} // namespace AudioAnalysis
