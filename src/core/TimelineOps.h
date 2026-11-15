// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Pure functions on the timeline model. No undo, no signals -
// Project::edit() takes care of that. This makes it easy to test and extend.

#include "core/Types.h"

#include <QSet>

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

// The audio clips among `clipIds` whose sound already plays over their whole length: other enabled clips (not in
// `clipIds`) on unmuted tracks play the same stream of the same file at the same source time (a second copy of a
// moment for picture-in-picture or a blurred background would play it twice). Retimed clips never count.
QVector<int> repeatedSound(const Timeline& tl, const QVector<int>& clipIds);

// Overwrites the range [start, end) on a track (like "Overwrite" in DaVinci):
// clips in it are shortened, split or removed. Nothing moves up.
// If a linked clip is split, the right piece provisionally gets the linkId -old (same on all tracks);
// resolvePendingLinks (called by Project::edit) turns that into a new common link.
// A clip cut off at its start keeps its comp graph's picture in place (Comp::followHead; fps = Timeline::frameRate).
void clearRange(Track& track, int start, int end, const IdGen& newId, double fps);
// Replace provisional (negative) linkIds from clearRange with new ones: same provisional id -> same new id. A partner
// that was not cut (on a track without a right piece) goes with the right piece when it plays in sync with it (same
// file and playback, same start - in, overlapping in time) and not with the left piece that kept the old link (a
// delete range over a picture whose sound starts after the range); otherwise it keeps the old link. A partner of
// another file (a recorder's sound to a camera's picture) goes with the piece it lies under more.
void resolvePendingLinks(Timeline& tl, const IdGen& newLinkId);
// Linked pairs (ids) in sync in `before` - the same file showing the same frame at the same place, or other files side
// by side in time - whose offset (start - in) to each other changed in `after`; pairs where one clip is in `ignore`
// (changed on purpose), plays otherwise now (speed, reverse, freeze, ramp) or is gone are left out.
QVector<QPair<int, int>> linksApart(const Timeline& before, const Timeline& after, const QSet<int>& ignore = {});
// After an edit from `before` to `after`: kill feed copies (Reframe::keepCopy, linked to their clip) that the edit left
// as they were while it cut their clip in place (another clip overwrote its middle, an end or all of it: clearRange,
// placeClip) are cut the same way - the part over the new clip goes, each piece stays linked to the clip's piece
// below it (before resolvePendingLinks). Copies on locked tracks stay. Called by Project::edit.
void cutCopiesLikeBases(const Timeline& before, Timeline& after, const IdGen& newId);

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
void placeClip(Track& track, const Clip& clip, const IdGen& newId, double fps);

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

// Deletes clips with ripple: the time taken out is the deleted time where nothing stays on any affected track (an
// L-cut's sound under the next picture keeps that time, so linked clips stay in sync). The affected tracks move up by
// it; the other unlocked tracks too, unless a clip of theirs would run into another one there (rippleTracks: that
// track stays). Returns the frames taken out (less than deleted when clips stay in the deleted time).
struct RippleResult {
    int deleted = 0; // frames of deleted time (all tracks together)
    int removed = 0; // frames taken out of the timeline
    // The time meant to go: of a linked group with a picture the picture's time (its sound's own J/L tail under the
    // next clip does not count), of other clips their time; and how much of it was taken out (less: other clips stay
    // in it)
    int meant = 0, meantRemoved = 0;
};
RippleResult rippleDelete(Timeline& tl, const QVector<int>& clipIds);

// Splits the clips at frame `frame`. Right halves get new IDs; linked
// pairs stay linked (new common linkId for the right parts; a partner that was not split and plays in sync with a
// right part only - an L-cut's sound starting after `frame` - goes with it, see resolvePendingLinks).
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
// overwrite: neighbors do not limit (see trimClipsOver)
int clampTrim(const Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
              const SourceLength& sourceLength, bool overwrite = false);

// Moves the edge (start or end) of the clips by delta. No ripple: nothing moves up.
void trimClips(Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
               const SourceLength& sourceLength);
// Like trimClips, but an edge may run over neighboring clips (overwrite): where the longer clip now lies they are
// shortened or removed (clearRange). Nothing moves up.
void trimClipsOver(Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
                   const SourceLength& sourceLength, const IdGen& newId);

// Trim mode (T) like DaVinci. All variants leave the view alone and never overwrite a neighbor.
enum class TrimKind {
    Ripple, // drag edge, later clips move up (start: clip stays put, content moves);
            // like DaVinci on all unlocked tracks (shortening only as far as all can move up)
    Roll,   // move the cut between two clips: left end, right start, total length stays
    Slip,   // move content within the clip (in/out), position, length and keyframes (their place in the clip) stay;
            // the place of Color page windows moves with the content
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
// The spans of transitions() that cover `frame`, asking sourceLength only for the clips around it (a source length
// can mean opening the file: the first frame of a project with many dissolves waited for all of them)
QVector<TransitionSpan> transitionsAt(const Track& track, int frame, const SourceLength& sourceLength);

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
// After an edit: a Fixed slot's own length (ClipSlot::nominalLen) belongs to the template fill. A slot clip the user
// made longer or shorter, cut or pasted (a clip that was not there before) lets go of it - with the other parts of
// its instance (the clips of the same slot that touch it on its track, before or after the edit) - so the next fill
// fills what is on the timeline.
void forgetSlotLengths(const Timeline& before, Timeline& after);

int endFrame(const Timeline& tl);
// The nested sequences (compound clips, multicam angles) of both are the same, their angles and frame sizes included;
// Timeline's == leaves them out
bool sameNested(const Timeline& a, const Timeline& b);
// Is the file anywhere on a video track? (e.g. only rebuild the preview if a proxy belongs to it)
bool usesMediaOnVideo(const Timeline& tl, const QString& path);

// Where the track with `uid` (Track::uid) is now in `tracks` (a window bound to a track follows it when tracks are
// added, removed or reordered, by hand or by undo); -1 = gone. `index` = where it was: kept when a copy of the track
// (same uid) lies elsewhere too.
int followTrack(const QVector<Track>& tracks, quint64 uid, int index);

} // namespace TimelineOps
