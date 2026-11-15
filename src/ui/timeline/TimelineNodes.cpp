// SPDX-License-Identifier: AGPL-3.0-only
// Time ranges of compositing nodes in the timeline (0.22): thin bars at the bottom of a clip with a graph, one per node
// with a range (text, an image laid in, a blur for a few seconds). A bar moves like a title, its ends trim it (snapping
// to the playhead, cuts and markers), each drag is one undo step (Editor::setCompNodeRange); a double-click opens the
// node on the Comp page. At most kMaxNodeBars bars, the rest as "+n"; a clip too low for them shows a dot.
#include "app/Theme.h"
#include "core/CompGraph.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompNodes.h"
#include "ui/timeline/TimelineView.h"
#include "ui/timeline/TimelineViewDetail.h"

using namespace TimelineViewDetail;

#include <QFileInfo>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {

// std::clamp with the lower bound above the upper one is undefined (a hard stop in builds with library assertions):
// a bar of zero width or a range outside its clip gives such bounds - the lower bound wins then
int bound(int v, int lo, int hi)
{
    return std::max(lo, std::min(v, hi));
}

constexpr int kMaxNodeBars = 3;
constexpr int kNodeBarH = 4;
constexpr int kNodeBarGap = 2;
constexpr int kNodeBarsMinBody = 20; // body height from which bars are drawn (else a dot)

double fpsOf(const Editor* ed) { return ed->project()->format().rate.fps(); }

// The node's name as shown: its kind, a text node's words or a media node's file
QString nodeLabel(const EffectInstance& e, const Comp::NodeRange& r)
{
    const Picture::Comp::NodeType* k = Picture::Comp::find(r.type);
    const QString kind = k ? k->name : r.type;
    const QString text = e.params.value(CompGraph::paramKey(r.node, QStringLiteral("text"))).toString().simplified();
    if ((r.type == QLatin1String(Comp::kTextType) || r.type == QLatin1String("text3d")) && !text.isEmpty())
        return QStringLiteral("%1 „%2“").arg(kind, text.left(24));
    const QString path = e.params.value(CompGraph::paramKey(r.node, QString::fromLatin1(Comp::kPathKey))).toString();
    if (r.type == QLatin1String(Comp::kMediaType) && !path.isEmpty())
        return QStringLiteral("%1 %2").arg(kind, QFileInfo(path).fileName());
    return kind;
}

} // namespace

QVector<TimelineView::NodeBar> TimelineView::nodeBars(const QRect& r, const Clip& c, int* more, QRect* dot) const
{
    QVector<NodeBar> bars;
    if (more) *more = 0;
    if (dot) *dot = {};
    const EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
    if (!e || !e->enabled) return bars;
    // Keyframed from/until: the range at the playhead, as the engine shows it there
    const int t = std::clamp(m_playhead - c.start, 0, std::max(0, c.length() - 1));
    const QVector<Comp::NodeRange> ranges = Comp::nodeRanges(*e, &c, t);
    if (ranges.isEmpty()) return bars;
    const QRect body = clipBodyRect(r);
    if (body.height() < kNodeBarsMinBody || r.width() < 8) { // too low (or narrow): a dot says "there are some"
        if (dot && r.width() >= 10) *dot = QRect(r.left() + 3, r.bottom() - 7, 5, 5);
        return bars;
    }
    const double fps = fpsOf(m_editor);
    const int shown = std::min<int>(kMaxNodeBars, int(ranges.size()));
    for (int i = 0; i < shown; ++i) {
        NodeBar b;
        b.node = ranges[i].node;
        b.label = nodeLabel(*e, ranges[i]);
        b.keyed = ranges[i].keyed;
        Comp::rangeFrames(c, ranges[i], fps, &b.first, &b.end);
        // The bar being dragged shows where it goes
        if (m_drag == Drag::NodeRange && m_nodeDrag.clipId == c.id && m_nodeDrag.node == b.node) {
            b.first = m_nodeFirst;
            b.end = m_nodeEnd;
        }
        const int x0 = int(std::floor(r.left() + (b.first - c.start) * m_view.pxPerFrame));
        const int x1 = int(std::ceil(r.left() + (b.end - c.start) * m_view.pxPerFrame));
        const int y = body.bottom() - 2 - (i + 1) * kNodeBarH - i * kNodeBarGap + 1;
        b.rect = QRect(x0, y, std::max(3, x1 - x0), kNodeBarH).intersected(r.adjusted(1, 0, -1, 0));
        if (b.rect.width() < 3) b.rect = QRect(bound(x0, r.left() + 1, r.right() - 3), y, 3, kNodeBarH);
        bars << b;
    }
    if (more) *more = int(ranges.size()) - shown;
    return bars;
}

void TimelineView::drawNodeBars(QPainter& p, const QRect& r, const Clip& c)
{
    int more = 0;
    QRect dot;
    const QVector<NodeBar> bars = nodeBars(r, c, &more, &dot);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    if (!dot.isEmpty()) {
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.setBrush(Theme::secondary);
        p.drawEllipse(QRectF(dot).adjusted(0.5, 0.5, -0.5, -0.5));
    }
    const QRect body = clipBodyRect(r);
    for (const NodeBar& b : bars) {
        const bool active = (m_drag == Drag::NodeRange && m_nodeDrag.clipId == c.id && m_nodeDrag.node == b.node)
                            || (m_nodeHover.clipId == c.id && m_nodeHover.node == b.node);
        // A dark band under it keeps it readable on the filmstrip
        p.fillRect(QRect(body.left(), b.rect.top() - 1, body.width(), b.rect.height() + 2), QColor(0, 0, 0, 70));
        p.setPen(QPen(QColor(0, 0, 0, 170), 1));
        p.setBrush(active ? Theme::primary : b.keyed ? Theme::textDim : Theme::secondary); // keyed: not draggable
        p.drawRoundedRect(QRectF(b.rect).adjusted(0.5, -0.5, -0.5, 0.5), 2, 2);
    }
    if (more > 0 && !bars.isEmpty()) { // the rest as "+n" above the bars
        QFont f = font();
        f.setPointSizeF(6.5);
        f.setBold(true);
        p.setFont(f);
        const QString text = QStringLiteral("+%1").arg(more);
        const QFontMetrics fm(f);
        const int w = fm.horizontalAdvance(text) + 6, h = fm.height();
        const QRect tag(std::max(r.left(), kHeaderW) + 3, bars.last().rect.top() - h - 2, w, h);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(tag, 2, 2);
        p.setPen(Theme::secondary);
        p.drawText(tag, Qt::AlignCenter, text);
    }
    p.restore();
    // While dragging: where it starts and how long (like a fade's label)
    if (m_drag == Drag::NodeRange && m_nodeDrag.clipId == c.id)
        for (const NodeBar& b : bars)
            if (b.node == m_nodeDrag.node) {
                const int fps = std::max(1, m_editor->project()->fps());
                drawLabel(p, QPoint(b.rect.left(), b.rect.top() - 18),
                          QStringLiteral("%1  (%2)").arg(Timecode::format(b.first, fps),
                                                        Timecode::format(b.end - b.first, fps)));
            }
}

std::optional<TimelineView::NodeHit> TimelineView::nodeBarAt(const QPoint& pos) const
{
    if (m_tool != Tool::Select || pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row || row->ref.kind != TrackKind::Video || isLocked(*row)) return std::nullopt;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const QRect r = clipRect(*row, c);
        if (!r.contains(pos)) continue;
        for (const NodeBar& b : nodeBars(r, c)) {
            const QRect grab = b.rect.adjusted(-3, -1, 3, 1); // half the gap: two bars never share a row
            if (!grab.contains(pos)) continue;
            const int edge = std::min(5, std::max(2, b.rect.width() / 3));
            NodeHit h{c.id, b.node, 0, b.keyed};
            if (pos.x() <= b.rect.left() + edge) h.part = -1;
            else if (pos.x() >= b.rect.right() - edge) h.part = 1;
            return h;
        }
    }
    return std::nullopt;
}

QString TimelineView::nodeBarToolTip(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return {};
    const auto row = rowAt(pos.y());
    if (!row || row->ref.kind != TrackKind::Video) return {};
    const int fps = std::max(1, m_editor->project()->fps());
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const QRect r = clipRect(*row, c);
        if (!r.contains(pos)) continue;
        int more = 0;
        QRect dot;
        const QVector<NodeBar> bars = nodeBars(r, c, &more, &dot);
        const auto line = [&](const NodeBar& b) {
            return T("%1: %2 bis %3").arg(b.label, Timecode::format(b.first, fps), Timecode::format(b.end, fps));
        };
        for (const NodeBar& b : bars)
            if (b.rect.adjusted(-3, -1, 3, 1).contains(pos))
                return line(b) + QStringLiteral("\n")
                       + (b.keyed ? T("Der Zeitbereich hat Keyframes: der Balken zeigt ihn am Abspielkopf und lässt sich "
                                      "nicht ziehen (Keyframes von „Wirkt ab/bis“ entfernen)")
                                  : T("Ziehen verschiebt, die Enden kürzen; Doppelklick öffnet den Knoten auf der "
                                      "Compositing-Seite"));
        if (!dot.adjusted(-3, -3, 3, 3).contains(pos) && !(more > 0 && clipBodyRect(r).contains(pos)
                                                            && pos.y() >= clipBodyRect(r).bottom() - 24))
            continue;
        // The dot (clip too low) or the "+n": every node with a range
        const EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
        if (!e) continue;
        QStringList lines{T("Knoten mit Zeitbereich (höhere Spur zeigt sie als Balken):")};
        const int t = std::clamp(m_playhead - c.start, 0, std::max(0, c.length() - 1));
        for (const Comp::NodeRange& nr : Comp::nodeRanges(*e, &c, t)) {
            NodeBar b;
            b.label = nodeLabel(*e, nr);
            Comp::rangeFrames(c, nr, fpsOf(m_editor), &b.first, &b.end);
            lines << line(b);
        }
        return lines.join('\n');
    }
    return {};
}

int TimelineView::nodeSnapDelta(const QVector<int>& edges) const
{
    if (!m_snap) return 0;
    QVector<int> pts = snapPoints({});
    pts << m_editor->project()->timeline().markers;
    const double maxFrames = kSnapPx / m_view.pxPerFrame;
    int best = 0;
    double bestDist = maxFrames + 1;
    for (int p : pts)
        for (int e : edges)
            if (const double d = std::abs(p - e); d <= maxFrames && d < bestDist) {
                bestDist = d;
                best = p - e;
            }
    return best;
}

bool TimelineView::nodeBarPress(const QPoint& pos)
{
    const auto h = nodeBarAt(pos);
    if (!h || h->keyed) return false;
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), h->clipId);
    const EffectInstance* e = c ? EffectRegistry::instance(*c, Comp::kEffectId) : nullptr;
    Comp::NodeRange r;
    if (!e || !Comp::activeRange(*e, h->node, &r, c, 0) || r.keyed) return false;
    m_nodeFrom = r.from;
    m_nodeUntil = r.until;
    Selection* sel = m_editor->selection();
    if (!sel->contains(h->clipId)) {
        const QVector<int> group = m_editor->withLinked({h->clipId});
        sel->set(QSet<int>(group.begin(), group.end()));
    }
    m_nodeDrag = *h;
    Comp::rangeFrames(*c, r, fpsOf(m_editor), &m_nodeStartFirst, &m_nodeStartEnd);
    m_nodeFirst = m_nodeStartFirst;
    m_nodeEnd = m_nodeStartEnd;
    m_drag = Drag::NodeRange;
    update();
    return true;
}

void TimelineView::nodeBarDrag(const QPoint& pos)
{
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_nodeDrag.clipId);
    if (!c) return;
    const int dx = int(std::lround((pos.x() - m_pressPos.x()) / m_view.pxPerFrame));
    int first = m_nodeStartFirst, end = m_nodeStartEnd;
    if (m_nodeDrag.part == 0) {
        const int len = end - first;
        int d = bound(dx, c->start - first, c->end() - end);
        d += nodeSnapDelta({first + d, end + d});
        d = bound(d, c->start - first, c->end() - end);
        first += d;
        end = first + len;
    } else if (m_nodeDrag.part < 0) {
        first = bound(first + dx, c->start, end - 1);
        first = bound(first + nodeSnapDelta({first}), c->start, end - 1);
    } else {
        end = bound(end + dx, first + 1, c->end());
        end = bound(end + nodeSnapDelta({end}), first + 1, c->end());
    }
    if (first != m_nodeFirst || end != m_nodeEnd) {
        m_nodeFirst = first;
        m_nodeEnd = end;
        update();
    }
}

void TimelineView::nodeBarRelease()
{
    const NodeHit h = m_nodeDrag;
    m_nodeDrag = {};
    if (m_nodeFirst == m_nodeStartFirst && m_nodeEnd == m_nodeStartEnd) return; // a click: nothing changes
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), h.clipId);
    if (!c) return;
    // Only what was dragged changes: the start edge keeps the stored until (0 = to the end stays), the end edge the
    // stored from; a moved bar stays "to the end" when it was and still ends at the clip's end
    double from = 0, until = 0;
    Comp::rangeValues(*c, m_nodeFirst, m_nodeEnd, fpsOf(m_editor), &from, &until, h.part == 0 && m_nodeUntil == 0);
    if (h.part < 0 && m_nodeUntil >= 0) until = m_nodeUntil; // a stored end before the clip (head trimmed past it): as shown
    if (h.part > 0) from = m_nodeFrom;
    QString err;
    if (!m_editor->setCompNodeRange(h.clipId, h.node, from, until, &err)) emit message(err);
}

bool TimelineView::nodeBarDoubleClick(const QPoint& pos)
{
    const auto h = nodeBarAt(pos);
    if (!h) return false;
    emit compNodeOpenRequested(h->clipId, h->node);
    return true;
}

void TimelineView::setNodeHover(const QPoint& pos)
{
    NodeHit h;
    if (m_drag == Drag::None)
        if (const auto at = nodeBarAt(pos)) h = *at;
    if (h.clipId != m_nodeHover.clipId || h.node != m_nodeHover.node) {
        m_nodeHover = h;
        update();
    }
}
