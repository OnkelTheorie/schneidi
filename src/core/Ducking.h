#pragma once
// Ducking (docs/archiv/plan-0.8.md): lower the music while other tracks (voice, game) have sound, with volume keyframes
// that stay editable on the volume line. Same logic for the app (DuckingDialog -> Editor::duck) and the CLI op `duck`;
// the sound ranges come from per-frame peaks of the 'under' tracks (AudioAnalysis::soundRanges).

#include "core/Types.h"

#include <QPair>
#include <QVector>
#include <vector>

namespace Ducking {

using Ranges = QVector<QPair<int, int>>; // [from, to) timeline frames

struct Params {
    double amountDb = -12.0;    // how far down
    double thresholdDb = -35.0; // what counts as sound on the 'under' tracks (sample peak, dBFS)
    int attack = 0;             // frames: ramp down before a sound
    int release = 0;            // frames: ramp up after it
    int hold = 0;               // frames: shorter pauses stay ducked
};

// -12 dB, -35 dBFS, attack 0.15 s, release 0.4 s, hold 0.5 s (rounded to frames)
Params defaults(double fps);

// Ranges where peaks (one per frame, linear; index 0 = timeline frame `first`) reach the threshold; pauses up to
// hold + attack + release are bridged
Ranges soundRanges(const std::vector<float>& peaks, int first, const Params& p);

// Replaces the clip's volume keyframes by the ducking for `ranges` (ramp of `attack` before and `release` after each
// sound, from the clip volume to clip volume + amount, never below -∞). false = no sound on the clip (no keyframes
// left, the clip volume stays).
bool apply(Clip& c, const Ranges& ranges, const Params& p);

} // namespace Ducking
