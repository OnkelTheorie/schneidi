// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Result of the tracker node of the compositing graph (0.19): per clip frame where the tracked thing is. The analysis
// (engine/picture/CompTrack) makes it, Editor::trackCompNode stores it as keyframes of the node's own parameters
// "n<id>.x/.y/.scale/.angle" on the effect "comp", other nodes follow them (numOf(id, "x")).

#include <QVector>

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

} // namespace Comp
