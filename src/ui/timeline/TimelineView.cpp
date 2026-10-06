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

namespace {

// Clipname, bei geänderter Geschwindigkeit mit Angabe wie DaVinci (z. B. „clip.mp4 (50 %)“);
// Audio aus Dateien mit mehreren Ton-Streams (OBS) mit Stream-Name („aufnahme.mkv · Mikro“)
QString clipLabel(const Project* project, const Clip& c, TrackKind kind)
{
    QString name = project->clipName(c); // Compound Clips: Name der Sequenz
    if (kind == TrackKind::Audio && !c.mediaPath.isEmpty())
        if (const MediaInfo* m = project->mediaInfo(c.mediaPath); m && m->audioStreamCount() > 1)
            name += QString(" · %1").arg(m->audioStreamName(c.audioStream));
    if (!c.isRetimed()) return name;
    if (!c.freeze && !c.reverse && c.speed == 1.0
        && std::all_of(c.ramp.begin(), c.ramp.end(), [](const SpeedPoint& p) { return p.speed == 1.0; }))
        return name; // Speed-Punkte ohne Tempowechsel
    const QString speed = c.freeze     ? T("Standbild")
                          : c.hasRamp() ? T("Speed Ramp")
                                        : QString("%1%2 %").arg(c.reverse ? "-" : "").arg(QLocale().toString(c.speed * 100, 'g', 4));
    return QString("%1 (%2)").arg(name, speed);
}

// Compound Clip im Clipkörper: gestapelte Ebenen (wie das DaVinci-Symbol) statt Filmstreifen/Wellenform
void drawCompoundBody(QPainter& p, const QRect& body, const QColor& base)
{
    if (body.height() < 8 || body.width() < 12) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double h = std::min(18.0, body.height() - 4.0), w = h * 1.4;
    const double x = body.left() + 6, y = body.center().y() - h / 2;
    for (int i = 2; i >= 0; --i) {
        const QRectF r(x + i * h * 0.22, y + (2 - i) * h * 0.22, w * 0.7, h * 0.56);
        p.setPen(QPen(base.lighter(170), 1));
        p.setBrush(base.darker(110 + 20 * i));
        p.drawRoundedRect(r, 1.5, 1.5);
    }
    p.restore();
}

// Clipfarbe der Spur: eigene Spurfarbe, sonst ungültig (= Standard je Clipart)
QColor trackColor(const Track& t)
{
    const TrackColorInfo* info = trackColorInfo(t.color);
    return info ? QColor::fromRgba(info->rgb) : QColor();
}

// Farbe eines Clips wie DaVinci: Clipfarbe des Media-Pool-Clips vor der Spurfarbe, sonst ungültig (= Standard)
QColor clipColor(const Project* project, const Clip& c, const Track* t)
{
    if (!c.isTitle())
        if (const MediaInfo* m = project->mediaInfo(c.mediaPath))
            if (const TrackColorInfo* info = trackColorInfo(m->clipColor)) return QColor::fromRgba(info->rgb);
    return t ? trackColor(*t) : QColor();
}

// Schloss-Symbol (Vorhängeschloss) mittig in r
void drawLock(QPainter& p, const QRectF& r, const QColor& color)
{
    const double cx = r.center().x(), cy = r.center().y();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.4));
    p.setBrush(Qt::NoBrush);
    QPainterPath shackle;
    shackle.moveTo(cx - 2.6, cy);
    shackle.lineTo(cx - 2.6, cy - 2.2);
    shackle.arcTo(QRectF(cx - 2.6, cy - 5.0, 5.2, 5.6), 180, -180);
    shackle.lineTo(cx + 2.6, cy);
    p.drawPath(shackle);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(cx - 4, cy - 0.5, 8, 6), 1, 1);
    p.restore();
}

constexpr int kSnapPx = 8;
constexpr double kFinePxPerFrame = 12; // ab hier verschieben Ton-Clips feiner als 1 Frame
constexpr int kDragStartPx = 4;
constexpr int kAutoScrollZone = 24; // Randbereich (px), in dem das Ziehen die Timeline mitscrollt
constexpr int kEdgeGrabPx = 6; // so nah an der Clipkante wird getrimmt statt verschoben
constexpr int kVolumeGrabPx = 4;
constexpr int kClipBarH = 16;  // Titelleiste im Clip
constexpr int kLaneH = 16;     // aufgeklappte Keyframe-Spur unter dem Clip
constexpr int kCurveH = 120;   // aufgeklappter Kurven-Editor unter dem Clip
constexpr int kKeyGrabPx = 5;  // so nah an einer Raute wird sie gegriffen

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

// Clip-Lautstärke an Clip-Frame t (mit Keyframes)
double volumeAt(const Clip& c, double t)
{
    return Keys::valueAt(c, AnimParam::Volume, std::clamp(t, 0.0, double(c.length() - 1)));
}

// Raute (Keyframe) um (x, y)
void drawDiamond(QPainter& p, double x, double y, double r)
{
    const QPointF pts[] = {{x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}};
    p.drawPolygon(pts, 4);
}
// Mauszeiger für den Trim-Modus (wie DaVinci): Klammer(n) für Ripple/Roll, Rahmen für Slip/Slide
QCursor trimCursor(TimelineOps::TrimKind kind, TimelineOps::Edge edge)
{
    using TimelineOps::TrimKind;
    static QHash<int, QCursor> cache;
    const int key = int(kind) * 2 + (edge == TimelineOps::Edge::Start);
    if (cache.contains(key)) return cache[key];
    constexpr int S = 32, M = S / 2;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    auto arrow = [&](double x1, double x2, double y) { // Linie mit Pfeilspitze bei x2
        path.moveTo(x1, y);
        path.lineTo(x2, y);
        const double d = x2 > x1 ? -4 : 4;
        path.moveTo(x2 + d, y - 4);
        path.lineTo(x2, y);
        path.lineTo(x2 + d, y + 4);
    };
    auto bracket = [&](double x, bool open) { // "[" (open) bzw. "]"
        const double w = open ? 4 : -4;
        path.moveTo(x + w, M - 9);
        path.lineTo(x, M - 9);
        path.lineTo(x, M + 9);
        path.lineTo(x + w, M + 9);
    };
    switch (kind) {
    case TrimKind::Ripple: {
        const bool start = edge == TimelineOps::Edge::Start;
        bracket(start ? M - 2 : M + 2, start);
        arrow(start ? M - 6 : M + 6, start ? 4 : S - 4, M);   // nach außen
        arrow(start ? M + 1 : M - 1, start ? M + 10 : M - 10, M); // nach innen
        break;
    }
    case TrimKind::Roll:
        bracket(M - 3, false);
        bracket(M + 3, true);
        arrow(M - 6, 3, M);
        arrow(M + 6, S - 3, M);
        break;
    case TrimKind::Slip: // Pfeile innerhalb des Rahmens
    case TrimKind::Slide: // Pfeile außerhalb
        path.addRect(M - 7, M - 8, 14, 16);
        if (kind == TrimKind::Slip) {
            arrow(M, M - 5, M);
            arrow(M, M + 5, M);
        } else {
            arrow(M - 8, 2, M);
            arrow(M + 8, S - 2, M);
        }
        break;
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
    p.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
    p.end();
    return cache[key] = QCursor(pm, M, M);
}
} // namespace

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

// Rechtsklick auf den Spurkopf (wie DaVinci): sperren, umbenennen, Spurfarbe
void TimelineView::headerMenu(const Row& row, const QPoint& globalPos)
{
    const Timeline& tl = m_editor->project()->timeline();
    const Track& t = tl.track(row.ref);
    const TrackRef ref = row.ref;
    QMenu menu(this);
    QAction* lock = menu.addAction(T("Spur sperren"));
    lock->setCheckable(true);
    lock->setChecked(t.locked);
    connect(lock, &QAction::triggered, this, [this, ref] { m_editor->toggleTrackLock(ref); });
    connect(menu.addAction(T("Spur umbenennen")), &QAction::triggered, this, [this, ref] { startRename(ref); });
    QMenu* colors = menu.addMenu(T("Spurfarbe ändern"));
    auto addColor = [&](const QString& id, const QString& name, const QColor& c) {
        QPixmap pm(12, 12);
        pm.fill(c);
        QAction* a = colors->addAction(QIcon(pm), name);
        a->setCheckable(true);
        a->setChecked(t.color == id);
        connect(a, &QAction::triggered, this, [this, ref, id] { m_editor->setTrackColor(ref, id); });
    };
    addColor({}, T("Standard"), ref.kind == TrackKind::Video ? Theme::videoClip : Theme::audioClip);
    colors->addSeparator();
    for (const auto& i : kTrackColors) addColor(QString::fromLatin1(i.id), T(i.name), QColor::fromRgba(i.rgb));
    // Spuren hinzufügen/löschen (DaVinci „Add Track“/„Delete Track“). Oberhalb/unterhalb wie angezeigt:
    // Video zählt von unten (V1 unten), Audio von oben (A1 oben)
    menu.addSeparator();
    const int above = ref.kind == TrackKind::Video ? ref.index + 1 : ref.index;
    const int below = ref.kind == TrackKind::Video ? ref.index : ref.index + 1;
    connect(menu.addAction(T("Spur oberhalb hinzufügen")), &QAction::triggered, this,
            [this, ref, above] { m_editor->addTrack(ref.kind, above); });
    connect(menu.addAction(T("Spur unterhalb hinzufügen")), &QAction::triggered, this,
            [this, ref, below] { m_editor->addTrack(ref.kind, below); });
    QAction* del = menu.addAction(T("Spur löschen"));
    del->setEnabled(m_editor->canRemoveTrack(ref));
    if (!del->isEnabled())
        del->setToolTip(t.locked ? T("Gesperrte Spuren lassen sich nicht löschen")
                                 : T("Die letzte Spur eines Typs lässt sich nicht löschen"));
    menu.setToolTipsVisible(true);
    connect(del, &QAction::triggered, this, [this, ref] { m_editor->removeTrack(ref); });
    menu.exec(globalPos);
}

// Rechtsklick auf die freie Fläche ohne Spur: neue Spur ans Ende (Video oben, Audio unten), wie DaVinci „Add Track“
void TimelineView::emptyAreaMenu(const QPoint& globalPos)
{
    const Timeline& tl = m_editor->project()->timeline();
    QMenu menu(this);
    connect(menu.addAction(T("Videospur hinzufügen")), &QAction::triggered, this,
            [this, n = int(tl.video.size())] { m_editor->addTrack(TrackKind::Video, n); });
    connect(menu.addAction(T("Audiospur hinzufügen")), &QAction::triggered, this,
            [this, n = int(tl.audio.size())] { m_editor->addTrack(TrackKind::Audio, n); });
    menu.exec(globalPos);
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

void TimelineView::startRename(TrackRef ref)
{
    if (!m_nameEdit) {
        m_nameEdit = new QLineEdit(this);
        m_nameEdit->setFrame(false);
        m_nameEdit->setStyleSheet(QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; padding: 0 2px; }")
                                      .arg(Theme::panel.name(), Theme::text.name(), Theme::primary.name()));
        m_nameEdit->installEventFilter(this);
        connect(m_nameEdit, &QLineEdit::editingFinished, this, [this] { finishRename(true); });
    }
    const auto row = rowFor(ref);
    if (!row) return;
    m_nameSub = -1;
    m_nameRef = ref;
    m_renaming = true;
    QFont f = font();
    f.setPointSizeF(8.5);
    m_nameEdit->setFont(f);
    m_nameEdit->setText(trackDisplayName(m_editor->project()->timeline().track(ref), ref));
    m_nameEdit->setGeometry(nameRect(*row).adjusted(-3, 0, 0, 0));
    m_nameEdit->show();
    m_nameEdit->selectAll();
    m_nameEdit->setFocus();
    update();
}

void TimelineView::finishRename(bool commit)
{
    if (!m_renaming) return;
    m_renaming = false; // vor hide(): Fokusverlust meldet sonst noch einmal editingFinished
    const QString text = m_nameEdit->text();
    m_nameEdit->hide();
    setFocus();
    if (commit && m_nameSub >= 0) m_editor->renameSubtitleTrack(m_nameSub, text);
    else if (commit) m_editor->renameTrack(m_nameRef, text);
    m_nameSub = -1;
    update();
}

bool TimelineView::eventFilter(QObject* obj, QEvent* e)
{
    // Esc im Namensfeld bricht ab (vorher als ShortcutOverride annehmen, sonst greift ein Tastenkürzel)
    if (obj == m_nameEdit && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
        e->accept();
        if (e->type() == QEvent::KeyPress) finishRename(false);
        return true;
    }
    return QWidget::eventFilter(obj, e);
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
    if (const auto sub = subRowAt(e->pos().y())) {
        if (e->pos().x() < kHeaderW) subtitleHeaderMenu(sub->index, e->globalPos());
        else subtitleTrackMenu(e->pos(), e->globalPos());
        return;
    }
    if (e->pos().x() < kHeaderW && e->pos().y() >= kRulerH) {
        if (const auto row = rowAt(e->pos().y())) headerMenu(*row, e->globalPos());
        else emptyAreaMenu(e->globalPos());
        return;
    }
    // Rechtsklick in der Retime-Leiste: Speed-Punkt (Übergang, Entfernen) bzw. Abschnitt (Tempo)
    if (const auto h = retimeHitAt(e->pos())) {
        if (h->point >= 0) speedPointMenu(*h, e->globalPos());
        else segmentMenu(*h, int(std::lround(xToFrame(e->pos().x()))), e->globalPos());
        return;
    }
    if (curveContextMenu(e->pos(), e->globalPos())) return;
    // Rechtsklick auf eine Keyframe-Raute: Verlauf (wie DaVinci) oder Löschen, gilt für die ausgewählten Rauten
    if (const auto k = keyframeAt(e->pos())) {
        Selection* sel = m_editor->selection();
        if (sel->keyClip() != k->clipId || sel->keyParam() >= 0 || !sel->keyTimes().contains(k->t))
            sel->setKeyframes(k->clipId, {k->t});
        const QVector<int> times = sel->keyTimes().values().toVector();
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), k->clipId);
        if (!c) return;
        std::optional<KeyEase> current;
        for (const KeyTrack& track : c->keys)
            for (const Keyframe& kf : track)
                if (kf.frame == c->in + k->t) current = kf.ease;
        QMenu menu(this);
        const struct { KeyEase ease; const char* name; } eases[] = {
            {KeyEase::Linear, "Linear"}, {KeyEase::EaseIn, "Ease In"}, {KeyEase::EaseOut, "Ease Out"},
            {KeyEase::EaseInOut, "Ease In and Out"}, {KeyEase::Bezier, "Bezier"}};
        for (const auto& it : eases) {
            QAction* a = menu.addAction(it.name); // wie DaVinci auch deutsch englisch
            a->setCheckable(true);
            a->setChecked(current == it.ease);
            connect(a, &QAction::triggered, this, [this, id = k->clipId, times, ease = it.ease] {
                m_editor->modifyClips({id}, T("Keyframe-Verlauf"), [&](Clip& clip) { Keys::setEase(clip, times, ease); });
            });
        }
        menu.addSeparator();
        connect(menu.addAction(T("Löschen")), &QAction::triggered, this,
                [this, id = k->clipId, times] { m_editor->removeKeyframes(id, times); });
        menu.exec(e->globalPos());
        return;
    }
    const auto t = transitionAt(e->pos());
    if (!t) {
        // Rechtsklick auf einen Clip: auswählen (falls nicht schon), Menü baut das Hauptfenster (Aktionen)
        if (const int id = clipAt(e->pos())) {
            Selection* sel = m_editor->selection();
            if (!sel->contains(id)) {
                const QVector<int> group = m_editor->withLinked({id});
                sel->set(QSet<int>(group.begin(), group.end()));
            }
            emit clipMenuRequested(e->globalPos());
        } else if (e->pos().y() >= kRulerH && !rowAt(e->pos().y())) {
            emptyAreaMenu(e->globalPos()); // freie Fläche unter/zwischen den Spuren
        }
        return;
    }
    const TimelineOps::TransitionSpan s = t->span;
    m_editor->selection()->setTransition({s.leftId, s.rightId});
    QMenu menu(this);
    if (t->ref.kind == TrackKind::Video) {
        for (const auto& i : kTransitionTypes) {
            QAction* a = menu.addAction(T(i.name));
            a->setCheckable(true);
            a->setChecked(s.style.type == i.type);
            connect(a, &QAction::triggered, this, [this, s, type = i.type] {
                TransitionStyle st = s.style;
                st.type = type;
                m_editor->setTransitionStyle(s.leftId, s.rightId, st);
            });
        }
        menu.addSeparator();
    } else {
        for (const auto& i : kAudioCurves) {
            QAction* a = menu.addAction(i.name);
            a->setCheckable(true);
            a->setChecked(s.style.audio == i.curve);
            connect(a, &QAction::triggered, this, [this, s, curve = i.curve] {
                TransitionStyle st = s.style;
                st.audio = curve;
                m_editor->setTransitionStyle(s.leftId, s.rightId, st);
            });
        }
        menu.addSeparator();
    }
    if (s.isDissolve()) {
        QMenu* align = menu.addMenu(T("Ausrichtung"));
        const QString names[] = {T("Mitte auf Schnitt"), T("Beginn am Schnitt"), T("Ende am Schnitt")}; // Index = TransitionAlign
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
    connect(menu.addAction(T("Löschen")), &QAction::triggered, this,
            [this, s] { m_editor->removeTransition(s.leftId, s.rightId); });
    menu.exec(e->globalPos());
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
        m_trimDelta = m_editor->clampTrim(m_trim.clipId, m_trim.edge, delta);
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

// ---------- Drag & Drop (Media Pool oder direkt aus dem Dateimanager) ----------

QStringList TimelineView::dropPaths(const QMimeData* mime) const
{
    if (mime->hasFormat(MediaPool::MimeType)) return {QString::fromUtf8(mime->data(MediaPool::MimeType))};
    QStringList paths;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile()) paths << url.toLocalFile();
    return paths;
}

bool TimelineView::dropRange(const QMimeData* mime, int* in, int* out)
{
    if (!mime->hasFormat(Viewer::RangeMimeType)) return false;
    const QStringList parts = QString::fromUtf8(mime->data(Viewer::RangeMimeType)).split(' ');
    if (parts.size() != 2) return false;
    *in = parts[0].toInt();
    *out = parts[1].toInt();
    return *out >= *in;
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

// Schnitt/Clipkante für einen Übergang unter der Maus: nächste Kante des Clips unter der Maus
// (oder bis 30 px daneben); links vom Schnitt = End on Edit, rechts = Start on Edit, nah dran = mittig.
std::optional<TimelineView::TransitionDrop> TimelineView::transitionDropAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row || row->ref.kind != m_transDragKind || isLocked(*row)) return std::nullopt;
    const auto& clips = m_editor->project()->timeline().track(row->ref).clips;
    const double f = xToFrame(pos.x());
    std::optional<TransitionDrop> best;
    double bestDx = 0;
    for (int i = 0; i < clips.size(); ++i) {
        const Clip& c = clips[i];
        const bool over = f >= c.start && f < c.end();
        const int prev = i > 0 && clips[i - 1].end() == c.start ? clips[i - 1].id : 0;
        const int next = i + 1 < clips.size() && clips[i + 1].start == c.end() ? clips[i + 1].id : 0;
        for (const auto& [frame, left, right] : {std::tuple{c.start, prev, c.id}, std::tuple{c.end(), c.id, next}}) {
            const double dx = pos.x() - frameToX(frame);
            if (!over && std::abs(dx) > 30) continue;
            if (best && std::abs(dx) >= std::abs(bestDx)) continue;
            TransitionDrop d;
            d.ref = row->ref;
            d.leftId = left;
            d.rightId = right;
            bestDx = dx;
            best = d;
        }
    }
    if (!best) return std::nullopt;
    best->style = m_transDragStyle;
    if (best->leftId && best->rightId) {
        const double w = m_editor->project()->fps() * m_view.pxPerFrame; // Standardlänge 1 s
        const double zone = std::max(4.0, w / 6);
        best->style.align = std::abs(bestDx) <= zone ? TransitionAlign::Center
                            : bestDx < 0             ? TransitionAlign::End
                                                     : TransitionAlign::Start;
    }
    const auto span = m_editor->previewTransitionAt(best->leftId, best->rightId, best->style);
    if (!span) return std::nullopt; // passt nicht (keine Handles)
    best->span = *span;
    return best;
}

int TimelineView::fxDropClipAt(const QPoint& pos) const
{
    const auto row = rowAt(pos.y());
    return row && row->ref.kind == TrackKind::Video && !m_editor->isTrackLocked(row->ref) ? clipAt(pos) : 0;
}

void TimelineView::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(EffectsLibrary::EffectMimeType)) {
        m_fxDragging = QString::fromUtf8(e->mimeData()->data(EffectsLibrary::EffectMimeType));
        m_fxDropClip = 0;
        e->acceptProposedAction();
        return;
    }
    if (e->mimeData()->hasFormat(EffectsLibrary::MimeType)) {
        m_transDragging = EffectsLibrary::parseTransition(e->mimeData()->data(EffectsLibrary::MimeType),
                                                          &m_transDragKind, &m_transDragStyle);
        if (m_transDragging) e->acceptProposedAction();
        return;
    }
    const QStringList paths = dropPaths(e->mimeData());
    if (paths.isEmpty()) return;
    m_dropItems.clear();
    for (const QString& path : paths) {
        if (path == MediaPool::TitleItem) { // Titel aus dem Media Pool: 5 s, nur Video
            m_dropItems << DropItem{5 * m_editor->project()->fps(), true, 0};
            continue;
        }
        if (const int seq = MediaPool::sequenceOfItem(path)) { // Timeline/Compound Clip aus dem Media Pool
            const Project* project = m_editor->project();
            const Sequence* s = project->sequence(seq);
            if (!s || !project->canNest(seq, project->currentSequence())) continue; // nie in sich selbst
            auto has = [](const QVector<Track>& tracks) {
                return std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return !t.clips.isEmpty(); });
            };
            const int len = TimelineOps::endFrame(s->timeline);
            if (len > 0) m_dropItems << DropItem{len, has(s->timeline.video), has(s->timeline.audio) ? 1 : 0};
            continue;
        }
        MediaInfo info;
        if (const MediaInfo* known = m_editor->project()->mediaInfo(path)) info = *known;
        else if (m_probe) info = m_probe(path);
        int in = 0, out = 0;
        if (dropRange(e->mimeData(), &in, &out)) info.length = out - in + 1; // Quell-In/Out aus dem Viewer
        if (info.length > 0 && (info.hasVideo || info.hasAudio))
            m_dropItems << DropItem{info.length, info.hasVideo, info.audioStreamCount()};
    }
    if (m_dropItems.isEmpty()) return;
    e->acceptProposedAction();
}

void TimelineView::dragMoveEvent(QDragMoveEvent* e)
{
    if (!m_fxDragging.isEmpty()) {
        m_fxDropClip = fxDropClipAt(e->position().toPoint());
        if (m_fxDropClip) e->acceptProposedAction();
        else e->ignore();
        update();
        return;
    }
    if (m_transDragging) {
        m_transDrop = transitionDropAt(e->position().toPoint());
        if (m_transDrop) e->acceptProposedAction();
        else e->ignore();
        update();
        return;
    }
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
    m_transDragging = false;
    m_transDrop.reset();
    m_fxDragging.clear();
    m_fxDropClip = 0;
    update();
}

void TimelineView::dropEvent(QDropEvent* e)
{
    if (!m_fxDragging.isEmpty()) {
        // Wie DaVinci: nur auf den Clip unter der Maus (auch wenn andere ausgewählt sind)
        if (const int id = fxDropClipAt(e->position().toPoint())) {
            m_editor->addEffect({id}, m_fxDragging);
            e->acceptProposedAction();
        }
        m_fxDragging.clear();
        m_fxDropClip = 0;
        update();
        return;
    }
    if (m_transDragging) {
        if (const auto d = transitionDropAt(e->position().toPoint())) {
            m_editor->addTransitionAt(d->leftId, d->rightId, d->style);
            e->acceptProposedAction();
        }
        m_transDragging = false;
        m_transDrop.reset();
        update();
        return;
    }
    int in = 0, out = 0;
    const QStringList paths = dropPaths(e->mimeData());
    if (m_dropFrame >= 0 && paths.size() == 1 && dropRange(e->mimeData(), &in, &out))
        emit rangeDropRequested(paths.first(), in, out, m_dropFrame, m_dropTrack);
    else if (m_dropFrame >= 0)
        emit dropRequested(paths, m_dropFrame, m_dropTrack);
    m_dropFrame = -1;
    m_dropItems.clear();
    e->acceptProposedAction();
    update();
}

void TimelineView::resizeEvent(QResizeEvent*)
{
    emit viewChanged();
}

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
    const Timeline& tl = m_editor->project()->timeline();
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

// ---------- Retime-Steuerung (Speed Ramps) ----------

namespace {
constexpr int kRetimeBarH = 14;
constexpr int kSpeedPointGrabPx = 5;

QString percentText(double speed)
{
    return QString("%1 %").arg(QLocale().toString(speed * 100, 'g', 4));
}
} // namespace

void TimelineView::setRetimeControls(const QVector<int>& ids, bool on)
{
    for (int id : m_editor->withLinked(ids)) {
        if (on && m_editor->canRetime(id)) m_retimeClips.insert(id);
        else m_retimeClips.remove(id);
    }
    update();
}

QRect TimelineView::retimeBarRect(const QRect& r) const
{
    const int top = r.top() + std::min(kClipBarH, r.height());
    return QRect(r.left(), top, r.width(), std::min(kRetimeBarH, r.bottom() - top));
}

std::optional<TimelineView::RetimeHit> TimelineView::retimeHitAt(const QPoint& pos) const
{
    if (m_retimeClips.isEmpty() || pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row || isLocked(*row)) return std::nullopt;
    const Project* project = m_editor->project();
    for (const Clip& c : project->timeline().track(row->ref).clips) {
        if (!m_retimeClips.contains(c.id)) continue;
        const QRect bar = retimeBarRect(clipRect(*row, c));
        if (bar.height() < 8 || !bar.adjusted(-kSpeedPointGrabPx, 0, kSpeedPointGrabPx, 0).contains(pos)) continue;
        const MediaInfo* m = project->mediaInfo(c.mediaPath);
        const RetimeMap map(c, m ? m->length : 0);
        const QVector<double> points = map.pointMaterial();
        RetimeHit hit;
        hit.clipId = c.id;
        double best = kSpeedPointGrabPx + 1;
        for (int i = 0; i < points.size(); ++i) {
            if (points[i] <= c.in || points[i] >= c.out + 1) continue; // außerhalb des Ausschnitts
            const double d = std::abs(frameToX(c.pos() + points[i] - c.in) - pos.x());
            if (d < best) {
                best = d;
                hit.point = i;
            }
        }
        if (hit.point >= 0) return hit;
        if (!bar.contains(pos)) continue;
        const double mat = c.in + xToFrame(pos.x()) - c.start;
        hit.segment = int(std::count_if(points.begin(), points.end(), [mat](double x) { return x <= mat; }));
        return hit;
    }
    return std::nullopt;
}

void TimelineView::drawRetime(QPainter& p, const QRect& r, const Clip& c)
{
    const QRect bar = retimeBarRect(r);
    if (bar.height() < 8) return;
    const MediaInfo* m = m_editor->project()->mediaInfo(c.mediaPath);
    const RetimeMap map(c, m ? m->length : 0);
    QVector<double> points = map.pointMaterial();
    const bool dragging = m_drag == Drag::SpeedPoint && m_rampDrag.clipId == c.id;
    p.save();
    p.setClipRect(r.intersected(QRect(kHeaderW, 0, width(), height())), Qt::IntersectClip);
    p.fillRect(bar, QColor(0, 0, 0, 150));
    auto xOf = [&](double mat) { return frameToX(c.pos() + mat - c.in); };
    // Weiche Übergänge: heller Bereich über die Clip-Höhe
    for (const RetimeMap::Piece& pc : map.pieces())
        if (pc.v0 != pc.v1)
            p.fillRect(QRectF(QPointF(xOf(pc.m0), bar.top()), QPointF(xOf(pc.m1), r.bottom())), QColor(0xff, 0xff, 0xff, 28));
    // Tempo je Abschnitt (mit ▾ wie DaVinci)
    QFont f = font();
    f.setPointSizeF(7);
    p.setFont(f);
    const QFontMetrics fm(f);
    for (int seg = 0; seg <= points.size(); ++seg) {
        const double a = std::max(double(c.in), seg > 0 ? points[seg - 1] : -1e9);
        const double b = std::min(double(c.out + 1), seg < points.size() ? points[seg] : 1e9);
        if (b <= a) continue;
        QRectF area(QPointF(xOf(a), bar.top()), QPointF(xOf(b), bar.bottom() + 1));
        area = area.intersected(QRectF(kHeaderW, 0, width() - kHeaderW, height()));
        const double speed = Retime::segmentSpeed(c, seg);
        const QString text = QString("%1%2 ▾").arg(c.reverse ? "-" : "").arg(percentText(speed));
        if (fm.horizontalAdvance(text) + 6 > area.width()) continue;
        p.setPen(speed == 1.0 ? QColor(0xe8, 0xe8, 0xe8) : QColor(0xff, 0xc8, 0x5a));
        p.drawText(area, Qt::AlignCenter, text);
    }
    // Speed-Punkte: Linie über den Clip, Griff in der Leiste
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < points.size(); ++i) {
        double x = xOf(points[i]);
        if (dragging && i == m_rampDrag.point) x += m_rampDelta * m_view.pxPerFrame;
        if (x < r.left() || x > r.right()) continue;
        p.setPen(QPen(QColor(0xf0, 0xf0, 0xf0, 200), 1));
        p.drawLine(QPointF(x, bar.top()), QPointF(x, r.bottom()));
        QPolygonF handle;
        handle << QPointF(x - 4, bar.top() + 1) << QPointF(x + 4, bar.top() + 1) << QPointF(x + 4, bar.bottom() - 4)
               << QPointF(x, bar.bottom()) << QPointF(x - 4, bar.bottom() - 4);
        p.setPen(QPen(QColor(0, 0, 0, 180), 1));
        p.setBrush(dragging && i == m_rampDrag.point ? Theme::primary : Theme::secondary);
        p.drawPolygon(handle);
    }
    p.restore();
    if (dragging && m_rampDelta != 0 && m_rampDrag.point < points.size()) {
        const double x = xOf(points[m_rampDrag.point]) + m_rampDelta * m_view.pxPerFrame;
        drawLabel(p, QPoint(int(x) + 8, bar.top() - 22),
                  (m_rampDelta > 0 ? "+" : "") + Timecode::format(m_rampDelta, m_editor->project()->fps()));
    }
}

void TimelineView::segmentMenu(const RetimeHit& hit, int frame, const QPoint& globalPos)
{
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), hit.clipId);
    if (!c) return;
    const double current = Retime::segmentSpeed(*c, hit.segment);
    QMenu menu(this);
    QMenu* speeds = menu.addMenu(T("Geschwindigkeit ändern"));
    for (double s : {0.1, 0.25, 0.5, 0.75, 1.0, 1.1, 1.25, 1.5, 2.0, 4.0, 8.0}) {
        QAction* a = speeds->addAction(percentText(s));
        a->setCheckable(true);
        a->setChecked(std::abs(current - s) < 1e-9);
        connect(a, &QAction::triggered, this,
                [this, hit, s] { m_editor->setSegmentSpeed(hit.clipId, hit.segment, s); });
    }
    speeds->addSeparator();
    connect(speeds->addAction(T("Andere…")), &QAction::triggered, this, [this, hit, current] {
        bool ok = false;
        const double v = QInputDialog::getDouble(this, T("Geschwindigkeit ändern"), T("Geschwindigkeit (%):"),
                                                 current * 100, Retime::kMinSpeed * 100, Retime::kMaxSpeed * 100, 1,
                                                 &ok);
        if (ok) m_editor->setSegmentSpeed(hit.clipId, hit.segment, v / 100.0);
    });
    menu.addSeparator();
    QAction* add = menu.addAction(T("Speed-Punkt hinzufügen"));
    add->setEnabled(frame > c->start && frame < c->end());
    connect(add, &QAction::triggered, this, [this, id = hit.clipId, frame] { m_editor->addSpeedPoint(id, frame); });
    QAction* reset = menu.addAction(T("Geschwindigkeit zurücksetzen"));
    reset->setEnabled(c->isRetimed());
    connect(reset, &QAction::triggered, this, [this, id = hit.clipId] {
        m_editor->setClipSpeed({id}, Editor::Retime{}, true);
    });
    connect(menu.addAction(T("Retime-Steuerung ausblenden")), &QAction::triggered, this,
            [this, id = hit.clipId] { setRetimeControls({id}, false); });
    menu.exec(globalPos);
}

void TimelineView::speedPointMenu(const RetimeHit& hit, const QPoint& globalPos)
{
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), hit.clipId);
    if (!c || hit.point >= c->ramp.size()) return;
    const int current = c->ramp[hit.point].smooth;
    const int fps = m_editor->project()->fps();
    QMenu menu(this);
    QMenu* smooth = menu.addMenu(T("Weicher Übergang"));
    const struct { double seconds; const char* name; } options[] = {
        {0, N_("Aus (harter Wechsel)")}, {0.25, N_("0,25 Sekunden")}, {0.5, N_("0,5 Sekunden")},
        {1, N_("1 Sekunde")}, {2, N_("2 Sekunden")}};
    for (const auto& o : options) {
        const int frames = int(std::lround(o.seconds * fps));
        QAction* a = smooth->addAction(T(o.name));
        a->setCheckable(true);
        a->setChecked(current == frames);
        connect(a, &QAction::triggered, this,
                [this, hit, frames] { m_editor->setSpeedPointSmooth(hit.clipId, hit.point, frames); });
    }
    menu.addSeparator();
    connect(menu.addAction(T("Speed-Punkt entfernen")), &QAction::triggered, this,
            [this, hit] { m_editor->removeSpeedPoint(hit.clipId, hit.point); });
    menu.exec(globalPos);
}
