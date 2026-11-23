// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The selected shape title (0.22, TitleKind::Shape) over the viewer on the Edit page: its box (turned with it), drag
// inside = move, the eight handles = size (around the centre; Shift on a corner keeps the shape's proportions), the knob
// above = rotate (Shift: 15 degree steps). A line has only its two ends: length and angle (Shift: 15 degree steps). Values change like the Inspector's (a keyframe at the playhead when the value
// is animated), one undo step per drag. A shape following a tracker shows no box (the tracker places it).

#include "ui/ViewerOverlay.h"

#include <QPointF>
#include <QTransform>
#include <functional>

class Editor;
class Selection;
struct Clip;

class ShapeOverlay : public ViewerOverlay {
    Q_OBJECT
public:
    // timelineShown: the viewer shows the timeline (not a Media Pool source); missing = always
    ShapeOverlay(Editor* editor, Selection* selection, std::function<int()> playhead, QObject* parent = nullptr,
                 std::function<bool()> timelineShown = {});

    void paint(QPainter& p, const QRectF& frame) override;
    std::optional<QRectF> paintedBounds(const QRectF& frame) const override;
    bool mousePress(QMouseEvent* e, const QRectF& frame) override;
    bool mouseMove(QMouseEvent* e, const QRectF& frame) override;
    bool mouseRelease(QMouseEvent* e, const QRectF& frame) override;
    Qt::CursorShape cursorAt(const QPointF& pos, const QRectF& frame) const override;

    // Phone: a selected text title gets a box too (drag inside = move, a corner = size around its centre), and every
    // grab zone is finger-sized. Off on the desktop (as before)
    void setTextTitles(bool on);
    bool textTitles() const { return m_textTitles; }

    // For tests: widget coordinates of the centre / a handle (0..7 clockwise from the top left, 8 = rotate knob)
    QPointF handlePos(int handle, const QRectF& frame) const;
    QPointF centerPos(const QRectF& frame) const;

private:
    enum Part { None = -1, Inside = 100 };
    const Clip* clip() const;
    int localFrame(const Clip& c) const;
    bool isLine() const; // a line: only its two ends are handles (length and angle)
    bool isText() const; // a text title (setTextTitles): no rotate knob, corners only, size = the font's
    double hit() const;
    // Project pixels -> widget; shape-local pixels (centre origin, not turned) -> project pixels
    bool transforms(const QRectF& frame, QTransform* toWidget, QTransform* local, double* hw, double* hh) const;
    int partAt(const QPointF& pos, const QRectF& frame) const;
    void set(const QVector<QPair<int, double>>& values); // AnimParam, value

    Editor* m_editor;
    Selection* m_selection;
    std::function<int()> m_playhead;
    std::function<bool()> m_timelineShown;
    int m_part = None;
    int m_clipId = 0;
    QPointF m_press; // project pixels
    double m_startX = 0, m_startY = 0, m_startW = 0, m_startH = 0;
    QTransform m_startLocal; // the shape's centre and rotation at the press (project pixels)
    bool m_textTitles = false;
    double m_startSize = 0, m_startDist = 0; // a text title's font size and the press's distance from its centre
};
