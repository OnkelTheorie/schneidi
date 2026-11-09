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
    // `followEngine` false: the frames come only through setPicture (the second monitor: ui/SecondScreen holds the
    // timeline picture there while the main window's viewer shows a source clip)
    FullScreenViewer(Engine* engine, const QImage& picture, bool followEngine = true);

    // Full screen on `screen` (the one the main window is on). `activate`: active with the keyboard focus (the
    // second monitor's window does not take the focus from the main window)
    void showOn(QScreen* screen, bool activate = true);
    // A new frame; `frameSize` = the project format (zoom steps, letterboxing)
    void setPicture(const QImage& picture, const QSize& frameSize);
    const QImage& picture() const;
    QRectF pictureRect() const; // where the picture is drawn (empty without one)
    // Longer edge of the preview frames this window wants: the screen pixels the fitted picture covers (at most
    // kMaxPreviewEdge), the project size when zoomed in to 100 % or more (Ctrl+wheel). Engine::setPreviewEdge
    int wantedPreviewEdge() const;
    static constexpr int kMaxPreviewEdge = 1920;
    // The cap of wantedPreviewEdge when fitted (kMaxPreviewEdge; the second monitor: none, its real pixels)
    void setPreviewEdgeCap(int px) { m_edgeCap = px; }

signals:
    void exitRequested();
    void previewEdgeWanted(int px); // the zoom changed: see wantedPreviewEdge()

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    class FrameView* m_view; // the picture (GPU or QPainter, see there)
    int m_edgeCap = kMaxPreviewEdge;
    QTimer m_hideCursor; // the pointer goes away when the mouse rests
};
