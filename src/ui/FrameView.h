// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Shows one video frame, aspect-fitted (or zoomed 50/100/200 % and panned) on the theme's letterbox colour, with an
// optional ViewerOverlay on top. Two ways to draw, the same API for both:
//  - GPU: a QRhiWidget child (OpenGL on Linux, Direct3D 11 on Windows, Metal on macOS) uploads the frame into a
//    texture and draws one quad; the overlay is painted by a transparent widget above it
//  - Painter: QPainter::drawImage in paintEvent (today's way) – chosen automatically where no hardware RHI works
//    (offscreen/minimal platform, VM without GL, software rasteriser, RHI not compiled in, setup failure) or forced
//    with SCHNEIDI_VIEWER=painter resp. the setting viewer/renderer = "painter"
// Mouse: the overlay gets the events first; middle button drag pans, so does left drag while zoomed in when nothing
// else uses it (no dragData, no overlay hit). Ctrl+wheel zooms in steps.
//
// Later (picture engine on the GPU): the engine hands over a texture that lives in the viewer's QRhi instead of a
// QImage. The hook for that is the GPU surface's render(): it samples whatever texture `frameTexture()` returns; a
// setFrameTexture(QRhiTexture*, QSize) next to setFrame() only needs to set it and skip the upload (see FrameViewRhi).

#include <QImage>
#include <QPointer>
#include <QWidget>
#include <functional>

class QMimeData;
class ViewerOverlay;

class FrameView : public QWidget {
    Q_OBJECT
public:
    enum class Backend { Painter, Rhi };
    // Zoom steps (factor of the project frame size in physical pixels); 0 = fit into the widget
    static constexpr double kFit = 0.0;

    explicit FrameView(QWidget* parent = nullptr);
    ~FrameView() override;

    // The backend used (Painter after a GPU setup failure, too)
    Backend backend() const { return m_backend; }
    // Name of the GPU device resp. why the painter is used (shown in the log/about, tests)
    QString backendInfo() const { return m_backendInfo; }

    // Which backend a new view takes. `env`: SCHNEIDI_VIEWER, `setting`: viewer/renderer ("painter", "gpu" or empty =
    // auto), `platform`: QGuiApplication::platformName(), `rhiCompiled`: the build has the GPU path, `probe`: tries a
    // GPU device once (nullptr = none) and tells whether it is a software one. "gpu"/"rhi" forces the GPU even on a
    // software device (for testing); auto takes the painter there (a CPU rasteriser draws slower than QPainter and
    // would also make the whole window composite through it)
    struct Probe {
        bool ok = false;
        bool software = false;
        QString device;
    };
    static Backend chooseBackend(const QString& env, const QString& setting, const QString& platform, bool rhiCompiled,
                                 const std::function<Probe()>& probe, QString* why = nullptr);
    static bool rhiCompiled();
    // Linux/BSD: does the GPU view compose through Vulkan rather than OpenGL. `env`: SCHNEIDI_VIEWER_API ("vulkan" or
    // "opengl"), `setting`: viewer/gpuApi ("opengl" once Vulkan failed here), `platform`: QGuiApplication::
    // platformName(), `vulkanBuilt`: Qt and the build have Vulkan. Auto: Vulkan on X11 - with the NVIDIA driver every
    // OpenGL context costs ~150 ms and stalls the window system while it is made (the window's open animation
    // stutters), a Vulkan device is cheaper and does not. Wayland keeps OpenGL (not measured there).
    static bool preferVulkan(const QString& env, const QString& setting, const QString& platform, bool vulkanBuilt);
    // The app's start: views take the previous run's GPU check (settings) instead of making a device to check now;
    // checkCachedGpu() checks for real after the window is up (a device that went away: the painter) and stores the
    // answer for the next start. Without a stored check (first start) the view checks right away as always. The
    // picture is the same on both backends.
    static void setUseCachedGpuCheck(bool use);
    static void checkCachedGpu();
    // Took the stored check, the real one is still to come (tests)
    bool gpuPending() const { return m_gpuPending; }
    // The app's start on X11: a GPU view that took the stored check paints with the painter until the window is on
    // screen and makes its GPU surface then (attachDeferredSurfaces) - setting up the window's GPU composition
    // (OpenGL: two GL contexts, ~300 ms with the NVIDIA driver) no longer delays the first picture. prepareWindow()
    // before the window's first show() creates it as a GPU window already (only the native window type, no device):
    // Qt then switches the shown window to GPU composition in place instead of destroying and re-creating it (a
    // flash, and the maximized state). Other platforms make the surface right away as before.
    static void prepareWindow(QWidget* window);
    static void attachDeferredSurfaces();
    // The app's start, before the window is built: GPU setup that can run on a helper thread meanwhile (Vulkan's
    // drivers and a first device); nothing when views will not use the GPU
    static void warmUpGpu();
    // GPU views compose through Vulkan in this run (preferVulkan; false after a Vulkan failure or without RHI)
    static bool composesWithVulkan();
    // Waits for attachDeferredSurfaces (tests)
    bool surfaceDeferred() const;
    // The GPU surface exists and has its QRhi (it rendered at least once; tests)
    bool gpuSurfaceReady() const;

    // The frame to show (implicitly shared, no copy). Its pixel size may be smaller than the project frame (preview
    // scale): the zoom refers to frameSize()
    void setFrame(const QImage& image);
    const QImage& frame() const { return m_image; }
    // Project frame size the zoom steps refer to; empty = the image's size
    void setFrameSize(const QSize& size);
    QSize frameSize() const;

    void setBackground(const QColor& color);
    QColor background() const { return m_background; }

    // Zoom: kFit or a factor (0.5, 1, 2 …). setZoom keeps the pan where possible (clamped to the new size)
    void setZoom(double zoom);
    double zoom() const { return m_zoom; }
    // Pan: offset of the frame's centre from the widget's centre in widget pixels, clamped so the picture covers the
    // widget on an axis where it is larger and stays centred where it is smaller
    void setPan(const QPointF& pan);
    QPointF pan() const { return m_pan; }
    // Smooth (bilinear) unless zoomed to 200 % or more – then each pixel is a sharp square
    bool smoothScaling() const { return m_zoom == kFit || m_zoom < 2.0; }

    // Where the frame lies in widget coordinates (may reach beyond the widget when zoomed); empty without a frame
    QRectF frameRect() const;
    QPointF widgetToFrame(const QPointF& widgetPos) const; // in frameSize() pixels
    QPointF frameToWidget(const QPointF& framePos) const;

    void setOverlay(ViewerOverlay* overlay);
    ViewerOverlay* overlay() const;

    // Set = the picture can be dragged out (source viewer into the timeline)
    std::function<QMimeData*()> dragData;
    // Pointer over the picture where the overlay has nothing to grab (open hand for a source, blank in full screen);
    // hovering and the end of a pan come back to it
    void setIdleCursor(Qt::CursorShape shape);

signals:
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    friend class FrameViewRhiSurface;
    friend class FrameViewOverlayLayer;
    void paintOverlay(QPainter& p);
    void repaintPicture();
    void repaintOverlay();
    void fallBackToPainter(const QString& why); // the GPU surface failed
    void makeRhiSurface(bool intoShownWindow = false);
    QPointF clampedPan(const QPointF& pan) const;
    double scale() const; // widget pixels per frameSize() pixel

    Backend m_backend = Backend::Painter;
    QString m_backendInfo;
    QWidget* m_surface = nullptr;      // GPU: the QRhiWidget
    QWidget* m_overlayLayer = nullptr; // GPU: transparent widget above it that paints the overlay
    bool m_gpuPending = false;         // backend from the stored GPU check, the real one comes later
    QImage m_image;
    QSize m_frameSize;
    QColor m_background;
    double m_zoom = kFit;
    QPointF m_pan;
    QPointer<ViewerOverlay> m_overlay;
    bool m_overlayDrag = false;
    bool m_panning = false;
    Qt::CursorShape m_idleCursor = Qt::ArrowCursor;
    QPoint m_pressPos{-1, -1};
    QPointF m_panStart, m_panPressPos;
};
