// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Picture engine on the graphics card (0.15 package 7) through Qt RHI. The CPU path (Picture::Renderer with
// video/Compose, ColorGrade, OutputColor) stays the reference and the fallback; this evaluates the same steps in shaders.
//
// Design
// ------
// Device: a Context owns one offscreen QRhi (no window): OpenGL through a QOffscreenSurface on Linux (the API the
//   viewer's QRhiWidget uses, so a texture can be shared with it later), Direct3D 11 on Windows, Metal on macOS.
//   Vulkan would need a QVulkanInstance and buys nothing yet. Rule as in FrameView: a software device (llvmpipe,
//   lavapipe, SwiftShader, WARP, CpuDevice) is no GPU -> CPU path. SCHNEIDI_GPU=0 never, =1 force (even a software
//   device: tests), unset = automatic. enabled() probes once. Needs a QGuiApplication (QOffscreenSurface for GL);
//   the GUI app has one, schneidi-cli and the test programs run with QT_QPA_PLATFORM=offscreen, where OpenGL works
//   only if the platform plugin finds GLX/EGL (headless CI: no -> CPU path, tests skip with 77).
// Threads: a QRhi (and the GL context behind it) belongs to the thread that created it. One Context per thread; the
//   Renderer will own one GPU thread that takes render jobs from the preview pump and the export workers (frames are
//   independent, the card works on one queue anyway), instead of a context per worker.
// Frames on the card: everything between the decoder and the readback is a Texture: RGBA8 premultiplied render
//   targets from a pool keyed by size (reused across frames; a frame needs ~3 of them: canvas, two transition sides),
//   decoded pictures as one texture per plane (R8/RG8, R16/RG16 for 10 bit; Y'CbCr -> RGB happens while sampling in the
//   layer shader, so no swscale and 1.5 instead of 4 bytes per pixel go over the bus). Picture::Frame will carry a
//   shared_ptr<Texture> next to (or instead of) its RGBA buffer; callers that only read pixels trigger a readback.
// Steps (shaders in src/engine/picture/shaders, baked to .qsb by tools/bake_shaders.sh):
//   - layer.frag: Compose::draw - affine map, bilinear in premultiplied alpha with transparent outside, mip level for
//     2:1 and more (the caller generates mips), opacity, crop (source pixels, area share), Y'CbCr planes or RGBA input,
//     the clip's colour grade as a 3D LUT (ColorGrade::bake evaluated on an N^3 lattice; one draw per layer).
//   - mix.frag: cross dissolve (premultiplied mix) and luma wipes (gradient texture, smoothstep band, straight mix).
//   - toyuv.frag: OutputColor::fromRgba for the encoder: Y / NV12 CbCr / Cb / Cr packed 4 values per RGBA8 texel, so
//     the readback is the encoder's plane bytes (1.5 bytes per pixel back instead of 4, no RGB->YUV on the CPU).
// Sharing with the viewer: QRhiWidget makes its own QRhi; a QRhi's textures are not visible to another. On OpenGL
//   with Qt::AA_ShareOpenGLContexts the widget can wrap our texture's native GL name (QRhiTexture::createFrom) and
//   draw it without a copy (FrameViewRhiSurface::frameTexture() hook) - needs a GL fence between the two contexts.
//   On D3D11 the same through a shared handle. Until then the viewer gets the readback (still cheaper than today:
//   no CPU compositing).
// Recording: begin() starts one offscreen frame, the operations record passes, finish() submits and waits; readbacks
//   asked for in between are filled by finish(). Keep everything of one output frame inside one begin/finish.

#include "engine/video/Compose.h"

#include <QRectF>
#include <QSize>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

struct AVFrame;
class QOffscreenSurface;
class QRhi;

namespace ColorGrade {
struct Baked;
}
namespace OutputColor {
struct Target;
}
namespace Video {
struct Info;
}

namespace Picture::Gpu {

struct DeviceInfo {
    bool ok = false;        // a hardware device works
    bool software = false;  // the device found is a software rasterizer
    QString api;            // "OpenGL", "Direct3D 11", ...
    QString device;         // driver's device name
    QString error;          // why not
    bool comp = false;      // compositing graphs work on it (Context::compSupported)
};

// The device a Context gets on this machine (probed once, on the first call; GUI application needed)
const DeviceInfo& probe();
// Use the GPU path: SCHNEIDI_GPU=0 -> false, =1 -> true when any device works, unset -> a hardware device works
bool enabled();

class Texture;       // a picture on the card (render target or uploaded planes)
struct Lut3d;        // a baked colour grade as 3D texture
using TexturePtr = std::shared_ptr<Texture>;
using LutPtr = std::shared_ptr<Lut3d>;

class Context {
public:
    // nullptr when no device works (error in *why). allowSoftware: also a software rasterizer (tests of the shaders)
    static std::unique_ptr<Context> create(QString* why = nullptr, bool allowSoftware = false);
    // The same with a surface made beforehand (OpenGL: QOffscreenSurface is made on the GUI thread, the context may then
    // live on another one); it must outlive the Context
    static std::unique_ptr<Context> create(QOffscreenSurface* surface, QString* why, bool allowSoftware);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    QRhi* rhi() const;
    const DeviceInfo& info() const;
    // A slot for the caller's per-context caches (LUTs, stills kept on the card); released before the device
    std::shared_ptr<void>& userData();
    // The same for the compositing passes' caches (gpu/CompGpu)
    std::shared_ptr<void>& compData();

    // false: the card refused the frame (context lost, driver reset): every operation would do nothing
    bool begin();
    // Submits what was recorded, waits for the card, fills the readbacks
    void finish();

    // A pooled RGBA8 render target (premultiplied), cleared to `clear` (straight RGBA 0..1) by its first pass
    TexturePtr target(QSize size, const float clear[4] = nullptr);
    // Upload RGBA (8 bit, rows tightly packed)
    TexturePtr uploadRgba(const uint8_t* rgba, int w, int h, bool premultiplied);
    // Upload a decoded software picture (planar/semi-planar 8 or 10-bit Y'CbCr); nullptr = format not supported (then
    // convert on the CPU and use uploadRgba). mips: also build mip levels (drawn at 2:1 or smaller)
    TexturePtr uploadYuv(const AVFrame* frame, const Video::Info& info, bool mips = false);
    // A gradient for luma wipes (8-bit gray, rows tightly packed)
    TexturePtr uploadGray(const uint8_t* gray, int w, int h);
    // The grade of ColorGrade::bake as a 3D LUT of size^3 nodes (trilinear on the card); 52 = nodes on whole code values
    // (51 steps of 5), so the lattice is the CPU result exactly
    LutPtr gradeLut(const ColorGrade::Baked& baked, int size = 52);

    struct Layer {
        const Texture* source = nullptr;
        Compose::Affine m;          // source pixels -> target pixels (Compose::placement)
        double opacity = 1.0;
        const Lut3d* grade = nullptr;
        QRectF crop;                // source pixels kept, empty = all
    };
    // Source over: draws the layer onto dst (premultiplied)
    void draw(Texture& dst, const Layer& layer);
    // Cross dissolve a*(1-t) + b*t (premultiplied, same size) into a new target
    TexturePtr mix(const Texture& a, const Texture& b, double t);
    // Luma wipe: b replaces a where gradient < progress*(1+softness), over the softness band (Renderer luma())
    TexturePtr luma(const Texture& a, const Texture& b, const Texture& gradient, double progress, double softness);

    // ---- Compositing graphs (gpu/CompGpu): node pictures in RGBA16F premultiplied targets (no rounding to 8 bits
    // between nodes; a mask holds its value in every channel). draw() works onto them too (transform, grade nodes);
    // they are drawn onto an 8-bit target like any other, never read back themselves.
    // RGBA16F render targets work and the compositing shaders loaded
    bool compSupported() const;
    // A pooled RGBA16F target cleared to `clear` (premultiplied 0..1; nullptr = transparent)
    TexturePtr compTarget(QSize size, const float clear[4] = nullptr);
    enum class CompShader { Merge, Mask, Key, Blur };
    // One pass of a compositing shader over the whole of a new compTarget(size): p = its 40 parameters (10 vec4, see
    // the shader), t0..t2 its inputs (nullptr = none)
    TexturePtr compPass(CompShader shader, QSize size, const float p[40], const Texture* t0, const Texture* t1 = nullptr,
                        const Texture* t2 = nullptr);

    // Readbacks, filled by the next finish(); the memory must stay valid until then
    // Straight RGBA, width*height*4 bytes
    void readRgba(const Texture& t, uint8_t* out);
    // Encoder planes (OutputColor::Target: Yuv420 or Nv12, 8 bit); false = layout not supported (CPU fromRgba)
    bool readYuv(const Texture& t, const OutputColor::Target& target);

    struct Stats {
        int texturesPooled = 0;  // render targets kept for reuse
        int passes = 0;          // passes of the last frame
    };
    Stats stats() const;

private:
    Context();
    struct Impl;
    std::unique_ptr<Impl> d;
};

QSize size(const Texture& t);

// uploadYuv() takes pictures of this pixel format (AVPixelFormat number)
bool canUpload(int avPixelFormat);

// The process's GPU thread: one Context, jobs run one after another on it (preview pump, export workers, stills). The
// callers keep doing their CPU work (decoding, layers the GPU cannot draw) in parallel and only wait for their job.
class Service {
public:
    // Makes the OpenGL surface ahead when called on the GUI thread (QOffscreenSurface needs it on the offscreen platform;
    // ~40 ms, mostly loading the GL library). Renderer's constructor calls it, so a first instance() on a worker (export
    // workers, preview pump) finds it.
    static void prepare();
    // GUI app: prepare() only posts the work to the event loop (not in the way of the start); a worker asking for
    // instance() meanwhile waits for it (at most 2 s)
    static void setDeferredPrepare(bool deferred);
    // nullptr when SCHNEIDI_GPU=0, no GUI application, or no hardware device (then the CPU path). The first call starts
    // the GPU thread and waits until its context is made (or refused).
    static Service* instance();
    // As instance(), but while a deferred prepare() is pending it starts the service in the background and returns
    // nullptr at once with *decided = false (render this frame on the CPU, ask again for the next)
    static Service* tryInstance(bool* decided);
    // Ends the GPU thread (its context released on it); after that run() refuses. Done on QCoreApplication::aboutToQuit
    static void shutdown();
    // Runs job(context) on the GPU thread inside begin()/finish() and waits for it (readbacks are filled on return);
    // false = shut down or the card refused the frame, nothing ran (the caller takes the CPU path)
    bool run(const std::function<void(Context&)>& job);
    const DeviceInfo& info() const;

private:
    Service() = default;
    struct Impl;
    Impl* d = nullptr; // never freed (the thread ends in shutdown())
};

} // namespace Picture::Gpu
