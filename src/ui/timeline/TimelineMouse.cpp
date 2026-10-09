// Timeline: mouse and wheel input (select, drag, trim, rubber band, auto scroll).
#include "ui/timeline/TimelineView.h"

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

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
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
    if (r.width() < kDragStartPx && r.height() < kDragStartPx) return; // Klick ohne Ziehen: nur abwählen
    // berührte Frames [f0, f1) und Zeilen (Rahmen muss die Clips nur berühren, wie DaVinci)
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
    // Nur Ziehvorgänge, bei denen Mitscrollen Sinn ergibt (nicht Lautstärke, Fades, Kurven …)
    const bool horizontal = m_drag == Drag::Rubber || m_drag == Drag::Move || m_drag == Drag::Trim ||
                            m_drag == Drag::TrimEdit || m_drag == Drag::CueMove || m_drag == Drag::CueTrim;
    const bool vertical = horizontal || m_drag == Drag::TrackMove;
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
    // Tempo wächst, je weiter die Maus über den Rand hinaus ist (px pro Schritt)
    auto speed = [](int depth) { return std::clamp(2 + depth / 2, 2, 60); };
    const bool horizontal = m_drag != Drag::TrackMove;
    int dx = 0, dy = 0;
    if (horizontal) {
        if (pos.x() < kHeaderW + kAutoScrollZone) dx = -speed(kHeaderW + kAutoScrollZone - pos.x());
        else if (pos.x() >= width() - kAutoScrollZone) dx = speed(pos.x() - (width() - kAutoScrollZone));
    }
    if (pos.y() < kRulerH + kAutoScrollZone) dy = -speed(kRulerH + kAutoScrollZone - pos.y());
    else if (pos.y() >= height() - kAutoScrollZone) dy = speed(pos.y() - (height() - kAutoScrollZone));

    // Ganze Pixel scrollen und den Druckpunkt um dieselben Pixel verschieben: alle Ziehrechnungen
    // (Abstand zum Druckpunkt) bleiben dadurch in Timeline-Koordinaten richtig
    const QPointF oldAnchor(frameToX(0), m_view.scrollY);
    if (dx) setLeftFrame(m_view.leftFrame + dx / m_view.pxPerFrame);
    if (dy) setScrollY(m_view.scrollY + dy);
    const int sx = int(std::lround(oldAnchor.x() - frameToX(0)));
    const int sy = m_view.scrollY - int(oldAnchor.y());
    if (!sx && !sy) return; // schon am Anfang / Ende
    m_pressPos -= QPoint(sx, sy);
    dragMove(pos, m_dragMods);
}

void TimelineView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const QPoint pos = e->position().toPoint();
    m_pressPos = pos;
    if (subtitlePress(e, pos)) return;

    if (pos.x() < kHeaderW) { // Spurköpfe
        if (const auto row = rowAt(pos.y()); row && headerButton(*row).contains(pos)) {
            if (row->ref.kind == TrackKind::Video) m_editor->toggleTrackHidden(row->ref);
            else m_editor->toggleTrackMute(row->ref);
        } else if (row && lockButton(*row).contains(pos)) {
            m_editor->toggleTrackLock(row->ref);
        } else if (row && shortNameRect(*row).contains(pos)) {
            if (row->ref.kind == TrackKind::Video) m_editor->setTargetTracks(row->ref.index, m_editor->targetAudioTrack());
            else m_editor->setTargetTracks(m_editor->targetVideoTrack(), row->ref.index);
        } else if (row) { // übriger Spurkopf: Ziehen sortiert die Spur um
            m_drag = Drag::TrackMaybeMove;
            m_trackDragRef = row->ref;
            m_trackDropTo = -1;
        }
        return;
    }
    if (pos.y() < kRulerH) {
        m_drag = Drag::Scrub;
        m_scrubFrame = std::max(0, int(std::lround(xToFrame(pos.x()))));
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
    // Keyframes: Symbol klappt die Spur auf/zu, Rauten auswählen (Strg = dazu) und ziehen
    if (const int kid = keyIconAt(pos)) {
        if (m_keyLanes.contains(kid)) m_keyLanes.remove(kid);
        else m_keyLanes.insert(kid);
        if (sel->keyClip() == kid) sel->setKeyframes(0, {});
        setScrollY(m_view.scrollY); // Höhe hat sich geändert (Scrollbar anpassen, zeichnet neu)
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
            emit seekRequested(c->start + k->t); // Inspector zeigt die Werte an diesem Keyframe
            m_keyDragClip = k->clipId;
            m_keyDelta = 0;
            m_drag = Drag::Keyframe;
        }
        update();
        return;
    }
    // Retime-Steuerung: Speed-Punkt ziehen, Klick auf das Tempo öffnet die Auswahl (wie DaVinci ▾)
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
    if (inLane(pos)) { // leere Stelle in der Keyframe-Spur: Rauten abwählen
        sel->setKeyframes(0, {});
        update();
        return;
    }
    if (m_tool == Tool::Trim) {
        const auto hit = trimHitAt(pos);
        if (!hit) {
            if (!(e->modifiers() & Qt::ControlModifier)) sel->clear();
            return;
        }
        const QVector<int> group = m_editor->withLinked({hit->clipId});
        if (!sel->contains(hit->clipId)) sel->set(QSet<int>(group.begin(), group.end()));
        m_trimHit = *hit;
        m_trimEdit = m_editor->trimEdit(hit->kind, hit->clipId, hit->edge);
        m_trimEditDelta = 0;
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
        m_editor->project()->closeMerge(); // ganzes Ziehen = ein Undo-Schritt
        m_drag = Drag::Fade;
        return;
    }
    if (const auto t = transitionAt(pos)) {
        sel->setTransition({t->span.leftId, t->span.rightId});
        if (t->edge) {
            m_transSpan = t->span;
            m_transEdge = t->edge;
            m_editor->project()->closeMerge(); // ganzes Ziehen = ein Undo-Schritt
            m_drag = Drag::TransitionLength;
        }
        return;
    }
    if (const auto hit = edgeAt(pos)) {
        const QVector<int> group = m_editor->withLinked({hit->clipId});
        if (!sel->ids().contains(hit->clipId)) sel->set(QSet<int>(group.begin(), group.end()));
        m_trim = *hit;
        m_trimIds = group;
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
        m_volStartDb = m_volDb = volumeAt(*vc, (pos.x() - frameToX(vc->start)) / m_view.pxPerFrame); // mit Keyframes
        m_volFine = e->modifiers() & Qt::ShiftModifier;
        m_drag = Drag::Volume;
        update();
        return;
    }
    if (!id) {
        startRubber(e->modifiers());
        return;
    }
    // Alt-Klick: nur dieser Teil eines verknüpften Clips (wie DaVinci), z. B. den Ton allein verschieben
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
    m_dragIds = m_editor->editable(sel->ids().values().toVector());
    m_dragDelta = 0;
    m_dragFine = 0;
    m_dragAnchorId = id;
    m_dragTrackDelta = 0;
    m_drag = Drag::MaybeMove;
}

void TimelineView::mouseMoveEvent(QMouseEvent* e)
{
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
    case Drag::Scrub: {
        // Nur bei neuem Frame springen: jeder Sprung verwirft während der Wiedergabe den Puffer und dekodiert neu –
        // beim Klicken (Maus zittert ein Pixel) ruckelte das
        const int frame = std::max(0, int(std::lround(xToFrame(pos.x()))));
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
        // Feinposition (Sub-Frame wie DaVinci Fairlight): nur reine Ton-Clips mit gleicher Feinposition und erst,
        // wenn ein Frame breit genug gezoomt ist; sonst ganze Frames
        bool fine = m_view.pxPerFrame >= kFinePxPerFrame;
        int sub = -1;
        for (int id : m_dragIds) {
            TrackRef ref;
            if (const Clip* c = TimelineOps::findClip(tl, id, &ref)) {
                edges << c->start + delta << c->end() + delta;
                minStart = std::min(minStart, c->start);
                if (ref.kind != TrackKind::Audio || (sub >= 0 && sub != c->subframe)) fine = false;
                sub = c->subframe;
            }
        }
        const QSet<int> exclude(m_dragIds.begin(), m_dragIds.end());
        bool snapped = false;
        delta += snapDelta(edges, exclude, &snapped);
        if (minStart != INT_MAX) delta = std::max(delta, -minStart);
        m_dragDelta = delta;
        m_dragFine = delta * 100;
        if (fine && sub >= 0) {
            // eingerastet: genau auf den Frame (Feinposition weg), sonst frei in 1/100 Frame
            m_dragFine = snapped ? delta * 100 - sub : int(std::lround(exact * 100));
            m_dragFine = std::max(m_dragFine, -(minStart * 100 + sub));
        }

        m_dragTrackDelta = 0;
        if (const auto row = rowAt(pos.y()); row && row->ref.kind == m_anchorRef.kind)
            m_dragTrackDelta = TimelineOps::clampTrackDelta(tl, m_dragIds, m_anchorRef.kind,
                                                            row->ref.index - m_anchorRef.index);
        for (TrackRef ref : TimelineOps::tracksOf(tl, m_dragIds)) // nie auf eine gesperrte Spur
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
        delta += snapDelta(edges, QSet<int>(m_cueIds.begin(), m_cueIds.end()));
        if (minStart != INT_MAX) delta = std::max(delta, -minStart);
        m_cueDelta = delta;
        m_cueTrackDelta = 0;
        if (const auto row = subRowAt(pos.y())) {
            m_cueTrackDelta = m_editor->clampSubtitleTrackDelta(m_cueIds, row->index - m_cueAnchor);
            for (int id : m_cueIds) { // nie auf eine gesperrte Spur
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
        const int clamped = m_editor->clampTrim(m_trim.clipId, m_trim.edge, delta);
        if (clamped != m_trimDelta) { // trim view: same edge trim on a copy (no ripple)
            Timeline preview = m_editor->project()->timeline();
            // already clamped -> source length not needed again
            TimelineOps::trimClips(preview, m_trimIds, m_trim.edge, clamped, [](const Clip&) { return 0; });
            updateTrimFrames(preview, {TimelineOps::TrimKind::Ripple, m_trimIds, {}, m_trim.edge}, clamped);
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
        // Snapping auf die Kanten, die sich sichtbar bewegen (Slip bewegt keine Kante)
        QSet<int> exclude(m_trimEdit.ids.begin(), m_trimEdit.ids.end());
        for (int id : m_trimEdit.rightIds) exclude.insert(id);
        QVector<int> edges;
        const bool start = m_trimHit.edge == TimelineOps::Edge::Start;
        switch (m_trimHit.kind) {
        case TrimKind::Ripple:
            if (!start) {
                edges << c->end() + delta;
                for (int id : m_trimEdit.ids) // was nachrückt (alle nicht gesperrten Spuren), ist kein fester Punkt
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
        if (delta != m_trimEditDelta || !m_trimPreview) {
            m_trimEditDelta = delta;
            m_trimPreview = m_editor->previewTrimEdit(m_trimEdit, delta);
            updateTrimFrames(*m_trimPreview, m_trimEdit, delta);
            update();
        }
        return;
    }
    case Drag::CurvePoint:
    case Drag::CurveHandle:
        curveMove(pos, mods);
        return;
    case Drag::Keyframe: {
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_keyDragClip);
        const QSet<int>& times = m_editor->selection()->keyTimes();
        if (!c || times.isEmpty()) return;
        int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        { // Einrasten wie DaVinci: Playhead, Clipgrenzen, Keyframes der anderen Zeiten
            QVector<int> moved;
            for (int t : times) moved << t + delta;
            delta += keySnapDelta(*c, moved, times);
        }
        // alle ausgewählten Rauten bleiben im Clip
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
        // Zentrierte Überblendung wächst an beiden Seiten, Ein-/Ausblenden nur an der freien Kante
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
        // Shift umgeschaltet -> von der aktuellen Position aus weiter, damit nichts springt
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
            if (std::abs(db) < 0.5) db = 0; // 0 dB rastet ein
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

void TimelineView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    m_autoScroll->stop();
    if (m_drag == Drag::Rubber) {
        m_drag = Drag::None;
        m_rubberBase.clear();
        updateHoverCursor(e->position().toPoint());
        update();
        return;
    }
    if (m_drag == Drag::TrackMove || m_drag == Drag::TrackMaybeMove) {
        const bool moved = m_drag == Drag::TrackMove;
        const int to = m_trackDropTo;
        m_drag = Drag::None;
        m_trackDropTo = -1;
        if (moved && to >= 0 && to != m_trackDragRef.index) m_editor->moveTrack(m_trackDragRef.kind, m_trackDragRef.index, to);
        updateHoverCursor(e->position().toPoint());
        update();
        return;
    }
    if (m_drag == Drag::Move)
        m_editor->moveClipsFine(m_dragIds, m_dragFine, m_anchorRef.kind, m_dragTrackDelta);
    if (m_drag == Drag::CueMove || m_drag == Drag::CueTrim) {
        const Drag d = m_drag;
        m_drag = Drag::None; // vor der Änderung, damit die Vorschau nicht doppelt gilt
        if (d == Drag::CueMove) m_editor->moveSubtitles(m_cueIds, m_cueDelta, m_cueTrackDelta);
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
    m_drag = Drag::None; // vor trimClip, damit die Vorschau nicht doppelt angewendet wird
    if (ramp && m_rampDelta != 0) m_editor->moveSpeedPoint(m_rampDrag.clipId, m_rampDrag.point, m_rampDelta);
    m_rampDelta = 0;
    if (trimmed && m_trimDelta != 0) m_editor->trimClip(m_trim.clipId, m_trim.edge, m_trimDelta);
    if (trimEdit && m_trimEditDelta != 0) m_editor->applyTrimEdit(m_trimEdit, m_trimEditDelta);
    m_trimPreview.reset();
    m_trimEditDelta = 0;
    if (trimmed || trimEdit) endTrimFrames(); // viewer back to the playhead frame
    if (keys && m_keyDelta != 0) { // ein Undo-Schritt pro Ziehen
        const QVector<int> times = m_editor->selection()->keyTimes().values().toVector();
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_keyDragClip);
        const int seek = c && !times.isEmpty() ? c->start + *std::min_element(times.begin(), times.end()) + m_keyDelta : -1;
        m_editor->moveKeyframes(m_keyDragClip, times, m_keyDelta); // ersetzt die Timeline -> c ungültig
        if (seek >= 0) emit seekRequested(seek); // Playhead auf die verschobene Raute
    }
    m_keyDelta = 0;
    const Clip* volClip = volume ? TimelineOps::findClip(m_editor->project()->timeline(), m_volClipId) : nullptr;
    if (volClip && Keys::animated(*volClip, AnimParam::Volume)) {
        // Keyframes: ganze Kurve um den gezogenen Betrag verschieben
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
    // Doppelklick auf den Spurnamen: umbenennen (wie DaVinci)
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
    // Qt tauscht bei gedrückter Alt-Taste die Achsen -> beide Richtungen auswerten
    const QPoint ad = e->angleDelta();
    const double steps = (ad.y() != 0 ? ad.y() : ad.x()) / 120.0;
    if (steps == 0) return;
    const double mouseX = e->position().x();

    switch (InputBindings::instance().wheelAction(e->modifiers())) {
    case WheelAction::ScrollVertical:
        setScrollY(m_view.scrollY - int(steps * 40));
        break;
    case WheelAction::ScrollHorizontal:
        setLeftFrame(m_view.leftFrame - steps * (width() - kHeaderW) * 0.1 / m_view.pxPerFrame);
        break;
    case WheelAction::Zoom: {
        const double ax = std::max<double>(mouseX, kHeaderW);
        m_view.setZoomAround(m_view.pxPerFrame * std::pow(1.2, steps), xToFrame(ax), ax - kHeaderW);
        update();
        emit viewChanged();
        break;
    }
    case WheelAction::TrackHeight: {
        // nur die Spurart unter der Maus (Video- oder Audiospuren), wie in DaVinci
        const auto row = rowAt(int(e->position().y()));
        const bool audio = row && row->ref.kind == TrackKind::Audio;
        int& h = audio ? m_view.audioTrackHeight : m_view.videoTrackHeight;
        h = std::clamp(h + int(steps * 6), 28, 220);
        setScrollY(m_view.scrollY);
        break;
    }
    case WheelAction::None:
        e->ignore();
        return;
    }
    e->accept();
}
