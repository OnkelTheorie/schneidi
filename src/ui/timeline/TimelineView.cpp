#include "ui/timeline/TimelineView.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/MediaCache.h"
#include "ui/MediaPool.h"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <array>
#include <cmath>

namespace {
constexpr int kSnapPx = 8;
constexpr int kDragStartPx = 4;
constexpr int kEdgeGrabPx = 6; // so nah an der Clipkante wird getrimmt statt verschoben
constexpr int kVolumeGrabPx = 4;
constexpr int kClipBarH = 16;  // Titelleiste im Clip

// Lautstärke <-> Höhe im Clip (0 = unten, 1 = oben), stückweise linear wie ein Fader:
// unteres Viertel -∞..-20 dB, Mitte -20..0 dB, oberes Viertel 0..+12 dB
double volumeToPos(double db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    if (db < -20) return 0.25 * (db - kMinVolumeDb) / (-20 - kMinVolumeDb);
    if (db < 0) return 0.25 + 0.5 * (db + 20) / 20;
    return 0.75 + 0.25 * db / kMaxVolumeDb;
}

double posToVolume(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    if (t < 0.25) return kMinVolumeDb + t / 0.25 * (-20 - kMinVolumeDb);
    if (t < 0.75) return -20 + (t - 0.25) / 0.5 * 20;
    return (t - 0.75) / 0.25 * kMaxVolumeDb;
}

QRect clipBodyRect(const QRect& r)
{
    const int barH = std::min(kClipBarH, r.height());
    return QRect(r.left(), r.top() + barH, r.width(), r.height() - barH);
}

int volumeLineY(const QRect& body, double db)
{
    return body.bottom() - int(std::lround(volumeToPos(db) * (body.height() - 1)));
}
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

void TimelineView::setMediaCache(MediaCache* cache)
{
    m_cache = cache;
    // Neue Bilder/Wellenformen fertig -> neu zeichnen (Qt fasst mehrere update() zusammen)
    connect(cache, &MediaCache::updated, this, qOverload<>(&QWidget::update));
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

// Kante unter der Maus (nur innerhalb des Clips, wie in DaVinci). Bei schmalen Clips
// wird der Greifbereich kleiner, damit man den Clip noch verschieben kann.
std::optional<TimelineView::EdgeHit> TimelineView::edgeAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row) return std::nullopt;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const double x1 = frameToX(c.start), x2 = frameToX(c.end());
        if (pos.x() < x1 || pos.x() >= x2) continue;
        const double grab = std::min<double>(kEdgeGrabPx, (x2 - x1) / 3);
        if (pos.x() < x1 + grab) return EdgeHit{c.id, TimelineOps::Edge::Start};
        if (pos.x() >= x2 - grab) return EdgeHit{c.id, TimelineOps::Edge::End};
        return std::nullopt;
    }
    return std::nullopt;
}

QRect TimelineView::clipRect(const Row& row, const Clip& c) const
{
    return QRect(QPoint(int(frameToX(c.start)), row.y + 1), QPoint(int(frameToX(c.end())) - 1, row.y + row.h - 3));
}

QRect TimelineView::transitionRect(const Row& row, const TimelineOps::TransitionSpan& s) const
{
    return QRect(QPoint(int(frameToX(s.start)), row.y + 1), QPoint(int(frameToX(s.end)) - 1, row.y + row.h - 3));
}

// Übergänge liegen über den Clipkanten und gehen beim Klicken vor (wie DaVinci)
std::optional<TimelineView::TransitionHit> TimelineView::transitionAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row) return std::nullopt;
    for (const auto& s : m_editor->transitions(row->ref)) {
        const QRect r = transitionRect(*row, s);
        if (pos.x() < r.left() || pos.x() > r.right()) continue;
        const int grab = std::min(kEdgeGrabPx, r.width() / 3);
        int edge = 0;
        // Nur die freie Kante ist ziehbar: Einblenden rechts, Ausblenden links, Überblendung beide
        if (s.leftId && pos.x() <= r.left() + grab) edge = -1;
        else if (s.rightId && pos.x() >= r.right() - grab) edge = 1;
        return TransitionHit{row->ref, s, edge};
    }
    return std::nullopt;
}

QRect TimelineView::fadeHandleRect(const QRect& r, const Clip& c, TimelineOps::Edge edge) const
{
    constexpr int w = 7, h = 9;
    if (edge == TimelineOps::Edge::Start) {
        const int x = int(r.left() + c.fadeIn * m_view.pxPerFrame);
        return QRect(std::clamp(x - w / 2, r.left() + 1, std::max(r.left() + 1, r.right() - w)), r.top() + 1, w, h);
    }
    const int x = int(r.right() - c.fadeOut * m_view.pxPerFrame);
    return QRect(std::clamp(x - w / 2, r.left() + 1, std::max(r.left() + 1, r.right() - w)), r.top() + 1, w, h);
}

std::optional<TimelineView::EdgeHit> TimelineView::fadeHandleAt(const QPoint& pos) const
{
    if (m_tool != Tool::Select || !m_hoverClip || pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row) return std::nullopt;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        if (c.id != m_hoverClip) continue;
        const QRect r = clipRect(*row, c);
        if (r.width() < 24) return std::nullopt; // zu schmal: Griffe würden das Trimmen verdecken
        for (auto edge : {TimelineOps::Edge::Start, TimelineOps::Edge::End})
            if (fadeHandleRect(r, c, edge).adjusted(-3, -2, 3, 3).contains(pos)) return EdgeHit{c.id, edge};
    }
    return std::nullopt;
}

int TimelineView::volumeLineAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return 0;
    const auto row = rowAt(pos.y());
    if (!row || row->ref.kind != TrackKind::Audio) return 0;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const QRect r = clipRect(*row, c);
        if (pos.x() < r.left() || pos.x() > r.right()) continue;
        const QRect body = clipBodyRect(r);
        if (body.height() < 6) return 0;
        return std::abs(pos.y() - volumeLineY(body, c.volumeDb)) <= kVolumeGrabPx ? c.id : 0;
    }
    return 0;
}

void TimelineView::updateHoverCursor(const QPoint& pos)
{
    const int hover = m_tool == Tool::Select && pos.x() >= kHeaderW && pos.y() >= kRulerH ? clipAt(pos) : 0;
    if (hover != m_hoverClip) {
        m_hoverClip = hover;
        update();
    }
    if (fadeHandleAt(pos)) {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    if (m_tool == Tool::Select) {
        if (const auto t = transitionAt(pos)) {
            if (m_hoverVolClip) {
                m_hoverVolClip = 0;
                update();
            }
            setCursor(t->edge ? Qt::SizeHorCursor : Qt::ArrowCursor);
            return;
        }
    }
    const int vol = m_tool == Tool::Select && !edgeAt(pos) ? volumeLineAt(pos) : 0;
    if (vol != m_hoverVolClip) {
        m_hoverVolClip = vol;
        update();
    }
    if (m_tool != Tool::Select) return;
    setCursor(edgeAt(pos) ? Qt::SizeHorCursor : vol ? Qt::SizeVerCursor : Qt::ArrowCursor);
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
    // In/Out-Bereich (I/O): heller Balken im Lineal mit Klammern an den Enden
    const Timeline& tlm = m_editor->project()->timeline();
    if (tlm.markIn >= 0 || tlm.markOut >= 0) {
        const double xIn = tlm.markIn >= 0 ? frameToX(tlm.markIn) : kHeaderW - 1;
        const double xOut = tlm.markOut >= 0 ? frameToX(tlm.markOut + 1) : width() + 1;
        p.fillRect(QRectF(xIn, kRulerH - 16, xOut - xIn, 16), QColor(255, 255, 255, 40));
        p.setPen(QPen(Theme::text, 2));
        if (tlm.markIn >= 0) {
            p.drawLine(QPointF(xIn + 1, kRulerH - 16), QPointF(xIn + 1, kRulerH - 1));
            p.drawLine(QPointF(xIn + 1, kRulerH - 15), QPointF(xIn + 5, kRulerH - 15));
        }
        if (tlm.markOut >= 0) {
            p.drawLine(QPointF(xOut - 1, kRulerH - 16), QPointF(xOut - 1, kRulerH - 1));
            p.drawLine(QPointF(xOut - 1, kRulerH - 15), QPointF(xOut - 5, kRulerH - 15));
        }
    }
    // Marker (M) als kleine blaue Fähnchen wie in DaVinci
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
        p.fillPath(flag, QColor(0x3d, 0x8e, 0xe0));
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

        const Track& track = tl.track(row.ref);
        p.setOpacity(track.muted || track.hidden ? 0.4 : 1.0);
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
            if (m_drag == Drag::Volume && c.id == m_volClipId) c.volumeDb = m_volDb;
            const QRect r = clipRect(row, c);
            if (r.right() < kHeaderW || r.left() > width()) continue;
            drawClip(p, r, c, row.ref.kind, sel.contains(c.id), false);
        }
        QSet<int> hidden; // Übergänge von Clips, die gerade gezogen/getrimmt werden, ausblenden
        if (moving) hidden = dragSet;
        if (m_drag == Drag::Trim) hidden = QSet<int>(m_trimIds.begin(), m_trimIds.end());
        drawTransitions(p, row, hidden);
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
            moved.start = c->start + m_dragDelta;
            const QRect r(QPoint(int(frameToX(moved.start)), row->y + 1),
                          QPoint(int(frameToX(moved.end())) - 1, row->y + row->h - 3));
            drawClip(p, r, moved, ref.kind, true, true);
        }
    }

    // Vorschau beim Reinziehen: Video auf V[n], Audio auf A[n]
    if (m_dropFrame >= 0 && !m_dropItems.isEmpty()) {
        auto ghostRow = [&](TrackKind kind) -> std::optional<Row> {
            if (auto r = rowFor({kind, m_dropTrack})) return r;
            if (kind == TrackKind::Audio && !allRows.isEmpty()) { // neue Spur unter der letzten
                const Row& last = allRows.last();
                return Row{{kind, m_dropTrack}, last.y + last.h, m_view.audioTrackHeight};
            }
            return std::nullopt;
        };
        const auto vRow = ghostRow(TrackKind::Video);
        const auto aRow = ghostRow(TrackKind::Audio);
        int start = m_dropFrame;
        for (const DropItem& it : m_dropItems) {
            const int x1 = int(frameToX(start)), x2 = int(frameToX(start + it.length));
            if (it.video && vRow) p.fillRect(QRect(x1, vRow->y + 1, x2 - x1, vRow->h - 3), QColor(0x3b, 0x6a, 0xa0, 150));
            if (it.audio && aRow) p.fillRect(QRect(x1, aRow->y + 1, x2 - x1, aRow->h - 3), QColor(0x3c, 0x86, 0x4c, 150));
            start += it.length;
        }
        p.setPen(QPen(Theme::accent, 1, Qt::DashLine));
        const int x = int(frameToX(m_dropFrame));
        p.drawLine(x, kRulerH, x, height());
    }

    // Trimmen: Kante markieren + Versatz anzeigen (wie DaVinci)
    if (m_drag == Drag::Trim) {
        if (const Clip* c = TimelineOps::findClip(tl, m_trim.clipId)) {
            const int edgeFrame = m_trim.edge == TimelineOps::Edge::Start ? c->start + m_trimDelta : c->end() + m_trimDelta;
            const int x = int(frameToX(edgeFrame));
            p.setPen(QPen(Theme::accent, 1));
            p.drawLine(x, kRulerH, x, height());
            drawLabel(p, QPoint(x + 8, kRulerH + 6),
                      (m_trimDelta >= 0 ? "+" : "") + Timecode::format(m_trimDelta, m_editor->project()->fps()));
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
        p.setPen(QPen(Theme::accent, 1));
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
        if (r.width() > 80 && r.height() > 14) {
            QFont f = font();
            f.setPointSizeF(7);
            p.setFont(f);
            p.setPen(QColor(0x10, 0x10, 0x10));
            const QString name = row.ref.kind == TrackKind::Video ? QString(transitionTypeInfo(s.style.type).name)
                                                                  : QStringLiteral("Cross Fade +3 dB");
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

void TimelineView::drawClip(QPainter& p, const QRect& r, const Clip& c, TrackKind kind, bool selected, bool ghost)
{
    QColor base = c.isTitle() ? Theme::titleClip : kind == TrackKind::Video ? Theme::videoClip : Theme::audioClip;
    if (!c.enabled) base = QColor(0x55, 0x55, 0x5c); // deaktiviert (D) wie DaVinci: grau
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
    if (!ghost && bodyRect.height() > 4) {
        p.setRenderHint(QPainter::Antialiasing, false);
        if (kind == TrackKind::Video && !c.isTitle()) drawFilmstrip(p, bodyRect, c); // Titel: nur Farbe
        else if (kind == TrackKind::Audio) drawWaveform(p, bodyRect, c);
        p.setRenderHint(QPainter::Antialiasing);
    }
    // Lautstärkelinie wie in DaVinci (zum Hoch-/Runterziehen)
    if (!ghost && kind == TrackKind::Audio && bodyRect.height() >= 6) {
        const bool active = c.id == m_hoverVolClip || (m_drag == Drag::Volume && c.id == m_volClipId);
        const int y = volumeLineY(bodyRect, c.volumeDb);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(QColor(0, 0, 0, 140), 1)); // Schatten, damit die Linie auf der Wellenform lesbar bleibt
        p.drawLine(bodyRect.left(), y + (active ? 2 : 1), bodyRect.right(), y + (active ? 2 : 1));
        p.setPen(QPen(active ? QColor(0xff, 0xff, 0xff) : QColor(0xff, 0xff, 0xff, 190), active ? 2 : 1));
        p.drawLine(bodyRect.left(), y, bodyRect.right(), y);
        p.setRenderHint(QPainter::Antialiasing);
    }
    // Fade-Bereiche wie DaVinci: Fläche über der Rampe abgedunkelt, Linie von unten nach oben
    if (!ghost && (c.fadeIn > 0 || c.fadeOut > 0)) {
        const int top = r.top() + barH, bottom = r.bottom();
        const int fi = std::min(c.fadeIn, c.length()), fo = std::min(c.fadeOut, c.length() - fi);
        auto ramp = [&](double x0, double x1, bool in) {
            QPolygonF shade;
            if (in) shade << QPointF(x0, top) << QPointF(x1, top) << QPointF(x0, bottom);
            else shade << QPointF(x0, top) << QPointF(x1, top) << QPointF(x1, bottom);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawPolygon(shade);
            p.setPen(QPen(QColor(0xff, 0xff, 0xff, 170), 1));
            if (in) p.drawLine(QPointF(x0, bottom), QPointF(x1, top));
            else p.drawLine(QPointF(x0, top), QPointF(x1, bottom));
        };
        if (fi > 0) ramp(r.left(), r.left() + fi * m_view.pxPerFrame, true);
        if (fo > 0) ramp(r.right() + 1 - fo * m_view.pxPerFrame, r.right() + 1, false);
        p.setBrush(Qt::NoBrush);
    }
    p.restore();

    if (r.width() > 24) {
        QFont f = font();
        f.setPointSizeF(7.5);
        p.setFont(f);
        p.setPen(QColor(0xf0, 0xf0, 0xf0));
        const QRect textRect(std::max(r.left(), kHeaderW) + 5, r.top(), r.width() - 8, barH);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(f).elidedText(c.displayName(), Qt::ElideRight, textRect.width()));
    }

    p.setPen(selected ? QPen(Theme::clipSelected, 2) : QPen(QColor(0, 0, 0, 120), 1));
    p.drawPath(path);

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
void TimelineView::drawFilmstrip(QPainter& p, const QRect& body, const Clip& c)
{
    if (!m_cache || body.height() < 10) return;
    const MediaInfo* info = m_editor->project()->mediaInfo(c.mediaPath);
    const bool still = info && info->isImage;

    const int tileH = body.height();
    const int tileW = std::max(12, tileH * 16 / 9);
    const double clipX = frameToX(c.start);
    const double framesPerTile = tileW / m_view.pxPerFrame;
    int step = 1;
    while (step * 2 <= framesPerTile) step *= 2;

    const int first = std::max(0, int((kHeaderW - clipX) / tileW));
    const int last = int((std::min(body.right(), width()) - clipX) / tileW);
    for (int i = first; i <= last; ++i) {
        const int src = still ? 0 : std::min(c.out, c.in + int(i * framesPerTile));
        QImage img = m_cache->thumbnail(c.mediaPath, src / step * step);
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

void TimelineView::drawWaveform(QPainter& p, const QRect& body, const Clip& c)
{
    if (!m_cache) return;
    const auto wave = m_cache->waveform(c.mediaPath);
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

    if (c.volumeDb <= kMinVolumeDb) return; // stumm -> flach
    const double gain = c.volumeDb / 48.0; // Wellenform folgt der Clip-Lautstärke (dB-Skala unten)
    const int mid = body.top() + body.height() / 2;
    const double half = body.height() / 2.0 - 1;
    const double clipX = frameToX(c.start);
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
        const double f0 = c.in + (x - clipX) / m_view.pxPerFrame;
        const int b0 = int(std::floor(f0 * scale));
        const int b1 = std::min(clipEnd, std::max(b0 + 1, int(std::ceil((f0 + 1 / m_view.pxPerFrame) * scale))));
        int peak = 0;
        for (int b = std::max(0, b0); b < b1; ++b) peak = std::max<int>(peak, lv[b]);
        if (peak == 0) continue;
        const double v = std::min(1.0, dbTable[peak] + gain);
        if (v <= 0) continue;
        const int h = std::max(1, int(v * half));
        lines << QLine(x, mid - h, x, mid + h);
    }
    p.setPen(QColor(0xc8, 0xf0, 0xcf, 210));
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

        // Knopf: Video = Auge (ausblenden), Audio = M (stumm)
        const QRect b = headerButton(row);
        const bool active = row.ref.kind == TrackKind::Video ? t.hidden : t.muted;
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(active ? (row.ref.kind == TrackKind::Video ? Theme::accent : QColor(0xd6, 0x45, 0x45))
                          : QColor(0x3a, 0x3a, 0x42));
        p.drawRoundedRect(b, 3, 3);
        p.setPen(active ? Qt::black : Theme::text);
        f.setBold(true);
        p.setFont(f);
        p.drawText(b, Qt::AlignCenter, row.ref.kind == TrackKind::Video ? (t.hidden ? "⊘" : "◉") : "M");
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
    p.fillRect(placed, QColor(0, 0, 0, 190));
    p.setPen(Theme::text);
    p.drawText(placed, Qt::AlignCenter, text);
}

QRect TimelineView::headerButton(const Row& row) const
{
    return QRect(kHeaderW - 30, row.y + 6, 20, 16);
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

// Rechtsklick auf einen Übergang: Art, Ausrichtung, Löschen (wie DaVinci)
void TimelineView::contextMenuEvent(QContextMenuEvent* e)
{
    const auto t = transitionAt(e->pos());
    if (!t) return;
    const TimelineOps::TransitionSpan s = t->span;
    m_editor->selection()->setTransition({s.leftId, s.rightId});
    QMenu menu(this);
    if (t->ref.kind == TrackKind::Video) {
        for (const auto& i : kTransitionTypes) {
            QAction* a = menu.addAction(i.name);
            a->setCheckable(true);
            a->setChecked(s.style.type == i.type);
            connect(a, &QAction::triggered, this, [this, s, type = i.type] {
                TransitionStyle st = s.style;
                st.type = type;
                m_editor->setTransitionStyle(s.leftId, s.rightId, st);
            });
        }
        menu.addSeparator();
    }
    if (s.isDissolve()) {
        QMenu* align = menu.addMenu("Ausrichtung");
        const char* names[] = {"Mitte auf Schnitt", "Beginn am Schnitt", "Ende am Schnitt"}; // Index = TransitionAlign
        for (int i = 0; i < 3; ++i) {
            QAction* a = align->addAction(names[i]);
            a->setCheckable(true);
            a->setChecked(int(s.style.align) == i);
            connect(a, &QAction::triggered, this, [this, s, i] {
                TransitionStyle st = s.style;
                st.align = TransitionAlign(i);
                m_editor->setTransitionStyle(s.leftId, s.rightId, st);
            });
        }
    }
    connect(menu.addAction("Löschen"), &QAction::triggered, this,
            [this, s] { m_editor->removeTransition(s.leftId, s.rightId); });
    menu.exec(e->globalPos());
}

void TimelineView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const QPoint pos = e->position().toPoint();
    m_pressPos = pos;

    if (pos.x() < kHeaderW) { // Spurköpfe
        if (const auto row = rowAt(pos.y()); row && headerButton(*row).contains(pos)) {
            if (row->ref.kind == TrackKind::Video) m_editor->toggleTrackHidden(row->ref);
            else m_editor->toggleTrackMute(row->ref);
        }
        return;
    }
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
        update();
        return;
    }
    if (const int vid = volumeLineAt(pos)) {
        const QVector<int> group = m_editor->withLinked({vid});
        if (!sel->ids().contains(vid)) sel->set(QSet<int>(group.begin(), group.end()));
        m_volClipId = vid;
        m_volStartDb = m_volDb = TimelineOps::findClip(m_editor->project()->timeline(), vid)->volumeDb;
        m_volFine = e->modifiers() & Qt::ShiftModifier;
        m_drag = Drag::Volume;
        update();
        return;
    }
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
            m_dragTrackDelta = TimelineOps::clampTrackDelta(tl, m_dragIds, m_anchorRef.kind,
                                                            row->ref.index - m_anchorRef.index);
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
        m_trimDelta = m_editor->clampTrim(m_trim.clipId, m_trim.edge, delta);
        update();
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
        const bool fine = e->modifiers() & Qt::ShiftModifier;
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
    if (m_drag == Drag::Move)
        m_editor->moveClips(m_dragIds, m_dragDelta, m_anchorRef.kind, m_dragTrackDelta);
    const bool trimmed = m_drag == Drag::Trim;
    const bool volume = m_drag == Drag::Volume;
    if (m_drag == Drag::TransitionLength || m_drag == Drag::Fade) m_editor->project()->closeMerge();
    m_drag = Drag::None; // vor trimClip, damit die Vorschau nicht doppelt angewendet wird
    if (trimmed && m_trimDelta != 0) m_editor->trimClip(m_trim.clipId, m_trim.edge, m_trimDelta);
    if (volume) m_editor->setClipVolume(m_volClipId, m_volDb);
    m_trimIds.clear();
    m_trimDelta = 0;
    m_dragIds.clear();
    updateHoverCursor(e->position().toPoint());
    update();
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

// ---------- Drag & Drop (Media Pool oder direkt aus dem Dateimanager) ----------

QStringList TimelineView::dropPaths(const QMimeData* mime) const
{
    if (mime->hasFormat(MediaPool::MimeType)) return {QString::fromUtf8(mime->data(MediaPool::MimeType))};
    QStringList paths;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile()) paths << url.toLocalFile();
    return paths;
}

int TimelineView::dropTrackAt(int y) const
{
    // V2 und A2 gehören zusammen -> Index der Spur unter der Maus, egal ob Video oder Audio
    const auto all = rows();
    for (const Row& r : all)
        if (y >= r.y && y < r.y + r.h) return r.ref.index;
    const Timeline& tl = m_editor->project()->timeline();
    if (!all.isEmpty() && y >= all.last().y + all.last().h) return tl.audio.size(); // unterhalb: neue Spur
    return 0;
}

void TimelineView::dragEnterEvent(QDragEnterEvent* e)
{
    const QStringList paths = dropPaths(e->mimeData());
    if (paths.isEmpty()) return;
    m_dropItems.clear();
    for (const QString& path : paths) {
        if (path == MediaPool::TitleItem) { // Titel aus dem Media Pool: 5 s, nur Video
            m_dropItems << DropItem{5 * m_editor->project()->fps(), true, false};
            continue;
        }
        MediaInfo info;
        if (const MediaInfo* known = m_editor->project()->mediaInfo(path)) info = *known;
        else if (m_probe) info = m_probe(path);
        if (info.length > 0 && (info.hasVideo || info.hasAudio))
            m_dropItems << DropItem{info.length, info.hasVideo, info.hasAudio};
    }
    if (m_dropItems.isEmpty()) return;
    e->acceptProposedAction();
}

void TimelineView::dragMoveEvent(QDragMoveEvent* e)
{
    if (m_dropItems.isEmpty()) return;
    int total = 0;
    for (const DropItem& it : m_dropItems) total += it.length;
    const QPoint pos = e->position().toPoint();
    int frame = std::max(0, int(std::lround(xToFrame(pos.x()))));
    frame += snapDelta({frame, frame + total}, {});
    m_dropFrame = std::max(0, frame);
    m_dropTrack = dropTrackAt(pos.y());
    e->acceptProposedAction();
    update();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropFrame = -1;
    m_dropItems.clear();
    update();
}

void TimelineView::dropEvent(QDropEvent* e)
{
    if (m_dropFrame >= 0) emit dropRequested(dropPaths(e->mimeData()), m_dropFrame, m_dropTrack);
    m_dropFrame = -1;
    m_dropItems.clear();
    e->acceptProposedAction();
    update();
}

void TimelineView::resizeEvent(QResizeEvent*)
{
    emit viewChanged();
}
