// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The magnetic main track (opt-in): V1's clips lie back to back from frame 0, as in the main track of phone editors
// and Final Cut's storyline. Desktop: "Lücken automatisch schließen (Spur 1)" (default off); phone and tablet:
// "Hauptspur magnetisch" (default on, finger input). Project::edit applies pack() after every edit that changed V1
// (one undo step with the edit); placements onto V1 make room first (openGap, at insertPoint) instead of overwriting.
// Pure functions on the timeline model like TimelineOps.

#include "core/Types.h"

class QString;

namespace MainTrack {

inline constexpr TrackRef kMain{TrackKind::Video, 0};

enum class Result { Unchanged, Moved, Refused };

// Track `ref`'s clips back to back from frame 0 in `order` (clip ids; empty = by their start; ids left out follow
// in start order). Clips linked to them (the same linkId, on other tracks: their sound, a kill feed copy) move by the
// same frames - only where that keeps everything: not before frame 0, not on a locked track, not onto another clip;
// else nothing moves (Refused, *why says why, translated). When the clips keep their order the unlocked subtitle
// tracks and the timeline's markers, notes and sections follow like a ripple (TimelineOps::rippleTracks: the time of
// a closed gap is taken out, markers in it land on the cut). A locked `ref` is Unchanged.
Result pack(Timeline& tl, TrackRef ref, const QVector<int>& order = {}, QString* why = nullptr);

// Room for `length` frames at `frame` on track `ref`: its clips from `frame` on and the clips linked to them move
// right (the same checks as pack; subtitles and markers from `frame` on follow). A clip lying across `frame` stays
// (insertPoint avoids that).
Result openGap(Timeline& tl, TrackRef ref, int frame, int length, QString* why = nullptr);

// Where something inserted at `frame` goes on the track: the nearer edge of a clip under it, the end of the last clip
// after it, else `frame` (a gap). Clips in `exclude` (being moved) do not count.
int insertPoint(const Track& t, int frame, const QVector<int>& exclude = {});

// Did an edit change where the clips of `ref` lie (ids, starts, lengths), or did the track appear?
bool touched(const Timeline& before, const Timeline& after, TrackRef ref);

} // namespace MainTrack
