// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Window of the Green Screen pipette: the clip's picture large – as large as the screen allows with a margin, in the
// picture's shape – with the pipette (KeyColorPicker) over it, so the exact green is easy to hit. The mouse wheel
// zooms in around the cursor (up to kMaxZoom times the fitted picture), the middle button drags the picture. A click
// picks, right-click or Esc cancel (the picker's signals); closing the window asks to cancel (closeRequested).
// MainWindow opens it over the main window and closes it when the pick ends.

#include "ui/KeyColorPicker.h"

#include <QPointer>
#include <QWidget>

class QScreen;

class KeyPickWindow : public QWidget {
    Q_OBJECT
public:
    // `picture`: size of the picker's frame (its aspect ratio sets the window's shape)
    KeyPickWindow(KeyColorPicker* picker, const QSize& picture, QWidget* parent = nullptr);

    // Centered on `screen`, active, with the keyboard focus
    void showOn(QScreen* screen);
    QRectF pictureRect() const; // where the picture is drawn (larger than the window when zoomed in)
    double zoom() const { return m_zoom; } // 1 = the whole picture fits
    KeyColorPicker* picker() const { return m_picker; }
    // Window size for a picture on a screen area: the picture's shape, at most kScreenShare of the area
    static QSize sizeFor(const QSize& picture, const QSize& available);
    static constexpr double kScreenShare = 0.9;
    static constexpr double kMaxZoom = 32, kZoomStep = 1.25; // per wheel notch

signals:
    void closeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    double fitScale() const; // window pixels per picture pixel at zoom 1
    void clampCenter();

    QPointer<KeyColorPicker> m_picker;
    QSize m_picture;
    double m_zoom = 1;
    QPointF m_center;             // picture point in the middle of the window
    QPointF m_dragFrom{-1, -1};   // middle button: where the drag is (widget coordinates)
};
