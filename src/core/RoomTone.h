// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Room tone: fill the gaps of a dialogue track with the quiet of the same recording, so a pause cut does not drop
// into dead silence. Pure functions (no decoding): the levels come from the caller (engine/AudioAnalysis), the
// result is clips on the timeline that play a quiet stretch of the take in a loop.

#include "core/Types.h"

#include <QVector>

#include <functional>
#include <vector>

namespace RoomTone {

// Blocks below this are digital silence (a gated recorder, the padding of a file), never room tone
constexpr double kSilentDb = -100;
// Block length of the level measurement
constexpr double kBlockSeconds = 0.05;

struct Quiet {
    double floorDb = -200; // the take's noise floor: 10th percentile of its blocks that are not digital silence
    int from = -1, to = -1; // the quiet stretch in blocks [from, to); from < 0 = none found
    double meanDb = -200;   // its level (power mean)
    bool found() const { return from >= 0 && to > from; }
};

// The quietest steady stretch of `want` blocks (shorter down to `least` when there is none): no block louder than
// floor + 6 dB (no word, breath or click), none in digital silence, its blocks within kSteadyDb of each other (no
// step in level that would pump in a loop); among those the one with the lowest level. `blockDb` = RMS level of each
// block in dBFS (silence -200).
constexpr double kSteadyDb = 4;
Quiet findQuiet(const std::vector<double>& blockDb, int want, int least);

// Crossfade length at the seams in frames: about 20 ms, at least one frame
int seamFrames(double fps);

// One gap to fill on an audio track
struct Fill {
    Clip take;             // the clip the quiet comes from (its file, stream, volume, pan and audio effects)
    int from = 0, to = 0;  // the gap [from, to) in timeline frames
    int quietIn = 0;       // the quiet stretch in source frames [quietIn, quietIn + quietLength) (speed 1)
    int quietLength = 0;
    double gainDb = 0;     // added to the take's volume so the loop sits at the take's floor
    int seam = 1;          // crossfade frames (seamFrames)
};

// Puts the loop pieces of `f` into the gap on audio track `track` (clips with ids from newId). Each seam between
// two pieces is a crossfade that starts on the cut (the left piece plays on into the next). At the gap's edges the
// take fades into the room tone the same way when its own edge is free (no transition, no fade) and has material:
// the take's sound before the cut stays untouched. Otherwise the piece fades in/out by `seam` frames. `fileLength` =
// the take's file length in frames (handles). Returns the ids of the new clips.
QVector<int> fill(Timeline& tl, int track, const Fill& f, int fileLength, const std::function<int()>& newId);

} // namespace RoomTone
