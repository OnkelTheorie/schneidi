// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/gpu/Gpu.h"

#include "engine/ColorGrade.h"
#include "engine/OutputColor.h"
#include "engine/video/Reader.h"

#include <QtGlobal>

#ifdef SCHNEIDI_HAVE_RHI
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <rhi/qrhi.h>
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace Picture::Gpu {

namespace {

// Matrix weights Kr, Kb of a colour space (MLT numbers as in Video::Info / OutputColor)
[[maybe_unused]] void weights(int colorspace, double& kr, double& kb)
{
    switch (colorspace) {
    case 601: case 170: case 470: case 624: kr = 0.299, kb = 0.114; return;
    case 240: kr = 0.212, kb = 0.087; return;
    case 2020: case 2021: kr = 0.2627, kb = 0.0593; return;
    default: kr = 0.2126, kb = 0.0722; return;
    }
}

[[maybe_unused]] int envSwitch()
{
    static const int v = [] {
        const QByteArray e = qgetenv("SCHNEIDI_GPU").trimmed().toLower();
        if (e == "0" || e == "off" || e == "cpu") return 0;
        if (e == "1" || e == "on" || e == "gpu") return 1;
        return -1;
    }();
    return v;
}

} // namespace

#ifdef SCHNEIDI_HAVE_RHI

// std140 layout of the shaders' uniform block (src/engine/picture/shaders/*: keep in step)
struct Uniforms {
    float rect[4] = {};
    float target[4] = {};
    float inv0[4] = {};
    float inv1[4] = {};
    float src[4] = {};
    float chroma[4] = {};
    float yuv[16] = {};
    float lut[4] = {};
    float crop[4] = {0, 0, -1, -1};
};
static_assert(sizeof(Uniforms) == 192);

class Texture {
public:
    enum Kind { Target, Rgba, RgbaStraight, Planar, SemiPlanar, Gray };
    Kind kind = Target;
    QSize size;
    std::unique_ptr<QRhiTexture> tex[3];
    std::unique_ptr<QRhiRenderPassDescriptor> rp;
    std::unique_ptr<QRhiTextureRenderTarget> rtClear, rtLoad;
    bool pendingClear = false;
    QColor clearColor = Qt::transparent;
    // Planes
    int log2cw = 0, log2ch = 0;
    int bits = 8;
    float sampleScale = 1;
    float yuv[16] = {};
    int mipLevels = 1;
    std::tuple<int, int, int, int, int> key; // pool key
};

struct Lut3d {
    std::unique_ptr<QRhiTexture> tex;
    int size = 0;
};

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
    for (const char* s : {"llvmpipe", "softpipe", "swrast", "swiftshader", "lavapipe", "basic render", "warp"})
        if (name.contains(s)) return true;
    return false;
}

// The offscreen QRhi of the platform's API; the GL fallback surface goes to *surface (must outlive the QRhi)
QRhi* createRhi(std::unique_ptr<QOffscreenSurface>& surface, QString& api)
{
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return nullptr;
#if defined(Q_OS_WIN)
    api = QStringLiteral("Direct3D 11");
    QRhiD3D11InitParams params;
    return QRhi::create(QRhi::D3D11, &params);
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    api = QStringLiteral("Metal");
    QRhiMetalInitParams params;
    return QRhi::create(QRhi::Metal, &params);
#else
    api = QStringLiteral("OpenGL");
    // 3.3 core: 3D textures, texelFetch, R8/RG8 (the default format asks for 2.0, and QRhi then assumes its limits)
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    surface.reset(QRhiGles2InitParams::newFallbackSurface(format));
    if (!surface) return nullptr;
    QRhiGles2InitParams params;
    params.format = format;
    params.fallbackSurface = surface.get();
    return QRhi::create(QRhi::OpenGLES2, &params);
#endif
}

} // namespace

struct Context::Impl {
    std::unique_ptr<QOffscreenSurface> surface; // before rhi: destroyed after it
    std::unique_ptr<QRhi> rhi;
    DeviceInfo info;
    QRhiCommandBuffer* cb = nullptr;
    bool recording = false;
    QRhiResourceUpdateBatch* pending = nullptr; // uploads for the next pass

    std::unique_ptr<QRhiBuffer> vbuf;
    bool vbufUploaded = false;
    std::unique_ptr<QRhiSampler> nearest, linear;
    std::unique_ptr<QRhiTexture> dummy2d, dummy3d;
    std::unique_ptr<QRhiShaderResourceBindings> layoutSrb;
    std::unique_ptr<QRhiGraphicsPipeline> layerPs, mixPs, yuvPs;
    QShader vert, layerFrag, mixFrag, yuvFrag;

    // Per-pass resources, reused frame after frame (slot i = i-th pass of the frame)
    std::vector<std::unique_ptr<QRhiBuffer>> ubos;
    std::vector<std::unique_ptr<QRhiShaderResourceBindings>> srbs;
    int passes = 0, lastPasses = 0;

    // Texture pool: shared state so textures given out can find it (or know it is gone)
    struct Pool {
        std::mutex lock;
        std::multimap<std::tuple<int, int, int, int, int>, std::unique_ptr<Texture>> free;
    };
    std::shared_ptr<Pool> pool = std::make_shared<Pool>();

    // Pictures referenced by uploads until finish() (the GL backend reads them when the frame is submitted)
    std::vector<AVFrame*> heldFrames;
    std::vector<std::shared_ptr<const void>> heldData;

    struct Readback {
        std::unique_ptr<QRhiReadbackResult> result;
        std::function<void(const QByteArray&)> deliver;
    };
    std::vector<Readback> readbacks;

    ~Impl()
    {
        pool.reset(); // textures still given out leak their GPU objects instead of touching a dead QRhi
    }

    QRhiResourceUpdateBatch* batch()
    {
        if (!pending) pending = rhi->nextResourceUpdateBatch();
        return pending;
    }

    TexturePtr take(std::tuple<int, int, int, int, int> key, const std::function<std::unique_ptr<Texture>()>& make)
    {
        std::unique_ptr<Texture> t;
        {
            std::lock_guard<std::mutex> g(pool->lock);
            auto it = pool->free.find(key);
            if (it != pool->free.end()) {
                t = std::move(it->second);
                pool->free.erase(it);
            }
        }
        if (!t) {
            t = make();
            if (!t) return {};
            t->key = key;
        }
        std::weak_ptr<Pool> weak = pool;
        return TexturePtr(t.release(), [weak](Texture* p) {
            if (auto pl = weak.lock()) {
                std::lock_guard<std::mutex> g(pl->lock);
                pl->free.emplace(p->key, std::unique_ptr<Texture>(p));
                return;
            }
            // Context gone: its QRhi too - the native objects cannot be released any more
            for (auto& t : p->tex) (void)t.release();
            (void)p->rp.release();
            (void)p->rtClear.release();
            (void)p->rtLoad.release();
            delete p;
        });
    }

    bool init()
    {
        QRhi* r = rhi.get();
        vert = loadShader(QStringLiteral(":/picture-shaders/quad.vert.qsb"));
        layerFrag = loadShader(QStringLiteral(":/picture-shaders/layer.frag.qsb"));
        mixFrag = loadShader(QStringLiteral(":/picture-shaders/mix.frag.qsb"));
        yuvFrag = loadShader(QStringLiteral(":/picture-shaders/toyuv.frag.qsb"));
        if (!vert.isValid() || !layerFrag.isValid() || !mixFrag.isValid() || !yuvFrag.isValid()) {
            info.error = QStringLiteral("shaders missing");
            return false;
        }
        if (!r->isFeatureSupported(QRhi::ThreeDimensionalTextures) || !r->isFeatureSupported(QRhi::TexelFetch)
            || !r->isTextureFormatSupported(QRhiTexture::R8) || !r->isTextureFormatSupported(QRhiTexture::RG8)) {
            info.error = QStringLiteral("device lacks 3D textures (%1) / texelFetch (%2) / R8, RG8 (%3) [%4]")
                             .arg(r->isFeatureSupported(QRhi::ThreeDimensionalTextures))
                             .arg(r->isFeatureSupported(QRhi::TexelFetch))
                             .arg(r->isTextureFormatSupported(QRhiTexture::RG8))
                             .arg(QString::fromUtf8(r->driverInfo().deviceName));
            return false;
        }
        vbuf.reset(r->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 8 * sizeof(float)));
        vbuf->create();
        nearest.reset(r->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                    QRhiSampler::ClampToEdge));
        nearest->create();
        linear.reset(r->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                   QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        linear->create();
        dummy2d.reset(r->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
        dummy2d->create();
        dummy3d.reset(r->newTexture(QRhiTexture::RGBA8, 1, 1, 1, 1, QRhiTexture::ThreeDimensional));
        dummy3d->create();
        ubos.emplace_back(r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
        ubos.back()->create();
        layoutSrb.reset(r->newShaderResourceBindings());
        const auto b = bindings(ubos.back().get(), dummy2d.get(), dummy2d.get(), dummy2d.get(), dummy3d.get());
        layoutSrb->setBindings(b.begin(), b.end());
        layoutSrb->create();
        return true;
    }

    std::array<QRhiShaderResourceBinding, 5> bindings(QRhiBuffer* ubo, QRhiTexture* t0, QRhiTexture* t1, QRhiTexture* t2, QRhiTexture* l)
    {
        const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
        const auto fs = QRhiShaderResourceBinding::FragmentStage;
        return {QRhiShaderResourceBinding::uniformBuffer(0, stages, ubo),
                QRhiShaderResourceBinding::sampledTexture(1, fs, t0, nearest.get()),
                QRhiShaderResourceBinding::sampledTexture(2, fs, t1, nearest.get()),
                QRhiShaderResourceBinding::sampledTexture(3, fs, t2, nearest.get()),
                QRhiShaderResourceBinding::sampledTexture(4, fs, l, linear.get())};
    }

    QRhiGraphicsPipeline* pipeline(std::unique_ptr<QRhiGraphicsPipeline>& ps, const QShader& frag, bool over,
                                   QRhiRenderPassDescriptor* rp)
    {
        if (ps) return ps.get();
        ps.reset(rhi->newGraphicsPipeline());
        ps->setShaderStages({{QRhiShaderStage::Vertex, vert}, {QRhiShaderStage::Fragment, frag}});
        QRhiVertexInputLayout layout;
        layout.setBindings({{2 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        ps->setVertexInputLayout(layout);
        ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        if (over) {
            QRhiGraphicsPipeline::TargetBlend blend; // premultiplied source over
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::One;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            ps->setTargetBlends({blend});
        }
        ps->setShaderResourceBindings(layoutSrb.get());
        ps->setRenderPassDescriptor(rp);
        if (!ps->create()) {
            ps.reset();
            return nullptr;
        }
        return ps.get();
    }

    std::unique_ptr<Texture> makeTarget(QSize size)
    {
        auto t = std::make_unique<Texture>();
        t->kind = Texture::Target;
        t->size = size;
        t->tex[0].reset(rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!t->tex[0]->create()) return {};
        QRhiColorAttachment att(t->tex[0].get());
        t->rtClear.reset(rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription(att)));
        t->rtLoad.reset(rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription(att),
                                                    QRhiTextureRenderTarget::PreserveColorContents));
        t->rp.reset(t->rtClear->newCompatibleRenderPassDescriptor());
        t->rtClear->setRenderPassDescriptor(t->rp.get());
        t->rtLoad->setRenderPassDescriptor(t->rp.get());
        if (!t->rtClear->create() || !t->rtLoad->create()) return {};
        return t;
    }

    // One pass drawing a quad over `rect` of dst with the given pipeline and inputs
    void pass(Texture& dst, std::unique_ptr<QRhiGraphicsPipeline>& psSlot, const QShader& frag, bool over, Uniforms u,
              QRhiTexture* t0, QRhiTexture* t1, QRhiTexture* t2, QRhiTexture* l)
    {
        QRhiGraphicsPipeline* ps = pipeline(psSlot, frag, over, dst.rp.get());
        if (!ps) return;
        QRhiResourceUpdateBatch* b = batch();
        if (!vbufUploaded) {
            static const float corners[] = {0, 0, 1, 0, 0, 1, 1, 1};
            b->uploadStaticBuffer(vbuf.get(), corners);
            vbufUploaded = true;
        }
        u.target[0] = float(dst.size.width());
        u.target[1] = float(dst.size.height());
        // Target row 0 = picture row 0 (the readback and later sampling see the picture upright on every backend)
        u.target[2] = rhi->isYUpInNDC() == rhi->isYUpInFramebuffer() ? 1.0f : -1.0f;
        const int slot = passes++;
        while (int(ubos.size()) <= slot) {
            ubos.emplace_back(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
            ubos.back()->create();
        }
        while (int(srbs.size()) <= slot) srbs.emplace_back(rhi->newShaderResourceBindings());
        b->updateDynamicBuffer(ubos[size_t(slot)].get(), 0, sizeof(Uniforms), &u);
        QRhiShaderResourceBindings* srb = srbs[size_t(slot)].get();
        const auto bs = bindings(ubos[size_t(slot)].get(), t0 ? t0 : dummy2d.get(), t1 ? t1 : dummy2d.get(),
                                 t2 ? t2 : dummy2d.get(), l ? l : dummy3d.get());
        srb->setBindings(bs.begin(), bs.end());
        srb->create();
        const bool clear = dst.pendingClear;
        dst.pendingClear = false;
        cb->beginPass(clear ? dst.rtClear.get() : dst.rtLoad.get(), clear ? dst.clearColor : QColor(Qt::transparent),
                      {1.0f, 0}, b);
        pending = nullptr;
        cb->setGraphicsPipeline(ps);
        cb->setViewport({0, 0, float(dst.size.width()), float(dst.size.height())});
        cb->setShaderResources(srb);
        const QRhiCommandBuffer::VertexInput input(vbuf.get(), 0);
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
        cb->endPass();
    }

    // A target still waiting for its clear gets an empty pass (before it is read)
    void flushClear(Texture& t)
    {
        if (t.kind != Texture::Target || !t.pendingClear) return;
        t.pendingClear = false;
        cb->beginPass(t.rtClear.get(), t.clearColor, {1.0f, 0}, pending);
        pending = nullptr;
        cb->endPass();
    }

    void readback(Texture& t, std::function<void(const QByteArray&)> deliver)
    {
        flushClear(t);
        Readback rb;
        rb.result = std::make_unique<QRhiReadbackResult>();
        rb.deliver = std::move(deliver);
        batch()->readBackTexture(QRhiReadbackDescription(t.tex[0].get()), rb.result.get());
        cb->resourceUpdate(pending);
        pending = nullptr;
        readbacks.push_back(std::move(rb));
    }
};

const DeviceInfo& probe()
{
    static const DeviceInfo info = [] {
        DeviceInfo i;
        std::unique_ptr<QOffscreenSurface> surface;
        QString api;
        std::unique_ptr<QRhi> rhi(createRhi(surface, api));
        i.api = api;
        if (!rhi) {
            i.error = QStringLiteral("no %1 device").arg(api.isEmpty() ? QStringLiteral("GPU") : api);
            return i;
        }
        i.device = QString::fromUtf8(rhi->driverInfo().deviceName);
        i.software = softwareDevice(rhi->driverInfo());
        i.ok = !i.software;
        if (i.software) i.error = QStringLiteral("software device %1").arg(i.device);
        rhi.reset(); // before the fallback surface
        return i;
    }();
    return info;
}

bool enabled()
{
    const int sw = envSwitch();
    if (sw == 0) return false;
    const DeviceInfo& i = probe();
    return sw == 1 ? (i.ok || i.software) : i.ok;
}

Context::Context() : d(std::make_unique<Impl>()) {}

Context::~Context()
{
    if (d->recording) finish();
    for (AVFrame* f : d->heldFrames) av_frame_free(&f);
}

std::unique_ptr<Context> Context::create(QString* why, bool allowSoftware)
{
    std::unique_ptr<Context> c(new Context);
    QString api;
    c->d->rhi.reset(createRhi(c->d->surface, api));
    c->d->info.api = api;
    if (!c->d->rhi) {
        if (why) *why = QStringLiteral("no %1 device").arg(api.isEmpty() ? QStringLiteral("GPU") : api);
        return {};
    }
    c->d->info.device = QString::fromUtf8(c->d->rhi->driverInfo().deviceName);
    c->d->info.software = softwareDevice(c->d->rhi->driverInfo());
    c->d->info.ok = !c->d->info.software;
    if (c->d->info.software && !allowSoftware) {
        if (why) *why = QStringLiteral("software device %1").arg(c->d->info.device);
        return {};
    }
    if (!c->d->init()) {
        if (why) *why = c->d->info.error;
        return {};
    }
    return c;
}

QRhi* Context::rhi() const { return d->rhi.get(); }
const DeviceInfo& Context::info() const { return d->info; }

void Context::begin()
{
    if (d->recording) return;
    if (d->rhi->beginOffscreenFrame(&d->cb) != QRhi::FrameOpSuccess) return;
    d->recording = true;
    d->passes = 0;
}

void Context::finish()
{
    if (!d->recording) return;
    if (d->pending) {
        d->cb->resourceUpdate(d->pending);
        d->pending = nullptr;
    }
    d->rhi->endOffscreenFrame(); // waits for the card: readbacks are complete
    d->recording = false;
    d->lastPasses = d->passes;
    for (auto& rb : d->readbacks) rb.deliver(rb.result->data);
    d->readbacks.clear();
    for (AVFrame* f : d->heldFrames) av_frame_free(&f);
    d->heldFrames.clear();
    d->heldData.clear();
}

TexturePtr Context::target(QSize size, const float clear[4])
{
    if (size.isEmpty()) return {};
    TexturePtr t = d->take({0, size.width(), size.height(), 0, 0}, [&] { return d->makeTarget(size); });
    if (!t) return {};
    t->pendingClear = true;
    t->clearColor = clear ? QColor::fromRgbF(clear[0] * clear[3], clear[1] * clear[3], clear[2] * clear[3], clear[3])
                          : QColor(Qt::transparent);
    return t;
}

TexturePtr Context::uploadRgba(const uint8_t* rgba, int w, int h, bool premultiplied)
{
    if (!rgba || w <= 0 || h <= 0) return {};
    TexturePtr t = d->take({1, w, h, 0, 0}, [&] {
        auto t = std::make_unique<Texture>();
        t->size = QSize(w, h);
        t->tex[0].reset(d->rhi->newTexture(QRhiTexture::RGBA8, t->size));
        return t->tex[0]->create() ? std::move(t) : nullptr;
    });
    if (!t) return {};
    t->kind = premultiplied ? Texture::Rgba : Texture::RgbaStraight;
    QRhiTextureSubresourceUploadDescription desc(rgba, quint32(size_t(w) * h * 4));
    d->batch()->uploadTexture(t->tex[0].get(), QRhiTextureUploadDescription({0, 0, desc}));
    return t;
}

TexturePtr Context::uploadGray(const uint8_t* gray, int w, int h)
{
    if (!gray || w <= 0 || h <= 0) return {};
    TexturePtr t = d->take({2, w, h, 0, 0}, [&] {
        auto t = std::make_unique<Texture>();
        t->size = QSize(w, h);
        t->tex[0].reset(d->rhi->newTexture(QRhiTexture::R8, t->size));
        return t->tex[0]->create() ? std::move(t) : nullptr;
    });
    if (!t) return {};
    t->kind = Texture::Gray;
    QRhiTextureSubresourceUploadDescription desc(gray, quint32(size_t(w) * h));
    d->batch()->uploadTexture(t->tex[0].get(), QRhiTextureUploadDescription({0, 0, desc}));
    return t;
}

TexturePtr Context::uploadYuv(const AVFrame* frame, const Video::Info& info, bool mips)
{
    if (!frame || frame->width <= 0 || frame->height <= 0) return {};
    const auto fmt = AVPixelFormat(frame->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BE))
        || desc->nb_components < 3)
        return {};
    const bool semi = desc->comp[1].plane == desc->comp[2].plane;
    const int depth = desc->comp[0].depth;
    if (depth > 16 || (depth > 8) != (desc->comp[0].step == 2) || (semi && desc->comp[1].step != 2 * desc->comp[0].step))
        return {};
    const bool wide = depth > 8;
    if (wide && !d->rhi->isTextureFormatSupported(QRhiTexture::R16)) return {};
    const int w = frame->width, h = frame->height;
    const int cw = AV_CEIL_RSHIFT(w, desc->log2_chroma_w), ch = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
    const int levels = mips ? d->rhi->mipLevelsForSize(QSize(w, h)) : 1;
    TexturePtr t = d->take({semi ? 4 : 3, w, h, (wide ? 1 : 0) | desc->log2_chroma_w << 1 | desc->log2_chroma_h << 2, levels}, [&] {
        auto t = std::make_unique<Texture>();
        t->size = QSize(w, h);
        QRhiTexture::Flags flags;
        if (mips) flags |= QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips;
        const auto one = wide ? QRhiTexture::R16 : QRhiTexture::R8;
        const auto two = wide ? QRhiTexture::RG16 : QRhiTexture::RG8;
        t->tex[0].reset(d->rhi->newTexture(one, QSize(w, h), 1, flags));
        t->tex[1].reset(d->rhi->newTexture(semi ? two : one, QSize(cw, ch), 1, flags));
        if (!semi) t->tex[2].reset(d->rhi->newTexture(one, QSize(cw, ch), 1, flags));
        for (auto& p : t->tex)
            if (p && !p->create()) return std::unique_ptr<Texture>();
        return t;
    });
    if (!t) return {};
    t->kind = semi ? Texture::SemiPlanar : Texture::Planar;
    t->log2cw = desc->log2_chroma_w;
    t->log2ch = desc->log2_chroma_h;
    t->bits = depth;
    t->mipLevels = levels;
    // 16-bit words hold `depth` bits: low bits (yuv420p10) or high bits (p010, shift > 0)
    const int shift = desc->comp[0].shift;
    t->sampleScale = wide ? float(65535.0 / (double((1 << depth) - 1) * double(1 << shift))) : 1.0f;
    // Y'CbCr (normalized code values) -> RGB 0..1
    double kr, kb;
    weights(info.colorspace, kr, kb);
    const double kg = 1 - kr - kb;
    const double maxCode = double((1 << depth) - 1), unit = double(1 << (depth - 8));
    const double yo = info.fullRange ? 0.0 : 16 * unit / maxCode, co = 128 * unit / maxCode;
    const double ys = info.fullRange ? 1.0 : maxCode / (219 * unit), cs = info.fullRange ? maxCode / (255 * unit) : maxCode / (224 * unit);
    const double rv = 2 * (1 - kr) * cs, bu = 2 * (1 - kb) * cs;
    const double gu = -2 * (1 - kb) * kb / kg * cs, gv = -2 * (1 - kr) * kr / kg * cs;
    float* m = t->yuv; // column-major: col0 = Y, col1 = Cb, col2 = Cr, col3 = constant
    const float a = float(ys);
    m[0] = a, m[1] = a, m[2] = a, m[3] = 0;
    m[4] = 0, m[5] = float(gu), m[6] = float(bu), m[7] = 0;
    m[8] = float(rv), m[9] = float(gv), m[10] = 0, m[11] = 0;
    m[12] = float(-ys * yo - rv * co), m[13] = float(-ys * yo - (gu + gv) * co), m[14] = float(-ys * yo - bu * co), m[15] = 1;

    // Keep the picture until finish(): the upload reads the plane memory when the frame is submitted (no copy here)
    AVFrame* held = av_frame_clone(frame);
    if (!held) return {};
    d->heldFrames.push_back(held);
    QRhiResourceUpdateBatch* b = d->batch();
    const int planes = semi ? 2 : 3;
    for (int i = 0; i < planes; ++i) {
        const int pw = i == 0 ? w : cw, ph = i == 0 ? h : ch;
        const int rowBytes = pw * (wide ? 2 : 1) * (semi && i == 1 ? 2 : 1);
        QRhiTextureSubresourceUploadDescription sub;
        sub.setData(QByteArray::fromRawData(reinterpret_cast<const char*>(held->data[i]), held->linesize[i] * (ph - 1) + rowBytes));
        sub.setDataStride(quint32(held->linesize[i]));
        sub.setSourceSize(QSize(pw, ph));
        b->uploadTexture(t->tex[i].get(), QRhiTextureUploadDescription({0, 0, sub}));
        if (mips) b->generateMips(t->tex[i].get());
    }
    return t;
}

LutPtr Context::gradeLut(const ColorGrade::Baked& baked, int n)
{
    n = std::clamp(n, 2, 256);
    // The lattice as an RGBA picture (R fastest, then G, then B = slice), corrected by the CPU code itself
    std::vector<uint8_t> lattice(size_t(n) * n * n * 4);
    uint8_t* p = lattice.data();
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r, p += 4) {
                p[0] = uint8_t(std::lround(r * 255.0 / (n - 1)));
                p[1] = uint8_t(std::lround(g * 255.0 / (n - 1)));
                p[2] = uint8_t(std::lround(b * 255.0 / (n - 1)));
                p[3] = 255;
            }
    ColorGrade::apply(lattice.data(), n * n, n, baked, 1);
    auto lut = std::make_shared<Lut3d>();
    lut->size = n;
    lut->tex.reset(d->rhi->newTexture(QRhiTexture::RGBA8, n, n, n, 1, QRhiTexture::ThreeDimensional));
    if (!lut->tex->create()) return {};
    QList<QRhiTextureUploadEntry> entries;
    const size_t slice = size_t(n) * n * 4;
    for (int z = 0; z < n; ++z)
        entries.append(QRhiTextureUploadEntry(z, 0, QRhiTextureSubresourceUploadDescription(lattice.data() + z * slice, quint32(slice))));
    QRhiTextureUploadDescription up;
    up.setEntries(entries.cbegin(), entries.cend());
    d->batch()->uploadTexture(lut->tex.get(), up);
    return lut;
}

void Context::draw(Texture& dst, const Layer& layer)
{
    const Texture* s = layer.source;
    if (!d->recording || !s || dst.kind != Texture::Target || layer.opacity <= 0) return;
    const Compose::Affine& m = layer.m;
    const double det = m.a * m.e - m.b * m.d;
    if (std::abs(det) < 1e-12) return;
    Uniforms u;
    const int sw = s->size.width(), sh = s->size.height();
    // Bounding box of the drawn source (+1 pixel: soft edge), as Compose::draw
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (const auto& [sx, sy] : {std::pair{0.0, 0.0}, std::pair{double(sw), 0.0}, std::pair{0.0, double(sh)},
                                 std::pair{double(sw), double(sh)}}) {
        const double x = m.a * sx + m.b * sy + m.c, y = m.d * sx + m.e * sy + m.f;
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    const double W = dst.size.width(), H = dst.size.height();
    u.rect[0] = float(std::clamp(std::floor(x0) - 1, 0.0, W));
    u.rect[1] = float(std::clamp(std::floor(y0) - 1, 0.0, H));
    u.rect[2] = float(std::clamp(std::ceil(x1) + 1, 0.0, W));
    u.rect[3] = float(std::clamp(std::ceil(y1) + 1, 0.0, H));
    if (u.rect[0] >= u.rect[2] || u.rect[1] >= u.rect[3]) return;
    const double ia = m.e / det, ib = -m.b / det, id = -m.d / det, ie = m.a / det;
    u.inv0[0] = float(ia), u.inv0[1] = float(ib), u.inv0[2] = float(-(ia * m.c + ib * m.f));
    u.inv1[0] = float(id), u.inv1[1] = float(ie), u.inv1[2] = float(-(id * m.c + ie * m.f));
    u.inv0[3] = float(std::lround(std::clamp(layer.opacity, 0.0, 1.0) * 256) / 256.0);
    // Mip level: Compose::draw halves while 2:1 or more is left (per axis; the card halves both: the smaller one)
    if (s->mipLevels > 1) {
        const double ax = std::hypot(m.a, m.d), ay = std::hypot(m.b, m.e);
        const double n = std::min(ax > 0 ? 1.0 / ax : 1.0, ay > 0 ? 1.0 / ay : 1.0);
        u.inv1[3] = float(std::clamp(int(std::floor(std::log2(std::max(1.0, n + 1e-9)))), 0, s->mipLevels - 1));
    }
    u.src[0] = float(sw), u.src[1] = float(sh), u.src[2] = float(s->log2cw), u.src[3] = float(s->log2ch);
    u.chroma[2] = s->sampleScale;
    std::memcpy(u.yuv, s->yuv, sizeof u.yuv);
    QRhiTexture* l = nullptr;
    if (layer.grade && layer.grade->tex) {
        const int n = layer.grade->size;
        u.chroma[3] = 1;
        u.lut[0] = float(n - 1) / n;
        u.lut[1] = 0.5f / n;
        l = layer.grade->tex.get();
    }
    if (!layer.crop.isEmpty()) {
        u.crop[0] = float(layer.crop.left()), u.crop[1] = float(layer.crop.top());
        u.crop[2] = float(layer.crop.right()), u.crop[3] = float(layer.crop.bottom());
    }
    switch (s->kind) {
    case Texture::Target: case Texture::Rgba: u.target[3] = 0; break;
    case Texture::RgbaStraight: u.target[3] = 1; break;
    case Texture::Planar: u.target[3] = 2; break;
    case Texture::SemiPlanar: u.target[3] = 3; break;
    case Texture::Gray: return;
    }
    if (s->kind == Texture::Target) d->flushClear(const_cast<Texture&>(*s));
    d->pass(dst, d->layerPs, d->layerFrag, true, u, s->tex[0].get(), s->tex[1].get(), s->tex[2].get(), l);
}

TexturePtr Context::mix(const Texture& a, const Texture& b, double t)
{
    if (!d->recording || a.size != b.size) return {};
    TexturePtr out = target(a.size);
    if (!out) return {};
    d->flushClear(const_cast<Texture&>(a));
    d->flushClear(const_cast<Texture&>(b));
    Uniforms u;
    u.rect[2] = float(a.size.width()), u.rect[3] = float(a.size.height());
    u.target[3] = 0;
    u.lut[2] = float(std::lround(std::clamp(t, 0.0, 1.0) * 256) / 256.0);
    d->pass(*out, d->mixPs, d->mixFrag, false, u, a.tex[0].get(), b.tex[0].get(), nullptr, nullptr);
    return out;
}

TexturePtr Context::luma(const Texture& a, const Texture& b, const Texture& gradient, double progress, double softness)
{
    if (!d->recording || a.size != b.size || gradient.size != a.size) return {};
    TexturePtr out = target(a.size);
    if (!out) return {};
    d->flushClear(const_cast<Texture&>(a));
    d->flushClear(const_cast<Texture&>(b));
    Uniforms u;
    u.rect[2] = float(a.size.width()), u.rect[3] = float(a.size.height());
    u.target[3] = 1;
    u.lut[2] = float(progress);
    u.lut[3] = float(std::max(softness, 1e-6));
    d->pass(*out, d->mixPs, d->mixFrag, false, u, a.tex[0].get(), b.tex[0].get(), gradient.tex[0].get(), nullptr);
    return out;
}

void Context::readRgba(const Texture& t, uint8_t* out)
{
    if (!d->recording || !out || t.kind != Texture::Target) return;
    const QSize sz = t.size;
    d->readback(const_cast<Texture&>(t), [dst = out, sz](const QByteArray& data) {
        uint8_t* out = dst;
        const size_t n = size_t(sz.width()) * sz.height();
        if (size_t(data.size()) < n * 4) return;
        const auto* s = reinterpret_cast<const uint8_t*>(data.constData());
        for (size_t i = 0; i < n; ++i, s += 4, out += 4) { // premultiplied -> straight
            const unsigned a = s[3];
            if (a == 255 || a == 0) {
                std::memcpy(out, s, 3);
                if (a == 0) out[0] = out[1] = out[2] = 0;
            } else {
                for (int c = 0; c < 3; ++c) out[c] = uint8_t(std::min(255u, (s[c] * 255u + a / 2) / a));
            }
            out[3] = uint8_t(a);
        }
    });
}

bool Context::readYuv(const Texture& t, const OutputColor::Target& out)
{
    using OutputColor::Layout;
    if (!d->recording || t.kind != Texture::Target || out.bits != 8
        || (out.layout != Layout::Yuv420 && out.layout != Layout::Nv12))
        return false;
    const int w = t.size.width(), h = t.size.height();
    // RGB -> Y'CbCr code values / 255 (OutputColor's coefficients, floating point)
    double kr, kb;
    weights(out.colorspace, kr, kb);
    const double kg = 1 - kr - kb;
    const double ly = (out.fullRange ? 255.0 : 219.0) / 255.0, lc = (out.fullRange ? 255.0 : 224.0) / 255.0;
    Uniforms u;
    float* m = u.yuv;
    m[0] = float(kr * ly), m[1] = float(kg * ly), m[2] = float(kb * ly), m[3] = float((out.fullRange ? 0 : 16) / 255.0);
    m[4] = float(-kr / (2 * (1 - kb)) * lc), m[5] = float(-kg / (2 * (1 - kb)) * lc), m[6] = float(0.5 * lc), m[7] = float(128 / 255.0);
    m[8] = float(0.5 * lc), m[9] = float(-kg / (2 * (1 - kr)) * lc), m[10] = float(-kb / (2 * (1 - kr)) * lc), m[11] = float(128 / 255.0);
    d->flushClear(const_cast<Texture&>(t));
    const bool nv = out.layout == Layout::Nv12;
    struct Plane {
        int mode, texW, texH, bytes, rows;
    };
    std::vector<Plane> planes{{0, (w + 3) / 4, h, w, h}};
    if (nv) planes.push_back({1, (w + 3) / 4, (h + 1) / 2, ((w + 1) / 2) * 2, (h + 1) / 2});
    else {
        planes.push_back({2, (w + 7) / 8, (h + 1) / 2, (w + 1) / 2, (h + 1) / 2});
        planes.push_back({3, (w + 7) / 8, (h + 1) / 2, (w + 1) / 2, (h + 1) / 2});
    }
    for (size_t i = 0; i < planes.size(); ++i) {
        const Plane p = planes[i];
        TexturePtr packed = target(QSize(p.texW, p.texH));
        if (!packed) return false;
        packed->pendingClear = false;
        u.rect[0] = u.rect[1] = 0;
        u.rect[2] = float(p.texW), u.rect[3] = float(p.texH);
        u.target[3] = float(p.mode);
        d->pass(*packed, d->yuvPs, d->yuvFrag, false, u, t.tex[0].get(), nullptr, nullptr, nullptr);
        uint8_t* dst = out.planes[i];
        const int stride = out.strides[i];
        // The texture stays referenced by the lambda until delivered (pooled again afterwards)
        d->readback(*packed, [dst, stride, p, packed](const QByteArray& data) {
            const size_t rowBytes = size_t(p.texW) * 4;
            if (size_t(data.size()) < rowBytes * p.texH) return;
            for (int y = 0; y < p.rows; ++y)
                std::memcpy(dst + size_t(y) * stride, data.constData() + y * rowBytes, size_t(p.bytes));
        });
    }
    return true;
}

Context::Stats Context::stats() const
{
    Stats s;
    {
        std::lock_guard<std::mutex> g(d->pool->lock);
        s.texturesPooled = int(d->pool->free.size());
    }
    s.passes = d->lastPasses;
    return s;
}

QSize size(const Texture& t) { return t.size; }

#else // no Qt RHI headers: the CPU path only

class Texture {
public:
    QSize size;
};
struct Lut3d {};
struct Context::Impl {
    DeviceInfo info;
};

const DeviceInfo& probe()
{
    static const DeviceInfo i = [] {
        DeviceInfo d;
        d.error = QStringLiteral("built without Qt RHI");
        return d;
    }();
    return i;
}
bool enabled() { return false; }
Context::Context() : d(std::make_unique<Impl>()) {}
Context::~Context() = default;
std::unique_ptr<Context> Context::create(QString* why, bool)
{
    if (why) *why = probe().error;
    return {};
}
QRhi* Context::rhi() const { return nullptr; }
const DeviceInfo& Context::info() const { return d->info; }
void Context::begin() {}
void Context::finish() {}
TexturePtr Context::target(QSize, const float*) { return {}; }
TexturePtr Context::uploadRgba(const uint8_t*, int, int, bool) { return {}; }
TexturePtr Context::uploadYuv(const AVFrame*, const Video::Info&, bool) { return {}; }
TexturePtr Context::uploadGray(const uint8_t*, int, int) { return {}; }
LutPtr Context::gradeLut(const ColorGrade::Baked&, int) { return {}; }
void Context::draw(Texture&, const Layer&) {}
TexturePtr Context::mix(const Texture&, const Texture&, double) { return {}; }
TexturePtr Context::luma(const Texture&, const Texture&, const Texture&, double, double) { return {}; }
void Context::readRgba(const Texture&, uint8_t*) {}
bool Context::readYuv(const Texture&, const OutputColor::Target&) { return false; }
Context::Stats Context::stats() const { return {}; }
QSize size(const Texture& t) { return t.size; }

#endif

} // namespace Picture::Gpu
