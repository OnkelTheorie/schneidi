// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Track automation (0.23): a volume curve on an audio track over timeline frames (Track::volumeAuto). Pure functions,
// no undo or signals (Editor makes the changes undo steps).
//
// The curve's values are dB on top of the track fader: what a track plays at frame f is fader + valueAt(f). Moving the
// fader trims the whole curve; an empty curve is the fader alone. Between points the value runs linearly (in dB, like
// clip volume keyframes), before the first and after the last point their value holds.
//
// The curve belongs to the timeline's time, not to clips: moving a clip leaves it. Ripple edits (ripple delete, insert,
// ripple trim, retime, closing gaps) move it with the time around it through the same shifts the clips get
// (TimelineOps::shiftFrom/rippleTracks): time taken out takes its points along (the value on both sides stays), time
// put in holds the value at the insert point. A locked track keeps its curve as it keeps its clips.

#include "core/Types.h"

#include <QJsonArray>
#include <QPair>
#include <QVector>

#include <functional>

namespace Automation {

// Range of a value (dB on top of the fader): enough to reach -inf from the loudest fader and back
inline constexpr double kMinDb = kMinVolumeDb - kMaxVolumeDb; // -72
inline constexpr double kMaxDb = kMaxVolumeDb - kMinVolumeDb; // +72

// Value at a timeline frame (fractions allowed); 0 for an empty curve
double valueAt(const AutoCurve& curve, double frame);
// What the track plays at a frame: fader + curve, clamped to kMinVolumeDb..kMaxVolumeDb (kMinVolumeDb = silent)
double levelAt(const Track& track, double frame);

// Sorted by frame, one point per frame (the last one given wins), frames >= 0, values clamped to kMinDb..kMaxDb
AutoCurve normalized(AutoCurve curve);

// Replaces the points in [from, to] (timeline frames) by `inside` (points outside that range are dropped). Outside
// the range the curve stays as it was: a point with the old value right before `from` and right after `to` holds it.
void replaceRange(AutoCurve& curve, int from, int to, const AutoCurve& inside);

// Ripple: shifts as in TimelineOps::rippleTracks - (frame, offset) pairs; a point moves by the sum of the offsets
// whose frame is <= its own. A negative offset takes the time [frame + offset, frame) out (its points go; the values
// at both edges stay), a positive one puts `offset` frames in at `frame` that hold the value there.
void ripple(AutoCurve& curve, const QVector<QPair<int, int>>& shifts);

// Every point's frame through f (frame rate change); points that land on the same frame keep the last one
void retime(AutoCurve& curve, const std::function<int(int)>& f);

// Project file / CLI: [[frame, db], ...] (db rounded to 1/1000)
QJsonArray toJson(const AutoCurve& curve);
AutoCurve fromJson(const QJsonArray& a);

} // namespace Automation
