// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Stutter in recordings with a variable frame rate: game capture keeps the time stamps of the frames it got, so where
// the game lagged a frame stays on screen for several frame durations (a jump in the time stamps). Pure functions on
// the time stamps; reading them from a file (packets, no decoding) and caching is engine/FrameGaps.h.

#include <QPair>
#include <QVector>

#include <vector>

namespace Stutter {

struct Jump {
    double at = 0;     // seconds in the file (frame 0 = 0): the last frame before the jump
    double length = 0; // seconds until the next frame (this long the picture stands still)
    int missing = 0;   // frames missing in between (length / frame duration - 1, rounded)
    double end() const { return at + length; }
    bool operator==(const Jump&) const = default;
};

struct Scan {
    double frameDuration = 0; // typical distance of two frames (the median), seconds
    double duration = 0;      // first to last frame plus one frame duration
    int frames = 0;
    QVector<Jump> jumps;      // longer than 2 frame durations, in time order
    bool operator==(const Scan&) const = default;
};

// Presentation times of all frames (seconds in the file, frame 0 = 0, any order): the jumps longer than 2 frame
// durations. The frame duration is the median distance, so a capture whose nominal rate is a time base (120, 1000)
// still counts its real frames.
Scan fromTimestamps(std::vector<double> times);

// The same from the video packets, leaving out jumps nobody sees: the frame after the jump repeats the picture before
// it. A screen capture with a variable frame rate sends few or no frames while nothing changes (05: a static browser
// page, "11-34 frames missing" that were never a stutter). Repeated = an inter frame of at most
// kUnchangedBytesPerPixel per pixel (an empty P-frame: about 250 bytes at 1080p; a game frame tens of KB); a key frame
// after the jump is judged by the frames before the jump and after the key frame. pixels <= 0: nothing left out.
struct Packet {
    double time = 0; // presentation time, seconds (frame 0 = 0)
    int size = 0;    // bytes
    bool key = false;
};
inline constexpr double kUnchangedBytesPerPixel = 0.0004;
Scan fromPackets(std::vector<Packet> packets, int pixels);

// Jumps that show inside [from, to) seconds of the source (the picture stands still in that range)
QVector<Jump> inRange(const Scan& s, double from, double to);

// Jumps less than `join` seconds apart as one stretch of stutter (the game lagged for a while)
struct Stretch {
    double from = 0, to = 0; // seconds: the last good frame before the first jump, the next frame after the last one
    int jumps = 0, missing = 0;
    double longest = 0;      // longest single jump, seconds
};
QVector<Stretch> stretches(const QVector<Jump>& jumps, double join = 0.5);

// Parts of [from, to) seconds without stutter: up to the end of the last good frame before a stretch (that frame
// still plays as usual), again from `margin` seconds after its next frame; pieces shorter than minLength dropped
QVector<QPair<double, double>> cleanPieces(const Scan& s, double from, double to, double margin = 0.1,
                                           double minLength = 0.5);

} // namespace Stutter
