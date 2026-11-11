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
void rippleRetimed(Timeline& tl, const QVector<RetimedTrack>& tracks, const QVector<QPair<int, int>>& shifts)
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
        tl.track(tracks[i].ref).clips = together ? joint[i] : moved(tracks[i].clips, tracks[i].own);
        edited << tracks[i].ref;
    }
    TimelineOps::rippleTracks(tl, shifts, edited);
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
        QSet<int> touched;
        QVector<QPair<int, int>> shifts; // ripple (from old end, length change)
        QVector<RetimedTrack> retimed;
        for (const TrackRef& ref : TimelineOps::tracksOf(tl, all)) {
            RetimedTrack t{ref, std::as_const(tl).track(ref).clips, {}};
            for (qsizetype i = t.clips.size() - 1; i >= 0; --i) {
                const Clip& was = std::as_const(t.clips)[i];
                if (!all.contains(was.id) || was.isTitle()) continue;
                const MediaInfo* m = m_project->mediaInfo(was.mediaPath);
                if (!m || m->isImage || m->length <= 0) continue;
                Clip& c = t.clips[i]; // (only now: the copy of the track detaches for a clip that changes)
                const int L = m->length;
                // Constant speed replaces a speed ramp (like DaVinci "Change Clip Speed")
                Clip n = c;
                n.speed = r.speed;
                n.reverse = r.reverse;
                n.freeze = r.freeze;
                n.ramp.clear();
                const RetimeMap oldMap(c, L), newMap(n, L);
                // Section in file frames (a = start of the clip, b = end, a > b when reversed)
                const int a = oldMap.pictureFrame(c.in);
                const int b = oldMap.pictureFrame(c.out);
                const bool flip = r.reverse != c.reverse;
                // New start: same file frame; on direction change the same piece reversed
                const int startFile = flip ? b : a;
                const int avail = std::max(1, int(L / r.speed));
                // The first material frame whose picture is that file frame (rounding could land one before it:
                // a flash of the previous frame at the cut)
                const int newIn = std::clamp(newMap.materialForPicture(startFile), 0, avail - 1);
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
                if (const int delta = newLen - oldLen; ripple && delta != 0) {
                    t.own << qMakePair(oldEnd, delta);
                    // linked partners yield the same offset -> only once
                    if (!shifts.contains(qMakePair(oldEnd, delta))) shifts << qMakePair(oldEnd, delta);
                }
            }
            retimed << t;
        }
        rippleRetimed(tl, retimed, shifts);
        fitTransitions(tl, touched, sourceLength());
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
    QVector<QPair<int, int>> shifts;
    QVector<RetimedTrack> retimed;
    for (const TrackRef& ref : TimelineOps::tracksOf(tl, ids)) {
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
            n.in = std::clamp(int(std::lround(newMap.materialAt(sIn))), 0, avail - 1);
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
            if (delta != 0) {
                t.own << qMakePair(oldEnd, delta);
                if (!shifts.contains(qMakePair(oldEnd, delta))) shifts << qMakePair(oldEnd, delta);
            }
        }
        retimed << t;
    }
    if (touched.isEmpty()) return;
    rippleRetimed(tl, retimed, shifts);
    fitTransitions(tl, touched, sourceLength());
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
