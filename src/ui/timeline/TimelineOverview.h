// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/Types.h"

#include <QPixmap>
#include <QWidget>

class Project;

// Overview strip (~28 px): the whole of ONE timeline value at a glance - clips as blocks in their track colors,
// story sections, markers, the playhead and a rectangle for the range a detail view shows. Not tied to the project's
// current timeline or to a TimelineView: the owner feeds it (setTimeline / setPlayhead / setVisibleRange) and maps
// its requests onto its own view. TimelinePanel does that for the Edit page; another page can show a proposal
// timeline with it the same way.
//
// Input (left button): a click outside the rectangle = seekRequested there + scrollRequested so the detail view
// centres it (keep the button down and it drags on); a click inside the rectangle = seekRequested only; dragging the
// rectangle = scrollRequested; dragging one of its edges = rangeRequested (zoom, the other edge stays). It only
// asks - it never changes a view itself, and model changes (setTimeline) never emit anything (core rule: zoom and
// scroll change only by user input). No editing of clips.
//
// Cheap: the clips are drawn into a cached picture that is rebuilt only when the timeline, the size or the strip's
// scale changes; the playhead and the rectangle are drawn over it, a playhead step repaints only its old and new
// column (nothing when it stays in the same pixel).
class TimelineOverview : public QWidget {
    Q_OBJECT
public:
    static constexpr int kHeight = 28;
    static constexpr int kEdgeGrabPx = 4; // this close to an edge of the rectangle it zooms instead of scrolling

    explicit TimelineOverview(QWidget* parent = nullptr);

    // The timeline shown (a copy; its vectors are shared, so this is cheap) and its frame rate. project (optional,
    // must outlive the widget or be set again) gives the Media Pool clip colors.
    void setTimeline(const Timeline& timeline, double fps, const Project* project = nullptr);
    const Timeline& timeline() const { return m_timeline; }
    double fps() const { return m_fps; }

    void setPlayhead(int frame);
    int playhead() const { return m_playhead; }

    // The frames [from, to) a detail view shows (the rectangle); to <= from = no rectangle
    void setVisibleRange(double from, double to);
    double visibleFrom() const { return m_visFrom; }
    double visibleTo() const { return m_visTo; }

    // Room on the left before frame 0 (TimelinePanel: TimelineView::kHeaderW, so the strip lines up with the lanes)
    void setLeftInset(int px);
    int leftInset() const { return m_inset; }

    // Frames the strip's width stands for: the timeline's end, at least the visible range's end and 10 seconds.
    // Frozen while the mouse drags, so the strip does not rescale under the pointer.
    double scaleFrames() const;
    double frameToX(double frame) const;
    double xToFrame(double x) const;
    // The rectangle in widget coordinates (empty = none)
    QRectF visibleRect() const;

    QSize sizeHint() const override { return {400, kHeight}; }
    QSize minimumSizeHint() const override { return {100, kHeight}; }

signals:
    void seekRequested(int frame);
    // A move of the playhead (a click): started before its seekRequested, finished on release (TimelineView's)
    void playheadMoveStarted();
    void playheadMoveFinished();
    // Scroll the detail view so that its left edge is at `leftFrame` (its zoom stays)
    void scrollRequested(double leftFrame);
    // Show the frames [from, to) in the detail view (an edge of the rectangle dragged)
    void rangeRequested(double from, double to);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void focusOutEvent(QFocusEvent*) override;

private:
    enum class Drag { None, Move, LeftEdge, RightEdge };
    Drag hitTest(double x, bool touch) const; // touch: a finger's larger edge zones
    void pinchRange(const QPointF& from, const QPointF& to, double scale);
    bool m_pinching = false; // two fingers on the strip (its scale frozen like a drag's)
    void endDrag();
    double contentScale() const;
    void rebuildCache();
    QRect playheadColumn(int frame) const;

    Timeline m_timeline;
    int m_end = 0; // TimelineOps::endFrame of it
    double m_fps = 25;
    const Project* m_project = nullptr;
    int m_playhead = 0;
    double m_visFrom = 0, m_visTo = 0;
    int m_inset = 0;

    QPixmap m_cache;
    bool m_dirty = true;
    double m_cacheScale = 0;

    Drag m_drag = Drag::None;
    double m_dragScale = 0;  // scaleFrames() frozen at the press
    double m_grab = 0;       // Move: frames from the rectangle's left edge to the pointer
    double m_fixed = 0;      // edge drag: the frame of the edge that stays
    QPoint m_pressPos;
    bool m_moved = false;
    bool m_pressInside = false;
    bool m_movedPlayhead = false; // this press moved the playhead: playheadMoveFinished on release
};
