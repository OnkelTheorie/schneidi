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
    // `picture`: the frame shown right now (while paused no new one comes until the playhead moves).
    // Without a parent on purpose: Qt fixes a window's surface (raster or GPU) when its native window object is made.
    // Under a native main window that happens in QWidget's constructor already (the attribute is inherited), before
    // the GPU picture (QRhiWidget) is a child - a raster surface, "No QRhi", the picture froze. The same with winId()
    // before the first show. The owner deletes it.
    FullScreenViewer(Engine* engine, const QImage& picture);

    // Full screen on `screen` (the one the main window is on), active, with the keyboard focus
    void showOn(QScreen* screen);
    const QImage& picture() const;
    QRectF pictureRect() const; // where the picture is drawn (empty without one)

    // Preview frames at the project size wanted: the screen is larger than the scaled-down preview, or zoomed in
    bool wantsFullSize() const;

signals:
    void exitRequested();
    void fullSizeWanted(bool full); // the zoom changed (Ctrl+wheel): see wantsFullSize()

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    class FrameView* m_view; // the picture (GPU or QPainter, see there)
    QTimer m_hideCursor; // the pointer goes away when the mouse rests
};
