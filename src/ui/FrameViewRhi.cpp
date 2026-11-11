// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/FrameViewRhi.h"

#include "core/StartupTrace.h"

#include <QFile>
#include <QOffscreenSurface>
#include <QTimer>
#include <rhi/qrhi.h>

#include <atomic>
#include <cstring>

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

QRhiWidget::Api FrameViewRhiSurface::platformApi()
{
#if defined(Q_OS_WIN)
    return QRhiWidget::Api::Direct3D11;
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    return QRhiWidget::Api::Metal;
#else
    return QRhiWidget::Api::OpenGL;
#endif
}

namespace {
std::atomic<bool> g_probed{false};
} // namespace

bool FrameViewRhiSurface::probed()
{
    return g_probed.load();
}

FrameView::Probe FrameViewRhiSurface::probe()
{
    static const FrameView::Probe result = [] {
        FrameView::Probe p;
        std::unique_ptr<QRhi> rhi;
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
        if (!rhi) return p;
        p.ok = true;
        p.device = QString::fromUtf8(rhi->driverInfo().deviceName);
        p.software = softwareDevice(rhi->driverInfo());
        rhi.reset(); // before the fallback surface (OpenGL) goes: the QRhi needs it to shut down
        StartupTrace::mark("viewer RHI probe");
        return p;
    }();
    g_probed = true;
    return result;
}

FrameViewRhiSurface::FrameViewRhiSurface(FrameView* view) : QRhiWidget(view), m_view(view)
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
