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

TimelineView::TimelineView(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setMinimumHeight(160);
    setFocusPolicy(Qt::ClickFocus);
    m_autoScroll = new QTimer(this);
    m_autoScroll->setInterval(30);
    connect(m_autoScroll, &QTimer::timeout, this, &TimelineView::autoScrollStep);

    // Projekt geändert -> nur neu zeichnen. Der ViewState bleibt unangetastet (Kernregel).
    connect(editor->project(), &Project::timelineChanged, this, [this] {
        // Trim-Vorschau beruht auf der alten Timeline (andere Spuranzahl möglich, z. B. Strg+Z beim Ziehen):
        // verwerfen, die nächste Mausbewegung rechnet sie neu
        m_trimPreview.reset();
        update();
        emit viewChanged(); // nur damit die Scrollbar ggf. mehr Platz bekommt
    });
    connect(editor->selection(), &Selection::changed, this, qOverload<>(&QWidget::update));
    connect(editor, &Editor::targetTracksChanged, this, qOverload<>(&QWidget::update));
    // Clipfarbe/Flags im Media Pool geändert -> Clips neu zeichnen
    connect(editor->project(), &Project::poolChanged, this, qOverload<>(&QWidget::update));
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
    // aus den Zeilen, damit aufgeklappte Keyframe-Spuren mitzählen
    const auto all = rows();
    if (all.isEmpty()) return 0;
    return all.last().y + all.last().h - (kRulerH - m_view.scrollY);
}

QVector<TimelineView::Row> TimelineView::rows() const
{
    const Timeline& tl = m_editor->project()->timeline();
    QVector<Row> out;
    int y = kRulerH - m_view.scrollY + subtitlesHeight(); // Untertitelspuren liegen darüber
    // Spur wird höher, solange ein Clip darauf seine Keyframe-Spur bzw. seinen Kurven-Editor aufgeklappt hat
    auto row = [&](TrackRef ref, int y, int h) {
        Row r{ref, y, h};
        for (const Clip& c : tl.track(ref).clips) {
            if (m_keyLanes.contains(c.id) && Keys::hasKeys(c)) r.keyLane = kLaneH;
            if (curveParam(c)) r.curve = kCurveH;
        }
        r.lane = r.keyLane + r.curve;
        r.h += r.lane;
        return r;
    };
    for (int i = tl.video.size() - 1; i >= 0; --i) { // V1 unten, wie in DaVinci
        out << row({TrackKind::Video, i}, y, m_view.videoTrackHeight);
        y += out.last().h;
    }
    y += kSeparator;
    for (int i = 0; i < tl.audio.size(); ++i) {
        out << row({TrackKind::Audio, i}, y, m_view.audioTrackHeight);
        y += out.last().h;
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
    if (!row || isLocked(*row)) return 0;
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
    if (!row || isLocked(*row)) return std::nullopt;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const double x1 = frameToX(c.pos()), x2 = frameToX(c.endPos());
        if (pos.x() < x1 || pos.x() >= x2) continue;
        const double grab = std::min<double>(kEdgeGrabPx, (x2 - x1) / 3);
        if (pos.x() < x1 + grab) return EdgeHit{c.id, TimelineOps::Edge::Start};
        if (pos.x() >= x2 - grab) return EdgeHit{c.id, TimelineOps::Edge::End};
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<TimelineView::TrimHit> TimelineView::trimHitAt(const QPoint& pos) const
{
    using TimelineOps::Edge;
    using TimelineOps::TrimKind;
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row || isLocked(*row)) return std::nullopt;
    const auto& clips = m_editor->project()->timeline().track(row->ref).clips;
    for (int i = 0; i < clips.size(); ++i) {
        const Clip& c = clips[i];
        const QRect r = clipRect(*row, c);
        const double x1 = frameToX(c.pos()), x2 = frameToX(c.endPos());
        if (pos.x() < x1 || pos.x() >= x2 || pos.y() > r.bottom()) continue;
        // genau am Schnitt zweier anliegender Clips: Roll
        const bool prevAdj = i > 0 && clips[i - 1].end() == c.start;
        const bool nextAdj = i + 1 < clips.size() && clips[i + 1].start == c.end();
        constexpr double kRollPx = 3;
        if (prevAdj && pos.x() < x1 + kRollPx) return TrimHit{TrimKind::Roll, c.id, Edge::Start};
        if (nextAdj && pos.x() >= x2 - kRollPx) return TrimHit{TrimKind::Roll, c.id, Edge::End};
        const double grab = std::min<double>(kEdgeGrabPx + (prevAdj || nextAdj ? kRollPx : 0), (x2 - x1) / 3);
        if (pos.x() < x1 + grab) return TrimHit{TrimKind::Ripple, c.id, Edge::Start};
        if (pos.x() >= x2 - grab) return TrimHit{TrimKind::Ripple, c.id, Edge::End};
        const bool bar = pos.y() < r.top() + std::min(kClipBarH, r.height() / 2);
        return TrimHit{bar ? TrimKind::Slide : TrimKind::Slip, c.id, Edge::End};
    }
    // knapp links neben einem Schnitt (Maus über dem Ende des linken Clips) ist oben schon erfasst;
    // hier bleibt nur Leere
    return std::nullopt;
}

QRect TimelineView::clipRect(const Row& row, const Clip& c) const
{
    return QRect(QPoint(int(frameToX(c.pos())), row.y + 1),
                 QPoint(int(frameToX(c.endPos())) - 1, row.y + row.h - row.lane - 3));
}

QRect TimelineView::transitionRect(const Row& row, const TimelineOps::TransitionSpan& s) const
{
    return QRect(QPoint(int(frameToX(s.start)), row.y + 1),
                 QPoint(int(frameToX(s.end)) - 1, row.y + row.h - row.lane - 3));
}

// Übergänge liegen über den Clipkanten und gehen beim Klicken vor (wie DaVinci)
std::optional<TimelineView::TransitionHit> TimelineView::transitionAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row || isLocked(*row)) return std::nullopt;
    for (const auto& s : m_editor->transitions(row->ref)) {
        const QRect r = transitionRect(*row, s);
        if (pos.x() < r.left() || pos.x() > r.right()) continue;
        const int grab = std::min(kEdgeGrabPx, r.width() / 3);
        int edge = 0;
        // Nur die freie Kante ist ziehbar: Einblenden rechts, Ausblenden links, Überblendung beide
        if (s.leftId && pos.x() <= r.left() + grab) edge = -1;
        else if (s.rightId && pos.x() >= r.right() - grab) edge = 1;
        // Klickfläche teilen: an einer Clipkante gehört die untere Hälfte dem Trimmen,
        // eine nicht ziehbare Übergangskante (Clip-Seite beim Ein-/Ausblenden) ganz
        if (edgeAt(pos) && (!edge || pos.y() >= row->y + row->h / 2)) return std::nullopt;
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
    if (!row || isLocked(*row)) return std::nullopt;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        if (c.id != m_hoverClip) continue;
        const QRect r = clipRect(*row, c);
        if (r.width() < 24) return std::nullopt; // zu schmal: Griffe würden das Trimmen verdecken
        for (auto edge : {TimelineOps::Edge::Start, TimelineOps::Edge::End})
            if (fadeHandleRect(r, c, edge).adjusted(-3, -2, 3, 3).contains(pos)) return EdgeHit{c.id, edge};
    }
    return std::nullopt;
}

QRect TimelineView::keyIconRect(const QRect& r) const
{
    return QRect(r.right() - 13, r.bottom() - 12, 11, 11);
}

int TimelineView::keyIconAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return 0;
    const auto row = rowAt(pos.y());
    if (!row) return 0;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const QRect r = clipRect(*row, c);
        if (r.width() >= 40 && r.height() >= 24 && Keys::hasKeys(c) && keyIconRect(r).adjusted(-2, -2, 2, 2).contains(pos))
            return c.id;
    }
    return 0;
}

QRect TimelineView::laneRect(const Row& row, const Clip& c) const
{
    const int top = keyLaneTop(row);
    return QRect(QPoint(int(frameToX(c.pos())), top), QPoint(int(frameToX(c.endPos())) - 1, top + row.keyLane - 2));
}

bool TimelineView::inLane(const QPoint& pos) const
{
    const auto row = rowAt(pos.y());
    return row && row->keyLane > 0 && pos.y() >= keyLaneTop(*row) && pos.y() < keyLaneTop(*row) + row->keyLane;
}

std::optional<TimelineView::KeyHit> TimelineView::keyframeAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || !inLane(pos)) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (isLocked(*row)) return std::nullopt;
    std::optional<KeyHit> best;
    double bestDist = kKeyGrabPx + 1;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        if (!m_keyLanes.contains(c.id)) continue;
        for (int t : Keys::keyTimes(c)) {
            if (t < 0 || t >= c.length()) continue; // außerhalb des Clips (nach Trimmen) nicht greifbar
            const double d = std::abs(frameToX(c.pos() + t) - pos.x());
            if (d < bestDist) {
                bestDist = d;
                best = KeyHit{c.id, t};
            }
        }
    }
    return best;
}

int TimelineView::volumeLineAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return 0;
    const auto row = rowAt(pos.y());
    if (!row || row->ref.kind != TrackKind::Audio || isLocked(*row)) return 0;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const QRect r = clipRect(*row, c);
        if (pos.x() < r.left() || pos.x() > r.right()) continue;
        const QRect body = clipBodyRect(r);
        if (body.height() < 6) return 0;
        const double db = volumeAt(c, (pos.x() - frameToX(c.pos())) / m_view.pxPerFrame);
        return std::abs(pos.y() - volumeLineY(body, db)) <= kVolumeGrabPx ? c.id : 0;
    }
    return 0;
}

void TimelineView::updateHoverCursor(const QPoint& pos)
{
    if (subRowAt(pos.y())) {
        if (m_hoverClip || m_hoverVolClip) {
            m_hoverClip = m_hoverVolClip = 0;
            update();
        }
        setCursor(pos.x() >= kHeaderW && cueEdgeAt(pos) ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }
    if (curveIconAt(pos)) {
        setCursor(Qt::PointingHandCursor);
        return;
    }
    if (const auto h = curveHitAt(pos)) {
        if (m_hoverClip) {
            m_hoverClip = 0;
            update();
        }
        setCursor(h->part == 2 ? Qt::PointingHandCursor : h->index >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
        return;
    }
    if (m_tool == Tool::Trim) {
        if (keyIconAt(pos)) setCursor(Qt::PointingHandCursor);
        else if (keyframeAt(pos)) setCursor(Qt::SizeHorCursor);
        else if (const auto h = trimHitAt(pos)) setCursor(trimCursor(h->kind, h->edge));
        else setCursor(Qt::ArrowCursor);
        return;
    }
    const int hover = m_tool == Tool::Select && pos.x() >= kHeaderW && pos.y() >= kRulerH ? clipAt(pos) : 0;
    if (hover != m_hoverClip) {
        m_hoverClip = hover;
        update();
    }
    if (m_tool == Tool::Select)
        if (const auto h = retimeHitAt(pos); h && (h->point >= 0 || !edgeAt(pos))) {
            setCursor(h->point >= 0 ? Qt::SizeHorCursor : Qt::PointingHandCursor);
            return;
        }
    if (fadeHandleAt(pos) || (m_tool == Tool::Select && keyframeAt(pos))) {
        setCursor(Qt::SizeHorCursor);
        return;
    }
    if (m_tool == Tool::Select && keyIconAt(pos)) {
        setCursor(Qt::PointingHandCursor);
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
    for (const SubtitleTrack& t : tl.subtitles)
        for (const SubtitleCue& c : t.cues)
            if (!exclude.contains(c.id)) pts << c.start << c.end;
    return pts;
}

// Liefert die Korrektur, damit eine der Kanten auf einen Snap-Punkt fällt (0 = kein Snap)
int TimelineView::snapDelta(const QVector<int>& edges, const QSet<int>& exclude, bool* hit) const
{
    if (hit) *hit = false;
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
                if (hit) *hit = true;
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

void TimelineView::setZoomAndScroll(double pxPerFrame, double leftFrame, int scrollY)
{
    m_view.pxPerFrame = std::clamp(pxPerFrame, ViewState::kMinPxPerFrame, ViewState::kMaxPxPerFrame);
    m_view.leftFrame = std::max(0.0, leftFrame);
    setScrollY(scrollY); // begrenzt auf die Höhe der Spuren, zeichnet neu, meldet viewChanged
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
    if (m_hoverClip) m_hoverClip = 0; // Fade-Griffe nur im Auswahl-Werkzeug
    m_hoverFrame = -1;
    update();
    emit toolChanged(tool);
}

void TimelineView::setShowClipNames(bool on)
{
    m_showNames = on;
    update();
}

void TimelineView::setShowClipDurations(bool on)
{
    m_showDurations = on;
    update();
}

void TimelineView::setSnapping(bool on)
{
    if (on == m_snap) return;
    m_snap = on;
    emit snappingChanged(on);
}

// ---------- Zeichnen ----------

void TimelineView::setRenderCacheSpans(const QVector<CacheSpan>& spans)
{
    auto same = [](const CacheSpan& a, const CacheSpan& b) { return a.start == b.start && a.end == b.end && a.done == b.done; };
    if (spans.size() == m_cacheSpans.size() && std::equal(spans.begin(), spans.end(), m_cacheSpans.begin(), same)) return;
    m_cacheSpans = spans;
    update(0, 0, width(), kRulerH);
}

QRect TimelineView::headerButton(const Row& row) const
{
    return QRect(kHeaderW - 30, row.y + 6, 20, 16);
}

QRect TimelineView::lockButton(const Row& row) const
{
    return QRect(kHeaderW - 54, row.y + 6, 20, 16);
}

QRect TimelineView::shortNameRect(const Row& row) const
{
    return QRect(10, row.y + 6, 26, 16);
}

QRect TimelineView::nameRect(const Row& row) const
{
    const int left = 42;
    return QRect(left, row.y + 5, lockButton(row).left() - 4 - left, 18);
}

bool TimelineView::isLocked(const Row& row) const
{
    return m_editor->isTrackLocked(row.ref);
}

int TimelineView::trackDropIndex(int y) const
{
    // Zielspur = Spur gleichen Typs unter der Maus (darüber/darunter hinaus: die nächste)
    int to = -1, best = 1 << 30;
    for (const Row& r : rows()) {
        if (r.ref.kind != m_trackDragRef.kind) continue;
        const int d = y < r.y ? r.y - y : y >= r.y + r.h ? y - (r.y + r.h - 1) : 0;
        if (d < best) {
            best = d;
            to = r.ref.index;
        }
    }
    return to;
}

void TimelineView::resizeEvent(QResizeEvent*)
{
    emit viewChanged();
}
