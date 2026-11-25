// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The magnetic main track (opt-in): V1's clips lie back to back from frame 0, as in the main track of phone editors
// and Final Cut's storyline. Desktop: "Lücken automatisch schließen (Spur 1)" (default off); phone and tablet:
// "Hauptspur magnetisch" (default on, finger input). Project::edit applies pack() after every edit that changed V1
// (one undo step with the edit); placements onto V1 make room first (openGap, at insertPoint) instead of overwriting.
// Pure functions on the timeline model like TimelineOps.

#include "core/Types.h"

#include <QHash>

class QString;

namespace MainTrack {

inline constexpr TrackRef kMain{TrackKind::Video, 0};

enum class Result { Unchanged, Moved, Refused };

// Track `ref`'s clips back to back from frame 0 in `order` (clip ids; empty = by their start; ids left out follow
// in start order). Clips linked to them (the same linkId, on other tracks: their sound, a kill feed copy) move by the
// same frames - only where that keeps everything: not before frame 0, not on a locked track, not onto another clip;
// else nothing moves (Refused, *why says why, translated). When the clips keep their order the unlocked subtitle
// tracks, the automation of the tracks whose clips move and the timeline's markers, notes and sections follow like a
// ripple (TimelineOps::rippleTracks: the time of a closed gap is taken out, markers in it land on the cut). A locked `ref` is Unchanged. `keep` (clip id -> frames):
// the gap left before that clip (keptGaps: the gaps that were there before an edit stay); empty = no gaps at all.
Result pack(Timeline& tl, TrackRef ref, const QVector<int>& order = {}, QString* why = nullptr,
            const QHash<int, int>& keep = {});

// The gap before each clip of `t` (clip id -> frames, from frame 0 or the end of the clips before it; 0 left out).
QHash<int, int> gapsBefore(const Track& t);

// What the magnet keeps after an edit (`before` -> `after`) on `ref`: only the gaps the edit itself opened close, the
// ones that were there before stay (a desktop project with a pause on V1 opened on the phone). Per clip of `after`
// (id -> frames, for pack's `keep`): between two clips that are on the track before and after (and from frame 0), the
// gap frames there were before - the gaps of removed clips included - are kept, from the first gap of that stretch on,
// at most as many as `after` has there. A clip new on the track keeps the gap before it within that budget; clips
// after the last old clip keep none (a new clip placed past the end moves up to it).
QHash<int, int> keptGaps(const Timeline& before, const Timeline& after, TrackRef ref);

// Room for `length` frames at `frame` on track `ref`: its clips from `frame` on and the clips linked to them move
// right (the same checks as pack; subtitles and markers from `frame` on follow). A clip lying across `frame` stays
// (insertPoint avoids that).
Result openGap(Timeline& tl, TrackRef ref, int frame, int length, QString* why = nullptr);

// Where something inserted at `frame` goes on the track: the nearer edge of a clip under it, the end of the last clip
// after it, else `frame` (a gap). Clips in `exclude` (being moved) do not count. Never before `atLeast`: then the first
// cut (clip edge, the track's end) at or after it, past the end `atLeast` itself (what comes along with the inserted
// clips - their sound, other pasted clips - must not go before frame 0).
int insertPoint(const Track& t, int frame, const QVector<int>& exclude = {}, int atLeast = 0);

// Did an edit change where the clips of `ref` lie (ids, starts, lengths)? Not when the track count changed (track
// structure edits; moving a track is held by the Editor: Project::MagnetHold).
bool touched(const Timeline& before, const Timeline& after, TrackRef ref);

} // namespace MainTrack
