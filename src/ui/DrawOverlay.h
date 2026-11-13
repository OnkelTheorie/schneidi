// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Painting in the viewer on the Edit page (0.22, tool "Malen", key P): drawing titles (TitleKind::Drawing,
// core/Drawing.h). With the tool on, a drag paints a stroke with the brush (DrawBrush: colour, width, opacity) or the
// eraser removes the whole strokes it touches. A stroke goes into the selected drawing clip under the playhead; with
// none, the first stroke makes a new drawing clip at the playhead (5 s like a title, on the free track above the
// picture) and selects it, so the next strokes go there too. One undo step per stroke (an eraser drag is one step).
// Strokes are smoothed lightly when the pen lifts.
// With the tool off, the selected drawing gets a box: drag inside = move (title position), a corner = scale (the
// clip's zoom, around the box's middle). Everything else goes to the overlay below (the shape box).
// A drawing following a tracker is not painted into (a new drawing is made) and shows no box (the tracker places it).

#include "ui/ViewerOverlay.h"

#include <QColor>
#include <QPointF>
#include <QTransform>
#include <QVector>
#include <QWidget>
#include <functional>

class Editor;
class QSpinBox;
class QToolButton;
class Selection;
struct Clip;

// The brush of the paint tool, for the whole app (kept in the settings): colour, width as a fraction of the frame
// height (sharp in any resolution), opacity 0..1, eraser on
class DrawBrush : public QObject {
    Q_OBJECT
public:
    static DrawBrush& instance();
    QColor color() const { return m_color; }
    double width() const { return m_width; }
    double opacity() const { return m_opacity; }
    bool eraser() const { return m_eraser; }
    void setColor(const QColor& c);
    void setWidth(double w);
    void setOpacity(double o);
    void setEraser(bool on);

signals:
    void changed();

private:
    DrawBrush();
    void save() const;
    QColor m_color{255, 214, 0};
    double m_width = 0.008, m_opacity = 1;
    bool m_eraser = false;
};

class DrawOverlay : public ViewerOverlay {
    Q_OBJECT
public:
    // timelineShown: the viewer shows the timeline (not a Media Pool source); missing = always. below: the overlay
    // that gets what this one does not take (the shape box)
    DrawOverlay(Editor* editor, Selection* selection, std::function<int()> playhead, QObject* parent = nullptr,
                std::function<bool()> timelineShown = {}, ViewerOverlay* below = nullptr);

    void setActive(bool on);
    bool active() const { return m_active; }

    void paint(QPainter& p, const QRectF& frame) override;
    bool mousePress(QMouseEvent* e, const QRectF& frame) override;
    bool mouseMove(QMouseEvent* e, const QRectF& frame) override;
    bool mouseRelease(QMouseEvent* e, const QRectF& frame) override;
    Qt::CursorShape cursorAt(const QPointF& pos, const QRectF& frame) const override;
    void hover(const QPointF& pos, const QRectF& frame) override;
    void leave() override;
    bool tablet(QTabletEvent* e, const QRectF& frame) override;

    // The drawing clip the next stroke goes into (0 = a new one is made)
    int target() const;
    // For tests: the selected drawing's box in widget coordinates (tool off; empty = none)
    QRectF boxRect(const QRectF& frame) const;

signals:
    void activeChanged(bool on);
    void notice(const QString& text); // a short note over the picture (an eraser on a locked track)

private:
    enum Part { None = -1, Inside = 100 };
    const Clip* drawingAt() const; // the selected drawing under the playhead (following a tracker included)
    int localFrame(const Clip& c) const;
    bool locked(const Clip& c) const; // on a locked track
    // Project pixels -> widget through the clip's transform (no clip: the plain view)
    QTransform toWidget(const Clip* c, const QRectF& frame) const;
    // A widget point -> 0..1 of the frame in the drawing's own space (its title position and transform taken off)
    QPointF toDrawing(const Clip* c, const QPointF& widget, const QRectF& frame) const;
    QRectF localBox(const Clip& c) const; // the strokes' bounds in project pixels (title position included)
    int partAt(const QPointF& pos, const QRectF& frame) const;
    void commitStroke(const QRectF& frame);
    void extendStroke(const QPointF& pos, float pressure);
    void erase(const QRectF& frame);

    Editor* m_editor;
    Selection* m_selection;
    std::function<int()> m_playhead;
    std::function<bool()> m_timelineShown;
    ViewerOverlay* m_below;
    bool m_active = false;
    bool m_belowDrag = false;
    // painting: the stroke so far in widget coordinates and its pressure
    bool m_painting = false, m_erasing = false;
    bool m_tablet = false; // the stroke comes from a pen's tablet events
    QVector<QPointF> m_stroke;
    QVector<float> m_pressure;
    int m_eraseSerial = 0;
    bool m_lockedSaid = false; // this eraser drag said its track is locked
    QPointF m_hover;
    bool m_hovering = false;
    // the box: what is dragged and the values at the press
    int m_part = None;
    int m_clipId = 0;
    QPointF m_press;
    double m_startX = 0, m_startY = 0, m_startZoomX = 1, m_startZoomY = 1, m_startPosX = 0, m_startPosY = 0;
};

// The paint tool's bar over the viewer: brush / eraser, colour, width (pixels of the timeline format), opacity and
// a button that ends painting. frameHeight: the timeline's height in pixels
class DrawBar : public QWidget {
    Q_OBJECT
public:
    explicit DrawBar(std::function<int()> frameHeight, QWidget* parent = nullptr);
    void sync(); // the brush's values into the controls

signals:
    void closeWanted();

private:
    std::function<int()> m_frameHeight;
    QToolButton *m_brush, *m_eraser, *m_color;
    QSpinBox *m_width, *m_opacity;
};
