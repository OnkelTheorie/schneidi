// SPDX-License-Identifier: AGPL-3.0-only
// Timeline view: the In/Out range (I/O) in the ruler. A handle at each edge drags that point (In never past Out:
// clamped), the lower half of the shaded range moves the whole range (its length stays), Alt-drag on the ruler draws
// a new range. Dragging snaps to the playhead, clip edges and markers; Shift drags freely. A click without dragging
// still moves the playhead, like everywhere in the ruler. Right-click: In/Out point here, remove. Each drag is one
// undo step through Editor::setMarks.
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "ui/timeline/TimelineView.h"
#include "ui/timeline/TimelineViewDetail.h"

#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <cmath>

using namespace TimelineViewDetail;

namespace {

constexpr int kHandlePx = 5;  // grab distance of a handle (px each side of the edge)
constexpr int kHandleW = 8;   // drawn width of a handle (inside the range)
constexpr int kBarH = 16;     // height of the range's bar at the bottom of the ruler

} // namespace

TimelineView::MarkGrab TimelineView::markGrabAt(const QPoint& pos) const
{
    const Timeline& tl = m_editor->project()->timeline();
    if (pos.x() < kHeaderW || pos.y() < kRulerH - kBarH || pos.y() >= kRulerH) return MarkGrab::None;
    if (tl.markIn < 0 && tl.markOut < 0) return MarkGrab::None;
    const double xIn = tl.markIn >= 0 ? frameToX(tl.markIn) : -1e9;
    const double xOut = tl.markOut >= 0 ? frameToX(tl.markOut + 1) : 1e9;
    const double dIn = std::abs(pos.x() - xIn), dOut = std::abs(pos.x() - xOut);
    // A narrow range: the nearer handle (Out wins a tie right of the middle, so both stay reachable)
    if (dIn <= kHandlePx || dOut <= kHandlePx) return dIn < dOut || (dIn == dOut && pos.x() < (xIn + xOut) / 2) ? MarkGrab::In : MarkGrab::Out;
    // The whole range moves only with both points set (else its length is open) and only from the bar's lower
    // half: the upper part of the ruler keeps scrubbing the playhead inside the range
    if (tl.markIn >= 0 && tl.markOut >= 0 && pos.x() > xIn && pos.x() < xOut && pos.y() >= kRulerH - kBarH / 2)
        return MarkGrab::Range;
    return MarkGrab::None;
}

void TimelineView::drawMarks(QPainter& p)
{
    const Timeline& tl = m_editor->project()->timeline();
    if (tl.markIn < 0 && tl.markOut < 0) return;
    const double xIn = tl.markIn >= 0 ? frameToX(tl.markIn) : kHeaderW - 1;
    const double xOut = tl.markOut >= 0 ? frameToX(tl.markOut + 1) : width() + 1;
    const MarkGrab lit = m_drag == Drag::Marks ? m_markGrab : m_markHover;
    const bool rangeLit = lit == MarkGrab::Range || lit == MarkGrab::Create;
    const double top = kRulerH - kBarH;
    p.fillRect(QRectF(xIn, top, xOut - xIn, kBarH), Theme::alpha(Theme::secondary, rangeLit ? 70 : 45));
    // Grip in the middle of the lower half: the range can be moved from there
    if (tl.markIn >= 0 && tl.markOut >= 0 && xOut - xIn > 4 * kHandleW + 12) {
        const double cx = (xIn + xOut) / 2, cy = kRulerH - kBarH / 4.0;
        for (int i = -1; i <= 1; ++i)
            p.fillRect(QRectF(cx + i * 4 - 1, cy - 1, 2, 2), Theme::alpha(Theme::secondary, rangeLit ? 255 : 170));
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    auto handle = [&](double x, bool in, bool hot) {
        // Edge line plus a tab on the inside of the range
        p.fillRect(QRectF(in ? x : x - 2, top, 2, kBarH), Theme::secondary);
        QPainterPath tab;
        const QRectF r(in ? x : x - kHandleW, top + 1, kHandleW, kBarH - 5);
        tab.addRoundedRect(r, 2, 2);
        p.fillPath(tab, hot ? Theme::secondary.lighter(135) : Theme::secondary);
        p.setPen(QPen(Theme::secondary.darker(170), 1));
        for (int i = 0; i < 2; ++i) { // two ridges: something to grab
            const double rx = r.left() + 3 + i * 2 + 0.5;
            p.drawLine(QPointF(rx, r.top() + 3), QPointF(rx, r.bottom() - 3));
        }
    };
    if (tl.markIn >= 0) handle(xIn, true, lit == MarkGrab::In);
    if (tl.markOut >= 0) handle(xOut, false, lit == MarkGrab::Out);
    p.restore();
}

int TimelineView::marksSnap(const QVector<int>& edges, Qt::KeyboardModifiers mods) const
{
    if (!m_snap || (mods & Qt::ShiftModifier)) return 0;
    // The usual snap points (playhead, clip and subtitle edges) plus the markers
    QVector<int> pts = snapPoints({});
    pts += m_editor->project()->timeline().markers;
    const double maxFrames = kSnapPx / m_view.pxPerFrame;
    int best = 0;
    double bestDist = maxFrames + 1;
    for (int pt : pts)
        for (int e : edges)
            if (const double d = std::abs(pt - e); d <= maxFrames && d < bestDist) {
                bestDist = d;
                best = pt - e;
            }
    return best;
}

bool TimelineView::marksPress(const QPoint& pos, Qt::KeyboardModifiers mods)
{
    MarkGrab grab = markGrabAt(pos);
    if (grab == MarkGrab::None && (mods & Qt::AltModifier) && pos.x() >= kHeaderW) grab = MarkGrab::Create;
    if (grab == MarkGrab::None) return false;
    const Timeline& tl = m_editor->project()->timeline();
    m_markGrab = grab;
    m_markMoved = false;
    m_markStartIn = tl.markIn;
    m_markStartOut = tl.markOut;
    m_markAnchor = std::max(0, int(std::lround(xToFrame(pos.x()))));
    if (grab == MarkGrab::Create) m_markAnchor += marksSnap({m_markAnchor}, mods);
    static int drags = 0; // over all views: a new view must not merge into an older drag's undo step
    m_markDrags = ++drags;
    m_drag = Drag::Marks;
    if (grab == MarkGrab::Range) setCursor(Qt::ClosedHandCursor);
    update(0, 0, width(), kRulerH);
    return true;
}

void TimelineView::marksDrag(const QPoint& pos, Qt::KeyboardModifiers mods)
{
    if (!m_markMoved) {
        if (std::abs(pos.x() - m_pressPos.x()) < kDragStartPx) return;
        m_markMoved = true;
    }
    // From the press point (auto-scroll shifts it along): the grabbed edge keeps its offset to the mouse
    const int delta = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
    const QString key = QString("marks-drag-%1").arg(m_markDrags);
    int in = m_markStartIn, out = m_markStartOut;
    switch (m_markGrab) {
    case MarkGrab::In: { // the edge is the first frame inside the range; never past Out (clamped)
        int edge = std::max(0, in + delta);
        edge += marksSnap({edge}, mods);
        in = std::max(0, edge);
        if (out >= 0) in = std::min(in, out);
        break;
    }
    case MarkGrab::Out: { // the edge lies after the last frame inside the range
        int edge = std::max(1, out + 1 + delta);
        edge += marksSnap({edge}, mods);
        out = std::max(0, edge - 1);
        if (in >= 0) out = std::max(out, in);
        break;
    }
    case MarkGrab::Range: { // both edges move, either one snaps; the length stays
        int d = std::max(delta, -in);
        d += marksSnap({in + d, out + 1 + d}, mods);
        d = std::max(d, -in);
        in += d;
        out += d;
        break;
    }
    case MarkGrab::Create: {
        int edge = std::max(0, m_markAnchor + delta);
        edge += marksSnap({edge}, mods);
        edge = std::max(0, edge);
        if (edge == m_markAnchor) return; // no range yet
        in = std::min(edge, m_markAnchor);
        out = std::max(edge, m_markAnchor) - 1;
        break;
    }
    case MarkGrab::None: return;
    }
    m_editor->setMarks(in, out, key);
    update(0, 0, width(), kRulerH);
    QToolTip::showText(mapToGlobal(QPoint(pos.x(), kRulerH)), marksToolTip(m_markGrab), this);
}

void TimelineView::marksRelease()
{
    // A click (no drag) moves the playhead as everywhere else in the ruler
    if (!m_markMoved) emit seekRequested(std::max(0, int(std::lround(xToFrame(m_pressPos.x())))));
    m_markGrab = MarkGrab::None;
    m_markMoved = false;
    update(0, 0, width(), kRulerH);
}

QString TimelineView::marksToolTip(MarkGrab grab) const
{
    const Timeline& tl = m_editor->project()->timeline();
    const int fps = std::max(1, m_editor->project()->fps());
    const bool both = tl.markIn >= 0 && tl.markOut >= 0;
    const QString length = both ? Timecode::format(tl.markOut - tl.markIn + 1, fps) : QString();
    switch (grab) {
    case MarkGrab::In:
        if (tl.markIn < 0) return {};
        return both ? T("In-Punkt %1 – Länge %2").arg(Timecode::format(tl.markIn, fps), length)
                    : T("In-Punkt %1").arg(Timecode::format(tl.markIn, fps));
    case MarkGrab::Out:
        if (tl.markOut < 0) return {};
        return both ? T("Out-Punkt %1 – Länge %2").arg(Timecode::format(tl.markOut, fps), length)
                    : T("Out-Punkt %1").arg(Timecode::format(tl.markOut, fps));
    case MarkGrab::Range:
    case MarkGrab::Create:
        if (!both) return {};
        return T("In/Out %1 – %2, Länge %3").arg(Timecode::format(tl.markIn, fps), Timecode::format(tl.markOut, fps), length);
    case MarkGrab::None: break;
    }
    return {};
}

void TimelineView::addMarksActions(QMenu& menu, int frame)
{
    connect(menu.addAction(T("In-Punkt hier setzen")), &QAction::triggered, this, [this, frame] { m_editor->setMarkIn(frame); });
    connect(menu.addAction(T("Out-Punkt hier setzen")), &QAction::triggered, this, [this, frame] { m_editor->setMarkOut(frame); });
    const Timeline& tl = m_editor->project()->timeline();
    QAction* clear = menu.addAction(T("In/Out entfernen"));
    clear->setEnabled(tl.markIn >= 0 || tl.markOut >= 0);
    connect(clear, &QAction::triggered, this, [this] { m_editor->clearMarks(); });
    // No "loop playback in the range" here: the player has no loop mode yet
}
