// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Result of the tracker node of the compositing graph (0.19): per clip frame where the tracked thing is. The analysis
// (engine/picture/CompTrack) makes it, Editor::trackCompNode stores it as keyframes of the node's own parameters
// "n<id>.x/.y/.scale/.angle" on the effect "comp", other nodes follow them (numOf(id, "x")).
//
// Follow rule (one for every follower: mask, transform, …): the tracker has a reference point "rx"/"ry" = where the
// tracked thing is at the analysis' start frame (point: a; two points: the middle of a and b; face: the face centre
// found there). A follower is drawn relative to it: its own positions move by (x - rx, y - ry) and scale/turn by
// scale/angle around (x, y). Not analyzed yet (no x/y values or keyframes): x/y = rx/ry, scale 1, angle 0 = nothing
// moves; no rx/ry written yet: what the mode implies from ax/ay/bx/by (the face mode: ax/ay, its centre is unknown).

#include <QPointF>
#include <QString>
#include <QVector>

#include <cmath>
#include <functional>

namespace Comp {

inline constexpr const char* kTrackerType = "tracker";

struct TrackPoint {
    int frame = 0;   // clip frame
    double x = 0.5;  // position 0..1 of the picture (point / middle of the two points / face center)
    double y = 0.5;
    double scale = 1; // size against the start frame (two points: distance; face: width)
    double angle = 0; // degrees clockwise against the start frame (two points only)
};

// The parameters the result is written into, in TrackPoint order
inline const char* const kTrackKeys[4] = {"x", "y", "scale", "angle"};
// The reference point (plain values, no keyframes): written by Editor::trackCompNode from the start frame's point
inline const char* const kRefKeys[2] = {"rx", "ry"};

// The tracker id a follower's "follow" value names (one rounding for every kind and check): 0 = follows nothing
inline int followId(double v)
{
    return std::isfinite(v) && v >= 0.5 && v < 2e9 ? int(std::lround(v)) : 0;
}

// The reference point a tracker's mode implies before an analysis wrote rx/ry ("two": middle of a and b, else a)
inline QPointF impliedReference(const QString& mode, QPointF a, QPointF b)
{
    return mode == QLatin1String("two") ? (a + b) / 2 : a;
}

// A tracker's value as followers read it, by the rule above. has(key) = the node has a value or keyframes for key,
// value(key) = its value at the frame (missing = the kind's default), mode = the node's mode id
inline double followValue(const QString& key, const QString& mode, const std::function<bool(const QString&)>& has,
                          const std::function<double(const QString&)>& value)
{
    // One point follows a position only: size and turn stay as they are (keys an earlier two-point run left behind
    // would otherwise scale the follower up to 15 times - text flew out of the picture)
    if (mode == QLatin1String("point")) {
        if (key == QLatin1String("scale")) return 1.0;
        if (key == QLatin1String("angle")) return 0.0;
    }
    const bool isX = key == QLatin1String("x"), isY = key == QLatin1String("y");
    const bool isRx = key == QLatin1String("rx"), isRy = key == QLatin1String("ry");
    if (!(isX || isY || isRx || isRy) || has(key)) return value(key);
    if (isX || isY) return followValue(QLatin1String(isX ? "rx" : "ry"), mode, has, value);
    const QPointF r = impliedReference(mode, {value(QStringLiteral("ax")), value(QStringLiteral("ay"))},
                                       {value(QStringLiteral("bx")), value(QStringLiteral("by"))});
    return isRx ? r.x() : r.y();
}

} // namespace Comp
