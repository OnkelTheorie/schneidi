// SPDX-License-Identifier: AGPL-3.0-only
// Editor: clip speed and speed ramps.
#include "core/Editor.h"
#include "core/EditorDetail.h"

#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Retime.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <optional>

using EditorDetail::fitTransitions;

namespace {

// A track with retimed clips: its clips with the new lengths at their old places, and its length changes (old end ->
// offset)
struct RetimedTrack {
    TrackRef ref;
    QVector<Clip> clips;
    QVector<QPair<int, int>> own;
};

// Ripple after retiming: the retimed tracks move by all length changes together, like the other tracks
// (rippleTracks) - each by only its own changes took linked clips apart (two clips on different tracks at different
// times). If that does not fit on one of them (a clip there lies across a change on another track and would run into
// its neighbor), each one moves by its own changes, as before. Writes only these tracks (undo snapshots share the rest).
// Returns a track where even its own changes do not fit (a clip there would run into another one: a group member that
// starts after the group's earliest end moves up with it, onto a clip before it) - then tl is left half done: refuse.
std::optional<TrackRef> rippleRetimed(Timeline& tl, const QVector<RetimedTrack>& tracks, const QVector<QPair<int, int>>& shifts)
{
    const auto moved = [](QVector<Clip> clips, const QVector<QPair<int, int>>& by) {
        for (qsizetype i = 0; i < clips.size(); ++i) {
            int d = 0;
            for (const auto& s : by)
                if (s.first <= std::as_const(clips)[i].start) d += s.second;
            if (d) clips[i].start += d;
        }
        return clips;
    };
    const auto fits = [](const QVector<Clip>& clips) {
        for (qsizetype i = 0; i < clips.size(); ++i)
            if (clips[i].start < (i ? clips[i - 1].end() : 0)) return false;
        return true;
    };
    QVector<QVector<Clip>> joint;
    bool together = true;
    for (const RetimedTrack& t : tracks) {
        joint << moved(t.clips, shifts);
        together = together && fits(joint.last());
    }
    QVector<TrackRef> edited;
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        QVector<Clip> clips = together ? joint[i] : moved(tracks[i].clips, tracks[i].own);
        if (!fits(clips)) return tracks[i].ref;
        tl.track(tracks[i].ref).clips = clips;
        edited << tracks[i].ref;
    }
    TimelineOps::rippleTracks(tl, shifts, edited);
    return std::nullopt;
}

QString noRoom(TrackRef ref)
{
    return T("Nicht ausgeführt: Auf %1 liefe ein Clip in den davor – dort ist kein Platz").arg(trackShortName(ref));
}

// Linked clips of one file in sync (same start - in, same playback) stay in sync to the frame through a retime: one
// offset (start - new in) for all of them - each rounded alone, an L-cut's sound (another in point) could end up a
// frame off its picture. The offset is the first one (pictures first: theirs is rounded to show the right first
// frame) that gives every clip of the group an in point within its material; none does: each its own.
class SyncedIns {
public:
    // First every clip with its own new in point and the highest one its material allows
    void offer(const Clip& c, int own, int hi, bool picture)
    {
        m_members << Member{c, own, hi, picture};
    }
    // Then: the new in point of `c` (before the change; its own: `own`)
    int in(const Clip& c, int own)
    {
        if (!m_settled) settle();
        for (const Member& m : std::as_const(m_members))
            if (m.was.id == c.id) return m.offset ? std::clamp(c.start - *m.offset, 0, m.hi) : own;
        return own;
    }

private:
    struct Member {
        Clip was;
        int own, hi;
        bool picture;
        std::optional<int> offset = {};
    };
    static bool together(const Clip& a, const Clip& b)
    {
        // Linked to each other and side by side in time: razor-cut pieces of one take (same file, same start - in,
        // other links) are no group - one offset for them squeezed a piece to a frame
        return a.linkId > 0 && a.linkId == b.linkId && a.start < b.end() && b.start < a.end() && a.mediaPath == b.mediaPath
               && a.start - a.in == b.start - b.in && a.speed == b.speed && a.reverse == b.reverse && a.freeze == b.freeze
               && a.ramp == b.ramp;
    }
    void settle()
    {
        m_settled = true;
        // Pictures first, as offered otherwise
        std::stable_sort(m_members.begin(), m_members.end(),
                         [](const Member& a, const Member& b) { return a.picture && !b.picture; });
        for (qsizetype i = 0; i < m_members.size(); ++i) {
            if (m_members[i].offset) continue;
            QVector<qsizetype> group;
            for (qsizetype j = i; j < m_members.size(); ++j)
                if (!m_members[j].offset && together(m_members[i].was, m_members[j].was)) group << j;
            if (group.size() < 2) continue;
            for (qsizetype g : std::as_const(group)) {
                const int offset = m_members[g].was.start - m_members[g].own;
                const bool fits = std::all_of(group.cbegin(), group.cend(), [&](qsizetype k) {
                    const int in = m_members[k].was.start - offset;
                    return in >= 0 && in <= m_members[k].hi;
                });
                if (!fits) continue;
                for (qsizetype k : std::as_const(group)) m_members[k].offset = offset;
                break;
            }
        }
    }
    QVector<Member> m_members;
    bool m_settled = false;
};

// The length changes of retimed clips, rippled as one per link group: everything after the group moves by the same
// amount - per clip, picture and sound of an L/J-cut (other ends, other changes) added up and moved what follows by
// the sum. The group's shift starts at its earliest old end and is the largest change, the least the next clip on
// each of its tracks needs (slower: a gap after the shorter one; faster: a gap after the longer one).
class LengthChanges {
public:
    void add(int link, int oldEnd, int delta, qsizetype track)
    {
        if (delta != 0) m_changes << Change{link, oldEnd, delta, track};
    }
    // The shifts for all tracks; each retimed track's own ones (its groups') into `own`
    QVector<QPair<int, int>> shifts(QVector<RetimedTrack>& tracks) const
    {
        QVector<QPair<int, int>> all, groupShift;
        QVector<bool> done(m_changes.size(), false);
        QVector<qsizetype> groupOf(m_changes.size(), 0);
        for (qsizetype i = 0; i < m_changes.size(); ++i) {
            if (done[i]) continue;
            int from = m_changes[i].oldEnd, by = m_changes[i].delta;
            QVector<qsizetype> members{i};
            if (m_changes[i].link > 0)
                for (qsizetype j = i + 1; j < m_changes.size(); ++j)
                    if (!done[j] && m_changes[j].link == m_changes[i].link) {
                        members << j;
                        from = std::min(from, m_changes[j].oldEnd);
                        by = std::max(by, m_changes[j].delta);
                    }
            groupShift << qMakePair(from, by);
            for (qsizetype m : std::as_const(members)) {
                done[m] = true;
                groupOf[m] = groupShift.size() - 1;
            }
        }
        // Groups changing at the same place (separate clips ending together, their changes apart by rounding: +100 and
        // +99) are one change there, the larger: added up, what follows moved by both (199)
        for (auto& g : groupShift)
            for (const auto& o : std::as_const(groupShift))
                if (o.first == g.first) g.second = std::max(g.second, o.second);
        for (const auto& g : std::as_const(groupShift))
            if (!all.contains(g)) all << g;
        for (qsizetype m = 0; m < m_changes.size(); ++m) {
            QVector<QPair<int, int>>& own = tracks[m_changes[m].track].own;
            if (!own.contains(groupShift[groupOf[m]])) own << groupShift[groupOf[m]];
        }
        return all;
    }

private:
    struct Change {
        int link, oldEnd, delta;
        qsizetype track;
    };
    QVector<Change> m_changes;
};

// The tracks of the clips, video first (see SyncedIns)
QVector<TrackRef> tracksVideoFirst(const Timeline& tl, const QVector<int>& ids)
{
    QVector<TrackRef> refs = TimelineOps::tracksOf(tl, ids);
    std::stable_sort(refs.begin(), refs.end(), [](const TrackRef& a, const TrackRef& b) {
        return a.kind == TrackKind::Video && b.kind != TrackKind::Video;
    });
    return refs;
}

} // namespace

void Editor::setFrameInterpolation(const QVector<int>& ids, FrameInterp mode)
{
    QVector<int> targets;
    for (const int id : withFollowers(ids)) {
        const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
        if (c && !c->isTitle() && c->interpolate != mode) targets << id;
    }
    if (targets.isEmpty()) return;
    modifyClips(targets, T("Zwischenbilder ändern"), [mode](Clip& c) { c.interpolate = mode; });
}

void Editor::setClipSpeed(const QVector<int>& ids, const Retime& r, bool ripple)
{
    // A freeze frame keeps the clip's length: exactly the given clips (a picture without its sound freezes while the
    // sound plays on in sync; a selection with the sound makes it silent); any other speed takes the partners along
    const QVector<int> all = r.freeze ? withCopies(editable(ids)) : withFollowers(ids);
    if (all.isEmpty() || r.speed <= 0) return;
    m_project->edit(T("Geschwindigkeit ändern"), [&](Timeline& tl) {
        const Timeline before = tl;
        QSet<int> touched;
        LengthChanges changes; // ripple: one shift per link group
        QVector<RetimedTrack> retimed;
        // The new in point of a clip on its own: the first material frame whose picture is the same file frame as
        // before (on direction change: the same piece reversed; rounding could land one before it: a flash of the
        // previous frame at the cut)
        const auto ownIn = [&](const Clip& c, int L, int* avail) {
            Clip n = c;
            n.speed = r.speed;
            n.reverse = r.reverse;
            n.freeze = r.freeze;
            n.ramp.clear();
            const RetimeMap oldMap(c, L), newMap(n, L);
            const int startFile = r.reverse != c.reverse ? oldMap.pictureFrame(c.out) : oldMap.pictureFrame(c.in);
            *avail = std::max(1, int(L / r.speed));
            return std::clamp(newMap.materialForPicture(startFile), 0, *avail - 1);
        };
        const auto retimable = [&](const Clip& c) -> const MediaInfo* {
            if (!all.contains(c.id) || c.isTitle()) return nullptr;
            const MediaInfo* m = m_project->mediaInfo(c.mediaPath);
            return m && !m->isImage && m->length > 0 ? m : nullptr;
        };
        const QVector<TrackRef> refs = tracksVideoFirst(tl, all);
        SyncedIns ins;
        for (const TrackRef& ref : refs)
            for (const Clip& c : std::as_const(tl).track(ref).clips)
                if (const MediaInfo* m = retimable(c)) {
                    int avail = 0;
                    const int own = ownIn(c, m->length, &avail);
                    ins.offer(c, own, avail - 1, ref.kind == TrackKind::Video);
                }
        for (const TrackRef& ref : refs) {
            RetimedTrack t{ref, std::as_const(tl).track(ref).clips, {}};
            for (qsizetype i = t.clips.size() - 1; i >= 0; --i) {
                const MediaInfo* m = retimable(std::as_const(t.clips)[i]);
                if (!m) continue;
                Clip& c = t.clips[i]; // (only now: the copy of the track detaches for a clip that changes)
                const int L = m->length;
                // Constant speed replaces a speed ramp (like DaVinci "Change Clip Speed")
                Clip n = c;
                n.speed = r.speed;
                n.reverse = r.reverse;
                n.freeze = r.freeze;
                n.ramp.clear();
                const RetimeMap oldMap(c, L), newMap(n, L);
                int avail = 0;
                const int newIn = ins.in(c, ownIn(c, L, &avail)); // in sync with its partners (SyncedIns)
                const int oldLen = c.length();
                int newLen = oldLen;
                if (ripple && !r.freeze && !c.freeze) {
                    // The same file time [t0, t1) (speed ramps included): up to the last frame starting before its
                    // end (rounding the source span / speed could show a picture beyond the old out-point)
                    const double s0 = oldMap.sourceAt(c.in), s1 = oldMap.sourceAt(c.out + 1);
                    const double t0 = c.reverse ? L - s1 : s0, t1 = c.reverse ? L - s0 : s1;
                    const int past = int(std::ceil(newMap.materialAt(r.reverse ? L - t0 : t1) - 1e-6));
                    newLen = std::max(1, past - newIn);
                }
                if (!r.freeze) newLen = std::min(newLen, avail - newIn);
                // Convert keyframes via the file frames
                for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
                    KeyTrack mapped = it.value();
                    QVector<double> exact;
                    for (Keyframe& kf : mapped) {
                        exact << newMap.materialAtFile(oldMap.fileFrameAt(kf.frame));
                        kf.frame = int(std::lround(exact.last()));
                    }
                    Keys::rescaleHandles(it.value(), mapped);
                    it.value() = Keys::onePerFrame(mapped, exact);
                }
                const int oldEnd = c.end();
                c.in = newIn;
                c.out = newIn + newLen - 1;
                c.speed = r.speed;
                c.slot.ownSpeed = 0; // the user's speed is the slot's own one now (not a fill's slow motion)
                c.reverse = r.reverse;
                c.freeze = r.freeze;
                c.freezeFrame = r.freeze ? newIn : -1; // frame at the clip start (stays when splitting/trimming)
                c.keepPitch = r.keepPitch;
                if (r.interpolate) c.interpolate = *r.interpolate;
                c.ramp.clear();
                c.fadeIn = std::min(c.fadeIn, newLen);
                c.fadeOut = std::min(c.fadeOut, newLen);
                touched.insert(c.id);
                if (ripple) changes.add(c.linkId, oldEnd, newLen - oldLen, retimed.size());
            }
            retimed << t;
        }
        const QVector<QPair<int, int>> shifts = changes.shifts(retimed);
        if (const std::optional<TrackRef> full = rippleRetimed(tl, retimed, shifts)) {
            tl = before;
            tell(noRoom(*full), true);
            return;
        }
        fitTransitions(tl, touched, sourceLength());
        if (!shifts.isEmpty()) keepsLinks(before, tl, touched);
    });
}

// ---- Speed Ramp (Retime Controls) ----

bool Editor::canRetime(int clipId) const
{
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || c->isTitle() || c->isNested() || c->freeze) return false;
    const MediaInfo* m = m_project->mediaInfo(c->mediaPath);
    return m && !m->isImage && m->length > 0;
}

void Editor::editRamp(const QString& text, int clipId, const std::function<bool(Clip&, const RetimeMap&, int)>& change)
{
    if (!canRetime(clipId) || !editable({clipId}).contains(clipId)) return;
    const Timeline& cur = m_project->timeline();
    const Clip* first = TimelineOps::findClip(cur, clipId);
    const MediaInfo* m = m_project->mediaInfo(first->mediaPath);
    const int L = m->length;
    // Partners of the same file take part (video + audio), foreign partners (other audio) do not
    QVector<int> ids;
    for (int id : withFollowers({clipId}))
        if (const Clip* c = TimelineOps::findClip(cur, id); c && c->mediaPath == first->mediaPath && !c->freeze)
            ids << id;
    // First check on the main clip whether anything changes (no empty undo step)
    {
        Clip probe = *first;
        if (!change(probe, RetimeMap(*first, L), L)) return;
    }
    Timeline tl = cur;
    QSet<int> touched;
    LengthChanges changes;
    QVector<RetimedTrack> retimed;
    // The new in point of a clip on its own (the same source position); nullopt = the change does not apply
    const auto ownIn = [&](const Clip& c, int* avail) -> std::optional<int> {
        Clip n = c;
        if (!change(n, RetimeMap(c, L), L)) return std::nullopt;
        ::Retime::normalize(n.ramp, L);
        const RetimeMap newMap(n, L);
        *avail = std::max(1, newMap.length());
        return std::clamp(int(std::lround(newMap.materialAt(RetimeMap(c, L).sourceAt(c.in)))), 0, *avail - 1);
    };
    const QVector<TrackRef> refs = tracksVideoFirst(tl, ids);
    SyncedIns ins;
    for (const TrackRef& ref : refs)
        for (const Clip& c : std::as_const(tl).track(ref).clips) {
            int avail = 0;
            if (!ids.contains(c.id)) continue;
            if (const std::optional<int> own = ownIn(c, &avail))
                ins.offer(c, *own, avail - 1, ref.kind == TrackKind::Video);
        }
    for (const TrackRef& ref : refs) {
        RetimedTrack t{ref, std::as_const(tl).track(ref).clips, {}};
        for (qsizetype i = t.clips.size() - 1; i >= 0; --i) {
            if (!ids.contains(std::as_const(t.clips)[i].id)) continue;
            const Clip c = std::as_const(t.clips)[i]; // a copy: the track is written only below, if it changes
            const RetimeMap oldMap(c, L);
            // Remember source positions: start, end (exclusive), keyframes
            const double sIn = oldMap.sourceAt(c.in), sEnd = oldMap.sourceAt(c.out + 1);
            QMap<AnimParam, QVector<double>> keySrc;
            for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it)
                for (const Keyframe& kf : it.value()) keySrc[it.key()] << oldMap.sourceAt(kf.frame);
            Clip n = c;
            if (!change(n, oldMap, L)) continue;
            ::Retime::normalize(n.ramp, L);
            const RetimeMap newMap(n, L);
            const int avail = std::max(1, newMap.length());
            n.in = ins.in(c, std::clamp(int(std::lround(newMap.materialAt(sIn))), 0, avail - 1)); // see SyncedIns
            const int len = std::clamp(int(std::lround(newMap.materialAt(sEnd))) - n.in, 1, avail - n.in);
            n.out = n.in + len - 1;
            for (auto it = n.keys.begin(); it != n.keys.end(); ++it) {
                KeyTrack mapped = it.value();
                const QVector<double>& src = keySrc[it.key()];
                mapped.resize(std::min(mapped.size(), src.size()));
                QVector<double> exact;
                for (int j = 0; j < mapped.size(); ++j) {
                    exact << newMap.materialAt(src[j]);
                    mapped[j].frame = int(std::lround(exact.last()));
                }
                Keys::rescaleHandles(it.value(), mapped);
                it.value() = Keys::onePerFrame(mapped, exact);
            }
            n.fadeIn = std::min(n.fadeIn, len);
            n.fadeOut = std::min(n.fadeOut, len);
            const int oldEnd = c.end();
            const int delta = n.length() - c.length();
            t.clips[i] = n;
            touched.insert(n.id);
            changes.add(n.linkId, oldEnd, delta, retimed.size());
        }
        retimed << t;
    }
    if (touched.isEmpty()) return;
    const QVector<QPair<int, int>> shifts = changes.shifts(retimed);
    if (const std::optional<TrackRef> full = rippleRetimed(tl, retimed, shifts)) {
        tell(noRoom(*full), true);
        return;
    }
    fitTransitions(tl, touched, sourceLength());
    if (!keepsLinks(cur, tl, touched)) return;
    m_project->edit(text, [&](Timeline& t) { t = tl; });
}

void Editor::addSpeedPoint(int clipId, int frame)
{
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || frame <= c->start || frame >= c->end()) return;
    const int m = c->in + frame - c->start;
    editRamp(T("Speed-Punkt hinzufügen"), clipId, [m](Clip& x, const RetimeMap& map, int) {
        const double s = map.sourceAt(m);
        for (const SpeedPoint& p : x.ramp)
            if (std::abs(p.source - s) < 1) return false;
        SpeedPoint p;
        p.source = s;
        // new section keeps the speed of that spot (hard value of the section, without transition)
        int seg = 0;
        while (seg < x.ramp.size() && x.ramp[seg].source <= s) ++seg;
        p.speed = ::Retime::segmentSpeed(x, seg);
        x.ramp << p;
        std::sort(x.ramp.begin(), x.ramp.end(), [](const SpeedPoint& a, const SpeedPoint& b) { return a.source < b.source; });
        return true;
    });
}

void Editor::removeSpeedPoint(int clipId, int index)
{
    editRamp(T("Speed-Punkt entfernen"), clipId, [index](Clip& x, const RetimeMap&, int) {
        if (index < 0 || index >= x.ramp.size()) return false;
        x.ramp.removeAt(index);
        return true;
    });
}

void Editor::moveSpeedPoint(int clipId, int index, int delta)
{
    if (delta == 0) return;
    editRamp(T("Speed-Punkt verschieben"), clipId, [index, delta](Clip& x, const RetimeMap&, int L) {
        if (index < 0 || index >= x.ramp.size()) return false;
        // Speed before continues or ends earlier -> new source position from the speed of the section before
        const double before = ::Retime::segmentSpeed(x, index);
        const double lo = (index > 0 ? x.ramp[index - 1].source : 0) + 1;
        const double hi = (index + 1 < x.ramp.size() ? x.ramp[index + 1].source : L) - 1;
        const double s = std::clamp(x.ramp[index].source + delta * before, lo, hi);
        if (std::abs(s - x.ramp[index].source) < 1e-6) return false;
        x.ramp[index].source = s;
        return true;
    });
}

void Editor::setSegmentSpeed(int clipId, int segment, double speed)
{
    if (speed <= 0) return;
    editRamp(T("Geschwindigkeit ändern"), clipId, [segment, speed](Clip& x, const RetimeMap&, int) {
        if (segment < 0 || segment > x.ramp.size()) return false;
        const double before = ::Retime::segmentSpeed(x, segment);
        ::Retime::setSegmentSpeed(x, segment, speed);
        return ::Retime::segmentSpeed(x, segment) != before;
    });
}

void Editor::setSpeedPointSmooth(int clipId, int index, int frames)
{
    editRamp(T("Übergang des Speed-Punkts"), clipId, [index, frames](Clip& x, const RetimeMap&, int) {
        if (index < 0 || index >= x.ramp.size() || x.ramp[index].smooth == std::max(0, frames)) return false;
        x.ramp[index].smooth = std::max(0, frames);
        return true;
    });
}
