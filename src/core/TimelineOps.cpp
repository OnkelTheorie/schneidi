// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TimelineOps.h"

#include "core/CompGraph.h"
#include "core/Keyframes.h"
#include "core/Subtitles.h"

#include <QHash>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <climits>
#include <cmath>

namespace {

void sortTrack(Track& t)
{
    std::sort(t.clips.begin(), t.clips.end(),
              [](const Clip& a, const Clip& b) { return a.start < b.start; });
}

} // namespace

namespace TimelineOps {

const Clip* findClip(const Timeline& tl, int clipId, TrackRef* where)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        const QVector<Track>& tracks = tl.tracks(k);
        for (int i = 0; i < tracks.size(); ++i)
            for (const Clip& c : tracks[i].clips)
                if (c.id == clipId) {
                    if (where) *where = {k, i};
                    return &c;
                }
    }
    return nullptr;
}

Clip* findClip(Timeline& tl, int clipId, TrackRef* where)
{
    // Searched read-only: only the track of the clip is detached from copies of the timeline (undo snapshots)
    TrackRef ref;
    const Clip* c = findClip(std::as_const(tl), clipId, &ref);
    if (!c) return nullptr;
    const qsizetype index = c - std::as_const(tl).track(ref).clips.constData();
    if (where) *where = ref;
    return &tl.track(ref).clips[index];
}

bool isLocked(const Timeline& tl, int clipId)
{
    TrackRef ref;
    return findClip(tl, clipId, &ref) && tl.track(ref).locked;
}

QVector<int> unlocked(const Timeline& tl, const QVector<int>& clipIds)
{
    QVector<int> out;
    for (int id : clipIds)
        if (!isLocked(tl, id)) out << id;
    return out;
}

QVector<TrackRef> tracksOf(const Timeline& tl, const QVector<int>& clipIds)
{
    QVector<TrackRef> refs;
    for (int id : clipIds) {
        TrackRef ref;
        if (findClip(tl, id, &ref) && !refs.contains(ref)) refs << ref;
    }
    return refs;
}

void rippleTracks(Timeline& tl, const QVector<QPair<int, int>>& shifts, const QVector<TrackRef>& skip)
{
    if (shifts.isEmpty()) return;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        auto& tracks = tl.tracks(k);
        for (int i = 0; i < tracks.size(); ++i) {
            Track& t = tracks[i];
            if (t.locked || skip.contains(TrackRef{k, i}) || t.clips.isEmpty()) continue;
            QVector<Clip> moved = t.clips;
            bool changed = false;
            for (Clip& c : moved) {
                int d = 0;
                for (const auto& s : shifts)
                    if (s.first <= c.start) d += s.second;
                c.start += d;
                changed |= d != 0;
            }
            if (!changed) continue;
            // Order stays; if anything overlaps, the track stays as it is
            bool ok = moved.first().start >= 0;
            for (int j = 1; ok && j < moved.size(); ++j) ok = moved[j].start >= moved[j - 1].end();
            if (!ok) continue;
            // release a dissolve that has been pushed apart (otherwise it would turn into fade out and fade in)
            for (int j = 1; j < moved.size(); ++j)
                if (t.clips[j - 1].end() == t.clips[j].start && moved[j - 1].end() != moved[j].start
                    && isDissolve(t.clips[j - 1], t.clips[j])) {
                    moved[j - 1].transOut = 0;
                    moved[j].transIn = 0;
                }
            t.clips = moved;
        }
    }
    // Subtitle tracks move along the same way (locked ones stay put, like DaVinci)
    for (SubtitleTrack& st : tl.subtitles)
        if (!st.locked) Subtitles::ripple(st, shifts);
}

int rippleRoom(const Timeline& tl, int from, const QVector<TrackRef>& skip)
{
    int room = INT_MAX / 2;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = tl.tracks(k);
        for (int i = 0; i < tracks.size(); ++i) {
            const Track& t = tracks[i];
            if (t.locked || skip.contains(TrackRef{k, i})) continue;
            int standEnd = 0, firstMoving = -1; // end of the clips that stay, start of the first moving one
            for (const Clip& c : t.clips) {
                if (c.start < from) standEnd = std::max(standEnd, c.end());
                else if (firstMoving < 0) firstMoving = c.start;
            }
            if (firstMoving >= 0) room = std::min(room, firstMoving - standEnd);
        }
    }
    return std::max(0, room); // subtitles do not limit: they are shortened (Subtitles::ripple)
}

void insertSubtitleGap(Timeline& tl, int frame, int length, const IdGen& newId)
{
    for (SubtitleTrack& st : tl.subtitles)
        if (!st.locked) Subtitles::insertGap(st, frame, length, newId);
}

QVector<int> linkedGroup(const Timeline& tl, int clipId)
{
    const Clip* c = findClip(tl, clipId);
    if (!c) return {};
    if (c->linkId == 0) return {clipId};
    QVector<int> ids;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& o : t.clips)
                if (o.linkId == c->linkId) ids << o.id;
    return ids;
}

QVector<int> repeatedSound(const Timeline& tl, const QVector<int>& clipIds)
{
    const auto plain = [](const Clip& c) { return c.kind == ClipKind::Media && c.enabled && !c.isRetimed(); };
    QVector<int> out;
    for (const Track& track : tl.audio)
        for (const Clip& x : track.clips) {
            if (!clipIds.contains(x.id) || !plain(x) || x.mediaPath.isEmpty()) continue;
            // Parts of x that the same sound covers elsewhere (source frame = timeline frame - start + in on both)
            QVector<QPair<int, int>> spans;
            for (const Track& t : tl.audio) {
                if (t.muted) continue;
                for (const Clip& c : t.clips)
                    if (!clipIds.contains(c.id) && plain(c) && c.mediaPath == x.mediaPath
                        && c.audioStream == x.audioStream && c.start - c.in == x.start - x.in
                        && c.subframe == x.subframe && c.start < x.end() && c.end() > x.start)
                        spans << qMakePair(std::max(c.start, x.start), std::min(c.end(), x.end()));
            }
            std::sort(spans.begin(), spans.end());
            int covered = x.start;
            for (const auto& [from, to] : spans)
                if (from <= covered) covered = std::max(covered, to);
            if (covered >= x.end()) out << x.id;
        }
    return out;
}

namespace {

// After a cut gave the right pieces of link groups new links (old link -> new link): an uncut partner (on a track
// without a right piece) goes with the right piece it plays in sync with (same file and playback, same start - in,
// overlapping in time) - unless it is in sync with the left piece too (it lies under both, or only under the left
// one): then it stays, as before. A partner of another file (a recorder's sound to a camera's picture) goes with the
// piece it lies under more. The pieces: the clips of the link on the tracks that were cut.
void relinkUncut(Timeline& tl, const QHash<int, int>& rightLinks)
{
    const auto inSync = [](const Clip& a, const Clip& b) {
        return a.mediaPath == b.mediaPath && a.start - a.in == b.start - b.in && a.start < b.end() && b.start < a.end()
               && a.speed == b.speed && a.reverse == b.reverse && a.freeze == b.freeze && a.ramp == b.ramp;
    };
    QHash<int, int> relink; // clip id -> new link
    for (auto m = rightLinks.cbegin(); m != rightLinks.cend(); ++m) {
        const int oldLink = m.key(), newLink = m.value();
        QVector<const Clip*> right, left, partners;
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : std::as_const(tl).tracks(k)) {
                const bool cut = std::any_of(t.clips.cbegin(), t.clips.cend(), [&](const Clip& c) { return c.linkId == newLink; });
                for (const Clip& c : t.clips) {
                    if (c.linkId == newLink) right << &c;
                    else if (c.linkId == oldLink) (cut ? left : partners) << &c;
                }
            }
        const auto overlap = [](const Clip* a, const QVector<const Clip*>& pieces) {
            int n = 0;
            for (const Clip* p : pieces) n += std::max(0, std::min(a->end(), p->end()) - std::max(a->start, p->start));
            return n;
        };
        for (const Clip* c : std::as_const(partners)) {
            if (!c->keepRegion.isEmpty()) continue; // kill feed copies follow their clip their own way (cutCopiesLikeBases)
            const auto sameFile = [&](const Clip* p) { return p->mediaPath == c->mediaPath; };
            const bool ofTheFile = std::any_of(right.cbegin(), right.cend(), sameFile)
                                   || std::any_of(left.cbegin(), left.cend(), sameFile);
            if (ofTheFile ? std::any_of(right.cbegin(), right.cend(), [&](const Clip* r) { return inSync(*c, *r); })
                                && std::none_of(left.cbegin(), left.cend(), [&](const Clip* l) { return inSync(*c, *l); })
                          // another file (camera + recorder): with the piece it lies under more
                          : overlap(c, right) > overlap(c, left))
                relink.insert(c->id, newLink);
        }
    }
    if (!relink.isEmpty())
        changeClips(tl, [&](const Clip& c) { return relink.contains(c.id); }, [&](Clip& c) { c.linkId = relink.value(c.id); });
}

} // namespace

void resolvePendingLinks(Timeline& tl, const IdGen& newLinkId)
{
    QHash<int, int> map;
    changeClips(tl, [](const Clip& c) { return c.linkId < 0; }, [&](Clip& c) {
        if (!map.contains(c.linkId)) map[c.linkId] = newLinkId();
        c.linkId = map[c.linkId];
    });
    QHash<int, int> rightLinks;
    for (auto m = map.cbegin(); m != map.cend(); ++m) rightLinks.insert(-m.key(), m.value());
    relinkUncut(tl, rightLinks);
}

QVector<QPair<int, int>> linksApart(const Timeline& before, const Timeline& after, const QSet<int>& ignore)
{
    const auto playsLike = [](const Clip& a, const Clip& b) {
        return a.speed == b.speed && a.reverse == b.reverse && a.freeze == b.freeze && a.ramp == b.ramp;
    };
    const auto linked = [](const Timeline& tl) {
        QHash<int, Clip> m;
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : tl.tracks(k))
                for (const Clip& c : t.clips)
                    if (c.linkId > 0) m.insert(c.id, c);
        return m;
    };
    const QHash<int, Clip> was = linked(before), now = linked(after);
    QHash<int, QVector<int>> groups; // link -> clips in both, same link, same playback
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        const auto old = was.constFind(it.key());
        if (old == was.cend() || ignore.contains(it.key()) || old->linkId != it->linkId || !playsLike(*old, *it)) continue;
        groups[it->linkId] << it.key();
    }
    QVector<QPair<int, int>> out;
    for (const QVector<int>& ids : std::as_const(groups))
        for (qsizetype i = 0; i < ids.size(); ++i)
            for (qsizetype j = i + 1; j < ids.size(); ++j) {
                const Clip &a0 = was[ids[i]], &b0 = was[ids[j]], &a1 = now[ids[i]], &b1 = now[ids[j]];
                if (!playsLike(a1, b1)) continue;
                const int d0 = (a0.start - a0.in) - (b0.start - b0.in), d1 = (a1.start - a1.in) - (b1.start - b1.in);
                const bool synced = a0.mediaPath == b0.mediaPath ? d0 == 0 : a0.start < b0.end() && b0.start < a0.end();
                if (synced && d0 != d1) out << qMakePair(std::min(ids[i], ids[j]), std::max(ids[i], ids[j]));
            }
    std::sort(out.begin(), out.end()); // (the hash's order changes per run: the message names the same pair each time)
    return out;
}

void cutCopiesLikeBases(const Timeline& before, Timeline& after, const IdGen& newId)
{
    const auto isCopy = [](const Clip& c) { return !c.keepRegion.isEmpty() && c.linkId > 0; };
    if (std::none_of(after.video.cbegin(), after.video.cend(), [&](const Track& t) {
            return std::any_of(t.clips.cbegin(), t.clips.cend(), isCopy);
        }))
        return;
    QHash<int, const Clip*> was, now;
    QHash<int, int> wasTrack, nowTrack;
    for (int i = 0; i < before.video.size(); ++i)
        for (const Clip& c : before.video[i].clips) was.insert(c.id, &c), wasTrack.insert(c.id, i);
    for (int i = 0; i < after.video.size(); ++i)
        for (const Clip& c : after.video[i].clips) now.insert(c.id, &c), nowTrack.insert(c.id, i);
    // Same file and the same part of it at the same place, played the same way (the copy's clip or a piece of it)
    const auto inSync = [](const Clip& x, const Clip& base) {
        return x.keepRegion.isEmpty() && x.mediaPath == base.mediaPath && x.start - x.in == base.start - base.in
               && x.speed == base.speed && x.reverse == base.reverse && x.freeze == base.freeze
               && x.freezeFrame == base.freezeFrame && x.ramp == base.ramp;
    };
    const auto sameTime = [](const Clip& a, const Clip& b) { return a.start == b.start && a.in == b.in && a.out == b.out; };
    QHash<int, QVector<Clip>> cut; // copy id -> its pieces (none: gone)
    for (int ti = 0; ti < after.video.size(); ++ti) {
        if (after.video[ti].locked) continue;
        for (const Clip& c : after.video[ti].clips) {
            const Clip* prev = was.value(c.id);
            if (!isCopy(c) || !prev || !sameTime(*prev, c)) continue; // new, or changed by the edit itself
            // Its clip before the edit: linked to it, in sync where they overlap
            const Clip* base = nullptr;
            for (const Track& t : before.video)
                for (const Clip& b : t.clips)
                    if (!base && b.linkId == c.linkId && inSync(b, c) && b.start < c.end() && b.end() > c.start) base = &b;
            if (!base) continue;
            const Clip* still = now.value(base->id);
            if (still && (sameTime(*still, *base) || !inSync(*still, *base) || still->linkId != base->linkId
                          || nowTrack.value(base->id) != wasTrack.value(base->id)))
                continue; // untouched, or moved/changed on purpose: not this kind of edit
            // The pieces of the clip left in place on its track (clearRange: the right one with the provisional link)
            QVector<const Clip*> pieces;
            for (const Clip& x : after.video[wasTrack.value(base->id)].clips)
                if ((x.id == base->id || !was.contains(x.id)) && inSync(x, *base) && x.start >= base->start
                    && x.end() <= base->end() && (x.linkId == base->linkId || x.linkId == -base->linkId))
                    pieces << &x;
            std::sort(pieces.begin(), pieces.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
            QVector<Clip> out;
            bool kept = false;
            for (const Clip* x : pieces) {
                const int lo = std::max(x->start, c.start), hi = std::min(x->end(), c.end());
                if (hi <= lo) continue;
                Clip q = c;
                q.start = lo;
                q.in = c.in + (lo - c.start);
                q.out = q.in + (hi - lo) - 1;
                if (lo > c.start) q.transIn = 0, q.fadeIn = 0;
                if (hi < c.end()) q.transOut = 0, q.fadeOut = 0;
                q.linkId = x->linkId;
                kept = kept || x->id == base->id;
                q.id = x->id == base->id ? c.id : 0;
                out << q;
            }
            // The piece over the clip's own keeps the copy's id (else the first), the others get new ones
            if (!kept && !out.isEmpty()) out[0].id = c.id;
            for (Clip& q : out)
                if (!q.id) q.id = newId();
            cut.insert(c.id, out);
        }
    }
    if (cut.isEmpty()) return;
    for (Track& t : after.video) {
        if (std::none_of(t.clips.cbegin(), t.clips.cend(), [&](const Clip& c) { return cut.contains(c.id); })) continue;
        QVector<Clip> clips;
        for (const Clip& c : std::as_const(t.clips))
            if (cut.contains(c.id)) clips << cut.value(c.id);
            else clips << c;
        t.clips = clips;
        sortTrack(t);
    }
}

void clearRange(Track& track, int start, int end, const IdGen& newId)
{
    if (end <= start) return;
    // Nothing there: the track stays untouched (read-only, undo snapshots share its clips)
    const QVector<Clip>& clips = std::as_const(track.clips);
    if (std::none_of(clips.cbegin(), clips.cend(), [&](const Clip& c) { return c.end() > start && c.start < end; })) return;
    QVector<Clip> result;
    for (const Clip& c : clips) {
        if (c.end() <= start || c.start >= end) { // no overlap
            result << c;
            continue;
        }
        if (c.start < start && c.end() > end) { // range lies in the middle of the clip -> split
            Clip left = c;
            left.out = c.in + (start - c.start) - 1;
            left.transOut = 0;
            left.fadeOut = 0;
            Clip right = c;
            right.id = newId();
            right.linkId = c.linkId > 0 ? -c.linkId : c.linkId; // provisional, see resolvePendingLinks
            right.in = c.in + (end - c.start);
            right.start = end;
            right.transIn = 0;
            right.fadeIn = 0;
            result << left << right;
        } else if (c.start < start) { // cut off the end
            Clip left = c;
            left.out = c.in + (start - c.start) - 1;
            left.transOut = 0;
            left.fadeOut = 0;
            result << left;
        } else if (c.end() > end) { // cut off the start
            Clip right = c;
            right.in = c.in + (end - c.start);
            right.start = end;
            right.transIn = 0;
            right.fadeIn = 0;
            result << right;
        }
        // otherwise: completely covered -> gone
    }
    track.clips = result;
    sortTrack(track);
}

void placeClip(Track& track, const Clip& clip, const IdGen& newId)
{
    clearRange(track, clip.start, clip.end(), newId);
    track.clips << clip;
    sortTrack(track);
}

bool removeClip(Timeline& tl, int clipId)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        for (auto& t : tl.tracks(k)) {
            for (int i = 0; i < t.clips.size(); ++i) {
                if (t.clips[i].id == clipId) {
                    t.clips.removeAt(i);
                    return true;
                }
            }
        }
    }
    return false;
}

void insertGap(Timeline& tl, const QVector<TrackRef>& tracks, int frame, int length,
               const IdGen& newClipId, const IdGen& newLinkId)
{
    if (length <= 0) return;
    QVector<int> spanning;
    for (const TrackRef& ref : tracks) {
        Track& t = tl.track(ref);
        for (const Clip& c : t.clips)
            if (c.start < frame && c.end() > frame) spanning << c.id;
        clearDissolveAt(t, frame); // the new clip goes right there
    }
    splitAt(tl, spanning, frame, newClipId, newLinkId);
    for (const TrackRef& ref : tracks) shiftFrom(tl.track(ref), frame, length);
}

void shiftFrom(Track& track, int frame, int delta)
{
    for (Clip& c : track.clips)
        if (c.start >= frame) c.start += delta;
    sortTrack(track);
}

void clearDissolveAt(Track& track, int frame)
{
    // Read first, written only where there is one (undo snapshots share the clips of the other tracks)
    for (qsizetype i = 0; i + 1 < track.clips.size(); ++i)
        if (const Clip& a = std::as_const(track.clips)[i]; a.end() == frame && isDissolve(a, std::as_const(track.clips)[i + 1])) {
            track.clips[i].transOut = 0;
            track.clips[i + 1].transIn = 0;
        }
}

QVector<int> splitAt(Timeline& tl, const QVector<int>& clipIds, int frame,
                     const IdGen& newClipId, const IdGen& newLinkId)
{
    QVector<int> created;
    QHash<int, int> linkMap; // old linkId -> new linkId of the right parts
    for (int id : clipIds) {
        TrackRef ref;
        Clip* c = findClip(tl, id, &ref);
        if (!c || frame <= c->start || frame >= c->end()) continue;
        const Clip original = *c;
        Clip right = *c;
        right.id = newClipId();
        right.in = c->in + (frame - c->start);
        right.start = frame;
        right.transIn = 0; // new cut without transition/fade
        right.fadeIn = 0;
        c->transOut = 0;
        c->fadeOut = 0;
        if (c->linkId) {
            if (!linkMap.contains(c->linkId)) linkMap[c->linkId] = newLinkId();
            right.linkId = linkMap[c->linkId];
        }
        c->out = c->in + (frame - c->start) - 1;
        Keys::split(original, *c, right); // split keyframes, interpolated value at the edge
        Track& t = tl.track(ref);
        t.clips << right;
        sortTrack(t);
        created << right.id;
    }
    relinkUncut(tl, linkMap); // a partner that starts after the cut plays with the right piece
    return created;
}

void ensureTracks(Timeline& tl, TrackKind kind, int count)
{
    auto& tracks = tl.tracks(kind);
    while (tracks.size() < count) {
        Track t;
        t.kind = kind; // name empty = default ("Video 3" etc.)
        tracks << t;
    }
}

RippleResult rippleDelete(Timeline& tl, const QVector<int>& clipIds)
{
    // Deleted ranges of all tracks combined
    const QVector<TrackRef> edited = tracksOf(tl, clipIds);
    QVector<QPair<int, int>> ranges;
    struct Span { int start, end, link; bool picture; };
    QVector<Span> spans; // the deleted clips (before)
    for (int id : clipIds) {
        TrackRef ref;
        if (const Clip* c = findClip(tl, id, &ref)) {
            ranges << qMakePair(c->start, c->end());
            spans << Span{c->start, c->end(), c->linkId, ref.kind == TrackKind::Video};
        }
    }
    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<int, int>> merged;
    for (const auto& r : ranges) {
        if (!merged.isEmpty() && r.first <= merged.last().second) merged.last().second = std::max(merged.last().second, r.second);
        else merged << r;
    }
    // What stays on the affected tracks
    QVector<QVector<Clip>> kept;
    QVector<QPair<int, int>> covered; // [start, end) of the clips that stay, all affected tracks
    for (const TrackRef& ref : edited) {
        QVector<Clip> rest;
        for (const Clip& c : std::as_const(tl).track(ref).clips)
            if (!clipIds.contains(c.id)) {
                rest << c;
                covered << qMakePair(c.start, c.end());
            }
        kept << rest;
    }
    std::sort(covered.begin(), covered.end());
    // The time taken out: the deleted time where nothing stays on any affected track. Every track moves up by it
    // together (the others in rippleTracks): each affected track by only its own gaps took linked clips apart (two
    // clips deleted on different tracks at different times), and taking out time where a clip stays on another
    // affected track (an L-cut: the sound of the deleted clip under the next picture) moved that clip against its
    // sound. Nothing on the affected tracks lies in that time, so none of their clips can run into another.
    QVector<QPair<int, int>> shifts; // (end of the time taken out, minus its length)
    for (const auto& r : merged) {
        int from = r.first;
        for (const auto& c : std::as_const(covered)) {
            if (c.second <= from || c.first >= r.second) continue;
            if (c.first > from) shifts << qMakePair(c.first, from - c.first);
            from = std::max(from, c.second);
            if (from >= r.second) break;
        }
        if (from < r.second) shifts << qMakePair(r.second, from - r.second);
    }
    // Only the affected tracks are written here (undo snapshots share the rest)
    for (qsizetype i = 0; i < edited.size(); ++i) {
        QVector<Clip>& rest = kept[i];
        for (Clip& c : rest) {
            int d = 0;
            for (const auto& s : std::as_const(shifts))
                if (s.first <= c.start) d += s.second;
            c.start += d;
        }
        Track& t = tl.track(edited[i]);
        t.clips = rest;
        sortTrack(t);
    }
    rippleTracks(tl, shifts, edited);
    RippleResult result;
    for (const auto& r : std::as_const(merged)) result.deleted += r.second - r.first;
    for (const auto& s : std::as_const(shifts)) result.removed -= s.second;
    // The time meant to go: of a linked group with a picture the picture's time (its sound's own J/L tail under the
    // next clip is no loss, a shorter sound does not make it less), of other clips their time; how much of it went
    QSet<int> withPicture;
    for (const Span& span : std::as_const(spans))
        if (span.link > 0 && span.picture) withPicture.insert(span.link);
    QVector<QPair<int, int>> meant;
    for (const Span& span : std::as_const(spans))
        if (span.picture || span.link <= 0 || !withPicture.contains(span.link)) meant << qMakePair(span.start, span.end);
    std::sort(meant.begin(), meant.end());
    QVector<QPair<int, int>> meantMerged;
    for (const auto& r : std::as_const(meant)) {
        if (!meantMerged.isEmpty() && r.first <= meantMerged.last().second)
            meantMerged.last().second = std::max(meantMerged.last().second, r.second);
        else meantMerged << r;
    }
    for (const auto& m : std::as_const(meantMerged)) {
        result.meant += m.second - m.first;
        for (const auto& s : std::as_const(shifts)) // the time taken out: [end - length, end)
            result.meantRemoved += std::max(0, std::min(m.second, s.first) - std::max(m.first, s.first + s.second));
    }
    return result;
}

int clampTrackDelta(const Timeline& tl, const QVector<int>& clipIds, TrackKind anchorKind, int trackDelta)
{
    for (int id : clipIds) {
        TrackRef ref;
        if (!findClip(tl, id, &ref)) continue;
        trackDelta = std::max(trackDelta, -ref.index);
        if (ref.kind == anchorKind) trackDelta = std::min(trackDelta, int(tl.tracks(ref.kind).size()) - 1 - ref.index);
    }
    return trackDelta;
}

void shiftFine(Clip& c, int fine)
{
    const int total = std::max(0, c.start * 100 + c.subframe + fine);
    c.start = total / 100;
    c.subframe = total % 100;
}

void moveClips(Timeline& tl, const QVector<int>& clipIds, int deltaFrames,
               TrackKind anchorKind, int trackDelta, const IdGen& newId)
{
    trackDelta = clampTrackDelta(tl, clipIds, anchorKind, trackDelta);
    struct Moving { Clip clip; TrackRef target; };
    QVector<Moving> moving;
    for (int id : clipIds) {
        TrackRef ref;
        const Clip* c = findClip(tl, id, &ref);
        if (!c) continue;
        Moving m{*c, ref};
        m.clip.start = std::max(0, c->start + deltaFrames);
        m.target.index = ref.index + trackDelta;
        moving << m;
    }
    // remove all first, then place anew -> clips of the selection do not overwrite each other
    for (const auto& m : moving) {
        removeClip(tl, m.clip.id);
        ensureTracks(tl, m.target.kind, m.target.index + 1);
    }
    for (const auto& m : moving) placeClip(tl.track(m.target), m.clip, newId);
}

int clampTrim(const Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
              const SourceLength& sourceLength, bool overwrite)
{
    for (int id : clipIds) {
        TrackRef ref;
        const Clip* c = findClip(tl, id, &ref);
        if (!c) continue;
        const auto& clips = tl.track(ref).clips;
        const int idx = int(c - clips.constData());
        if (edge == Edge::Start) {
            const int prevEnd = idx > 0 && !overwrite ? clips[idx - 1].end() : 0;
            if (!c->isTitle()) delta = std::max(delta, -c->in); // not before the start of the material
            delta = std::max(delta, prevEnd - c->start);  // not into the left neighbor (nor before frame 0)
            delta = std::min(delta, c->length() - 1);     // at least 1 frame
        } else {
            const int len = sourceLength ? sourceLength(*c) : 0;
            if (len > 0) delta = std::min(delta, len - 1 - c->out);
            if (idx + 1 < clips.size() && !overwrite) delta = std::min(delta, clips[idx + 1].start - c->end());
            delta = std::max(delta, 1 - c->length());
        }
    }
    return delta;
}

void trimClips(Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
               const SourceLength& sourceLength)
{
    delta = clampTrim(tl, clipIds, edge, delta, sourceLength);
    if (delta == 0) return;
    for (int id : clipIds) {
        Clip* c = findClip(tl, id);
        if (!c) continue;
        if (edge == Edge::Start) {
            c->start += delta;
            c->in += delta;
        } else {
            c->out += delta;
        }
    }
}

void trimClipsOver(Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
                   const SourceLength& sourceLength, const IdGen& newId)
{
    delta = clampTrim(tl, clipIds, edge, delta, sourceLength, true);
    if (delta == 0) return;
    for (int id : clipIds) {
        TrackRef ref;
        const Clip* c = findClip(tl, id, &ref);
        if (!c) continue;
        Clip trimmed = *c;
        if (edge == Edge::Start) {
            trimmed.start += delta;
            trimmed.in += delta;
        } else {
            trimmed.out += delta;
        }
        Track& track = tl.track(ref);
        track.clips.removeIf([id](const Clip& o) { return o.id == id; });
        placeClip(track, trimmed, newId); // overwrites what the longer clip covers now
    }
}

namespace {

// Neighbors of a clip on its track (nullptr = none)
struct Neighbors {
    const Clip* clip = nullptr;
    const Clip* prev = nullptr;
    const Clip* next = nullptr;
};
Neighbors neighbors(const Timeline& tl, int clipId)
{
    TrackRef ref;
    const Clip* c = findClip(tl, clipId, &ref);
    if (!c) return {};
    const auto& clips = tl.track(ref).clips;
    const int idx = int(c - clips.constData());
    return {c, idx > 0 ? &clips[idx - 1] : nullptr, idx + 1 < clips.size() ? &clips[idx + 1] : nullptr};
}

// Limits for "move start by d" (start and in move along) or "move end by d" (out moves)
void limitStart(const Clip& c, int& lo, int& hi)
{
    if (!c.isTitle()) lo = std::max(lo, -c.in);
    hi = std::min(hi, c.length() - 1);
}
void limitEnd(const Clip& c, int& lo, int& hi, const SourceLength& sourceLength)
{
    const int len = sourceLength ? sourceLength(c) : 0;
    if (len > 0 && !c.isTitle()) hi = std::min(hi, len - 1 - c.out);
    lo = std::max(lo, 1 - c.length());
}

} // namespace

int clampTrimEdit(const Timeline& tl, const TrimEdit& e, int delta, const SourceLength& sourceLength)
{
    int lo = INT_MIN / 2, hi = INT_MAX / 2;
    switch (e.kind) {
    case TrimKind::Ripple: {
        // Neighbors do not limit: the rest of the track moves along. The other tracks move along from the
        // (earliest) old end; shortening only as far as they have room.
        int from = INT_MAX;
        QMap<QPair<int, int>, int> perTrack; // several clips of one track: the rest moves by the sum
        for (int id : e.ids) {
            TrackRef ref;
            if (const Clip* c = findClip(tl, id, &ref)) {
                if (e.edge == Edge::Start) limitStart(*c, lo, hi);
                else limitEnd(*c, lo, hi, sourceLength);
                from = std::min(from, c->end());
                ++perTrack[qMakePair(int(ref.kind), ref.index)];
            }
        }
        if (from != INT_MAX) {
            int most = 1;
            for (int n : perTrack) most = std::max(most, n);
            const int room = rippleRoom(tl, from, tracksOf(tl, e.ids)) / most;
            if (e.edge == Edge::Start) hi = std::min(hi, room);
            else lo = std::max(lo, -room);
        }
        break;
    }
    case TrimKind::Roll:
        for (int id : e.ids) {
            const auto n = neighbors(tl, id);
            if (!n.clip) continue;
            limitEnd(*n.clip, lo, hi, sourceLength);
            if (n.next && !e.rightIds.contains(n.next->id)) hi = std::min(hi, n.next->start - n.clip->end());
        }
        for (int id : e.rightIds) {
            const auto n = neighbors(tl, id);
            if (!n.clip) continue;
            limitStart(*n.clip, lo, hi);
            // left partner limits via limitEnd, otherwise gap or frame 0
            if (!n.prev || !e.ids.contains(n.prev->id)) lo = std::max(lo, (n.prev ? n.prev->end() : 0) - n.clip->start);
        }
        break;
    case TrimKind::Slip:
        for (int id : e.ids)
            if (const Clip* c = findClip(tl, id); c && !c->isTitle()) {
                lo = std::max(lo, -c->in);
                const int len = sourceLength ? sourceLength(*c) : 0;
                if (len > 0) hi = std::min(hi, len - 1 - c->out);
            }
        break;
    case TrimKind::Slide:
        for (int id : e.ids) {
            const auto n = neighbors(tl, id);
            if (!n.clip) continue;
            // adjacent neighbor is trimmed along, otherwise the gap limits
            if (n.prev && n.prev->end() == n.clip->start && !e.ids.contains(n.prev->id))
                limitEnd(*n.prev, lo, hi, sourceLength);
            else if (!n.prev || !e.ids.contains(n.prev->id))
                lo = std::max(lo, (n.prev ? n.prev->end() : 0) - n.clip->start);
            if (n.next && n.next->start == n.clip->end() && !e.ids.contains(n.next->id))
                limitStart(*n.next, lo, hi);
            else if (n.next && !e.ids.contains(n.next->id))
                hi = std::min(hi, n.next->start - n.clip->end());
        }
        break;
    }
    if (lo > hi) return 0;
    return std::clamp(delta, std::min(lo, 0), std::max(hi, 0));
}

void applyTrimEdit(Timeline& tl, const TrimEdit& e, int delta, const SourceLength& sourceLength)
{
    delta = clampTrimEdit(tl, e, delta, sourceLength);
    if (delta == 0) return;
    switch (e.kind) {
    case TrimKind::Ripple: {
        // per track: clips from the old end of the trimmed clip on move by its length change,
        // the other unlocked tracks likewise
        const QVector<TrackRef> edited = tracksOf(tl, e.ids);
        const int shift = e.edge == Edge::Start ? -delta : delta;
        // Other tracks move like the track with the most trimmed clips (per clip from its old end),
        // otherwise they drift apart with several clips on one track
        QMap<QPair<int, int>, QVector<QPair<int, int>>> perTrack; // (kind, index) -> (old end, offset)
        for (int id : e.ids) {
            TrackRef ref;
            if (const Clip* c = findClip(tl, id, &ref)) perTrack[qMakePair(int(ref.kind), ref.index)] << qMakePair(c->end(), shift);
        }
        QVector<QPair<int, int>> shifts;
        for (const auto& list : perTrack)
            if (list.size() > shifts.size()) shifts = list;
        for (int id : e.ids) {
            TrackRef ref;
            Clip* c = findClip(tl, id, &ref);
            if (!c) continue;
            const int oldEnd = c->end();
            if (e.edge == Edge::Start) c->in += delta;
            else c->out += delta;
            // further trimmed clips of the same track move too (otherwise, depending on the order, a gap remains
            // or the rest slides under them)
            for (Clip& o : tl.track(ref).clips)
                if (o.id != id && o.start >= oldEnd) o.start += shift;
        }
        if (!shifts.isEmpty()) rippleTracks(tl, shifts, edited);
        break;
    }
    case TrimKind::Roll:
        for (int id : e.ids)
            if (Clip* c = findClip(tl, id)) c->out += delta;
        for (int id : e.rightIds)
            if (Clip* c = findClip(tl, id)) {
                c->start += delta;
                c->in += delta;
            }
        break;
    case TrimKind::Slip:
        // Only which source frames play changes: keyframes stay where they are in the clip (stored as source frames,
        // they used to move with the content, some out of the clip)
        for (int id : e.ids)
            if (Clip* c = findClip(tl, id); c && !c->isTitle()) {
                c->in += delta;
                c->out += delta;
                Keys::shiftWithIn(*c, delta);
            }
        break;
    case TrimKind::Slide: {
        // remember neighbors first (pointers stay valid, nothing is inserted/removed)
        QVector<int> prevs, nexts;
        for (int id : e.ids) {
            const auto n = neighbors(tl, id);
            if (!n.clip) continue;
            if (n.prev && n.prev->end() == n.clip->start && !e.ids.contains(n.prev->id)) prevs << n.prev->id;
            if (n.next && n.next->start == n.clip->end() && !e.ids.contains(n.next->id)) nexts << n.next->id;
        }
        for (int id : prevs)
            if (Clip* c = findClip(tl, id)) c->out += delta;
        for (int id : nexts)
            if (Clip* c = findClip(tl, id)) {
                c->start += delta;
                c->in += delta;
            }
        for (int id : e.ids)
            if (Clip* c = findClip(tl, id)) c->start += delta;
        break;
    }
    }
}

QVector<TransitionSpan> transitions(const Track& track, const SourceLength& sourceLength)
{
    QVector<TransitionSpan> spans;
    const auto& clips = track.clips;
    // Portion of the start that a transition already occupies (dissolve + fade out fit in the clip)
    int usedIn = 0;
    for (int i = 0; i < clips.size(); ++i) {
        const Clip& c = clips[i];
        const Clip* prev = i > 0 ? &clips[i - 1] : nullptr;
        const Clip* next = i + 1 < clips.size() ? &clips[i + 1] : nullptr;
        const bool dissolveIn = prev && isDissolve(*prev, c);
        if (!dissolveIn) usedIn = 0; // otherwise already set at the predecessor
        if (c.transIn > 0 && !dissolveIn) { // fade in
            usedIn = std::min(c.transIn, c.length());
            spans << TransitionSpan{0, c.id, c.start, c.start + usedIn, c.start, c.transInStyle};
        }
        if (c.transOut <= 0) continue;
        const int room = c.length() - usedIn;
        if (next && isDissolve(c, *next)) {
            // Dissolve around the cut
            const int srcLen = sourceLength ? sourceLength(c) : 0;
            const int handleOut = srcLen > 0 ? srcLen - 1 - c.out : INT_MAX / 2;
            const int nextSrcLen = sourceLength ? sourceLength(*next) : 0;
            const int handleIn = nextSrcLen > 0 ? next->in : INT_MAX / 2; // still: any
            // l1 frames before the cut (needs material before next->in), l2 after (needs material after c.out)
            const TransitionAlign align = c.transOutStyle.align;
            auto before = [align](int len) {
                return align == TransitionAlign::Start ? 0 : align == TransitionAlign::End ? len : len / 2;
            };
            int len = std::min(c.transOut, next->transIn);
            for (; len > 0; --len) {
                const int l1 = before(len), l2 = len - l1;
                if (l2 <= handleOut && l1 <= handleIn && l1 <= room && l2 <= next->length()) break;
            }
            if (len > 0) {
                const int l1 = before(len), l2 = len - l1;
                spans << TransitionSpan{c.id, next->id, c.end() - l1, c.end() + l2, c.end(), c.transOutStyle};
                usedIn = l2;
            } else {
                usedIn = 0;
            }
            continue; // start of the next clip is thereby done (dissolveIn)
        }
        const int len = std::min(c.transOut, room); // fade out
        if (len > 0) spans << TransitionSpan{c.id, 0, c.end() - len, c.end(), c.end(), c.transOutStyle};
    }
    return spans;
}

double fadeRamp(const Clip& c, double t)
{
    const int length = c.length();
    const int fi = std::min(c.fadeIn, length);
    const int fo = std::min(c.fadeOut, length - fi);
    double g = 1.0;
    if (fi > 0 && t < fi) g = std::min(g, t / fi);
    if (fo > 0 && t >= length - fo) g = std::min(g, (length - 1 - t) / fo);
    return std::clamp(g, 0.0, 1.0);
}

double audioFadeGain(const Clip& c, double t)
{
    return std::sin(fadeRamp(c, t) * M_PI / 2);
}

double audioTransitionGain(const TransitionSpan& s, int clipId, double frame)
{
    if (frame < s.start || frame >= s.end || (clipId != s.leftId && clipId != s.rightId)) return 1.0;
    const int len = std::max(1, s.length());
    const double i = frame - s.start;
    // like the engine: fade in i/len, fade out (len-1-i)/len
    const double t = std::clamp(clipId == s.rightId ? i / len : (len - 1 - i) / len, 0.0, 1.0);
    // Curve like DaVinci (also applies to fading in/out to silence): +3 dB = equal power (sine),
    // 0 dB = linear (levels add up to 1), -3 dB = t^1.5 (middle -9 dB per side)
    switch (s.style.audio) {
    case AudioCurve::Zero: return t;
    case AudioCurve::Minus3dB: return t * std::sqrt(t);
    default: return std::sin(t * M_PI / 2);
    }
}

bool isDissolve(const Clip& a, const Clip& b)
{
    return a.end() == b.start && a.transOut > 0 && b.transIn > 0 && !a.transOutAlone && !b.transInAlone;
}

void detachTransitions(Timeline& tl, const QVector<int>& clipIds)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (auto& t : tl.tracks(k))
            for (int i = 0; i + 1 < t.clips.size(); ++i) {
                Clip& a = t.clips[i];
                Clip& b = t.clips[i + 1];
                if (!isDissolve(a, b)) continue;
                if (clipIds.contains(a.id) == clipIds.contains(b.id)) continue;
                a.transOut = 0;
                b.transIn = 0;
            }
}

namespace {
// ids of the clips whose end (outSide) or start lies in a dissolve
void dissolveSides(const Timeline& tl, QSet<int>& outSide, QSet<int>& inSide)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : tl.tracks(k))
            for (int i = 0; i + 1 < t.clips.size(); ++i) {
                const Clip& a = t.clips[i];
                const Clip& b = t.clips[i + 1];
                if (!isDissolve(a, b)) continue;
                outSide.insert(a.id);
                inSide.insert(b.id);
            }
}
} // namespace

void unpairBrokenDissolves(const Timeline& before, Timeline& after)
{
    QSet<int> outBefore, inBefore, outAfter, inAfter;
    dissolveSides(before, outBefore, inBefore);
    if (outBefore.isEmpty()) return;
    dissolveSides(after, outAfter, inAfter);
    const auto brokenOut = [&](const Clip& c) { return c.transOut > 0 && outBefore.contains(c.id) && !outAfter.contains(c.id); };
    const auto brokenIn = [&](const Clip& c) { return c.transIn > 0 && inBefore.contains(c.id) && !inAfter.contains(c.id); };
    changeClips(after, [&](const Clip& c) { return brokenOut(c) || brokenIn(c); }, [&](Clip& c) {
        if (brokenOut(c)) c.transOut = 0;
        if (brokenIn(c)) c.transIn = 0;
    });
}

void forgetSlotLengths(const Timeline& before, Timeline& after)
{
    // The ids of a slot instance: the clips of the same slot that touch each other on a track
    const auto instance = [](const QVector<Clip>& cs, int i, QSet<int>& ids) {
        int lo = i, hi = i;
        while (lo > 0 && cs[lo - 1].slot.id == cs[i].slot.id && cs[lo - 1].end() == cs[lo].start) --lo;
        while (hi + 1 < cs.size() && cs[hi + 1].slot.id == cs[i].slot.id && cs[hi].end() == cs[hi + 1].start) ++hi;
        for (int k = lo; k <= hi; ++k) ids.insert(cs[k].id);
    };
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        QVector<Track>& tracks = after.tracks(k);
        for (int ti = 0; ti < tracks.size(); ++ti) {
            // Read first, written only where needed (unchanged tracks stay shared with the undo snapshot)
            const QVector<Clip>& cs = std::as_const(tracks)[ti].clips;
            QSet<int> ids; // the instance as it is now and as it was (a trimmed part may not touch the others anymore)
            for (int i = 0; i < cs.size(); ++i) {
                const Clip& c = cs[i];
                if (c.slot.nominalLen <= 0 || ids.contains(c.id)) continue;
                TrackRef ref;
                const Clip* b = findClip(before, c.id, &ref);
                if (b && b->length() == c.length()) continue;
                instance(cs, i, ids);
                if (b) {
                    const QVector<Clip>& bs = before.tracks(ref.kind)[ref.index].clips;
                    for (int j = 0; j < bs.size(); ++j)
                        if (bs[j].id == c.id) instance(bs, j, ids);
                }
            }
            if (ids.isEmpty()) continue;
            for (Clip& c : tracks[ti].clips)
                if (c.slot.nominalLen > 0 && ids.contains(c.id)) c.slot.nominalLen = 0;
        }
    }
}

void keepFadesApart(const Timeline& before, Timeline& after)
{
    // Clips that were already adjacent with a transition at the same edge before (right clip id per left one)
    QHash<int, int> touchedBefore;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : before.tracks(k))
            for (int i = 0; i + 1 < t.clips.size(); ++i) {
                const Clip& a = t.clips[i];
                const Clip& b = t.clips[i + 1];
                if (a.end() == b.start && a.transOut > 0 && b.transIn > 0) touchedBefore.insert(a.id, b.id);
            }
    // The flags are worked out on the side and only changed clips are written (unchanged tracks stay shared with
    // the undo snapshot)
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        QVector<Track>& tracks = after.tracks(k);
        for (int ti = 0; ti < tracks.size(); ++ti) {
            const QVector<Clip>& cs = std::as_const(tracks)[ti].clips;
            const int n = int(cs.size());
            QVector<char> outAlone(n), inAlone(n);
            for (int i = 0; i < n; ++i) {
                outAlone[i] = cs[i].transOutAlone;
                inAlone[i] = cs[i].transInAlone;
            }
            for (int i = 0; i < n; ++i) {
                const Clip& a = cs[i];
                const Clip* b = i + 1 < n ? &cs[i + 1] : nullptr;
                const bool touching = b && a.end() == b->start && a.transOut > 0 && b->transIn > 0;
                if (!touching) {
                    outAlone[i] = false;
                    if (b) inAlone[i + 1] = false;
                    continue;
                }
                if (outAlone[i] || inAlone[i + 1]) { // stays standalone (mark both sides the same)
                    outAlone[i] = inAlone[i + 1] = true;
                    continue;
                }
                if (touchedBefore.value(a.id) == b->id) continue; // was already a dissolve or deliberately made one
                // Newly bumped together: only leave standalone if both transitions were already there before
                const Clip* a0 = findClip(before, a.id);
                const Clip* b0 = findClip(before, b->id);
                if (a0 && b0 && a0->transOut > 0 && b0->transIn > 0) outAlone[i] = inAlone[i + 1] = true;
            }
            // First clip of each track: a fade in without predecessor is never marked standalone
            if (n > 0) inAlone[0] = false;
            for (int i = 0; i < n; ++i) {
                const Clip& c = std::as_const(tracks)[ti].clips[i];
                if (c.transOutAlone == bool(outAlone[i]) && c.transInAlone == bool(inAlone[i])) continue;
                Clip& m = tracks[ti].clips[i];
                m.transOutAlone = outAlone[i];
                m.transInAlone = inAlone[i];
            }
        }
    }
}

int endFrame(const Timeline& tl)
{
    int end = 0;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            if (!t.clips.isEmpty()) end = std::max(end, t.clips.last().end());
    return end;
}

bool sameNested(const Timeline& a, const Timeline& b)
{
    if (a.nested == b.nested) return true;
    if (!a.nested || !b.nested || *a.nested != *b.nested) return false;
    for (auto it = a.nested->cbegin(); it != a.nested->cend(); ++it)
        if (const Timeline other = b.nested->value(it.key()); it->angles != other.angles || it->frameSize != other.frameSize)
            return false; // a sequence's own resolution changed: fitted in anew even when its clips stayed the same
    return true;
}

bool usesMediaOnVideo(const Timeline& tl, const QString& path)
{
    for (const auto& t : tl.video)
        for (const Clip& c : t.clips) {
            if (c.mediaPath == path) return true;
            if (c.isTitle() && c.title.look.texture == path) return true; // a title's texture picture
            for (const EffectInstance& e : c.effects) // a file a compositing node reads
                if (e.effectId == QLatin1String(Comp::kEffectId) && Comp::mediaPaths(e).contains(path)) return true;
        }
    return false;
}

} // namespace TimelineOps
