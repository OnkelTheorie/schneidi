// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Drawing a volume curve with the pen (Studio page, 0.23): a stroke over an audio track becomes volume keyframes of the
// clips under it. Pure functions on Clip::keys (no undo, no signals; Editor::drawVolume makes it one undo step).
//
// A stroke is the pointer's path in timeline frames and dB, in the order it was drawn. It may go back and forth: what
// was drawn last at a frame wins. The path is resampled to one value per frame, smoothed (moving average, `smoothing`
// frames to each side) and thinned to the keyframes that keep it within kTolerance dB (Ramer-Douglas-Peucker). Outside
// the stroke the curve stays as it was: a key with the old value just before and just after the stroke holds it.
//
// The scale of the volume line (lanes, faders): bottom quarter -inf..-20 dB, middle half -20..0 dB, top quarter
// 0..+12 dB - the same as the mixer's faders and the timeline's volume line.

#include "core/Types.h"

#include <QVector>

namespace VolumeDraw {

struct Point {
    double frame = 0; // timeline frame (fractions allowed)
    double db = 0;    // clip volume in dB (clamped to kMinVolumeDb..kMaxVolumeDb)
    bool operator==(const Point&) const = default;
};

inline constexpr double kTolerance = 0.25; // dB the thinned curve may differ from the smoothed stroke

// Volume line scale: 0 = bottom (-inf), 1 = top (+12 dB) and back
double posOf(double db);
double dbAt(double pos);

// One value per whole timeline frame from frame `first` on (resampled, later strokes over earlier ones); empty for an
// empty stroke
QVector<double> resample(const QVector<Point>& stroke, int* first);
// Moving average, `radius` frames to each side (0 = unchanged); the ends average over what is there
QVector<double> smooth(const QVector<double>& values, int radius);
// Indices of the values to keep as keyframes (always the first and the last), so that the straight lines between
// them stay within `tolerance` of every value
QVector<int> thin(const QVector<double>& values, double tolerance = kTolerance);

// Frames a stroke must span (first to last drawn frame) to change anything: a click without dragging, or a stroke
// straight up and down in place, would leave a 1-frame dip or spike (old level, new level, old level)
inline constexpr int kMinSpan = 2;
// True when the stroke spans at least kMinSpan frames
bool spansEnough(const QVector<Point>& stroke);

// Writes the stroke into clip `c` (placed on the timeline: c.start), only where the clip is under it. Returns false
// when the stroke does not touch the clip or is too short (spansEnough; nothing changed). The clip's keyframes under the stroke are replaced.
bool apply(Clip& c, const QVector<Point>& stroke, int smoothing);

} // namespace VolumeDraw
