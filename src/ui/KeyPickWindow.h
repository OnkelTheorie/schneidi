#pragma once
// Window of the Green Screen pipette: the clip's picture large – as large as the screen allows with a margin, in the
// picture's shape – with the pipette (KeyColorPicker) over it, so the exact green is easy to hit. A click picks,
// right-click or Esc cancel (the picker's signals); closing the window asks to cancel (closeRequested). MainWindow
// opens it over the main window and closes it when the pick ends.

#include <QPointer>
#include <QWidget>

class KeyColorPicker;
class QScreen;

class KeyPickWindow : public QWidget {
    Q_OBJECT
public:
    // `picture`: size of the picker's frame (its aspect ratio sets the window's shape)
    KeyPickWindow(KeyColorPicker* picker, const QSize& picture, QWidget* parent = nullptr);

    // Centered on `screen`, active, with the keyboard focus
    void showOn(QScreen* screen);
    QRectF pictureRect() const; // where the picture is drawn
    KeyColorPicker* picker() const { return m_picker; }
    // Window size for a picture on a screen area: the picture's shape, at most kScreenShare of the area
    static QSize sizeFor(const QSize& picture, const QSize& available);
    static constexpr double kScreenShare = 0.9;

signals:
    void closeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    QPointer<KeyColorPicker> m_picker;
    QSize m_picture;
};
