// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Track automation (0.23): a volume and a pan curve on an audio track over timeline frames (Track::volumeAuto,
// Track::panAuto). Pure functions, no undo or signals (Editor makes the changes undo steps).
//
// The pan curve works like the volume curve: its values are added to the pan knob (-100..+100 after adding), the knob
// trims the whole curve. Everything below (ripple, retime, ranges) treats both the same; only the value range differs.
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

// Range of a pan value (on top of the knob): from hard left with the knob hard right and back
inline constexpr double kMinPan = -200;
inline constexpr double kMaxPan = 200;
// Value range of a curve of that kind
inline double minValue(AutoTarget t) { return t == AutoTarget::Pan ? kMinPan : kMinDb; }
inline double maxValue(AutoTarget t) { return t == AutoTarget::Pan ? kMaxPan : kMaxDb; }

// Value at a timeline frame (fractions allowed); 0 for an empty curve
double valueAt(const AutoCurve& curve, double frame);
// What the track plays at a frame: fader + curve, clamped to kMinVolumeDb..kMaxVolumeDb (kMinVolumeDb = silent)
double levelAt(const Track& track, double frame);
// The pan the track plays at a frame: knob + pan curve, clamped to -100..+100
double panAt(const Track& track, double frame);
// levelAt or panAt; base(): the fader or the knob the curve sits on
double heardAt(const Track& track, AutoTarget t, double frame);
inline double base(const Track& track, AutoTarget t) { return t == AutoTarget::Pan ? track.pan : track.volumeDb; }

// Sorted by frame, one point per frame (the last one given wins), frames >= 0, values clamped to the target's range
AutoCurve normalized(AutoCurve curve, AutoTarget target = AutoTarget::Volume);

// Replaces the points in [from, to] (timeline frames) by `inside` (points outside that range are dropped). Outside
// the range the curve stays as it was: a point with the old value right before `from` and right after `to` holds it.
void replaceRange(AutoCurve& curve, int from, int to, const AutoCurve& inside, AutoTarget target = AutoTarget::Volume);

// Ripple: shifts as in TimelineOps::rippleTracks - (frame, offset) pairs; a point moves by the sum of the offsets
// whose frame is <= its own. A negative offset takes the time [frame + offset, frame) out (its points go; the values
// at both edges stay), a positive one puts `offset` frames in at `frame` that hold the value there.
void ripple(AutoCurve& curve, const QVector<QPair<int, int>>& shifts);

// Every point's frame through f (frame rate change); points that land on the same frame keep the last one
void retime(AutoCurve& curve, const std::function<int(int)>& f);
// Both curves of a track (ripple / retime); nothing happens to an empty one
void rippleTrack(Track& t, const QVector<QPair<int, int>>& shifts);
void retimeTrack(Track& t, const std::function<int(int)>& f);

// Project file / CLI: [[frame, value], ...] (value rounded to 1/1000)
QJsonArray toJson(const AutoCurve& curve);
AutoCurve fromJson(const QJsonArray& a, AutoTarget target = AutoTarget::Volume);

// Write mode (Studio page, "touch"): while playing, a moved fader or pan knob records what it heard; on release the
// recording replaces the curve where it was touched and glides back to the old curve over kGlideBack frames.
// One Touch per drag. Values are what is heard (fader + curve, knob + curve), frames timeline frames.
struct Touch {
    AutoTarget target = AutoTarget::Volume;
    AutoCurve original;  // the curve before the touch (what it glides back to)
    double baseValue = 0; // the fader / knob the curve sits on
    int start = -1;       // first frame touched (-1 = nothing recorded yet)
    QVector<QPair<int, double>> heard; // one value per frame recorded (frames rising; the last one at a frame wins)
};
// Smallest change worth a key (dB / pan steps) and the most keys per second
inline constexpr double kWriteStepDb = 0.1;
inline constexpr double kWriteStepPan = 1.0;
inline constexpr double kWriteKeysPerSecond = 30;
// Frames the value takes on release to get back to the old curve
int glideBackFrames(double fps);
// Starts a touch of track `t`'s curve
Touch beginTouch(const Track& t, AutoTarget target);
// Records what is heard at `frame` (playing). A frame before the last one (a jump back, a loop) is ignored.
void record(Touch& touch, int frame, double heard);
// The curve while touching (live): the old curve up to the touch, the thinned recording, from the last recorded frame
// on the last value holds (the mixer runs ahead of the playhead: it must hear the fader's value, not the old curve)
AutoCurve touchingCurve(const Touch& touch, double fps);
// The curve on release at `frame`: the old curve outside, the thinned recording up to the release, then a glide back
// to the old curve over glideBackFrames
AutoCurve releasedCurve(const Touch& touch, int frame, double fps);
// The recording thinned (keys at most every 1/kWriteKeysPerSecond s, only where the value changes by a step)
AutoCurve thinned(const Touch& touch, double fps);

} // namespace Automation
