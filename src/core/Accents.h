// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Accents on a hit (a kill, a beat, a drop): zoom punch, exposure flash, position shake and mixes of them, written
// as ordinary keyframes (zoom x/y, grade exposure, x/y) that stay editable in the Inspector and the curve editor.
// One call writes every hit of a clip, so an AI needs one op for a whole edit instead of a hundred keyframe ops
// (docs archive 2026-10-26: 119 ops / 28 KB for the kill effects of one TikTok). Used by op 'accent' and the
// timeline clip menu. Pure functions on a Clip, without undo (Editor::modifyClips does that).

#include "core/Types.h"

#include <QSizeF>
#include <QString>
#include <QVector>

namespace Accents {

// Strength of each part at the hit; the hit fades back over `length` seconds
struct Amounts {
    double punch = 0;     // zoom up by this share (0.15 = 15 % bigger), back with a slow landing
    double flash = 0;     // exposure up by these stops (Color page grade; +2 ≈ 1.8 x brighter)
    double shake = 0;     // position jumps of up to this share of the frame's shorter side, dying down
    double length = 0.25; // seconds from the hit until it has faded
    bool any() const { return punch != 0 || flash != 0 || shake != 0; }
    Amounts scaled(double strength) const { return {punch * strength, flash * strength, shake * strength, length}; }
};

struct Preset {
    const char* id;          // op 'accent' preset and the menu
    const char* name;        // shown in the app (N_)
    const char* description; // for AIs (`effects`, help), English
    Amounts amounts;
};
const QVector<Preset>& presets();
const Preset* find(const QString& id);

// Seconds between two flashes at least: a burst of kills (three in 0.8 s) strobes - tiring to watch and against the
// photosensitivity guidelines (at most 3 flashes a second). Punch and shake still mark every hit.
constexpr double kFlashGap = 0.5;
// The hits (frames) that get a flash: the first one, then each one at least kFlashGap after the last flash
QVector<int> flashHits(QVector<int> hits, double fps);

// Writes the accents at clip frames `hits` (0 = the clip's first frame; hits outside the clip only count for the
// spacing of flashes) into c and returns how many landed. Values build on what the clip has there (a zoom animation
// keeps its course, a grade its exposure) and close hits add up. The value starts rising ~30 ms before the hit and
// peaks on it. fps = timeline frame rate, frame = timeline size (shake amplitude). A missing grade is added for the
// flash.
int apply(Clip& c, const QVector<int>& hits, const Amounts& a, double fps, QSizeF frame);

} // namespace Accents
