// Timeline: subtitle tracks (layout, drawing, headers, menus, mouse).
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

// ---------- Untertitelspuren ----------

int TimelineView::subtitlesHeight() const
{
    const int n = m_editor->project()->timeline().subtitles.size();
    return n ? n * kSubtitleTrackH + kSeparator : 0;
}

QVector<TimelineView::SubRow> TimelineView::subRows() const
{
    QVector<SubRow> out;
    int y = kRulerH - m_view.scrollY;
    for (int i = m_editor->project()->timeline().subtitles.size() - 1; i >= 0; --i) { // ST1 unten
        out << SubRow{i, y, kSubtitleTrackH};
        y += kSubtitleTrackH;
    }
    return out;
}

std::optional<TimelineView::SubRow> TimelineView::subRowAt(int y) const
{
    if (y < kRulerH) return std::nullopt;
    for (const SubRow& r : subRows())
        if (y >= r.y && y < r.y + r.h) return r;
    return std::nullopt;
}

QRect TimelineView::cueRect(const SubRow& row, const SubtitleCue& c) const
{
    const int x0 = int(std::floor(frameToX(c.start))), x1 = int(std::floor(frameToX(c.end)));
    return QRect(x0, row.y + 3, std::max(2, x1 - x0), row.h - 6);
}

int TimelineView::cueAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW) return 0;
    const auto row = subRowAt(pos.y());
    if (!row) return 0;
    const SubtitleTrack& t = m_editor->project()->timeline().subtitles[row->index];
    if (t.locked) return 0;
    for (const SubtitleCue& c : t.cues)
        if (cueRect(*row, c).contains(pos)) return c.id;
    return 0;
}

std::optional<TimelineView::EdgeHit> TimelineView::cueEdgeAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || m_tool == Tool::Blade) return std::nullopt;
    const auto row = subRowAt(pos.y());
    if (!row) return std::nullopt;
    const SubtitleTrack& t = m_editor->project()->timeline().subtitles[row->index];
    if (t.locked) return std::nullopt;
    for (const SubtitleCue& c : t.cues) {
        const QRect r = cueRect(*row, c);
        const int grab = std::min(kEdgeGrabPx, std::max(1, r.width() / 3));
        if (std::abs(pos.x() - r.left()) <= grab) return EdgeHit{c.id, TimelineOps::Edge::Start};
        if (std::abs(pos.x() - r.right()) <= grab) return EdgeHit{c.id, TimelineOps::Edge::End};
    }
    return std::nullopt;
}

QRect TimelineView::subNameRect(const SubRow& row) const
{
    const int left = 42;
    return QRect(left, row.y + (row.h - 18) / 2, kHeaderW - 54 - 4 - left, 18);
}

void TimelineView::drawSubtitleTracks(QPainter& p)
{
    // während Ripple-Trimmen rücken die Untertitel in der Vorschau mit (wie die Clip-Spuren)
    const bool trimEdit = m_drag == Drag::TrimEdit && m_trimPreview;
    const Timeline& tl = trimEdit ? *m_trimPreview : m_editor->project()->timeline();
    if (tl.subtitles.isEmpty()) return;
    const auto& sel = m_editor->selection()->ids();
    const QSet<int> dragging(m_cueIds.begin(), m_cueIds.end());
    p.save();
    p.setClipRect(kHeaderW, kRulerH, width() - kHeaderW, height() - kRulerH);
    QFont f = font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    const QFontMetrics fm(f);

    auto drawCue = [&](const QRect& r, const SubtitleCue& c, bool selected, bool dim) {
        if (r.right() < kHeaderW || r.left() > width()) return;
        QColor base = Theme::subtitleClip;
        if (dim) base = base.darker(150);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        p.fillPath(path, base);
        if (r.width() > 12) {
            const QRect text(std::max(r.left(), kHeaderW) + 5, r.top(), r.right() - std::max(r.left(), kHeaderW) - 8, r.height());
            if (text.width() > 4) {
                p.setPen(QColor(0xf0, 0xf0, 0xf0));
                const QString line = QString(c.text).replace('\n', QLatin1String(" / "));
                p.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(line, Qt::ElideRight, text.width()));
            }
        }
        p.setPen(selected ? QPen(Theme::clipSelected, 2) : QPen(QColor(0, 0, 0, 120), 1));
        p.drawPath(path);
        p.setRenderHint(QPainter::Antialiasing, false);
    };

    const bool moving = m_drag == Drag::CueMove;
    for (const SubRow& row : subRows()) {
        const SubtitleTrack& t = tl.subtitles[row.index];
        p.fillRect(QRect(kHeaderW, row.y, width() - kHeaderW, row.h), row.index % 2 ? Theme::trackBgAlt : Theme::trackBg);
        p.setPen(Theme::border);
        p.drawLine(kHeaderW, row.y + row.h - 1, width(), row.y + row.h - 1);
        p.setOpacity(t.enabled ? 1.0 : 0.45);
        for (SubtitleCue c : t.cues) {
            if (moving && dragging.contains(c.id)) continue; // wird unten verschoben gezeichnet
            if (m_drag == Drag::CueTrim && c.id == m_cueTrim.clipId)
                (m_cueTrim.edge == TimelineOps::Edge::Start ? c.start : c.end) += m_cueDelta;
            drawCue(cueRect(row, c), c, sel.contains(c.id), t.locked);
        }
        p.setOpacity(1.0);
    }
    if (moving) {
        const auto all = subRows();
        for (int id : m_cueIds) {
            int ti = -1;
            const SubtitleCue* c = Subtitles::find(tl, id, &ti);
            if (!c) continue;
            const int target = ti + m_cueTrackDelta;
            for (const SubRow& row : all)
                if (row.index == target) {
                    SubtitleCue moved = *c;
                    moved.start += m_cueDelta;
                    moved.end += m_cueDelta;
                    drawCue(cueRect(row, moved), moved, true, false);
                }
        }
    }
    // Trennfuge zu den Videospuren
    const int sepY = kRulerH - m_view.scrollY + tl.subtitles.size() * kSubtitleTrackH;
    p.fillRect(QRect(kHeaderW, sepY, width() - kHeaderW, kSeparator), Theme::timelineBg);
    p.restore();
}

void TimelineView::drawSubtitleHeaders(QPainter& p)
{
    const Timeline& tl = m_editor->project()->timeline();
    if (tl.subtitles.isEmpty()) return;
    p.save();
    p.setClipRect(0, kRulerH, kHeaderW, height() - kRulerH);
    for (const SubRow& row : subRows()) {
        const SubtitleTrack& t = tl.subtitles[row.index];
        const QRect r(0, row.y, kHeaderW - 1, row.h - 1);
        p.fillRect(r, Theme::trackHeader);
        p.fillRect(QRect(0, row.y, 3, row.h - 1), Theme::subtitleClip);
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(8);
        p.setFont(f);
        const QRect shortBox(10, row.y + (row.h - 16) / 2, 26, 16);
        p.setPen(Theme::textFaint);
        p.setBrush(Qt::NoBrush);
        p.drawRect(shortBox.adjusted(0, 0, -1, -1));
        p.setPen(Theme::text);
        p.drawText(shortBox, Qt::AlignCenter, QString("ST%1").arg(row.index + 1));
        if (!(m_nameEdit && m_nameEdit->isVisible() && m_nameSub == row.index)) {
            f.setBold(false);
            f.setPointSizeF(8.5);
            p.setFont(f);
            const QRect nr = subNameRect(row);
            const QString name = subtitleTrackDisplayName(t, row.index);
            p.drawText(nr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(f).elidedText(name, Qt::ElideRight, nr.width()));
        }
        // Auge (sichtbar) und Schloss wie bei den anderen Spuren; Auge an = orange
        const QRect b(kHeaderW - 30, row.y + (row.h - 16) / 2, 20, 16);
        const QRect lb(kHeaderW - 54, b.top(), 20, 16);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(t.enabled ? Theme::primary : Theme::control);
        p.drawRoundedRect(b, 3, 3);
        p.setPen(t.enabled ? Theme::onPrimary : Theme::text);
        f.setBold(true);
        f.setPointSizeF(7.5);
        p.setFont(f);
        p.drawText(b, Qt::AlignCenter, t.enabled ? "◉" : "⊘");
        p.setPen(Qt::NoPen);
        p.setBrush(t.locked ? Theme::primary : Theme::control);
        p.drawRoundedRect(lb, 3, 3);
        drawLock(p, lb, t.locked ? Theme::onPrimary : Theme::textDim);
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    p.restore();
}

void TimelineView::subtitleHeaderMenu(int index, const QPoint& globalPos)
{
    const Timeline& tl = m_editor->project()->timeline();
    if (index < 0 || index >= tl.subtitles.size()) return;
    const SubtitleTrack& t = tl.subtitles[index];
    QMenu menu(this);
    QAction* show = menu.addAction(T("Untertitelspur einblenden"));
    show->setCheckable(true);
    show->setChecked(t.enabled);
    connect(show, &QAction::triggered, this, [this, index](bool on) { m_editor->setSubtitleTrackEnabled(index, on); });
    QAction* lock = menu.addAction(T("Spur sperren"));
    lock->setCheckable(true);
    lock->setChecked(t.locked);
    connect(lock, &QAction::triggered, this, [this, index] { m_editor->toggleSubtitleTrackLock(index); });
    connect(menu.addAction(T("Spur umbenennen")), &QAction::triggered, this, [this, index] { startSubtitleRename(index); });
    menu.addSeparator();
    QAction* add = menu.addAction(T("Untertitel hinzufügen"));
    add->setEnabled(!t.locked);
    connect(add, &QAction::triggered, this, [this, index] { m_editor->addSubtitle(m_playhead, index); });
    connect(menu.addAction(T("Untertitelspur hinzufügen")), &QAction::triggered, this, [this] { m_editor->addSubtitleTrack(); });
    connect(menu.addAction(T("Spur löschen")), &QAction::triggered, this, [this, index] { m_editor->removeSubtitleTrack(index); });
    menu.exec(globalPos);
}

void TimelineView::subtitleTrackMenu(const QPoint& pos, const QPoint& globalPos)
{
    const auto row = subRowAt(pos.y());
    if (!row) return;
    const int frame = std::max(0, int(std::floor(xToFrame(pos.x()))));
    QMenu menu(this);
    if (const int id = cueAt(pos)) {
        Selection* sel = m_editor->selection();
        if (!sel->contains(id)) sel->set({id});
        connect(menu.addAction(T("Text bearbeiten")), &QAction::triggered, this, [this, id] { emit subtitleEditRequested(id); });
        connect(menu.addAction(T("Löschen")), &QAction::triggered, this, [this] { m_editor->deleteSelection(); });
    } else {
        QAction* add = menu.addAction(T("Untertitel hier hinzufügen"));
        add->setEnabled(!m_editor->isSubtitleTrackLocked(row->index));
        connect(add, &QAction::triggered, this, [this, frame, index = row->index] { m_editor->addSubtitle(frame, index); });
    }
    menu.exec(globalPos);
}

void TimelineView::startSubtitleRename(int index)
{
    std::optional<SubRow> row;
    for (const SubRow& r : subRows())
        if (r.index == index) row = r;
    if (!row) return;
    startRename({TrackKind::Video, -1}); // Eingabefeld anlegen (ohne Zeile: tut sonst nichts)
    if (!m_nameEdit) return;
    m_nameSub = index;
    m_renaming = true;
    QFont f = font();
    f.setPointSizeF(8.5);
    m_nameEdit->setFont(f);
    m_nameEdit->setText(subtitleTrackDisplayName(m_editor->project()->timeline().subtitles[index], index));
    m_nameEdit->setGeometry(subNameRect(*row).adjusted(-3, 0, 0, 0));
    m_nameEdit->show();
    m_nameEdit->selectAll();
    m_nameEdit->setFocus();
    update();
}

bool TimelineView::subtitlePress(QMouseEvent* e, const QPoint& pos)
{
    const auto row = subRowAt(pos.y());
    if (!row) return false;
    if (pos.x() < kHeaderW) { // Spurkopf: Auge, Schloss
        const QRect b(kHeaderW - 30, row->y + (row->h - 16) / 2, 20, 16);
        const QRect lb(kHeaderW - 54, b.top(), 20, 16);
        const SubtitleTrack& t = m_editor->project()->timeline().subtitles[row->index];
        if (b.contains(pos)) m_editor->setSubtitleTrackEnabled(row->index, !t.enabled);
        else if (lb.contains(pos)) m_editor->toggleSubtitleTrackLock(row->index);
        return true;
    }
    Selection* sel = m_editor->selection();
    if (m_tool == Tool::Blade) { // Eintrag an der Klinge teilen (wie Clips, mit Snapping)
        if (const int id = cueAt(pos)) {
            int frame = int(std::lround(xToFrame(pos.x())));
            frame += snapDelta({frame}, {});
            m_editor->bladeAt(id, frame);
        }
        return true;
    }
    if (const auto edge = cueEdgeAt(pos)) {
        if (!sel->contains(edge->clipId)) sel->set({edge->clipId});
        m_cueTrim = *edge;
        m_cueDelta = 0;
        m_drag = Drag::CueTrim;
        update();
        return true;
    }
    const int id = cueAt(pos);
    if (!id) {
        if (m_tool == Tool::Select) startRubber(e->modifiers());
        else if (!(e->modifiers() & Qt::ControlModifier)) sel->clear();
        return true;
    }
    QSet<int> ids = sel->ids();
    if (e->modifiers() & Qt::ControlModifier) {
        if (ids.contains(id)) ids.remove(id);
        else ids.insert(id);
        sel->set(ids);
        return true;
    }
    if (!ids.contains(id)) sel->set({id});
    m_cueIds = m_editor->selectedSubtitles();
    m_cueAnchor = row->index;
    m_cueDelta = 0;
    m_cueTrackDelta = 0;
    m_drag = Drag::CueMaybeMove;
    return true;
}
