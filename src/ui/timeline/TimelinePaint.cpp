// Timeline: drawing (ruler, tracks, clips, filmstrips, waveforms, headers, playhead).
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

void TimelineView::paintEvent(QPaintEvent*)
{
    if (m_renaming && m_nameSub >= 0) { // Untertitelspur
        std::optional<SubRow> row;
        for (const SubRow& r : subRows())
            if (r.index == m_nameSub) row = r;
        if (row) {
            const QRect g = subNameRect(*row).adjusted(-3, 0, 0, 0);
            if (m_nameEdit->geometry() != g) m_nameEdit->setGeometry(g);
        } else {
            finishRename(false);
        }
    } else if (m_renaming) { // Namensfeld folgt der Zeile (Scrollen, Spurhöhe)
        if (const auto row = rowFor(m_nameRef)) {
            const QRect g = nameRect(*row).adjusted(-3, 0, 0, 0);
            if (m_nameEdit->geometry() != g) m_nameEdit->setGeometry(g);
        } else {
            finishRename(false);
        }
    }
    QPainter p(this);
    p.fillRect(rect(), Theme::timelineBg);
    drawTracks(p);
    drawSubtitleTracks(p);
    drawHeaders(p);
    drawSubtitleHeaders(p);
    drawTrackDrop(p);
    drawRubber(p);
    drawRuler(p);
    drawPlayhead(p);
}

void TimelineView::drawRuler(QPainter& p)
{
    const int fps = m_editor->project()->fps();
    p.fillRect(QRect(0, 0, width(), kRulerH), Theme::ruler);
    p.setPen(Theme::border);
    p.drawLine(0, kRulerH - 1, width(), kRulerH - 1);

    // Abstand der Beschriftungen so wählen, dass mind. ~100 px dazwischen liegen
    const int candidates[] = {1, 2, 5, 10, fps, 2 * fps, 5 * fps, 10 * fps, 15 * fps, 30 * fps, 60 * fps,
                              120 * fps, 300 * fps, 600 * fps, 900 * fps, 1800 * fps, 3600 * fps, 7200 * fps};
    int major = candidates[std::size(candidates) - 1];
    for (int c : candidates) {
        if (c * m_view.pxPerFrame >= 100) {
            major = c;
            break;
        }
    }
    int minor = major % 5 == 0 ? major / 5 : (major % 2 == 0 ? major / 2 : major);
    if (minor * m_view.pxPerFrame < 6) minor = major;

    p.save();
    p.setClipRect(kHeaderW, 0, width() - kHeaderW, kRulerH);
    const int first = int(m_view.leftFrame) / minor * minor;
    const int last = int(xToFrame(width())) + minor;
    QFont f = font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    for (int fr = first; fr <= last; fr += minor) {
        const int x = int(frameToX(fr));
        const bool isMajor = fr % major == 0;
        p.setPen(isMajor ? Theme::textDim : Theme::textFaint);
        p.drawLine(x, kRulerH - (isMajor ? 12 : 5), x, kRulerH - 1);
        if (isMajor) {
            p.setPen(Theme::textDim);
            p.drawText(x + 3, 12, Timecode::format(fr, fps));
        }
    }
    // Render-Cache (wie DaVinci): rot = muss noch gerendert werden, blau = fertig
    for (const CacheSpan& c : m_cacheSpans) {
        const double x0 = frameToX(c.start), x1 = frameToX(c.end);
        if (x1 < kHeaderW || x0 > width()) continue;
        const double xd = x0 + (x1 - x0) * std::clamp(c.done, 0.0, 1.0);
        p.fillRect(QRectF(x0, 0, xd - x0, 3), Theme::cacheReady);
        p.fillRect(QRectF(xd, 0, x1 - xd, 3), Theme::cacheMissing);
    }
    // In/Out-Bereich (I/O): Balken in der Sekundärfarbe im Lineal mit Klammern an den Enden
    const Timeline& tlm = m_editor->project()->timeline();
    if (tlm.markIn >= 0 || tlm.markOut >= 0) {
        const double xIn = tlm.markIn >= 0 ? frameToX(tlm.markIn) : kHeaderW - 1;
        const double xOut = tlm.markOut >= 0 ? frameToX(tlm.markOut + 1) : width() + 1;
        p.fillRect(QRectF(xIn, kRulerH - 16, xOut - xIn, 16), Theme::alpha(Theme::secondary, 45));
        p.setPen(QPen(Theme::secondary, 2));
        if (tlm.markIn >= 0) {
            p.drawLine(QPointF(xIn + 1, kRulerH - 16), QPointF(xIn + 1, kRulerH - 1));
            p.drawLine(QPointF(xIn + 1, kRulerH - 15), QPointF(xIn + 5, kRulerH - 15));
        }
        if (tlm.markOut >= 0) {
            p.drawLine(QPointF(xOut - 1, kRulerH - 16), QPointF(xOut - 1, kRulerH - 1));
            p.drawLine(QPointF(xOut - 1, kRulerH - 15), QPointF(xOut - 5, kRulerH - 15));
        }
    }
    // Marker (M) als kleine Fähnchen wie in DaVinci (Sekundärfarbe)
    p.setRenderHint(QPainter::Antialiasing);
    for (int m : m_editor->project()->timeline().markers) {
        const double x = frameToX(m);
        if (x < kHeaderW - 8 || x > width() + 8) continue;
        QPainterPath flag;
        flag.moveTo(x - 5, kRulerH - 14);
        flag.lineTo(x + 5, kRulerH - 14);
        flag.lineTo(x + 5, kRulerH - 7);
        flag.lineTo(x, kRulerH - 2);
        flag.lineTo(x - 5, kRulerH - 7);
        flag.closeSubpath();
        p.fillPath(flag, Theme::secondary);
    }
    p.restore();

    // Ecke links oben: aktueller Timecode wie in DaVinci, im eingelassenen Feld
    p.fillRect(QRect(0, 0, kHeaderW, kRulerH), Theme::panelHeader);
    const QRect tcBox(4, 4, kHeaderW - 9, kRulerH - 8);
    p.fillRect(tcBox, Theme::well);
    p.setPen(Theme::border);
    p.drawRect(tcBox.adjusted(0, 0, -1, -1));
    QFont tf = font();
    tf.setPointSizeF(11);
    tf.setFamily("monospace");
    p.setFont(tf);
    p.setPen(Theme::text);
    p.drawText(tcBox.adjusted(5, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, Timecode::format(m_playhead, fps));
    p.setPen(Theme::border);
    p.drawLine(kHeaderW - 1, 0, kHeaderW - 1, height());
}

void TimelineView::drawTracks(QPainter& p)
{
    // Trim-Modus: Timeline so zeichnen, wie sie nach dem Loslassen aussieht
    const bool trimEdit = m_drag == Drag::TrimEdit && m_trimPreview;
    const Timeline& tl = trimEdit ? *m_trimPreview : m_editor->project()->timeline();
    const auto& sel = m_editor->selection()->ids();
    const QSet<int> dragSet(m_dragIds.begin(), m_dragIds.end());
    const bool moving = m_drag == Drag::Move;

    p.save();
    p.setClipRect(kHeaderW, kRulerH, width() - kHeaderW, height() - kRulerH);

    const auto allRows = rows();
    for (const Row& row : allRows) {
        const bool alt = row.ref.index % 2;
        p.fillRect(QRect(kHeaderW, row.y, width() - kHeaderW, row.h), alt ? Theme::trackBgAlt : Theme::trackBg);
        p.setPen(Theme::border);
        p.drawLine(kHeaderW, row.y + row.h - 1, width(), row.y + row.h - 1);

        const Track& track = tl.track(row.ref);
        p.setOpacity(track.muted || track.hidden ? 0.4 : 1.0);
        QSet<int> hidden; // Übergänge von Clips, die gerade gezogen/getrimmt werden, ausblenden
        if (moving) hidden = dragSet;
        if (m_drag == Drag::Trim) hidden = QSet<int>(m_trimIds.begin(), m_trimIds.end());
        if (trimEdit) {
            // Spur ändert sich evtl. -> Übergänge ausblenden (Ripple rückt alle nicht gesperrten Spuren)
            bool affected = m_trimEdit.kind == TimelineOps::TrimKind::Ripple && !track.locked;
            for (const Clip& c : track.clips)
                affected |= m_trimEdit.ids.contains(c.id) || m_trimEdit.rightIds.contains(c.id);
            if (affected)
                for (const Clip& o : track.clips) hidden.insert(o.id);
        }
        QVector<TimelineOps::TransitionSpan> audioSpans; // Wellenform folgt den Crossfades
        if (row.ref.kind == TrackKind::Audio)
            for (const auto& s : m_editor->transitions(row.ref))
                if (!hidden.contains(s.leftId) && !hidden.contains(s.rightId)) audioSpans << s;
        for (Clip c : track.clips) {
            if (moving && dragSet.contains(c.id)) continue; // wird unten verschoben gezeichnet
            if (m_drag == Drag::Trim && m_trimIds.contains(c.id)) {
                if (m_trim.edge == TimelineOps::Edge::Start) {
                    c.start += m_trimDelta;
                    c.in += m_trimDelta;
                } else {
                    c.out += m_trimDelta;
                }
            }
            if (m_drag == Drag::Volume && c.id == m_volClipId) {
                c.volumeDb = m_volDb;
                // animiert: ganze Kurve verschieben (Vorschau wie beim Loslassen)
                for (Keyframe& k : c.keys[AnimParam::Volume])
                    k.value = std::clamp(k.value + m_volDb - m_volStartDb, kMinVolumeDb, kMaxVolumeDb);
                if (c.keys.value(AnimParam::Volume).isEmpty()) c.keys.remove(AnimParam::Volume);
            }
            const QRect r = clipRect(row, c);
            if (r.right() < kHeaderW || r.left() > width()) continue;
            drawClip(p, r, c, row.ref.kind, sel.contains(c.id), false, audioSpans, clipColor(m_editor->project(), c, &track));
            if (row.keyLane && m_keyLanes.contains(c.id)) drawKeyLane(p, row, c);
            if (row.curve && curveParam(c)) drawCurveLane(p, row, c);
        }
        drawTransitions(p, row, hidden);
        if (track.locked) { // gesperrt: abgedunkelt und schraffiert wie DaVinci
            p.setOpacity(1.0);
            const QRect area(kHeaderW, row.y, width() - kHeaderW, row.h - 1);
            p.fillRect(area, QColor(0, 0, 0, 90));
            p.fillRect(area, QBrush(QColor(255, 255, 255, 30), Qt::BDiagPattern));
        }
    }

    p.setOpacity(1.0);

    // Verschobene Clips an der Zielposition
    if (moving) {
        for (int id : m_dragIds) {
            TrackRef ref;
            const Clip* c = TimelineOps::findClip(tl, id, &ref);
            if (!c) continue;
            ref.index += m_dragTrackDelta;
            auto row = rowFor(ref);
            if (!row && ref.kind == TrackKind::Audio && !allRows.isEmpty()) { // Spur entsteht beim Loslassen
                const Row& last = allRows.last();
                const int extra = ref.index - int(tl.audio.size());
                row = Row{ref, last.y + last.h + extra * m_view.audioTrackHeight, m_view.audioTrackHeight};
            }
            if (!row) continue;
            Clip moved = *c;
            TimelineOps::shiftFine(moved, m_dragFine);
            const QRect r(QPoint(int(frameToX(moved.pos())), row->y + 1),
                          QPoint(int(frameToX(moved.endPos())) - 1, row->y + row->h - row->lane - 3));
            const QColor col = clipColor(m_editor->project(), *c, ref.index < tl.tracks(ref.kind).size() ? &tl.track(ref) : nullptr);
            drawClip(p, r, moved, ref.kind, true, true, {}, col);
        }
    }

    // Vorschau beim Reinziehen: Video auf V[n], Audio auf A[n]
    if (m_dropFrame >= 0 && !m_dropItems.isEmpty()) {
        // Spur k unter der Zielspur (weitere Ton-Streams); fehlende Audiospuren entstehen unter der letzten
        auto ghostRow = [&](TrackKind kind, int k) -> std::optional<Row> {
            const int idx = m_dropTrack + k;
            if (auto r = rowFor({kind, idx})) return isLocked(*r) ? std::nullopt : r; // gesperrte Spur bekommt nichts
            if (kind == TrackKind::Audio && !allRows.isEmpty()) {
                const Row& last = allRows.last();
                const int missing = std::max(0, idx - int(tl.audio.size()));
                return Row{{kind, idx}, last.y + last.h + missing * m_view.audioTrackHeight, m_view.audioTrackHeight};
            }
            return std::nullopt;
        };
        const auto vRow = ghostRow(TrackKind::Video, 0);
        int start = m_dropFrame;
        for (const DropItem& it : m_dropItems) {
            const int x1 = int(frameToX(start)), x2 = int(frameToX(start + it.length));
            if (it.video && vRow) p.fillRect(QRect(x1, vRow->y + 1, x2 - x1, vRow->h - 3), Theme::alpha(Theme::videoClip, 150));
            for (int k = 0; k < it.audio; ++k)
                if (const auto aRow = ghostRow(TrackKind::Audio, k))
                    p.fillRect(QRect(x1, aRow->y + 1, x2 - x1, aRow->h - 3), Theme::alpha(Theme::audioClip, 150));
            start += it.length;
        }
        p.setPen(QPen(Theme::primary, 1, Qt::DashLine));
        const int x = int(frameToX(m_dropFrame));
        p.drawLine(x, kRulerH, x, height());
    }

    // Übergang reinziehen: Lage des neuen Übergangs am Schnitt zeigen
    if (m_transDrop) {
        if (const auto row = rowFor(m_transDrop->ref)) {
            const QRect r = transitionRect(*row, m_transDrop->span);
            QColor fill = Theme::primary;
            fill.setAlpha(110);
            p.fillRect(r, fill);
            p.setPen(QPen(Theme::primary, 2));
            p.drawRect(r.adjusted(1, 1, -1, -1));
            const int x = int(frameToX(m_transDrop->span.cut));
            p.drawLine(x, row->y, x, row->y + row->h - 2);
        }
    }

    // Filter reinziehen: Ziel-Clip umranden
    if (m_fxDropClip) {
        TrackRef ref;
        if (const Clip* c = TimelineOps::findClip(tl, m_fxDropClip, &ref))
            if (const auto row = rowFor(ref)) {
                const QRect r(int(frameToX(c->pos())), row->y, int(frameToX(c->endPos()) - frameToX(c->pos())),
                              row->h - row->lane - 1);
                p.setPen(QPen(Theme::primary, 2));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(1, 1, -1, -1));
            }
    }

    // Gewählter Schnittpunkt (V/U): Klammer an der gewählten Seite bzw. an beiden (Roll), mit Partnern
    if (const EditPoint ep = m_editor->selection()->editPoint(); !ep.isNull()) {
        auto bracket = [&](int id, bool end) {
            for (int pid : m_editor->withLinked({id})) {
                TrackRef ref;
                const Clip* c = TimelineOps::findClip(tl, pid, &ref);
                const auto row = c ? rowFor(ref) : std::nullopt;
                if (!row) continue;
                const QRect r = clipRect(*row, *c);
                const int w = std::min(4, std::max(1, r.width() / 2));
                p.fillRect(end ? QRect(r.right() - w + 1, r.top(), w, r.height()) : QRect(r.left(), r.top(), w, r.height()),
                           Theme::primary);
            }
        };
        if (ep.leftId && ep.side <= 0) bracket(ep.leftId, true);
        if (ep.rightId && ep.side >= 0) bracket(ep.rightId, false);
    }

    // Trimmen: Kante markieren + Versatz anzeigen (wie DaVinci)
    if (m_drag == Drag::Trim) {
        if (const Clip* c = TimelineOps::findClip(tl, m_trim.clipId)) {
            const int edgeFrame = m_trim.edge == TimelineOps::Edge::Start ? c->start + m_trimDelta : c->end() + m_trimDelta;
            const int x = int(frameToX(edgeFrame));
            p.setPen(QPen(Theme::primary, 1));
            p.drawLine(x, kRulerH, x, height());
            drawLabel(p, QPoint(x + 8, kRulerH + 6),
                      (m_trimDelta >= 0 ? "+" : "") + Timecode::format(m_trimDelta, m_editor->project()->fps()));
        }
    }

    // Trim-Modus: bewegte Kante bzw. Clip markieren + Versatz anzeigen
    if (trimEdit) {
        using TimelineOps::TrimKind;
        TrackRef ref;
        if (const Clip* c = TimelineOps::findClip(tl, m_trimHit.clipId, &ref)) {
            const bool start = m_trimHit.edge == TimelineOps::Edge::Start;
            int frame = c->start;
            if (m_trimHit.kind == TrimKind::Roll || (m_trimHit.kind == TrimKind::Ripple && !start))
                frame = start ? c->start : c->end();
            const int x = int(frameToX(frame));
            p.setPen(QPen(Theme::primary, 1));
            if (m_trimHit.kind == TrimKind::Slip || m_trimHit.kind == TrimKind::Slide) {
                if (const auto row = rowFor(ref)) p.drawRect(clipRect(*row, *c).adjusted(0, 0, -1, -1));
            } else {
                p.drawLine(x, kRulerH, x, height());
            }
            drawLabel(p, QPoint(x + 8, kRulerH + 6),
                      (m_trimEditDelta >= 0 ? "+" : "") + Timecode::format(m_trimEditDelta, m_editor->project()->fps()));
        }
    }

    // Lautstärke ziehen: aktuellen Wert an der Linie anzeigen
    if (m_drag == Drag::Volume) {
        TrackRef ref;
        if (const Clip* c = TimelineOps::findClip(tl, m_volClipId, &ref)) {
            if (const auto row = rowFor(ref)) {
                const QRect body = clipBodyRect(clipRect(*row, *c));
                const int y = volumeLineY(body, m_volDb);
                const int x = std::max(body.left(), kHeaderW) + 6;
                drawLabel(p, QPoint(x, y < body.top() + body.height() / 2 ? y + 4 : y - 22), formatVolumeDb(m_volDb));
            }
        }
    }

    // Klingen-Vorschau
    if (m_tool == Tool::Blade && m_hoverFrame >= 0) {
        const int x = int(frameToX(m_hoverFrame));
        p.setPen(QPen(Theme::primary, 1));
        p.drawLine(x, kRulerH, x, height());
    }
    p.restore();
}

// Übergang als halbtransparentes Kästchen über dem Schnitt (wie DaVinci), Diagonale zeigt die Richtung
void TimelineView::drawTransitions(QPainter& p, const Row& row, const QSet<int>& hiddenClips)
{
    const TransitionKey selKey = m_editor->selection()->transition();
    for (const auto& s : m_editor->transitions(row.ref)) {
        if (hiddenClips.contains(s.leftId) || hiddenClips.contains(s.rightId)) continue;
        const QRect r = transitionRect(row, s);
        if (r.right() < kHeaderW || r.left() > width() || r.width() < 2) continue;
        const bool selected = selKey == TransitionKey{s.leftId, s.rightId};
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        p.fillPath(path, QColor(0xe6, 0xe6, 0xee, selected ? 130 : 95));
        p.setClipPath(path);
        p.setPen(QPen(QColor(0xff, 0xff, 0xff, 170), 1));
        if (s.rightId) p.drawLine(r.bottomLeft(), r.topRight());  // einblenden
        if (s.leftId) p.drawLine(r.topLeft(), r.bottomRight());   // ausblenden
        // Schnitt in der unteren Hälfte andeuten: dort wird getrimmt statt der Übergang gewählt
        if (s.isDissolve()) {
            const int cx = int(frameToX(s.cut));
            p.setPen(QPen(QColor(0x10, 0x10, 0x10, 160), 1, Qt::DashLine));
            p.drawLine(cx, r.top() + r.height() / 2, cx, r.bottom());
        }
        if (r.width() > 80 && r.height() > 14) {
            QFont f = font();
            f.setPointSizeF(7);
            p.setFont(f);
            p.setPen(QColor(0x10, 0x10, 0x10));
            const QString name = row.ref.kind != TrackKind::Video ? QString(audioCurveInfo(s.style.audio).name)
                                 : s.style.isLuma()                 ? EffectFolders::displayName(s.style.luma)
                                                                    : T(transitionTypeInfo(s.style.type).name);
            const QRect tr = r.adjusted(4, 0, -4, 0);
            p.drawText(tr, Qt::AlignTop | Qt::AlignHCenter, p.fontMetrics().elidedText(name, Qt::ElideRight, tr.width()));
        }
        p.setClipping(false);
        p.setPen(selected ? QPen(Theme::clipSelected, 2) : QPen(QColor(0xff, 0xff, 0xff, 200), 1));
        p.drawPath(path);
        p.restore();
    }
    // Länge ziehen: Wert anzeigen
    if (m_drag == Drag::TransitionLength) {
        for (const auto& s : m_editor->transitions(row.ref)) {
            if (s.leftId != m_transSpan.leftId || s.rightId != m_transSpan.rightId) continue;
            const QRect r = transitionRect(row, s);
            drawLabel(p, QPoint(std::max(r.left(), kHeaderW) + 4, r.top() + 4),
                      Timecode::format(s.length(), m_editor->project()->fps()));
        }
    }
}

// Keyframe-Spur unter dem Clip: eine Raute pro Keyframe-Zeit (alle Parameter zusammen, wie DaVincis
// zusammengefasste Spur); ausgewählte orange, beim Ziehen an der neuen Stelle
void TimelineView::drawKeyLane(QPainter& p, const Row& row, const Clip& c)
{
    const QRect lane = laneRect(row, c);
    if (lane.right() < kHeaderW || lane.left() > width()) return;
    const Selection* sel = m_editor->selection();
    const bool mine = sel->keyClip() == c.id && sel->keyParam() < 0;
    p.save();
    p.fillRect(lane, Theme::lane);
    p.setPen(Theme::control);
    p.drawRect(lane.adjusted(0, 0, -1, -1));
    p.setClipRect(lane.adjusted(-6, 0, 6, 0), Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing);
    const double y = lane.center().y() + 0.5;
    // ausgewählte zuletzt, damit sie beim Ziehen über anderen liegen
    for (bool pass : {false, true})
        for (int t : Keys::keyTimes(c)) {
            if (t < 0 || t >= c.length()) continue;
            const bool selected = mine && sel->keyTimes().contains(t);
            if (selected != pass) continue;
            const int shown = t + (selected && m_drag == Drag::Keyframe ? m_keyDelta : 0);
            p.setPen(QPen(QColor(0, 0, 0, 180), 1));
            p.setBrush(selected ? Theme::primary : Theme::secondary);
            drawDiamond(p, frameToX(c.pos() + shown), y, 4.5);
        }
    p.restore();
    if (m_drag == Drag::Keyframe && m_keyDragClip == c.id && m_keyDelta != 0 && !sel->keyTimes().isEmpty()) {
        const int t = *std::min_element(sel->keyTimes().begin(), sel->keyTimes().end()) + m_keyDelta;
        drawLabel(p, QPoint(int(frameToX(c.pos() + t)) + 8, lane.top() - 22),
                  (m_keyDelta > 0 ? "+" : "") + Timecode::format(m_keyDelta, m_editor->project()->fps()));
    }
}

void TimelineView::drawClip(QPainter& p, const QRect& r, const Clip& c, TrackKind kind, bool selected, bool ghost,
                            const QVector<TimelineOps::TransitionSpan>& spans, const QColor& color)
{
    QColor base = color.isValid() ? color
                  : c.isTitle()   ? Theme::titleClip
                  : c.isCompound() ? Theme::compoundClip
                  : kind == TrackKind::Video ? Theme::videoClip
                                             : Theme::audioClip;
    if (!c.enabled) base = Theme::controlOff; // deaktiviert (D) wie DaVinci: grau
    if (ghost) base.setAlpha(200);
    const QColor body = base.darker(135);

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    p.fillPath(path, body);

    // Titelleiste mit Dateiname
    const int barH = std::min(kClipBarH, r.height());
    p.save();
    p.setClipPath(path);
    p.fillRect(QRect(r.left(), r.top(), r.width(), barH), base);
    const QRect bodyRect = clipBodyRect(r);
    if (bodyRect.height() > 4) { // auch beim Ziehen: Wellenform/Bilder wandern mit (wie DaVinci)
        p.setRenderHint(QPainter::Antialiasing, false);
        if (c.isCompound()) drawCompoundBody(p, bodyRect, base);
        else if (kind == TrackKind::Video && !c.isTitle()) drawFilmstrip(p, bodyRect, c); // Titel: nur Farbe
        else if (kind == TrackKind::Audio) drawWaveform(p, bodyRect, c, spans, color);
        p.setRenderHint(QPainter::Antialiasing);
    }
    // Lautstärkelinie wie in DaVinci (zum Hoch-/Runterziehen)
    if (!ghost && kind == TrackKind::Audio && bodyRect.height() >= 6) {
        const bool active = c.id == m_hoverVolClip || (m_drag == Drag::Volume && c.id == m_volClipId);
        p.setRenderHint(QPainter::Antialiasing, false);
        if (Keys::animated(c, AnimParam::Volume)) {
            // Keyframes: Linie folgt der Kurve (nur sichtbarer Teil, alle 2 px)
            QPolygonF curve;
            const int x0 = std::max(bodyRect.left(), kHeaderW), x1 = std::min(bodyRect.right(), width());
            const double clipX = frameToX(c.pos());
            for (int x = x0; x <= x1 + 1; x += 2) {
                const int xx = std::min(x, x1);
                curve << QPointF(xx, volumeLineY(bodyRect, volumeAt(c, (xx - clipX) / m_view.pxPerFrame)));
            }
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(QColor(0, 0, 0, 140), 1));
            p.drawPolyline(curve.translated(0, active ? 2 : 1));
            p.setPen(QPen(active ? QColor(0xff, 0xff, 0xff) : QColor(0xff, 0xff, 0xff, 190), active ? 2 : 1));
            p.drawPolyline(curve);
        } else {
            const int y = volumeLineY(bodyRect, c.volumeDb);
            p.setPen(QPen(QColor(0, 0, 0, 140), 1)); // Schatten, damit die Linie auf der Wellenform lesbar bleibt
            p.drawLine(bodyRect.left(), y + (active ? 2 : 1), bodyRect.right(), y + (active ? 2 : 1));
            p.setPen(QPen(active ? QColor(0xff, 0xff, 0xff) : QColor(0xff, 0xff, 0xff, 190), active ? 2 : 1));
            p.drawLine(bodyRect.left(), y, bodyRect.right(), y);
        }
        p.setRenderHint(QPainter::Antialiasing);
    }
    // Fade-Bereiche wie DaVinci: Fläche über der Kurve abgedunkelt, Kurve von unten zum Pegel.
    // Video: gerade Rampe (Deckkraft linear), Audio: Sinus-Kurve auf der Lautstärkelinie (wie die Engine)
    if (!ghost && (c.fadeIn > 0 || c.fadeOut > 0)) {
        const int top = r.top() + barH, bottom = r.bottom();
        const int fi = std::min(c.fadeIn, c.length()), fo = std::min(c.fadeOut, c.length() - fi);
        const bool audio = kind == TrackKind::Audio && bodyRect.height() >= 6;
        auto ramp = [&](double x0, double x1, bool in) {
            QPolygonF curve; // von links nach rechts
            if (audio) {
                const double clipX = frameToX(c.pos());
                const int steps = std::clamp(int(x1 - x0) / 2, 2, 64);
                for (int i = 0; i <= steps; ++i) {
                    const double x = x0 + (x1 - x0) * i / steps;
                    // Randpixel liegen genau auf 0 bzw. 1, damit die Kurve unten ansetzt
                    const double t = in ? (x - clipX) / m_view.pxPerFrame
                                        : c.length() - (x1 - x) / m_view.pxPerFrame;
                    const double g = TimelineOps::audioFadeGain(c, std::clamp(t, 0.0, double(c.length())));
                    const double db = g > 0.001 ? volumeAt(c, t) + 20.0 * std::log10(g) : kMinVolumeDb;
                    curve << QPointF(x, volumeLineY(bodyRect, db));
                }
            } else if (in) {
                curve << QPointF(x0, bottom) << QPointF(x1, top);
            } else {
                curve << QPointF(x0, top) << QPointF(x1, bottom);
            }
            QPolygonF shade;
            shade << QPointF(x0, top) << QPointF(x1, top);
            for (int i = curve.size() - 1; i >= 0; --i) shade << curve[i];
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawPolygon(shade);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0xff, 0xff, 0xff, audio ? 220 : 170), 1));
            p.drawPolyline(curve);
        };
        if (fi > 0) ramp(r.left(), r.left() + fi * m_view.pxPerFrame, true);
        if (fo > 0) ramp(r.right() + 1 - fo * m_view.pxPerFrame, r.right() + 1, false);
        p.setBrush(Qt::NoBrush);
    }
    p.restore();

    // "fx" rechts in der Titelleiste wie DaVinci, wenn der Clip Effekte hat (Farbkorrektur der Color-Seite zählt nicht)
    const bool hasFx = std::any_of(c.effects.begin(), c.effects.end(), [](const EffectInstance& e) {
        const EffectDescriptor* d = EffectRegistry::find(e.effectId);
        return d && e.effectId != QLatin1String("grade") && (d->library || e.enabled);
    });
    int fxW = 0;
    if (hasFx && r.width() > 44 && barH >= 10) {
        QFont f = font();
        f.setPointSizeF(6.5);
        f.setItalic(true);
        f.setBold(true);
        p.setFont(f);
        fxW = QFontMetrics(f).horizontalAdvance("fx") + 6;
        const QRect fx(r.right() - fxW - 2, r.top() + 2, fxW, std::max(8, barH - 4));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawRoundedRect(fx, 2, 2);
        p.setBrush(Qt::NoBrush);
        p.setPen(QColor(0xf0, 0xf0, 0xf0));
        p.drawText(fx, Qt::AlignCenter, "fx");
        fxW += 4;
    }

    // Flags des Media-Pool-Clips vorne in der Titelleiste (wie DaVinci)
    int flagW = 0;
    if (!c.isTitle() && r.width() > 40 && barH >= 10) {
        if (const MediaInfo* m = m_editor->project()->mediaInfo(c.mediaPath)) {
            const int fs = std::min(barH - 4, 11);
            int x = std::max(r.left(), kHeaderW) + 4;
            for (const QString& id : m->flags) {
                const FlagColorInfo* fi = flagColorInfo(id);
                if (!fi || x + fs > r.right() - fxW - 20) continue;
                MediaPool::drawFlag(p, QRectF(x, r.top() + (barH - fs) / 2.0, fs, fs), QColor::fromRgba(fi->rgb));
                x += fs + 1;
                flagW += fs + 1;
            }
        }
    }

    if (r.width() > 24 && (m_showNames || m_showDurations)) {
        QFont f = font();
        f.setPointSizeF(7.5);
        p.setFont(f);
        const QFontMetrics fm(f);
        QRect textRect(std::max(r.left(), kHeaderW) + 5 + flagW, r.top(), r.right() - 3 - fxW - std::max(r.left(), kHeaderW) - 5 - flagW, barH);
        // Dauer rechts (vor „fx“), nur wenn sie ganz hineinpasst; der Name nimmt den Rest
        if (m_showDurations) {
            const QString dur = Timecode::format(c.length(), m_editor->project()->fps());
            const int w = fm.horizontalAdvance(dur);
            if (w + 6 <= textRect.width()) {
                p.setPen(QColor(0xf0, 0xf0, 0xf0, 170));
                p.drawText(textRect, Qt::AlignVCenter | Qt::AlignRight, dur);
                textRect.setRight(textRect.right() - w - 8);
            }
        }
        if (m_showNames && textRect.width() > 4) {
            p.setPen(QColor(0xf0, 0xf0, 0xf0));
            p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(clipLabel(m_editor->project(), c, kind), Qt::ElideRight, textRect.width()));
        }
    }

    if (!ghost && m_retimeClips.contains(c.id)) drawRetime(p, r, c);

    p.setPen(selected ? QPen(Theme::clipSelected, 2) : QPen(QColor(0, 0, 0, 120), 1));
    p.drawPath(path);

    // Keyframe-Symbol unten rechts (wie DaVinci): Klick klappt die Keyframe-Spur auf
    if (!ghost && r.width() >= 40 && r.height() >= 24 && Keys::hasKeys(c)) {
        const QRectF k = keyIconRect(r);
        const bool open = m_keyLanes.contains(c.id);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(0, 0, 0, 170), 1));
        p.setBrush(open ? Theme::primary : Theme::secondary);
        drawDiamond(p, k.center().x(), k.center().y(), k.width() / 2);
        p.setBrush(Qt::NoBrush);
        if (r.width() >= 56) drawCurveIcon(p, r, m_curves.contains(c.id));
    }

    // Fade-Griffe (weiße Anfasser oben an den Ecken), nur unter der Maus bzw. beim Ziehen
    const bool fading = m_drag == Drag::Fade && m_fade.clipId == c.id;
    if (!ghost && r.width() >= 24 && m_tool == Tool::Select && (c.id == m_hoverClip || fading)) {
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.setBrush(QColor(0xf4, 0xf4, 0xf4));
        for (auto edge : {TimelineOps::Edge::Start, TimelineOps::Edge::End})
            p.drawRoundedRect(QRectF(fadeHandleRect(r, c, edge)).adjusted(0.5, 0.5, -0.5, -0.5), 1.5, 1.5);
        if (fading) {
            const int frames = m_fade.edge == TimelineOps::Edge::Start ? c.fadeIn : c.fadeOut;
            const QRect hr = fadeHandleRect(r, c, m_fade.edge);
            p.restore();
            drawLabel(p, QPoint(hr.left(), r.top() + 14), Timecode::format(frames, m_editor->project()->fps()));
            return;
        }
    }
    p.restore();
}

// Filmstreifen wie in DaVinci: Kacheln ab Clip-Anfang, jede zeigt das Frame an ihrer Position.
// Die Quell-Frames werden auf Zweierpotenzen gerundet, damit beim Zoomen Bilder wiederverwendet werden.
void TimelineView::updateTrimFrames(const Timeline& preview, const TimelineOps::TrimEdit& edit, int delta)
{
    const Project* project = m_editor->project();
    TrimFrames::View v = TrimFrames::compute(
        preview, edit, delta, [project](const QString& path) { return project->mediaInfo(path); },
        [project](const Clip& c) { return project->clipName(c); });
    if (v.isNull()) { // e.g. audio-only edit: keep the normal viewer
        endTrimFrames();
        return;
    }
    if (m_trimFramesShown && v == m_trimFrames) return;
    m_trimFrames = v;
    m_trimFramesShown = true;
    emit trimFramesChanged(v);
}

void TimelineView::endTrimFrames()
{
    if (!m_trimFramesShown) return;
    m_trimFramesShown = false;
    m_trimFrames = {};
    emit trimFramesEnded();
}

void TimelineView::drawFilmstrip(QPainter& p, const QRect& body, const Clip& c)
{
    if (!m_cache || body.height() < 10) return;
    const MediaInfo* info = m_editor->project()->mediaInfo(c.mediaPath);
    const bool still = info && info->isImage;

    const int tileH = body.height();
    const int tileW = std::max(12, tileH * 16 / 9);
    const double clipX = frameToX(c.pos());
    const double framesPerTile = tileW / m_view.pxPerFrame;
    int step = 1;
    while (step * 2 <= framesPerTile) step *= 2;

    const int first = std::max(0, int((kHeaderW - clipX) / tileW));
    const int last = int((std::min(body.right(), width()) - clipX) / tileW);
    // Geschwindigkeit/Speed Ramp: Kachel zeigt das Datei-Frame, das dort gespielt wird
    const int fileLen = info ? info->length : 0;
    const std::optional<RetimeMap> map = c.isRetimed() && fileLen > 0 ? std::optional<RetimeMap>(RetimeMap(c, fileLen))
                                                                       : std::nullopt;
    for (int i = first; i <= last; ++i) {
        int src = still ? 0 : std::min(c.out, c.in + int(i * framesPerTile));
        if (map && !still)
            src = std::clamp(int(map->fileFrameAt(c.freeze ? c.stillFrame() : src)), 0, fileLen - 1);
        QImage img = m_cache->thumbnail(c.mediaPath, src / step * step, step);
        // Noch nicht fertig -> übergangsweise ein Bild aus einer gröberen Stufe
        for (int s = step * 2; img.isNull() && s <= step * 64; s *= 2)
            img = m_cache->cachedThumbnail(c.mediaPath, src / s * s);
        if (img.isNull()) continue;

        const QRect target(int(clipX) + i * tileW, body.top(), tileW, tileH);
        // Seitenverhältnis der Kachel mittig aus dem Bild ausschneiden
        QRectF source(img.rect());
        const double want = double(tileW) / tileH;
        if (source.width() / source.height() > want) {
            const double w = source.height() * want;
            source.adjust((source.width() - w) / 2, 0, -(source.width() - w) / 2, 0);
        } else {
            const double h = source.width() / want;
            source.adjust(0, (source.height() - h) / 2, 0, -(source.height() - h) / 2);
        }
        p.drawImage(target, img, source);
    }
}

void TimelineView::drawWaveform(QPainter& p, const QRect& body, const Clip& c,
                                const QVector<TimelineOps::TransitionSpan>& spans, const QColor& color)
{
    if (!m_cache) return;
    const auto wave = m_cache->waveform(c.mediaPath, c.audioStream);
    if (!wave || wave->levels.isEmpty()) return;

    constexpr int N = Waveform::kBucketsPerFrame;
    // Stufe wählen, bei der pro Pixel nur 1-2 Werte zusammengefasst werden müssen
    const double bucketsPerPx = N / m_view.pxPerFrame;
    int level = 0;
    while (level + 1 < wave->levels.size() && (1 << (level + 1)) <= bucketsPerPx) ++level;
    const QVector<quint8>& lv = wave->levels[level];
    const double scale = double(N) / (1 << level); // Buckets dieser Stufe pro Frame
    const int available = int(std::min<double>(lv.size(), std::ceil(wave->frames * scale)));
    const int clipEnd = int(std::min<double>(available, (c.out + 1) * scale));

    const bool volAnim = Keys::animated(c, AnimParam::Volume);
    if ((c.volumeDb <= kMinVolumeDb && !volAnim) || c.freeze) return; // stumm (auch Standbild) -> flach
    // Geschwindigkeit/Speed Ramp: Pixel zeigt den Ton der Datei, der dort gespielt wird
    const MediaInfo* info = m_editor->project()->mediaInfo(c.mediaPath);
    const int fileLen = info && info->length > 0 ? info->length : int(wave->frames);
    const std::optional<RetimeMap> map = c.isRetimed() ? std::optional<RetimeMap>(RetimeMap(c, fileLen)) : std::nullopt;
    // Übergänge, die diesen Clip betreffen (Crossfade/Ein-/Ausblenden)
    QVector<TimelineOps::TransitionSpan> own;
    for (const auto& s : spans)
        if (s.leftId == c.id || s.rightId == c.id) own << s;
    const bool fades = c.fadeIn > 0 || c.fadeOut > 0 || !own.isEmpty();
    const int mid = body.top() + body.height() / 2;
    const double half = body.height() / 2.0 - 1;
    const double clipX = frameToX(c.pos());
    const int x0 = std::max(body.left(), kHeaderW);
    const int x1 = std::min(body.right(), width() - 1);

    // Höhe in dB statt linear (-48 dB .. 0 dB), sonst sieht normales Material fast flach aus
    static const auto dbTable = [] {
        std::array<double, 256> t{};
        for (int i = 1; i < 256; ++i) t[i] = std::max(0.0, 1.0 + 20.0 * std::log10(i / 255.0) / 48.0);
        return t;
    }();

    QVector<QLine> lines;
    lines.reserve(x1 - x0 + 1);
    for (int x = x0; x <= x1; ++x) {
        double f0 = c.in + (x - clipX) / m_view.pxPerFrame;
        double f1 = f0 + 1 / m_view.pxPerFrame;
        int end = clipEnd;
        if (map) {
            if (f0 >= c.out + 1) continue;
            f0 = map->fileFrameAt(f0);
            f1 = map->fileFrameAt(f1);
            if (f0 > f1) std::swap(f0, f1); // rückwärts
            end = available;
        }
        const int b0 = int(std::floor(f0 * scale));
        const int b1 = std::min(end, std::max(b0 + 1, int(std::ceil(f1 * scale))));
        int peak = 0;
        for (int b = std::max(0, b0); b < b1; ++b) peak = std::max<int>(peak, lv[b]);
        if (peak == 0) continue;
        // Wellenform folgt Clip-Lautstärke, Fade-Griffen und Übergängen (dB-Skala unten)
        double db = volAnim ? volumeAt(c, (x + 0.5 - clipX) / m_view.pxPerFrame) : c.volumeDb;
        if (db <= kMinVolumeDb) continue;
        if (fades) {
            const double t = (x + 0.5 - clipX) / m_view.pxPerFrame; // Mitte des Pixels, ab Clipanfang
            double g = TimelineOps::audioFadeGain(c, t);
            for (const auto& s : own) g *= TimelineOps::audioTransitionGain(s, c.id, c.start + t);
            if (g <= 0.001) continue;
            db += 20.0 * std::log10(g);
        }
        const double v = std::min(1.0, dbTable[peak] + db / 48.0);
        if (v <= 0) continue;
        const int h = std::max(1, int(v * half));
        lines << QLine(x, mid - h, x, mid + h);
    }
    // Standard hellgrün; bei eigener Spurfarbe ein heller Ton davon
    QColor pen(0xc8, 0xf0, 0xcf, 210);
    if (color.isValid()) pen = QColor::fromHsvF(color.hsvHueF(), color.hsvSaturationF() * 0.25f, 0.94, 210 / 255.0);
    p.setPen(pen);
    p.drawLines(lines);
}

void TimelineView::drawHeaders(QPainter& p)
{
    const Timeline& tl = m_editor->project()->timeline();
    p.save();
    p.setClipRect(0, kRulerH, kHeaderW, height() - kRulerH);
    p.fillRect(QRect(0, kRulerH, kHeaderW, height() - kRulerH), Theme::window);
    for (const Row& row : rows()) {
        const Track& t = tl.track(row.ref);
        const QRect r(0, row.y, kHeaderW - 1, row.h - 1);
        p.fillRect(r, Theme::trackHeader);
        // farbige Kennung links wie in DaVinci (Spurfarbe)
        const QColor col = trackColor(t);
        p.fillRect(QRect(0, row.y, 3, row.h - 1),
                   col.isValid() ? col : row.ref.kind == TrackKind::Video ? Theme::videoClip : Theme::audioClip);
        // Kürzel im Kästchen, daneben der Name (wie DaVinci „V1  Video 1“)
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(8);
        p.setFont(f);
        // Zielspur für F9/F10 … (DaVinci Destination Control): Kästchen orange umrandet (nicht gefüllt, wie in
        // DaVinci), Klick wählt die Spur
        const QRect shortBox = shortNameRect(row);
        const bool target = row.ref.index == (row.ref.kind == TrackKind::Video ? m_editor->targetVideoTrack()
                                                                                 : m_editor->targetAudioTrack());
        p.setBrush(Qt::NoBrush);
        p.setPen(target ? Theme::primary : Theme::textFaint);
        p.drawRect(shortBox.adjusted(0, 0, -1, -1));
        p.setPen(Theme::text);
        p.drawText(shortBox, Qt::AlignCenter, trackShortName(row.ref));
        if (!(m_nameEdit && m_nameEdit->isVisible() && m_nameRef == row.ref)) {
            f.setBold(false);
            f.setPointSizeF(8.5);
            p.setFont(f);
            const QRect nr = nameRect(row);
            p.drawText(nr, Qt::AlignVCenter | Qt::AlignLeft,
                       QFontMetrics(f).elidedText(trackDisplayName(t, row.ref), Qt::ElideRight, nr.width()));
        }
        f.setBold(false);
        f.setPointSizeF(7.5);
        p.setFont(f);
        p.setPen(Theme::textDim);
        p.drawText(r.adjusted(10, 0, -6, -6), Qt::AlignBottom | Qt::AlignLeft,
                   (t.clips.size() == 1 ? T("1 Clip") : T("%1 Clips").arg(t.clips.size())));

        // Knopf: Video = Auge (ausblenden), Audio = M (stumm)
        const QRect b = headerButton(row);
        const bool active = row.ref.kind == TrackKind::Video ? t.hidden : t.muted;
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        const QColor on = row.ref.kind == TrackKind::Video ? Theme::primary : Theme::warning;
        p.setBrush(active ? on : Theme::control);
        p.drawRoundedRect(b, 3, 3);
        p.setPen(active ? Theme::readableOn(on) : Theme::text);
        f.setBold(true);
        p.setFont(f);
        p.drawText(b, Qt::AlignCenter, row.ref.kind == TrackKind::Video ? (t.hidden ? "⊘" : "◉") : "M");
        // Schloss: gesperrt = hell auf Orange
        const QRect lb = lockButton(row);
        p.setPen(Qt::NoPen);
        p.setBrush(t.locked ? Theme::primary : Theme::control);
        p.drawRoundedRect(lb, 3, 3);
        drawLock(p, lb, t.locked ? Theme::onPrimary : Theme::textDim);
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    p.restore();
}

void TimelineView::drawLabel(QPainter& p, const QPoint& topLeft, const QString& text)
{
    QFont f = font();
    f.setPointSizeF(8);
    p.setFont(f);
    const QRect box = QFontMetrics(f).boundingRect(text).adjusted(-5, -3, 5, 3);
    const QRect placed = box.translated(topLeft.x() - box.left(), topLeft.y() - box.top());
    p.fillRect(placed, Theme::alpha(Theme::well, 225));
    p.setPen(Theme::text);
    p.drawText(placed, Qt::AlignCenter, text);
}

void TimelineView::drawTrackDrop(QPainter& p)
{
    if (m_drag != Drag::TrackMove) return;
    // Orange Fläche folgt der Maus und markiert die Zielspur (ohne Einfügelinie)
    const auto row = rowFor({m_trackDragRef.kind, m_trackDropTo >= 0 ? m_trackDropTo : m_trackDragRef.index});
    if (!row) return;
    p.save();
    p.setClipRect(0, kRulerH, width(), height() - kRulerH);
    p.fillRect(QRect(0, row->y, width(), row->h), Theme::alpha(Theme::primary, 40));
    p.restore();
}

void TimelineView::drawRubber(QPainter& p)
{
    if (m_drag != Drag::Rubber) return;
    const QRect r = QRect(m_pressPos, m_rubberPos).normalized();
    if (r.width() < kDragStartPx && r.height() < kDragStartPx) return;
    p.save();
    p.setClipRect(kHeaderW, kRulerH, width() - kHeaderW, height() - kRulerH);
    p.setPen(QPen(Theme::primary, 1));
    p.setBrush(Theme::alpha(Theme::primary, 35));
    p.drawRect(r.adjusted(0, 0, -1, -1));
    p.restore();
}

void TimelineView::drawPlayhead(QPainter& p)
{
    const int x = int(frameToX(m_playhead));
    if (x < kHeaderW || x > width()) return;
    p.setPen(QPen(Theme::playhead, 1));
    p.drawLine(x, kRulerH - 8, x, height());
    // Griff im Lineal
    QPolygon handle;
    handle << QPoint(x - 6, kRulerH - 14) << QPoint(x + 6, kRulerH - 14) << QPoint(x + 6, kRulerH - 8)
           << QPoint(x, kRulerH - 2) << QPoint(x - 6, kRulerH - 8);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Theme::playhead);
    p.setPen(Qt::NoPen);
    p.drawPolygon(handle);
}
