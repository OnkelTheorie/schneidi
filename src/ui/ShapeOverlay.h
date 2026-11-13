// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The selected shape title (0.22, TitleKind::Shape) over the viewer on the Edit page: its box (turned with it), drag
// inside = move, the eight handles = size (around the centre; Shift on a corner keeps the shape's proportions), the knob
// above = rotate (Shift: 15 degree steps). Values change like the Inspector's (a keyframe at the playhead when the value
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
    ShapeOverlay(Editor* editor, Selection* selection, std::function<int()> playhead, QObject* parent = nullptr);

    void paint(QPainter& p, const QRectF& frame) override;
    bool mousePress(QMouseEvent* e, const QRectF& frame) override;
    bool mouseMove(QMouseEvent* e, const QRectF& frame) override;
    bool mouseRelease(QMouseEvent* e, const QRectF& frame) override;
    Qt::CursorShape cursorAt(const QPointF& pos, const QRectF& frame) const override;

    // For tests: widget coordinates of the centre / a handle (0..7 clockwise from the top left, 8 = rotate knob)
    QPointF handlePos(int handle, const QRectF& frame) const;
    QPointF centerPos(const QRectF& frame) const;

private:
    enum Part { None = -1, Inside = 100 };
    const Clip* clip() const;
    int localFrame(const Clip& c) const;
    // Project pixels -> widget; shape-local pixels (centre origin, not turned) -> project pixels
    bool transforms(const QRectF& frame, QTransform* toWidget, QTransform* local, double* hw, double* hh) const;
    int partAt(const QPointF& pos, const QRectF& frame) const;
    void set(const QVector<QPair<int, double>>& values); // AnimParam, value

    Editor* m_editor;
    Selection* m_selection;
    std::function<int()> m_playhead;
    int m_part = None;
    int m_clipId = 0;
    QPointF m_press; // project pixels
    double m_startX = 0, m_startY = 0, m_startW = 0, m_startH = 0;
};
