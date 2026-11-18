// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// GPU surface of FrameView (only built with SCHNEIDI_HAVE_RHI): uploads the frame into a texture when it changed and
// draws one quad at FrameView::frameRect() over the letterbox colour. Shaders: src/ui/shaders/frameview.{vert,frag}
// (baked to .qsb with tools/bake_shaders.sh; GLSL/HLSL/MSL/SPIR-V variants, so every RHI backend finds its code).

#include "ui/FrameView.h"

#include <QRhiWidget>
#include <QSurface>
#include <QtGui/qtguiglobal.h>
#include <memory>

// Linux/BSD: the viewer may compose through Vulkan (FrameView::preferVulkan); needs Qt with Vulkan and its headers
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS) && !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID) && QT_CONFIG(vulkan) \
    && __has_include(<vulkan/vulkan.h>)
#define SCHNEIDI_VIEWER_VULKAN 1
#endif

class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QVulkanInstance;
class QWindow;

class FrameViewRhiSurface : public QRhiWidget {
public:
    // `parented` false: made as a parentless widget, the caller reparents it into `view` (FrameView::makeRhiSurface)
    explicit FrameViewRhiSurface(FrameView* view, bool parented = true);
    ~FrameViewRhiSurface() override;

    // Creates a device of the platform's API once (cached): does a hardware RHI work here, and which device is it
    static FrameView::Probe probe();
    // probe() has run (its answer is known without touching the GPU)
    static bool probed();
    // probe() gave up on a start check stuck in the driver (says nothing about the GPU: not stored)
    static bool probeTimedOut();
    // The API QRhiWidget is set to (the same one the probe tried)
    static QRhiWidget::Api platformApi();
    // The native window type a top-level needs for that API's composition
    static QSurface::SurfaceType surfaceType();
    // Linux/BSD: the viewer composes through Vulkan (FrameView::preferVulkan), decided once per run
    static bool vulkan();
    // Vulkan did not work here (no device, only a software one, the window's QRhi failed): OpenGL from now on and
    // for the next starts (viewer/gpuApi)
    static void vulkanFailed(const QString& why);
#if defined(SCHNEIDI_VIEWER_VULKAN)
    // The run's Vulkan instance (nullptr when Vulkan cannot start)
    static QVulkanInstance* vulkanInstance();
#endif
    // The GPU check of the start on a helper thread, while the window is built (probe() then answers with it).
    // Vulkan: also loads the drivers and makes a device once, so the window's own device costs little later.
    // Linux/BSD only (OpenGL: xcb only).
    // `openGl`: OpenGL too (Vulkan always) - the app asks for it after the window's open animation only.
    static void warmUp(bool openGl);
    // warmUp() ran for the API used now
    static bool warmedUp();
    // warmUp()'s check is still being made
    static bool warmUpRunning();
    // Makes this run's API final before a window is made for it (FrameView::prepareWindow, the first surface).
    // Vulkan (opt-in) must pass the start's check within 2 s and show into this display's windows, else OpenGL in
    // this run already. Never changes after that.
    static void settleApi(bool allowSoftware);
    // warmUp()'s check allows the GPU (never waits: not made yet = failed; true when there was none): the shown window
    // may be switched to GPU composition. False: why, and Vulkan is given up for the next starts.
    static bool startCheckPasses(bool allowSoftware, QString* why);
    // Gives a top-level window what its GPU composition needs before Qt makes the window's QRhi (Vulkan: the
    // instance); false when that cannot work (Vulkan without an instance)
    static bool prepareWindow(QWindow* window);
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
