// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/Renderer.h"

#include "core/Captions.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Multicam.h"
#include "core/Retime.h"
#include "core/TimelineOps.h"
#include "engine/BlurFill.h"
#include "engine/ColorGrade.h"
#include "engine/Lumas.h"
#include "engine/OutputColor.h"
#include "engine/StabilizeCorrection.h"
#include "engine/VideoEffects.h"
#include "engine/picture/Effects.h"
#include "engine/picture/gpu/Gpu.h"
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
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
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
};

struct StillSource {
    QImage image; // RGBA8888, straight alpha
    bool opaque = true;
};

struct Source {
    std::shared_ptr<VideoSource> video;
    std::shared_ptr<StillSource> still;
    bool failed = false;
    std::chrono::steady_clock::time_point used;
    int64_t usedFrame = -1; // last output frame that asked for it (export: dropped once rendering went past it)
};

// Output frame the calling thread renders right now (Renderer::render)
thread_local int64_t t_frame = -1;
// Its hints (null = none) and whether a close picture stood in for one of its videos
thread_local const RenderHints* t_hints = nullptr;
thread_local bool t_exact = true;

// Preview: what a decoder keeps of the pictures decoded on the way to a jump's target (steps back after a jump)
constexpr int64_t kSeekWindowBytes = 96ll * 1024 * 1024;

bool looksLikeStill(const QString& path)
{
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
    std::mutex lengthMutex;
    std::map<QString, int> lengths; // file -> length in project frames (mediaLength)

    std::mutex lutMutex;
    std::map<QString, std::shared_ptr<const ColorGrade::Lut>> luts;

    std::atomic<int> framesRendered{0}, layersDecoded{0}, layersSkipped{0}, framesGpu{0}, layersCpu{0};
    // GPU path (Options::gpu and a hardware device), looked up by the renders (GUI app: CPU until the GPU has started)
    std::atomic<Gpu::Service*> gpu{nullptr};
    std::atomic<bool> gpuDecided{false};
    Gpu::Service* gpuService()
    {
        if (!options.gpu || gpuDecided.load()) return gpu.load();
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
    };

    Source source(const QString& path, const QString& lane)
    {
        const QString file = options.proxy ? options.proxy(path) : path;
        const QString key = lane + '|' + file;
        {
            std::lock_guard lock(sourceMutex);
            auto it = sources.find(key);
            if (it != sources.end()) {
                it->second.used = std::chrono::steady_clock::now();
                it->second.usedFrame = std::max(it->second.usedFrame, t_frame);
                return it->second;
            }
        }
        Source s;
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
                v->reader = std::move(r);
                s.video = std::move(v);
            }
        }
        s.failed = !s.video && !s.still;
        s.used = std::chrono::steady_clock::now();
        s.usedFrame = t_frame;
        std::lock_guard lock(sourceMutex);
        return sources.emplace(key, s).first->second;
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

    std::shared_ptr<const ColorGrade::Lut> lut(const QString& path)
    {
        std::lock_guard lock(lutMutex);
        auto it = luts.find(path);
        if (it != luts.end()) return it->second;
        auto l = ColorGrade::loadLut(path);
        luts.emplace(path, l);
        return l;
    }

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

    // Effects on the clip's picture (before crop/transform), clip frame t; order as TimelineBuilderDetail::applyEffects:
    // the fixed stages first (grade, then the Green Screen key), then every other effect in the clip's order.
    // frameHeight: the frame's height in pixels of img (blur strength is relative to it); fit: the size img stands for
    // in project pixels (a source of another aspect ratio: the fitted picture without the bars, centred in the frame)
    void applyEffects(Image& img, const Clip& c, double t, const Ctx& ctx, double frameHeight, QSizeF fit)
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
            } else if (!VideoEffects::apply(inst.effectId, img.data(), img.w, img.h, effectParams(c, inst, *d, t),
                                            int64_t(c.in + t))) {
                static std::once_flag warned;
                std::call_once(warned, [&] {
                    qWarning("Picture engine: effect %s not supported yet (skipped)", qPrintable(inst.effectId));
                });
            }
        }
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
        const double file = fileFrame(c, t, *s.video);
        if (file < 0) return {};
        return videoFrame(*s.video, file, dw, dh, ctx, native, into);
    }

    // Frame of the file (project rate) a video clip shows at clip frame t; < 0 = none
    static double fileFrame(const Clip& c, int t, const VideoSource& v)
    {
        // Material frame -> file frame (project rate) -> stream position
        const double m = c.freeze ? c.stillFrame() : c.in + t;
        double file = m;
        // Speed, reverse, ramp and freeze frame: the one mapping of RetimeMap (by the sound's time)
        if (c.isRetimed()) file = RetimeMap(c, v.length).pictureFrame(m);
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
    void drawTitle(Image& layer, const Clip& c, double t, const Ctx& ctx, double sizeOverride = 0, QPoint shift = {},
                   QRect* bounds = nullptr)
    {
        const TitleStyle& st = c.title;
        const double sx = ctx.W / ctx.PW, sy = ctx.H / ctx.PH;
        const double size = std::max(1.0, sizeOverride > 0 ? sizeOverride : st.size);
        // An animated size scales the whole title: outline and box margin with it
        const double grow = st.size > 0 ? size / st.size : 1.0;
        const double outlineWidth = st.outlineWidth * grow, boxPad = std::max(0.0, st.boxPad) * grow;
        QFont font(st.fontFamily());
        font.setPixelSize(std::max(1, int(std::lround(size))));
        font.setBold(st.bold);
        font.setItalic(st.italic);
        const QFontMetricsF fm(font);
        double w = 1;
        for (const QString& line : st.text.split('\n')) w = std::max(w, fm.horizontalAdvance(line));
        const bool outline = st.outlineOn && outlineWidth > 0;
        auto layerOf = [&](bool outlineLayer) {
            Text::Params p;
            p.text = st.text;
            const double x = (ctx.PW - w) / 2 + Keys::valueAt(c, AnimParam::TitlePosX, t);
            const double y = -Keys::valueAt(c, AnimParam::TitlePosY, t);
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
        Compose::draw(onto.data(), onto.w, onto.h, px, w, h, m, opacity * fade);
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
            Timeline content;
            const Timeline* inner = &*it;
            if (c.isMulticam()) {
                const int angle = std::max(0, c.angle);
                if (angle >= it->angles.size()) return {};
                content = Multicam::angleTimeline(*it, it->angles[angle], TrackKind::Video);
                content.nested = it->nested ? it->nested : tl.nested;
                inner = &content;
            }
            src = Image(W, H);
            Timeline withNested;
            if (!inner->nested && tl.nested) {
                withNested = *inner;
                withNested.nested = tl.nested;
                inner = &withNested;
            }
            renderTimeline(*inner, in, src, ctx, lane + QString("n%1/").arg(c.sequenceId), false, depth + 1);
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
                const double scale = std::max(double(W) / nw, double(H) / nh) * zoom * 1.1;
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
            applyEffects(src, c, t, ctx, frameHeight, fit);
        }

        auto val = [&](AnimParam p) { return Keys::valueAt(c, p, t); };
        const double opacity = Keys::hasOpacity(c) ? std::clamp(val(AnimParam::Opacity) / 100.0, 0.0, 1.0) : 1.0;
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
        Compose::draw(layer.data(), W, H, src.data(), src.w, src.h, m, blurFill ? 1.0 : opacity);
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
        const QVector<TimelineOps::TransitionSpan> spans =
            TimelineOps::transitions(track, [&](const Clip& c) { return sourceLength(c, tl); });
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

    // The track's picture at f surely covers the whole frame opaquely (then nothing below is decoded). Conservative:
    // false when unsure.
    bool covers(const Track& track, int trackIndex, int f, const Ctx& ctx, const Timeline& tl, const QString& prefix)
    {
        if (track.hidden) return false;
        for (const auto& s : spansOf(track, tl))
            if (f >= s.start && f < s.end) return false;
        const Clip* c = clipAt(track, f);
        if (!c || !c->enabled || c->isTitle() || c->isNested() || c->mediaPath.isEmpty()) return false;
        if (Keys::hasCrop(*c) || Keys::hasOpacity(*c)) return false;
        // A fixed zoom/position without rotation still covers when the enlarged picture reaches past every edge
        const bool transformed = Keys::hasTransform(*c);
        if (transformed) {
            const ClipTransform& t = c->transform;
            if (t.rotation != 0 || t.hasBlurFill()) return false;
            for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY, AnimParam::Rotation})
                if (Keys::animated(*c, p)) return false;
        }
        if (TimelineOps::fadeRamp(*c, f - c->start) < 1.0) return false;
        for (const EffectInstance& e : c->effects)
            if (e.enabled && e.effectId != QLatin1String(ColorGrade::EffectId)) return false;
        const QString lane = clipLane(track, *c, prefix + QString("v%1").arg(trackIndex));
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
            for (const auto& span : spansOf(track, tl))
                if (f >= span.start && f < span.end) return false;
            if (const Clip* c = clipAt(track, f); c && c->enabled) index = i;
        }
        if (index < 0 || !covers(tl.video[index], index, f, ctx, tl, QString())) return false;
        const Clip& c = *clipAt(tl.video[index], f);
        if (Keys::hasTransform(c) || !c.effects.isEmpty()) return false;
        const QString lane = QStringLiteral("v%1").arg(index);
        const QSizeF fit = MediaAspect::fit(nativeAspect(c, lane), QSizeF(ctx.PW, ctx.PH));
        if (std::lround(fit.width() * ctx.W / ctx.PW) != ctx.W || std::lround(fit.height() * ctx.H / ctx.PH) != ctx.H)
            return false;
        const Source s = source(c.mediaPath, lane);
        if (!s.video) return false;
        const Video::Info& info = s.video->info;
        auto family = [](int cs) { return cs == 170 || cs == 470 || cs == 624 ? 601 : cs == 2021 ? 2020 : cs; };
        if (family(info.colorspace) != family(t.colorspace)) return false;
        const double file = fileFrame(c, f - c.start, *s.video);
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
        for (const auto& s : spansOf(track, tl))
            if (f >= s.start && f < s.end) {
                Image layer = trackLayer(track, trackIndex, f, ctx, tl, prefix, depth);
                if (!layer.empty()) Compose::over(canvas.data(), layer.data(), canvas.w, canvas.h, 1.0);
                return;
            }
        const Clip* c = clipAt(track, f);
        if (!c || !c->enabled) return;
        const QString lane = clipLane(track, *c, prefix + QString("v%1").arg(trackIndex));
        const int t = f - c->start;
        const double fade = TimelineOps::fadeRamp(*c, t);
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

    // luma with a gradient: b replaces a where the gradient is below the progress, over a soft band
    // (smoothstep(value, value + softness, progress * (1 + softness)))
    static void luma(Image& a, const Image& b, double progress, double softness, const std::function<double(int, int)>& value)
    {
        const double weight = progress * (1 + softness);
        uint8_t* d = a.data();
        const uint8_t* s = b.data();
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x, d += 4, s += 4) {
                const double v = value(x, y);
                double k;
                if (weight < v) k = 0;
                else if (weight >= v + softness) k = 1;
                else {
                    const double t = (weight - v) / softness;
                    k = t * t * (3 - 2 * t);
                }
                if (k <= 0) continue;
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
        const double soft = 0.02 + 0.98 * std::clamp(st.softness, 0.0, 100.0) / 100.0;
        const double progress = double(i) / len;
        if (st.isLuma()) { // both sides (one side is a fade, see trackLayer)
            const QImage grad = Lumas::image(st.luma, a.w, a.h, st.invert);
            if (grad.isNull()) {
                mix(a, b, progress);
                return a;
            }
            const QImage g8 = grad.convertToFormat(QImage::Format_Grayscale8);
            luma(a, b, progress, soft, [&](int x, int y) { return g8.constScanLine(y)[x] / 255.0; });
            return a;
        }
        const bool horizontal = type == TransitionType::WipeRight || type == TransitionType::WipeLeft;
        if (a.empty() || b.empty()) {
            // Wipe from/to nothing: the visible part of the one side shrinks/grows (hard edge)
            const bool fromStart = (type == TransitionType::WipeRight || type == TransitionType::WipeDown) == a.empty();
            Image& one = a.empty() ? b : a;
            const double p = len > 1 ? double(i) / (len - 1) : 1.0;
            const double shown = !a.empty() ? 1.0 - p : p;
            const double full = horizontal ? ctx.W : ctx.H;
            const double size = full * shown, pos = fromStart ? 0.0 : full - size;
            const uint8_t none[4] = {0, 0, 0, 0};
            if (horizontal) Compose::crop(one.data(), one.w, one.h, pos, 0, pos + std::max(size, 0.0), one.h, none);
            else Compose::crop(one.data(), one.w, one.h, 0, pos, one.w, pos + std::max(size, 0.0), none);
            wipeBorder(one, st, p, ctx);
            return std::move(one);
        }
        // Wipe between two clips: luma with a linear gradient (wipeLuma: lround(t * 255) per pixel)
        const int w = a.w, h = a.h;
        luma(a, b, progress, soft, [&](int x, int y) {
            double t = 0;
            switch (type) {
            case TransitionType::WipeRight: t = double(x) / std::max(1, w - 1); break;
            case TransitionType::WipeLeft: t = 1.0 - double(x) / std::max(1, w - 1); break;
            case TransitionType::WipeDown: t = double(y) / std::max(1, h - 1); break;
            case TransitionType::WipeUp: t = 1.0 - double(y) / std::max(1, h - 1); break;
            default: break;
            }
            return std::lround(t * 255) / 255.0;
        });
        wipeBorder(a, st, progress * (1.0 + soft) - soft / 2, ctx); // middle of luma's soft zone
        return a;
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
        bool mips = false;
        Compose::Affine m;
        double opacity = 1;
        QRectF crop;
        bool graded = false;
        ColorGrade::Params grade;
        QString gradeLutPath;
        std::shared_ptr<const ColorGrade::Lut> gradeLut;
        // Group/Mix/Luma
        std::vector<GpuOp> a, b;
        QColor fill = Qt::transparent;
        double t = 0, softness = 0;
        std::shared_ptr<std::vector<uint8_t>> gradient; // 8-bit gray, output size
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
        const Source s = source(c.mediaPath, lane);
        if (s.still) return true;
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
            const double file = fileFrame(c, t, *s.video);
            if (file < 0) return false;
            op.picture = fetchPicture(*s.video, file, ctx);
            if (!op.picture) return false;
            op.info = s.video->info;
            nw = op.picture->frame->width;
            nh = op.picture->frame->height;
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
                op.gradeLutPath = lp;
                op.gradeLut = std::move(l);
            }
        }
        if ((transform || crop) && c.transform.hasBlurFill()) return blurFillOp(c, ctx, shares, op);
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
        Image img = clipLayer(*c, f, lane, ctx, tl, depth);
        if (img.empty()) return false;
        out.push_back(cpuOp(std::move(img), k));
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
        if (styled && wipe && (!both || st.border >= 0.5)) {
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
            m.softness = 0.02 + 0.98 * std::clamp(st.softness, 0.0, 100.0) / 100.0;
            const int W = ctx.W, H = ctx.H;
            auto grad = std::make_shared<std::vector<uint8_t>>(size_t(W) * H);
            if (st.isLuma()) {
                const QImage img = Lumas::image(st.luma, W, H, st.invert);
                if (img.isNull()) {
                    m.kind = GpuOp::Mix;
                } else {
                    const QImage g8 = img.convertToFormat(QImage::Format_Grayscale8);
                    for (int y = 0; y < H; ++y) std::memcpy(grad->data() + size_t(y) * W, g8.constScanLine(y), size_t(W));
                }
            } else {
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x) {
                        double v = 0;
                        switch (type) {
                        case TransitionType::WipeRight: v = double(x) / std::max(1, W - 1); break;
                        case TransitionType::WipeLeft: v = 1.0 - double(x) / std::max(1, W - 1); break;
                        case TransitionType::WipeDown: v = double(y) / std::max(1, H - 1); break;
                        case TransitionType::WipeUp: v = 1.0 - double(y) / std::max(1, H - 1); break;
                        default: break;
                        }
                        (*grad)[size_t(y) * W + x] = uint8_t(std::lround(v * 255));
                    }
            }
            m.gradient = std::move(grad);
        }
        ops.push_back(std::move(m));
    }

    void gpuTrack(std::vector<GpuOp>& ops, const Track& track, int trackIndex, int f, const Ctx& ctx, const Timeline& tl,
                  const QString& prefix, int depth)
    {
        if (track.hidden) return;
        for (const auto& s : spansOf(track, tl))
            if (f >= s.start && f < s.end) {
                gpuTransition(ops, track, trackIndex, s, f, ctx, tl, prefix, depth);
                return;
            }
        const Clip* c = clipAt(track, f);
        if (!c || !c->enabled) return;
        const QString lane = clipLane(track, *c, prefix + QString("v%1").arg(trackIndex));
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
        Image layer = clipLayer(*c, f, lane, ctx, tl, depth);
        if (!layer.empty()) ops.push_back(cpuOp(std::move(layer), fade));
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
            QString lutPath;
            Gpu::LutPtr lut;
        };
        std::vector<Entry> entries;
        Gpu::LutPtr get(Gpu::Context& g, const GpuOp& op)
        {
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].params == op.grade && entries[i].lutPath == op.gradeLutPath) {
                    std::rotate(entries.begin(), entries.begin() + i, entries.begin() + i + 1);
                    return entries.front().lut;
                }
            Gpu::LutPtr l = g.gradeLut(*ColorGrade::bake(op.grade, op.gradeLut.get()));
            if (!l) return {};
            entries.insert(entries.begin(), {op.grade, op.gradeLutPath, l});
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
                if (op.picture) {
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
                        const QImage& img = op.still->image;
                        src = g.uploadRgba(img.constBits(), img.width(), img.height(), false);
                        for (auto e = stills.begin(); e != stills.end();) // sources closed since: textures go
                            e = e->second.first.expired() ? stills.erase(e) : std::next(e);
                        if (src) stills[op.still.get()] = {op.still, src};
                    }
                } else if (op.rgba && !op.rgba->empty()) {
                    src = g.uploadRgba(op.rgba->data(), op.rgba->w, op.rgba->h, op.premultiplied);
                }
                if (!src) break;
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
            if (yuv) yuvDone = g.readYuv(*canvas, *yuv);
            if (!yuvDone) {
                if (!rgba) return; // the card could not write the planes after all: CPU path
                g.readRgba(*canvas, rgba);
            }
            keep.textures.push_back(std::move(canvas));
            ok = true;
        });
        (void)ran; // shut down or the frame refused: the job did not run, ok stays false
        if (!ok) {
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
    ctx.preview = d->options.mode == Mode::Preview;
    ctx.bypassGrade = ctx.preview && d->options.gradeBypass && d->options.gradeBypass->load();

    // Export renders forward: decoders (and their cached pictures) of clips the export has passed go. A source asked
    // for again later (the same file further on) is opened again.
    if (d->options.mode == Mode::Export && frame % 16 == 0)
        d->dropBefore(frame - std::max<int64_t>(int64_t(2 * ctx.fps), 4 * d->options.parallelFrames + 8));
    t_frame = frame;
    t_hints = hints.nearest || hints.cancel ? &hints : nullptr;
    t_exact = true;

    if (hints.yuv && d->passThrough(int(frame), ctx, *hints.yuv)) {
        t_hints = nullptr;
        if (hints.cancel && hints.cancel->load()) return {};
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
            if (hints.cancel && hints.cancel->load()) return {};
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
    if (hints.cancel && hints.cancel->load()) return {};
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
    ctx.PW = d->options.format.width;
    ctx.PH = d->options.format.height;
    ctx.preview = d->options.mode == Mode::Preview;
    for (int i = 0; i < tl->video.size(); ++i) {
        const Track& track = tl->video[i];
        const QVector<Clip>& clips = track.clips;
        for (const Clip& c : clips) {
            if (!c.enabled || c.mediaPath.isEmpty() || c.isTitle() || c.isNested()) continue;
            const QString lane = d->clipLane(track, c, QString("v%1").arg(i));
            if (frame >= c.start - 50 && frame < c.end()) d->source(c.mediaPath, lane);
            // A clip starting soon: decode its first picture now (long-GOP material decodes up to seconds from the
            // keyframe before its in point; in the middle of playback the picture stood that long). Only when the
            // clip's decoder is not in use until then (a split without a jump continues on it, see clipLane).
            if (c.start <= frame || c.start > soon) continue;
            const bool shared = std::any_of(clips.cbegin(), clips.cend(), [&](const Clip& o) {
                return o.id != c.id && o.mediaPath == c.mediaPath && o.end() > frame && o.start < c.start
                       && d->clipLane(track, o, QString("v%1").arg(i)) == lane;
            });
            if (shared) continue;
            QSize native;
            d->mediaPicture(c, 0, lane, 0, 0, ctx, &native); // decodes, converts nothing
        }
    }
}

void Renderer::trim()
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(d->sourceMutex);
    for (auto it = d->sources.begin(); it != d->sources.end();)
        it = now - it->second.used > std::chrono::seconds(30) ? d->sources.erase(it) : std::next(it);
}

int64_t Renderer::length() const
{
    std::lock_guard lock(d->snapMutex);
    return TimelineOps::endFrame(*d->snapshot);
}

const Options& Renderer::options() const { return d->options; }

Renderer::Stats Renderer::stats() const
{
    Stats s;
    s.framesRendered = d->framesRendered;
    s.layersDecoded = d->layersDecoded;
    s.layersSkipped = d->layersSkipped;
    s.framesGpu = d->framesGpu;
    s.layersCpu = d->layersCpu;
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
