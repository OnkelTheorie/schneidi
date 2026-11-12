// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/FrameView.h"

#include "app/Theme.h"
#include "core/StartupTrace.h"
#include "engine/CpuPath.h"
#include "ui/ViewerOverlay.h"
#ifdef SCHNEIDI_HAVE_RHI
#include "ui/FrameViewRhi.h"
#endif

#include <QApplication>
#include <QDrag>
#include <QGuiApplication>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

namespace {

// Zoom steps for Ctrl+wheel (fit sits wherever it falls between them)
constexpr std::array<double, 5> kSteps{0.25, 0.5, 1.0, 2.0, 4.0};

} // namespace

// Transparent widget above the GPU surface: paints the overlay (the QRhiWidget cannot be painted on with QPainter).
// Mouse events go through to the FrameView.
class FrameViewOverlayLayer : public QWidget {
public:
    explicit FrameViewOverlayLayer(FrameView* view) : QWidget(view), m_view(view)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        m_view->paintOverlay(p);
    }

private:
    FrameView* m_view;
};

bool FrameView::rhiCompiled()
{
#ifdef SCHNEIDI_HAVE_RHI
    return true;
#else
    return false;
#endif
}

bool FrameView::preferVulkan(const QString& env, const QString& setting, const QString& platform, bool vulkanBuilt)
{
    if (!vulkanBuilt) return false;
    for (const QString& wish : {env.trimmed().toLower(), setting.trimmed().toLower()}) {
        if (wish == "vulkan" || wish == "vk") return true;
        if (wish == "opengl" || wish == "gl") return false;
    }
    Q_UNUSED(platform); // OpenGL by default everywhere: the planned zero-copy display shares the engine's GL textures
    return false;
}

FrameView::Backend FrameView::chooseBackend(const QString& env, const QString& setting, const QString& platform,
                                            bool rhiCompiled, const std::function<Probe()>& probe, QString* why)
{
    auto result = [why](Backend b, const QString& reason) {
        if (why) *why = reason;
        return b;
    };
    const QString wish = (!env.isEmpty() ? env : setting).trimmed().toLower();
    if (wish == "painter" || wish == "cpu") return result(Backend::Painter, "painter requested");
    if (!rhiCompiled) return result(Backend::Painter, "built without RHI");
    // No GPU behind these platforms (tests, headless servers, screen sharing)
    static const QStringList headless{"offscreen", "minimal", "vnc", "linuxfb", "vkkhrdisplay"};
    if (headless.contains(platform)) return result(Backend::Painter, "platform " + platform);
    const bool forced = wish == "gpu" || wish == "rhi";
    const Probe p = probe ? probe() : Probe{};
    if (!p.ok) return result(Backend::Painter, "no RHI device");
    if (p.software && !forced) return result(Backend::Painter, "software device " + p.device);
    return result(Backend::Rhi, p.device);
}

namespace {
// Startup without the GPU check: the previous run's answer (settings), checked again after the window is up
bool g_cachedGpu = false;
constexpr const char* kProbeKey = "viewer/gpuCheck"; // "ok|software|device" of the last check
QList<QPointer<FrameView>>& cachedViews()
{
    static QList<QPointer<FrameView>> views;
    return views;
}
// X11 start: views whose GPU surface waits for the window's first picture (attachDeferredSurfaces); `g_deferring`
// ends when that window is about to show (prepareWindow; views made later make their surface right away)
bool g_deferring = true;
QList<QPointer<FrameView>>& deferredViews()
{
    static QList<QPointer<FrameView>> views;
    return views;
}
// Only where measured and checked: xcb (no flash, the maximized state stays; checked with Qt 6.8.2 - the in-place
// switch relies on QWidget::setParent of a widget asking for RHI into a window of the right surface type). Wayland
// makes another kind of native window for a GPU surface (the raster flush into it is untested); Windows/macOS have
// not shown the delay.
bool deferSurfaceAtStart()
{
    return QGuiApplication::platformName() == QLatin1String("xcb");
}
// The GPU forced on a software device too (SCHNEIDI_VIEWER/viewer/renderer "gpu"; tests)
bool gpuForced()
{
    const QString wish =
        qEnvironmentVariable("SCHNEIDI_VIEWER", QSettings().value("viewer/renderer").toString()).trimmed().toLower();
    return wish == "gpu" || wish == "rhi";
}
// The deferred views include one in this window: its switch to GPU composition is still to come
bool attachPending(const QWidget* window)
{
    return std::any_of(deferredViews().cbegin(), deferredViews().cend(),
                       [window](const QPointer<FrameView>& v) { return v && v->window() == window; });
}
std::optional<FrameView::Probe> cachedProbe()
{
    const QStringList parts = QSettings().value(kProbeKey).toString().split('|');
    if (parts.size() < 3) return std::nullopt;
    return FrameView::Probe{parts[0] == "1", parts[1] == "1", parts.mid(2).join('|')};
}
} // namespace

FrameView::FrameView(QWidget* parent) : QWidget(parent), m_background(Theme::viewerBg)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    std::function<Probe()> probe;
#ifdef SCHNEIDI_HAVE_RHI
    probe = [] { return FrameViewRhiSurface::probe(); };
    // The app's start: the last run's check instead of a device made and thrown away now (an OpenGL/D3D context,
    // 50-300 ms with the driver load); the real check follows after the window is up (checkCachedGpu)
    if (g_cachedGpu && !FrameViewRhiSurface::probed())
        if (const std::optional<Probe> cached = cachedProbe())
            probe = [this, p = *cached] {
                m_gpuPending = true; // only when the choice needed it (not for a painter wish or a headless platform)
                cachedViews() << this;
                return p;
            };
#endif
    // Compatibility mode (CpuPath): the painter unless SCHNEIDI_VIEWER asks for something else
    m_backend = chooseBackend(qEnvironmentVariable("SCHNEIDI_VIEWER"),
                              CpuPath::on() ? QStringLiteral("painter") : QSettings().value("viewer/renderer").toString(),
                              QGuiApplication::platformName(),
                              rhiCompiled(), probe, &m_backendInfo);
    if (m_backend != Backend::Rhi) return;
    if (m_gpuPending && g_deferring && deferSurfaceAtStart()) deferredViews() << this; // painter until then
    else if (deferSurfaceAtStart() && attachPending(window())) deferredViews() << this; // switches with the others
    else makeRhiSurface();
}

bool FrameView::gpuSurfaceReady() const
{
#ifdef SCHNEIDI_HAVE_RHI
    return m_surface && static_cast<FrameViewRhiSurface*>(m_surface)->hasRhi();
#else
    return false;
#endif
}

bool FrameView::surfaceDeferred() const
{
    return deferredViews().contains(const_cast<FrameView*>(this));
}

void FrameView::prepareWindow(QWidget* window)
{
    // Views made from now on (the window is about to show) make their surface right away as before
    g_deferring = false;
#ifdef SCHNEIDI_HAVE_RHI
    if (!window) return;
    bool needed = false;
    // Only this window's views wait: one in another window (not shown yet) makes its surface now, its window is
    // then created for the GPU when it shows
    QList<QPointer<FrameView>>& views = deferredViews();
    for (qsizetype i = views.size() - 1; i >= 0; --i) {
        const QPointer<FrameView> v = views.at(i);
        if (v && v->window() == window) {
            needed = true;
        } else {
            views.removeAt(i);
            if (v && v->m_backend == Backend::Rhi && !v->m_surface) v->makeRhiSurface();
        }
    }
    if (!needed) return;
    FrameViewRhiSurface::settleApi(gpuForced()); // the window's type follows the API: final from here on
    if (!window->testAttribute(Qt::WA_WState_Created)) {
        // The QWindow without its native window yet (a top-level is native anyway): it takes the surface type now,
        // show() makes the native window of that type
        window->setAttribute(Qt::WA_NativeWindow);
        if (QWindow* handle = window->windowHandle()) handle->setSurfaceType(FrameViewRhiSurface::surfaceType());
        return;
    }
    // Created already: of the GPU type, attachDeferredSurfaces switches it in place; otherwise the surfaces now
    // (Qt makes the native window again - before it is shown, nobody sees that)
    if (const QWindow* handle = window->windowHandle(); handle && handle->surfaceType() == FrameViewRhiSurface::surfaceType())
        return;
    qWarning("Viewer: the window was created before prepareWindow without the GPU surface type: GPU surface now");
    for (qsizetype i = views.size() - 1; i >= 0; --i) {
        const QPointer<FrameView> v = views.takeAt(i);
        if (v && v->m_backend == Backend::Rhi && !v->m_surface) v->makeRhiSurface();
    }
#else
    Q_UNUSED(window);
#endif
}

void FrameView::attachDeferredSurfaces()
{
    g_deferring = false;
#ifdef SCHNEIDI_HAVE_RHI
    for (const QPointer<FrameView>& v : std::exchange(deferredViews(), {})) {
        if (!v || v->m_backend != Backend::Rhi || v->m_surface) continue;
        const QWidget* window = v->window();
        const QWindow* handle = window->windowHandle();
        if (!window->testAttribute(Qt::WA_WState_Created) || !handle) {
            v->makeRhiSurface(); // not on screen yet: its creation takes the surface along
        } else if (handle->surfaceType() == FrameViewRhiSurface::surfaceType()) {
            // The stored check said "GPU ok", but it may not work any more (a driver update, ssh -X, VNC): the check
            // made on a helper thread before (FrameView::warmUpGpu) decides before the shown window is touched - a window switched
            // to a failing GPU composition turns black
            QString why;
            if (!FrameViewRhiSurface::startCheckPasses(gpuForced(), &why)) {
                v->fallBackToPainter(why);
                continue;
            }
            v->makeRhiSurface(true);
        } else {
            // Not prepared (prepareWindow): Qt would destroy and re-create the shown window - the painter until the
            // next start instead of a flash
            v->fallBackToPainter("window shown without a GPU surface type");
        }
    }
#endif
}

void FrameView::warmUpGpu(bool openGl)
{
#ifdef SCHNEIDI_HAVE_RHI
    static const QStringList headless{"offscreen", "minimal", "vnc", "linuxfb", "vkkhrdisplay"};
    const QString wish =
        qEnvironmentVariable("SCHNEIDI_VIEWER", QSettings().value("viewer/renderer").toString()).trimmed().toLower();
    if (CpuPath::on() || wish == "painter" || wish == "cpu" || headless.contains(QGuiApplication::platformName())) return;
    FrameViewRhiSurface::warmUp(openGl);
#else
    Q_UNUSED(openGl);
#endif
}

void FrameView::whenGpuChecked(std::function<void()> then, int waited)
{
#ifdef SCHNEIDI_HAVE_RHI
    constexpr int kStepMs = 20, kMaxWaitMs = 3000; // the check gives up itself after 2 s (startCheckPasses)
    if (FrameViewRhiSurface::warmUpRunning() && waited < kMaxWaitMs) {
        QTimer::singleShot(kStepMs, qApp, [then = std::move(then), waited]() mutable {
            whenGpuChecked(std::move(then), waited + kStepMs);
        });
        return;
    }
#else
    Q_UNUSED(waited);
#endif
    then();
}

bool FrameView::composesWithVulkan()
{
#ifdef SCHNEIDI_HAVE_RHI
    return FrameViewRhiSurface::vulkan();
#else
    return false;
#endif
}

void FrameView::setUseCachedGpuCheck(bool use)
{
    g_cachedGpu = use;
}

void FrameView::checkCachedGpu()
{
    g_cachedGpu = false;
#ifdef SCHNEIDI_HAVE_RHI
    // A view whose GPU surface is coming or has not rendered yet answers the check for free shortly (its window's
    // QRhi) - a device made for the check instead would stall the GUI thread once more (two GL contexts). Waits a
    // few seconds at most.
    static int waits = 0;
    const bool surfaceComing = std::any_of(cachedViews().cbegin(), cachedViews().cend(), [](const QPointer<FrameView>& v) {
        return v && v->m_backend == Backend::Rhi && v->isVisible()
               && (v->surfaceDeferred() || (v->m_surface && !static_cast<FrameViewRhiSurface*>(v->m_surface)->hasRhi()));
    });
    // The start's own check (warmUpGpu) answers without a device made now: no need to wait
    if (surfaceComing && !FrameViewRhiSurface::probed() && !FrameViewRhiSurface::warmedUp() && waits++ < 12) {
        QTimer::singleShot(250, qApp, &FrameView::checkCachedGpu);
        return;
    }
    const QList<QPointer<FrameView>> views = std::exchange(cachedViews(), {});
    // Nothing took the stored check and nothing checked: nothing to learn (painter wished, headless, tests)
    if (views.isEmpty() && !FrameViewRhiSurface::probed()) return;
    // The window's own QRhi answers when a view already renders with it; otherwise a device made for the check
    // (free when a view checked at its start)
    for (const QPointer<FrameView>& v : views)
        if (v && v->m_surface && static_cast<FrameViewRhiSurface*>(v->m_surface)->adoptAsProbe()) break;
    const Probe p = FrameViewRhiSurface::probe();
    if (FrameViewRhiSurface::vulkan() && (!p.ok || p.software)) {
        // The window's Vulkan device is a software one (lavapipe; OpenGL may have the hardware): the next start
        // checks again with OpenGL
        FrameViewRhiSurface::vulkanFailed(p.ok ? "software device " + p.device : QStringLiteral("no device"));
        QSettings().remove(kProbeKey);
    } else {
        QSettings().setValue(kProbeKey, QString("%1|%2|%3").arg(p.ok ? 1 : 0).arg(p.software ? 1 : 0).arg(p.device));
    }
    for (const QPointer<FrameView>& v : views) {
        if (!v) continue;
        v->m_gpuPending = false;
        // The device went away (driver, VM): the painter now. One that came is taken from the next start on - a
        // window that composites without the GPU cannot take a GPU surface while it is shown.
        const Backend now = chooseBackend(qEnvironmentVariable("SCHNEIDI_VIEWER"),
                                          CpuPath::on() ? QStringLiteral("painter")
                                                        : QSettings().value("viewer/renderer").toString(),
                                          QGuiApplication::platformName(), rhiCompiled(), [p] { return p; });
        if (v->m_backend == Backend::Rhi && now == Backend::Painter) v->fallBackToPainter("GPU check after the start");
    }
#endif
}

void FrameView::makeRhiSurface(bool intoShownWindow)
{
#ifdef SCHNEIDI_HAVE_RHI
    FrameViewRhiSurface::settleApi(gpuForced());
    // Vulkan: the window takes the run's instance (made now: ~50 ms of driver loading, not on the way to the window)
    if (!FrameViewRhiSurface::prepareWindow(window()->windowHandle())) {
        FrameViewRhiSurface::vulkanFailed(QStringLiteral("no Vulkan instance"));
        // A shown window of the Vulkan type cannot take OpenGL in place (prepareWindow): the painter for this run
        if (intoShownWindow) {
            fallBackToPainter("no Vulkan instance");
            return;
        }
    }
    // A window that exists already (a view made after its window was created): only the in-place switch makes Qt
    // compose it through the GPU - a QRhiWidget made with its parent would find no QRhi ("No QRhi")
    if (!intoShownWindow && window()->testAttribute(Qt::WA_WState_Created) && window()->windowHandle()) {
        if (window()->windowHandle()->surfaceType() != FrameViewRhiSurface::surfaceType())
            qInfo("Viewer: its window has another surface type, Qt makes the window again for the GPU");
        intoShownWindow = true;
    }
    FrameViewRhiSurface* surface = nullptr;
    if (intoShownWindow) {
        // Made without a parent and then reparented: only setParent() of a widget that already asks for RHI makes Qt
        // switch the shown window to GPU composition (inside the constructor the QRhiWidget does not ask yet). The
        // window's QRhi is created here (the GUI thread waits for the GL contexts).
        surface = new FrameViewRhiSurface(this, false);
        StartupTrace::mark("viewer surface made");
        surface->setParent(this);
        StartupTrace::mark("viewer window switched to RHI");
        surface->setGeometry(rect());
    } else {
        surface = new FrameViewRhiSurface(this);
    }
    m_surface = surface;
    m_overlayLayer = new FrameViewOverlayLayer(this);
    m_overlayLayer->setGeometry(rect());
    m_overlayLayer->raise();
    if (intoShownWindow) {
        surface->show();
        m_overlayLayer->show();
        if (StartupTrace::enabled()) {
            auto once = std::make_shared<QMetaObject::Connection>();
            *once = connect(surface, &QRhiWidget::frameSubmitted, this, [once] {
                StartupTrace::mark("viewer first GPU frame");
                disconnect(*once);
            });
        }
    }
    connect(surface, &QRhiWidget::renderFailed, this, [this, surface] {
        // The window's Vulkan device could not be made: OpenGL from the next start on
        if (FrameViewRhiSurface::vulkan() && !surface->hasRhi())
            FrameViewRhiSurface::vulkanFailed(QStringLiteral("the window's device failed"));
        fallBackToPainter("RHI render failed");
    });
#endif
}

FrameView::~FrameView() = default;

void FrameView::fallBackToPainter(const QString& why)
{
    if (m_backend == Backend::Painter) return;
    m_backend = Backend::Painter;
    m_backendInfo = why;
    // Deleted later: the signal that brought us here may come from inside its own render call
    if (m_surface) m_surface->deleteLater();
    if (m_overlayLayer) m_overlayLayer->deleteLater();
    m_surface = m_overlayLayer = nullptr;
    update();
}

void FrameView::setFrame(const QImage& image)
{
    const bool sizeChanged = image.size() != m_image.size();
    m_image = image;
    if (sizeChanged && m_frameSize.isEmpty()) m_pan = clampedPan(m_pan);
    repaintPicture();
    // The overlay is drawn relative to the frame: it moves along when the size changes (also while it is hidden)
    if (sizeChanged) repaintOverlay();
}

void FrameView::setFrameSize(const QSize& size)
{
    if (size == m_frameSize) return;
    m_frameSize = size;
    m_pan = clampedPan(m_pan);
    repaintPicture();
    repaintOverlay();
}

QSize FrameView::frameSize() const
{
    return m_frameSize.isEmpty() ? m_image.size() : m_frameSize;
}

void FrameView::setBackground(const QColor& color)
{
    m_background = color;
    repaintPicture();
}

void FrameView::setZoom(double zoom)
{
    zoom = zoom <= 0.0 ? kFit : std::clamp(zoom, 0.05, 16.0);
    if (zoom == m_zoom) return;
    // Keep the point at the widget's centre where it is: the pan scales with the picture
    const double before = scale();
    m_zoom = zoom;
    if (m_zoom == kFit) m_pan = {};
    else if (before > 0) m_pan = clampedPan(m_pan * (scale() / before));
    repaintPicture();
    repaintOverlay();
    emit zoomChanged(m_zoom);
}

void FrameView::setPan(const QPointF& pan)
{
    const QPointF p = clampedPan(pan);
    if (p == m_pan) return;
    m_pan = p;
    repaintPicture();
    repaintOverlay();
}

double FrameView::scale() const
{
    const QSize fs = frameSize();
    if (fs.isEmpty() || width() <= 0 || height() <= 0) return 0.0;
    if (m_zoom == kFit) return std::min(double(width()) / fs.width(), double(height()) / fs.height());
    // 100 % = one frame pixel on one physical screen pixel
    return m_zoom / devicePixelRatioF();
}

QPointF FrameView::clampedPan(const QPointF& pan) const
{
    const double s = scale();
    const QSize fs = frameSize();
    if (s <= 0 || m_zoom == kFit) return {};
    const double spareX = std::max(0.0, (fs.width() * s - width()) / 2);
    const double spareY = std::max(0.0, (fs.height() * s - height()) / 2);
    return {std::clamp(pan.x(), -spareX, spareX), std::clamp(pan.y(), -spareY, spareY)};
}

QRectF FrameView::frameRect() const
{
    const double s = scale();
    if (s <= 0 || m_image.isNull()) return {};
    const QSize fs = frameSize();
    double w = fs.width() * s, h = fs.height() * s;
    if (m_zoom == kFit) {
        // Whole pixels like before (no blurred edge against the bars)
        w = std::round(w);
        h = std::round(h);
        return QRectF(std::floor((width() - w) / 2), std::floor((height() - h) / 2), w, h);
    }
    return QRectF((width() - w) / 2 + m_pan.x(), (height() - h) / 2 + m_pan.y(), w, h);
}

QPointF FrameView::widgetToFrame(const QPointF& pos) const
{
    const QRectF r = frameRect();
    const QSize fs = frameSize();
    if (r.isEmpty()) return {};
    return {(pos.x() - r.x()) * fs.width() / r.width(), (pos.y() - r.y()) * fs.height() / r.height()};
}

QPointF FrameView::frameToWidget(const QPointF& framePos) const
{
    const QRectF r = frameRect();
    const QSize fs = frameSize();
    if (r.isEmpty()) return {};
    return {r.x() + framePos.x() * r.width() / fs.width(), r.y() + framePos.y() * r.height() / fs.height()};
}

void FrameView::setOverlay(ViewerOverlay* o)
{
    if (m_overlay) disconnect(m_overlay, nullptr, this, nullptr);
    m_overlay = o;
    if (o) connect(o, &ViewerOverlay::changed, this, &FrameView::repaintOverlay);
    repaintOverlay();
}

ViewerOverlay* FrameView::overlay() const
{
    return m_overlay;
}

void FrameView::repaintPicture()
{
#ifdef SCHNEIDI_HAVE_RHI
    if (m_surface) {
        m_surface->update();
        return;
    }
#endif
    update();
}

void FrameView::repaintOverlay()
{
    if (m_overlayLayer) m_overlayLayer->update();
    else update();
}

void FrameView::paintOverlay(QPainter& p)
{
    if (m_overlay && !m_image.isNull()) m_overlay->paint(p, frameRect());
}

void FrameView::paintEvent(QPaintEvent*)
{
    if (m_surface) return; // the surface and the overlay layer cover the widget
    QPainter p(this);
    const QRectF r = frameRect();
    // Bars only where the picture is not (no overdraw of the whole widget per frame)
    if (r.isEmpty() || !r.contains(QRectF(rect()))) {
        QRegion bars(rect());
        if (!r.isEmpty()) bars -= r.toAlignedRect().intersected(rect());
        for (const QRect& b : bars) p.fillRect(b, m_background);
    }
    if (m_image.isNull()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform, smoothScaling());
    // Only the visible part: drawImage clips anyway, but a source rect keeps a 200 % view from scaling the whole image
    const QRectF visible = r.intersected(QRectF(rect()));
    if (visible.isEmpty()) return;
    const double sx = m_image.width() / r.width(), sy = m_image.height() / r.height();
    const QRectF src((visible.x() - r.x()) * sx, (visible.y() - r.y()) * sy, visible.width() * sx,
                     visible.height() * sy);
    p.drawImage(visible, m_image, src);
    paintOverlay(p);
}

void FrameView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    m_pan = clampedPan(m_pan);
    if (m_surface) m_surface->setGeometry(rect());
    if (m_overlayLayer) m_overlayLayer->setGeometry(rect());
}

void FrameView::mousePressEvent(QMouseEvent* e)
{
    const QRectF r = frameRect();
    if (m_overlay && !r.isEmpty() && m_overlay->mousePress(e, r)) {
        m_overlayDrag = true;
        return;
    }
    const bool zoomedIn = m_zoom != kFit && clampedPan({1e9, 1e9}) != QPointF();
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && zoomedIn && !dragData)) {
        m_panning = true;
        m_panStart = m_pan;
        m_panPressPos = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() == Qt::LeftButton) m_pressPos = e->pos();
}

void FrameView::setIdleCursor(Qt::CursorShape shape)
{
    m_idleCursor = shape;
    if (!m_panning) setCursor(shape);
}

void FrameView::mouseMoveEvent(QMouseEvent* e)
{
    if (m_overlayDrag) {
        if (m_overlay) m_overlay->mouseMove(e, frameRect());
        return;
    }
    if (m_panning) {
        setPan(m_panStart + (e->position() - m_panPressPos));
        return;
    }
    if (!(e->buttons() & Qt::LeftButton)) {
        const QRectF r = frameRect();
        const Qt::CursorShape shape = m_overlay ? m_overlay->cursorAt(e->position(), r) : Qt::ArrowCursor;
        setCursor(shape == Qt::ArrowCursor ? m_idleCursor : shape);
        if (m_overlay) m_overlay->hover(e->position(), r);
    }
    if (!(e->buttons() & Qt::LeftButton) || m_pressPos.x() < 0 || !dragData) return;
    if ((e->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
    m_pressPos = QPoint(-1, -1);
    QMimeData* mime = dragData();
    if (!mime) return;
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    if (!m_image.isNull())
        drag->setPixmap(QPixmap::fromImage(m_image.scaled(160, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    drag->exec(Qt::CopyAction);
}

void FrameView::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_overlayDrag) {
        m_overlayDrag = false;
        if (m_overlay) m_overlay->mouseRelease(e, frameRect());
    }
    if (m_panning) {
        m_panning = false;
        setCursor(m_idleCursor);
    }
    m_pressPos = QPoint(-1, -1);
}

void FrameView::wheelEvent(QWheelEvent* e)
{
    if (!(e->modifiers() & Qt::ControlModifier) || m_image.isNull() || e->angleDelta().y() == 0) {
        QWidget::wheelEvent(e);
        return;
    }
    // From the zoom shown (fit included) to the next step up resp. down; below the fit size = fit
    const double fitZoom = std::min(double(width()) / frameSize().width(), double(height()) / frameSize().height())
                           * devicePixelRatioF();
    const double now = m_zoom == kFit ? fitZoom : m_zoom;
    double next = now;
    if (e->angleDelta().y() > 0) {
        for (double s : kSteps)
            if (s > now * 1.001) { next = s; break; }
    } else {
        next = kFit;
        for (auto it = kSteps.rbegin(); it != kSteps.rend(); ++it)
            if (*it < now * 0.999) { next = *it; break; }
        if (next != kFit && next <= fitZoom * 1.001) next = kFit; // a step at or below the fit size: fit
    }
    setZoom(next);
    e->accept();
}

void FrameView::leaveEvent(QEvent*)
{
    if (m_overlay) m_overlay->leave();
}
