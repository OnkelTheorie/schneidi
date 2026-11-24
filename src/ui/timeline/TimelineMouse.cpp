// SPDX-License-Identifier: AGPL-3.0-only
// Timeline: mouse and wheel input (select, drag, trim, rubber band, auto scroll).
#include "ui/timeline/TimelineView.h"
#include "ui/timeline/ZoomAnchor.h"
#include "ui/timeline/TimelineOptions.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/I18n.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/MediaCache.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/touch/Touch.h"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QUndoStack>
#include <QUrl>
#include <QContextMenuEvent>
#include <QHash>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <array>
#include <tuple>
#include <cmath>
#include "ui/timeline/TimelineViewDetail.h"

using namespace TimelineViewDetail;


namespace {
// A macro that ended up empty (nothing changed: a trim against its limit, a move onto itself) is no undo step and
// does not mark the project modified
void endMacroOrDrop(QUndoStack* undo)
{
    undo->endMacro();
    const int i = undo->index() - 1;
    if (i < 0) return;
    if (const QUndoCommand* c = undo->command(i); c && c->childCount() == 0 && c->id() == -1) {
        const_cast<QUndoCommand*>(c)->setObsolete(true);
        undo->undo(); // (an obsolete command is deleted instead of undone)
    }
}
} // namespace

void TimelineView::startRubber(Qt::KeyboardModifiers mods)
{
    Selection* sel = m_editor->selection();
    const bool add = mods & (Qt::ControlModifier | Qt::ShiftModifier);
    if (!add) sel->clear();
    m_rubberBase = add ? sel->ids() : QSet<int>{};
    m_rubberPos = m_pressPos;
    m_drag = Drag::Rubber;
}

void TimelineView::updateRubber(const QPoint& pos)
{
    if (pos == m_rubberPos) return;
    m_rubberPos = pos;
    const QRect r = QRect(m_pressPos, pos).normalized();
    if (r.width() < kDragStartPx && r.height() < kDragStartPx) return; // click without dragging: just deselect
    // touched frames [f0, f1) and rows (the frame only has to touch the clips, like DaVinci)
    const double f0 = xToFrame(std::max(r.left(), kHeaderW));
    const double f1 = xToFrame(r.right() + 1);
    const Timeline& tl = m_editor->project()->timeline();
    QVector<int> hit;
    if (r.right() >= kHeaderW) {
        for (const Row& row : rows()) {
            if (isLocked(row) || row.y + row.h <= r.top() || row.y > r.bottom()) continue;
            for (const Clip& c : tl.track(row.ref).clips)
                if (c.start < f1 && c.end() > f0) hit << c.id;
        }
        hit = m_editor->withLinked(hit);
        for (const SubRow& row : subRows()) {
            if (m_editor->isSubtitleTrackLocked(row.index) || row.y + row.h <= r.top() || row.y > r.bottom()) continue;
            for (const SubtitleCue& c : tl.subtitles[row.index].cues)
                if (c.start < f1 && c.end > f0) hit << c.id;
        }
    }
    QSet<int> ids = m_rubberBase;
    for (int id : hit) ids.insert(id);
    m_editor->selection()->set(ids);
    update();
}

void TimelineView::updateAutoScroll(const QPoint& pos)
{
    // Only drags where scrolling along makes sense (not volume, fades, curves …)
    const bool horizontal = m_drag == Drag::Rubber || m_drag == Drag::Move || m_drag == Drag::Trim ||
                            m_drag == Drag::TrimEdit || m_drag == Drag::CueMove || m_drag == Drag::CueTrim ||
                            m_drag == Drag::Marks;
    // The In/Out range lives in the ruler: only sideways (the ruler is always in the vertical edge zone)
    const bool vertical = (horizontal && m_drag != Drag::Marks) || m_drag == Drag::TrackMove;
    if (m_drag == Drag::Rubber && (pos - m_pressPos).manhattanLength() < kDragStartPx) return;
    const bool atX = horizontal && (pos.x() < kHeaderW + kAutoScrollZone || pos.x() >= width() - kAutoScrollZone);
    const bool atY = vertical && (pos.y() < kRulerH + kAutoScrollZone || pos.y() >= height() - kAutoScrollZone);
    if (atX || atY) {
        if (!m_autoScroll->isActive()) m_autoScroll->start();
    } else {
        m_autoScroll->stop();
    }
}

void TimelineView::autoScrollStep()
{
    if (m_drag == Drag::None) {
        m_autoScroll->stop();
        return;
    }
    const QPoint pos = m_dragPos;
    // Speed grows the further the mouse is beyond the edge (px per step)
    auto speed = [](int depth) { return std::clamp(2 + depth / 2, 2, 60); };
    const bool horizontal = m_drag != Drag::TrackMove;
    int dx = 0, dy = 0;
    if (horizontal) {
        if (pos.x() < kHeaderW + kAutoScrollZone) dx = -speed(kHeaderW + kAutoScrollZone - pos.x());
        else if (pos.x() >= width() - kAutoScrollZone) dx = speed(pos.x() - (width() - kAutoScrollZone));
    }
    if (m_drag == Drag::Marks) {} // sideways only
    else if (pos.y() < kRulerH + kAutoScrollZone) dy = -speed(kRulerH + kAutoScrollZone - pos.y());
    else if (pos.y() >= height() - kAutoScrollZone) dy = speed(pos.y() - (height() - kAutoScrollZone));

    // Scroll whole pixels and shift the press point by the same pixels: all drag computations
    // (distance to the press point) thus stay correct in timeline coordinates
    const QPointF oldAnchor(frameToX(0), m_view.scrollY);
    if (dx) setLeftFrame(m_view.leftFrame + dx / m_view.pxPerFrame);
    if (dy) setScrollY(m_view.scrollY + dy);
    const int sx = int(std::lround(oldAnchor.x() - frameToX(0)));
    const int sy = m_view.scrollY - int(oldAnchor.y());
    if (!sx && !sy) return; // already at the start / end
    m_pressPos -= QPoint(sx, sy);
    dragMove(pos, m_dragMods);
}

TimelineView::GapPick TimelineView::gapAt(const TrackRef& ref, double frame) const
{
    GapPick g;
    const Timeline& tl = m_editor->project()->timeline();
    if (ref.index < 0 || ref.index >= tl.tracks(ref.kind).size() || frame < 0) return g;
    int from = 0, to = -1;
    for (const Clip& c : tl.track(ref).clips) {
        if (c.start <= frame && frame < c.end()) return g; // on a clip
        if (c.end() <= frame) from = std::max(from, c.end());
        else if (to < 0 || c.start < to) to = c.start;
    }
    if (to <= from) return g; // nothing after it: no gap, just the end of the track
    g.on = true;
    g.ref = ref;
    g.from = from;
    g.to = to;
    return g;
}

void TimelineView::pickGap(const GapPick& g)
{
    m_pickingGap = true;
    m_editor->selection()->clear();
    m_pickingGap = false;
    m_gap = g;
    update();
}

bool TimelineView::closeSelectedGap()
{
    if (!m_gap.on) return false;
    const GapPick g = std::exchange(m_gap, {});
    const GapPick now = gapAt(g.ref, g.from); // still the same gap? (safety net; changes drop the pick already)
    if (!now.on || now.from != g.from || now.to != g.to) {
        update();
        return false;
    }
    QString why;
    if (!m_editor->closeGap(g.ref, g.from, g.to, &why)) {
        m_gap = g; // stays picked: lock the other track and press Delete again
        if (!why.isEmpty()) emit message(why);
    }
    update();
    return true;
}

void TimelineView::mousePressEvent(QMouseEvent* e)
{
    m_touchPointer = Touch::isTouch(e);
    if (m_reorder.on) { // the held clip of the magnetic main track follows the finger (mouseMoveEvent)
        m_reorder.fingerX = e->position().x();
        return;
    }
    if (e->button() == Qt::LeftButton && m_gap.on) {
        m_gap = {};
        update();
    }
    // Right button on the ruler: dragging draws a new In/Out range, a click opens the ruler menu (on release)
    if (e->button() == Qt::RightButton) {
        marksRightPress(e->position().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    if (m_drag == Drag::Marks && m_markRight) return; // the right button's drag runs: the left one waits
    const QPoint pos = e->position().toPoint();
    m_pressPos = pos;
    if (subtitlePress(e, pos)) return;

    if (pos.x() < kHeaderW) { // track headers
        if (const auto row = rowAt(pos.y()); row && micButton(*row).contains(pos)) {
            emit armToggled(row->ref.index);
        } else if (row && headerButton(*row).contains(pos)) {
            if (row->ref.kind == TrackKind::Video) m_editor->toggleTrackHidden(row->ref);
            else m_editor->toggleTrackMute(row->ref);
        } else if (row && lockButton(*row).contains(pos)) {
            m_editor->toggleTrackLock(row->ref);
        } else if (row && shortNameRect(*row).contains(pos)) {
            if (row->ref.kind == TrackKind::Video) m_editor->setTargetTracks(row->ref.index, m_editor->targetAudioTrack());
            else m_editor->setTargetTracks(m_editor->targetVideoTrack(), row->ref.index);
        } else if (row) { // remaining track header: dragging reorders the track
            m_drag = Drag::TrackMaybeMove;
            m_trackDragRef = row->ref;
            m_trackDropTo = -1;
        }
        return;
    }
    if (pos.y() < kRulerH) {
        if (sectionPress(pos)) return; // the edge of a story section
        if (marksPress(pos, e->modifiers())) return; // In/Out handles, the range's grip
        m_drag = Drag::Scrub;
        m_scrubFrame = std::max(0, int(std::lround(xToFrame(pos.x()))));
        // The centre mode: the press jumps there, the lanes re-centre on it - a drag then slides the ruler under the
        // fixed line from that point (the absolute frame under the pointer would run away with every re-centring)
        m_scrubPressX = pos.x();
        m_scrubPressFrame = m_scrubFrame;
        emit playheadMoveStarted();
        emit seekRequested(m_scrubFrame);
        return;
    }

    const int id = clipAt(pos);
    if (m_tool == Tool::Blade) {
        if (id) {
            int frame = int(std::lround(xToFrame(pos.x())));
            frame += snapDelta({frame}, {});
            m_editor->bladeAt(id, frame);
        }
        return;
    }

    Selection* sel = m_editor->selection();
    if (curvePress(e, pos)) return;
    // Keyframes: icon opens/closes the lane, diamonds select (Ctrl = add) and drag
    if (const int kid = keyIconAt(pos)) {
        if (m_keyLanes.contains(kid)) m_keyLanes.remove(kid);
        else m_keyLanes.insert(kid);
        if (sel->keyClip() == kid) sel->setKeyframes(0, {});
        setScrollY(m_view.scrollY); // height has changed (adjust scrollbar, redraws)
        emit trackHeightsChanged();
        return;
    }
    if (const auto k = keyframeAt(pos)) {
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), k->clipId);
        if (!sel->contains(k->clipId)) {
            const QVector<int> group = m_editor->withLinked({k->clipId});
            sel->set(QSet<int>(group.begin(), group.end()));
        }
        QSet<int> times = sel->keyClip() == k->clipId && sel->keyParam() < 0 ? sel->keyTimes() : QSet<int>{};
        const bool ctrl = e->modifiers() & Qt::ControlModifier;
        if (ctrl && times.contains(k->t)) times.remove(k->t);
        else if (ctrl) times.insert(k->t);
        else if (!times.contains(k->t)) times = {k->t};
        sel->setKeyframes(k->clipId, times);
        if (!ctrl && c) {
            emit seekRequested(c->start + k->t); // Inspector shows the values at this keyframe
            m_keyDragClip = k->clipId;
            m_keyDelta = 0;
            m_drag = Drag::Keyframe;
        }
        update();
        return;
    }
    // Retime controls: drag speed point, click on the speed opens the selection (like DaVinci ▾)
    if (m_tool == Tool::Select)
        if (const auto h = retimeHitAt(pos); h && (h->point >= 0 || !edgeAt(pos))) {
            if (!sel->contains(h->clipId)) {
                const QVector<int> group = m_editor->withLinked({h->clipId});
                sel->set(QSet<int>(group.begin(), group.end()));
            }
            if (h->point >= 0) {
                m_rampDrag = *h;
                m_rampDelta = 0;
                m_drag = Drag::SpeedPoint;
                update();
            } else {
                segmentMenu(*h, int(std::lround(xToFrame(pos.x()))), e->globalPosition().toPoint());
            }
            return;
        }
    if (inLane(pos)) { // empty spot in the keyframe lane: deselect diamonds
        sel->setKeyframes(0, {});
        update();
        return;
    }
    if (trimTool()) {
        const auto hit = trimHitAt(pos);
        if (!hit) {
            if (!(e->modifiers() & Qt::ControlModifier)) sel->clear();
            return;
        }
        // Alt at a cut: only this track's edge rolls (J/L cut), like Alt-clicking one part of a linked clip
        const bool split = (e->modifiers() & Qt::AltModifier) && hit->kind == TimelineOps::TrimKind::Roll;
        const QVector<int> group = split ? m_editor->editable({hit->clipId}) : m_editor->withLinked({hit->clipId});
        if (!sel->contains(hit->clipId) || split) sel->set(QSet<int>(group.begin(), group.end()));
        m_trimHit = *hit;
        m_trimEdit = split ? m_editor->splitEdit(hit->clipId, hit->edge) : m_editor->trimEdit(hit->kind, hit->clipId, hit->edge);
        m_trimEditDelta = 0;
        m_trimEditWanted = 0;
        m_trimEditWhy.clear();
        m_trimPreview = m_editor->project()->timeline();
        m_drag = Drag::TrimEdit;
        updateTrimFrames(*m_trimPreview, m_trimEdit, 0);
        update();
        return;
    }
    if (const auto f = fadeHandleAt(pos)) {
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), f->clipId);
        m_fade = *f;
        m_fadeStart = f->edge == TimelineOps::Edge::Start ? c->fadeIn : c->fadeOut;
        m_editor->project()->closeMerge(); // whole drag = one undo step
        m_drag = Drag::Fade;
        return;
    }
    if (const auto t = transitionAt(pos)) {
        sel->setTransition({t->span.leftId, t->span.rightId});
        if (t->edge) {
            m_transSpan = t->span;
            m_transEdge = t->edge;
            m_editor->project()->closeMerge(); // whole drag = one undo step
            m_drag = Drag::TransitionLength;
        }
        return;
    }
    // A node's time range bar (moves, its ends trim it); clip edges, fade handles and transitions above win
    if (!edgeAt(pos) && nodeBarPress(pos)) return;
    if (const auto hit = edgeAt(pos)) {
        // Alt-drag at a cut (a clip touches it on the other side): only this track's edge rolls, the linked partners
        // keep their cut and stay linked (J/L cut: the sound starts early or runs on under the next picture)
        if (e->modifiers() & Qt::AltModifier) {
            const TimelineOps::TrimEdit split = m_editor->splitEdit(hit->clipId, hit->edge);
            if (!split.ids.isEmpty() && !split.rightIds.isEmpty()) {
                const QVector<int> one = m_editor->editable({hit->clipId});
                sel->set(QSet<int>(one.begin(), one.end()));
                m_trimHit = TrimHit{TimelineOps::TrimKind::Roll, hit->clipId, hit->edge};
                m_trimEdit = split;
                m_trimEditDelta = 0;
                m_trimEditWanted = 0;
                m_trimEditWhy.clear();
                m_trimPreview = m_editor->project()->timeline();
                m_drag = Drag::TrimEdit;
                updateTrimFrames(*m_trimPreview, m_trimEdit, 0);
                update();
                return;
            }
        }
        const QVector<int> group = m_editor->withLinked({hit->clipId});
        if (!sel->ids().contains(hit->clipId)) sel->set(QSet<int>(group.begin(), group.end()));
        m_trim = *hit;
        m_trimIds = m_editor->withCopies(group); // as Editor::trimClip trims
        m_trimDelta = 0;
        m_drag = Drag::Trim;
        updateTrimFrames(m_editor->project()->timeline(), {TimelineOps::TrimKind::Ripple, m_trimIds, {}, m_trim.edge}, 0);
        update();
        return;
    }
    if (const int vid = volumeLineAt(pos)) {
        const QVector<int> group = m_editor->withLinked({vid});
        if (!sel->ids().contains(vid)) sel->set(QSet<int>(group.begin(), group.end()));
        m_volClipId = vid;
        const Clip* vc = TimelineOps::findClip(m_editor->project()->timeline(), vid);
        m_volStartDb = m_volDb = volumeAt(*vc, (pos.x() - frameToX(vc->start)) / m_view.pxPerFrame); // with keyframes
        m_volFine = e->modifiers() & Qt::ShiftModifier;
        m_drag = Drag::Volume;
        update();
        return;
    }
    if (!id) {
        startRubber(e->modifiers());
        return;
    }
    // Alt-click: only this part of a linked clip (like DaVinci), e.g. move the audio alone
    const bool alone = e->modifiers() & Qt::AltModifier;
    const QVector<int> group = alone ? m_editor->editable({id}) : m_editor->withLinked({id});
    QSet<int> ids = sel->ids();
    if (e->modifiers() & Qt::ControlModifier) {
        const bool remove = ids.contains(id);
        for (int g : group) {
            if (remove) ids.remove(g);
            else ids.insert(g);
        }
        sel->set(ids);
        return;
    }
    if (!ids.contains(id) || alone) sel->set(QSet<int>(group.begin(), group.end()));

    TimelineOps::findClip(m_editor->project()->timeline(), id, &m_anchorRef);
    m_dragIds = m_editor->withCopies(m_editor->editable(sel->ids().values().toVector())); // kill feed copies follow
    m_cueIds = m_editor->selectedSubtitles(); // selected subtitles move along in time (also among m_dragIds)
    m_cueDelta = 0;
    m_cueTrackDelta = 0;
    m_dragDelta = 0;
    m_dragFine = 0;
    m_dragAnchorId = id;
    m_dragTrackDelta = 0;
    m_drag = Drag::MaybeMove;
}

void TimelineView::mouseMoveEvent(QMouseEvent* e)
{
    if (m_reorder.on) {
        m_reorder.fingerX = e->position().x();
        reorderFollowFinger();
        if (reorderEdgeSpeed() != 0) {
            if (!m_reorderScroll) {
                m_reorderScroll = new QTimer(this);
                m_reorderScroll->setInterval(16);
                connect(m_reorderScroll, &QTimer::timeout, this, [this] {
                    const double speed = m_reorder.on ? reorderEdgeSpeed() : 0;
                    if (speed == 0) {
                        m_reorderScroll->stop();
                        return;
                    }
                    const int n = int(m_reorder.order.size());
                    const double row = n * (m_reorder.tw + 4.0) - 4;
                    const double most = kHeaderW + 4.0, least = width() - 4.0 - row; // the row's first tile / last tile in sight
                    double base = m_reorder.base + speed;
                    base = speed > 0 ? std::min(base, std::max(m_reorder.base, most)) : std::max(base, std::min(m_reorder.base, least));
                    m_reorder.base = base;
                    reorderFollowFinger();
                });
            }
            if (!m_reorderScroll->isActive()) m_reorderScroll->start();
        }
        return;
    }
    if (m_drag == Drag::None) m_touchPointer = Touch::isTouch(e); // a running drag keeps the press's zones
    const QPoint pos = e->position().toPoint();
    m_dragPos = pos;
    m_dragMods = e->modifiers();
    if (m_drag != Drag::None) updateAutoScroll(pos);
    dragMove(pos, e->modifiers());
}

void TimelineView::dragMove(const QPoint& pos, Qt::KeyboardModifiers mods)
{
    switch (m_drag) {
    case Drag::Rubber:
        updateRubber(pos);
        return;
    case Drag::SpeedPoint: {
        const int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        if (delta != m_rampDelta) {
            m_rampDelta = delta;
            update();
        }
        return;
    }
    case Drag::SectionEdge:
        sectionDrag(pos);
        return;
    case Drag::Marks:
        marksDrag(pos, mods);
        return;
    case Drag::Scrub: {
        // Only jump on a new frame: every jump discards the buffer during playback and decodes anew –
        // when clicking (mouse jitters by a pixel) that stuttered
        const int frame = m_center ? std::max(0, int(std::lround(m_scrubPressFrame - (pos.x() - m_scrubPressX)
                                                                                          / m_view.pxPerFrame)))
                                   : std::max(0, int(std::lround(xToFrame(pos.x()))));
        if (frame != m_scrubFrame) {
            m_scrubFrame = frame;
            emit seekRequested(frame);
        }
        return;
    }
    case Drag::TrackMaybeMove:
        if (std::abs(pos.y() - m_pressPos.y()) < kDragStartPx) return;
        m_drag = Drag::TrackMove;
        setCursor(Qt::ClosedHandCursor);
        [[fallthrough]];
    case Drag::TrackMove: {
        const int to = trackDropIndex(pos.y());
        if (to != m_trackDropTo) {
            m_trackDropTo = to;
            update();
        }
        return;
    }
    case Drag::MaybeMove:
        if ((pos - m_pressPos).manhattanLength() < kDragStartPx) return;
        m_drag = Drag::Move;
        [[fallthrough]];
    case Drag::Move: {
        const Timeline& tl = m_editor->project()->timeline();
        const double exact = (pos.x() - m_pressPos.x()) / m_view.pxPerFrame;
        int delta = int(std::lround(exact));
        QVector<int> edges;
        int minStart = INT_MAX;
        // Sub-frame position (like DaVinci Fairlight): only pure audio clips with the same sub-frame position and only
        // when zoomed in far enough for a frame to be wide enough; otherwise whole frames
        bool fine = m_view.pxPerFrame >= kFinePxPerFrame;
        int sub = -1;
        for (int id : m_dragIds) {
            TrackRef ref;
            if (const Clip* c = TimelineOps::findClip(tl, id, &ref)) {
                edges << c->start + delta << c->end() + delta;
                minStart = std::min(minStart, c->start);
                if (ref.kind != TrackKind::Audio || (sub >= 0 && sub != c->subframe)) fine = false;
                sub = c->subframe;
            } else if (const SubtitleCue* q = Subtitles::find(tl, id); q && m_cueIds.contains(id)) {
                edges << q->start + delta << q->end + delta;
                minStart = std::min(minStart, q->start);
                fine = false; // subtitles move in whole frames
            }
        }
        const QSet<int> exclude(m_dragIds.begin(), m_dragIds.end());
        bool snapped = false;
        delta += snapDelta(edges, exclude, &snapped);
        if (minStart != INT_MAX) delta = std::max(delta, -minStart);
        m_dragDelta = delta;
        m_cueDelta = delta;
        m_dragFine = delta * 100;
        if (fine && sub >= 0) {
            // snapped: exactly onto the frame (sub-frame position gone), otherwise free in 1/100 frame
            m_dragFine = snapped ? delta * 100 - sub : int(std::lround(exact * 100));
            m_dragFine = std::max(m_dragFine, -(minStart * 100 + sub));
        }

        m_dragTrackDelta = 0;
        if (const auto row = rowAt(pos.y()); row && row->ref.kind == m_anchorRef.kind)
            m_dragTrackDelta = m_editor->clampTrackDelta(m_dragIds, m_anchorRef.kind, row->ref.index - m_anchorRef.index);
        for (TrackRef ref : TimelineOps::tracksOf(tl, m_dragIds)) // never onto a locked track
            if (m_editor->isTrackLocked({ref.kind, ref.index + m_dragTrackDelta})) m_dragTrackDelta = 0;
        update();
        return;
    }
    case Drag::CueMaybeMove:
        if ((pos - m_pressPos).manhattanLength() < kDragStartPx) return;
        m_drag = Drag::CueMove;
        [[fallthrough]];
    case Drag::CueMove: {
        const Timeline& tl = m_editor->project()->timeline();
        int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        QVector<int> edges;
        int minStart = INT_MAX;
        for (int id : m_cueIds)
            if (const SubtitleCue* c = Subtitles::find(tl, id)) {
                edges << c->start + delta << c->end + delta;
                minStart = std::min(minStart, c->start);
            }
        for (int id : m_dragIds) // selected clips move along in time
            if (const Clip* c = TimelineOps::findClip(tl, id)) {
                edges << c->start + delta << c->end() + delta;
                minStart = std::min(minStart, c->start);
            }
        QSet<int> exclude(m_cueIds.begin(), m_cueIds.end());
        for (int id : m_dragIds) exclude.insert(id);
        delta += snapDelta(edges, exclude);
        if (minStart != INT_MAX) delta = std::max(delta, -minStart);
        m_cueDelta = delta;
        m_dragFine = delta * 100;
        m_cueTrackDelta = 0;
        if (const auto row = subRowAt(pos.y())) {
            m_cueTrackDelta = m_editor->clampSubtitleTrackDelta(m_cueIds, row->index - m_cueAnchor);
            for (int id : m_cueIds) { // never onto a locked track
                int t = -1;
                if (Subtitles::find(tl, id, &t) && m_editor->isSubtitleTrackLocked(t + m_cueTrackDelta)) m_cueTrackDelta = 0;
            }
        }
        update();
        return;
    }
    case Drag::CueTrim: {
        const SubtitleCue* c = Subtitles::find(m_editor->project()->timeline(), m_cueTrim.clipId);
        if (!c) return;
        int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        const int edgeFrame = m_cueTrim.edge == TimelineOps::Edge::Start ? c->start : c->end;
        delta += snapDelta({edgeFrame + delta}, {c->id});
        m_cueDelta = m_editor->clampSubtitleTrim(c->id, m_cueTrim.edge, delta);
        update();
        return;
    }
    case Drag::Trim: {
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_trim.clipId);
        if (!c) return;
        int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        const int edgeFrame = m_trim.edge == TimelineOps::Edge::Start ? c->start : c->end();
        const QSet<int> exclude(m_trimIds.begin(), m_trimIds.end());
        delta += snapDelta({edgeFrame + delta}, exclude);
        // the edge runs over a neighbor (overwrite); its edge only snaps
        const int clamped = m_editor->clampTrim(m_trim.clipId, m_trim.edge, delta, true);
        if (clamped != m_trimDelta || !m_trimPreview) { // timeline and trim view: as after release (no ripple)
            m_trimPreview = m_editor->previewTrim(m_trim.clipId, m_trim.edge, clamped, true);
            updateTrimFrames(*m_trimPreview, {TimelineOps::TrimKind::Ripple, m_trimIds, {}, m_trim.edge}, clamped);
        }
        m_trimDelta = clamped;
        update();
        return;
    }
    case Drag::TrimEdit: {
        using TimelineOps::TrimKind;
        const Timeline& tl = m_editor->project()->timeline();
        const Clip* c = TimelineOps::findClip(tl, m_trimHit.clipId);
        if (!c) return;
        int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        // Snapping to the edges that visibly move (slip moves no edge)
        QSet<int> exclude(m_trimEdit.ids.begin(), m_trimEdit.ids.end());
        for (int id : m_trimEdit.rightIds) exclude.insert(id);
        QVector<int> edges;
        const bool start = m_trimHit.edge == TimelineOps::Edge::Start;
        switch (m_trimHit.kind) {
        case TrimKind::Ripple:
            if (!start) {
                edges << c->end() + delta;
                for (int id : m_trimEdit.ids) // what moves up (all unlocked tracks) is not a fixed point
                    if (const Clip* t = TimelineOps::findClip(tl, id))
                        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
                            for (const Track& tr : tl.tracks(k))
                                if (!tr.locked)
                                    for (const Clip& o : tr.clips)
                                        if (o.start >= t->end()) exclude.insert(o.id);
            }
            break;
        case TrimKind::Roll: edges << (start ? c->start : c->end()) + delta; break;
        case TrimKind::Slide: edges << c->start + delta << c->end() + delta; break;
        case TrimKind::Slip: break;
        }
        if (!edges.isEmpty()) delta += snapDelta(edges, exclude);
        delta = m_editor->clampTrimEdit(m_trimEdit, delta);
        if (delta == m_trimEditWanted && m_trimPreview) return; // nothing new (one preview per change, not per pixel)
        m_trimEditWanted = delta;
        // A ripple that would take linked picture and sound apart: the preview stops at the last allowed delta (release
        // applies that) and says why - once per reason, cleared back in the allowed range
        QString why;
        Timeline preview;
        delta = m_editor->allowedTrimEdit(m_trimEdit, delta, &why, &preview);
        if (why != m_trimEditWhy) {
            m_trimEditWhy = why;
            emit message(why);
        }
        if (delta != m_trimEditDelta || !m_trimPreview) {
            m_trimEditDelta = delta;
            m_trimPreview = std::move(preview);
            updateTrimFrames(*m_trimPreview, m_trimEdit, delta);
            update();
        }
        return;
    }
    case Drag::NodeRange:
        nodeBarDrag(pos);
        return;
    case Drag::CurvePoint:
    case Drag::CurveHandle:
        curveMove(pos, mods);
        return;
    case Drag::Keyframe: {
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_keyDragClip);
        const QSet<int>& times = m_editor->selection()->keyTimes();
        if (!c || times.isEmpty()) return;
        int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        { // Snapping like DaVinci: playhead, clip boundaries, keyframes of the other times
            QVector<int> moved;
            for (int t : times) moved << t + delta;
            delta += keySnapDelta(*c, moved, times);
        }
        // all selected diamonds stay in the clip
        const int lo = *std::min_element(times.begin(), times.end());
        const int hi = *std::max_element(times.begin(), times.end());
        delta = std::clamp(delta, -lo, c->length() - 1 - hi);
        if (delta != m_keyDelta) {
            m_keyDelta = delta;
            update();
        }
        return;
    }
    case Drag::Fade: {
        const int dx = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        const int frames = m_fadeStart + (m_fade.edge == TimelineOps::Edge::Start ? dx : -dx);
        m_editor->setClipFade(m_fade.clipId, m_fade.edge, std::max(0, frames), QStringLiteral("clip-fade"));
        update();
        return;
    }
    case Drag::TransitionLength: {
        const int dx = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        const int grow = m_transEdge > 0 ? dx : -dx;
        // Centered crossfade grows on both sides, fade-in/out only on the free edge
        const int len = m_transSpan.length() + (m_transSpan.isDissolve() ? 2 * grow : grow);
        m_editor->setTransitionLength(m_transSpan.leftId, m_transSpan.rightId, std::max(1, len),
                                      QStringLiteral("transition-length"));
        return;
    }
    case Drag::Volume: {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_volClipId, &ref);
        const auto row = c ? rowFor(ref) : std::nullopt;
        if (!row) return;
        // Shift toggled -> continue from the current position so that nothing jumps
        const bool fine = mods & Qt::ShiftModifier;
        if (fine != m_volFine) {
            m_volFine = fine;
            m_volStartDb = m_volDb;
            m_pressPos = pos;
        }
        const int dy = pos.y() - m_pressPos.y();
        double db;
        if (fine) {
            db = m_volStartDb - dy * 0.1;
        } else {
            const QRect body = clipBodyRect(clipRect(*row, *c));
            db = posToVolume(volumeToPos(m_volStartDb) - dy / double(std::max(1, body.height() - 1)));
            if (std::abs(db) < 0.5) db = 0; // 0 dB snaps in
        }
        db = std::clamp(std::round(db * 10) / 10, kMinVolumeDb, kMaxVolumeDb);
        if (db != m_volDb) {
            m_volDb = db;
            update();
        }
        return;
    }
    case Drag::None:
        break;
    }
    updateHoverCursor(pos);

    if (m_tool == Tool::Blade) {
        const int old = m_hoverFrame;
        if (pos.x() >= kHeaderW && pos.y() >= kRulerH) {
            int frame = int(std::lround(xToFrame(pos.x())));
            m_hoverFrame = frame + snapDelta({frame}, {});
        } else {
            m_hoverFrame = -1;
        }
        if (old != m_hoverFrame) update();
    }
}

void TimelineView::clickSeek()
{
    if (!TimelineOptions::clickSetsPlayhead()) return;
    // Framed like a click in the ruler: playback after the move follows the same rule (PlayheadMove, MainWindow)
    emit playheadMoveStarted();
    emit seekRequested(std::max(0, int(std::lround(xToFrame(m_pressPos.x())))));
    emit playheadMoveFinished();
}

void TimelineView::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_reorder.on) {
        finishReorder(true);
        return;
    }
    // The right button's drag (a new In/Out range) ends with that button only, the left one's never with it
    if ((m_drag == Drag::Marks && m_markRight) != (e->button() == Qt::RightButton)) return;
    if (e->button() != Qt::LeftButton && e->button() != Qt::RightButton) return;
    m_autoScroll->stop();
    if (m_drag == Drag::Scrub) { // the playhead let go: playback after the move (PlayheadMove, MainWindow)
        m_drag = Drag::None;
        emit playheadMoveFinished();
        updateHoverCursor(e->position().toPoint());
        update();
        return;
    }
    if (m_drag == Drag::SectionEdge) {
        m_drag = Drag::None;
        m_sectionDrag = -1;
        updateHoverCursor(e->position().toPoint());
        return;
    }
    if (m_drag == Drag::NodeRange) {
        m_drag = Drag::None;
        nodeBarRelease();
        updateHoverCursor(e->position().toPoint());
        update();
        return;
    }
    if (m_drag == Drag::Marks) {
        m_drag = Drag::None;
        marksRelease();
        updateHoverCursor(e->position().toPoint());
        return;
    }
    if (m_drag == Drag::Rubber) {
        m_drag = Drag::None;
        m_rubberBase.clear();
        // A click (no frame drawn) into the empty part of a track between clips: that gap is picked
        const QPoint at = e->position().toPoint();
        const bool click = QRect(m_pressPos, at).normalized().width() < kDragStartPx
                           && std::abs(at.y() - m_pressPos.y()) < kDragStartPx && m_pressPos.x() >= kHeaderW;
        if (click)
            if (const auto row = rowAt(m_pressPos.y()); row && !isLocked(*row))
                if (const GapPick g = gapAt(row->ref, xToFrame(m_pressPos.x())); g.on) pickGap(g);
        if (click && e->button() == Qt::LeftButton) clickSeek();
        updateHoverCursor(e->position().toPoint());
        update();
        return;
    }
    // A click on a clip (pressed, not moved): selected on the press; by choice the playhead goes there too
    if (m_drag == Drag::MaybeMove && e->button() == Qt::LeftButton) clickSeek();
    if (m_drag == Drag::TrackMove || m_drag == Drag::TrackMaybeMove) {
        const bool moved = m_drag == Drag::TrackMove;
        const int to = m_trackDropTo;
        m_drag = Drag::None;
        m_trackDropTo = -1;
        if (moved && to >= 0 && to != m_trackDragRef.index) m_editor->moveTrack(m_trackDragRef.kind, m_trackDragRef.index, to);
        updateHoverCursor(e->position().toPoint());
        update();
        // Compact headers (phone): a tap on the header is its menu (lock, name, mic, color, tracks live there)
        if (!moved && compactHeaders())
            if (const auto row = rowFor(m_trackDragRef)) headerMenu(*row, mapToGlobal(e->position().toPoint()));
        return;
    }
    if (m_drag == Drag::Move && (m_dragFine != 0 || m_dragTrackDelta != 0)) {
        const Timeline& before = m_editor->project()->timeline();
        const auto onMain = [&](const Timeline& tl) {
            for (int id : m_dragIds) {
                TrackRef r;
                if (TimelineOps::findClip(tl, id, &r) && r.kind == TrackKind::Video && r.index == 0) return true;
            }
            return false;
        };
        const bool magnetic = m_magnetic && !before.video.isEmpty();
        const bool fromMain = magnetic && onMain(before);
        QUndoStack* undo = m_editor->project()->undoStack();
        // (only V1 clips and their linked sound in the drag)
        const auto mainOnly = [&] {
            QSet<int> links;
            for (const Clip& c : before.video[0].clips)
                if (m_dragIds.contains(c.id) && c.linkId) links.insert(c.linkId);
            for (int id : m_dragIds) {
                TrackRef r;
                const Clip* c = TimelineOps::findClip(before, id, &r);
                if (!c || (!(r.kind == TrackKind::Video && r.index == 0) && !(c->linkId && links.contains(c->linkId))))
                    return false;
            }
            return true;
        };
        if (fromMain && m_anchorRef.kind == TrackKind::Video && m_dragTrackDelta == 0 && m_dragFine % 100 == 0
            && mainOnly()) {
            // Along the magnetic main track: a reorder (the clips take their new place in the row, the others make
            // room) - never an overwrite that cuts a neighbour and then closes the gap (its material was gone)
            QVector<QPair<double, int>> places; // new start (the moved ones at their drop, others where they are)
            const int frames = m_dragFine / 100;
            for (const Clip& c : before.video[0].clips) {
                const bool moved = m_dragIds.contains(c.id);
                // a moved clip goes before one it was dropped on the first half of (its middle decides)
                places << qMakePair(moved ? c.start + frames + c.length() / 2.0 : c.start + c.length() / 2.0 + 0.1, c.id);
            }
            std::stable_sort(places.begin(), places.end());
            QVector<int> order;
            for (const auto& [at, id] : places) order << id;
            m_editor->packTrack({TrackKind::Video, 0}, order, T("Clips verschieben"));
        } else {
            // The magnetic main track: a clip moved onto or off V1 leaves no gap there (one undo step). A clip moved
            // elsewhere (music on A2, a title on V2) leaves V1 alone
            undo->beginMacro(T("Clips verschieben"));
            m_editor->moveClipsFine(m_dragIds, m_dragFine, m_anchorRef.kind, m_dragTrackDelta);
            if (magnetic && (fromMain || onMain(m_editor->project()->timeline()))) m_editor->packTrack({TrackKind::Video, 0});
            endMacroOrDrop(undo);
        }
    }
    if (m_drag == Drag::CueMove || m_drag == Drag::CueTrim) {
        const Drag d = m_drag;
        m_drag = Drag::None; // before the change, so the preview does not apply twice
        if (d == Drag::CueMove) m_editor->moveSubtitles(m_cueIds + m_dragIds, m_cueDelta, m_cueTrackDelta);
        else m_editor->trimSubtitle(m_cueTrim.clipId, m_cueTrim.edge, m_cueDelta);
    }
    if (m_drag == Drag::CueMaybeMove) m_drag = Drag::None;
    m_cueIds.clear();
    m_cueDelta = 0;
    m_cueTrackDelta = 0;
    const bool trimmed = m_drag == Drag::Trim;
    const bool volume = m_drag == Drag::Volume;
    const bool keys = m_drag == Drag::Keyframe;
    if (m_drag == Drag::TransitionLength || m_drag == Drag::Fade) m_editor->project()->closeMerge();
    const bool trimEdit = m_drag == Drag::TrimEdit;
    const bool ramp = m_drag == Drag::SpeedPoint;
    if (m_drag == Drag::CurvePoint || m_drag == Drag::CurveHandle) curveRelease();
    m_drag = Drag::None; // before trimClip, so the preview is not applied twice
    if (ramp && m_rampDelta != 0) m_editor->moveSpeedPoint(m_rampDrag.clipId, m_rampDrag.point, m_rampDelta);
    m_rampDelta = 0;
    if (trimmed && m_trimDelta != 0) {
        TrackRef where;
        const bool onMain = m_magnetic && TimelineOps::findClip(m_editor->project()->timeline(), m_trim.clipId, &where)
                            && where.kind == TrackKind::Video && where.index == 0;
        QUndoStack* undo = m_editor->project()->undoStack();
        if (onMain) undo->beginMacro(T("Clip trimmen"));
        m_editor->trimClip(m_trim.clipId, m_trim.edge, m_trimDelta, true);
        if (onMain) { // magnetic main track: the gap the trim left closes
            m_editor->packTrack({TrackKind::Video, 0});
            endMacroOrDrop(undo);
        }
    }
    if (trimEdit && m_trimEditDelta != 0) m_editor->applyTrimEdit(m_trimEdit, m_trimEditDelta);
    m_trimPreview.reset();
    m_trimEditDelta = 0;
    m_trimEditWhy.clear(); // (its message stays a while: why the trim stopped there)
    if (trimmed || trimEdit) endTrimFrames(); // viewer back to the playhead frame
    if (keys && m_keyDelta != 0) { // one undo step per drag
        const QVector<int> times = m_editor->selection()->keyTimes().values().toVector();
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_keyDragClip);
        const int seek = c && !times.isEmpty() ? c->start + *std::min_element(times.begin(), times.end()) + m_keyDelta : -1;
        m_editor->moveKeyframes(m_keyDragClip, times, m_keyDelta); // replaces the timeline -> c invalid
        if (seek >= 0) emit seekRequested(seek); // playhead onto the moved diamond
    }
    m_keyDelta = 0;
    const Clip* volClip = volume ? TimelineOps::findClip(m_editor->project()->timeline(), m_volClipId) : nullptr;
    if (volClip && Keys::animated(*volClip, AnimParam::Volume)) {
        // Keyframes: shift the whole curve by the dragged amount
        const double delta = m_volDb - m_volStartDb;
        if (delta != 0)
            m_editor->modifyClips({m_volClipId}, T("Lautstärke"), [delta](Clip& c) {
                for (Keyframe& k : c.keys[AnimParam::Volume])
                    k.value = std::clamp(k.value + delta, kMinVolumeDb, kMaxVolumeDb);
            });
    } else if (volume) {
        m_editor->setClipVolume(m_volClipId, m_volDb);
    }
    m_trimIds.clear();
    m_trimDelta = 0;
    m_dragIds.clear();
    updateHoverCursor(e->position().toPoint());
    update();
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (const auto sub = e->button() == Qt::LeftButton ? subRowAt(pos.y()) : std::nullopt) {
        if (pos.x() < kHeaderW && subNameRect(*sub).adjusted(-32, 0, 0, 0).contains(pos)) startSubtitleRename(sub->index);
        else if (const int id = cueAt(pos)) emit subtitleEditRequested(id);
        return;
    }
    if (e->button() == Qt::LeftButton && pos.x() >= kHeaderW && curveDoubleClick(pos)) return;
    if (e->button() == Qt::LeftButton && nodeBarDoubleClick(pos)) return; // the node on the Comp page
    // Double-click on the track name: rename (like DaVinci)
    if (e->button() == Qt::LeftButton && pos.x() < kHeaderW && pos.y() >= kRulerH) {
        if (const auto row = rowAt(pos.y()); row && nameRect(*row).adjusted(-32, 0, 0, 0).contains(pos)) {
            startRename(row->ref);
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TimelineView::leaveEvent(QEvent*)
{
    if (m_markHover != MarkGrab::None && m_drag == Drag::None) {
        m_markHover = MarkGrab::None;
        update(0, 0, width(), kRulerH);
    }
    if (m_hoverClip && m_drag == Drag::None) {
        m_hoverClip = 0;
        update();
    }
    if (m_hoverVolClip && m_drag == Drag::None) {
        m_hoverVolClip = 0;
        update();
    }
    if (m_hoverFrame >= 0) {
        m_hoverFrame = -1;
        update();
    }
}

void TimelineView::wheelEvent(QWheelEvent* e)
{
    // The binding of the held modifiers; a sideways swipe pans sideways (InputBindings::wheelMove, like the Studio)
    const InputBindings::WheelMove move = InputBindings::instance().wheelMove(e->angleDelta(), e->modifiers());
    const double steps = move.steps;
    if (steps == 0) return;
    const double mouseX = e->position().x();

    switch (move.action) {
    case WheelAction::ScrollVertical:
        setScrollY(m_view.scrollY - int(steps * 40));
        break;
    case WheelAction::ScrollHorizontal:
        setLeftFrame(m_view.leftFrame - steps * (width() - kHeaderW) * 0.1 / m_view.pxPerFrame);
        break;
    case WheelAction::Zoom: {
        stopFollowing(); // the user zooms: the view no longer follows the playing playhead
        // At the mouse or, by the user's choice, at the playhead while it is visible (ZoomAnchor)
        const double ax = ZoomAnchor::anchorX(std::max<double>(mouseX, kHeaderW), frameToX(m_playhead), kHeaderW, width());
        m_view.setZoomAround(m_view.pxPerFrame * std::pow(1.2, steps), xToFrame(ax), ax - kHeaderW);
        if (m_center) m_view.leftFrame = centreLeft();
        update();
        emit viewChanged();
        break;
    }
    case WheelAction::TrackHeight: {
        // only the track kind under the mouse (video or audio tracks), like in DaVinci
        const auto row = rowAt(int(e->position().y()));
        const bool audio = row && row->ref.kind == TrackKind::Audio;
        if (audio && m_fitAudio) { // the user's own height from now on, from what is shown
            m_view.audioTrackHeight = audioRowHeight();
            m_fitAudio = false;
        }
        int& h = audio ? m_view.audioTrackHeight : m_view.videoTrackHeight;
        h = std::clamp(h + int(steps * 6), 28, 220);
        setScrollY(m_view.scrollY);
        emit trackHeightsChanged();
        break;
    }
    case WheelAction::None:
        e->ignore();
        return;
    }
    e->accept();
}
