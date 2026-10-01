#include "core/TimelineOps.h"

#include "core/Keyframes.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <climits>
#include <cmath>

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
            // Reihenfolge bleibt; überschneidet sich etwas, bleibt die Spur wie sie ist
            bool ok = moved.first().start >= 0;
            for (int j = 1; ok && j < moved.size(); ++j) ok = moved[j].start >= moved[j - 1].end();
            if (!ok) continue;
            // auseinandergerückte Überblendung lösen (sonst würden daraus Aus- und Einblenden)
            for (int j = 1; j < moved.size(); ++j)
                if (t.clips[j - 1].end() == t.clips[j].start && moved[j - 1].end() != moved[j].start
                    && isDissolve(t.clips[j - 1], t.clips[j])) {
                    moved[j - 1].transOut = 0;
                    moved[j].transIn = 0;
                }
            t.clips = moved;
        }
    }
}

int rippleRoom(const Timeline& tl, int from, const QVector<TrackRef>& skip)
{
    int room = INT_MAX / 2;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = tl.tracks(k);
        for (int i = 0; i < tracks.size(); ++i) {
            const Track& t = tracks[i];
            if (t.locked || skip.contains(TrackRef{k, i})) continue;
            int standEnd = 0, firstMoving = -1; // Ende der stehenbleibenden Clips, Anfang des ersten rückenden
            for (const Clip& c : t.clips) {
                if (c.start < from) standEnd = std::max(standEnd, c.end());
                else if (firstMoving < 0) firstMoving = c.start;
            }
            if (firstMoving >= 0) room = std::min(room, firstMoving - standEnd);
        }
    }
    return std::max(0, room);
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

void resolvePendingLinks(Timeline& tl, const IdGen& newLinkId)
{
    QHash<int, int> map;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : tl.tracks(k))
            for (Clip& c : t.clips)
                if (c.linkId < 0) {
                    if (!map.contains(c.linkId)) map[c.linkId] = newLinkId();
                    c.linkId = map[c.linkId];
                }
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
            right.linkId = c.linkId > 0 ? -c.linkId : c.linkId; // vorläufig, siehe resolvePendingLinks
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

void insertGap(Timeline& tl, const QVector<TrackRef>& tracks, int frame, int length,
               const IdGen& newClipId, const IdGen& newLinkId)
{
    if (length <= 0) return;
    QVector<int> spanning;
    for (const TrackRef& ref : tracks) {
        Track& t = tl.track(ref);
        for (Clip& c : t.clips) {
            if (c.start < frame && c.end() > frame) spanning << c.id;
            // Überblendung genau am Einfügepunkt fällt weg, dort liegt gleich der neue Clip
            if (c.end() == frame) c.transOut = 0;
            if (c.start == frame) c.transIn = 0;
        }
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

void clearEdgeTransitions(Track& track, int start, int end)
{
    for (Clip& c : track.clips) {
        if (c.end() == start) c.transOut = 0;
        if (c.start == end) c.transIn = 0;
    }
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
        const Clip original = *c;
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
        Keys::split(original, *c, right); // Keyframes aufteilen, an der Kante interpolierter Wert
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
        t.kind = kind; // Name leer = Standard („Video 3“ usw.)
        tracks << t;
    }
}

void rippleDelete(Timeline& tl, const QVector<int>& clipIds)
{
    // gelöschte Bereiche aller Spuren zusammengefasst -> Versatz für die übrigen Spuren
    const QVector<TrackRef> edited = tracksOf(tl, clipIds);
    QVector<QPair<int, int>> ranges;
    for (int id : clipIds)
        if (const Clip* c = findClip(tl, id)) ranges << qMakePair(c->start, c->end());
    std::sort(ranges.begin(), ranges.end());
    QVector<QPair<int, int>> merged;
    for (const auto& r : ranges) {
        if (!merged.isEmpty() && r.first <= merged.last().second) merged.last().second = std::max(merged.last().second, r.second);
        else merged << r;
    }
    QVector<QPair<int, int>> shifts;
    for (const auto& r : merged) shifts << qMakePair(r.second, r.first - r.second);

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
    rippleTracks(tl, shifts, edited);
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
            const int len = sourceLength ? sourceLength(*c) : 0;
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

namespace {

// Nachbarn eines Clips auf seiner Spur (nullptr = keiner)
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

// Grenzen für "Anfang um d verschieben" (start und in wandern mit) bzw. "Ende um d verschieben" (out wandert)
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
        // Nachbarn begrenzen nicht: der Rest der Spur rückt mit. Die übrigen Spuren rücken ab dem
        // (frühesten) alten Ende mit; Verkürzen nur so weit, wie sie Platz haben.
        int from = INT_MAX;
        QMap<QPair<int, int>, int> perTrack; // mehrere Clips einer Spur: der Rest rückt um die Summe
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
            // linker Partner begrenzt über limitEnd, sonst Lücke bzw. Frame 0
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
            // anliegender Nachbar wird mitgetrimmt, sonst begrenzt die Lücke
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
        // pro Spur: Clips ab dem alten Ende des getrimmten Clips rücken um dessen Längenänderung,
        // die übrigen nicht gesperrten Spuren ebenso
        const QVector<TrackRef> edited = tracksOf(tl, e.ids);
        const int shift = e.edge == Edge::Start ? -delta : delta;
        // Übrige Spuren rücken wie die Spur mit den meisten getrimmten Clips (je Clip ab dessen altem Ende),
        // sonst laufen sie bei mehreren Clips einer Spur auseinander
        QMap<QPair<int, int>, QVector<QPair<int, int>>> perTrack; // (Art, Index) -> (altes Ende, Versatz)
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
            // auch weitere getrimmte Clips derselben Spur rücken (sonst bleibt je nach Reihenfolge eine Lücke
            // oder der Rest rutscht unter sie)
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
        for (int id : e.ids)
            if (Clip* c = findClip(tl, id); c && !c->isTitle()) {
                c->in += delta;
                c->out += delta;
            }
        break;
    case TrimKind::Slide: {
        // erst Nachbarn merken (Zeiger bleiben gültig, es wird nichts eingefügt/entfernt)
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
    // Anteil des Anfangs, den ein Übergang schon belegt (Überblendung + Ausblenden passen in den Clip)
    int usedIn = 0;
    for (int i = 0; i < clips.size(); ++i) {
        const Clip& c = clips[i];
        const Clip* prev = i > 0 ? &clips[i - 1] : nullptr;
        const Clip* next = i + 1 < clips.size() ? &clips[i + 1] : nullptr;
        const bool dissolveIn = prev && isDissolve(*prev, c);
        if (!dissolveIn) usedIn = 0; // sonst schon beim Vorgänger gesetzt
        if (c.transIn > 0 && !dissolveIn) { // Einblenden
            usedIn = std::min(c.transIn, c.length());
            spans << TransitionSpan{0, c.id, c.start, c.start + usedIn, c.start, c.transInStyle};
        }
        if (c.transOut <= 0) continue;
        const int room = c.length() - usedIn;
        if (next && isDissolve(c, *next)) {
            // Überblendung um den Schnitt
            const int srcLen = sourceLength ? sourceLength(c) : 0;
            const int handleOut = srcLen > 0 ? srcLen - 1 - c.out : INT_MAX / 2;
            const int nextSrcLen = sourceLength ? sourceLength(*next) : 0;
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
    // wie die Engine: einblenden i/len, ausblenden (len-1-i)/len
    const double t = std::clamp(clipId == s.rightId ? i / len : (len - 1 - i) / len, 0.0, 1.0);
    // Kurve wie DaVinci (gilt auch fürs Ein-/Ausblenden zur Stille): +3 dB = gleiche Leistung (Sinus),
    // 0 dB = linear (Pegel addiert sich zu 1), -3 dB = t^1,5 (Mitte je Seite -9 dB)
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
// ids der Clips, deren Ende (outSide) bzw. Anfang in einer Überblendung liegt
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
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : after.tracks(k))
            for (Clip& c : t.clips) {
                if (c.transOut > 0 && outBefore.contains(c.id) && !outAfter.contains(c.id)) c.transOut = 0;
                if (c.transIn > 0 && inBefore.contains(c.id) && !inAfter.contains(c.id)) c.transIn = 0;
            }
}

void keepFadesApart(const Timeline& before, Timeline& after)
{
    // Clips, die vorher schon mit Übergang an derselben Kante aneinanderlagen (rechte Clip-id je linker)
    QHash<int, int> touchedBefore;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : before.tracks(k))
            for (int i = 0; i + 1 < t.clips.size(); ++i) {
                const Clip& a = t.clips[i];
                const Clip& b = t.clips[i + 1];
                if (a.end() == b.start && a.transOut > 0 && b.transIn > 0) touchedBefore.insert(a.id, b.id);
            }
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : after.tracks(k))
            for (int i = 0; i < t.clips.size(); ++i) {
                Clip& a = t.clips[i];
                Clip* b = i + 1 < t.clips.size() ? &t.clips[i + 1] : nullptr;
                const bool touching = b && a.end() == b->start && a.transOut > 0 && b->transIn > 0;
                if (!touching) {
                    a.transOutAlone = false;
                    if (b) b->transInAlone = false;
                    continue;
                }
                if (a.transOutAlone || b->transInAlone) { // bleibt eigenständig (beide Seiten gleich markieren)
                    a.transOutAlone = b->transInAlone = true;
                    continue;
                }
                if (touchedBefore.value(a.id) == b->id) continue; // war schon Überblendung bzw. bewusst dazu gemacht
                // Neu aneinandergestoßen: nur eigenständig lassen, wenn beide Übergänge schon vorher da waren
                const Clip* a0 = findClip(before, a.id);
                const Clip* b0 = findClip(before, b->id);
                if (a0 && b0 && a0->transOut > 0 && b0->transIn > 0) a.transOutAlone = b->transInAlone = true;
            }
    // Erster Clip jeder Spur: Einblenden ohne Vorgänger ist nie eigenständig markiert
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : after.tracks(k))
            if (!t.clips.isEmpty()) t.clips.first().transInAlone = false;
}

int endFrame(const Timeline& tl)
{
    int end = 0;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            if (!t.clips.isEmpty()) end = std::max(end, t.clips.last().end());
    return end;
}

bool usesMediaOnVideo(const Timeline& tl, const QString& path)
{
    for (const auto& t : tl.video)
        for (const Clip& c : t.clips)
            if (c.mediaPath == path) return true;
    return false;
}

} // namespace TimelineOps
