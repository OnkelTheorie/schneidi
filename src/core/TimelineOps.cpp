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

void ensureTracks(Timeline& tl, TrackKind kind, int count)
{
    auto& tracks = tl.tracks(kind);
    while (tracks.size() < count) {
        Track t;
        t.kind = kind;
        t.name = QString("%1%2").arg(kind == TrackKind::Video ? "V" : "A").arg(tracks.size() + 1);
        tracks << t;
    }
}

void rippleDelete(Timeline& tl, const QVector<int>& clipIds)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        for (auto& t : tl.tracks(k)) {
            QVector<QPair<int, int>> gaps; // gelöschte Bereiche [start, end)
            QVector<Clip> kept;
            for (const Clip& c : t.clips) {
                if (clipIds.contains(c.id)) gaps << qMakePair(c.start, c.end());
                else kept << c;
            }
            if (gaps.isEmpty()) continue;
            for (Clip& c : kept) {
                int shift = 0;
                for (const auto& g : gaps)
                    if (g.second <= c.start) shift += g.second - g.first;
                c.start -= shift;
            }
            t.clips = kept;
            sortTrack(t);
        }
    }
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
    // erst alle entfernen, dann neu platzieren -> Clips der Auswahl überschreiben sich nicht
    for (const auto& m : moving) {
        removeClip(tl, m.clip.id);
        ensureTracks(tl, m.target.kind, m.target.index + 1);
    }
    for (const auto& m : moving) placeClip(tl.track(m.target), m.clip, newId);
}

int clampTrim(const Timeline& tl, const QVector<int>& clipIds, Edge edge, int delta,
              const SourceLength& sourceLength)
{
    for (int id : clipIds) {
        TrackRef ref;
        const Clip* c = findClip(tl, id, &ref);
        if (!c) continue;
        const auto& clips = tl.track(ref).clips;
        const int idx = int(c - clips.constData());
        if (edge == Edge::Start) {
            const int prevEnd = idx > 0 ? clips[idx - 1].end() : 0;
            delta = std::max(delta, -c->in);              // nicht vor den Anfang des Materials
            delta = std::max(delta, prevEnd - c->start);  // nicht in den linken Nachbarn
            delta = std::min(delta, c->length() - 1);     // mind. 1 Frame
        } else {
            const int len = sourceLength ? sourceLength(c->mediaPath) : 0;
            if (len > 0) delta = std::min(delta, len - 1 - c->out);
            if (idx + 1 < clips.size()) delta = std::min(delta, clips[idx + 1].start - c->end());
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

int endFrame(const Timeline& tl)
{
    int end = 0;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            if (!t.clips.isEmpty()) end = std::max(end, t.clips.last().end());
    return end;
}

} // namespace TimelineOps
