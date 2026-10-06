#pragma once
#include "core/TrimFrames.h"

#include <QImage>
#include <QSize>
#include <QWidget>

// Trim view in the viewer like DaVinci Resolve: while an edit is dragged the viewer shows the frames on both
// sides of it instead of the playhead frame. Two-up for ripple/roll/edge trims (left Out, right In),
// four-up for slip/slide (two large panes below, two small ones above, see TrimFrames). Each pane has an
// In/Out badge, the clip name and its source timecode; the trim amount is shown below.
class TrimView : public QWidget {
    Q_OBJECT
public:
    explicit TrimView(QWidget* parent = nullptr);

    void setFps(int fps) { m_fps = std::max(1, fps); }
    void setAspect(const QSize& frameSize); // project format (pane shape)
    // New frames to show. A pane that still shows the same clip keeps its last image until the new one arrives
    // (no flicker to black while dragging).
    void setView(const TrimFrames::View& view);
    const TrimFrames::View& view() const { return m_view; }
    // Decoded frame arrived: fills every pane that waits for exactly this frame. true = a pane took it.
    bool setImage(const QString& path, int frame, const QImage& image);
    // Pane shows the frame it asks for (not a stale one / black)
    bool isExact(bool small, int index) const;

    // Pane rectangles for the current size (images only, without labels); index = TrimFrames::View order
    QVector<QRect> mainRects() const;
    QVector<QRect> smallRects() const;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    struct Slot {
        TrimFrames::Pane pane;
        QImage image;
        bool exact = false;
    };
    void layoutRects(QVector<QRect>* main, QVector<QRect>* small) const;
    void drawSlot(class QPainter& p, const QRect& r, const Slot& s, bool large) const;
    static void assign(QVector<Slot>& list, const QVector<TrimFrames::Pane>& panes);

    TrimFrames::View m_view;
    QVector<Slot> m_main, m_small;
    QSize m_aspect{16, 9};
    int m_fps = 25;
};
