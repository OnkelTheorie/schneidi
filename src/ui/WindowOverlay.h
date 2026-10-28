// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Outline of the selected window of the Color page in the viewer, like DaVinci's on-screen window controls: the shape
// (solid) and its soft edge (dashed), drag inside = move, the eight handles = size (around the center), the knob above
// = rotate. Changes go through Editor::setGradeValues like the panel's fields (one undo step per drag, keyframed
// values get a keyframe at the playhead).

#include "ui/ViewerOverlay.h"

#include <QPointF>
#include <QTransform>
#include <QVector>
#include <functional>

class Editor;
struct Clip;

class WindowOverlay : public ViewerOverlay {
    Q_OBJECT
public:
    // targets: clips the Color page acts on (the first one is the shown clip)
    WindowOverlay(Editor* editor, std::function<QVector<int>()> targets, QObject* parent = nullptr);

    // Shown clip, window number (0 = none: nothing drawn) and playhead (timeline frame)
    void setTarget(int clipId, int window, int playhead);
    void setVisible(bool on);

    void paint(QPainter& p, const QRectF& frame) override;
    bool mousePress(QMouseEvent* e, const QRectF& frame) override;
    bool mouseMove(QMouseEvent* e, const QRectF& frame) override;
    bool mouseRelease(QMouseEvent* e, const QRectF& frame) override;
    Qt::CursorShape cursorAt(const QPointF& pos, const QRectF& frame) const override;

    // For tests: widget coordinates of the window center / a handle (0..7 clockwise from top left, 8 = rotate knob)
    QPointF centerPos(const QRectF& frame) const;
    QPointF handlePos(int handle, const QRectF& frame) const;

private:
    enum Part { None = -1, Inside = 100 };
    const Clip* clip() const;
    // Clip picture pixels -> widget; window-local pixels (center origin, unrotated) -> clip picture pixels
    bool transforms(const QRectF& frame, QTransform* clipToWidget, QTransform* localToClip, QSizeF* picture) const;
    int partAt(const QPointF& pos, const QRectF& frame) const;
    void set(const QVector<QPair<QString, double>>& values);

    Editor* m_editor;
    std::function<QVector<int>()> m_targets;
    int m_clipId = 0, m_window = 0, m_playhead = 0;
    bool m_visible = false;
    // Drag
    int m_part = None;
    QPointF m_pressClip;   // press position in clip picture pixels
    double m_startX = 0, m_startY = 0;
};
