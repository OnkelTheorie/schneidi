// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Handles of the selected Comp node over the viewer picture (like ui/WindowOverlay on the Color page): a mask's shape
// (rectangle/ellipse: move by the middle, size by the corner; polygon and curve points; the two points of a gradient),
// a transform's position, a tracker's start points. Each drag = setCompNodeValuesAt with one merge key (one undo
// step; a value with keyframes gets one at the playhead). Positions are 0..1 of the clip picture, mapped through the
// clip's own transform like the Color page windows.

#include "ui/ViewerOverlay.h"

#include <QPointF>
#include <QString>
#include <QTransform>
#include <QVector>

class Editor;
struct Clip;

class CompOverlay : public ViewerOverlay {
    Q_OBJECT
public:
    explicit CompOverlay(Editor* editor, QObject* parent = nullptr);

    void setTarget(int clipId, int node, int playhead);
    void setVisible(bool on);
    void refresh() { emit changed(); }

    void paint(QPainter& p, const QRectF& frame) override;
    bool mousePress(QMouseEvent* e, const QRectF& frame) override;
    bool mouseMove(QMouseEvent* e, const QRectF& frame) override;
    bool mouseRelease(QMouseEvent* e, const QRectF& frame) override;
    Qt::CursorShape cursorAt(const QPointF& pos, const QRectF& frame) const override;

    struct Handle {
        enum Kind { Point, Size };
        Kind kind = Point;
        QString kx, ky; // the node values it moves (Size: w/h around the shape's x/y)
        QPointF at;     // clip picture pixels
        QString label;
    };
    QVector<Handle> handles() const;
    // Widget coordinates of handle i (tests)
    QPointF handlePos(int i, const QRectF& frame) const;

private:
    const Clip* clip() const;
    bool clipToWidget(const QRectF& frame, QTransform* t) const;
    QSizeF pictureSize() const;
    int handleAt(const QPointF& pos, const QRectF& frame) const;

    Editor* m_editor;
    int m_clipId = 0, m_node = 0, m_playhead = 0;
    bool m_visible = false;
    int m_drag = -1;
};
