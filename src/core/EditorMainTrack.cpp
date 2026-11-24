// SPDX-License-Identifier: AGPL-3.0-only
// Editor: the magnetic main track (core/MainTrack.h) - packing, inserting onto V1, moving along it, trimming on it.
#include "core/Editor.h"

#include "core/I18n.h"
#include "core/MainTrack.h"
#include "core/Project.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QSet>
#include <algorithm>
#include <climits>
#include <cstdlib>

bool Editor::packTrack(TrackRef ref, const QVector<int>& order, const QString& text)
{
    Timeline probe = m_project->timeline();
    QString why;
    if (MainTrack::pack(probe, ref, order, &why) != MainTrack::Result::Moved) {
        if (!why.isEmpty()) tell(why, true);
        return false;
    }
    m_project->edit(text.isEmpty() ? T("Hauptspur schließen") : text,
                    [&](Timeline& tl) { MainTrack::pack(tl, ref, order); });
    return true;
}

void Editor::setMagneticMain(bool on)
{
    m_project->setMagneticMain(on);
}

bool Editor::magneticMain() const
{
    return m_project->magneticMain();
}

bool Editor::magnetActive() const
{
    return m_project->magnetActive();
}

bool Editor::mainInsert(int& frame, int length, bool* room, int atLeast)
{
    *room = false;
    const Timeline& cur = m_project->timeline();
    if (!magnetActive() || length <= 0 || cur.video.isEmpty() || isTrackLocked(MainTrack::kMain)) return true;
    frame = MainTrack::insertPoint(cur.video[0], std::max(0, frame), {}, atLeast);
    Timeline probe = cur;
    QString why;
    if (MainTrack::openGap(probe, MainTrack::kMain, frame, length, &why) == MainTrack::Result::Refused) {
        tell(why, true);
        return false;
    }
    *room = true;
    return true;
}

bool Editor::moveOnMain(const QVector<int>& ids, int deltaFrames, TrackKind kind, int trackDelta,
                        const QVector<int>& cues, int cueDelta)
{
    const Timeline& cur = m_project->timeline();
    if (!magnetActive() || cur.video.isEmpty() || isTrackLocked(MainTrack::kMain)) return false;
    QVector<const Clip*> onMain, arriving; // moved V1 clips; moved video clips that land on V1 from above
    for (int id : ids) {
        TrackRef r;
        const Clip* c = TimelineOps::findClip(cur, id, &r);
        if (!c || r.kind != TrackKind::Video) continue;
        if (r.index == 0) onMain << c;
        else if (r.index + trackDelta == 0) arriving << c;
    }
    Project* p = m_project;
    const IdGen newId = [p] { return p->newClipId(); };
    if (!onMain.isEmpty() && trackDelta == 0) {
        // Along V1: a reorder. The moved clips take their new place in the row (a clip goes before one it was dropped
        // on the first half of: its middle decides), the others make room - never an overwrite that cuts a neighbour.
        // The sound linked to them goes along with the pack; other moved items (a title on V2, subtitles) move by the
        // offset the first moved V1 clip really got, so they stay with it.
        QVector<QPair<double, int>> places;
        for (const Clip& c : cur.video[0].clips) {
            const bool moved = ids.contains(c.id);
            places << qMakePair(moved ? c.start + deltaFrames + c.length() / 2.0 : c.start + c.length() / 2.0 + 0.1, c.id);
        }
        std::stable_sort(places.begin(), places.end());
        QVector<int> order;
        for (const auto& [at, id] : places) order << id;
        QSet<int> links;
        for (const Clip* c : onMain)
            if (c->linkId) links.insert(c->linkId);
        QVector<int> others;
        for (int id : ids) {
            TrackRef r;
            const Clip* c = TimelineOps::findClip(cur, id, &r);
            if (c && !(r == MainTrack::kMain) && !(c->linkId && links.contains(c->linkId))) others << id;
        }
        QVector<int> sorted; // V1 now, by start
        for (const Clip& c : cur.video[0].clips) sorted << c.id;
        const bool reordered = order != sorted;
        QVector<int> mainMoved; // the moved V1 clips and their sound (their dissolves go when the order changes)
        const Clip* anchor = nullptr;
        for (const Clip* c : onMain) {
            if (!anchor || c->start < anchor->start) anchor = c;
            mainMoved << c->id;
        }
        for (int id : ids)
            if (TrackRef r; const Clip* c = TimelineOps::findClip(cur, id, &r))
                if (!(r == MainTrack::kMain) && c->linkId && links.contains(c->linkId)) mainMoved << id;
        int earliest = INT_MAX; // the others and the subtitles: not before frame 0
        for (int id : others)
            if (const Clip* c = TimelineOps::findClip(cur, id)) earliest = std::min(earliest, c->start);
        for (int id : cues)
            if (const SubtitleCue* c = Subtitles::find(cur, id)) earliest = std::min(earliest, c->start);
        const int anchorId = anchor->id, anchorStart = anchor->start;
        const auto apply = [&](Timeline& tl, QString* why) {
            if (reordered) TimelineOps::detachTransitions(tl, mainMoved);
            const MainTrack::Result r = MainTrack::pack(tl, MainTrack::kMain, order, why);
            if (r == MainTrack::Result::Refused) return r;
            const Clip* moved = TimelineOps::findClip(std::as_const(tl), anchorId);
            int offset = moved ? moved->start - anchorStart : 0;
            if (earliest != INT_MAX) offset = std::max(offset, -earliest);
            if (offset != 0 && !others.isEmpty()) {
                TimelineOps::detachTransitions(tl, others);
                TimelineOps::moveClips(tl, others, offset, kind, 0, newId);
            }
            if (offset != 0 && cueDelta) Subtitles::move(tl, cues, offset, 0, newId);
            return r;
        };
        Timeline probe = cur;
        QString why;
        if (apply(probe, &why) == MainTrack::Result::Refused) {
            tell(why, true);
            return true;
        }
        p->edit(T("Clips verschieben"), [&](Timeline& tl) { apply(tl, nullptr); });
        return true;
    }
    if (arriving.isEmpty()) return false;
    // Onto V1 from another track: inserted at the nearest cut, the later V1 clips (with their sound) move right
    int first = INT_MAX, last = 0;
    for (const Clip* c : arriving) {
        first = std::min(first, c->start);
        last = std::max(last, c->end());
    }
    // Not before frame 0 with one delta for everything (as moveClips): the insert point is far enough right for that,
    // so the clips land exactly in the room made for them
    int earliest = INT_MAX;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(cur, id)) earliest = std::min(earliest, c->start);
    int at = first + deltaFrames;
    bool room = false;
    if (!mainInsert(at, last - first, &room, first - earliest)) return true;
    const int delta = at - first;
    p->edit(T("Clips verschieben"), [&](Timeline& tl) {
        if (room) MainTrack::openGap(tl, MainTrack::kMain, at, last - first);
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::moveClips(tl, ids, delta, kind, trackDelta, newId);
        if (cueDelta) Subtitles::move(tl, cues, delta, 0, newId);
    });
    return true;
}

std::optional<Timeline> Editor::mainTrimRoom(int clipId, TimelineOps::Edge edge, int delta, int* at, int* room) const
{
    const Timeline& cur = m_project->timeline();
    if (!magnetActive() || isTrackLocked(MainTrack::kMain)) return std::nullopt;
    TrackRef r;
    const Clip* c = TimelineOps::findClip(cur, clipId, &r);
    const bool longer = edge == TimelineOps::Edge::End ? delta > 0 : delta < 0;
    if (!c || !(r == MainTrack::kMain) || !longer) return std::nullopt;
    // The end: room after the clip. The start: the clip itself moves right, its head grows back into the room
    const int frame = edge == TimelineOps::Edge::End ? c->end() : c->start;
    Timeline tl = cur;
    if (MainTrack::openGap(tl, MainTrack::kMain, frame, std::abs(delta)) != MainTrack::Result::Moved) return std::nullopt;
    if (at) *at = frame;
    if (room) *room = std::abs(delta);
    return tl;
}
