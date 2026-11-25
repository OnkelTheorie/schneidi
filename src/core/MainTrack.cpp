// SPDX-License-Identifier: AGPL-3.0-only
#include "core/MainTrack.h"

#include "core/Automation.h"
#include "core/Captions.h"
#include "core/I18n.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QHash>
#include <QString>
#include <algorithm>
#include <functional>

namespace MainTrack {
namespace {

// Why a shift cannot be done (a fragment after "Lücke auf V1 bleibt: " / "Kein Platz auf V1: ")
QString linkedOnLocked(TrackRef r)
{
    return T("ein verknüpfter Clip liegt auf der gesperrten Spur %1").arg(trackShortName(r));
}

// Moves the clips of track `ref` by `shift` (clip id -> frames) and the clips linked to them on the other tracks by
// the same frames; refused when anything would land before 0, on a locked track or on another clip. ripple: the
// shifts as a ripple (TimelineOps::rippleTracks) for the timeline's annotations and the moving tracks' automation;
// cues: what the unlocked subtitle tracks do (with a ripple only).
Result shiftWithLinks(Timeline& tl, TrackRef ref, const QHash<int, int>& shift,
                      const QVector<QPair<int, int>>& ripple, const std::function<void(SubtitleTrack&)>& cues,
                      const QString& prefix, QString* why)
{
    if (shift.isEmpty()) return Result::Unchanged;
    const auto refuse = [&](const QString& reason) {
        if (why) *why = prefix.arg(trackShortName(ref)) + reason;
        return Result::Refused;
    };
    const Track& own = std::as_const(tl).track(ref);
    QHash<int, int> byLink; // linkId -> frames
    for (const Clip& c : own.clips) {
        const int d = shift.value(c.id, 0);
        if (!c.linkId) continue;
        // Two clips of the track sharing a link (whatever made them) cannot both take theirs along
        if (const auto it = byLink.constFind(c.linkId); it != byLink.cend() && *it != d)
            return refuse(T("zwei Clips der Spur teilen sich eine Verknüpfung"));
        byLink.insert(c.linkId, d);
    }
    // The track itself: in order, without overlap, not before 0
    {
        QVector<QPair<int, int>> spans;
        for (const Clip& c : own.clips) {
            const int start = c.start + shift.value(c.id, 0);
            if (start < 0) return refuse(T("ein Clip käme vor Bild 0"));
            spans << qMakePair(start, start + c.length());
        }
        std::sort(spans.begin(), spans.end());
        for (int i = 1; i < spans.size(); ++i)
            if (spans[i].first < spans[i - 1].second) return refuse(T("Clips würden sich überdecken"));
    }
    // The linked clips go along - only where that keeps everything (else a gap stays; nothing is cut or goes out of
    // sync)
    QVector<TrackRef> moving{ref};
    for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = std::as_const(tl).tracks(kind);
        for (int t = 0; t < tracks.size(); ++t) {
            const TrackRef here{kind, t};
            if (here == ref) continue;
            QVector<QPair<int, int>> spans; // the track's clips as they would be
            bool shifted = false;
            for (const Clip& c : tracks[t].clips) {
                int start = c.start;
                if (const int d = c.linkId ? byLink.value(c.linkId, 0) : 0; d != 0) {
                    start += d;
                    shifted = true;
                    if (start < 0) return refuse(T("ein verknüpfter Clip auf %1 käme vor Bild 0").arg(trackShortName(here)));
                }
                spans << qMakePair(start, start + c.length());
            }
            if (!shifted) continue;
            if (tracks[t].locked) return refuse(linkedOnLocked(here));
            std::sort(spans.begin(), spans.end());
            for (int i = 1; i < spans.size(); ++i)
                if (spans[i].first < spans[i - 1].second)
                    return refuse(T("ein verknüpfter Clip auf %1 würde einen anderen Clip überdecken").arg(trackShortName(here)));
            moving << here;
        }
    }
    for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        auto& tracks = tl.tracks(kind);
        for (int t = 0; t < tracks.size(); ++t) {
            const bool isOwn = TrackRef{kind, t} == ref;
            bool changed = false;
            for (Clip& c : tracks[t].clips) {
                const int d = isOwn ? shift.value(c.id, 0) : (c.linkId ? byLink.value(c.linkId, 0) : 0);
                if (d == 0) continue;
                c.start += d;
                changed = true;
            }
            if (changed)
                std::stable_sort(tracks[t].clips.begin(), tracks[t].clips.end(),
                                 [](const Clip& a, const Clip& b) { return a.start < b.start; });
        }
    }
    if (!ripple.isEmpty()) {
        // Subtitles belong to the picture: the unlocked subtitle tracks follow (locked ones stay put, as on a ripple)
        for (SubtitleTrack& st : tl.subtitles)
            if (!st.locked) cues(st);
        // A track whose clips move takes its automation along, as on a ripple (Automation.h: a dip under a sentence
        // stays under it); the tracks that stay keep theirs
        for (const TrackRef& r : std::as_const(moving)) Automation::rippleTrack(tl.track(r), ripple);
        TimelineOps::rippleAnnotations(tl, ripple, moving, TimelineOps::MarkerCut::MoveToCut);
    }
    return Result::Moved;
}

// A pack's subtitles: each entry moves with the clip it starts in; one in a closed gap lands on the cut like a marker.
// Lengths stay; where entries would overlap they keep their order and the earlier one is shortened only as far as
// needed (to 1 frame at least, then the later one moves on) - a pack never deletes a subtitle.
void packCues(SubtitleTrack& st, const QVector<Clip>& byStart, const QHash<int, int>& shift)
{
    if (st.cues.isEmpty() || byStart.isEmpty()) return;
    const auto map = [&](int t) {
        int at = byStart.first().start + shift.value(byStart.first().id, 0); // before the first clip: its new start
        for (const Clip& c : byStart) {
            if (c.start > t) break;
            const int d = shift.value(c.id, 0);
            at = t < c.end() ? t + d : c.end() + d; // in the clip, or in the gap after it: on the cut
        }
        return at;
    };
    QVector<SubtitleCue> cues = st.cues;
    QVector<int> s(cues.size()), e(cues.size());
    for (int i = 0; i < cues.size(); ++i) {
        s[i] = map(cues[i].start);
        e[i] = s[i] + (cues[i].end - cues[i].start);
    }
    for (int i = 1; i < cues.size(); ++i) {
        if (s[i] >= e[i - 1]) continue;
        e[i - 1] = std::max(s[i - 1] + 1, s[i]);
        if (s[i] < e[i - 1]) {
            const int len = e[i] - s[i];
            s[i] = e[i - 1];
            e[i] = s[i] + std::max(1, len);
        }
    }
    bool changed = false;
    for (int i = 0; i < cues.size(); ++i) {
        SubtitleCue& c = cues[i];
        if (c.start == s[i] && c.end == e[i]) continue;
        changed = true;
        const bool shorter = e[i] - s[i] < c.end - c.start;
        c.start = s[i]; // (word times count from the start: they move along)
        c.end = e[i];
        if (shorter) {
            SubtitleCue cropped = c;
            Captions::crop(cropped, s[i], e[i]);
            if (!Captions::lostAllWords(c, cropped)) c = cropped;
        }
    }
    if (changed) st.cues = cues;
}

} // namespace

Result pack(Timeline& tl, TrackRef ref, const QVector<int>& order, QString* why, const QHash<int, int>& keep)
{
    if (ref.index < 0 || ref.index >= tl.tracks(ref.kind).size() || tl.track(ref).locked) return Result::Unchanged;
    QVector<Clip> clips = std::as_const(tl).track(ref).clips;
    std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    const QVector<Clip> byStart = clips;
    if (!order.isEmpty()) {
        QVector<Clip> sorted;
        for (int id : order)
            for (const Clip& c : byStart)
                if (c.id == id) sorted << c;
        for (const Clip& c : byStart) // (any the order left out: after it)
            if (!order.contains(c.id)) sorted << c;
        clips = sorted;
    }
    bool sameOrder = true;
    for (int i = 0; i < clips.size(); ++i) sameOrder &= clips[i].id == byStart[i].id;
    QHash<int, int> shift; // clip id -> frames
    QVector<QPair<int, int>> ripple; // (frame before the edit, offset) as on a ripple: the time of each gap taken out
    int at = 0, last = 0;
    for (const Clip& c : clips) {
        at += std::max(0, keep.value(c.id, 0));
        const int d = at - c.start;
        if (d != 0) shift.insert(c.id, d);
        if (d != last) ripple << qMakePair(c.start, d - last);
        last = d;
        at += c.length();
    }
    // A reorder moves the time around (no ripple): subtitles and markers stay where they are
    if (!sameOrder) ripple.clear();
    return shiftWithLinks(tl, ref, shift, ripple, [&](SubtitleTrack& st) { packCues(st, byStart, shift); },
                          T("Lücke auf %1 bleibt: "), why);
}

QHash<int, int> gapsBefore(const Track& t)
{
    QVector<Clip> clips = t.clips;
    std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    QHash<int, int> gaps;
    int end = 0;
    for (const Clip& c : clips) {
        if (c.start > end) gaps.insert(c.id, c.start - end);
        end = std::max(end, c.end());
    }
    return gaps;
}

QHash<int, int> keptGaps(const Timeline& before, const Timeline& after, TrackRef ref)
{
    if (ref.index < 0 || ref.index >= before.tracks(ref.kind).size() || ref.index >= after.tracks(ref.kind).size())
        return {};
    const auto byStart = [](QVector<Clip> clips) {
        std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        return clips;
    };
    const QVector<Clip> was = byStart(before.track(ref).clips);
    const QVector<Clip> now = byStart(after.track(ref).clips);
    QHash<int, int> nowIds, wasIds;
    for (const Clip& c : now) nowIds.insert(c.id, 1);
    for (const Clip& c : was) wasIds.insert(c.id, 1);
    // Before: the gap frames up to each clip that stays on the track (the gaps before removed clips go to the next one)
    QHash<int, int> budgetOf;
    int pending = 0, end = 0;
    for (const Clip& c : was) {
        pending += std::max(0, c.start - end);
        end = std::max(end, c.end());
        if (nowIds.contains(c.id)) {
            budgetOf.insert(c.id, pending);
            pending = 0;
        }
    }
    // After: each stretch up to an old clip (new clips in it included) keeps at most that clip's budget
    QVector<int> budget(now.size(), 0);
    int cur = 0;
    for (int i = int(now.size()) - 1; i >= 0; --i) {
        if (wasIds.contains(now[i].id)) cur = budgetOf.value(now[i].id, 0);
        budget[i] = cur; // (the stretch's budget; clips after the last old clip: 0)
    }
    QHash<int, int> keep;
    int left = 0;
    end = 0;
    for (int i = 0; i < now.size(); ++i) {
        if (i == 0 || wasIds.contains(now[i - 1].id)) left = budget[i]; // a new stretch starts
        const int gap = std::max(0, now[i].start - end);
        const int k = std::min(gap, left);
        if (k > 0) keep.insert(now[i].id, k);
        left -= k;
        end = std::max(end, now[i].end());
    }
    return keep;
}

Result openGap(Timeline& tl, TrackRef ref, int frame, int length, QString* why)
{
    if (length <= 0 || ref.index < 0 || ref.index >= tl.tracks(ref.kind).size() || tl.track(ref).locked)
        return Result::Unchanged;
    QHash<int, int> shift;
    for (const Clip& c : std::as_const(tl).track(ref).clips)
        if (c.start >= frame) shift.insert(c.id, length);
    const QVector<QPair<int, int>> ripple{qMakePair(frame, length)};
    return shiftWithLinks(tl, ref, shift, ripple, [&](SubtitleTrack& st) { Subtitles::ripple(st, ripple); },
                          T("Kein Platz auf %1: "), why);
}

int insertPoint(const Track& t, int frame, const QVector<int>& exclude, int atLeast)
{
    int end = 0, at = -1;
    for (const Clip& c : t.clips) {
        if (exclude.contains(c.id)) continue;
        if (at < 0 && c.start < frame && frame < c.end()) at = frame - c.start < c.end() - frame ? c.start : c.end();
        end = std::max(end, c.end());
    }
    if (at < 0) at = frame >= end ? end : std::max(0, frame);
    if (at >= atLeast) return at;
    int best = std::max(end, atLeast); // the first cut at or after atLeast
    for (const Clip& c : t.clips)
        if (!exclude.contains(c.id))
            for (int cut : {c.start, c.end()})
                if (cut >= atLeast) best = std::min(best, cut);
    return best;
}

bool touched(const Timeline& before, const Timeline& after, TrackRef ref)
{
    const auto& a = before.tracks(ref.kind);
    const auto& b = after.tracks(ref.kind);
    // Tracks added or removed: another track may be V1 now (it is no edit of V1's clips)
    if (a.size() != b.size() || ref.index >= b.size()) return false;
    const QVector<Clip>& x = a[ref.index].clips;
    const QVector<Clip>& y = b[ref.index].clips;
    if (x.size() != y.size()) return true;
    for (int i = 0; i < x.size(); ++i)
        if (x[i].id != y[i].id || x[i].start != y[i].start || x[i].length() != y[i].length()) return true;
    return false;
}

} // namespace MainTrack
