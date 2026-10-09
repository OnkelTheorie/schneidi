// Timeline: retime controls (speed bar, speed points, segment menus).
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
