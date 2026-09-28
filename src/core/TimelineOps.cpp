#include "core/TimelineOps.h"

#include <QHash>
#include <algorithm>
#include <climits>

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
            left.transOut = 0;
            left.fadeOut = 0;
            Clip right = c;
            right.id = newId();
            right.linkId = 0;
            right.in = c.in + (end - c.start);
            right.start = end;
            right.transIn = 0;
            right.fadeIn = 0;
            result << left << right;
        } else if (c.start < start) { // Ende abschneiden
            Clip left = c;
            left.out = c.in + (start - c.start) - 1;
            left.transOut = 0;
            left.fadeOut = 0;
            result << left;
        } else if (c.end() > end) { // Anfang abschneiden
            Clip right = c;
            right.in = c.in + (end - c.start);
            right.start = end;
            right.transIn = 0;
            right.fadeIn = 0;
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
        right.transIn = 0; // neuer Schnitt ohne Übergang/Fade
        right.fadeIn = 0;
        c->transOut = 0;
        c->fadeOut = 0;
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
            if (!c->isTitle()) delta = std::max(delta, -c->in); // nicht vor den Anfang des Materials
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

QVector<TransitionSpan> transitions(const Track& track, const SourceLength& sourceLength)
{
    QVector<TransitionSpan> spans;
    const auto& clips = track.clips;
    // Anteil des Anfangs, den ein Übergang schon belegt (Überblendung + Ausblenden passen in den Clip)
    int usedIn = 0;
    for (int i = 0; i < clips.size(); ++i) {
        const Clip& c = clips[i];
        const Clip* prev = i > 0 ? &clips[i - 1] : nullptr;
        const Clip* next = i + 1 < clips.size() ? &clips[i + 1] : nullptr;
        const bool dissolveIn = prev && prev->end() == c.start && prev->transOut > 0 && c.transIn > 0;
        if (!dissolveIn) usedIn = 0; // sonst schon beim Vorgänger gesetzt
        if (c.transIn > 0 && !dissolveIn) { // Einblenden
            usedIn = std::min(c.transIn, c.length());
            spans << TransitionSpan{0, c.id, c.start, c.start + usedIn, c.start, c.transInStyle};
        }
        if (c.transOut <= 0) continue;
        const int room = c.length() - usedIn;
        if (next && next->start == c.end() && next->transIn > 0) {
            // Überblendung um den Schnitt
            const int srcLen = sourceLength ? sourceLength(c.mediaPath) : 0;
            const int handleOut = srcLen > 0 ? srcLen - 1 - c.out : INT_MAX / 2;
            const int nextSrcLen = sourceLength ? sourceLength(next->mediaPath) : 0;
            const int handleIn = nextSrcLen > 0 ? next->in : INT_MAX / 2; // Standbild: beliebig
            // l1 Frames vor dem Schnitt (braucht Material vor next->in), l2 danach (braucht Material hinter c.out)
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
            continue; // Anfang des nächsten Clips ist damit erledigt (dissolveIn)
        }
        const int len = std::min(c.transOut, room); // Ausblenden
        if (len > 0) spans << TransitionSpan{c.id, 0, c.end() - len, c.end(), c.end(), c.transOutStyle};
    }
    return spans;
}

void detachTransitions(Timeline& tl, const QVector<int>& clipIds)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (auto& t : tl.tracks(k))
            for (int i = 0; i + 1 < t.clips.size(); ++i) {
                Clip& a = t.clips[i];
                Clip& b = t.clips[i + 1];
                if (a.end() != b.start || a.transOut <= 0 || b.transIn <= 0) continue;
                if (clipIds.contains(a.id) == clipIds.contains(b.id)) continue;
                a.transOut = 0;
                b.transIn = 0;
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
