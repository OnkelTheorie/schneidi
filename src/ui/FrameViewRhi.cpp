// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/FrameViewRhi.h"

#include "core/StartupTrace.h"

#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <rhi/qrhi.h>
#if defined(SCHNEIDI_VIEWER_VULKAN)
#include <QVulkanInstance>
#endif

#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <optional>

namespace {

QShader loadShader(const QString& name)
{
    QFile f(name);
    return f.open(QIODevice::ReadOnly) ? QShader::fromSerialized(f.readAll()) : QShader();
}

bool softwareDevice(const QRhiDriverInfo& info)
{
    if (info.deviceType == QRhiDriverInfo::CpuDevice) return true;
    const QByteArray name = info.deviceName.toLower();
    for (const char* s : {"llvmpipe", "softpipe", "swrast", "swiftshader", "lavapipe", "basic render"})
        if (name.contains(s)) return true;
    return false;
}

} // namespace

namespace {
// The viewer's API on Linux/BSD, decided once per run (FrameView::preferVulkan); false after a Vulkan failure before
// the API was settled (settleApi: a window is made for it, it never changes after that in this run)
std::optional<bool> g_vulkan;
bool g_apiSettled = false;
constexpr const char* kApiKey = "viewer/gpuApi"; // "vulkan" opts in, "opengl" after Vulkan failed here
// Set while the start's Vulkan check runs (warmUp): still there at the next start = the driver crashed or hung in it
constexpr const char* kVulkanTryingKey = "viewer/vulkanCheckRunning";
// How long the window's setup waits for the start's check (a hung driver: the other API, or the painter)
constexpr int kCheckWaitMs = 2000;
} // namespace

bool FrameViewRhiSurface::vulkan()
{
#if defined(SCHNEIDI_VIEWER_VULKAN)
    if (!g_vulkan) {
        g_vulkan = FrameView::preferVulkan(qEnvironmentVariable("SCHNEIDI_VIEWER_API"),
                                           QSettings().value(kApiKey).toString(), QGuiApplication::platformName(), true);
        if (*g_vulkan && QSettings().value(kVulkanTryingKey).toBool()) {
            QSettings().remove(kVulkanTryingKey);
            vulkanFailed(QStringLiteral("the last start ended inside the Vulkan check"));
        }
    }
    return *g_vulkan;
#else
    return false;
#endif
}

void FrameViewRhiSurface::vulkanFailed(const QString& why)
{
    if (!vulkan()) return;
    qWarning("Viewer: Vulkan not usable (%s), OpenGL %s", qPrintable(why),
             g_apiSettled ? "from the next start on" : "from now on");
    // Never mid-run: a window made for Vulkan keeps it (its views fall back to the painter)
    if (!g_apiSettled) g_vulkan = false;
    // Remembered unless the environment asked for Vulkan (a test of it must not change the user's choice)
    if (qEnvironmentVariableIsEmpty("SCHNEIDI_VIEWER_API")) QSettings().setValue(kApiKey, QStringLiteral("opengl"));
}

QRhiWidget::Api FrameViewRhiSurface::platformApi()
{
#if defined(Q_OS_WIN)
    return QRhiWidget::Api::Direct3D11;
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    return QRhiWidget::Api::Metal;
#else
    return vulkan() ? QRhiWidget::Api::Vulkan : QRhiWidget::Api::OpenGL;
#endif
}

QSurface::SurfaceType FrameViewRhiSurface::surfaceType()
{
#if defined(Q_OS_WIN)
    return QSurface::Direct3DSurface;
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    return QSurface::MetalSurface;
#else
    return vulkan() ? QSurface::VulkanSurface : QSurface::OpenGLSurface;
#endif
}

#if defined(SCHNEIDI_VIEWER_VULKAN)
QVulkanInstance* FrameViewRhiSurface::vulkanInstance()
{
    // One for the whole run (probe and windows), never destroyed while a window may use it; made on first use:
    // loading the Vulkan drivers costs ~50 ms
    static QVulkanInstance* instance = [] {
        auto* inst = new QVulkanInstance;
        inst->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
        inst->setApiVersion(inst->supportedApiVersion());
        if (inst->create()) return inst;
        delete inst;
        return static_cast<QVulkanInstance*>(nullptr);
    }();
    return instance;
}
#endif

bool FrameViewRhiSurface::prepareWindow(QWindow* window)
{
#if defined(SCHNEIDI_VIEWER_VULKAN)
    if (!vulkan()) return true;
    QVulkanInstance* instance = vulkanInstance();
    if (!instance) return false;
    // A window made later (not created yet) gets Qt's own default instance when it composes
    if (window && !window->vulkanInstance()) window->setVulkanInstance(instance);
#else
    Q_UNUSED(window);
#endif
    return true;
}

namespace {
// The GPU check's answer once known (GUI thread only)
std::optional<FrameView::Probe> g_probe;

FrameView::Probe describe(const QRhi& rhi)
{
    FrameView::Probe p;
    p.ok = true;
    p.device = QString::fromUtf8(rhi.driverInfo().deviceName);
    p.software = softwareDevice(rhi.driverInfo());
    return p;
}

// The start's check (warmUp), made on a helper thread of its own: never waited for without a time limit, a hung
// driver must not hang the start (nor the quit: the thread is detached)
struct WarmUp {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool vulkan = false;
    FrameView::Probe probe;
#if defined(SCHNEIDI_VIEWER_VULKAN)
    VkPhysicalDevice physDev = VK_NULL_HANDLE; // for the present check (settleApi)
    uint32_t queueFamily = 0;
#endif
};
std::shared_ptr<WarmUp> g_warmUp; // GUI thread

// A device of the given API made and thrown away: does it work, which is it (`into`: also the Vulkan device)
FrameView::Probe probeApi(bool vulkan, WarmUp* into = nullptr)
{
    FrameView::Probe p;
    std::unique_ptr<QRhi> rhi;
#if defined(SCHNEIDI_VIEWER_VULKAN)
    if (vulkan) {
        QRhiVulkanInitParams params;
        params.inst = FrameViewRhiSurface::vulkanInstance();
        if (params.inst) rhi.reset(QRhi::create(QRhi::Vulkan, &params));
        if (rhi) {
            p = describe(*rhi);
            if (into)
                if (const auto* h = static_cast<const QRhiVulkanNativeHandles*>(rhi->nativeHandles())) {
                    into->physDev = h->physDev;
                    into->queueFamily = h->gfxQueueFamilyIdx;
                }
        }
        return p;
    }
#else
    Q_UNUSED(vulkan);
    Q_UNUSED(into);
#endif
#if defined(Q_OS_WIN)
    QRhiD3D11InitParams params;
    rhi.reset(QRhi::create(QRhi::D3D11, &params));
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    QRhiMetalInitParams params;
    rhi.reset(QRhi::create(QRhi::Metal, &params));
#else
    std::unique_ptr<QOffscreenSurface> surface(QRhiGles2InitParams::newFallbackSurface());
    QRhiGles2InitParams params;
    params.fallbackSurface = surface.get();
    rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
#endif
    if (rhi) {
        p = describe(*rhi);
        rhi.reset(); // before the fallback surface (OpenGL) goes: the QRhi needs it to shut down
    }
    return p;
}

// The start's check, if it ran for the API used now and finished within `ms`
std::optional<FrameView::Probe> warmUpResult(int ms)
{
    const std::shared_ptr<WarmUp> w = g_warmUp;
    if (!w || w->vulkan != FrameViewRhiSurface::vulkan()) return std::nullopt;
    std::unique_lock lock(w->m);
    if (!w->cv.wait_for(lock, std::chrono::milliseconds(ms), [&w] { return w->done; })) return std::nullopt;
    return w->probe;
}
} // namespace

bool FrameViewRhiSurface::probed()
{
    return g_probe.has_value();
}

void FrameViewRhiSurface::warmUp()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    return; // a D3D11/Metal device is cheap and has not delayed the start
#else
    if (g_probe || g_warmUp) return;
    const bool vk = vulkan();
    // OpenGL: only where a fallback surface may be made off the GUI thread (GLX pbuffers), the platform allows
    // contexts on other threads, and there are cores to spare (a check only: on one or two cores it would slow the
    // window's setup down; the stored check decides then, as before)
    if (!vk && (QGuiApplication::platformName() != QLatin1String("xcb") || !QOpenGLContext::supportsThreadedOpenGL()
                || QThread::idealThreadCount() < 4))
        return;
    if (vk) { // a driver that crashes or hangs here: OpenGL from the next start on (vulkan())
        QSettings settings;
        settings.setValue(kVulkanTryingKey, true);
        settings.sync();
    }
    auto w = std::make_shared<WarmUp>();
    w->vulkan = vk;
    g_warmUp = w;
    // Vulkan: loads the drivers (every installed one: ~100-200 ms) and makes a device once - the first one costs the
    // most, the window's own one afterwards ~50 ms. OpenGL: the check only (each context costs the same). A
    // QVulkanInstance and a QRhi may be made on any thread.
    std::thread([w] {
        const FrameView::Probe p = probeApi(w->vulkan, w.get());
        {
            std::lock_guard lock(w->m);
            w->probe = p;
            w->done = true;
        }
        w->cv.notify_all();
        if (w->vulkan)
            QMetaObject::invokeMethod(
                QCoreApplication::instance(), [] { QSettings().remove(kVulkanTryingKey); }, Qt::QueuedConnection);
    }).detach();
#endif
}

bool FrameViewRhiSurface::warmedUp()
{
    return g_warmUp && g_warmUp->vulkan == vulkan();
}

void FrameViewRhiSurface::settleApi(bool allowSoftware)
{
    if (g_apiSettled) return;
#if defined(SCHNEIDI_VIEWER_VULKAN)
    if (vulkan()) {
        warmUp(); // (nothing when it ran)
        QString why;
        const std::optional<FrameView::Probe> p = warmUpResult(kCheckWaitMs);
        if (!p) why = QStringLiteral("the Vulkan check did not finish in time");
        else if (!p->ok) why = QStringLiteral("no Vulkan device");
        else if (p->software && !allowSoftware) why = "software device " + p->device;
        else {
            // Can the device show into this display's windows (a throwaway window of the Vulkan type, not shown)
            QWindow test;
            test.setSurfaceType(QSurface::VulkanSurface);
            test.setVulkanInstance(vulkanInstance());
            test.create();
            if (!vulkanInstance()->supportsPresent(g_warmUp->physDev, g_warmUp->queueFamily, &test))
                why = QStringLiteral("the device cannot show into this display's windows");
        }
        if (!why.isEmpty()) vulkanFailed(why); // OpenGL in this run already: no window was made for Vulkan yet
    }
#else
    Q_UNUSED(allowSoftware);
#endif
    g_apiSettled = true;
}

bool FrameViewRhiSurface::startCheckPasses(bool allowSoftware, QString* why)
{
    if (!warmedUp()) return true; // nothing checked: as before (the stored check)
    const std::optional<FrameView::Probe> p = warmUpResult(kCheckWaitMs);
    if (p && p->ok && (!p->software || allowSoftware)) return true;
    const QString reason = !p   ? QStringLiteral("the GPU check of the start did not finish in time")
                           : p->ok ? "software device " + p->device
                                   : QStringLiteral("no GPU device at the start");
    if (why) *why = reason;
    vulkanFailed(reason); // Vulkan: OpenGL from the next start on (the shown window keeps its type)
    return false;
}

FrameView::Probe FrameViewRhiSurface::probe()
{
    if (g_probe) return *g_probe;
    std::optional<FrameView::Probe> warm = warmedUp() ? warmUpResult(kCheckWaitMs) : std::nullopt;
    FrameView::Probe p = warm ? *warm : probeApi(vulkan());
    // Vulkan missing or only a software device (lavapipe), before a window was made for it: OpenGL may still have
    // the hardware
    if (vulkan() && !g_apiSettled && (!p.ok || p.software)) {
        const FrameView::Probe gl = probeApi(false);
        if (gl.ok && (!gl.software || !p.ok)) {
            vulkanFailed(p.ok ? QStringLiteral("software device ") + p.device : QStringLiteral("no device"));
            p = gl;
        }
    }
    StartupTrace::mark("viewer RHI probe");
    g_probe = p;
    return p;
}

bool FrameViewRhiSurface::adoptAsProbe()
{
    if (g_probe || !rhi()) return false;
    g_probe = describe(*rhi());
    return true;
}

FrameViewRhiSurface::FrameViewRhiSurface(FrameView* view, bool parented)
    : QRhiWidget(parented ? view : nullptr), m_view(view)
{
    setApi(platformApi());
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setGeometry(view->rect());
}

FrameViewRhiSurface::~FrameViewRhiSurface() = default;

void FrameViewRhiSurface::releaseResources()
{
    m_pipeline.reset();
    m_srbLinear.reset();
    m_srbNearest.reset();
    m_texture.reset();
    m_linear.reset();
    m_nearest.reset();
    m_ubuf.reset();
    m_vbuf.reset();
    m_uploadedKey = 0;
    m_vbufUploaded = false;
}

void FrameViewRhiSurface::initialize(QRhiCommandBuffer*)
{
    // Called again on resize (same QRhi) or after a new QRhi: build what is missing
    if (m_pipeline && m_pipeline->renderPassDescriptor()->isCompatible(renderTarget()->renderPassDescriptor())) return;
    if (m_pipeline) releaseResources();
    QRhi* r = rhi();
    m_vbuf.reset(r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 8 * sizeof(float)));
    m_vbuf->create();
    m_ubuf.reset(r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 8 * sizeof(float)));
    m_ubuf->create();
    m_linear.reset(r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                 QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_linear->create();
    m_nearest.reset(r->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                  QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_nearest->create();
    // A 1x1 placeholder until the first frame: the bindings always have a texture
    m_texture.reset(r->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    m_texture->create();
    m_srbLinear.reset(r->newShaderResourceBindings());
    m_srbNearest.reset(r->newShaderResourceBindings());
    rebuildBindings();

    m_pipeline.reset(r->newGraphicsPipeline());
    m_pipeline->setShaderStages({{QRhiShaderStage::Vertex, loadShader(":/shaders/frameview.vert.qsb")},
                                 {QRhiShaderStage::Fragment, loadShader(":/shaders/frameview.frag.qsb")}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    m_pipeline->setVertexInputLayout(layout);
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    QRhiGraphicsPipeline::TargetBlend blend; // straight alpha over the letterbox colour
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_pipeline->setTargetBlends({blend});
    m_pipeline->setSampleCount(sampleCount());
    m_pipeline->setShaderResourceBindings(m_srbLinear.get());
    m_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!m_pipeline->create()) m_pipeline.reset();
}

void FrameViewRhiSurface::rebuildBindings()
{
    for (auto [srb, sampler] : {std::pair{m_srbLinear.get(), m_linear.get()}, std::pair{m_srbNearest.get(), m_nearest.get()}}) {
        srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage, m_ubuf.get()),
                          QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                                    m_texture.get(), sampler)});
        srb->create();
    }
}

void FrameViewRhiSurface::uploadFrame(QRhiResourceUpdateBatch* batch)
{
    const QImage& img = m_view->frame();
    if (img.isNull() || img.cacheKey() == m_uploadedKey) return;
    QImage upload = img;
    // RGBA8888 (what the engine delivers) goes up as it is; anything else is converted once here
    if (upload.format() != QImage::Format_RGBA8888 && upload.format() != QImage::Format_RGBA8888_Premultiplied
        && upload.format() != QImage::Format_RGBX8888)
        upload = upload.convertToFormat(QImage::Format_RGBA8888);
    // A frame over the device's texture size limit (OpenGL does not refuse it, the picture just stays black; D3D11 and
    // Vulkan refuse it): shown scaled down to the limit, still on the GPU (no screen shows 16k pixels anyway)
    const int maxSize = rhi()->resourceLimit(QRhi::TextureSizeMax);
    if (maxSize > 0 && (upload.width() > maxSize || upload.height() > maxSize))
        upload = upload.scaled(QSize(maxSize, maxSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (m_texture->pixelSize() != upload.size()) {
        m_texture->setPixelSize(upload.size());
        if (!m_texture->create()) {
            // The device could not make it (out of memory): nothing drawn from the old, now destroyed texture, and the
            // painter takes over. Posted: the handler replaces this surface, which must not happen inside render()
            m_texture->setPixelSize({});
            m_uploadedKey = 0;
            QTimer::singleShot(0, this, [this] { emit renderFailed(); });
            return;
        }
        rebuildBindings(); // the bindings refer to the native texture object
    }
    // The batch keeps a reference to the image data (implicitly shared): no copy on the CPU side
    batch->uploadTexture(m_texture.get(), upload);
    m_uploadedKey = img.cacheKey();
}

void FrameViewRhiSurface::render(QRhiCommandBuffer* cb)
{
    QRhi* r = rhi();
    QRhiResourceUpdateBatch* batch = r->nextResourceUpdateBatch();
    const QRectF fr = m_view->frameRect();
    const bool draw = m_pipeline && !fr.isEmpty() && width() > 0 && height() > 0;
    if (draw) {
        if (!m_vbufUploaded) {
            static const float corners[] = {0, 0, 1, 0, 0, 1, 1, 1};
            batch->uploadStaticBuffer(m_vbuf.get(), corners);
            m_vbufUploaded = true;
        }
        uploadFrame(batch);
        // Widget pixels -> normalized device coordinates (y up or down depending on the backend)
        const auto ndcX = [this](double x) { return float(2.0 * x / width() - 1.0); };
        const auto ndcY = [this, r](double y) {
            const double t = 2.0 * y / height() - 1.0;
            return float(r->isYUpInNDC() ? -t : t);
        };
        const float placement[8] = {ndcX(fr.left()), ndcY(fr.top()), ndcX(fr.right()), ndcY(fr.bottom()), 0, 0, 1, 1};
        batch->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(placement), placement);
    }
    const QColor bg = m_view->background();
    cb->beginPass(renderTarget(), bg, {1.0f, 0}, batch);
    if (draw && m_uploadedKey != 0) {
        const QSize px = renderTarget()->pixelSize();
        cb->setGraphicsPipeline(m_pipeline.get());
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setShaderResources(m_view->smoothScaling() ? m_srbLinear.get() : m_srbNearest.get());
        const QRhiCommandBuffer::VertexInput input(m_vbuf.get(), 0);
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
    }
    cb->endPass();
}
