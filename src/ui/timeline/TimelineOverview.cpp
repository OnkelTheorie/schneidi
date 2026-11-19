// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/timeline/TimelineOverview.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "core/TimelineOps.h"
#include "ui/timeline/TimelineColors.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kBandH = 4;      // story sections at the top
constexpr int kClickSlopPx = 3; // a press that moves less than this is a click
} // namespace

TimelineOverview::TimelineOverview(QWidget* parent) : QWidget(parent)
{
    setObjectName("timelineOverview");
    setFixedHeight(kHeight);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setToolTip(T("Überblick: Klick springt hin, Rechteck ziehen scrollt, Rand ziehen zoomt"));
}

void TimelineOverview::setTimeline(const Timeline& timeline, double fps, const Project* project)
{
    m_timeline = timeline;
    m_end = TimelineOps::endFrame(m_timeline);
    m_fps = fps > 0 ? fps : 25;
    m_project = project;
    m_dirty = true;
    update();
}

void TimelineOverview::setPlayhead(int frame)
{
    if (frame == m_playhead) return;
    const QRect before = playheadColumn(m_playhead), after = playheadColumn(frame);
    m_playhead = frame;
    if (before == after) return; // the same pixel: nothing to draw
    update(before);
    update(after);
}

void TimelineOverview::setVisibleRange(double from, double to)
{
    if (from == m_visFrom && to == m_visTo) return;
    m_visFrom = from;
    m_visTo = to;
    update(); // a new range may also change the scale (beyond the end); paintEvent checks
}

void TimelineOverview::setLeftInset(int px)
{
    px = std::max(0, px);
    if (px == m_inset) return;
    m_inset = px;
    m_dirty = true;
    update();
}

double TimelineOverview::contentScale() const
{
    return std::max({double(m_end), m_visTo, m_fps * 10, 1.0});
}

double TimelineOverview::scaleFrames() const
{
    return m_drag != Drag::None && m_dragScale > 0 ? m_dragScale : contentScale();
}

double TimelineOverview::frameToX(double frame) const
{
    const double w = std::max(1, width() - m_inset - 1);
    return m_inset + frame * w / scaleFrames();
}

double TimelineOverview::xToFrame(double x) const
{
    const double w = std::max(1, width() - m_inset - 1);
    return (x - m_inset) * scaleFrames() / w;
}

QRectF TimelineOverview::visibleRect() const
{
    if (m_visTo <= m_visFrom) return {};
    const double x0 = frameToX(m_visFrom), x1 = std::max(x0 + 2, frameToX(m_visTo));
    return QRectF(x0, 0, x1 - x0, height());
}

QRect TimelineOverview::playheadColumn(int frame) const
{
    return QRect(int(std::floor(frameToX(frame))) - 1, 0, 3, height());
}

TimelineOverview::Drag TimelineOverview::hitTest(double x) const
{
    const QRectF r = visibleRect();
    if (r.isEmpty()) return Drag::None;
    // A narrow rectangle: the edge zones shrink, the middle third stays for moving
    const double grab = std::min<double>(kEdgeGrabPx, r.width() / 3);
    if (std::abs(x - r.left()) <= grab && x < r.center().x()) return Drag::LeftEdge;
    if (std::abs(x - r.right()) <= grab && x >= r.center().x()) return Drag::RightEdge;
    if (x > r.left() && x < r.right()) return Drag::Move;
    return Drag::None;
}

void TimelineOverview::rebuildCache()
{
    const qreal dpr = devicePixelRatioF();
    if (m_cache.size() != size() * dpr) m_cache = QPixmap(size() * dpr);
    m_cache.setDevicePixelRatio(dpr);
    m_cacheScale = scaleFrames();
    m_dirty = false;

    QPainter p(&m_cache);
    const int h = height(), w = width();
    p.fillRect(QRect(0, 0, w, h), Theme::timelineBg);
    if (m_inset > 0) {
        p.fillRect(QRect(0, 0, m_inset, h), Theme::trackHeader);
        if (m_inset >= 60) {
            QFont f = font();
            f.setPointSizeF(f.pointSizeF() * 0.85);
            p.setFont(f);
            p.setPen(Theme::textDim);
            p.drawText(QRect(8, 0, m_inset - 12, h), Qt::AlignVCenter | Qt::AlignLeft, T("Überblick"));
        }
    }
    p.setClipRect(QRect(m_inset, 0, w - m_inset, h));

    // Story sections: a band at the top in the color of their kind
    for (const StorySection& s : m_timeline.sections) {
        const double x0 = frameToX(s.from), x1 = std::max(x0 + 1, frameToX(s.to));
        p.fillRect(QRectF(x0, 0, x1 - x0, kBandH - 1), TimelineColors::kindColor(s.kind));
    }

    // Tracks: video above audio (V1 at the bottom of its half, A1 at the top of its half, like the detail view)
    const int nv = m_timeline.video.size(), na = m_timeline.audio.size();
    const int top = kBandH, bottom = h - 1;
    const int split = nv && na ? top + (bottom - top) / 2 : nv ? bottom : top;
    auto drawRow = [&](const Track& track, TrackKind kind, double y0, double y1) {
        const int iy0 = int(std::lround(y0)), iy1 = std::max(iy0 + 1, int(std::lround(y1)) - (y1 - y0 >= 3 ? 1 : 0));
        // Clips narrower than a few pixels are merged into runs of the same color (2000 clips on 1500 px)
        int runX0 = 0, runX1 = -1;
        QColor runColor;
        auto flush = [&] {
            if (runX1 > runX0) p.fillRect(QRect(runX0, iy0, runX1 - runX0, iy1 - iy0), runColor);
            runX1 = -1;
        };
        for (const Clip& c : track.clips) {
            const QColor col = TimelineColors::clipBaseColor(c, kind, TimelineColors::clipColor(m_project, c, &track));
            const double fx0 = frameToX(c.start), fx1 = frameToX(c.end());
            const int x0 = int(std::floor(fx0)), x1 = std::max(x0 + 1, int(std::ceil(fx1)));
            if (x1 - x0 >= 4) { // wide enough to show where it starts
                flush();
                p.fillRect(QRect(x0, iy0, x1 - x0, iy1 - iy0), col);
                p.fillRect(QRect(x0, iy0, 1, iy1 - iy0), col.darker(160));
                continue;
            }
            if (runX1 >= x0 - 1 && col == runColor) {
                runX1 = std::max(runX1, x1);
                continue;
            }
            flush();
            runX0 = x0;
            runX1 = x1;
            runColor = col;
        }
        flush();
    };
    if (nv) {
        const double rowH = double(split - top) / nv;
        for (int i = 0; i < nv; ++i) {
            const double y1 = split - i * rowH;
            drawRow(m_timeline.video[i], TrackKind::Video, y1 - rowH, y1);
        }
    }
    if (na) {
        const double rowH = double(bottom - split) / na;
        for (int i = 0; i < na; ++i) {
            const double y0 = split + i * rowH;
            drawRow(m_timeline.audio[i], TrackKind::Audio, y0 + (nv ? 1 : 0), y0 + rowH);
        }
    }

    // Markers: small flags at the top
    p.setRenderHint(QPainter::Antialiasing);
    for (int m : m_timeline.markers) {
        const double x = frameToX(m);
        QPainterPath flag;
        flag.moveTo(x - 3, 0);
        flag.lineTo(x + 3, 0);
        flag.lineTo(x, 5);
        flag.closeSubpath();
        p.fillPath(flag, Theme::secondary);
    }
}

void TimelineOverview::paintEvent(QPaintEvent*)
{
    if (m_dirty || m_cache.size() != size() * devicePixelRatioF() || scaleFrames() != m_cacheScale) rebuildCache();
    QPainter p(this);
    p.drawPixmap(0, 0, m_cache);
    p.setClipRect(QRect(m_inset, 0, width() - m_inset, height()));
    if (const QRectF r = visibleRect(); !r.isEmpty()) {
        const QRectF box = r.adjusted(0.5, 0.5, -0.5, -0.5);
        p.fillRect(box, Theme::alpha(Theme::text, 34));
        p.setPen(QPen(Theme::alpha(Theme::text, 190), 1));
        p.drawRect(box);
    }
    const double px = std::floor(frameToX(m_playhead));
    p.fillRect(QRectF(px, 0, 1, height()), Theme::playhead);
}

void TimelineOverview::resizeEvent(QResizeEvent*)
{
    m_dirty = true;
}

void TimelineOverview::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || e->position().x() < m_inset) return QWidget::mousePressEvent(e);
    const double x = e->position().x();
    const Drag hit = hitTest(x);
    m_dragScale = contentScale(); // frozen until the release
    m_pressPos = e->position().toPoint();
    m_moved = false;
    m_pressInside = hit != Drag::None;
    const double len = m_visTo - m_visFrom;
    switch (hit) {
    case Drag::LeftEdge: m_fixed = m_visTo; break;
    case Drag::RightEdge: m_fixed = m_visFrom; break;
    case Drag::Move: m_grab = xToFrame(x) - m_visFrom; break;
    case Drag::None: {
        // Outside the rectangle: the playhead goes there and the detail view centres it; holding on drags
        const int frame = std::max(0, int(std::lround(xToFrame(x))));
        emit seekRequested(frame);
        if (len > 0) {
            const double left = std::max(0.0, frame - len / 2); // near the end it may reach past it (the view can)
            m_grab = frame - left;
            emit scrollRequested(left);
        }
        m_drag = len > 0 ? Drag::Move : Drag::None;
        if (m_drag == Drag::None) m_dragScale = 0;
        return;
    }
    }
    m_drag = hit;
}

void TimelineOverview::mouseMoveEvent(QMouseEvent* e)
{
    const double x = e->position().x();
    if (m_drag == Drag::None) {
        const Drag hit = x >= m_inset ? hitTest(x) : Drag::None;
        setCursor(hit == Drag::LeftEdge || hit == Drag::RightEdge ? Qt::SizeHorCursor
                  : hit == Drag::Move                              ? Qt::OpenHandCursor
                                                                   : Qt::ArrowCursor);
        return;
    }
    if (!m_moved && (e->position().toPoint() - m_pressPos).manhattanLength() < kClickSlopPx) return;
    m_moved = true;
    const double len = m_visTo - m_visFrom;
    const double minLen = std::max(1.0, m_fps * 0.2); // at least a few frames wide
    switch (m_drag) {
    case Drag::Move: {
        setCursor(Qt::ClosedHandCursor);
        const double left = std::clamp(xToFrame(x) - m_grab, 0.0, std::max(0.0, m_dragScale - len));
        emit scrollRequested(left);
        break;
    }
    case Drag::LeftEdge: {
        const double from = std::clamp(xToFrame(x), 0.0, m_fixed - minLen);
        emit rangeRequested(from, m_fixed);
        break;
    }
    case Drag::RightEdge: {
        const double to = std::max(xToFrame(x), m_fixed + minLen);
        emit rangeRequested(m_fixed, to);
        break;
    }
    case Drag::None: break;
    }
}

void TimelineOverview::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || m_drag == Drag::None) return QWidget::mouseReleaseEvent(e);
    // A click inside the rectangle (no drag): only the playhead moves, that frame is in view already
    if (!m_moved && m_pressInside) emit seekRequested(std::max(0, int(std::lround(xToFrame(e->position().x())))));
    m_drag = Drag::None;
    m_dragScale = 0;
    setCursor(Qt::ArrowCursor);
    update(); // the scale may follow the new range now
}

void TimelineOverview::leaveEvent(QEvent*)
{
    if (m_drag == Drag::None) unsetCursor();
}
