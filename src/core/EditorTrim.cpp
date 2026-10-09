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
        if (c.isCompound()) return p->sequenceLength(c.sequenceId); // Inhalt der Sequenz
        const MediaInfo* m = p->mediaInfo(c.mediaPath);
        return m && !m->isImage ? c.retimedLength(m->length) : 0; // Standbilder beliebig lang ziehbar
    };
}

int Editor::clampTrim(int clipId, TimelineOps::Edge edge, int delta) const
{
    return TimelineOps::clampTrim(m_project->timeline(), withLinked({clipId}), edge, delta, sourceLength());
}

void Editor::trimClip(int clipId, TimelineOps::Edge edge, int delta)
{
    const QVector<int> ids = withLinked({clipId});
    if (TimelineOps::clampTrim(m_project->timeline(), ids, edge, delta, sourceLength()) == 0) return;
    m_project->edit(T("Trimmen"), [&](Timeline& tl) { TimelineOps::trimClips(tl, ids, edge, delta, sourceLength()); });
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
        e.ids = withLinked({clipId});
        return e;
    }
    // Roll: auf jeder Spur der Partner den Clip, der am Schnitt endet, und den, der dort beginnt
    const int cut = edge == Edge::Start ? c->start : c->end();
    TrackRef ref;
    findClip(tl, clipId, &ref);
    QVector<int> seeds = withLinked({clipId});
    for (const Clip& o : tl.track(ref).clips) // Nachbar auf der anderen Seite des Schnitts
        if (o.id != clipId && (edge == Edge::Start ? o.end() == cut : o.start == cut)) seeds += withLinked({o.id});
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

void Editor::applyTrimEdit(const TimelineOps::TrimEdit& e, int delta)
{
    using TimelineOps::TrimKind;
    if (e.isNull() || clampTrimEdit(e, delta) == 0) return;
    const QString text = e.kind == TrimKind::Ripple ? T("Ripple-Trimmen")
                         : e.kind == TrimKind::Roll ? T("Schnitt verschieben (Roll)")
                         : e.kind == TrimKind::Slip ? T("Inhalt verschieben (Slip)")
                                                    : T("Clip verschieben (Slide)");
    m_project->edit(text, [&](Timeline& tl) { TimelineOps::applyTrimEdit(tl, e, delta, sourceLength()); });
}

void Editor::slipSelection(int frames)
{
    if (m_selection->isEmpty()) return;
    TimelineOps::TrimEdit e;
    e.kind = TimelineOps::TrimKind::Slip;
    e.ids = withLinked(m_selection->ids().values().toVector());
    applyTrimEdit(e, frames);
}

void Editor::selectNearestEditPoint(int frame)
{
    const Timeline& tl = m_project->timeline();
    EditPoint best;
    int bestDist = INT_MAX;
    for (TrackRef ref : {TrackRef{TrackKind::Video, m_targetVideo}, TrackRef{TrackKind::Audio, m_targetAudio}}) {
        if (ref.index < 0 || ref.index >= tl.tracks(ref.kind).size() || isTrackLocked(ref)) continue;
        const auto& clips = tl.track(ref).clips; // nach Start sortiert
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
    if (!e.leftId || !e.rightId) return; // Kante an einer Lücke: nur eine Seite
    e.side = e.side == 0 ? -1 : e.side < 0 ? 1 : 0;
    m_selection->setEditPoint(e);
}

bool Editor::nudgeEditPoint(int frames, bool ripple)
{
    using namespace TimelineOps;
    const EditPoint e = m_selection->editPoint();
    if (e.isNull()) return false;
    const Timeline& tl = m_project->timeline();
    if (!findClip(tl, e.leftId) && !findClip(tl, e.rightId)) { // Clip gelöscht o. ä.
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
