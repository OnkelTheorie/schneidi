// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/Renderer.h"
#include "engine/FileStamp.h"

#include "core/Captions.h"
#include "core/CodeTrust.h"
#include "core/CompGraph.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Multicam.h"
#include "core/Retime.h"
#include "core/TimelineOps.h"
#include "engine/BlurFill.h"
#include "engine/ColorGrade.h"
#include "engine/Lumas.h"
#include "engine/MemoryRelease.h"
#include "engine/OutputColor.h"
#include "engine/StabilizeCorrection.h"
#include "engine/VideoEffects.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CustomEffects.h"
#include "engine/picture/Effects.h"
#include "engine/picture/Interpolate.h"
#include "engine/picture/CompTrack.h"
#include "engine/picture/ShapePaint.h"
#include "core/Shapes.h"
#include "engine/picture/TitleLook.h"
#include "engine/picture/gpu/CompGpu.h"
#include "engine/picture/gpu/Gpu.h"
#include "engine/text3d/Text3d.h"
#include "engine/video/Compose.h"
#include "engine/video/Convert.h"
#include "engine/video/Reader.h"
#include "engine/video/Text.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <QDebug>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QImageReader>
#include <QPainter>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <numbers>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace Picture {

// ---- Buffers ----

class Buffer {
public:
    explicit Buffer(size_t bytes) : data(bytes) {}
    std::vector<uint8_t> data;
};

namespace {

// Free buffers by size; a frame's buffer goes back here when its last Frame is gone. Small: at most a few per size
// (preview read-ahead + export workers), the rest is freed.
struct Pool {
    std::mutex mutex;
    std::map<size_t, std::vector<std::unique_ptr<Buffer>>> free;
    static constexpr size_t kKeep = 12;

    std::shared_ptr<Buffer> take(size_t bytes)
    {
        std::unique_ptr<Buffer> b;
        {
            std::lock_guard lock(mutex);
            auto& list = free[bytes];
            if (!list.empty()) {
                b = std::move(list.back());
                list.pop_back();
            }
        }
        if (!b) b = std::make_unique<Buffer>(bytes);
        return std::shared_ptr<Buffer>(b.release(), [this](Buffer* p) { give(p); });
    }
    void give(Buffer* p)
    {
        std::lock_guard lock(mutex);
        auto& list = free[p->data.size()];
        if (list.size() < kKeep) list.emplace_back(p);
        else delete p;
    }
    void clear()
    {
        std::lock_guard lock(mutex);
        free.clear();
    }
};

Pool& pool()
{
    static Pool* p = new Pool; // never destroyed: frames may outlive statics
    return *p;
}

// Storage of layers (Image) that went out of use, by size. Layers, transitions and transforms take a cleared
// full-frame buffer per clip and frame; fresh allocations of that size come from mmap (page faults and zeroing by the
// kernel each time) or, once glibc raised its mmap threshold, from per-thread arenas that grow and stay large with
// several render threads (export peaks of GBs). Reusing them keeps memory at what the frames in flight need.
struct LayerPool {
    std::mutex mutex;
    std::map<size_t, std::vector<std::vector<uint8_t>>> free;
    size_t bytes = 0;
    static constexpr size_t kMaxBytes = size_t(256) << 20; // what stays around between frames
    static constexpr size_t kMinBytes = size_t(64) << 10;  // small pictures (titles, icons) are not worth it

    std::vector<uint8_t> take(size_t n)
    {
        if (n >= kMinBytes) {
            std::lock_guard lock(mutex);
            auto it = free.find(n);
            if (it != free.end() && !it->second.empty()) {
                std::vector<uint8_t> v = std::move(it->second.back());
                it->second.pop_back();
                bytes -= v.capacity();
                std::memset(v.data(), 0, n);
                return v;
            }
        }
        return std::vector<uint8_t>(n, 0);
    }
    void give(std::vector<uint8_t>&& v)
    {
        if (v.size() < kMinBytes || v.size() != v.capacity()) return;
        std::lock_guard lock(mutex);
        if (bytes + v.capacity() > kMaxBytes) return; // freed by the caller's vector
        bytes += v.capacity();
        free[v.size()].push_back(std::move(v));
    }
    void clear()
    {
        std::lock_guard lock(mutex);
        free.clear();
        bytes = 0;
    }
};

LayerPool& layerPool()
{
    static LayerPool* p = new LayerPool; // never destroyed: images may outlive statics
    return *p;
}

// An RGBA picture being worked on (layer, canvas). Its storage comes from and goes back to layerPool().
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    Image() = default;
    Image(int width, int height) : w(width), h(height), px(layerPool().take(size_t(width) * height * 4)) {}
    Image(const Image&) = default;
    Image& operator=(const Image&) = default;
    Image(Image&& o) noexcept : w(o.w), h(o.h), px(std::move(o.px)) { o.w = o.h = 0; }
    Image& operator=(Image&& o) noexcept
    {
        if (this != &o) {
            layerPool().give(std::move(px));
            px = std::move(o.px);
            w = std::exchange(o.w, 0);
            h = std::exchange(o.h, 0);
        }
        return *this;
    }
    ~Image() { layerPool().give(std::move(px)); }
    bool empty() const { return w <= 0 || h <= 0; }
    uint8_t* data() { return px.data(); }
    const uint8_t* data() const { return px.data(); }
};

bool fullyOpaque(const uint8_t* rgba, size_t pixels)
{
    for (size_t i = 0; i < pixels; ++i)
        if (rgba[4 * i + 3] != 255) return false;
    return true;
}

// Alpha times k (brightness filter with level 1: MLT's fade)
void scaleAlpha(Image& img, double k)
{
    if (k >= 1.0) return;
    const int m = int(std::lround(std::clamp(k, 0.0, 1.0) * 256));
    uint8_t* p = img.data();
    for (size_t i = 0, n = size_t(img.w) * img.h; i < n; ++i) p[4 * i + 3] = uint8_t((p[4 * i + 3] * m) >> 8);
}

// A kept region (Reframe::keepCopy: a game's kill feed above a short) sits on a thin dark frame with a soft outer edge,
// so it does not blend into similar scenery behind it. The frame is a small dark picture (straight alpha) drawn behind
// the cropped picture: `cropSrc` (source pixels) widened by the frame on each side; m = source pixels -> output pixels,
// frameSize = the frame's width in output pixels. Its texels are a third of the frame, the outer ones lighter.
struct KeepFrame {
    Image img;
    Compose::Affine m;
};
constexpr double kKeepFrame = 0.006; // frame width: of the output's shorter side (1080 x 1920: 6.5 pixels)
constexpr int kKeepFrameAlpha[3] = {110, 190, 230}; // texel rings from the outside in

bool keepFrame(const Compose::Affine& m, const QRectF& cropSrc, double frameSize, QSize output, KeepFrame& out)
{
    const double sx = std::hypot(m.a, m.d), sy = std::hypot(m.b, m.e); // output pixels per source pixel
    if (sx <= 0 || sy <= 0 || cropSrc.isEmpty() || frameSize <= 0) return false;
    const double texel = frameSize / 3;
    QRectF r = cropSrc.adjusted(-frameSize / sx, -frameSize / sy, frameSize / sx, frameSize / sy);
    // Only the part that can show: the output (widened by twice the frame, so the rings of an edge cut here stay
    // outside it) taken back into source pixels. A kept region zoomed far in made a frame picture of its whole size
    // (zoom 100: hundreds of MB and seconds per frame)
    const double det = m.a * m.e - m.b * m.d;
    if (std::abs(det) > 1e-12 && !output.isEmpty()) {
        const double pad = 2 * frameSize, W = output.width() + pad, H = output.height() + pad;
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (const QPointF& o : {QPointF(-pad, -pad), QPointF(W, -pad), QPointF(-pad, H), QPointF(W, H)}) {
            const double dx = o.x() - m.c, dy = o.y() - m.f;
            const double x = (m.e * dx - m.b * dy) / det, y = (-m.d * dx + m.a * dy) / det;
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        }
        r &= QRectF(QPointF(x0, y0), QPointF(x1, y1));
        if (r.isEmpty()) return false;
    }
    const int w = std::max(7, int(std::lround(r.width() * sx / texel))), h = std::max(7, int(std::lround(r.height() * sy / texel)));
    out.img = Image(w, h);
    uint8_t* p = out.img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x, p += 4) {
            const int ring = std::min({x, y, w - 1 - x, h - 1 - y});
            p[0] = p[1] = p[2] = 0;
            p[3] = uint8_t(kKeepFrameAlpha[std::min(ring, 2)]);
        }
    // texel (u, v) -> source (r.x + u * kx, r.y + v * ky) -> output through m
    const double kx = r.width() / w, ky = r.height() / h;
    out.m.a = m.a * kx;
    out.m.b = m.b * ky;
    out.m.c = m.a * r.x() + m.b * r.y() + m.c;
    out.m.d = m.d * kx;
    out.m.e = m.e * ky;
    out.m.f = m.d * r.x() + m.e * r.y() + m.f;
    return true;
}

// Linear mix of two same-size pictures in premultiplied alpha: a*(1-t) + b*t
void mix(Image& a, const Image& b, double t)
{
    const int k = int(std::lround(std::clamp(t, 0.0, 1.0) * 256)), j = 256 - k;
    uint8_t* d = a.data();
    const uint8_t* s = b.data();
    for (size_t i = 0, n = size_t(a.w) * a.h; i < n; ++i, d += 4, s += 4) {
        const int aa = d[3] * j, ba = s[3] * k;
        const int alpha = aa + ba; // * 256
        if (alpha <= 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
            continue;
        }
        for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp((d[c] * aa + s[c] * ba + alpha / 2) / alpha, 0, 255));
        d[3] = uint8_t((alpha + 128) >> 8);
    }
}

void fill(Image& img, const QColor& c)
{
    const uint8_t px[4] = {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), uint8_t(c.alpha())};
    uint32_t word;
    std::memcpy(&word, px, 4);
    uint8_t* p = img.data();
    const size_t n = size_t(img.w) * img.h;
    if (word == 0) {
        std::memset(p, 0, n * 4);
        return;
    }
    std::fill_n(reinterpret_cast<uint32_t*>(p), n, word); // vector storage is aligned for uint32_t
}

// ---- Sources ----

struct VideoSource {
    std::unique_ptr<Video::Reader> reader;
    Video::Info info;
    double fps = 0;     // stream frame rate (positions count in it)
    int length = 0;     // frames in the project rate (as MLT's producer length)
    bool hasAlpha = false; // pixel format with alpha
    QString file;          // the file decoded (the proxy in a preview with proxies)
    QString media;         // the media file of the project it stands for (named when it is lost)
    qint64 modified = 0;   // its time of change at opening (ms since the epoch): names its pictures with the path
};

struct StillSource {
    QImage image; // RGBA8888, straight alpha
    bool opaque = true;
};

struct Source {
    std::shared_ptr<VideoSource> video;
    std::shared_ptr<StillSource> still;
    bool failed = false;
    FileStamp::Stamp stamp; // of the media file when opened: another one = the file was overwritten, open it again
    std::chrono::steady_clock::time_point opened; // when it was opened (or the open failed): a lost one is retried
    bool lost = false;  // failed although the file opened before (Renderer::retryLost tries it again)
    uint64_t retry = 0; // Impl::retryGen when it was opened
    std::chrono::steady_clock::time_point used;
    int64_t usedFrame = -1; // last output frame that asked for it (export: dropped once rendering went past it)
};

// Output frame the calling thread renders right now (Renderer::render)
thread_local int64_t t_frame = -1;
// Its hints (null = none) and whether a close picture stood in for one of its videos
thread_local const RenderHints* t_hints = nullptr;
thread_local bool t_exact = true;
// A video's decoder was lost for good (Video::Reader::failed): an export renders no frame without it (render() null)
thread_local bool t_sourceLost = false;
thread_local QString t_lostFile; // its media file
// A lost source (its file opened before, then could not be opened or read: a USB disk or share gone) is opened again
// after the next seek or play start (Renderer::retryLost), and at most this often: a file back with the same content
// (FileStamp sees no change) shows again, a file still gone costs one failed open per seek, never one per frame (each
// open of a gone share may block). Files that never opened stay failed (offline media, unreadable imports).
constexpr std::chrono::milliseconds kRetryBroken{1500};

// GPU path (0.19): a clip whose compositing graph the card may evaluate. clipLayer() of that clip plans the graph
// (gpu/CompGpu) instead of evaluating it, and hands back where the result goes instead of drawing it
struct CompCapture {
    const Clip* clip = nullptr;
    Picture::Gpu::CompPlanPtr plan;
    Compose::Affine m; // result picture -> output pixels
    QRectF crop;       // result pixels kept (empty = all)
    double opacity = 1;
};
thread_local CompCapture* t_compCapture = nullptr;
// The timeline being drawn on this thread (the root, or the content of a compound clip): what a shape following a
// tracker looks its tracked clip up in. And a number per rendered frame, so per-frame caches never outlive it
thread_local const Timeline* t_timeline = nullptr;
thread_local uint64_t t_renderSeq = 0;
struct TimelineScope {
    const Timeline* outer;
    explicit TimelineScope(const Timeline& tl) : outer(t_timeline) { t_timeline = &tl; }
    ~TimelineScope() { t_timeline = outer; }
};

// Preview: what a decoder keeps of the pictures decoded on the way to a jump's target (steps back after a jump)
constexpr int64_t kSeekWindowBytes = 96ll * 1024 * 1024;

bool looksLikeStill(const QString& path)
{
    // Only a regular file: an image plugin asked about a device or a pipe (a broken project naming /dev/zero) read
    // it forever, and the render (preview, frames, export) hung with it
    if (!QFileInfo(path).isFile()) return false;
    const QByteArray fmt = QImageReader::imageFormat(path);
    return !fmt.isEmpty() && fmt != "gif"; // animated GIFs go through the decoder
}

QStringList wrapSubtitle(const QString& text, const QFontMetricsF& fm, double maxWidth)
{
    QStringList out;
    for (const QString& para : text.split('\n')) {
        QString line;
        for (const QString& word : para.split(' ', Qt::SkipEmptyParts)) {
            const QString candidate = line.isEmpty() ? word : line + ' ' + word;
            if (!line.isEmpty() && fm.horizontalAdvance(candidate) > maxWidth) {
                out << line;
                line = word;
            } else {
                line = candidate;
            }
        }
        out << line;
    }
    return out;
}

// A clip's tilt in depth at clip frame t (Inspector Kippen X/Y, Perspektive) around the pivot (px, py) in output
// pixels: the clip's center after its position, the point the rotation turns around. Off when not tilted (then every
// draw stays the flat affine one, bit for bit as before).
struct Tilt {
    double x = 0, y = 0, fov = 40, px = 0, py = 0;
    bool on() const { return std::abs(x) > 1e-9 || std::abs(y) > 1e-9; }
};

// Tilted at some frame (a static tilt or a keyframe off 0): such a clip is drawn by the CPU and from its own size
bool tiltedEver(const Clip& c)
{
    if (!Keys::hasTilt(c)) return false;
    for (AnimParam p : {AnimParam::TiltX, AnimParam::TiltY}) {
        if (std::abs(Keys::staticValue(c, p)) > 1e-9) return true;
        for (const Keyframe& k : c.keys.value(p))
            if (std::abs(k.value) > 1e-9) return true;
    }
    return false;
}

Tilt tiltOf(const Clip& c, int t, double px, double py)
{
    Tilt tl;
    if (!Keys::hasTilt(c)) return tl;
    tl.x = std::clamp(Keys::valueAt(c, AnimParam::TiltX, t), -89.0, 89.0);
    tl.y = std::clamp(Keys::valueAt(c, AnimParam::TiltY, t), -89.0, 89.0);
    tl.fov = std::clamp(Keys::valueAt(c, AnimParam::Fov, t), 5.0, 120.0);
    tl.px = px;
    tl.py = py;
    return tl;
}

// Compose::draw of src through the flat placement m, tilted in depth by tl when that is on (nothing drawn when the
// tilt puts the picture behind the camera)
void drawTilted(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh, const Compose::Affine& m,
                const Tilt& tl, double opacity)
{
    if (!tl.on()) {
        Compose::draw(dst, dw, dh, src, sw, sh, m, opacity);
        return;
    }
    Compose::Homography h;
    if (Compose::tilted(m, sw, sh, tl.px, tl.py, tl.x, tl.y, tl.fov, dw, dh, &h))
        Compose::draw(dst, dw, dh, src, sw, sh, h, opacity);
}

} // namespace

// ---- Frame ----

const uint8_t* Frame::bits() const { return m_buffer ? m_buffer->data.data() : nullptr; }
uint8_t* Frame::bits() { return m_buffer ? m_buffer->data.data() : nullptr; }

QImage Frame::view() const
{
    if (!m_buffer) return {};
    return QImage(m_buffer->data.data(), m_width, m_height, m_width * 4, QImage::Format_RGBA8888);
}

QImage Frame::toImage() const { return view().copy(); }

// ---- Renderer ----

struct Renderer::Impl {
    Options options;
    mutable std::mutex snapMutex;
    std::shared_ptr<const Timeline> snapshot;
    QSize output;
    std::atomic<int> direction{1};

    std::mutex sourceMutex;
    std::map<QString, Source> sources;
    QSet<QString> opened; // media files opened once (sourceMutex): failing later = lost
    std::atomic<uint64_t> retryGen{0}; // counts retryLost(): a lost source is tried again once per count
    mutable std::mutex lostMutex;
    QString lost; // media file of the last frame that went without a lost source (lostSource)
    std::mutex lengthMutex;
    std::map<QString, int> lengths; // file -> length in project frames (mediaLength)

    // Gradients of luma transitions and wipes (8-bit gray, rows tightly packed), by style and size: building one
    // costs more than the rest of the frame (iris 1080p: 75 ms; it was built for every frame of the transition)
    std::mutex gradientMutex;
    std::map<QString, std::shared_ptr<const std::vector<uint8_t>>> gradients;
    // Static parts of compositing graphs (text, flat colors, stills without keyframes), shared by all frames
    Comp::Cache compCache;
    std::mutex compLogMutex;
    QSet<int> compLogged; // clips whose graph the card did not take, logged once

    std::atomic<int> framesRendered{0}, layersDecoded{0}, layersSkipped{0}, framesGpu{0}, layersCpu{0}, layersCompGpu{0},
        gradientsBuilt{0};
    // GPU path (Options::gpu and a hardware device), looked up by the renders (GUI app: CPU until the GPU has started)
    std::atomic<Gpu::Service*> gpu{nullptr};
    std::atomic<bool> gpuDecided{false};
    Gpu::Service* gpuService()
    {
        if (!options.gpu) return nullptr;
        if (gpuDecided.load()) {
            Gpu::Service* s = gpu.load();
            if (s && !s->available()) { // went off for good (device lost): no more draws built for nothing
                gpu = nullptr;
                return nullptr;
            }
            return s;
        }
        bool decided = false;
        Gpu::Service* s = Gpu::Service::tryInstance(&decided);
        if (decided) {
            gpu = s;
            gpuDecided = true;
        }
        return s;
    }

    // What one render call works with
    struct Ctx {
        std::shared_ptr<const Timeline> root;
        int W = 0, H = 0;   // output
        double PW = 0, PH = 0; // project
        double fps = 25;
        bool preview = false;
        bool bypassGrade = false;
        // The video being made (progress bars fill over it): the timeline being rendered (TimelineOps::endFrame), in
        // an export of a range only that range
        int begin = 0, end = 0;
    };

    Source source(const QString& path, const QString& lane)
    {
        // Before the proxy lookup: a change seen here re-resolves the file's proxy first (Engine's observer)
        const FileStamp::Stamp stamp = FileStamp::recent(path);
        const QString file = options.proxy ? options.proxy(path) : path;
        const QString key = lane + '|' + file;
        {
            std::vector<Source> stale; // closed outside the lock
            Source retried;            // a broken one tried again (closed outside the lock too)
            std::lock_guard lock(sourceMutex);
            auto it = sources.find(key);
            if (it != sources.end() && it->second.stamp == stamp) {
                const auto now = std::chrono::steady_clock::now();
                const bool lost = it->second.lost || (it->second.video && it->second.video->reader->failed());
                if (!lost || it->second.retry == retryGen.load() || now - it->second.opened < kRetryBroken) {
                    it->second.used = now;
                    it->second.usedFrame = std::max(it->second.usedFrame, t_frame);
                    return it->second;
                }
                retried = std::move(it->second); // lost, and a seek or play since: try the file again
                sources.erase(it);
            }
            // Overwritten under the same name: everything kept of the old content goes - in every lane (mediaLength
            // reads any lane's source: one of the old content would bring the old length back)
            const QString suffix = '|' + file;
            for (auto o = sources.begin(); o != sources.end();) {
                if (o->second.stamp != stamp && o->first.endsWith(suffix)) {
                    stale.push_back(std::move(o->second));
                    o = sources.erase(o);
                } else {
                    ++o;
                }
            }
            if (!stale.empty()) {
                {
                    std::lock_guard lengthLock(lengthMutex);
                    lengths.erase(file);
                }
                compCache.clear(); // static media pictures of graphs
            }
        }
        Source s;
        s.stamp = stamp;
        s.retry = retryGen.load();
        if (looksLikeStill(file)) {
            QImageReader reader(file);
            reader.setAutoTransform(true);
            // As engine/video/Still: at most 4x the project's pixels
            QSize size = reader.size();
            if (reader.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
            const double limit = 4.0 * options.format.width * options.format.height;
            if (size.isValid() && double(size.width()) * size.height() > limit) {
                const double k = std::sqrt(limit / (double(size.width()) * size.height()));
                const QSize want(std::max(1, int(size.width() * k)), std::max(1, int(size.height() * k)));
                reader.setScaledSize(reader.transformation() & QImageIOHandler::TransformationRotate90 ? want.transposed()
                                                                                                       : want);
            }
            QImage img = reader.read();
            if (!img.isNull()) {
                s.still = std::make_shared<StillSource>();
                s.still->image = img.convertToFormat(QImage::Format_RGBA8888);
                s.still->opaque = fullyOpaque(s.still->image.constBits(),
                                              size_t(s.still->image.width()) * s.still->image.height());
            }
        }
        if (!s.still) {
            Video::Reader::Options ro;
            ro.threads = options.decoderThreads;
            ro.behind = std::max(4, options.parallelFrames);
            if (direction < 0) ro.behind = std::max(ro.behind, backwardKeep(0, 0)); // size not known yet: the budget's
            ro.ahead = std::max(6, options.parallelFrames + 2);
            if (options.mode == Mode::Export) {
                // Export asks in order (workers at most a few frames apart): the small default windows suffice.
                // Measured 1080p, 3 tracks, 10 workers: peak 1.68 -> 1.35 GB at the same frame rate.
                ro.behind = 4;
                ro.ahead = 6;
            }
            if (options.mode == Mode::Preview) ro.keepBytes = kSeekWindowBytes;
            ro.autoBackward = options.mode != Mode::Still;
            if (auto r = Video::Reader::open(file, ro)) {
                auto v = std::make_shared<VideoSource>();
                // Only when the file states none (as engine/video/Producer): overriding it put two pictures of a
                // variable-rate file on one position, the first one won (a flash frame of a phone video went missing)
                if (r->info().frameRate.num <= 0 || r->info().frameRate.den <= 0)
                    r->setFrameRate(AVRational{options.format.rate.num, options.format.rate.den});
                v->info = r->info();
                const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(v->info.format);
                v->hasAlpha = !desc || (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
                v->fps = v->info.frameRate.num > 0 && v->info.frameRate.den > 0 ? av_q2d(v->info.frameRate)
                                                                                 : options.format.rate.fps();
                if (v->info.duration > 0)
                    v->length = int(double(v->info.duration) / AV_TIME_BASE * options.format.rate.fps() + 0.5);
                if (direction < 0) r->setBehind(std::max(4, backwardKeep(v->info.width, v->info.height)));
                v->file = file;
                v->media = path;
                v->modified = QFileInfo(file).lastModified().toMSecsSinceEpoch();
                v->reader = std::move(r);
                s.video = std::move(v);
            }
        }
        s.failed = !s.video && !s.still;
        s.used = s.opened = std::chrono::steady_clock::now();
        s.usedFrame = t_frame;
        bool lost = false;
        Source result;
        {
            std::lock_guard lock(sourceMutex);
            if (!s.failed) opened.insert(path);
            else lost = s.lost = opened.contains(path);
            result = sources.emplace(key, s).first->second;
        }
        if (lost) {
            // It opened before: lost while the project is open (a disk or share gone, the file moved). An export stops
            // naming it; the preview tells (Options::sourceLost) and tries again on a later request (kRetryBroken)
            t_sourceLost = true;
            t_lostFile = path;
            if (options.sourceLost) options.sourceLost(path);
        }
        return result;
    }

    // Pictures a decoder keeps behind the last request while playing backwards: a whole GOP if memory allows (each
    // picture of it is then decoded once per GOP instead of decoding from the keyframe for every picture)
    static int backwardKeep(int width, int height)
    {
        constexpr double kBudget = 384.0 * 1024 * 1024; // bytes per decoder
        constexpr int kMost = 250;
        if (width <= 0 || height <= 0) return 60;
        const double bytes = 1.5 * width * height; // 8-bit 4:2:0
        return std::clamp(int(kBudget / bytes), 8, kMost);
    }

    // Source length in project frames for TimelineOps::transitions (0 = unlimited: stills, titles)
    int sourceLength(const Clip& c, const Timeline& tl)
    {
        if (c.isNested()) {
            if (!tl.nested) return 0;
            const auto it = tl.nested->constFind(c.sequenceId);
            return it == tl.nested->cend() ? 0 : TimelineOps::endFrame(*it);
        }
        if (c.isTitle() || c.mediaPath.isEmpty()) return 0;
        const int length = mediaLength(c.mediaPath);
        return length > 0 ? c.retimedLength(length) : 0;
    }

    // Length of a video file in project frames as its Source has it (0 = none: stills, unreadable), once per file.
    // Asked for both clips of every transition on a track at every frame: a decoder of its own per file stayed open
    // for good (never unused, so neither trim() nor the export's dropBefore() closed it) – 60 clips with dissolves
    // opened 60 decoders before the first frame.
    int mediaLength(const QString& path)
    {
        const QString file = options.proxy ? options.proxy(path) : path;
        {
            std::lock_guard lock(lengthMutex);
            const auto it = lengths.find(file);
            if (it != lengths.end()) return it->second;
        }
        int length = -1;
        {
            std::lock_guard lock(sourceMutex);
            for (const auto& [key, src] : sources)
                if (src.video && key.endsWith('|' + file)) {
                    length = src.video->length;
                    break;
                }
        }
        if (length < 0) { // not drawn yet: the container's duration, the decoder closed again at once
            length = 0;
            Video::Reader::Options ro;
            ro.threads = 1;
            ro.hardware = false;
            if (!looksLikeStill(file))
                if (auto r = Video::Reader::open(file, ro); r && r->info().duration > 0)
                    length = int(double(r->info().duration) / AV_TIME_BASE * options.format.rate.fps() + 0.5);
        }
        std::lock_guard lock(lengthMutex);
        return lengths.emplace(file, length).first->second;
    }

    // loadLut keeps the loaded LUTs itself and reads a file again once its time or size changed (a LUT overwritten,
    // or one on a drive mounted after the project was opened); a cache by path here would keep the first result
    static std::shared_ptr<const ColorGrade::Lut> lut(const QString& path) { return ColorGrade::loadLut(path); }

    // An effect instance's values for VideoEffects::apply (by property name, keyframes at clip frame t)
    static VideoEffects::Params effectParams(const Clip& c, const EffectInstance& inst, const EffectDescriptor& d, double t)
    {
        auto find = [&d](const char* name) -> const EffectParam* {
            for (const EffectParam& p : d.params)
                if (p.mltProperty == QLatin1String(name)) return &p;
            return nullptr;
        };
        VideoEffects::Params out;
        out.has = [find](const char* name) { return find(name) != nullptr; };
        out.num = [&c, &inst, find, t](const char* name) {
            const EffectParam* p = find(name);
            if (p->anim != AnimParam::Count && Keys::animated(c, p->anim)) return Keys::valueAt(c, p->anim, t);
            const QVariant v = inst.params.value(p->key, p->defaultValue);
            return p->type == EffectParam::Bool ? (v.toBool() ? 1.0 : 0.0) : v.toDouble();
        };
        out.color = [&inst, find](const char* name) {
            const EffectParam* p = find(name);
            return inst.params.value(p->key, p->defaultValue).value<QColor>();
        };
        out.text = [&inst, find](const char* name) {
            const EffectParam* p = find(name);
            return inst.params.value(p->key, p->defaultValue).toString();
        };
        return out;
    }

    // A compound clip's inner sequence while its picture is made (clipLayer): the compositing graph reads its tracks
    // one by one. deferred = the inner picture is rendered only when something asks for it (a graph made of tracks)
    struct Nested {
        const Timeline* inner = nullptr;
        int in = 0;
        Ctx sub;
        QString prefix;
        int depth = 0;
        bool deferred = false;
    };
    // Effects on the clip's picture (before crop/transform), clip frame t; order as TimelineBuilderDetail::applyEffects:
    // the fixed stages first (grade, then the Green Screen key), then every other effect in the clip's order.
    // frameHeight: the frame's height in pixels of img (blur strength is relative to it); fit: the size img stands for
    // in project pixels (a source of another aspect ratio: the fitted picture without the bars, centred in the frame)
    void applyEffects(Image& img, const Clip& c, double t, const Ctx& ctx, double frameHeight, QSizeF fit,
                      const QString& lane = {}, Nested* nested = nullptr)
    {
        // Stabilization first (it has to see the camera picture): the analyzed correction at the clip's material
        // frame. Border "keep" has no previous output picture here: edge pixels (as a seek in the MLT filter).
        // Analyzed on the whole frame (bars included): the fitted picture is warped in the frame's units.
        if (const auto st = StabilizeCorrection::of(c, options.format.rate, ctx.PH > 0 ? ctx.PW / ctx.PH : 0.0)) {
            Image warped(img.w, img.h);
            if (StabilizeCorrection::warpRgba(*st, int(std::floor(c.in + t + 1e-6)), img.data(), warped.data(), img.w,
                                              img.h, nullptr, fit.width() / ctx.PW, fit.height() / ctx.PH))
                img = std::move(warped);
        }
        if (ColorGrade::active(c) && !ctx.bypassGrade) {
            const ColorGrade::Params p = ColorGrade::at(c, t);
            const QString lp = ColorGrade::lutPath(c);
            const auto l = lp.isEmpty() ? nullptr : lut(lp);
            if (!p.isNeutral() || l) ColorGrade::apply(img.data(), img.w, img.h, *ColorGrade::bake(p, l.get()), 1);
            for (const ColorGrade::Window& w : ColorGrade::windowsAt(c, t))
                ColorGrade::applyWindow(img.data(), img.w, img.h, ColorGrade::onPicture(w, QSizeF(ctx.PW, ctx.PH), fit));
        }
        if (const EffectInstance* key = EffectRegistry::instance(c, "chromakey"); key && key->enabled)
            if (const EffectDescriptor* d = EffectRegistry::find(key->effectId))
                VideoEffects::apply(key->effectId, img.data(), img.w, img.h, effectParams(c, *key, *d, t), int64_t(c.in + t));
        for (const EffectInstance& inst : c.effects) {
            if (!inst.enabled) continue;
            const EffectDescriptor* d = EffectRegistry::find(inst.effectId);
            if (!d || d->fixed || !d->video) continue;
            using P = AnimParam;
            if (inst.effectId == QLatin1String("color")) {
                const double temp = Keys::valueAt(c, P::FxTemp, t), tint = Keys::valueAt(c, P::FxTint, t);
                const double sat = Keys::valueAt(c, P::FxSaturation, t);
                if (temp != 0 || tint != 0 || sat != 0) applyMatrix(img.data(), img.w, img.h, colorMatrix(temp, tint, sat));
                const double bright = Keys::valueAt(c, P::FxBrightness, t), contrast = Keys::valueAt(c, P::FxContrast, t);
                if (bright != 0 || contrast != 0) applyLevels(img.data(), img.w, img.h, levels(bright, contrast));
            } else if (inst.effectId == QLatin1String("blur")) {
                // Strength 100 = 1/20 of the frame height
                const double sigma = std::max(0.0, Keys::valueAt(c, P::FxBlur, t)) * frameHeight / 2000.0;
                VideoEffects::gaussian(img.data(), img.w, img.h, sigma);
            } else if (Custom::isCustom(inst.effectId)) {
                Custom::applyToClip(c, inst, t, ctx.fps, img.data(), img.w, img.h); // filter chain, shader (0.19)
            } else if (!VideoEffects::apply(inst.effectId, img.data(), img.w, img.h, effectParams(c, inst, *d, t),
                                            int64_t(c.in + t))) {
                static std::once_flag warned;
                std::call_once(warned, [&] {
                    qWarning("Picture engine: effect %s not supported yet (skipped)", qPrintable(inst.effectId));
                });
            }
        }
        // Compositing graph last (0.19): its input "clip" is the picture with every effect above
        const EffectInstance* comp = EffectRegistry::instance(c, ::Comp::kEffectId);
        const bool composed = comp && comp->enabled && applyComp(img, c, *comp, t, ctx, frameHeight / ctx.PH, lane, nested);
        if (!composed && nested && nested->deferred) renderNested(img, *nested);
        // Clip in a shape (0.22) last: the cut edge, stroke and shadow stay as set whatever the effects did
        if (Shapes::ClipShape cs; !img.empty() && Shapes::clipShapeAt(c, t, &cs))
            ShapePaint::clipShape(img.data(), img.w, img.h, cs, frameHeight / std::max(1.0, ctx.PH));
    }

    // The clip's only picture effect is an enabled compositing graph (no grade, stabilization, other effects)
    bool onlyComp(const Clip& c) const
    {
        bool comp = false;
        for (const EffectInstance& e : c.effects) {
            if (!e.enabled) continue;
            if (e.effectId != QLatin1String(::Comp::kEffectId)) {
                const EffectDescriptor* d = EffectRegistry::find(e.effectId);
                if (!d || d->video) return false;
                continue;
            }
            comp = true;
        }
        return comp && !ColorGrade::active(c);
    }
    void renderNested(Image& img, Nested& n)
    {
        renderTimeline(*n.inner, n.in, img, n.sub, n.prefix, false, n.depth + 1);
        n.deferred = false;
    }

    // Evaluates the clip's compositing graph into img (same size, CompEval); false = the picture stays as it is.
    // scale: project pixels -> pixels of img
    bool applyComp(Image& img, const Clip& c, const EffectInstance& comp, double t, const Ctx& ctx, double scale,
                   const QString& lane, Nested* nested)
    {
        if (img.empty()) return false;
        Comp::Buf clip; // img's pixels, lent to the graph while it runs
        clip.w = img.w;
        clip.h = img.h;
        clip.px.swap(img.px);
        Comp::Env env;
        env.t = t;
        env.frameNo = int64_t(std::floor(c.in + t + 1e-6));
        env.size = QSize(img.w, img.h);
        env.scale = scale;
        env.fps = ctx.fps;
        env.cancel = t_hints ? t_hints->cancel : nullptr;
        env.clip = &c;
        env.takeBytes = [](size_t n) { return layerPool().take(n); };
        env.giveBytes = [](std::vector<uint8_t>&& v) { layerPool().give(std::move(v)); };
        env.clipPicture = [&]() -> const Comp::Buf* {
            if (nested && nested->deferred) {
                Image whole;
                whole.w = clip.w;
                whole.h = clip.h;
                whole.px.swap(clip.px);
                renderNested(whole, *nested);
                whole.px.swap(clip.px);
                whole.w = whole.h = 0;
            }
            return &clip;
        };
        if (nested && nested->inner)
            env.paintTrack = [&, nested](int track, Comp::Buf& onto) {
                if (track < 0 || track >= nested->inner->video.size() || onto.kind != Comp::Buf::Rgba) return false;
                Image canvas;
                canvas.w = onto.w;
                canvas.h = onto.h;
                canvas.px.swap(onto.px);
                drawTrack(canvas, false, nested->inner->video[track], track, nested->in, nested->sub, *nested->inner,
                          nested->prefix, nested->depth + 1);
                canvas.px.swap(onto.px);
                canvas.w = canvas.h = 0;
                return true;
            };
        env.mediaPicture = [&](const QString& path, double clipTime) -> Comp::Buf {
            if (path.isEmpty() || clipTime < -1e-6) return {};
            Clip m;
            m.mediaPath = path;
            m.out = std::numeric_limits<int>::max() / 2;
            const QString mlane = lane + QStringLiteral("m");
            const QSizeF fit = MediaAspect::fit(nativeAspect(m, mlane), QSizeF(clip.w, clip.h));
            const int fw = std::max(1, int(std::lround(fit.width()))), fh = std::max(1, int(std::lround(fit.height())));
            QSize native;
            Image pic = mediaPicture(m, int(std::floor(clipTime + 1e-6)), mlane, fw, fh, ctx, &native);
            if (pic.empty()) return {};
            layersDecoded.fetch_add(1, std::memory_order_relaxed);
            Comp::Buf b;
            b.w = clip.w;
            b.h = clip.h;
            if (fw == clip.w && fh == clip.h) {
                b.px.swap(pic.px);
                pic.w = pic.h = 0;
                return b;
            }
            b.px = layerPool().take(size_t(b.w) * b.h * 4);
            Compose::Affine a;
            a.c = (b.w - fw) / 2;
            a.f = (b.h - fh) / 2;
            Compose::draw(b.data(), b.w, b.h, pic.data(), fw, fh, a, 1.0);
            return b;
        };
        if (t_compCapture && t_compCapture->clip == &c && !t_compCapture->plan) {
            // The card evaluates the graph (its CPU nodes are evaluated and kept in the plan here); img stays
            QString why;
            // Nodes outside their time range are out of the way here already (0.22): the card sees what works at t
            EffectInstance active;
            const bool ranged = Comp::activeGraph(comp, t, ctx.fps, &c, &active);
            if ((t_compCapture->plan = Gpu::planComp(ranged ? active : comp, env, &compCache, &why))) {
                img.px.swap(clip.px);
                return true;
            }
            const std::lock_guard lock(compLogMutex); // why a graph stays on the CPU: once per clip
            if (!compLogged.contains(c.id)) {
                compLogged.insert(c.id);
                qDebug("Picture engine: compositing graph of clip %d on the CPU (%s)", c.id, qPrintable(why));
            }
        }
        Comp::Buf out;
        Comp::Stats stats;
        const bool ok = Comp::evaluate(comp, env, &compCache, &out, &stats);
        if (!stats.unknownKinds.isEmpty()) {
            static std::once_flag warned;
            std::call_once(warned, [&] {
                qWarning("Picture engine: compositing node kind %s unknown (passed through)",
                         qPrintable(QStringList(stats.unknownKinds.values()).join(", ")));
            });
        }
        if (!ok || out.px.size() != size_t(img.w) * img.h * 4) {
            if (!out.px.empty()) layerPool().give(std::move(out.px));
            img.px.swap(clip.px);
            return false;
        }
        img.px.swap(out.px);
        layerPool().give(std::move(out.px));
        layerPool().give(std::move(clip.px));
        return true;
    }

    // Source picture of a media clip at clip frame t, fetched at (dw x dh); empty = none
    Image mediaPicture(const Clip& c, int t, const QString& lane, int dw, int dh, const Ctx& ctx, QSize* native,
                       uint8_t* into = nullptr)
    {
        const Source s = source(c.mediaPath, lane);
        if (s.still) {
            const QImage& img = s.still->image;
            *native = img.size();
            if (dw <= 0 || dh <= 0) return {};
            Image out;
            uint8_t* dst = into;
            if (!dst) {
                out = Image(dw, dh);
                dst = out.data();
            } else { // straight into the caller's picture (a layer that covers the frame): size only, as videoFrame
                out.w = dw;
                out.h = dh;
            }
            if (img.width() == dw && img.height() == dh) {
                std::memcpy(dst, img.constBits(), size_t(dw) * dh * 4);
            } else {
                Compose::Affine m;
                m.a = double(dw) / img.width();
                m.e = double(dh) / img.height();
                Compose::draw(dst, dw, dh, img.constBits(), img.width(), img.height(), m, 1.0);
            }
            return out;
        }
        if (!s.video) return {};
        if (dw > 0 && dh > 0) {
            Image between;
            if (inBetween(c, t, lane, *s.video, dw, dh, ctx, native, into, &between)) return between;
        }
        const double file = fileFrame(c, t, *s.video, ctx);
        if (file < 0) return {};
        return videoFrame(*s.video, file, dw, dh, ctx, native, into);
    }

    // Slow motion with in-between pictures (Clip::interpolate): where clip frame t falls between two stream frames,
    // the picture between them. false = not slower than the stream there, exactly on a frame, or a frame missing
    // (the caller shows the plain frame)
    // Where clip frame t lies between stream frames i0 and i0 + 1 (frac 0..1); false = no in-between there
    // Stream position (stream frames, fractional) of clip frame t where an in-between mode plays the clip slower
    // than its stream; false = repeat mode, a freeze frame or not slower there
    static bool slowStreamPosition(const Clip& c, int t, const VideoSource& v, const Ctx& ctx, double* q)
    {
        if (c.interpolate == FrameInterp::Repeat || !c.isRetimed() || c.freeze || v.fps <= 0 || ctx.fps <= 0) return false;
        const RetimeMap map(c, v.length);
        const double m = c.in + t;
        const double perFrame = map.speedAt(m) * v.fps / ctx.fps; // stream frames per output frame
        if (perFrame >= 1.0 - 1e-6) return false;
        // Backwards: from the end of the stream frames the file frame covers (project frame f holds stream frames
        // f*r .. (f+1)*r - 1); f*r started a 60 fps clip in a 30 fps project one stream frame early and never
        // showed its last one
        const double f = map.fileFrameAt(m), r = v.fps / ctx.fps;
        *q = c.reverse ? (f + 1) * r - 1 : f * r;
        return true;
    }

    static bool betweenFrames(const Clip& c, int t, const VideoSource& v, const Ctx& ctx, int64_t* i0, double* frac)
    {
        double q = 0;
        if (!slowStreamPosition(c, t, v, ctx, &q)) return false;
        *i0 = int64_t(std::floor(q + 1e-6));
        *frac = q - double(*i0);
        const int64_t streamLength = int64_t(double(v.length) / ctx.fps * v.fps);
        return *frac >= 1e-3 && *frac <= 1 - 1e-3 && *i0 >= 0 && (streamLength <= 0 || *i0 + 1 < streamLength);
    }

    static bool inBetween(const Clip& c, int t, const QString& lane, const VideoSource& v, int dw, int dh, const Ctx& ctx,
                          QSize* native, uint8_t* into, Image* result)
    {
        int64_t i0 = 0;
        double frac = 0;
        if (!betweenFrames(c, t, v, ctx, &i0, &frac)) return false;
        Video::Reader::Fetch fetch;
        if (t_hints) {
            fetch.nearest = t_hints->nearest;
            fetch.cancel = t_hints->cancel;
        }
        bool exactA = true, exactB = true;
        const auto pa = v.reader->picture(i0, fetch, &exactA);
        if (!pa || !pa->frame || !exactA) return false;
        const auto pb = v.reader->picture(i0 + 1, fetch, &exactB);
        if (!pb || !pb->frame || !exactB) return false;
        Image a(dw, dh), b(dw, dh);
        if (!Video::toRgba(*pa, v.info, a.data(), dw, dh, ctx.preview) ||
            !Video::toRgba(*pb, v.info, b.data(), dw, dh, ctx.preview))
            return false;
        *native = QSize(pa->frame->width, pa->frame->height);
        Image out;
        uint8_t* dst = into;
        if (!dst) {
            out = Image(dw, dh);
            dst = out.data();
        } else { // straight into the caller's picture: size only, as videoFrame
            out.w = dw;
            out.h = dh;
        }
        if (c.interpolate == FrameInterp::Blend) {
            if (Interp::isCut(a.data(), b.data(), dw, dh)) // no ghost of the other shot across a cut
                std::memcpy(dst, (frac < 0.5 ? a : b).data(), size_t(dw) * dh * 4);
            else
                Interp::blend(a.data(), b.data(), dst, dw, dh, frac);
        } else {
            // The pictures really decoded: proxy and original (or a file replaced since) never share motion; the
            // preview converts colours the fast way
            const QString key = QString("%1|%2|%3|%4|%5|%6x%7")
                                    .arg(v.file, lane)
                                    .arg(v.modified)
                                    .arg(int(ctx.preview))
                                    .arg(i0)
                                    .arg(dw)
                                    .arg(dh);
            const auto motion = Interp::cachedMotion(key, a.data(), b.data(), dw, dh);
            Interp::flow(*motion, a.data(), b.data(), dst, dw, dh, frac);
        }
        *result = std::move(out);
        return true;
    }

    // Frame of the file (project rate) a video clip shows at clip frame t; < 0 = none
    static double fileFrame(const Clip& c, int t, const VideoSource& v, const Ctx& ctx)
    {
        // Material frame -> file frame (project rate) -> stream position
        const double m = c.freeze ? c.stillFrame() : c.in + t;
        double file = m;
        // Speed, reverse, ramp and freeze frame: the one mapping of RetimeMap (by the sound's time)
        if (c.isRetimed()) file = RetimeMap(c, v.length).pictureFrame(m);
        // In-between modes on (or next to) a stream frame: that stream frame itself. The project-rate frame above
        // is rounded down: a 60 fps clip at 25 % in a 30 fps project would show stream frame 0 where frame 1 is
        // due, the picture jumping back between two in-between pictures (0, 0.5, 0, 1.5, 2 ...)
        const int64_t streamLength = ctx.fps > 0 ? int64_t(double(v.length) / ctx.fps * v.fps) : 0;
        const auto streamFrame = [&](int64_t frame) {
            frame = std::max<int64_t>(0, frame);
            if (streamLength > 0) frame = std::min(frame, streamLength - 1);
            return double(frame) * ctx.fps / v.fps;
        };
        if (double q = 0; slowStreamPosition(c, t, v, ctx, &q)) return streamFrame(std::llround(q));
        // Repeat mode with a stream faster than the project (60 fps gameplay slowed down in a 30 fps edit): the
        // stream frame that started last, like pictureFrame but counted in stream frames. Rounded in project frames,
        // every other stream frame was never shown (at 25 %: 0, 0, 0, 0, 2, 2, 2, 2 instead of 0, 0, 1, 1, 2, 2)
        if (c.isRetimed() && !c.freeze && ctx.fps > 0 && v.fps > ctx.fps * (1 + 1e-6)) {
            const RetimeMap map(c, v.length);
            const double f = map.fileFrameAt(c.in + t), r = v.fps / ctx.fps;
            const int64_t frame = c.reverse ? int64_t(std::ceil((f + 1) * r - 1e-6)) - 1 : int64_t(std::floor(f * r + 1e-6));
            return streamFrame(frame);
        }
        return file;
    }

    // The decoded picture of frame `file` (project rate) of a video; nullptr = none
    static std::shared_ptr<const Video::Picture> fetchPicture(const VideoSource& v, double file, const Ctx& ctx)
    {
        const int64_t request = int64_t(file / ctx.fps * v.fps + 0.5);
        Video::Reader::Fetch fetch;
        if (t_hints) {
            fetch.nearest = t_hints->nearest;
            fetch.cancel = t_hints->cancel;
        }
        bool exact = true;
        auto pic = v.reader->picture(request, fetch, &exact);
        if (!exact) t_exact = false;
        if (!pic && v.reader->failed()) {
            t_sourceLost = true;
            t_lostFile = v.media;
        }
        if (!pic || !pic->frame) return {};
        return pic;
    }

    // Frame `file` (project rate) of a video at dw x dh (into: straight into that picture, see mediaPicture)
    static Image videoFrame(const VideoSource& v, double file, int dw, int dh, const Ctx& ctx, QSize* native,
                            uint8_t* into = nullptr)
    {
        const auto pic = fetchPicture(v, file, ctx);
        if (!pic) return {};
        *native = QSize(pic->frame->width, pic->frame->height);
        if (dw <= 0 || dh <= 0) return {};
        if (into) { // straight into the caller's picture (a layer that covers the frame)
            if (!Video::toRgba(*pic, v.info, into, dw, dh, ctx.preview)) return {};
            Image done;
            done.w = dw;
            done.h = dh;
            return done; // size only: the pixels are in `into`
        }
        Image out(dw, dh);
        if (!Video::toRgba(*pic, v.info, out.data(), dw, dh, ctx.preview)) return {};
        return out;
    }

    double nativeAspect(const Clip& c, const QString& lane)
    {
        if (c.pictureAspect > 0 && c.kind == ClipKind::Media) return c.pictureAspect; // a stretched recording put right
        const Source s = source(c.mediaPath, lane);
        if (s.still) return double(s.still->image.width()) / std::max(1, s.still->image.height());
        if (!s.video) return 0;
        const Video::Info& i = s.video->info;
        double w = i.width, h = i.height;
        if (w <= 0 || h <= 0) return 0;
        w *= i.sar.num > 0 && i.sar.den > 0 ? av_q2d(i.sar) : 1.0;
        const bool sideways = std::lround(std::abs(i.rotation)) % 180 == 90;
        return sideways ? h / w : w / h;
    }

    // Title text onto an output-size layer (clip frame t)
    // Draws the title onto layer, whose top left is at (shift) of the output frame. bounds: instead of drawing, the
    // part of the output frame it covers (whole pixels, inside the frame)
    // The entry/exit animation (TitleStyle::enter, exit) is applied here: scale and rise in the layout, a partial
    // opacity by drawing the title on its own and laying it on.
    void drawTitle(Image& layer, const Clip& c, double t, const Ctx& ctx, double sizeOverride = 0, QPoint shift = {},
                   QRect* bounds = nullptr)
    {
        Generators::TitleMotion m = Generators::titleMotion(c.title, t, c.length(), ctx.fps);
        if (c.title.isShape()) { // a shape following a tracker fades with it when the point is lost
            Masks::Follow f;
            double seen = 1;
            if (shapeFollow(c, t, ctx, &f, &seen)) m.opacity *= seen;
        }
        if (m.opacity <= 0) {
            if (bounds) *bounds = QRect();
            return;
        }
        if (m.opacity >= 1 || bounds) {
            drawTitleAt(layer, c, t, ctx, sizeOverride, shift, bounds, m);
            return;
        }
        QRect box;
        drawTitleAt(layer, c, t, ctx, sizeOverride, shift, &box, m);
        box &= QRect(shift, QSize(layer.w, layer.h));
        if (box.isEmpty()) return;
        Image part(box.width(), box.height());
        drawTitleAt(part, c, t, ctx, sizeOverride, box.topLeft(), nullptr, m);
        const int x0 = box.x() - shift.x(), y0 = box.y() - shift.y();
        for (int y = 0; y < part.h; ++y)
            Compose::over(layer.data() + (size_t(y0 + y) * layer.w + x0) * 4, part.data() + size_t(y) * part.w * 4,
                          part.w, 1, m.opacity);
    }

    void drawTitleAt(Image& layer, const Clip& c, double t, const Ctx& ctx, double sizeOverride, QPoint shift,
                     QRect* bounds, const Generators::TitleMotion& m)
    {
        const TitleStyle& st = c.title;
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        if (st.isProgress()) {
            drawProgress(layer, c, t, ctx, shift, bounds);
            return;
        }
        if (st.isShape()) {
            drawShape(layer, c, t, ctx, shift, bounds, m);
            return;
        }
        double size = std::max(1.0, sizeOverride > 0 ? sizeOverride : st.size);
        const double rise = m.rise * size; // a slide moves by the size drawn (keyframed too)
        if (m.scale != 1) size = std::max(1.0, size * m.scale);
        // An animated size scales the whole title: outline and box margin with it
        const double grow = st.size > 0 ? size / st.size : 1.0;
        const double outlineWidth = st.outlineWidth * grow, boxPad = std::max(0.0, st.boxPad) * grow;
        QFont font(st.fontFamily());
        font.setPixelSize(std::max(1, int(std::lround(size))));
        font.setBold(st.bold);
        font.setItalic(st.italic);
        const QFontMetricsF fm(font);
        // A counter's block keeps the size of its widest value while the number changes (no jumping)
        const QString text = st.isCounter() ? Generators::counterText(st, int(std::lround(t)), c.length()) : st.text;
        double w = 1;
        for (const QString& line : (st.isCounter() ? Generators::counterWidest(st, fm) : st.text).split('\n'))
            w = std::max(w, fm.horizontalAdvance(line));
        const bool outline = st.outlineOn && outlineWidth > 0;
        if (st.look.solidOn) {
            drawTitle3d(layer, c, t, ctx, text, w, size, boxPad, shift, bounds, rise);
            return;
        }
        // Fill, edge, shadow, glow (0.19): only a styled title takes this path, a plain one is drawn as before
        Text::Look look;
        const bool styled = st.look.active();
        if (styled) {
            QImage texture;
            if (st.look.fill == TitleFill::Texture && !st.look.texture.isEmpty())
                if (const Source s = source(st.look.texture, QStringLiteral("title")); s.still) texture = s.still->image;
            look = titleLook(st.look, st.color, grow * sy, texture);
        }
        auto layerOf = [&](bool outlineLayer) {
            Text::Params p;
            if (styled) {
                p.look = &look;
                p.lookBehind = outlineLayer || !outline;
                p.lookFill = !outlineLayer;
            }
            p.text = text;
            const double x = (ctx.PW - w) / 2 + Keys::valueAt(c, AnimParam::TitlePosX, t);
            const double y = -Keys::valueAt(c, AnimParam::TitlePosY, t) - rise;
            p.area = QRectF(x * sx, y * sy, w * sx, ctx.PH * sy);
            QFont f(st.fontFamily());
            f.setPixelSize(std::max(1, int(std::lround(size * sy))));
            f.setWeight(st.bold ? QFont::Bold : QFont::Normal);
            f.setItalic(st.italic);
            p.font = f;
            p.halign = st.align == 0 ? 0 : st.align == 2 ? 2 : 1;
            p.valign = 1;
            p.fg = outlineLayer ? st.outlineColor
                                : (Keys::animated(c, AnimParam::TitleColor)
                                       ? Keys::toColor(Keys::valueAt(c, AnimParam::TitleColor, t))
                                       : st.color);
            const bool box = st.boxOn && (outlineLayer || !outline);
            p.bg = box ? st.boxColor : QColor(Qt::transparent);
            p.pad = (st.boxOn ? boxPad : 0.0) * sy;
            p.outlineColor = st.outlineColor;
            p.outline = (outlineLayer ? 2 * outlineWidth : 0.0) * sy;
            p.room = (outline ? 2 * outlineWidth : 0.0) * sy;
            if (bounds) {
                *bounds |= Text::bounds(p).toAlignedRect();
                return;
            }
            p.area.translate(-shift);
            Text::draw(layer.data(), layer.w, layer.h, p);
        };
        if (bounds) *bounds = QRect();
        if (outline) layerOf(true);
        layerOf(false);
        if (bounds) *bounds &= QRect(0, 0, ctx.W, ctx.H);
    }

    // Real 3D letters (TitleLook::solidOn, engine/text3d) over the box: turned, they may reach anywhere in the picture.
    // text/w/size/boxPad as drawTitle laid them out
    void drawTitle3d(Image& layer, const Clip& c, double t, const Ctx& ctx, const QString& text, double w, double size,
                     double boxPad, QPoint shift, QRect* bounds, double rise)
    {
        const TitleStyle& st = c.title;
        const TitleLook& l = st.look;
        if (bounds) {
            *bounds = QRect(0, 0, ctx.W, ctx.H);
            return;
        }
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        const double grow = st.size > 0 ? size / st.size : 1.0;
        Text::Params p;
        p.text = text;
        const double x = (ctx.PW - w) / 2 + Keys::valueAt(c, AnimParam::TitlePosX, t);
        const double y = -Keys::valueAt(c, AnimParam::TitlePosY, t) - rise;
        p.area = QRectF(x * sx, y * sy, w * sx, ctx.PH * sy).translated(-shift);
        QFont f(st.fontFamily());
        f.setPixelSize(std::max(1, int(std::lround(size * sy))));
        f.setWeight(st.bold ? QFont::Bold : QFont::Normal);
        f.setItalic(st.italic);
        p.font = f;
        p.halign = st.align == 0 ? 0 : st.align == 2 ? 2 : 1;
        p.valign = 1;
        p.pad = (st.boxOn ? boxPad : 0.0) * sy;
        if (st.boxOn && st.boxColor.alpha() > 0) { // the box alone (letters in no colour)
            Text::Params box = p;
            box.bg = st.boxColor;
            box.fg = Qt::transparent;
            Text::draw(layer.data(), layer.w, layer.h, box);
        }
        const double depth = std::max(0.0, l.solidDepth) * grow * sy;
        const Text3d::Mesh mesh = Text3d::extrude(Text::letters(p), depth, std::max(0.0, l.solidBevel) * grow * sy);
        Text3d::View v;
        v.rotX = l.solidRotX;
        v.rotY = l.solidRotY;
        v.rotZ = l.solidRotZ;
        v.pivot = v.at = p.area.center();
        v.depth = depth;
        v.fov = l.solidFov;
        v.distance = l.solidDistance;
        v.lightAngle = l.solidLightAngle;
        v.lightHeight = l.solidLightHeight;
        v.front = Keys::animated(c, AnimParam::TitleColor) ? Keys::toColor(Keys::valueAt(c, AnimParam::TitleColor, t))
                                                           : st.color;
        v.side = l.solidSide;
        v.shine = l.solidShine;
        v.metal = l.solidMetal;
        Text3d::render(mesh, v, layer.data(), layer.w, layer.h);
    }

    // Where a shape title following a tracker (ShapeStyle::followClip/followNode) is carried at its clip frame t:
    // the tracked clip's tracker values (0..1 of its picture) placed where that picture lies in the frame at zoom and
    // position (its rotation and crop are left out), in project pixels. seen = the tracker's visibility. false = none
    bool shapeFollow(const Clip& c, double t, const Ctx& ctx, Masks::Follow* f, double* seen)
    {
        const ShapeStyle& s = c.title.shape;
        const Timeline* tl = t_timeline ? t_timeline : ctx.root.get();
        if (s.followClip <= 0 || s.followNode <= 0 || !tl) return false;
        // Asked several times per frame (visibility, bounds, drawing): the graph is read once
        struct Cached {
            uint64_t seq = 0;
            const Timeline* tl = nullptr;
            const Clip* clip = nullptr;
            double t = 0;
            bool ok = false;
            Masks::Follow f;
            double seen = 1;
        };
        thread_local Cached cache[4];
        thread_local int next = 0;
        for (const Cached& k : cache)
            if (k.seq == t_renderSeq && k.tl == tl && k.clip == &c && k.t == t) {
                if (k.ok) {
                    *f = k.f;
                    *seen = k.seen;
                }
                return k.ok;
            }
        Cached& slot = cache[next];
        next = (next + 1) % 4;
        slot = Cached{t_renderSeq, tl, &c, t};
        TrackRef ref;
        const Clip* tracked = TimelineOps::findClip(*tl, s.followClip, &ref);
        if (!tracked) return false;
        // The tracked clip was split (the part after the cut has a new id): the clip on its track at this frame
        // with the same tracker
        const int at0 = c.start + int(std::floor(t));
        Picture::Comp::FollowAt at;
        if ((at0 < tracked->start || at0 >= tracked->end()) && ref.kind == TrackKind::Video && ref.index < tl->video.size())
            for (const Clip& o : tl->video[ref.index].clips)
                if (at0 >= o.start && at0 < o.end() && o.mediaPath == tracked->mediaPath
                    && Picture::Comp::followAt(o, s.followNode, at0 - o.start, ctx.fps, &at)) {
                    tracked = &o;
                    break;
                }
        const double tt = c.start + t - tracked->start;
        if (!Picture::Comp::followAt(*tracked, s.followNode, tt, ctx.fps, &at)) return false;
        double aspect = tracked->isTitle() || tracked->isNested() ? 0.0 : MediaAspect::ofClip(*tracked);
        if (aspect <= 0 && !tracked->isTitle() && !tracked->isNested()) aspect = nativeAspect(*tracked, QStringLiteral("follow"));
        const QSizeF fit = MediaAspect::fit(aspect, QSizeF(ctx.PW, ctx.PH));
        const bool transform = Keys::hasTransform(*tracked);
        const int tf = int(std::lround(std::clamp(tt, 0.0, double(std::max(0, tracked->length() - 1)))));
        const double zx = transform ? Keys::valueAt(*tracked, AnimParam::ZoomX, tf) : 1.0;
        const double zy = transform ? Keys::valueAt(*tracked, AnimParam::ZoomY, tf) : 1.0;
        const double px = transform ? Keys::valueAt(*tracked, AnimParam::PosX, tf) : 0.0;
        const double py = transform ? Keys::valueAt(*tracked, AnimParam::PosY, tf) : 0.0;
        const double fw = fit.width() * zx, fh = fit.height() * zy;
        const QPointF centre(ctx.PW / 2 + px, ctx.PH / 2 - py);
        auto place = [&](QPointF q) { return centre + QPointF((q.x() - 0.5) * fw, (q.y() - 0.5) * fh); };
        f->from = place(at.from);
        f->to = place(at.to);
        f->scale = at.scale;
        f->angle = at.angle;
        *seen = at.seen;
        slot.ok = true;
        slot.f = *f;
        slot.seen = *seen;
        return true;
    }

    // A shape title's outline at clip frame t in project pixels (keyframes, title motion, tracker); placed = also
    // through the clip's own transform (zoom, position, rotation: for the shapes drawn straight onto the frame)
    Shapes::Outline shapeOutline(const Clip& c, double t, const Ctx& ctx, const Generators::TitleMotion& m,
                                 bool placed, double* reveal)
    {
        Shapes::Place p = Shapes::placeAt(c, t, ctx.PW, ctx.PH);
        if (m.scale != 1) {
            p.w *= m.scale;
            p.h *= m.scale;
        }
        p.center.ry() -= m.rise * std::max(p.h, 1.0);
        *reveal = p.reveal;
        Masks::Follow f;
        double seen = 1;
        const bool follows = shapeFollow(c, t, ctx, &f, &seen);
        const double tolerance = 0.25 * ctx.PH / std::max(1, ctx.H);
        Shapes::Outline o = Shapes::placed(c.title.shape, p, follows ? &f : nullptr, tolerance);
        if (placed && Keys::hasTransform(c)) {
            const double zx = Keys::valueAt(c, AnimParam::ZoomX, t), zy = Keys::valueAt(c, AnimParam::ZoomY, t);
            const QPointF to(ctx.PW / 2 + Keys::valueAt(c, AnimParam::PosX, t), ctx.PH / 2 - Keys::valueAt(c, AnimParam::PosY, t));
            const double a = Keys::valueAt(c, AnimParam::Rotation, t) * std::numbers::pi / 180; // counterclockwise
            const double cs = std::cos(a), sn = std::sin(a);
            for (QPointF& q : o.points) {
                const QPointF d((q.x() - ctx.PW / 2) * zx, (q.y() - ctx.PH / 2) * zy);
                q = to + QPointF(d.x() * cs + d.y() * sn, -d.x() * sn + d.y() * cs);
            }
        }
        return o;
    }

    QColor shapeFill(const Clip& c, double t) const
    {
        if (c.title.shape.fill != ShapeFill::Color && c.title.shape.kind != ShapeKind::Line) return Qt::transparent;
        return Keys::animated(c, AnimParam::TitleColor) ? Keys::toColor(Keys::valueAt(c, AnimParam::TitleColor, t))
                                                        : c.title.color;
    }

    // Shape title (core/Shapes.h, 0.22): fill, stroke and shadow; the picture below (blur, pixelate, spotlight) is
    // shapeBelow's. Same arguments as drawTitle
    void drawShape(Image& layer, const Clip& c, double t, const Ctx& ctx, QPoint shift, QRect* bounds,
                   const Generators::TitleMotion& m)
    {
        double reveal = 1;
        Shapes::Outline o = shapeOutline(c, t, ctx, m, false, &reveal);
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        if (bounds) {
            const QRectF r = Shapes::bounds(c.title, o);
            *bounds = QRectF(r.x() * sx, r.y() * sy, r.width() * sx, r.height() * sy).toAlignedRect() & QRect(0, 0, ctx.W, ctx.H);
            return;
        }
        for (QPointF& q : o.points) q = QPointF(q.x() * sx, q.y() * sy);
        ShapePaint::paint(layer.data(), layer.w, layer.h, shift, o, c.title, reveal, sy, 1.0, shapeFill(c, t));
    }

    // A shape that changes the picture below it (blur or pixelate fill, spotlight) at clip frame t, straight onto the
    // frame so far with its opacity times k; then its stroke, shadow and colour fill over it
    void shapeBelow(Image& canvas, const Clip& c, int t, const Ctx& ctx, double k, bool paint = true)
    {
        Generators::TitleMotion m = Generators::titleMotion(c.title, t, c.length(), ctx.fps);
        Masks::Follow f;
        double seen = 1;
        if (shapeFollow(c, t, ctx, &f, &seen)) k *= seen;
        k *= m.opacity;
        if (k <= 0) return;
        double reveal = 1;
        Shapes::Outline o = shapeOutline(c, t, ctx, m, true, &reveal);
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        for (QPointF& q : o.points) q = QPointF(q.x() * sx, q.y() * sy);
        ShapePaint::below(canvas.data(), canvas.w, canvas.h, o, c.title.shape, sy, k);
        if (paint) ShapePaint::paint(canvas.data(), canvas.w, canvas.h, {}, o, c.title, reveal, sy, k, shapeFill(c, t));
    }

    // A shape at frame f that reads the picture below it: the card does not make that (the frame goes the CPU way)
    bool shapeReadsBelow(const Timeline& tl, int f)
    {
        const auto reads = [](const Clip* c) {
            return c && c->enabled && c->isTitle() && c->title.isShape() && c->title.shape.readsBelow();
        };
        for (const Track& track : tl.video) {
            if (track.hidden) continue;
            if (reads(clipAt(track, f))) return true;
            for (const auto& s : spansAt(track, f, tl)) // a side of a transition reaching past its clip
                if (f >= s.start && f < s.end)
                    for (const Clip& c : track.clips)
                        if ((c.id == s.leftId || c.id == s.rightId) && reads(&c)) return true;
        }
        return false;
    }

    // Progress bar title (core/Generators.h): the track (box colour, when on) along the edge and its filled part in the
    // title colour, rounded ends when set. Same arguments as drawTitle.
    void drawProgress(Image& layer, const Clip& c, double t, const Ctx& ctx, QPoint shift, QRect* bounds)
    {
        const TitleStyle& st = c.title;
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        const QRectF track = Generators::progressTrack(st, ctx.PW, ctx.PH);
        const QRectF out(track.x() * sx, track.y() * sy, track.width() * sx, track.height() * sy);
        if (bounds) {
            *bounds = out.toAlignedRect() & QRect(0, 0, ctx.W, ctx.H);
            return;
        }
        const double fraction = Generators::progress(st, c, c.start + int(std::lround(t)), ctx.begin, ctx.end);
        QImage img(layer.data(), layer.w, layer.h, layer.w * 4, QImage::Format_RGBA8888);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-shift);
        p.setPen(Qt::NoPen);
        auto bar = [&](const QRectF& r, const QColor& col) {
            if (r.width() <= 0 || col.alpha() == 0) return;
            const double radius = st.barRound ? std::min(r.height(), r.width()) / 2 : 0.0;
            p.setBrush(col);
            if (radius > 0) p.drawRoundedRect(r, radius, radius);
            else p.drawRect(r);
        };
        if (st.boxOn) bar(out, st.boxColor);
        QRectF fill = out;
        fill.setWidth(out.width() * fraction);
        bar(fill, Keys::animated(c, AnimParam::TitleColor) ? Keys::toColor(Keys::valueAt(c, AnimParam::TitleColor, t))
                                                           : st.color);
    }

    // A clip's source picture (w x h, standing for the whole fitted picture) placed by its transform onto `onto`, with
    // its opacity times `fade` (no crop, no blur fill: see clipLayer)
    void drawPlaced(Image& onto, const Clip& c, int t, const Ctx& ctx, QSizeF fit, const uint8_t* px, int w, int h,
                    double fade)
    {
        auto val = [&](AnimParam p) { return Keys::valueAt(c, p, t); };
        const bool transform = Keys::hasTransform(c);
        const double opacity = Keys::hasOpacity(c) ? std::clamp(val(AnimParam::Opacity) / 100.0, 0.0, 1.0) : 1.0;
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        const double zx = transform ? val(AnimParam::ZoomX) : 1.0, zy = transform ? val(AnimParam::ZoomY) : 1.0;
        const double x = transform ? val(AnimParam::PosX) : 0.0, y = transform ? val(AnimParam::PosY) : 0.0;
        const double fw = fit.width() * zx, fh = fit.height() * zy;
        const double rx = (ctx.PW - fw) / 2 + x, ry = (ctx.PH - fh) / 2 - y;
        const double angle = transform ? -val(AnimParam::Rotation) : 0.0;
        const Compose::Affine m = Compose::placement(rx * sx, ry * sy, fw * sx, fh * sy, angle, w, h, true, true);
        const Tilt tilt = transform ? tiltOf(c, t, (ctx.PW / 2 + x) * sx, (ctx.PH / 2 - y) * sy) : Tilt{};
        drawTilted(onto.data(), onto.w, onto.h, px, w, h, m, tilt, opacity * fade);
    }

    // One clip's picture at timeline frame f on a transparent output-size layer; empty = nothing visible.
    // onto: a media clip with a transform but no crop, effects or blur fill is drawn straight onto that picture
    // (opacity times `fade`) instead - no layer of its own, a still not copied - and nothing is returned.
    Image clipLayer(const Clip& c, int f, const QString& lane, const Ctx& ctx, const Timeline& tl, int depth,
                    Image* onto = nullptr, double fade = 1.0)
    {
        if (!c.enabled) return {};
        const int t = f - c.start;
        const int W = ctx.W, H = ctx.H;
        const double sx = W / ctx.PW, sy = H / ctx.PH;
        const bool transform = Keys::hasTransform(c), crop = Keys::hasCrop(c);
        Image layer;

        // The source picture and where it goes at zoom 1 (project pixels)
        Image src;
        QSizeF fit(ctx.PW, ctx.PH);
        // A title drawn only as large as its text: where that part sits in the full-frame picture it stands for
        QPoint part;
        bool partial = false;
        // A compound clip: its inner sequence, for the compositing graph's per-track inputs
        Timeline content, withNested;
        Nested nested;
        if (c.isTitle()) {
            double size = 0;
            if (Keys::animated(c, AnimParam::TitleSize)) size = Keys::valueAt(c, AnimParam::TitleSize, t);
            QRect box;
            // Effects (blur, glow …) work on the whole picture: there it stays full size
            if ((transform || crop) && c.effects.isEmpty() && !ColorGrade::active(c)) {
                drawTitle(src, c, t, ctx, size, {}, &box);
                // Even offset: the mip steps of Compose::draw average the same 2x2 blocks as on the full picture
                box.setLeft(box.left() & ~1);
                box.setTop(box.top() & ~1);
                if (box.isEmpty()) return {};
                partial = true;
                part = box.topLeft();
                src = Image(box.width(), box.height());
                drawTitle(src, c, t, ctx, size, part);
            } else {
                src = Image(W, H);
                drawTitle(src, c, t, ctx, size);
            }
        } else if (c.isNested()) {
            if (!tl.nested || depth > 16) return {};
            const auto it = tl.nested->constFind(c.sequenceId);
            if (it == tl.nested->cend()) return {};
            const int in = c.in + t;
            if (in < 0) return {};
            const Timeline* inner = &*it;
            if (c.isMulticam()) {
                const int angle = std::max(0, c.angle);
                if (angle >= it->angles.size()) return {};
                content = Multicam::angleTimeline(*it, it->angles[angle], TrackKind::Video);
                content.nested = it->nested ? it->nested : tl.nested;
                inner = &content;
            }
            if (!inner->nested && tl.nested) {
                withNested = *inner;
                withNested.nested = tl.nested;
                inner = &withNested;
            }
            // A sequence of another resolution (a 16:9 edit in a 9:16 short): rendered in its own pixels and fitted
            // into the frame like a clip of that shape, at the output's scale
            const QSize own = it->frameSize;
            if (!own.isEmpty() && (own.width() != ctx.PW || own.height() != ctx.PH)) {
                fit = MediaAspect::fit(double(own.width()) / own.height(), QSizeF(ctx.PW, ctx.PH));
                Ctx sub = ctx;
                sub.begin = 0;
                sub.end = TimelineOps::endFrame(*inner);
                sub.PW = own.width();
                sub.PH = own.height();
                sub.W = std::max(1, int(std::lround(fit.width() * sx)));
                sub.H = std::max(1, int(std::lround(fit.height() * sy)));
                src = Image(sub.W, sub.H);
                nested.sub = sub;
            } else {
                src = Image(W, H);
                nested.sub = ctx;
                nested.sub.begin = 0;
                nested.sub.end = TimelineOps::endFrame(*inner);
            }
            nested.inner = inner;
            nested.in = in;
            nested.prefix = lane + QString("n%1/").arg(c.sequenceId);
            nested.depth = depth;
            // Only a compositing graph on it: the graph asks for the whole inner picture if it needs it (a graph of
            // its tracks alone never renders it)
            nested.deferred = onlyComp(c);
            if (!nested.deferred) renderNested(src, nested);
        } else {
            if (c.mediaPath.isEmpty()) return {};
            const double aspect = nativeAspect(c, lane);
            fit = MediaAspect::fit(aspect, QSizeF(ctx.PW, ctx.PH));
            QSize native;
            if (!transform && !crop) {
                // Fitted into the frame at its own shape, centred (letterbox bars stay transparent)
                const int fw = std::max(1, int(std::lround(fit.width() * sx))), fh = std::max(1, int(std::lround(fit.height() * sy)));
                src = mediaPicture(c, t, lane, fw, fh, ctx, &native);
            } else {
                // At its own size (capped to what the output needs at the clip's largest zoom), placed when drawn
                Source s = source(c.mediaPath, lane);
                int nw = 0, nh = 0;
                if (s.still) {
                    nw = s.still->image.width();
                    nh = s.still->image.height();
                } else if (s.video) {
                    const bool sideways = std::lround(std::abs(s.video->info.rotation)) % 180 == 90;
                    nw = sideways ? s.video->info.height : s.video->info.width;
                    nh = sideways ? s.video->info.width : s.video->info.height;
                }
                if (nw <= 0 || nh <= 0) return {};
                // Largest zoom of the clip (keyframes included), with headroom for Bezier overshoot: a picture shown
                // small is converted small (one scaling pass instead of a full-size conversion)
                double zoom = 0;
                for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY}) {
                    zoom = std::max(zoom, std::abs(Keys::staticValue(c, p)));
                    for (const Keyframe& k : c.keys.value(p)) zoom = std::max(zoom, std::abs(k.value));
                }
                if (!transform || zoom <= 0) zoom = 1;
                double scale = std::max(double(W) / nw, double(H) / nh) * zoom * 1.1;
                // Tilted in depth: the near side shows larger than the zoom says; kept at its own size
                if (transform && tiltedEver(c)) scale = 1.0;
                const int dw = scale < 1.0 ? std::max(2, int(std::ceil(nw * scale))) : nw;
                const int dh = scale < 1.0 ? std::max(2, int(std::ceil(nh * scale))) : nh;
                if (onto && s.still && dw == nw && dh == nh && !crop && !c.transform.hasBlurFill()) {
                    // Straight from the kept still (no copy)
                    const QImage& img = s.still->image;
                    layersDecoded.fetch_add(1, std::memory_order_relaxed);
                    drawPlaced(*onto, c, t, ctx, fit, img.constBits(), img.width(), img.height(), fade);
                    return {};
                }
                src = mediaPicture(c, t, lane, dw, dh, ctx, &native);
            }
            if (src.empty()) return {};
            layersDecoded.fetch_add(1, std::memory_order_relaxed);
        }
        if (src.empty()) return {};
        {
            // the frame's height in the picture's pixels: the picture is fit.height() project pixels tall at zoom 1
            const double frameHeight = src.h * ctx.PH / std::max(1.0, fit.height());
            applyEffects(src, c, t, ctx, frameHeight, fit, lane, nested.inner ? &nested : nullptr);
        }

        auto val = [&](AnimParam p) { return Keys::valueAt(c, p, t); };
        const double opacity = Keys::hasOpacity(c) ? std::clamp(val(AnimParam::Opacity) / 100.0, 0.0, 1.0) : 1.0;
        if (t_compCapture && t_compCapture->clip == &c && t_compCapture->plan) {
            // The graph's picture is made on the card: only where it goes (as below; no blur fill, keep frame, part)
            CompCapture& cap = *t_compCapture;
            cap.opacity = opacity;
            if (!transform && !crop) {
                cap.m = {};
                cap.m.c = (W - src.w) / 2;
                cap.m.f = (H - src.h) / 2;
                return {};
            }
            if (crop)
                cap.crop = QRectF(QPointF(val(AnimParam::CropLeft) / fit.width() * src.w, val(AnimParam::CropTop) / fit.height() * src.h),
                                  QPointF((1 - val(AnimParam::CropRight) / fit.width()) * src.w,
                                          (1 - val(AnimParam::CropBottom) / fit.height()) * src.h));
            const double zx = transform ? val(AnimParam::ZoomX) : 1.0, zy = transform ? val(AnimParam::ZoomY) : 1.0;
            const double x = transform ? val(AnimParam::PosX) : 0.0, y = transform ? val(AnimParam::PosY) : 0.0;
            const double w = fit.width() * zx, h = fit.height() * zy;
            cap.m = Compose::placement(((ctx.PW - w) / 2 + x) * sx, ((ctx.PH - h) / 2 - y) * sy, w * sx, h * sy,
                                       transform ? -val(AnimParam::Rotation) : 0.0, src.w, src.h, true, true);
            if (crop && (cap.crop.width() <= 0 || cap.crop.height() <= 0)) cap.opacity = 0;
            return {};
        }
        if (!transform && !crop) {
            if (src.w == W && src.h == H) {
                layer = std::move(src);
                scaleAlpha(layer, opacity);
            } else {
                layer = Image(W, H);
                Compose::Affine m;
                m.c = (W - src.w) / 2; // whole pixels, as MLT's resize filter pads
                m.f = (H - src.h) / 2;
                Compose::draw(layer.data(), W, H, src.data(), src.w, src.h, m, opacity);
            }
            return layer;
        }
        // Blurred fill from the uncropped picture
        std::unique_ptr<BlurFill::Background> bg;
        const bool blurFill = c.transform.hasBlurFill() && !c.isTitle() && !c.isNested();
        double shares[4] = {0, 0, 0, 0};
        if (crop)
            shares[0] = val(AnimParam::CropLeft) / fit.width(), shares[1] = val(AnimParam::CropTop) / fit.height(),
            shares[2] = val(AnimParam::CropRight) / fit.width(), shares[3] = val(AnimParam::CropBottom) / fit.height();
        if (blurFill)
            bg = std::make_unique<BlurFill::Background>(BlurFill::makeBackground(
                src.data(), src.w, src.h, double(src.w) / src.h, shares, ctx.PW / ctx.PH, c.transform.fillBlur,
                c.transform.fillDim));
        // The full-frame picture the source stands for (a title drawn at text size is a part of it)
        const int fullW = partial ? W : src.w, fullH = partial ? H : src.h;
        if (crop) {
            const uint8_t none[4] = {0, 0, 0, 0};
            Compose::crop(src.data(), src.w, src.h, shares[0] * fullW - part.x(), shares[1] * fullH - part.y(),
                          (1 - shares[2]) * fullW - part.x(), (1 - shares[3]) * fullH - part.y(), none);
        }
        const double zx = transform ? val(AnimParam::ZoomX) : 1.0, zy = transform ? val(AnimParam::ZoomY) : 1.0;
        const double x = transform ? val(AnimParam::PosX) : 0.0, y = transform ? val(AnimParam::PosY) : 0.0;
        const double w = fit.width() * zx, h = fit.height() * zy;
        const double rx = (ctx.PW - w) / 2 + x, ry = (ctx.PH - h) / 2 - y;
        const double angle = transform ? -val(AnimParam::Rotation) : 0.0;
        if (onto && !crop && !bg && !partial) {
            drawPlaced(*onto, c, t, ctx, fit, src.data(), src.w, src.h, fade);
            return {};
        }
        layer = Image(W, H);
        Compose::Affine m = Compose::placement(rx * sx, ry * sy, w * sx, h * sy, angle, fullW, fullH, true, true);
        if (partial) { // the part's pixel (0, 0) is the full picture's part.x(), part.y()
            m.c += m.a * part.x() + m.b * part.y();
            m.f += m.d * part.x() + m.e * part.y();
        }
        KeepFrame kf;
        const bool framed = !c.keepRegion.isEmpty() && crop && !bg
                            && keepFrame(m, QRectF(QPointF(shares[0] * fullW, shares[1] * fullH),
                                                   QPointF((1 - shares[2]) * fullW, (1 - shares[3]) * fullH)),
                                         kKeepFrame * std::min(W, H), QSize(W, H), kf);
        // Tilted in depth: the frame goes through the same camera (one plane, so it stays around the picture)
        const Tilt tilt = transform ? tiltOf(c, t, (ctx.PW / 2 + x) * sx, (ctx.PH / 2 - y) * sy) : Tilt{};
        if (framed) drawTilted(layer.data(), W, H, kf.img.data(), kf.img.w, kf.img.h, kf.m, tilt, 1.0);
        drawTilted(layer.data(), W, H, src.data(), src.w, src.h, m, tilt, blurFill || framed ? 1.0 : opacity);
        if (framed) scaleAlpha(layer, opacity);
        if (bg) {
            BlurFill::composite(layer.data(), W, H, *bg);
            scaleAlpha(layer, opacity);
        }
        return layer;
    }

    // Picture of a track at frame f (transitions included); empty = nothing on it
    Image trackLayer(const Track& track, int trackIndex, int f, const Ctx& ctx, const Timeline& tl, const QString& prefix,
                     int depth)
    {
        if (track.hidden) return {};
        // Quick out: nothing on this track near f (spans only reach into clips' handles next to a cut)
        bool near = false;
        for (const Clip& c : track.clips)
            if (f >= c.start - 1000 && f < c.end() + 1000) { // handles are short; a generous window
                near = true;
                break;
            }
        if (!near) return {};
        const QVector<TimelineOps::TransitionSpan> spans = spansAt(track, f, tl);
        auto byId = [&](int id) -> const Clip* {
            for (const Clip& c : track.clips)
                if (c.id == id) return &c;
            return nullptr;
        };
        const QString lane = prefix + QString("v%1").arg(trackIndex);
        // Each side on its clip's lane; the right side of a dissolve that would share the left side's decoder on a
        // second one
        auto laneOf = [&](const Clip& c, const Clip* left) {
            const QString l = clipLane(track, c, lane);
            return left && left->mediaPath == c.mediaPath && clipLane(track, *left, lane) == l ? l + 'x' : l;
        };
        for (const auto& s : spans) {
            if (f < s.start || f >= s.end) continue;
            const Clip* a = s.leftId ? byId(s.leftId) : nullptr;
            const Clip* b = s.rightId ? byId(s.rightId) : nullptr;
            const bool sameFile = a && b && a->mediaPath == b->mediaPath;
            Image ia = a ? clipLayer(*a, f, laneOf(*a, nullptr), ctx, tl, depth) : Image();
            Image ib = b ? clipLayer(*b, f, laneOf(*b, a), ctx, tl, depth) : Image();
            for (Image* img : {&ia, &ib})
                if (!img->empty()) {
                    const Clip* c = img == &ia ? a : b;
                    scaleAlpha(*img, TimelineOps::fadeRamp(*c, f - c->start));
                }
            const int len = s.length(), i = f - s.start;
            const TransitionStyle& st = s.style;
            const bool styled = st.type != TransitionType::CrossDissolve && (!st.isLuma() || (!ia.empty() && !ib.empty()));
            if (ia.empty() && ib.empty()) return {};
            if (styled) return styledTransition(std::move(ia), std::move(ib), i, len, st, ctx);
            if (!ia.empty() && !ib.empty()) { // Cross Dissolve (MLT: luma without a gradient, progress i / len)
                mix(ia, ib, double(i) / len);
                return ia;
            }
            // One side: fade out/in through transparency
            Image& one = ia.empty() ? ib : ia;
            scaleAlpha(one, ia.empty() ? double(i) / len : double(len - 1 - i) / len);
            return std::move(one);
        }
        return {};
    }

    // Transition spans of a track (cached per render call would be nicer; cheap enough for now)
    QVector<TimelineOps::TransitionSpan> spansOf(const Track& track, const Timeline& tl)
    {
        bool any = false;
        for (const Clip& c : track.clips) any = any || c.transIn > 0 || c.transOut > 0;
        if (!any) return {};
        return TimelineOps::transitions(track, [&](const Clip& c) { return sourceLength(c, tl); });
    }

    // The spans of spansOf() that cover frame f: source lengths only of the clips around f (each can open a file)
    QVector<TimelineOps::TransitionSpan> spansAt(const Track& track, int f, const Timeline& tl)
    {
        bool any = false;
        for (const Clip& c : track.clips) any = any || c.transIn > 0 || c.transOut > 0;
        if (!any) return {};
        return TimelineOps::transitionsAt(track, f, [&](const Clip& c) { return sourceLength(c, tl); });
    }

    static const Clip* clipAt(const Track& track, int f)
    {
        // clips are sorted by start
        auto it = std::upper_bound(track.clips.begin(), track.clips.end(), f,
                                   [](int frame, const Clip& c) { return frame < c.start; });
        if (it == track.clips.begin()) return nullptr;
        --it;
        return f < it->end() ? &*it : nullptr;
    }

    // Decoder lane of a clip. Preview: a clip that jumps to another part of a file used just before it on the track
    // gets the lane's other decoder (suffix 'y'), so Renderer::prepare decodes its first picture while the clip
    // before it still plays (one shared decoder had to seek at the cut: the picture stood ~40 ms at 60 fps). A clip
    // that continues the one before it (a split without a jump) keeps its decoder.
    QString clipLane(const Track& track, const Clip& c, const QString& lane) const
    {
        if (options.mode != Mode::Preview || c.mediaPath.isEmpty()) return lane;
        bool second = false;
        const Clip* prev = nullptr;
        for (const Clip& o : track.clips) { // sorted by start
            if (o.mediaPath != c.mediaPath) continue;
            if (prev) {
                const bool continues = o.start == prev->end() && o.in == prev->out + 1 && o.speed == prev->speed
                                       && o.reverse == prev->reverse && !o.freeze && !prev->freeze;
                if (!continues) second = !second;
            }
            if (o.id == c.id) return second ? lane + 'y' : lane;
            prev = &o;
        }
        return lane;
    }

    // Decoder lane of the clip on video track `trackIndex`: a clip showing the same part of the same file at the same
    // time as one on a track below (the kill feed kept above a reframed game clip, a facecam cut out of the gameplay)
    // reads that clip's decoder - the picture is decoded once instead of twice (a 9:16 short with its kill feed kept
    // took ~1.5x the export time)
    QString trackClipLane(const Timeline& tl, int trackIndex, const Clip& c, const QString& prefix) const
    {
        const auto sameSource = [&](const Clip& o) {
            return o.mediaPath == c.mediaPath && o.start == c.start && o.in == c.in && o.out == c.out
                   && o.speed == c.speed && o.reverse == c.reverse && o.freeze == c.freeze
                   && o.stillFrame() == c.stillFrame() && o.ramp == c.ramp && o.kind == c.kind;
        };
        if (!c.mediaPath.isEmpty() && !c.isNested())
            for (int j = 0; j < trackIndex && j < tl.video.size(); ++j)
                for (const Clip& o : tl.video[j].clips)
                    if (o.start <= c.start && sameSource(o))
                        return clipLane(tl.video[j], o, prefix + QString("v%1").arg(j));
        return clipLane(tl.video[trackIndex], c, prefix + QString("v%1").arg(trackIndex));
    }

    // The track's picture at f surely covers the whole frame opaquely (then nothing below is decoded). Conservative:
    // false when unsure.
    bool covers(const Track& track, int trackIndex, int f, const Ctx& ctx, const Timeline& tl, const QString& prefix)
    {
        if (track.hidden) return false;
        for (const auto& s : spansAt(track, f, tl))
            if (f >= s.start && f < s.end) return false;
        const Clip* c = clipAt(track, f);
        if (!c || !c->enabled || c->isTitle() || c->isNested() || c->mediaPath.isEmpty()) return false;
        if (Keys::hasCrop(*c) || Keys::hasOpacity(*c)) return false;
        // A fixed zoom/position without rotation still covers when the enlarged picture reaches past every edge
        const bool transformed = Keys::hasTransform(*c);
        if (transformed) {
            const ClipTransform& t = c->transform;
            if (t.rotation != 0 || t.hasBlurFill() || tiltedEver(*c)) return false;
            for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY, AnimParam::Rotation})
                if (Keys::animated(*c, p)) return false;
        }
        if (TimelineOps::fadeRamp(*c, f - c->start) < 1.0) return false;
        for (const EffectInstance& e : c->effects)
            if (e.enabled && e.effectId != QLatin1String(ColorGrade::EffectId)) return false;
        const QString lane = trackClipLane(tl, trackIndex, *c, prefix);
        const Source s = source(c->mediaPath, lane);
        if (s.still) {
            if (!s.still->opaque) return false;
        } else if (!s.video || s.video->hasAlpha) {
            return false;
        }
        const QSizeF fit = MediaAspect::fit(nativeAspect(*c, lane), QSizeF(ctx.PW, ctx.PH));
        if (!transformed)
            return std::lround(fit.width() * ctx.W / ctx.PW) >= ctx.W && std::lround(fit.height() * ctx.H / ctx.PH) >= ctx.H;
        // Rectangle in project pixels (centered, Y of the position upward), then in output pixels with a pixel to spare
        const ClipTransform& t = c->transform;
        const double w = fit.width() * t.zoomX, h = fit.height() * t.zoomY;
        const double cx = ctx.PW / 2.0 + t.posX, cy = ctx.PH / 2.0 - t.posY;
        const double sx = double(ctx.W) / ctx.PW, sy = double(ctx.H) / ctx.PH;
        return (cx - w / 2) * sx <= -1 && (cx + w / 2) * sx >= ctx.W + 1 && (cy - h / 2) * sy <= -1
               && (cy + h / 2) * sy >= ctx.H + 1;
    }

    // Export: frame f shows just one plain opaque video at output size, decoded in the encoder's layout, range and
    // matrix. Then its planes go to the encoder as they are (bit-exact, no YUV -> RGBA -> YUV round trip, like a
    // plain FFmpeg transcode). false = render it normally.
    bool passThrough(int f, const Ctx& ctx, const OutputColor::Target& t)
    {
        if (options.mode != Mode::Export || options.transparent || t.bits != 8
            || (t.layout != OutputColor::Layout::Yuv420 && t.layout != OutputColor::Layout::Nv12))
            return false;
        const Timeline& tl = *ctx.root;
        if (options.subtitles)
            for (const SubtitleTrack& st : tl.subtitles)
                if (st.enabled)
                    for (const SubtitleCue& cue : st.cues)
                        if (f >= cue.start && f < cue.end && !cue.text.trimmed().isEmpty()) return false;
        // The top track with something at f must cover the frame (nothing below shows)
        int index = -1;
        for (int i = tl.video.size() - 1; i >= 0 && index < 0; --i) {
            const Track& track = tl.video[i];
            if (track.hidden) continue;
            for (const auto& span : spansAt(track, f, tl))
                if (f >= span.start && f < span.end) return false;
            if (const Clip* c = clipAt(track, f); c && c->enabled) index = i;
        }
        if (index < 0 || !covers(tl.video[index], index, f, ctx, tl, QString())) return false;
        const Clip& c = *clipAt(tl.video[index], f);
        if (Keys::hasTransform(c) || !c.effects.isEmpty()) return false;
        const QString lane = trackClipLane(tl, index, c, QString()); // a kept copy reads the lower clip's decoder
        const QSizeF fit = MediaAspect::fit(nativeAspect(c, lane), QSizeF(ctx.PW, ctx.PH));
        if (std::lround(fit.width() * ctx.W / ctx.PW) != ctx.W || std::lround(fit.height() * ctx.H / ctx.PH) != ctx.H)
            return false;
        const Source s = source(c.mediaPath, lane);
        if (!s.video) return false;
        const Video::Info& info = s.video->info;
        auto family = [](int cs) { return cs == 170 || cs == 470 || cs == 624 ? 601 : cs == 2021 ? 2020 : cs; };
        if (family(info.colorspace) != family(t.colorspace)) return false;
        int64_t i0 = 0;
        double frac = 0;
        if (betweenFrames(c, f - c.start, *s.video, ctx, &i0, &frac)) return false; // an in-between picture
        const double file = fileFrame(c, f - c.start, *s.video, ctx);
        if (file < 0) return false;
        const auto pic = fetchPicture(*s.video, file, ctx);
        if (!pic) return false;
        const AVFrame* fr = pic->frame;
        const auto format = AVPixelFormat(fr->format);
        const bool full = format == AV_PIX_FMT_YUVJ420P || info.fullRange;
        if (fr->width != ctx.W || fr->height != ctx.H || full != t.fullRange) return false;
        const int cw = (ctx.W + 1) / 2, ch = (ctx.H + 1) / 2;
        if (t.layout == OutputColor::Layout::Yuv420 && (format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P)) {
            av_image_copy_plane(t.planes[0], t.strides[0], fr->data[0], fr->linesize[0], ctx.W, ctx.H);
            av_image_copy_plane(t.planes[1], t.strides[1], fr->data[1], fr->linesize[1], cw, ch);
            av_image_copy_plane(t.planes[2], t.strides[2], fr->data[2], fr->linesize[2], cw, ch);
        } else if (t.layout == OutputColor::Layout::Nv12 && format == AV_PIX_FMT_NV12) {
            av_image_copy_plane(t.planes[0], t.strides[0], fr->data[0], fr->linesize[0], ctx.W, ctx.H);
            av_image_copy_plane(t.planes[1], t.strides[1], fr->data[1], fr->linesize[1], 2 * cw, ch);
        } else {
            return false;
        }
        layersDecoded.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Draws the track's picture at f over canvas. bottom: the canvas holds nothing that shows (this track covers it)
    void drawTrack(Image& canvas, bool bottom, const Track& track, int trackIndex, int f, const Ctx& ctx,
                   const Timeline& tl, const QString& prefix, int depth)
    {
        if (track.hidden) return;
        for (const auto& s : spansAt(track, f, tl))
            if (f >= s.start && f < s.end) {
                // A shape that hides what lies below (blur, pixelate, spotlight) does so at full strength through
                // its transitions: a face must never show sharp while the blur fades in or dissolves into its other
                // half. Only its stroke, shadow and colour take part in the transition (trackLayer)
                for (const int id : {s.leftId, s.rightId})
                    for (const Clip& sc : track.clips)
                        if (id && sc.id == id && sc.enabled && sc.isTitle() && sc.title.isShape() && sc.title.shape.readsBelow()) {
                            const int st = f - sc.start;
                            const double op = Keys::hasOpacity(sc) ? std::clamp(Keys::valueAt(sc, AnimParam::Opacity, st) / 100.0, 0.0, 1.0) : 1.0;
                            shapeBelow(canvas, sc, st, ctx, op, false);
                        }
                Image layer = trackLayer(track, trackIndex, f, ctx, tl, prefix, depth);
                if (!layer.empty()) Compose::over(canvas.data(), layer.data(), canvas.w, canvas.h, 1.0);
                return;
            }
        const Clip* c = clipAt(track, f);
        if (!c || !c->enabled) return;
        const QString lane = trackClipLane(tl, trackIndex, *c, prefix);
        const int t = f - c->start;
        const double fade = TimelineOps::fadeRamp(*c, t);
        if (c->isTitle() && c->title.isShape() && c->title.shape.readsBelow()) { // works on the frame below it
            const double opacity = Keys::hasOpacity(*c) ? std::clamp(Keys::valueAt(*c, AnimParam::Opacity, t) / 100.0, 0.0, 1.0) : 1.0;
            shapeBelow(canvas, *c, t, ctx, opacity * fade);
            return;
        }
        // Render cache (preview, outer timeline): the clip's finished output (frame 0 = clip start) instead of the
        // original with its effects; not while Before/After shows a graded clip without its grade
        if (ctx.preview && options.renderCache && depth == 0 && !c->isTitle() && !c->mediaPath.isEmpty()
            && !(ctx.bypassGrade && ColorGrade::active(*c))) {
            if (const QString file = options.renderCache(*c); !file.isEmpty()) {
                const Source s = source(file, lane + QStringLiteral("c"));
                if (s.video) {
                    QSize native;
                    const Image img = videoFrame(*s.video, t, canvas.w, canvas.h, ctx, &native);
                    if (!img.empty()) {
                        layersDecoded.fetch_add(1, std::memory_order_relaxed);
                        Compose::over(canvas.data(), img.data(), canvas.w, canvas.h, fade);
                        return;
                    }
                }
            }
        }
        const bool plain = !Keys::hasTransform(*c) && !Keys::hasCrop(*c);
        const double opacity = Keys::hasOpacity(*c) ? std::clamp(Keys::valueAt(*c, AnimParam::Opacity, t) / 100.0, 0.0, 1.0) : 1.0;
        if (plain && !c->isTitle() && !c->isNested() && !c->mediaPath.isEmpty() && !ColorGrade::active(*c)
            && c->effects.isEmpty()) {
            // Media without transform or effects: decoded at its fitted size and laid on directly
            const QSizeF fit = MediaAspect::fit(nativeAspect(*c, lane), QSizeF(ctx.PW, ctx.PH));
            const int fw = std::max(1, int(std::lround(fit.width() * ctx.W / ctx.PW)));
            const int fh = std::max(1, int(std::lround(fit.height() * ctx.H / ctx.PH)));
            QSize native;
            const double k = opacity * fade;
            if (bottom && fw == canvas.w && fh == canvas.h && k >= 1.0) {
                if (!mediaPicture(*c, t, lane, fw, fh, ctx, &native, canvas.data()).empty())
                    layersDecoded.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const Image src = mediaPicture(*c, t, lane, fw, fh, ctx, &native);
            if (src.empty()) return;
            layersDecoded.fetch_add(1, std::memory_order_relaxed);
            if (fw == canvas.w && fh == canvas.h) {
                Compose::over(canvas.data(), src.data(), canvas.w, canvas.h, k);
            } else {
                Compose::Affine m;
                m.c = (canvas.w - fw) / 2;
                m.f = (canvas.h - fh) / 2;
                Compose::draw(canvas.data(), canvas.w, canvas.h, src.data(), fw, fh, m, k);
            }
            return;
        }
        if (c->isTitle() && plain && opacity * fade >= 1.0 && c->effects.isEmpty()
            && !Keys::animated(*c, AnimParam::TitleSize)) {
            drawTitle(canvas, *c, t, ctx); // painted straight on: no full-frame layer
            return;
        }
        if (Keys::hasTransform(*c) && !c->isTitle() && !c->isNested() && !c->mediaPath.isEmpty() && c->effects.isEmpty()
            && !Keys::hasCrop(*c) && !c->transform.hasBlurFill()) {
            clipLayer(*c, f, lane, ctx, tl, depth, &canvas, fade); // transformed: drawn straight onto the frame
            return;
        }
        Image layer = clipLayer(*c, f, lane, ctx, tl, depth);
        if (layer.empty()) return;
        Compose::over(canvas.data(), layer.data(), canvas.w, canvas.h, fade);
    }

    // Alpha keyframes linear between (frame, value) points, as MLT's brightness filter animates them
    static double alphaKeys(int i, std::initializer_list<std::pair<int, double>> keys)
    {
        auto prev = *keys.begin();
        for (const auto& k : keys) {
            if (i <= k.first) {
                if (k.first == prev.first) return k.second;
                return prev.second + (k.second - prev.second) * (i - prev.first) / double(k.first - prev.first);
            }
            prev = k;
        }
        return prev.second;
    }

    // Gradient of a luma transition or a wipe between two clips at w x h (8-bit gray, rows tightly packed); null =
    // none (an unknown/unreadable luma image: the transition mixes). Built once per style and size and shared by
    // every frame of the transition, CPU and GPU path.
    std::shared_ptr<const std::vector<uint8_t>> gradient(const TransitionStyle& st, int w, int h)
    {
        if (w <= 0 || h <= 0) return nullptr;
        QString key = QString("%1|%2|%3x%4").arg(int(st.type)).arg(st.invert ? 1 : 0).arg(w).arg(h);
        if (st.isLuma()) {
            key += '|' + st.luma;
            if (!EffectFolders::isBuiltin(st.luma)) { // an image file: a changed file is read again
                const QFileInfo fi(st.luma);
                key += QString("|%1|%2").arg(fi.lastModified().toMSecsSinceEpoch()).arg(fi.size());
            }
        }
        // Built under the lock: the workers rendering the transition's first frames wait for one build instead of
        // each building it
        std::lock_guard lock(gradientMutex);
        if (auto it = gradients.find(key); it != gradients.end()) return it->second;
        std::shared_ptr<std::vector<uint8_t>> grad;
        if (st.isLuma()) {
            const QImage img = Lumas::image(st.luma, w, h, st.invert);
            if (!img.isNull()) {
                const QImage g8 = img.convertToFormat(QImage::Format_Grayscale8);
                grad = std::make_shared<std::vector<uint8_t>>(size_t(w) * h);
                for (int y = 0; y < h; ++y) std::memcpy(grad->data() + size_t(y) * w, g8.constScanLine(y), size_t(w));
            }
        } else {
            // Wipe: a linear gradient (wipeLuma: lround(t * 255) per pixel)
            grad = std::make_shared<std::vector<uint8_t>>(size_t(w) * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    double v = 0;
                    switch (st.type) {
                    case TransitionType::WipeRight: v = double(x) / std::max(1, w - 1); break;
                    case TransitionType::WipeLeft: v = 1.0 - double(x) / std::max(1, w - 1); break;
                    case TransitionType::WipeDown: v = double(y) / std::max(1, h - 1); break;
                    case TransitionType::WipeUp: v = 1.0 - double(y) / std::max(1, h - 1); break;
                    default: break;
                    }
                    (*grad)[size_t(y) * w + x] = uint8_t(std::lround(v * 255));
                }
        }
        gradientsBuilt.fetch_add(1, std::memory_order_relaxed);
        if (gradients.size() >= 8) gradients.clear(); // a few transitions at one or two sizes; a resize builds anew
        gradients[key] = grad;
        return grad;
    }

    // luma with a gradient (8-bit gray, a's size): b replaces a where the gradient is below the progress, over a soft
    // band (smoothstep(value, value + softness, progress * (1 + softness)))
    static void luma(Image& a, const Image& b, double progress, double softness, const uint8_t* gradient)
    {
        const double weight = progress * (1 + softness);
        // The weight depends only on the gradient's value: one table instead of the smoothstep per pixel
        double table[256];
        for (int g = 0; g < 256; ++g) {
            const double v = g / 255.0;
            double k;
            if (weight < v) k = 0;
            else if (weight >= v + softness) k = 1;
            else {
                const double t = (weight - v) / softness;
                k = t * t * (3 - 2 * t);
            }
            table[g] = k;
        }
        uint8_t* __restrict d = a.data();
        const uint8_t* __restrict s = b.data();
        const size_t n = size_t(a.w) * a.h;
        for (size_t i = 0; i < n; ++i, d += 4, s += 4) {
            const double k = table[gradient[i]];
            if (k <= 0) continue;
            if (k >= 1) {
                std::memcpy(d, s, 4);
                continue;
            }
            for (int c = 0; c < 4; ++c) d[c] = uint8_t(std::lround(d[c] + (s[c] - d[c]) * k));
        }
    }

    // Transitions other than Cross Dissolve (TimelineBuilderDetail::styledTransition): a leaves, b enters, either may
    // be missing
    Image styledTransition(Image a, Image b, int i, int len, const TransitionStyle& st, const Ctx& ctx)
    {
        const TransitionType type = st.type;
        if (type == TransitionType::DipToColor) {
            // Colour at the bottom, a fades out over the first half, b in over the second (one side: whole length)
            Image out(ctx.W, ctx.H);
            QColor col = st.color;
            col.setAlpha(255);
            fill(out, col);
            const bool both = !a.empty() && !b.empty();
            const int half = both ? len / 2 : 0;
            if (!a.empty()) {
                scaleAlpha(a, alphaKeys(i, {{0, 1.0}, {(both ? half : len) - 1, 0.0}, {len - 1, 0.0}}));
                Compose::over(out.data(), a.data(), out.w, out.h, 1.0);
            }
            if (!b.empty()) {
                scaleAlpha(b, alphaKeys(i, {{0, 0.0}, {half, 0.0}, {len - 1, 1.0}}));
                Compose::over(out.data(), b.data(), out.w, out.h, 1.0);
            }
            return out;
        }
        if (st.isSlide()) return slide(std::move(a), std::move(b), i, len, st, ctx);
        const double soft = 0.02 + 0.98 * std::clamp(st.softness, 0.0, 100.0) / 100.0;
        const double progress = Generators::ease(st.ease, double(i) / len);
        if (st.isLuma()) { // both sides (one side is a fade, see trackLayer)
            const auto grad = gradient(st, a.w, a.h);
            if (!grad) {
                mix(a, b, progress);
                return a;
            }
            luma(a, b, progress, soft, grad->data());
            return a;
        }
        const bool horizontal = type == TransitionType::WipeRight || type == TransitionType::WipeLeft;
        if (a.empty() || b.empty()) {
            // Wipe from/to nothing: the visible part of the one side shrinks/grows (hard edge)
            const bool fromStart = (type == TransitionType::WipeRight || type == TransitionType::WipeDown) == a.empty();
            Image& one = a.empty() ? b : a;
            const double p = Generators::ease(st.ease, len > 1 ? double(i) / (len - 1) : 1.0);
            const double shown = !a.empty() ? 1.0 - p : p;
            const double full = horizontal ? ctx.W : ctx.H;
            const double size = full * shown, pos = fromStart ? 0.0 : full - size;
            const uint8_t none[4] = {0, 0, 0, 0};
            if (horizontal) Compose::crop(one.data(), one.w, one.h, pos, 0, pos + std::max(size, 0.0), one.h, none);
            else Compose::crop(one.data(), one.w, one.h, 0, pos, one.w, pos + std::max(size, 0.0), none);
            wipeBorder(one, st, p, ctx);
            return std::move(one);
        }
        // Wipe between two clips: luma with a linear gradient
        luma(a, b, progress, soft, gradient(st, a.w, a.h)->data());
        wipeBorder(a, st, progress * (1.0 + soft) - soft / 2, ctx); // middle of luma's soft zone
        return a;
    }

    // Push/whip: b pushes a out in the transition's direction (both move by the eased progress; either may be
    // missing: it slides over transparency). Whip adds motion blur along the direction, strongest in the middle.
    static Image slide(Image a, Image b, int i, int len, const TransitionStyle& st, const Ctx& ctx)
    {
        const int dir = st.direction();
        const bool horizontal = dir < 2;
        const int sign = dir == 0 || dir == 2 ? 1 : -1;
        const int full = horizontal ? ctx.W : ctx.H;
        const double x = double(i) / std::max(1, len);
        const int shift = int(std::lround(Generators::ease(st.ease, x) * full));
        Image out(ctx.W, ctx.H);
        std::fill(out.px.begin(), out.px.end(), uint8_t(0));
        // Copies `src` moved by `offset` pixels along the direction (what leaves the frame is dropped)
        auto put = [&](const Image& src, int offset) {
            if (src.empty() || std::abs(offset) >= full) return;
            const int W = ctx.W, H = ctx.H;
            if (horizontal) {
                const int x0 = std::max(0, offset), x1 = std::min(W, W + offset);
                for (int y = 0; y < H; ++y)
                    std::memcpy(out.data() + (size_t(y) * W + x0) * 4, src.data() + (size_t(y) * W + x0 - offset) * 4,
                                size_t(x1 - x0) * 4);
            } else {
                const int y0 = std::max(0, offset), y1 = std::min(H, H + offset);
                std::memcpy(out.data() + size_t(y0) * W * 4, src.data() + size_t(y0 - offset) * W * 4,
                            size_t(y1 - y0) * W * 4);
            }
        };
        put(a, sign * shift);
        put(b, sign * (shift - full));
        if (st.isWhip() && st.blur > 0) {
            // Box blur as long as up to a quarter of the frame, following the speed (sin^2 bell over the transition)
            const double bell = std::pow(std::sin(std::numbers::pi * x), 2);
            const int radius = int(std::lround(std::clamp(st.blur, 0.0, 100.0) / 100.0 * 0.125 * full * bell));
            if (radius > 0) motionBlur(out, horizontal, radius);
        }
        return out;
    }

    // Box blur of a straight-alpha picture along rows (horizontal) or columns, `radius` pixels each side; the edge
    // pixels repeat. Blurred premultiplied, so transparent parts do not bleed their colour. A sliding window sum (cost
    // independent of the radius, no division per channel); columns go in blocks of neighbours that lie side by side
    // in memory (cache). About 10 ms for a 1080p frame on a desktop CPU, 60 ms in 4K (a prefix sum per line with
    // integer divisions took twice that; working on a downscaled line was slower still).
    static void motionBlur(Image& img, bool horizontal, int radius)
    {
        const int n = horizontal ? img.w : img.h, lines = horizontal ? img.h : img.w;
        if (n <= 0 || lines <= 0 || radius <= 0) return;
        const size_t step = horizontal ? 4 : size_t(img.w) * 4; // between pixels along a line
        const size_t lineStep = horizontal ? size_t(img.w) * 4 : 4;
        const int block = horizontal ? 1 : 16;
        const float invSpan = 1.0f / float(2 * radius + 1);
        // Sums stay below 2^31: (2 radius + 1) * 255 * 255 for a radius up to 16000
        std::vector<int32_t> pre(size_t(n) * block * 4); // premultiplied lines of the block, pixel-major
        std::vector<int32_t> acc(static_cast<size_t>(block) * 4);
        for (int l0 = 0; l0 < lines; l0 += block) {
            const int m = std::min(block, lines - l0);
            uint8_t* base = img.data() + size_t(l0) * lineStep;
            for (int k = 0; k < n; ++k)
                for (int j = 0; j < m; ++j) {
                    const uint8_t* q = base + size_t(k) * step + size_t(j) * lineStep;
                    int32_t* d = &pre[(size_t(k) * block + j) * 4];
                    const int al = q[3];
                    d[0] = q[0] * al;
                    d[1] = q[1] * al;
                    d[2] = q[2] * al;
                    d[3] = al;
                }
            auto at = [&](int k) { return &pre[size_t(std::clamp(k, 0, n - 1)) * block * 4]; };
            std::fill(acc.begin(), acc.end(), 0);
            for (int k = -radius; k <= radius; ++k) {
                const int32_t* p = at(k);
                for (int c = 0; c < m * 4; ++c) acc[size_t(c)] += p[c];
            }
            for (int k = 0; k < n; ++k) {
                for (int j = 0; j < m; ++j) {
                    uint8_t* q = base + size_t(k) * step + size_t(j) * lineStep;
                    const int32_t* a = &acc[size_t(j) * 4];
                    q[3] = uint8_t(float(a[3]) * invSpan + 0.5f);
                    if (a[3] > 0) {
                        const float inv = 1.0f / float(a[3]);
                        for (int c = 0; c < 3; ++c) q[c] = uint8_t(std::min(255.0f, float(a[c]) * inv + 0.5f));
                    } else {
                        q[0] = q[1] = q[2] = 0;
                    }
                }
                const int32_t* add = at(k + radius + 1);
                const int32_t* sub = at(k - radius);
                for (int c = 0; c < m * 4; ++c) acc[size_t(c)] += add[c] - sub[c];
            }
        }
    }

    // Border of a wipe: a stripe in the border colour around the edge (edge = 0..1 in wipe direction)
    static void wipeBorder(Image& img, const TransitionStyle& st, double edge, const Ctx& ctx)
    {
        if (st.border < 0.5) return;
        const bool horizontal = st.type == TransitionType::WipeRight || st.type == TransitionType::WipeLeft;
        const bool reverse = st.type == TransitionType::WipeLeft || st.type == TransitionType::WipeUp;
        const double full = horizontal ? ctx.PW : ctx.PH;
        const double pos = (reverse ? 1.0 - edge : edge) * full - st.border / 2; // project pixels
        const double k = horizontal ? ctx.W / ctx.PW : ctx.H / ctx.PH;
        Image strip(img.w, img.h);
        QColor c = st.borderColor;
        c.setAlpha(255);
        fill(strip, c);
        const uint8_t none[4] = {0, 0, 0, 0};
        if (horizontal) Compose::crop(strip.data(), strip.w, strip.h, pos * k, 0, (pos + st.border) * k, strip.h, none);
        else Compose::crop(strip.data(), strip.w, strip.h, 0, pos * k, strip.w, (pos + st.border) * k, none);
        Compose::over(img.data(), strip.data(), img.w, img.h, 1.0);
    }

    // Word captions (core/Captions.h): karaoke = the cue's words with the spoken one highlighted, word = the spoken
    // word alone, popping in. Lines wrap at Captions::lineWidth; posY = bottom of the block above the picture's bottom.
    void captionLayer(Image& canvas, const SubtitleTrack& st, const SubtitleCue& cue, int f, const Ctx& ctx)
    {
        const CaptionStyle& cs = st.captions;
        const TitleStyle& t = st.style;
        QStringList words = Captions::words(cue.text);
        if (words.isEmpty()) return;
        if (cs.uppercase)
            for (QString& w : words) w = w.toUpper();
        const int active = Captions::activeWord(cue, f);
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        QFont font(t.fontFamily());
        font.setPixelSize(std::max(1, int(std::lround(std::max(1.0, t.size) * sy))));
        font.setWeight(t.bold ? QFont::Bold : QFont::Normal);
        font.setItalic(t.italic);
        const double lineWidth = Captions::lineWidth(QSize(int(ctx.PW), int(ctx.PH)));
        Text::WordParams p;
        if (cs.mode == CaptionMode::Word) {
            if (active < 0) return;
            const QVector<CueWord> times = Captions::timing(cue);
            const Captions::Pop pop = Captions::popIn((f - cue.start - times[active].from) * 1000.0 / ctx.fps, cs.popMs);
            p.lines = {{Text::Word{words[active], t.color, pop.scale, pop.opacity}}};
        } else {
            const QFontMetricsF fm(font);
            QVector<double> widths;
            p.reserve = std::max(1.0, cs.highlightScale);
            double widest = 0;
            for (const QString& w : words) widths << fm.horizontalAdvance(w), widest = std::max(widest, widths.last());
            // Room on every line for the widest word grown (Text::drawWords): the lines do not change as it moves
            const QVector<int> starts = Captions::lineStarts(widths, fm.horizontalAdvance(QLatin1Char(' ')),
                                                             lineWidth * sx - widest * (p.reserve - 1), cs.maxWordsPerLine);
            for (int i = 0; i < words.size(); ++i) {
                if (starts.contains(i)) p.lines.append(QVector<Text::Word>());
                const bool now = i == active;
                p.lines.last() << Text::Word{words[i], now ? cs.highlight : t.color, now ? cs.highlightScale : 1.0, 1.0};
            }
        }
        const double bottom = std::clamp(ctx.PH - t.posY, 1.0, ctx.PH);
        p.area = QRectF(((ctx.PW - lineWidth) / 2 + t.posX) * sx, 0, lineWidth * sx, bottom * sy);
        p.font = font;
        p.halign = t.align == 0 ? 0 : t.align == 2 ? 2 : 1;
        p.valign = 2;
        p.outlineColor = t.outlineColor;
        p.outline = (t.outlineOn ? 2 * std::max(0.0, t.outlineWidth) : 0.0) * sy;
        p.bg = t.boxOn ? t.boxColor : QColor(Qt::transparent);
        p.pad = (t.boxOn ? std::max(0.0, t.boxPad) : 0.0) * sy;
        p.shadow = cs.shadow ? QColor(0, 0, 0, 150) : QColor(Qt::transparent);
        Image layer(ctx.W, ctx.H);
        Text::drawWords(layer.data(), layer.w, layer.h, p);
        Compose::over(canvas.data(), layer.data(), canvas.w, canvas.h, 1.0);
    }

    // Every enabled subtitle track at once (e.g. two speakers), in track order
    void subtitleLayer(Image& canvas, const Timeline& tl, int f, const Ctx& ctx)
    {
        for (const SubtitleTrack& st : tl.subtitles) {
            if (!st.enabled) continue;
            for (const SubtitleCue& cue : st.cues) {
                if (f < cue.start || f >= cue.end || cue.text.trimmed().isEmpty()) continue;
                if (st.captions.mode != CaptionMode::Lines) {
                    captionLayer(canvas, st, cue, f, ctx);
                    break;
                }
                const TitleStyle& t = st.style;
                const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
                QFont font(t.fontFamily());
                font.setPixelSize(std::max(1, int(std::lround(t.size))));
                font.setBold(t.bold);
                font.setItalic(t.italic);
                const QFontMetricsF fm(font);
                const QStringList lines = wrapSubtitle(cue.text.trimmed(), fm, ctx.PW * 0.9);
                double w = 1;
                for (const QString& line : lines) w = std::max(w, fm.horizontalAdvance(line));
                const double bottom = std::clamp(ctx.PH - t.posY, 1.0, ctx.PH);
                const bool outline = t.outlineOn && t.outlineWidth > 0;
                Image layer(ctx.W, ctx.H);
                auto draw = [&](bool outlineLayer) {
                    Text::Params p;
                    p.text = lines.join('\n');
                    p.area = QRectF(((ctx.PW - w) / 2 + t.posX) * sx, 0, w * sx, bottom * sy);
                    QFont f2(t.fontFamily());
                    f2.setPixelSize(std::max(1, int(std::lround(std::max(1.0, t.size) * sy))));
                    f2.setWeight(t.bold ? QFont::Bold : QFont::Normal);
                    f2.setItalic(t.italic);
                    p.font = f2;
                    p.halign = t.align == 0 ? 0 : t.align == 2 ? 2 : 1;
                    p.valign = 2;
                    p.fg = outlineLayer ? t.outlineColor : t.color;
                    const bool box = t.boxOn && (outlineLayer || !outline);
                    p.bg = box ? t.boxColor : QColor(Qt::transparent);
                    p.pad = (t.boxOn ? std::max(0.0, t.boxPad) : 0.0) * sy;
                    p.outlineColor = t.outlineColor;
                    p.outline = (outlineLayer ? 2 * t.outlineWidth : 0.0) * sy;
                    p.room = (outline ? 2 * t.outlineWidth : 0.0) * sy;
                    Text::draw(layer.data(), layer.w, layer.h, p);
                };
                if (outline) draw(true);
                draw(false);
                Compose::over(canvas.data(), layer.data(), canvas.w, canvas.h, 1.0);
                break; // one cue per track
            }
        }
    }

    // ---- GPU path (see the header): the frame as a list of draws, worked out here, composited on the GPU thread ----

    struct GpuOp {
        enum Kind {
            Draw,  // a picture over the target
            Group, // `a` onto a new target cleared to `fill`, that over the target with `opacity`
            Mix,   // `a` and `b` each onto a transparent target, cross dissolved (t), the result over the target
            Luma,  // as Mix, wiped by `gradient` (progress t, softness)
        };
        Kind kind = Draw;
        // Draw: one of these
        std::shared_ptr<const Video::Picture> picture; // decoded (its planes are uploaded)
        Video::Info info;
        std::shared_ptr<StillSource> still;
        std::shared_ptr<Image> rgba; // made on the CPU (straight alpha, or premultiplied)
        bool premultiplied = false;
        Gpu::CompPlanPtr comp; // a compositing graph evaluated on the card (gpu/CompGpu)
        bool mips = false;
        Compose::Affine m;
        double opacity = 1;
        QRectF crop;
        bool graded = false;
        ColorGrade::Params grade;
        std::shared_ptr<const ColorGrade::Lut> gradeLut;
        // Group/Mix/Luma
        std::vector<GpuOp> a, b;
        QColor fill = Qt::transparent;
        double t = 0, softness = 0;
        std::shared_ptr<const std::vector<uint8_t>> gradient; // 8-bit gray, output size (shared, Impl::gradient)
    };

    // A clip the card draws by itself at clip frame t: media (video in an uploadable format, still) with transform,
    // crop, opacity, fade and colour grade, nothing else
    bool gpuClip(const Clip& c, int t, const Ctx& ctx, const QString& lane)
    {
        if (!c.enabled || c.isTitle() || c.isNested() || c.mediaPath.isEmpty()) return false;
        for (const EffectInstance& e : c.effects)
            if (e.enabled && e.effectId != QLatin1String(ColorGrade::EffectId)) return false;
        if (ColorGrade::active(c) && !ctx.bypassGrade && !ColorGrade::windowsAt(c, t).isEmpty()) return false;
        if (StabilizeCorrection::of(c, options.format.rate, ctx.PH > 0 ? ctx.PW / ctx.PH : 0.0)) return false;
        // Tilted in depth: the card draws affine placements only; the CPU draws it through its homography
        if (tiltedEver(c)) return false;
        const Source s = source(c.mediaPath, lane);
        // Larger than the card's largest texture (a panorama still; D3D11 takes 16384 px): drawn on the CPU
        const Gpu::Service* gpuNow = gpu.load(std::memory_order_relaxed);
        const int maxTexture = gpuNow ? gpuNow->info().maxTextureSize : 0;
        auto tooLarge = [maxTexture](int w, int h) { return maxTexture > 0 && std::max(w, h) > maxTexture; };
        if (s.still) return !tooLarge(s.still->image.width(), s.still->image.height());
        if (s.video && tooLarge(s.video->info.width, s.video->info.height)) return false;
        // In-between pictures of slow motion come from the CPU (engine/picture/Interpolate)
        int64_t i0 = 0;
        double frac = 0;
        if (s.video && betweenFrames(c, t, *s.video, ctx, &i0, &frac)) return false;
        return s.video && Gpu::canUpload(s.video->info.format);
    }

    // The draw of a gpuClip at timeline frame f, its opacity times k; false = nothing to draw
    bool gpuMediaOp(const Clip& c, int f, const QString& lane, const Ctx& ctx, double k, GpuOp& op)
    {
        const int t = f - c.start;
        const Source s = source(c.mediaPath, lane);
        int nw = 0, nh = 0;
        if (s.still) {
            op.still = s.still;
            nw = s.still->image.width();
            nh = s.still->image.height();
        } else if (s.video) {
            const double file = fileFrame(c, t, *s.video, ctx);
            if (file < 0) return false;
            op.picture = fetchPicture(*s.video, file, ctx);
            if (!op.picture) return false;
            op.info = s.video->info;
            nw = op.picture->frame->width;
            nh = op.picture->frame->height;
            // Interlaced 8-bit planar 4:2:0: the chroma rows alternate between the two fields. The CPU converts it
            // field by field (Video::toRgba); the card samples the chroma plane as one picture and blended the
            // fields' colours (stripes of one field tinted by the other). Converted here, the card draws the RGBA
            // picture (from the layer pool). Only the formats the CPU converts field by field: the others (NV12, P010
            // of hardware decoding, 10 bit) the CPU converts as progressive too, the planes upload gives the same
            const auto fmt = AVPixelFormat(op.picture->frame->format);
            if (!op.picture->progressive && (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P) && nw > 0 && nh > 0) {
                auto img = std::make_shared<Image>(nw, nh);
                if (!Video::toRgba(*op.picture, op.info, img->data(), nw, nh, ctx.preview)) return false;
                op.rgba = std::move(img);
            }
        }
        if (nw <= 0 || nh <= 0) return false;
        layersDecoded.fetch_add(1, std::memory_order_relaxed);
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        const bool transform = Keys::hasTransform(c), crop = Keys::hasCrop(c);
        const QSizeF fit = MediaAspect::fit(nativeAspect(c, lane), QSizeF(ctx.PW, ctx.PH));
        auto val = [&](AnimParam p) { return Keys::valueAt(c, p, t); };
        const double opacity = Keys::hasOpacity(c) ? std::clamp(val(AnimParam::Opacity) / 100.0, 0.0, 1.0) : 1.0;
        if (!transform && !crop) {
            // Fitted into the frame at its own shape, centred on whole pixels (as the CPU path)
            const int fw = std::max(1, int(std::lround(fit.width() * sx))), fh = std::max(1, int(std::lround(fit.height() * sy)));
            op.m.a = double(fw) / nw;
            op.m.e = double(fh) / nh;
            op.m.c = (ctx.W - fw) / 2;
            op.m.f = (ctx.H - fh) / 2;
        }
        double shares[4] = {0, 0, 0, 0};
        if (transform || crop) {
            if (crop) {
                const double l = val(AnimParam::CropLeft) / fit.width(), tp = val(AnimParam::CropTop) / fit.height();
                const double r = val(AnimParam::CropRight) / fit.width(), b = val(AnimParam::CropBottom) / fit.height();
                shares[0] = l, shares[1] = tp, shares[2] = r, shares[3] = b;
                op.crop = QRectF(QPointF(l * nw, tp * nh), QPointF((1 - r) * nw, (1 - b) * nh));
                if (op.crop.width() <= 0 || op.crop.height() <= 0) return false;
            }
            const double zx = transform ? val(AnimParam::ZoomX) : 1.0, zy = transform ? val(AnimParam::ZoomY) : 1.0;
            const double x = transform ? val(AnimParam::PosX) : 0.0, y = transform ? val(AnimParam::PosY) : 0.0;
            const double w = fit.width() * zx, h = fit.height() * zy;
            const double rx = (ctx.PW - w) / 2 + x, ry = (ctx.PH - h) / 2 - y;
            const double angle = transform ? -val(AnimParam::Rotation) : 0.0;
            op.m = Compose::placement(rx * sx, ry * sy, w * sx, h * sy, angle, nw, nh, true, true);
        }
        op.opacity = opacity * k;
        // Shrunk 2:1 or more: mip levels (as Compose::draw's 2x2 steps)
        op.mips = std::hypot(op.m.a, op.m.d) <= 0.5 && std::hypot(op.m.b, op.m.e) <= 0.5;
        if (ColorGrade::active(c) && !ctx.bypassGrade) {
            const ColorGrade::Params p = ColorGrade::at(c, t);
            const QString lp = ColorGrade::lutPath(c);
            auto l = lp.isEmpty() ? nullptr : lut(lp);
            if (!p.isNeutral() || l) {
                op.graded = true;
                op.grade = p;
                op.gradeLut = std::move(l);
            }
        }
        if ((transform || crop) && c.transform.hasBlurFill()) return blurFillOp(c, ctx, shares, op);
        KeepFrame kf;
        if (!c.keepRegion.isEmpty() && crop
            && keepFrame(op.m, op.crop, kKeepFrame * std::min(ctx.W, ctx.H), QSize(ctx.W, ctx.H), kf)) {
            // As clipLayer: the frame, the picture over it, then opacity
            GpuOp back;
            back.m = kf.m;
            back.rgba = std::make_shared<Image>(std::move(kf.img));
            GpuOp group;
            group.kind = GpuOp::Group;
            group.opacity = op.opacity;
            op.opacity = 1.0;
            group.a.push_back(std::move(back));
            group.a.push_back(std::move(op));
            op = std::move(group);
        }
        return true;
    }

    // Blurred fill behind a gpuClip's draw (as clipLayer: BlurFill::makeBackground from the graded, uncropped picture,
    // the picture over it, then opacity): the small background is made here from a shrunk copy of the picture (the
    // fill is blurred anyway; the full picture never needs converting) and scaled up on the card. op becomes a Group.
    bool blurFillOp(const Clip& c, const Ctx& ctx, const double shares[4], GpuOp& op)
    {
        constexpr int kMost = 640; // longer side of the shrunk picture (the background is at most 640 x 360)
        Image small;
        double aspect = 0;
        if (op.still) {
            const QImage& img = op.still->image;
            aspect = double(img.width()) / std::max(1, img.height());
            const double k = std::min(1.0, double(kMost) / std::max(img.width(), img.height()));
            small = Image(std::max(1, int(std::lround(img.width() * k))), std::max(1, int(std::lround(img.height() * k))));
            Compose::Affine m;
            m.a = double(small.w) / img.width();
            m.e = double(small.h) / img.height();
            Compose::draw(small.data(), small.w, small.h, img.constBits(), img.width(), img.height(), m, 1.0);
        } else {
            const int w = op.picture->frame->width, h = op.picture->frame->height;
            aspect = double(w) / std::max(1, h);
            const double k = std::min(1.0, double(kMost) / std::max(w, h));
            small = Image(std::max(2, int(std::lround(w * k))), std::max(2, int(std::lround(h * k))));
            if (!Video::toRgba(*op.picture, op.info, small.data(), small.w, small.h, true)) return false;
        }
        if (op.graded) ColorGrade::apply(small.data(), small.w, small.h, *ColorGrade::bake(op.grade, op.gradeLut.get()), 1);
        const BlurFill::Background bg = BlurFill::makeBackground(small.data(), small.w, small.h, aspect, shares,
                                                                 ctx.PW / ctx.PH, c.transform.fillBlur, c.transform.fillDim);
        if (bg.width <= 0 || bg.height <= 0) return true; // just the picture
        // One pixel of edge repeated around it: scaled up, the frame's edge samples it as composite() clamps
        const int pw = bg.width + 2, ph = bg.height + 2;
        auto padded = std::make_shared<Image>(pw, ph);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x)
                std::memcpy(padded->data() + (size_t(y) * pw + x) * 4,
                            bg.rgba.data() + (size_t(std::clamp(y - 1, 0, bg.height - 1)) * bg.width
                                              + std::clamp(x - 1, 0, bg.width - 1)) * 4,
                            4);
        GpuOp back;
        back.rgba = std::move(padded);
        back.premultiplied = true;
        back.m.a = double(ctx.W) / bg.width;
        back.m.e = double(ctx.H) / bg.height;
        back.m.c = -back.m.a;
        back.m.f = -back.m.e;
        GpuOp group;
        group.kind = GpuOp::Group;
        group.opacity = op.opacity;
        op.opacity = 1.0;
        group.a.push_back(std::move(back));
        group.a.push_back(std::move(op));
        op = std::move(group);
        return true;
    }

    // A picture rendered on the CPU (straight alpha) drawn at (x, y) with opacity k
    GpuOp cpuOp(Image&& img, double k, int x = 0, int y = 0)
    {
        layersCpu.fetch_add(1, std::memory_order_relaxed);
        GpuOp op;
        op.rgba = std::make_shared<Image>(std::move(img));
        op.opacity = k;
        op.m.c = x;
        op.m.f = y;
        return op;
    }

    // A clip the card does not draw by itself (clipLayer on the CPU), opacity times k; with a compositing graph the
    // card can evaluate, only the graph's sources are made on the CPU. false = nothing to draw
    bool layerOp(const Clip& c, int f, const QString& lane, const Ctx& ctx, const Timeline& tl, int depth, double k,
                 GpuOp& op)
    {
        CompCapture cap;
        CompCapture* const outer = t_compCapture;
        const EffectInstance* comp = EffectRegistry::instance(c, ::Comp::kEffectId);
        Gpu::Service* gpu = this->gpu.load(std::memory_order_relaxed);
        // (a tilted clip: the graph's result is placed through a homography, which only the CPU path draws)
        if (comp && comp->enabled && gpu && gpu->info().comp && !c.transform.hasBlurFill() && c.keepRegion.isEmpty()
            && !c.isTitle() && !tiltedEver(c)
            && [&] {
                   const EffectInstance* cs = EffectRegistry::instance(c, Shapes::kClipShapeEffect);
                   return !cs || !cs->enabled; // a cut to a shape is made on the CPU after the graph
               }()) {
            cap.clip = &c;
            t_compCapture = &cap;
        }
        Image img = clipLayer(c, f, lane, ctx, tl, depth);
        t_compCapture = outer;
        if (cap.plan) {
            if (cap.opacity <= 0) return false;
            op = GpuOp();
            op.comp = std::move(cap.plan);
            op.m = cap.m;
            op.crop = cap.crop;
            op.opacity = cap.opacity * k;
            layersCompGpu.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (img.empty()) return false;
        op = cpuOp(std::move(img), k);
        return true;
    }

    // One side of a transition: its draw (GPU or CPU layer), opacity times k; false = nothing
    bool gpuSide(std::vector<GpuOp>& out, const Clip* c, int f, const QString& lane, const Ctx& ctx, const Timeline& tl,
                 int depth, double k)
    {
        if (!c) return false;
        k *= TimelineOps::fadeRamp(*c, f - c->start);
        if (gpuClip(*c, f - c->start, ctx, lane)) {
            GpuOp op;
            if (!gpuMediaOp(*c, f, lane, ctx, k, op)) return false;
            out.push_back(std::move(op));
            return true;
        }
        GpuOp op;
        if (!layerOp(*c, f, lane, ctx, tl, depth, k, op)) return false;
        out.push_back(std::move(op));
        return true;
    }

    // A transition span on the GPU (types it has: dissolve, luma wipes, wipes between two clips without border, dip)
    void gpuTransition(std::vector<GpuOp>& ops, const Track& track, int trackIndex, const TimelineOps::TransitionSpan& s,
                       int f, const Ctx& ctx, const Timeline& tl, const QString& prefix, int depth)
    {
        auto byId = [&](int id) -> const Clip* {
            for (const Clip& c : track.clips)
                if (c.id == id) return &c;
            return nullptr;
        };
        const Clip* a = s.leftId ? byId(s.leftId) : nullptr;
        const Clip* b = s.rightId ? byId(s.rightId) : nullptr;
        if (a && !a->enabled) a = nullptr;
        if (b && !b->enabled) b = nullptr;
        const QString lane = prefix + QString("v%1").arg(trackIndex);
        const bool sameFile = a && b && a->mediaPath == b->mediaPath;
        const QString laneA = a ? clipLane(track, *a, lane) : lane;
        QString laneB = b ? clipLane(track, *b, lane) : lane;
        if (sameFile && laneB == laneA) laneB += 'x'; // the right side would share the left side's decoder
        const int len = s.length(), i = f - s.start;
        const TransitionStyle& st = s.style;
        const TransitionType type = st.type;
        const bool both = a && b;
        const bool wipe = type == TransitionType::WipeRight || type == TransitionType::WipeLeft
                          || type == TransitionType::WipeDown || type == TransitionType::WipeUp;
        const bool styled = type != TransitionType::CrossDissolve && (!st.isLuma() || both);
        if (styled && ((wipe && (!both || st.border >= 0.5)) || st.isSlide())) {
            // Not on the card yet: the CPU's picture of the track
            Image layer = trackLayer(track, trackIndex, f, ctx, tl, prefix, depth);
            if (!layer.empty()) ops.push_back(cpuOp(std::move(layer), 1.0));
            return;
        }
        if (type == TransitionType::DipToColor) {
            GpuOp g;
            g.kind = GpuOp::Group;
            g.fill = st.color;
            g.fill.setAlpha(255);
            const int half = both ? len / 2 : 0;
            if (a) gpuSide(g.a, a, f, laneA, ctx, tl, depth, alphaKeys(i, {{0, 1.0}, {(both ? half : len) - 1, 0.0}, {len - 1, 0.0}}));
            if (b) gpuSide(g.a, b, f, laneB, ctx, tl, depth, alphaKeys(i, {{0, 0.0}, {half, 0.0}, {len - 1, 1.0}}));
            ops.push_back(std::move(g));
            return;
        }
        if (!both) { // one side: fade out/in through transparency
            if (a) gpuSide(ops, a, f, laneA, ctx, tl, depth, double(len - 1 - i) / len);
            else if (b) gpuSide(ops, b, f, laneB, ctx, tl, depth, double(i) / len);
            return;
        }
        GpuOp m;
        m.kind = GpuOp::Mix;
        m.t = double(i) / len;
        const bool hasA = gpuSide(m.a, a, f, laneA, ctx, tl, depth, 1.0);
        const bool hasB = gpuSide(m.b, b, f, laneB, ctx, tl, depth, 1.0);
        if (!hasA && !hasB) return;
        if (hasA != hasB) { // one side empty (no picture): as the CPU path, a fade of the other
            std::vector<GpuOp>& one = hasA ? m.a : m.b;
            for (GpuOp& op : one) op.opacity *= hasA ? double(len - 1 - i) / len : double(i) / len;
            for (GpuOp& op : one) ops.push_back(std::move(op));
            return;
        }
        if (styled) {
            m.kind = GpuOp::Luma;
            m.t = Generators::ease(st.ease, m.t);
            m.softness = 0.02 + 0.98 * std::clamp(st.softness, 0.0, 100.0) / 100.0;
            m.gradient = gradient(st, ctx.W, ctx.H);
            if (!m.gradient) m.kind = GpuOp::Mix; // unknown/unreadable luma image
        }
        ops.push_back(std::move(m));
    }

    void gpuTrack(std::vector<GpuOp>& ops, const Track& track, int trackIndex, int f, const Ctx& ctx, const Timeline& tl,
                  const QString& prefix, int depth)
    {
        if (track.hidden) return;
        for (const auto& s : spansAt(track, f, tl))
            if (f >= s.start && f < s.end) {
                gpuTransition(ops, track, trackIndex, s, f, ctx, tl, prefix, depth);
                return;
            }
        const Clip* c = clipAt(track, f);
        if (!c || !c->enabled) return;
        const QString lane = trackClipLane(tl, trackIndex, *c, prefix);
        const int t = f - c->start;
        const double fade = TimelineOps::fadeRamp(*c, t);
        if (ctx.preview && options.renderCache && depth == 0 && !c->isTitle() && !c->mediaPath.isEmpty()
            && !(ctx.bypassGrade && ColorGrade::active(*c))) {
            if (const QString file = options.renderCache(*c); !file.isEmpty()) {
                const Source s = source(file, lane + QStringLiteral("c"));
                if (s.video) {
                    QSize native;
                    Image img = videoFrame(*s.video, t, ctx.W, ctx.H, ctx, &native);
                    if (!img.empty()) {
                        layersDecoded.fetch_add(1, std::memory_order_relaxed);
                        ops.push_back(cpuOp(std::move(img), fade));
                        return;
                    }
                }
            }
        }
        if (gpuClip(*c, t, ctx, lane)) {
            GpuOp op;
            if (gpuMediaOp(*c, f, lane, ctx, fade, op)) ops.push_back(std::move(op));
            return;
        }
        const bool plain = !Keys::hasTransform(*c) && !Keys::hasCrop(*c);
        const double opacity = Keys::hasOpacity(*c) ? std::clamp(Keys::valueAt(*c, AnimParam::Opacity, t) / 100.0, 0.0, 1.0) : 1.0;
        if (c->isTitle() && plain && opacity * fade >= 1.0 && c->effects.isEmpty() && !Keys::animated(*c, AnimParam::TitleSize)) {
            // Drawn only as large as its text
            Image dummy;
            QRect box;
            drawTitle(dummy, *c, t, ctx, 0, {}, &box);
            if (box.isEmpty()) return;
            Image part(box.width(), box.height());
            drawTitle(part, *c, t, ctx, 0, box.topLeft());
            ops.push_back(cpuOp(std::move(part), 1.0, box.x(), box.y()));
            return;
        }
        GpuOp op;
        if (layerOp(*c, f, lane, ctx, tl, depth, fade, op)) ops.push_back(std::move(op));
    }

    bool subtitleVisible(const Timeline& tl, int f) const
    {
        for (const SubtitleTrack& st : tl.subtitles) {
            if (!st.enabled) continue;
            for (const SubtitleCue& cue : st.cues)
                if (f >= cue.start && f < cue.end && !cue.text.trimmed().isEmpty()) return true;
        }
        return false;
    }

    // The draws of a timeline's picture at frame f (as renderTimeline)
    void gpuTimeline(std::vector<GpuOp>& ops, const Timeline& tl, int f, const Ctx& ctx, bool subtitles)
    {
        const TimelineScope scope(tl);
        int lowest = 0;
        for (int i = tl.video.size() - 1; i >= 0; --i)
            if (covers(tl.video[i], i, f, ctx, tl, QString())) {
                lowest = i;
                for (int j = 0; j < i; ++j)
                    if (!tl.video[j].hidden && clipAt(tl.video[j], f)) layersSkipped.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        for (int i = lowest; i < tl.video.size(); ++i) gpuTrack(ops, tl.video[i], i, f, ctx, tl, QString(), 0);
        if (subtitles && subtitleVisible(tl, f)) {
            Image layer(ctx.W, ctx.H);
            subtitleLayer(layer, tl, f, ctx);
            ops.push_back(cpuOp(std::move(layer), 1.0));
        }
    }

    // GPU thread: the baked grades as 3D LUTs, the last few (keyframed grades bake one per frame)
    struct LutCache {
        struct Entry {
            ColorGrade::Params params;
            // The loaded LUT itself, not its path: a file read again (overwritten, or missing before) is a new
            // object. Held here, so its address cannot be reused by another LUT while the entry lives
            std::shared_ptr<const ColorGrade::Lut> source;
            Gpu::LutPtr lut;
        };
        std::vector<Entry> entries;
        Gpu::LutPtr get(Gpu::Context& g, const GpuOp& op)
        {
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].params == op.grade && entries[i].source == op.gradeLut) {
                    std::rotate(entries.begin(), entries.begin() + i, entries.begin() + i + 1);
                    return entries.front().lut;
                }
            Gpu::LutPtr l = g.gradeLut(*ColorGrade::bake(op.grade, op.gradeLut.get()));
            if (!l) return {};
            entries.insert(entries.begin(), {op.grade, op.gradeLut, l});
            if (entries.size() > 16) entries.pop_back();
            return l;
        }
    };

    struct GpuCaches {
        LutCache luts;
        std::map<const StillSource*, std::pair<std::weak_ptr<StillSource>, Gpu::TexturePtr>> stills;
    };

    // What a GPU job keeps alive until the frame is submitted (uploads read their memory then; textures go back to
    // the pool after that)
    struct GpuKeep {
        std::vector<Gpu::TexturePtr> textures;
        std::vector<std::vector<uint8_t>> buffers;
        bool compRefused = false; // a compositing graph the card did not make: the frame goes the CPU way
        bool refused = false;     // a picture the card did not take (upload refused): the frame goes the CPU way
    };

    // GPU thread: the draws onto dst (size of the output)
    static void runOps(Gpu::Context& g, Gpu::Texture& dst, const std::vector<GpuOp>& ops, QSize size, GpuKeep& keep)
    {
        auto& slot = g.userData(); // the caches live with the context (their textures belong to it)
        if (!slot) slot = std::make_shared<GpuCaches>();
        GpuCaches& caches = *static_cast<GpuCaches*>(slot.get());
        LutCache& luts = caches.luts;
        for (const GpuOp& op : ops) {
            switch (op.kind) {
            case GpuOp::Draw: {
                Gpu::TexturePtr src;
                const bool upload = (op.rgba && !op.rgba->empty()) || op.picture || op.still;
                if (op.rgba && !op.rgba->empty()) { // made on the CPU (also an interlaced picture, see gpuMediaOp)
                    src = g.uploadRgba(op.rgba->data(), op.rgba->w, op.rgba->h, op.premultiplied, op.mips);
                } else if (op.picture) {
                    src = g.uploadYuv(op.picture->frame, op.info, op.mips);
                    if (!src) { // a format the planes upload does not take after all: converted here
                        const int w = op.picture->frame->width, h = op.picture->frame->height;
                        std::vector<uint8_t> rgba(size_t(w) * h * 4);
                        if (Video::toRgba(*op.picture, op.info, rgba.data(), w, h, false))
                            src = g.uploadRgba(rgba.data(), w, h, false);
                        keep.buffers.push_back(std::move(rgba));
                    }
                } else if (op.still) {
                    // Stills stay on the card while their source lives (a photo is uploaded once, not every frame)
                    auto& stills = caches.stills;
                    auto it = stills.find(op.still.get());
                    if (it != stills.end() && it->second.first.lock() == op.still) {
                        src = it->second.second;
                    } else {
                        // With mip levels: uploaded once, drawn every frame - shrunk, a level is read instead of a grid
                        // of taps. The levels average what is stored: a still with transparency is premultiplied first
                        const QImage& img = op.still->image;
                        if (op.still->opaque) {
                            src = g.uploadRgba(img.constBits(), img.width(), img.height(), false, true);
                        } else {
                            std::vector<uint8_t> pm(size_t(img.width()) * img.height() * 4);
                            for (int y = 0; y < img.height(); ++y) {
                                const uint8_t* in = img.constScanLine(y);
                                uint8_t* out = pm.data() + size_t(y) * img.width() * 4;
                                for (int x = 0; x < img.width() * 4; x += 4) {
                                    const uint32_t a = in[x + 3];
                                    for (int c = 0; c < 3; ++c) out[x + c] = uint8_t((in[x + c] * a + 127) / 255);
                                    out[x + 3] = uint8_t(a);
                                }
                            }
                            src = g.uploadRgba(pm.data(), img.width(), img.height(), true, true);
                            keep.buffers.push_back(std::move(pm)); // read when the frame is submitted
                        }
                        for (auto e = stills.begin(); e != stills.end();) // sources closed since: textures go
                            e = e->second.first.expired() ? stills.erase(e) : std::next(e);
                        if (src) stills[op.still.get()] = {op.still, src};
                    }
                } else if (op.comp) {
                    src = Gpu::runComp(g, *op.comp, &keep.textures);
                    if (!src) { // (a pass or a texture refused) not a frame without the clip: the CPU makes it
                        keep.compRefused = true;
                        static std::once_flag warned;
                        std::call_once(warned, [] { qWarning("Picture engine: compositing graph refused by the card (frame on the CPU)"); });
                        return;
                    }
                }
                if (!src) {
                    // An upload the card refused (larger than it takes, out of memory): not a frame without the
                    // layer - the CPU makes the frame
                    if (!upload) break;
                    keep.refused = true;
                    return;
                }
                Gpu::LutPtr lut = op.graded ? luts.get(g, op) : nullptr;
                g.draw(dst, {src.get(), op.m, op.opacity, lut.get(), op.crop});
                keep.textures.push_back(std::move(src));
                break;
            }
            case GpuOp::Group: {
                const float clear[4] = {float(op.fill.redF()), float(op.fill.greenF()), float(op.fill.blueF()), float(op.fill.alphaF())};
                Gpu::TexturePtr t = g.target(size, clear);
                if (!t) break;
                runOps(g, *t, op.a, size, keep);
                g.draw(dst, {t.get(), {}, op.opacity, nullptr, {}});
                keep.textures.push_back(std::move(t));
                break;
            }
            case GpuOp::Mix:
            case GpuOp::Luma: {
                Gpu::TexturePtr ta = g.target(size), tb = g.target(size);
                if (!ta || !tb) break;
                runOps(g, *ta, op.a, size, keep);
                runOps(g, *tb, op.b, size, keep);
                Gpu::TexturePtr out;
                if (op.kind == GpuOp::Luma && op.gradient) {
                    Gpu::TexturePtr grad = g.uploadGray(op.gradient->data(), size.width(), size.height());
                    if (grad) out = g.luma(*ta, *tb, *grad, op.t, op.softness);
                    keep.textures.push_back(std::move(grad));
                } else {
                    out = g.mix(*ta, *tb, op.t);
                }
                if (out) g.draw(dst, {out.get(), {}, 1.0, nullptr, {}});
                keep.textures.push_back(std::move(ta));
                keep.textures.push_back(std::move(tb));
                keep.textures.push_back(std::move(out));
                break;
            }
            }
        }
    }

    // The frame on the GPU path into f (buffer or hints.yuv); false = the card could not do it (CPU path then)
    bool renderGpu(Gpu::Service& gpu, Frame& f, int64_t frame, const Ctx& ctx, const RenderHints& hints)
    {
        if (shapeReadsBelow(*ctx.root, int(frame))) return false; // blur/pixelate/spotlight shapes: CPU
        std::vector<GpuOp> ops;
        gpuTimeline(ops, *ctx.root, int(frame), ctx, options.subtitles);
        if (hints.cancel && hints.cancel->load()) return true; // caller sees the cancel
        const QSize size(ctx.W, ctx.H);
        const bool transparent = options.transparent;
        bool yuvDone = false;
        bool ok = false;
        GpuKeep keep;
        const OutputColor::Target* yuv =
            hints.yuv && hints.yuv->bits == 8
                    && (hints.yuv->layout == OutputColor::Layout::Yuv420 || hints.yuv->layout == OutputColor::Layout::Nv12)
                ? hints.yuv
                : nullptr;
        if (!yuv) f.m_buffer = pool().take(size_t(ctx.W) * ctx.H * 4);
        uint8_t* rgba = f.m_buffer ? f.m_buffer->data.data() : nullptr;
        const bool ran = gpu.run([&](Gpu::Context& g) {
            const float black[4] = {0, 0, 0, 1};
            Gpu::TexturePtr canvas = g.target(size, transparent ? nullptr : black);
            if (!canvas) return;
            runOps(g, *canvas, ops, size, keep);
            if (keep.compRefused || keep.refused) return;
            if (yuv) yuvDone = g.readYuv(*canvas, *yuv);
            if (!yuvDone) {
                if (!rgba) return; // the card could not write the planes after all: CPU path
                g.readRgba(*canvas, rgba);
            }
            keep.textures.push_back(std::move(canvas));
            ok = true;
        });
        // Not run: shut down, or the card refused or failed the frame (device lost: the job ran, but its readbacks were
        // never filled - the buffer holds an old picture) -> CPU path
        if (!ran || !ok) {
            f.m_buffer.reset();
            return false;
        }
        f.m_yuvWritten = yuvDone;
        f.m_gpu = true;
        framesGpu.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Drops sources last used before output frame `frame` (export only: it renders forward)
    void dropBefore(int64_t frame);

    // A timeline's picture at frame f onto canvas (black or transparent background already there)
    void renderTimeline(const Timeline& tl, int f, Image& canvas, const Ctx& ctx, const QString& prefix, bool subtitles,
                        int depth)
    {
        const TimelineScope scope(tl);
        // Top down to the first track that surely covers the frame: what lies below is never decoded
        // (tests/test_occlusion), then bottom up onto the canvas
        int lowest = 0;
        bool lowestCovers = false;
        for (int i = tl.video.size() - 1; i >= 0; --i)
            if (covers(tl.video[i], i, f, ctx, tl, prefix)) {
                lowest = i;
                lowestCovers = true;
                for (int j = 0; j < i; ++j)
                    if (!tl.video[j].hidden && clipAt(tl.video[j], f)) layersSkipped.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        for (int i = lowest; i < tl.video.size(); ++i)
            drawTrack(canvas, i == lowest && lowestCovers, tl.video[i], i, f, ctx, tl, prefix, depth);
        if (subtitles) subtitleLayer(canvas, tl, f, ctx);
    }
};

void Renderer::Impl::dropBefore(int64_t frame)
{
    std::vector<Source> gone; // closed outside the lock
    std::lock_guard lock(sourceMutex);
    for (auto it = sources.begin(); it != sources.end();)
        if (it->second.usedFrame >= 0 && it->second.usedFrame < frame) {
            gone.push_back(std::move(it->second));
            it = sources.erase(it);
        } else {
            ++it;
        }
}

Renderer::Renderer(const Timeline& tl, const Options& options) : d(std::make_unique<Impl>())
{
    d->options = options;
    d->snapshot = std::make_shared<const Timeline>(tl);
    d->output = options.outputSize();
    if (options.gpu) Gpu::Service::prepare(); // the surface on the GUI thread; the context comes with the first frame
}

Renderer::~Renderer() = default;

Frame Renderer::render(int64_t frame)
{
    return render(frame, RenderHints{});
}

Frame Renderer::render(int64_t frame, const RenderHints& hints)
{
    if (!d->options.overlay) return renderFrame(frame, hints);
    // The overlay needs the RGBA picture: no pass-through or GPU read into the encoder's planes
    RenderHints rgba = hints;
    rgba.yuv = nullptr;
    Frame f = renderFrame(frame, rgba);
    if (!f.isNull() && f.m_buffer) d->options.overlay(f.m_buffer->data.data(), f.m_width, f.m_height, frame);
    return f;
}

Frame Renderer::renderFrame(int64_t frame, const RenderHints& hints)
{
    if (hints.cancel && hints.cancel->load()) return {};
    Impl::Ctx ctx;
    QSize out;
    {
        std::lock_guard lock(d->snapMutex);
        ctx.root = d->snapshot;
        out = d->output;
    }
    ctx.W = std::max(1, out.width());
    ctx.H = std::max(1, out.height());
    ctx.PW = d->options.format.width;
    ctx.PH = d->options.format.height;
    ctx.fps = d->options.format.rate.fps();
    ctx.end = ctx.root ? TimelineOps::endFrame(*ctx.root) : 0;
    if (d->options.rangeEnd >= 0) { // export of a range: the bar fills over the exported video
        ctx.begin = int(d->options.rangeStart);
        ctx.end = int(d->options.rangeEnd) + 1;
    }
    ctx.preview = d->options.mode == Mode::Preview;
    ctx.bypassGrade = ctx.preview && d->options.gradeBypass && d->options.gradeBypass->load();

    // Export renders forward: decoders (and their cached pictures) of clips the export has passed go. A source asked
    // for again later (the same file further on) is opened again.
    if (d->options.mode == Mode::Export && frame % 16 == 0)
        d->dropBefore(frame - std::max<int64_t>(int64_t(2 * ctx.fps), 4 * d->options.parallelFrames + 8));
    t_frame = frame;
    ++t_renderSeq;
    t_hints = hints.nearest || hints.cancel ? &hints : nullptr;
    t_exact = true;
    t_sourceLost = false;
    t_lostFile.clear();
    // Export: a frame without a video whose decoder was lost is no frame (the export fails, saying so, instead of
    // writing the clip black)
    const auto lost = [&] {
        if (!t_sourceLost || d->options.mode != Mode::Export) return false;
        std::lock_guard lock(d->lostMutex);
        d->lost = t_lostFile;
        return true;
    };

    if (hints.yuv && d->passThrough(int(frame), ctx, *hints.yuv)) {
        t_hints = nullptr;
        if ((hints.cancel && hints.cancel->load()) || lost()) return {};
        Frame f;
        f.m_yuvWritten = true;
        f.m_exact = t_exact;
        f.m_width = ctx.W;
        f.m_height = ctx.H;
        f.m_position = frame;
        f.m_opaque = true;
        d->framesRendered.fetch_add(1, std::memory_order_relaxed);
        return f;
    }
    t_exact = true; // a fetch that did not pass through is fetched again
    if (Gpu::Service* gpu = d->gpuService()) {
        Frame f;
        if (d->renderGpu(*gpu, f, frame, ctx, hints)) {
            t_hints = nullptr;
            if ((hints.cancel && hints.cancel->load()) || lost()) return {};
            f.m_exact = t_exact;
            f.m_width = ctx.W;
            f.m_height = ctx.H;
            f.m_position = frame;
            f.m_opaque = !d->options.transparent
                         || (f.m_buffer && fullyOpaque(f.m_buffer->data.data(), size_t(ctx.W) * ctx.H));
            d->framesRendered.fetch_add(1, std::memory_order_relaxed);
            return f;
        }
        t_exact = true; // the CPU path fetches again
    }

    // Rendered straight into a pooled buffer (its vector lent to the canvas)
    Frame f;
    f.m_buffer = pool().take(size_t(ctx.W) * ctx.H * 4);
    Image canvas;
    canvas.w = ctx.W;
    canvas.h = ctx.H;
    canvas.px.swap(f.m_buffer->data);
    if (d->options.transparent) std::memset(canvas.data(), 0, canvas.px.size());
    else fill(canvas, Qt::black);
    d->renderTimeline(*ctx.root, int(frame), canvas, ctx, QString(), d->options.subtitles, 0);
    canvas.px.swap(f.m_buffer->data);
    t_hints = nullptr;
    if ((hints.cancel && hints.cancel->load()) || lost()) return {};
    f.m_exact = t_exact;
    f.m_width = ctx.W;
    f.m_height = ctx.H;
    f.m_position = frame;
    f.m_opaque = !d->options.transparent || fullyOpaque(f.m_buffer->data.data(), size_t(ctx.W) * ctx.H);
    d->framesRendered.fetch_add(1, std::memory_order_relaxed);
    return f;
}

void Renderer::update(const Timeline& tl)
{
    auto snap = std::make_shared<const Timeline>(tl);
    std::lock_guard lock(d->snapMutex);
    d->snapshot = std::move(snap);
}

void Renderer::setOutputSize(const QSize& size)
{
    std::lock_guard lock(d->snapMutex);
    d->output = size.isEmpty() ? d->options.format.size() : size;
}

void Renderer::setDirection(int direction)
{
    if (d->direction.exchange(direction) == direction) return;
    std::lock_guard lock(d->sourceMutex);
    for (auto& [key, s] : d->sources)
        if (s.video && s.video->reader) {
            const int keep = std::max(4, d->options.parallelFrames);
            s.video->reader->setBehind(direction < 0 ? std::max(keep, Impl::backwardKeep(s.video->info.width,
                                                                                         s.video->info.height))
                                                     : keep);
        }
}

void Renderer::prepare(int64_t frame)
{
    std::shared_ptr<const Timeline> tl;
    {
        std::lock_guard lock(d->snapMutex);
        tl = d->snapshot;
    }
    const double fps = d->options.format.rate.fps();
    const int64_t soon = frame + int64_t(3 * fps); // as far as Preroll looked ahead
    Impl::Ctx ctx;
    ctx.root = tl;
    ctx.fps = fps;
    ctx.end = tl ? TimelineOps::endFrame(*tl) : 0;
    if (d->options.rangeEnd >= 0) {
        ctx.begin = int(d->options.rangeStart);
        ctx.end = int(d->options.rangeEnd) + 1;
    }
    ctx.PW = d->options.format.width;
    ctx.PH = d->options.format.height;
    ctx.preview = d->options.mode == Mode::Preview;
    QSize out;
    {
        std::lock_guard lock(d->snapMutex);
        out = d->output;
    }
    for (int i = 0; i < tl->video.size(); ++i) {
        const Track& track = tl->video[i];
        // The gradient of a luma transition or wipe starting soon (built once, before its first frame is due)
        for (const auto& s : d->spansOf(track, *tl)) {
            const TransitionType type = s.style.type;
            const bool wipe = type == TransitionType::WipeRight || type == TransitionType::WipeLeft
                              || type == TransitionType::WipeDown || type == TransitionType::WipeUp;
            if (s.end > frame && s.start <= soon && s.isDissolve() && (s.style.isLuma() || wipe))
                d->gradient(s.style, std::max(1, out.width()), std::max(1, out.height()));
        }
        const QVector<Clip>& clips = track.clips;
        for (const Clip& c : clips) {
            if (!c.enabled || c.mediaPath.isEmpty() || c.isTitle() || c.isNested()) continue;
            const QString lane = d->trackClipLane(*tl, i, c, QString());
            if (frame >= c.start - 50 && frame < c.end()) d->source(c.mediaPath, lane);
            // A clip starting soon: decode its first picture now (long-GOP material decodes up to seconds from the
            // keyframe before its in point; in the middle of playback the picture stood that long). Only when the
            // clip's decoder is not in use until then (a split without a jump continues on it, see clipLane).
            if (c.start <= frame || c.start > soon) continue;
            // (on any track: a kept copy reads the decoder of the clip below it, see trackClipLane)
            bool shared = false;
            for (int j = 0; j < tl->video.size() && !shared; ++j)
                shared = std::any_of(tl->video[j].clips.cbegin(), tl->video[j].clips.cend(), [&](const Clip& o) {
                    return o.id != c.id && o.mediaPath == c.mediaPath && o.end() > frame && o.start < c.start
                           && d->trackClipLane(*tl, j, o, QString()) == lane;
                });
            if (shared) continue;
            QSize native;
            d->mediaPicture(c, 0, lane, 0, 0, ctx, &native); // decodes, converts nothing
        }
    }
}

void Renderer::trim(int64_t shown)
{
    trim(std::chrono::steady_clock::now(), shown);
}

void Renderer::trim(std::chrono::steady_clock::time_point now, int64_t shown)
{
    // Sources stay open over pauses, however long: Play opening its files again waited for the first picture (1080p60
    // ~260 ms, 4K HEVC ~550 ms; open: 2-15 ms, bench_play_after_pause), and the readers put an unused decoder to sleep
    // themselves (video/Reader: all but the 4 most recent after 4 s; asleep ~2 MB each). Closed only
    // - under memory pressure: everything unused for a few seconds
    // - no longer used by the timeline (project closed, clip deleted, the proxy finished): after 30 s unused (an undo
    //   right after a delete finds it open)
    // - beyond kKeepSources by last use, once unused for 10 s: playing through many files does not keep all of them
    //   (a sleeping reader's thread also keeps up to ~50 MB of glibc arena)
    using namespace std::chrono;
    const bool pressure = MemoryRelease::underPressure();
    std::shared_ptr<const Timeline> tl;
    {
        std::lock_guard lock(d->snapMutex);
        tl = d->snapshot;
    }
    // The files the timeline reads: originals, the proxies read for them, rendered clips
    // ... and those of the clips at the shown frame (outer timeline; a compound clip's inner files all count)
    QSet<QString> used, atShown;
    if (tl) {
        std::function<void(const Timeline&, bool)> collect = [&](const Timeline& t, bool outer) {
            for (const QVector<Track>* tracks : {&t.video, &t.audio})
                for (const Track& track : *tracks)
                    for (const Clip& c : track.clips) {
                        const bool here = outer && shown >= 0 && tracks == &t.video && shown >= c.start
                                          && shown < c.end();
                        if (here && c.isNested() && tl->nested)
                            if (const auto n = tl->nested->constFind(c.sequenceId); n != tl->nested->cend()) {
                                QSet<QString> inner;
                                std::swap(inner, used);
                                collect(*n, false);
                                std::swap(inner, used);
                                atShown.unite(inner);
                                used.unite(inner);
                            }
                        if (c.mediaPath.isEmpty()) continue;
                        QStringList files{c.mediaPath};
                        if (d->options.proxy) files << d->options.proxy(c.mediaPath);
                        if (d->options.renderCache && tracks == &t.video)
                            if (const QString file = d->options.renderCache(c); !file.isEmpty()) files << file;
                        for (const QString& f : files) {
                            used.insert(f);
                            if (here) atShown.insert(f);
                        }
                    }
        };
        collect(*tl, true);
        if (tl->nested)
            for (const Timeline& n : *tl->nested) collect(n, false);
    }
    std::vector<Source> closed; // closed outside the lock (a reader joins its threads)
    std::lock_guard lock(d->sourceMutex);
    const auto fileOf = [](const QString& key) { return key.mid(key.indexOf('|') + 1); };
    for (auto it = d->sources.begin(); it != d->sources.end();) {
        const auto idle = now - it->second.used;
        // A still picture (up to 4x the project's pixels as RGBA: ~33 MB at 1080p, ~133 MB at 4K) is quick to read
        // again and holds no decoder: closed after 30 s unused, as before
        const bool drop = (pressure && idle > seconds(5))
                          || (idle > seconds(30) && (it->second.still || !used.contains(fileOf(it->first))));
        if (drop) {
            closed.push_back(std::move(it->second));
            it = d->sources.erase(it);
        } else {
            ++it;
        }
    }
    if (int(d->sources.size()) > kKeepSources) {
        // Never what the shown frame needs (paused: Play goes on from there), whatever its last use - the pump may
        // have rendered ahead into the next clip and prepared later ones since
        std::vector<std::pair<steady_clock::time_point, QString>> byUse;
        for (const auto& [key, s] : d->sources)
            if (!atShown.contains(fileOf(key))) byUse.emplace_back(s.used, key);
        std::sort(byUse.begin(), byUse.end()); // least recently used first
        size_t over = d->sources.size() - kKeepSources;
        for (size_t i = 0; i < byUse.size() && over > 0; ++i, --over) {
            if (now - byUse[i].first <= seconds(10)) break; // in use: kept beyond the count
            auto it = d->sources.find(byUse[i].second);
            closed.push_back(std::move(it->second));
            d->sources.erase(it);
        }
    }
}

int64_t Renderer::length() const
{
    std::lock_guard lock(d->snapMutex);
    return TimelineOps::endFrame(*d->snapshot);
}

const Options& Renderer::options() const { return d->options; }

void Renderer::retryLost()
{
    ++d->retryGen;
}

QString Renderer::lostSource() const
{
    std::lock_guard lock(d->lostMutex);
    return d->lost;
}

Renderer::Stats Renderer::stats() const
{
    Stats s;
    s.framesRendered = d->framesRendered;
    s.layersDecoded = d->layersDecoded;
    s.layersSkipped = d->layersSkipped;
    s.framesGpu = d->framesGpu;
    s.layersCpu = d->layersCpu;
    s.layersCompGpu = d->layersCompGpu;
    s.gradients = d->gradientsBuilt;
    std::lock_guard lock(d->sourceMutex);
    s.sourcesOpen = int(d->sources.size());
    return s;
}

void releasePools()
{
    layerPool().clear();
    pool().clear();
}

QVector<SkippedEffect> skippedEffects(const Timeline& tl)
{
    QVector<SkippedEffect> out;
    QList<int> seen; // nested sequences looked at (`nested` holds all of them, the open one too: only those clips show)
    std::function<void(const Timeline&, int)> check = [&](const Timeline& t, int sequence) {
        // Only what the render really uses: a hidden video track draws nothing, a muted audio track (or one silent
        // because another is solo, like the mixer) plays nothing, a disabled clip neither
        const bool anySolo = std::any_of(t.audio.begin(), t.audio.end(), [](const Track& a) { return a.solo; });
        const auto used = [&](const Track& track, const Clip& c, bool video) {
            if (!c.enabled) return false;
            return video ? !track.hidden : !track.muted && (!anySolo || track.solo);
        };
        for (const Track& track : t.video)
            for (const Clip& c : track.clips) {
                if (!used(track, c, true)) continue;
                for (const EffectInstance& e : c.effects) {
                    if (!e.enabled) continue;
                    // A compositing graph renders; node kinds of a newer version in it are passed through
                    // Custom code the user did not confirm (core/CodeTrust.h) stays unrun
                    const auto untrusted = [&](const EffectInstance& inst) {
                        for (const CodeTrust::Item& it : CodeTrust::itemsOf(inst))
                            if (!CodeTrust::isTrusted(it.code))
                                out << SkippedEffect{c.id, it.node ? QStringLiteral("comp:") + it.effect : it.effect,
                                                     true, sequence, QStringLiteral("untrusted")};
                    };
                    if (e.effectId == QLatin1String(::Comp::kEffectId)) {
                        for (const QString& kind : Comp::unknownKinds(e))
                            out << SkippedEffect{c.id, QStringLiteral("comp:") + kind, true, sequence};
                        untrusted(e);
                        continue;
                    }
                    if (Custom::isCustom(e.effectId) && Custom::canRender(e.effectId)) {
                        untrusted(e);
                        continue;
                    }
                    const EffectDescriptor* d = EffectRegistry::find(e.effectId);
                    if (d && (d->fixed || !d->video || e.effectId == QLatin1String("color")
                              || e.effectId == QLatin1String("blur") || e.effectId == QLatin1String("chromakey")
                              || VideoEffects::isOwn(e.effectId)))
                        continue;
                    out << SkippedEffect{c.id, e.effectId, true, sequence}; // not known here or not drawn by this engine
                }
            }
        // Sound: the mixer plays the effects it knows; an unknown id (LADSPA plugin, newer version) is left out
        for (const Track& track : t.audio)
            for (const Clip& c : track.clips)
                if (used(track, c, false))
                    for (const EffectInstance& e : c.effects)
                        if (e.enabled && !EffectRegistry::find(e.effectId))
                            out << SkippedEffect{c.id, e.effectId, false, sequence};
        if (!tl.nested) return;
        for (const QVector<Track>* tracks : {&t.video, &t.audio})
            for (const Track& track : *tracks)
                for (const Clip& c : track.clips) {
                    if ((c.kind != ClipKind::Compound && c.kind != ClipKind::Multicam) || seen.contains(c.sequenceId)
                        || !used(track, c, tracks == &t.video))
                        continue;
                    seen << c.sequenceId;
                    if (const auto it = tl.nested->constFind(c.sequenceId); it != tl.nested->constEnd())
                        check(it.value(), it.key());
                }
    };
    check(tl, -1);
    return out;
}

} // namespace Picture
