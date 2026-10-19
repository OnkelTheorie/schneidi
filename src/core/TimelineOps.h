#pragma once
// Pure functions on the timeline model. No undo, no signals -
// Project::edit() takes care of that. This makes it easy to test and extend.

#include "core/Types.h"

#include <functional>

using IdGen = std::function<int()>;

namespace TimelineOps {

// Finds a clip; optionally the track is returned.
Clip* findClip(Timeline& tl, int clipId, TrackRef* where = nullptr);
const Clip* findClip(const Timeline& tl, int clipId, TrackRef* where = nullptr);

// Is the clip on a locked track (lock in the track header)? Unknown clips count as not locked.
bool isLocked(const Timeline& tl, int clipId);
// Only the clips that are not on locked tracks (order stays)
QVector<int> unlocked(const Timeline& tl, const QVector<int>& clipIds);
// Tracks on which the clips lie (without duplicates)
QVector<TrackRef> tracksOf(const Timeline& tl, const QVector<int>& clipIds);

// Ripple on the other tracks like DaVinci: all unlocked tracks except `skip` stay in sync
// (also the unlocked subtitle tracks, same rules, see Subtitles::ripple).
// shifts: (from frame, offset) - a clip moves by the sum of the offsets whose frame is <= its start.
// If a track would overwrite a clip that stays (or slide before frame 0), it stays
// entirely put - never overwrite.
void rippleTracks(Timeline& tl, const QVector<QPair<int, int>>& shifts, const QVector<TrackRef>& skip);
// How far (frames to the left) the other tracks can move up from `from` without overwriting
// (unlocked subtitle tracks count too)
int rippleRoom(const Timeline& tl, int from, const QVector<TrackRef>& skip);
// Insert on all unlocked subtitle tracks: split the entry across `frame`, the rest moves by `length`
void insertSubtitleGap(Timeline& tl, int frame, int length, const IdGen& newId);

// All clips with the same linkId (including the clip itself).
QVector<int> linkedGroup(const Timeline& tl, int clipId);

// Overwrites the range [start, end) on a track (like "Overwrite" in DaVinci):
// clips in it are shortened, split or removed. Nothing moves up.
// If a linked clip is split, the right piece provisionally gets the linkId -old (same on all tracks);
// resolvePendingLinks (called by Project::edit) turns that into a new common link.
void clearRange(Track& track, int start, int end, const IdGen& newId);
// Replace provisional (negative) linkIds from clearRange with new ones: same provisional id -> same new id
void resolvePendingLinks(Timeline& tl, const IdGen& newLinkId);

// Changes the clips for which `need` is true with `apply`, without touching the others: tracks without such a clip
// stay shared with copies of the timeline (the undo snapshots; a plain non-const loop would copy every track)
template <typename Need, typename Apply>
void changeClips(Timeline& tl, Need need, Apply apply)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        QVector<Track>& tracks = tl.tracks(k);
        for (int t = 0; t < tracks.size(); ++t)
            for (int i = 0; i < std::as_const(tracks)[t].clips.size(); ++i)
                if (need(std::as_const(tracks)[t].clips[i])) apply(tracks[t].clips[i]);
    }
}

// Places a clip on the track in overwrite mode.
void placeClip(Track& track, const Clip& clip, const IdGen& newId);

bool removeClip(Timeline& tl, int clipId);

// Insert with ripple (Insert, F9): on the tracks the clip is split across `frame` (linked parts stay
// linked, see splitAt) and everything from `frame` on moves right by `length`.
void insertGap(Timeline& tl, const QVector<TrackRef>& tracks, int frame, int length,
               const IdGen& newClipId, const IdGen& newLinkId);
// Shifts all clips on the track with start >= frame by delta (ripple; the caller makes sure there is room)
void shiftFrom(Track& track, int frame, int delta);
// Remove a dissolve at the cut at `frame` (a new clip goes there right away). Standalone fade out/in
// (to black or silence, also with a neighbor) stays on the clip, as in DaVinci.
void clearDissolveAt(Track& track, int frame);

// Creates missing tracks until there are `count` tracks of the kind (names V3, A3, ...).
void ensureTracks(Timeline& tl, TrackKind kind, int count);

// Deletes clips with ripple: on the affected tracks everything following moves up (like Shift+Del in DaVinci),
// the other unlocked tracks move along by the deleted ranges (rippleTracks).
void rippleDelete(Timeline& tl, const QVector<int>& clipIds);

// Splits the clips at frame `frame`. Right halves get new IDs; linked
// pairs stay linked (new common linkId for the right parts).
// Returns the IDs of the new right parts.
QVector<int> splitAt(Timeline& tl, const QVector<int>& clipIds, int frame,
                     const IdGen& newClipId, const IdGen& newLinkId);

// Limits the track offset so that all clips move equally far (like DaVinci: V1->V2 takes A1->A2 along):
// no track below V1/A1, tracks of kind `anchorKind` stay within the existing range,
// the other kind may need new tracks (created when moving).
int clampTrackDelta(const Timeline& tl, const QVector<int>& clipIds, TrackKind anchorKind, int trackDelta);

// Moves clips by deltaFrames and trackDelta tracks (see clampTrackDelta).
// Move a clip by `fine` 1/100 frame (start + subframe, carry over to whole frames)
void shiftFine(Clip& c, int fine);
void moveClips(Timeline& tl, const QVector<int>& clipIds, int deltaFrames,
               TrackKind anchorKind, int trackDelta, const IdGen& newId);

enum class Edge { Start, End };
// Length of a clip's source material in frames, converted with speed (Clip::retimedLength;
// <= 0 = unlimited, e.g. stills)
using SourceLength = std::function<int(const Clip& clip)>;

// Limits a trim delta so that for all clips: not beyond the source material,
// at least 1 frame long, not before frame 0 and not into the neighboring clip (no overwriting).
int clampTrim(const Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
              const SourceLength& sourceLength);

// Moves the edge (start or end) of the clips by delta. No ripple: nothing moves up.
void trimClips(Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
               const SourceLength& sourceLength);

// Trim mode (T) like DaVinci. All variants leave the view alone and never overwrite a neighbor.
enum class TrimKind {
    Ripple, // drag edge, later clips move up (start: clip stays put, content moves);
            // like DaVinci on all unlocked tracks (shortening only as far as all can move up)
    Roll,   // move the cut between two clips: left end, right start, total length stays
    Slip,   // move content within the clip (in/out), position and length stay
    Slide,  // move clip, left neighbor gets longer/shorter, right one the other way round
};
struct TrimEdit {
    TrimKind kind = TrimKind::Ripple;
    QVector<int> ids;      // Ripple: clips of the edge; Roll: clips left of the cut; Slip/Slide: the clips
    QVector<int> rightIds; // Roll only: clips right of the cut
    Edge edge = Edge::End; // Ripple only
    bool isNull() const { return ids.isEmpty() && rightIds.isEmpty(); }
};
// Largest allowed delta in the direction of `delta` (source material, at least 1 frame, neighbors, frame 0)
int clampTrimEdit(const Timeline& tl, const TrimEdit& e, int delta, const SourceLength& sourceLength);
void applyTrimEdit(Timeline& tl, const TrimEdit& e, int delta, const SourceLength& sourceLength);

// Effective transition on a track (see Clip::transIn/transOut)
struct TransitionSpan {
    int leftId = 0;         // clip that fades out (0 = none -> fade in from black/silence)
    int rightId = 0;        // clip that fades in (0 = none -> fade out)
    int start = 0, end = 0; // Timeline-Frames [start, end)
    int cut = 0;            // cut (for fade in = start, for fade out = end)
    TransitionStyle style;  // from the left clip (transOutStyle), for fade in from the right (transInStyle)
    int length() const { return end - start; }
    bool isDissolve() const { return leftId && rightId; }
};

// Transitions of a track with the length that really fits: Cross Dissolve centered on the cut,
// shortened if handles (material beyond in/out) or clip length do not suffice; length 0 is dropped.
QVector<TransitionSpan> transitions(const Track& track, const SourceLength& sourceLength);

// Fade handle ramp (0..1, linear) at clip frame t (from clip start); fades limited to the clip length.
// Video uses the value as opacity, audio as a sine curve (audioFadeGain).
double fadeRamp(const Clip& c, double t);
// Level (linear, 0..1) of an audio clip through its fade handles at clip frame t
double audioFadeGain(const Clip& c, double t);
// Level (linear, 0..1) of clip clipId in audio transition s at timeline frame frame (1 outside)
double audioTransitionGain(const TransitionSpan& s, int clipId, double frame);

// Do a (left) and b (right) form a dissolve? Adjacent, both edges with a transition, neither standalone.
bool isDissolve(const Clip& a, const Clip& b);

// Release dissolves where only one of the two clips is in `clipIds`
// (before deleting/moving, so the remaining one does not suddenly fade out).
void detachTransitions(Timeline& tl, const QVector<int>& clipIds);
// After an edit: clip sides that were part of a dissolve before and now have no partner anymore
// (trimmed apart, partner has no handles anymore, speed changed ...) lose their transition - otherwise the
// dissolve would silently turn into a fade out/in over black. New clips (other ids) stay untouched.
void unpairBrokenDissolves(const Timeline& before, Timeline& after);
// After an edit: fade out and fade in that were not next to each other before and now bump together
// (gap closed, clip pushed up) stay two transitions instead of one dissolve (like DaVinci).
// Also tidies up the standalone marking where the edges no longer touch.
void keepFadesApart(const Timeline& before, Timeline& after);

int endFrame(const Timeline& tl);
// Is the file anywhere on a video track? (e.g. only rebuild the preview if a proxy belongs to it)
bool usesMediaOnVideo(const Timeline& tl, const QString& path);

} // namespace TimelineOps
