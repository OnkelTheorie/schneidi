// SPDX-License-Identifier: AGPL-3.0-only
// Editor: trimming (edges, trim mode, slip, edit points, trim to playhead).
#include "core/Editor.h"

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

TimelineOps::SourceLength Editor::sourceLength() const
{
    return [p = m_project](const Clip& c) {
        if (c.isNested()) return p->sequenceLength(c.sequenceId); // content of the sequence
        const MediaInfo* m = p->mediaInfo(c.mediaPath);
        return m && !m->isImage ? c.retimedLength(m->length) : 0; // stills can be stretched to any length
    };
}

int Editor::clampTrim(int clipId, TimelineOps::Edge edge, int delta, bool overwrite) const
{
    return TimelineOps::clampTrim(m_project->timeline(), withFollowers({clipId}), edge, delta, sourceLength(), overwrite);
}

void Editor::trimClip(int clipId, TimelineOps::Edge edge, int delta, bool overwrite)
{
    const QVector<int> ids = withFollowers({clipId});
    if (TimelineOps::clampTrim(m_project->timeline(), ids, edge, delta, sourceLength(), overwrite) == 0) return;
    m_project->edit(T("Trimmen"), [&](Timeline& tl) {
        if (overwrite) TimelineOps::trimClipsOver(tl, ids, edge, delta, sourceLength(), [this] { return m_project->newClipId(); });
        else TimelineOps::trimClips(tl, ids, edge, delta, sourceLength());
    });
}

Timeline Editor::previewTrim(int clipId, TimelineOps::Edge edge, int delta, bool overwrite) const
{
    Timeline tl = m_project->timeline();
    const QVector<int> ids = withFollowers({clipId});
    int nextId = -1; // a preview never keeps its ids (and an extended edge splits no neighbor)
    if (overwrite) TimelineOps::trimClipsOver(tl, ids, edge, delta, sourceLength(), [&nextId] { return nextId--; });
    else TimelineOps::trimClips(tl, ids, edge, delta, sourceLength());
    return tl;
}

TimelineOps::TrimEdit Editor::trimEdit(TimelineOps::TrimKind kind, int clipId, TimelineOps::Edge edge) const
{
    using namespace TimelineOps;
    const Timeline& tl = m_project->timeline();
    TrimEdit e;
    e.kind = kind;
    e.edge = edge;
    const Clip* c = findClip(tl, clipId);
    if (!c) return e;
    if (kind != TrimKind::Roll) {
        e.ids = withFollowers({clipId});
        return e;
    }
    // Roll: on each partner track the clip that ends at the cut and the one that starts there
    const int cut = edge == Edge::Start ? c->start : c->end();
    TrackRef ref;
    findClip(tl, clipId, &ref);
    QVector<int> seeds = withFollowers({clipId});
    for (const Clip& o : tl.track(ref).clips) // neighbor on the other side of the cut
        if (o.id != clipId && (edge == Edge::Start ? o.end() == cut : o.start == cut)) seeds += withFollowers({o.id});
    QVector<TrackRef> done;
    for (int id : seeds) {
        if (!findClip(tl, id, &ref) || done.contains(ref)) continue;
        done << ref;
        for (const Clip& o : tl.track(ref).clips) {
            if (o.end() == cut) e.ids << o.id;
            if (o.start == cut) e.rightIds << o.id;
        }
    }
    return e;
}

int Editor::clampTrimEdit(const TimelineOps::TrimEdit& e, int delta) const
{
    return TimelineOps::clampTrimEdit(m_project->timeline(), e, delta, sourceLength());
}

Timeline Editor::previewTrimEdit(const TimelineOps::TrimEdit& e, int delta) const
{
    Timeline tl = m_project->timeline();
    TimelineOps::applyTrimEdit(tl, e, delta, sourceLength());
    return tl;
}

QString Editor::trimEditRefusal(const TimelineOps::TrimEdit& e, int delta) const
{
    QString why;
    allowedTrimEdit(e, delta, &why);
    return why;
}

int Editor::allowedTrimEdit(const TimelineOps::TrimEdit& e, int delta, QString* why, Timeline* preview) const
{
    const Timeline& now = m_project->timeline();
    const QSet<int> trimmed(e.ids.cbegin(), e.ids.cend());
    const auto refusal = [&](const Timeline& after) { return linksRefusal(now, after, trimmed, nullptr); };
    Timeline at = previewTrimEdit(e, delta);
    QString reason = e.kind == TimelineOps::TrimKind::Ripple && delta != 0 ? refusal(at) : QString();
    if (why) *why = reason;
    if (reason.isEmpty()) {
        if (preview) *preview = at;
        return delta;
    }
    // The furthest allowed one between 0 (always: nothing changes) and delta (refused); the farther, the more comes
    // apart, so halving finds it in a few previews
    int ok = 0, no = delta;
    Timeline okTimeline = now;
    while (std::abs(no - ok) > 1) {
        const int mid = ok + (no - ok) / 2;
        Timeline t = previewTrimEdit(e, mid);
        if (refusal(t).isEmpty()) {
            ok = mid;
            okTimeline = std::move(t);
        } else {
            no = mid;
        }
    }
    if (preview) *preview = okTimeline;
    return ok;
}

void Editor::applyTrimEdit(const TimelineOps::TrimEdit& e, int delta)
{
    using TimelineOps::TrimKind;
    if (e.isNull() || clampTrimEdit(e, delta) == 0) return;
    const QString text = e.kind == TrimKind::Ripple ? T("Trimmen mit Nachrücken")
                         : e.kind == TrimKind::Roll ? T("Schnittpunkt verschieben")
                         : e.kind == TrimKind::Slip ? T("Inhalt im Clip verschieben")
                                                    : T("Clip zwischen den Nachbarn gleiten lassen");
    m_project->edit(text, [&](Timeline& tl) {
        const Timeline before = tl;
        TimelineOps::applyTrimEdit(tl, e, delta, sourceLength());
        // A ripple that a blocked or locked track cannot follow: refused (the trimmed clips change on purpose)
        if (e.kind == TrimKind::Ripple) keepsLinks(before, tl, QSet<int>(e.ids.cbegin(), e.ids.cend()));
    });
}

TimelineOps::TrimEdit Editor::splitEdit(int clipId, TimelineOps::Edge edge) const
{
    using namespace TimelineOps;
    const Timeline& tl = m_project->timeline();
    TrimEdit e;
    e.kind = TrimKind::Roll;
    TrackRef ref;
    const Clip* c = findClip(tl, clipId, &ref);
    if (!c || isTrackLocked(ref)) return e;
    const int cut = edge == Edge::Start ? c->start : c->end();
    QVector<int> left, right;
    for (const Clip& o : tl.track(ref).clips) {
        if (o.end() == cut) left << o.id;
        if (o.start == cut) right << o.id;
    }
    e.ids = withCopies(left);
    e.rightIds = withCopies(right);
    return e;
}

std::optional<Editor::SoundEdge> Editor::soundEdge(int leftId, int rightId) const
{
    using namespace TimelineOps;
    const Timeline& tl = m_project->timeline();
    const Clip* l = findClip(tl, leftId);
    const Clip* r = findClip(tl, rightId);
    if (!l || !r || l->end() != r->start || !l->linkId || !r->linkId || l->linkId == r->linkId) return std::nullopt;
    SoundEdge s;
    s.cut = r->start;
    s.lo = INT_MIN / 2;
    s.hi = INT_MAX / 2;
    bool first = true;
    for (int i = 0; i < tl.audio.size(); ++i) {
        if (tl.audio[i].locked) continue;
        const auto& clips = tl.audio[i].clips; // sorted by start
        for (int k = 0; k + 1 < clips.size(); ++k) {
            const Clip& a = clips[k];
            const Clip& b = clips[k + 1];
            if (a.id == leftId || b.id == rightId || a.linkId != l->linkId || b.linkId != r->linkId || a.end() != b.start)
                continue;
            TrimEdit e;
            e.kind = TrimKind::Roll;
            e.ids = {a.id};
            e.rightIds = {b.id};
            const int offset = a.end() - s.cut;
            if (first) s.offset = offset; // the sound cut as it is (the first track; the others follow it)
            first = false;
            s.lo = std::max(s.lo, offset + clampTrimEdit(e, INT_MIN / 4));
            s.hi = std::min(s.hi, offset + clampTrimEdit(e, INT_MAX / 4));
            s.pairs << qMakePair(a.id, b.id);
        }
    }
    if (s.pairs.isEmpty()) return std::nullopt;
    if (s.lo > s.hi) s.lo = s.hi = s.offset; // the tracks stand differently: only what they share
    return s;
}

bool Editor::setSoundEdge(int leftId, int rightId, int offset)
{
    const std::optional<SoundEdge> s = soundEdge(leftId, rightId);
    if (!s || offset < s->lo || offset > s->hi) return false;
    const auto len = sourceLength();
    m_project->edit(T("J/L-Schnitt"), [&](Timeline& tl) {
        for (const auto& p : s->pairs) {
            const Clip* a = TimelineOps::findClip(tl, p.first);
            if (!a) continue;
            TimelineOps::TrimEdit e;
            e.kind = TimelineOps::TrimKind::Roll;
            e.ids = {p.first};
            e.rightIds = {p.second};
            TimelineOps::applyTrimEdit(tl, e, s->cut + offset - a->end(), len);
        }
    });
    return true;
}

QVector<int> Editor::fillRoomTone(int track, const QVector<RoomTone::Fill>& fills)
{
    if (fills.isEmpty() || isTrackLocked({TrackKind::Audio, track})) return {};
    QVector<int> ids;
    m_project->edit(T("Raumton füllen"), [&](Timeline& tl) {
        for (const RoomTone::Fill& f : fills) {
            const MediaInfo* m = m_project->mediaInfo(f.take.mediaPath);
            ids += RoomTone::fill(tl, track, f, m ? m->length : 0, [this] { return m_project->newClipId(); });
        }
    });
    return ids;
}

void Editor::slipSelection(int frames)
{
    if (m_selection->isEmpty()) return;
    TimelineOps::TrimEdit e;
    e.kind = TimelineOps::TrimKind::Slip;
    e.ids = withFollowers(m_selection->ids().values().toVector());
    applyTrimEdit(e, frames);
}

void Editor::slideSelection(int frames)
{
    TimelineOps::TrimEdit e;
    e.kind = TimelineOps::TrimKind::Slide;
    e.ids = withFollowers(clipIdsOf(m_selection->ids()));
    applyTrimEdit(e, frames);
}

void Editor::selectNearestEditPoint(int frame)
{
    const Timeline& tl = m_project->timeline();
    EditPoint best;
    int bestDist = INT_MAX;
    for (TrackRef ref : {TrackRef{TrackKind::Video, m_targetVideo}, TrackRef{TrackKind::Audio, m_targetAudio}}) {
        if (ref.index < 0 || ref.index >= tl.tracks(ref.kind).size() || isTrackLocked(ref)) continue;
        const auto& clips = tl.track(ref).clips; // sorted by start
        for (int i = 0; i < clips.size(); ++i) {
            const Clip& c = clips[i];
            const bool joined = i > 0 && clips[i - 1].end() == c.start;
            auto consider = [&](int at, const EditPoint& e) {
                if (std::abs(at - frame) < bestDist) {
                    bestDist = std::abs(at - frame);
                    best = e;
                }
            };
            consider(c.start, joined ? EditPoint{clips[i - 1].id, c.id, 0} : EditPoint{0, c.id, 1});
            if (i + 1 >= clips.size() || clips[i + 1].start != c.end()) consider(c.end(), EditPoint{c.id, 0, -1});
        }
    }
    if (!best.isNull()) m_selection->setEditPoint(best);
}

void Editor::cycleEditPointSide()
{
    EditPoint e = m_selection->editPoint();
    if (!e.leftId || !e.rightId) return; // edge at a gap: only one side
    e.side = e.side == 0 ? -1 : e.side < 0 ? 1 : 0;
    m_selection->setEditPoint(e);
}

bool Editor::nudgeEditPoint(int frames, bool ripple)
{
    using namespace TimelineOps;
    const EditPoint e = m_selection->editPoint();
    if (e.isNull()) return false;
    const Timeline& tl = m_project->timeline();
    if (!findClip(tl, e.leftId) && !findClip(tl, e.rightId)) { // clip deleted or similar
        m_selection->clear();
        return false;
    }
    if (e.side == 0 && e.leftId && e.rightId) {
        const TrimEdit roll = trimEdit(TrimKind::Roll, e.leftId, Edge::End);
        if (const int d = clampTrimEdit(roll, frames)) applyTrimEdit(roll, d);
        return true;
    }
    const int id = e.side < 0 ? e.leftId : e.rightId;
    const Edge edge = e.side < 0 ? Edge::End : Edge::Start;
    if (ripple) {
        const TrimEdit r = trimEdit(TrimKind::Ripple, id, edge);
        if (const int d = clampTrimEdit(r, frames)) applyTrimEdit(r, d);
    } else if (const int d = clampTrim(id, edge, frames)) {
        trimClip(id, edge, d);
    }
    return true;
}

void Editor::trimToPlayhead(TimelineOps::Edge edge, int frame)
{
    const Timeline& cur = m_project->timeline();
    QVector<int> ids;
    for (int id : targetIds(frame))
        if (const Clip* c = TimelineOps::findClip(cur, id); c && frame > c->start && frame < c->end()) ids << id;
    if (ids.isEmpty()) return;
    const auto len = sourceLength();
    m_project->edit(edge == TimelineOps::Edge::Start ? T("Anfang trimmen") : T("Ende trimmen"), [&](Timeline& tl) {
        for (int id : ids) {
            const Clip* c = TimelineOps::findClip(tl, id);
            if (!c) continue;
            const int delta = frame - (edge == TimelineOps::Edge::Start ? c->start : c->end());
            TimelineOps::trimClips(tl, {id}, edge, delta, len);
        }
    });
}
