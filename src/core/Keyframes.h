#pragma once
// Keyframes (like DaVinci): pure functions on Clip::keys, without undo/signals (Project::edit does that).
// Times here are clip frames t (0 = first frame of the clip), the source frame (Clip::in + t) is stored.
// Between two keyframes it is interpolated (linear or ease curve), before the first/after the last
// its value applies. Without keyframes the static value in the clip applies.

#include "core/Types.h"

#include <QColor>

namespace Keys {

struct ParamInfo {
    AnimParam param;
    const char* id; // key in the project file
    bool color;     // value = ARGB number, interpolate channels individually
    const char* name; // display (N_, nullptr = from the EffectRegistry)
    double min, max;  // sensible value range (curve editor)
};
const ParamInfo& info(AnimParam p);
bool fromId(const QString& id, AnimParam* p);

// Dynamic effect parameters (frei0r plugins, known only at run time): animatable without an AnimParam enum entry.
// Every (effect id, parameter key) pair gets its own AnimParam value >= kEffectParamBase for the lifetime of the
// process, so Clip::keys, undo, the curve editor etc. treat it like a fixed parameter. The number is never saved:
// the project file uses the id "fx:<effect id>:<key>", which works even when the plugin is missing.
inline constexpr int kEffectParamBase = 1000;
AnimParam effectParam(const QString& effectId, const QString& key);
inline bool isEffectParam(AnimParam p) { return int(p) >= kEffectParamBase; }
// Effect id and key of a dynamic parameter (false = fixed parameter)
bool effectParamOf(AnimParam p, QString* effectId, QString* key);

// Color <-> keyframe value (ARGB as a number, exactly representable in double)
inline double fromColor(const QColor& c) { return double(c.rgba()); }
inline QColor toColor(double v) { return QColor::fromRgba(QRgb(quint32(v))); }

QString label(AnimParam p);                     // translated name (curve editor)
void range(AnimParam p, double* lo, double* hi); // allowed value range

double staticValue(const Clip& c, AnimParam p);
void setStaticValue(Clip& c, AnimParam p, double v);

bool animated(const Clip& c, AnimParam p);
bool hasKeys(const Clip& c);
// Value at clip frame t (also fractions)
double valueAt(const Clip& c, AnimParam p, double t);
// Keyframe exactly at clip frame t (nullptr = none)
const Keyframe* keyAt(const Clip& c, AnimParam p, int t);

void setKey(Clip& c, AnimParam p, int t, double v);  // sets or replaces (interpolation stays)
void removeKey(Clip& c, AnimParam p, int t);         // last one removed -> value stays as static value
// Change a value as in the DaVinci Inspector: animated -> keyframe at t (new or replaced), otherwise static value
void setValue(Clip& c, AnimParam p, int t, double v);
void clear(Clip& c, AnimParam p);                    // all keyframes removed, static value stays

// Clip frames of all keyframes (sorted, without duplicates); params empty = all parameters
QVector<int> keyTimes(const Clip& c, const QVector<AnimParam>& params = {});
// Does the parameter run linearly or constantly between t0 and t1 (adjacent keyframe times)?
bool linearBetween(const Clip& c, AnimParam p, int t0, int t1);

// Bezier (curve editor): handles relative to keyframe k[i] in frames/value. autoHandles sets smooth handles
// (slope from the neighbors, flat at peaks and ends). handle() returns the effective handle (shortened to the section,
// for keyframes without Bezier the derived shape); false = no neighbor on this side.
// setHandle makes the keyframe Bezier; without broken the other handle stays on a line (same slope).
void autoHandles(KeyTrack& k, int i);
bool handle(const KeyTrack& k, int i, bool out, double* dt, double* dv);
void setHandle(KeyTrack& k, int i, bool out, double dt, double dv, bool broken = false);
// After converting the keyframe times (speed): stretch handle times along in proportion to the distances.
// after = the same keyframes in the same order as before (not yet sorted); reversed swaps in/out.
void rescaleHandles(const KeyTrack& before, KeyTrack& after);

// Timeline keyframe track: all parameters together at the times
void setEase(Clip& c, const QVector<int>& times, KeyEase ease, const QVector<AnimParam>& params = {});
void removeAt(Clip& c, const QVector<int>& times, const QVector<AnimParam>& params = {});
void move(Clip& c, const QVector<int>& times, int delta);
// All keyframes by `delta` source frames, after Clip::in/out moved by it (slip): they keep their clip frame t, i.e.
// their place in the clip and in the timeline, instead of riding along with the content (and out of the clip)
void shiftWithIn(Clip& c, int delta);

// After splitting (left/right = copies with new in/out): like DaVinci both parts keep all keyframes,
// also those outside their range (invisible) -> both look exactly as before, whatever the curve shape.
void split(const Clip& original, Clip& left, Clip& right);

// Range active and (static or animated) not neutral
bool hasTransform(const Clip& c);
bool hasCrop(const Clip& c);
bool hasOpacity(const Clip& c);

} // namespace Keys
