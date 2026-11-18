// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Handles of the selected Comp node over the viewer picture (like ui/WindowOverlay on the Color page): a mask's shape
// (rectangle/ellipse: move by the middle, size by the corner; polygon and curve points; the two points of a gradient),
// a transform's position, a tracker's start points. Each drag = setCompNodeValuesAt with one merge key (one undo
// step; a value with keyframes gets one at the playhead). Positions are 0..1 of the clip picture, mapped through the
// clip's own transform like the Color page windows. A tracker's points are 0..1 of the clip's own picture (a 9:16 clip
// in a 16:9 frame: of the picture between the bars), as the tracker sees it.
// Picking a region (0.22, a mask node): while picking, a click on the picture makes the mask the outline of the even
// surface there (engine/RegionPick: replace), Shift-click adds another surface, Alt- (or Ctrl-) click takes one away
// (a new mask chained behind, combine add/subtract); each click one undo step. setPickTolerance picks the last click
// again with the new tolerance (the live check of the slider).

#include "ui/ViewerOverlay.h"

#include <QImage>
#include <QPointF>
#include <QString>
#include <QTransform>
#include <QVector>

#include <functional>

class Editor;
struct Clip;

class CompOverlay : public ViewerOverlay {
    Q_OBJECT
public:
    explicit CompOverlay(Editor* editor, QObject* parent = nullptr);

    void setTarget(int clipId, int node, int playhead);
    void setVisible(bool on);
    void refresh() { emit changed(); }
    void setPicking(bool on);
    bool picking() const { return m_picking; }
    void setPickTolerance(double tolerance);
    double pickTolerance() const { return m_tolerance; }
    // A pick at point (0..1 of the clip's own picture) as a click there would (tests, the CLI does the same with op
    // pick_region); returns the node holding the polygon, 0 = nothing picked (pickStatus says why)
    int pickAt(QPointF point, Qt::KeyboardModifiers modifiers);
    QString pickStatus() const { return m_pickStatus; }

    void paint(QPainter& p, const QRectF& frame) override;
    bool mousePress(QMouseEvent* e, const QRectF& frame) override;
    bool mouseMove(QMouseEvent* e, const QRectF& frame) override;
    bool mouseRelease(QMouseEvent* e, const QRectF& frame) override;
    Qt::CursorShape cursorAt(const QPointF& pos, const QRectF& frame) const override;

    struct Handle {
        enum Kind { Point, Size };
        Kind kind = Point;
        QString kx, ky; // the node values it moves (Size: w/h around the shape's x/y)
        QPointF at;     // clip picture pixels (the project frame the graph works in)
        QString label;
        // The values are 0..1 of the clip's own picture without the bars of another aspect ratio (a tracker's start
        // points, Tracker::pictureRect), not of the frame
        bool ownPicture = false;
    };
    QVector<Handle> handles() const;
    // Widget coordinates of handle i (tests)
    QPointF handlePos(int i, const QRectF& frame) const;

signals:
    void pickingChanged(bool on);
    void picked(const QString& status); // after every pick: what came out (translated)

private:
    const Clip* clip() const;
    int repick(); // the last pick again with m_tolerance
    QImage clipPicture(const Clip& c, int frame, int maxEdge); // RegionPick::clipPicture with one decoder per clip
    bool clipToWidget(const QRectF& frame, QTransform* t) const;
    QSizeF pictureSize() const;
    QRectF ownRect() const; // where the clip's own picture lies in the frame (project pixels)
    int handleAt(const QPointF& pos, const QRectF& frame) const;

    Editor* m_editor;
    int m_clipId = 0, m_node = 0, m_playhead = 0;
    bool m_visible = false;
    int m_drag = -1;
    bool m_picking = false;
    double m_tolerance;
    struct LastPick {
        int clipId = 0;
        int base = 0; // the mask selected when it was picked
        int node = 0; // the mask holding the polygon (base, or one chained behind it)
        int playhead = 0;
        QPointF point;
        QImage picture;
        QPointF placed{-1, -1}; // where it put the tracker's point (-1 = not)
    };
    LastPick m_last;
    // The undo step of the last repick (its stack index after it; -1 = none or something else came since): the next
    // repick of the same mask replaces it, so a run of tolerance changes is one undo step
    int m_repickIndex = -1;
    int m_repickNode = 0;
    QPointF m_repickPlaced{-1, -1}; // m_last.placed before that step
    bool m_inRepick = false;
    QString m_pickStatus;
    std::function<QImage(int, int)> m_source;
    QString m_sourceKey;
};
