#pragma once

#include "core/Types.h"

#include <mlt++/Mlt.h>
#include <memory>

// Speed Ramp (Clip::ramp) in der Engine. Positionen beider Producer = Material-Frames des Clips (wie bei timewarp),
// d. h. Clip::in/out und Keyframe-Filter funktionieren unverändert.
namespace RampProducer {

// Bild: eigener Producer, der je Frame die Quellstelle aus der RetimeMap holt (ein Decoder für alle Tempi,
// weiche Übergänge Frame für Frame). `inner` = normaler Producer der Datei im Projektprofil.
std::unique_ptr<Mlt::Producer> video(Mlt::Profile& profile, std::unique_ptr<Mlt::Producer> inner, const Clip& c);

// Ton: Playlist aus timewarp-Ausschnitten, je Abschnitt konstantes Tempo; weiche Übergänge in kurzen Stufen.
// Tonhöhe halten (warp_pitch) nur in Abschnitten mit festem Tempo: der Pitch-Filter braucht nach jedem Wechsel
// 1–2 Frames zum Einschwingen, in den kurzen Stufen wäre der Ton sonst voller Lücken. 100 % vorwärts = normaler
// Producer (kein Einschwingen). nullptr = Datei nicht lesbar.
std::unique_ptr<Mlt::Producer> audio(Mlt::Profile& profile, const QString& file, const Clip& c);

// Stufen des Tons (auch für Tests): Material [m0, m1) mit konstantem Tempo ab Quellstelle s0
struct AudioStep {
    int m0, m1;
    double s0, speed;
    bool smooth = false; // Stufe eines weichen Übergangs (ohne Tonhöhenkorrektur)
};
QVector<AudioStep> audioSteps(const Clip& c, int fileLength);

} // namespace RampProducer
