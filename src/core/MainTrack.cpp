// SPDX-License-Identifier: AGPL-3.0-only
#include "core/MainTrack.h"

#include "core/I18n.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QHash>
#include <QString>
#include <algorithm>

namespace MainTrack {
namespace {

// Why a shift cannot be done (a fragment after "Lücke auf V1 bleibt: " / "Kein Platz auf V1: ")
QString linkedOnLocked(TrackRef r)
{
    return T("ein verknüpfter Clip liegt auf der gesperrten Spur %1").arg(trackShortName(r));
}

// Moves the clips of track `ref` by `shift` (clip id -> frames) and the clips linked to them on the other tracks by
// the same frames; refused when anything would land before 0, on a locked track or on another clip. ripple: the
// shifts as a ripple (TimelineOps::rippleTracks) for the subtitles and the timeline's annotations.
Result shiftWithLinks(Timeline& tl, TrackRef ref, const QHash<int, int>& shift,
                      const QVector<QPair<int, int>>& ripple, const QString& prefix, QString* why)
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
            if (!st.locked) Subtitles::ripple(st, ripple);
        TimelineOps::rippleAnnotations(tl, ripple, moving, TimelineOps::MarkerCut::MoveToCut);
    }
    return Result::Moved;
}

} // namespace

Result pack(Timeline& tl, TrackRef ref, const QVector<int>& order, QString* why)
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
        const int d = at - c.start;
        if (d != 0) shift.insert(c.id, d);
        if (d != last) ripple << qMakePair(c.start, d - last);
        last = d;
        at += c.length();
    }
    // A reorder moves the time around (no ripple): subtitles and markers stay where they are
    if (!sameOrder) ripple.clear();
    return shiftWithLinks(tl, ref, shift, ripple, T("Lücke auf %1 bleibt: "), why);
}

Result openGap(Timeline& tl, TrackRef ref, int frame, int length, QString* why)
{
    if (length <= 0 || ref.index < 0 || ref.index >= tl.tracks(ref.kind).size() || tl.track(ref).locked)
        return Result::Unchanged;
    QHash<int, int> shift;
    for (const Clip& c : std::as_const(tl).track(ref).clips)
        if (c.start >= frame) shift.insert(c.id, length);
    return shiftWithLinks(tl, ref, shift, {qMakePair(frame, length)}, T("Kein Platz auf %1: "), why);
}

int insertPoint(const Track& t, int frame, const QVector<int>& exclude)
{
    int end = 0;
    for (const Clip& c : t.clips) {
        if (exclude.contains(c.id)) continue;
        if (c.start < frame && frame < c.end()) return frame - c.start < c.end() - frame ? c.start : c.end();
        end = std::max(end, c.end());
    }
    return frame >= end ? end : std::max(0, frame);
}

bool touched(const Timeline& before, const Timeline& after, TrackRef ref)
{
    const auto& a = before.tracks(ref.kind);
    const auto& b = after.tracks(ref.kind);
    if (ref.index >= b.size()) return false;
    if (ref.index >= a.size()) return !b[ref.index].clips.isEmpty();
    const QVector<Clip>& x = a[ref.index].clips;
    const QVector<Clip>& y = b[ref.index].clips;
    if (x.size() != y.size()) return true;
    for (int i = 0; i < x.size(); ++i)
        if (x[i].id != y[i].id || x[i].start != y[i].start || x[i].length() != y[i].length()) return true;
    return false;
}

} // namespace MainTrack
