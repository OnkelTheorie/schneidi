#include "core/TimelineOps.h"

#include <QHash>
#include <algorithm>

namespace {

void sortTrack(Track& t)
{
    std::sort(t.clips.begin(), t.clips.end(),
              [](const Clip& a, const Clip& b) { return a.start < b.start; });
}

template <typename TL, typename C>
C* findIn(TL& tl, int clipId, TrackRef* where)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        auto& tracks = tl.tracks(k);
        for (int i = 0; i < tracks.size(); ++i) {
            for (auto& c : tracks[i].clips) {
                if (c.id == clipId) {
                    if (where) *where = {k, i};
                    return &c;
                }
            }
        }
    }
    return nullptr;
}

} // namespace

namespace TimelineOps {

Clip* findClip(Timeline& tl, int clipId, TrackRef* where)
{
    return findIn<Timeline, Clip>(tl, clipId, where);
}

const Clip* findClip(const Timeline& tl, int clipId, TrackRef* where)
{
    return findIn<const Timeline, const Clip>(tl, clipId, where);
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

void clearRange(Track& track, int start, int end, const IdGen& newId)
{
    if (end <= start) return;
    QVector<Clip> result;
    for (const Clip& c : track.clips) {
        if (c.end() <= start || c.start >= end) { // keine Überschneidung
            result << c;
            continue;
        }
        if (c.start < start && c.end() > end) { // Bereich liegt mitten im Clip -> teilen
            Clip left = c;
            left.out = c.in + (start - c.start) - 1;
            Clip right = c;
            right.id = newId();
            right.linkId = 0;
            right.in = c.in + (end - c.start);
            right.start = end;
            result << left << right;
        } else if (c.start < start) { // Ende abschneiden
            Clip left = c;
            left.out = c.in + (start - c.start) - 1;
            result << left;
        } else if (c.end() > end) { // Anfang abschneiden
            Clip right = c;
            right.in = c.in + (end - c.start);
            right.start = end;
            result << right;
        }
        // sonst: komplett überdeckt -> weg
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

QVector<int> splitAt(Timeline& tl, const QVector<int>& clipIds, int frame,
                     const IdGen& newClipId, const IdGen& newLinkId)
{
    QVector<int> created;
    QHash<int, int> linkMap; // alte linkId -> neue linkId der rechten Teile
    for (int id : clipIds) {
        TrackRef ref;
        Clip* c = findClip(tl, id, &ref);
        if (!c || frame <= c->start || frame >= c->end()) continue;
        Clip right = *c;
        right.id = newClipId();
        right.in = c->in + (frame - c->start);
        right.start = frame;
        if (c->linkId) {
            if (!linkMap.contains(c->linkId)) linkMap[c->linkId] = newLinkId();
            right.linkId = linkMap[c->linkId];
        }
        c->out = c->in + (frame - c->start) - 1;
        Track& t = tl.track(ref);
        t.clips << right;
        sortTrack(t);
        created << right.id;
    }
    return created;
}

void moveClips(Timeline& tl, const QVector<int>& clipIds, int deltaFrames,
               TrackKind trackDeltaKind, int trackDelta, const IdGen& newId)
{
    struct Moving { Clip clip; TrackRef target; };
    QVector<Moving> moving;
    for (int id : clipIds) {
        TrackRef ref;
        const Clip* c = findClip(tl, id, &ref);
        if (!c) continue;
        Moving m{*c, ref};
        m.clip.start = std::max(0, c->start + deltaFrames);
        if (ref.kind == trackDeltaKind) {
            const int count = tl.tracks(ref.kind).size();
            m.target.index = std::clamp(ref.index + trackDelta, 0, count - 1);
        }
        moving << m;
    }
    // erst alle entfernen, dann neu platzieren -> Clips der Auswahl überschreiben sich nicht
    for (const auto& m : moving) removeClip(tl, m.clip.id);
    for (const auto& m : moving) placeClip(tl.track(m.target), m.clip, newId);
}

int endFrame(const Timeline& tl)
{
    int end = 0;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            if (!t.clips.isEmpty()) end = std::max(end, t.clips.last().end());
    return end;
}

} // namespace TimelineOps
