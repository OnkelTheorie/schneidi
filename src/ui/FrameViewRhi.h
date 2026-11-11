// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// GPU surface of FrameView (only built with SCHNEIDI_HAVE_RHI): uploads the frame into a texture when it changed and
// draws one quad at FrameView::frameRect() over the letterbox colour. Shaders: src/ui/shaders/frameview.{vert,frag}
// (baked to .qsb with tools/bake_shaders.sh; GLSL/HLSL/MSL/SPIR-V variants, so every RHI backend finds its code).

#include "ui/FrameView.h"

#include <QRhiWidget>
#include <QSurface>
#include <memory>

class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

class FrameViewRhiSurface : public QRhiWidget {
public:
    // `parented` false: made as a parentless widget, the caller reparents it into `view` (FrameView::makeRhiSurface)
    explicit FrameViewRhiSurface(FrameView* view, bool parented = true);
    ~FrameViewRhiSurface() override;

    // Creates a device of the platform's API once (cached): does a hardware RHI work here, and which device is it
    static FrameView::Probe probe();
    // probe() has run (its answer is known without touching the GPU)
    static bool probed();
    // The API QRhiWidget is set to (the same one the probe tried)
    static QRhiWidget::Api platformApi();
    // The native window type a top-level needs for that API's composition
    static QSurface::SurfaceType surfaceType();
    // This surface's QRhi (the window's, once it has rendered) answers the GPU check instead of a device made for it
    // (probe() then returns it); false while it has none or the check is known already
    bool adoptAsProbe();
    // Has rendered once: the window's QRhi is known
    bool hasRhi() const { return rhi() != nullptr; }

    // Texture sampled for the picture. Today the uploaded frame; the hook for a texture the picture engine renders
    // into the same QRhi later (then no upload: set it here and skip uploadFrame)
    QRhiTexture* frameTexture() const { return m_texture.get(); }

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    void uploadFrame(class QRhiResourceUpdateBatch* batch);
    void rebuildBindings();

    FrameView* m_view;
    std::unique_ptr<QRhiBuffer> m_vbuf, m_ubuf;
    std::unique_ptr<QRhiSampler> m_linear, m_nearest;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiShaderResourceBindings> m_srbLinear, m_srbNearest;
    std::unique_ptr<QRhiGraphicsPipeline> m_pipeline;
    qint64 m_uploadedKey = 0;   // QImage::cacheKey() of the frame in the texture
    bool m_vbufUploaded = false;
};
