#include "ui/timeline/TimelineView.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "ui/MediaPool.h"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <cmath>

namespace {
constexpr int kSnapPx = 8;
constexpr int kDragStartPx = 4;
} // namespace

TimelineView::TimelineView(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setMinimumHeight(160);
    setFocusPolicy(Qt::ClickFocus);

    // Projekt geändert -> nur neu zeichnen. Der ViewState bleibt unangetastet (Kernregel).
    connect(editor->project(), &Project::timelineChanged, this, [this] {
        update();
        emit viewChanged(); // nur damit die Scrollbar ggf. mehr Platz bekommt
    });
    connect(editor->selection(), &Selection::changed, this, qOverload<>(&QWidget::update));
}

// ---------- Geometrie ----------

double TimelineView::frameToX(double frame) const
{
    return kHeaderW + (frame - m_view.leftFrame) * m_view.pxPerFrame;
}

double TimelineView::xToFrame(double x) const
{
    return m_view.leftFrame + (x - kHeaderW) / m_view.pxPerFrame;
}

int TimelineView::visibleFrames() const
{
    return int(std::ceil((width() - kHeaderW) / m_view.pxPerFrame));
}

int TimelineView::scrollRangeFrames() const
{
    // Hinter dem letzten Clip ist immer Platz, und der Bereich wird nie kleiner
    // als die aktuelle Position -> nichts wird zurückgeschoben.
    const int vis = visibleFrames();
    const int end = TimelineOps::endFrame(m_editor->project()->timeline());
    return std::max(end, int(m_view.leftFrame) + vis) + vis * 2;
}

int TimelineView::contentHeight() const
{
    const Timeline& tl = m_editor->project()->timeline();
    return tl.video.size() * m_view.videoTrackHeight + kSeparator + tl.audio.size() * m_view.audioTrackHeight;
}

QVector<TimelineView::Row> TimelineView::rows() const
{
    const Timeline& tl = m_editor->project()->timeline();
    QVector<Row> out;
    int y = kRulerH - m_view.scrollY;
    for (int i = tl.video.size() - 1; i >= 0; --i) { // V1 unten, wie in DaVinci
        out << Row{{TrackKind::Video, i}, y, m_view.videoTrackHeight};
        y += m_view.videoTrackHeight;
    }
    y += kSeparator;
    for (int i = 0; i < tl.audio.size(); ++i) {
        out << Row{{TrackKind::Audio, i}, y, m_view.audioTrackHeight};
        y += m_view.audioTrackHeight;
    }
    return out;
}

std::optional<TimelineView::Row> TimelineView::rowAt(int y) const
{
    for (const Row& r : rows())
        if (y >= r.y && y < r.y + r.h) return r;
    return std::nullopt;
}

std::optional<TimelineView::Row> TimelineView::rowFor(TrackRef ref) const
{
    for (const Row& r : rows())
        if (r.ref == ref) return r;
    return std::nullopt;
}

int TimelineView::clipAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return 0;
    const auto row = rowAt(pos.y());
    if (!row) return 0;
    const double f = xToFrame(pos.x());
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips)
        if (f >= c.start && f < c.end()) return c.id;
    return 0;
}

// ---------- Snapping ----------

QVector<int> TimelineView::snapPoints(const QSet<int>& exclude) const
{
    QVector<int> pts{0, m_playhead};
    const Timeline& tl = m_editor->project()->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips)
                if (!exclude.contains(c.id)) pts << c.start << c.end();
    return pts;
}

// Liefert die Korrektur, damit eine der Kanten auf einen Snap-Punkt fällt (0 = kein Snap)
int TimelineView::snapDelta(const QVector<int>& edges, const QSet<int>& exclude) const
{
    if (!m_snap) return 0;
    const double maxFrames = kSnapPx / m_view.pxPerFrame;
    int best = 0;
    double bestDist = maxFrames + 1;
    for (int p : snapPoints(exclude)) {
        for (int e : edges) {
            const double d = std::abs(p - e);
            if (d <= maxFrames && d < bestDist) {
                bestDist = d;
                best = p - e;
            }
        }
    }
    return best;
}

// ---------- View-Steuerung (nur durch Nutzer) ----------

void TimelineView::setLeftFrame(double frame)
{
    m_view.leftFrame = std::max(0.0, frame);
    update();
    emit viewChanged();
}

void TimelineView::setScrollY(int y)
{
    m_view.scrollY = std::clamp(y, 0, std::max(0, contentHeight() - viewportHeight()));
    update();
    emit viewChanged();
}

void TimelineView::zoomBy(double factor)
{
    // Tastatur-Zoom: um den Playhead, falls sichtbar, sonst um die Mitte
    double anchorX = frameToX(m_playhead);
    if (anchorX < kHeaderW || anchorX > width()) anchorX = kHeaderW + (width() - kHeaderW) / 2.0;
    m_view.setZoomAround(m_view.pxPerFrame * factor, xToFrame(anchorX), anchorX - kHeaderW);
    update();
    emit viewChanged();
}

void TimelineView::zoomToFit()
{
    const int fps = m_editor->project()->fps();
    const int end = std::max(TimelineOps::endFrame(m_editor->project()->timeline()), fps * 10);
    m_view.pxPerFrame = std::clamp((width() - kHeaderW - 40) / double(end), ViewState::kMinPxPerFrame,
                                   ViewState::kMaxPxPerFrame);
    m_view.leftFrame = 0;
    update();
    emit viewChanged();
}

void TimelineView::setPlayhead(int frame)
{
    if (frame == m_playhead) return;
    m_playhead = frame;
    update(); // bewusst KEIN automatisches Mitscrollen
}

void TimelineView::setTool(Tool tool)
{
    if (tool == m_tool) return;
    m_tool = tool;
    setCursor(tool == Tool::Blade ? Qt::IBeamCursor : Qt::ArrowCursor);
    m_hoverFrame = -1;
    update();
    emit toolChanged(tool);
}

void TimelineView::setSnapping(bool on)
{
    if (on == m_snap) return;
    m_snap = on;
    emit snappingChanged(on);
}

// ---------- Zeichnen ----------

void TimelineView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Theme::timelineBg);
    drawTracks(p);
    drawHeaders(p);
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
        p.setPen(isMajor ? Theme::textDim : QColor(0x4a, 0x4a, 0x52));
        p.drawLine(x, kRulerH - (isMajor ? 12 : 5), x, kRulerH - 1);
        if (isMajor) {
            p.setPen(Theme::textDim);
            p.drawText(x + 3, 12, Timecode::format(fr, fps));
        }
    }
    p.restore();

    // Ecke links oben: aktueller Timecode wie in DaVinci
    p.fillRect(QRect(0, 0, kHeaderW, kRulerH), Theme::panelHeader);
    QFont tf = font();
    tf.setPointSizeF(11);
    tf.setFamily("monospace");
    p.setFont(tf);
    p.setPen(Theme::text);
    p.drawText(QRect(8, 0, kHeaderW - 8, kRulerH), Qt::AlignVCenter | Qt::AlignLeft,
               Timecode::format(m_playhead, fps));
    p.setPen(Theme::border);
    p.drawLine(kHeaderW - 1, 0, kHeaderW - 1, height());
}

void TimelineView::drawTracks(QPainter& p)
{
    const Timeline& tl = m_editor->project()->timeline();
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

        for (const Clip& c : tl.track(row.ref).clips) {
            if (moving && dragSet.contains(c.id)) continue; // wird unten verschoben gezeichnet
            const QRect r(QPoint(int(frameToX(c.start)), row.y + 1), QPoint(int(frameToX(c.end())) - 1, row.y + row.h - 3));
            if (r.right() < kHeaderW || r.left() > width()) continue;
            drawClip(p, r, c, row.ref.kind, sel.contains(c.id), false);
        }
    }

    // Verschobene Clips an der Zielposition
    if (moving) {
        for (int id : m_dragIds) {
            TrackRef ref;
            const Clip* c = TimelineOps::findClip(tl, id, &ref);
            if (!c) continue;
            if (ref.kind == m_anchorRef.kind)
                ref.index = std::clamp(ref.index + m_dragTrackDelta, 0, int(tl.tracks(ref.kind).size()) - 1);
            const auto row = rowFor(ref);
            if (!row) continue;
            Clip moved = *c;
            moved.start = c->start + m_dragDelta;
            const QRect r(QPoint(int(frameToX(moved.start)), row->y + 1),
                          QPoint(int(frameToX(moved.end())) - 1, row->y + row->h - 3));
            drawClip(p, r, moved, ref.kind, true, true);
        }
    }

    // Vorschau beim Reinziehen aus dem Media Pool
    if (m_dropFrame >= 0 && m_dropLength > 0) {
        const auto row = rowFor({TrackKind::Video, m_dropVideoTrack});
        const int x1 = int(frameToX(m_dropFrame)), x2 = int(frameToX(m_dropFrame + m_dropLength));
        if (row) {
            p.fillRect(QRect(x1, row->y + 1, x2 - x1, row->h - 3), QColor(255, 255, 255, 40));
        }
        p.setPen(QPen(Theme::accent, 1, Qt::DashLine));
        p.drawLine(x1, kRulerH, x1, height());
    }

    // Klingen-Vorschau
    if (m_tool == Tool::Blade && m_hoverFrame >= 0) {
        const int x = int(frameToX(m_hoverFrame));
        p.setPen(QPen(Theme::accent, 1));
        p.drawLine(x, kRulerH, x, height());
    }
    p.restore();
}

void TimelineView::drawClip(QPainter& p, const QRect& r, const Clip& c, TrackKind kind, bool selected, bool ghost)
{
    QColor base = kind == TrackKind::Video ? Theme::videoClip : Theme::audioClip;
    if (ghost) base.setAlpha(200);
    const QColor body = base.darker(135);

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    p.fillPath(path, body);

    // Titelleiste mit Dateiname
    const int barH = std::min(16, r.height());
    p.save();
    p.setClipPath(path);
    p.fillRect(QRect(r.left(), r.top(), r.width(), barH), base);
    if (kind == TrackKind::Audio) { // Platzhalter bis Waveforms kommen
        p.setPen(QColor(255, 255, 255, 50));
        const int mid = r.top() + barH + (r.height() - barH) / 2;
        p.drawLine(r.left(), mid, r.right(), mid);
    }
    p.restore();

    if (r.width() > 24) {
        QFont f = font();
        f.setPointSizeF(7.5);
        p.setFont(f);
        p.setPen(QColor(0xf0, 0xf0, 0xf0));
        const QRect textRect(std::max(r.left(), kHeaderW) + 5, r.top(), r.width() - 8, barH);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(f).elidedText(QFileInfo(c.mediaPath).fileName(), Qt::ElideRight, textRect.width()));
    }

    p.setPen(selected ? QPen(Theme::clipSelected, 2) : QPen(QColor(0, 0, 0, 120), 1));
    p.drawPath(path);
    p.restore();
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
        // farbige Kennung links wie in DaVinci
        p.fillRect(QRect(0, row.y, 3, row.h - 1),
                   row.ref.kind == TrackKind::Video ? Theme::videoClip : Theme::audioClip);
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(Theme::text);
        p.drawText(r.adjusted(10, 6, 0, 0), Qt::AlignTop | Qt::AlignLeft, t.name);
        f.setBold(false);
        f.setPointSizeF(7.5);
        p.setFont(f);
        p.setPen(Theme::textDim);
        p.drawText(r.adjusted(10, 0, -6, -6), Qt::AlignBottom | Qt::AlignLeft,
                   QString("%1 Clip%2").arg(t.clips.size()).arg(t.clips.size() == 1 ? "" : "s"));
    }
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

// ---------- Maus ----------

void TimelineView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const QPoint pos = e->position().toPoint();
    m_pressPos = pos;

    if (pos.x() < kHeaderW) return; // Spurköpfe: später Mute/Solo/Lock
    if (pos.y() < kRulerH) {
        m_drag = Drag::Scrub;
        emit seekRequested(std::max(0, int(std::lround(xToFrame(pos.x())))));
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
    if (!id) {
        if (!(e->modifiers() & Qt::ControlModifier)) sel->clear();
        return;
    }
    const QVector<int> group = m_editor->withLinked({id});
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
    if (!ids.contains(id)) sel->set(QSet<int>(group.begin(), group.end()));

    TimelineOps::findClip(m_editor->project()->timeline(), id, &m_anchorRef);
    m_dragIds = sel->ids().values().toVector();
    m_dragDelta = 0;
    m_dragTrackDelta = 0;
    m_drag = Drag::MaybeMove;
}

void TimelineView::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    switch (m_drag) {
    case Drag::Scrub:
        emit seekRequested(std::max(0, int(std::lround(xToFrame(pos.x())))));
        return;
    case Drag::MaybeMove:
        if ((pos - m_pressPos).manhattanLength() < kDragStartPx) return;
        m_drag = Drag::Move;
        [[fallthrough]];
    case Drag::Move: {
        const Timeline& tl = m_editor->project()->timeline();
        int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
        QVector<int> edges;
        int minStart = INT_MAX;
        for (int id : m_dragIds) {
            if (const Clip* c = TimelineOps::findClip(tl, id)) {
                edges << c->start + delta << c->end() + delta;
                minStart = std::min(minStart, c->start);
            }
        }
        const QSet<int> exclude(m_dragIds.begin(), m_dragIds.end());
        delta += snapDelta(edges, exclude);
        if (minStart != INT_MAX) delta = std::max(delta, -minStart);
        m_dragDelta = delta;

        m_dragTrackDelta = 0;
        if (const auto row = rowAt(pos.y()); row && row->ref.kind == m_anchorRef.kind)
            m_dragTrackDelta = row->ref.index - m_anchorRef.index;
        update();
        return;
    }
    case Drag::None:
        break;
    }

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
    if (m_drag == Drag::Move)
        m_editor->moveClips(m_dragIds, m_dragDelta, m_anchorRef.kind, m_dragTrackDelta);
    m_drag = Drag::None;
    m_dragIds.clear();
    update();
}

void TimelineView::leaveEvent(QEvent*)
{
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
    case WheelAction::TrackHeight:
        m_view.videoTrackHeight = std::clamp(m_view.videoTrackHeight + int(steps * 6), 28, 220);
        m_view.audioTrackHeight = std::clamp(m_view.audioTrackHeight + int(steps * 6), 28, 220);
        setScrollY(m_view.scrollY);
        break;
    case WheelAction::None:
        e->ignore();
        return;
    }
    e->accept();
}

// ---------- Drag & Drop aus dem Media Pool ----------

void TimelineView::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(MediaPool::MimeType)) e->acceptProposedAction();
}

void TimelineView::dragMoveEvent(QDragMoveEvent* e)
{
    const QString path = QString::fromUtf8(e->mimeData()->data(MediaPool::MimeType));
    const MediaInfo* info = m_editor->project()->mediaInfo(path);
    if (!info) return;
    const QPoint pos = e->position().toPoint();
    int frame = std::max(0, int(std::lround(xToFrame(pos.x()))));
    frame += snapDelta({frame, frame + info->length}, {});
    m_dropFrame = std::max(0, frame);
    m_dropLength = info->length;
    const auto row = rowAt(pos.y());
    m_dropVideoTrack = (row && row->ref.kind == TrackKind::Video) ? row->ref.index : 0;
    e->acceptProposedAction();
    update();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropFrame = -1;
    update();
}

void TimelineView::dropEvent(QDropEvent* e)
{
    const QString path = QString::fromUtf8(e->mimeData()->data(MediaPool::MimeType));
    if (m_dropFrame >= 0) m_editor->addMediaAt(path, m_dropFrame, m_dropVideoTrack);
    m_dropFrame = -1;
    e->acceptProposedAction();
    update();
}

void TimelineView::resizeEvent(QResizeEvent*)
{
    emit viewChanged();
}
