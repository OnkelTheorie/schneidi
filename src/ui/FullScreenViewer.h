// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Full-screen viewer (DaVinci: Cinema Viewer): only the picture the engine shows, aspect-fitted on black, in a window
// of its own over a whole screen. Esc, a double click or closing the window ask to leave (exitRequested); the main
// window adds its playback actions, so their keys (Space, J/K/L, arrows …) work here too. The engine is not touched:
// playback goes on across the switch.

#include <QImage>
#include <QTimer>
#include <QWidget>

class Engine;
class QScreen;

class FullScreenViewer : public QWidget {
    Q_OBJECT
public:
    // `picture`: the frame shown right now (while paused no new one comes until the playhead moves)
    FullScreenViewer(Engine* engine, const QImage& picture, QWidget* parent = nullptr);

    // Full screen on `screen` (the one the main window is on), active, with the keyboard focus
    void showOn(QScreen* screen);
    const QImage& picture() const { return m_image; }
    QRectF pictureRect() const; // where the picture is drawn (empty without one)

signals:
    void exitRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    QImage m_image;
    QTimer m_hideCursor; // the pointer goes away when the mouse rests
};
