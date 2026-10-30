// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/FrameView.h"

#include "app/Theme.h"
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
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>

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

FrameView::FrameView(QWidget* parent) : QWidget(parent), m_background(Theme::viewerBg)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    std::function<Probe()> probe;
#ifdef SCHNEIDI_HAVE_RHI
    probe = [] { return FrameViewRhiSurface::probe(); };
#endif
    m_backend = chooseBackend(qEnvironmentVariable("SCHNEIDI_VIEWER"),
                              QSettings().value("viewer/renderer").toString(), QGuiApplication::platformName(),
                              rhiCompiled(), probe, &m_backendInfo);
#ifdef SCHNEIDI_HAVE_RHI
    if (m_backend == Backend::Rhi) {
        auto* surface = new FrameViewRhiSurface(this);
        m_surface = surface;
        m_overlayLayer = new FrameViewOverlayLayer(this);
        m_overlayLayer->raise();
        connect(surface, &QRhiWidget::renderFailed, this, [this] { fallBackToPainter("RHI render failed"); });
    }
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
    if (m_backend == Backend::Rhi) return; // the surface and the overlay layer cover the widget
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
        setCursor(m_overlay ? m_overlay->cursorAt(e->position(), r) : Qt::ArrowCursor);
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
        unsetCursor();
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
