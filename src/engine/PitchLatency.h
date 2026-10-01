#pragma once

#include <mlt++/Mlt.h>

// Verzögerung des Tons durch „Tonhöhe halten“ (MLT timewarp mit warp_pitch -> Rubberband-Filter rbpitch). MLT gleicht
// sie nicht aus: der Ton läuft je nach Tempo 2–10 Frames hinter dem Bild. Sie ist je Tempo/Abtastrate fest (auch nach
// Sprüngen), hängt aber von der Rubberband-Version ab -> einmal zur Laufzeit messen statt Tabelle.
// Ausgleich: den Ton um so viele Material-Frames früher aus dem timewarp-Producer holen (Cut-in + frames()).
namespace PitchLatency {

// Verzögerung in Timeline-Frames bei Tempo |speed| (1 oder ohne Tonhöhe: 0). Erstes Mal ~50 ms (Messung mit einem
// Klick in einer temporären WAV, mit und ohne Tonhöhe), danach aus dem Cache. Thread-sicher.
int frames(Mlt::Profile& profile, double speed, int sampleRate = 48000);

// timewarp-Producer um `lead` Frames verlängern, damit Cuts mit Vorlauf bis zum Dateiende reichen (sonst kürzt MLT
// den Cut). Gelesen wird dort hinter dem Dateiende, hörbar wäre das erst nach dem Clipende – also nie. Nur „length“,
// nicht „out“: rückwärts rechnet timewarp von „out“ aus.
void extend(Mlt::Producer& p, int lead);

// Abtastrate des ersten Ton-Streams eines Producers (für frames()), 48000 wenn unbekannt
int sampleRateOf(Mlt::Producer& p);

} // namespace PitchLatency
