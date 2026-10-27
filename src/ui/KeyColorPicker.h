#pragma once
// Pipette for the Green Screen key color: while it is on, the viewer shows the selected clip alone and without its
// key (so the green is there to pick); next to the cursor a loupe shows the pixels around it magnified, taken from
// the frame at full resolution (not the scaled viewer picture: the exact green can be hit in a small viewer), and a
// square the color a click takes (the average around that point); right-click or Esc cancel. MainWindow grabs the picture (Engine::grabFrame of
// isolate()) and hands the color to the Inspector.

#include "core/Types.h"
#include "ui/ViewerOverlay.h"

#include <QColor>
#include <QImage>
#include <QPointF>

class KeyColorPicker : public ViewerOverlay {
    Q_OBJECT
public:
    // `picture`: the frame with the clip alone (RGBA, transparent where the clip does not cover it)
    explicit KeyColorPicker(const QImage& picture, QObject* parent = nullptr);

    // `tl` with only the video clip `clipId` left, as its key sees it: the stages before the Green Screen
    // (stabilization, Color page grade), without the key and the effects after it, full opacity, no fades or
    // transitions, its track shown. Empty timeline if the clip is not in it.
    static Timeline isolate(const Timeline& tl, int clipId);
    // Average color of the covered pixels (alpha >= 128) in a square of 2*radius+1 around `at` (image pixels);
    // invalid if none is covered
    static QColor sample(const QImage& rgba, const QPointF& at, int radius = kSampleRadius);
    // Loupe: the (2*radius+1)² pixels of `rgba` around `at` (image pixels; the pixel under it in the middle),
    // transparent outside the picture; drawn kLoupeZoom times as large with the middle pixel framed
    static constexpr int kLoupeRadius = 5, kLoupeZoom = 8, kSampleRadius = 3;
    static QImage loupe(const QImage& rgba, const QPointF& at, int radius = kLoupeRadius);
    // Where the loupe is drawn for the cursor at `pos` (beside it, flipped at the edges of `frame`)
    static QRectF loupeRect(const QPointF& pos, const QRectF& frame);

    void paint(QPainter& p, const QRectF& frame) override;
    bool mousePress(QMouseEvent* e, const QRectF& frame) override;
    Qt::CursorShape cursorAt(const QPointF& pos, const QRectF& frame) const override;
    void hover(const QPointF& pos, const QRectF& frame) override;
    void leave() override;

signals:
    void picked(const QColor& color);
    void cancelled();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override; // Esc anywhere in the app

private:
    QPointF imagePoint(const QPointF& pos, const QRectF& frame) const;

    QImage m_picture;
    QPointF m_hover{-1, -1}; // widget coordinates, x < 0 = not over the viewer
    QColor m_hoverColor;
};
