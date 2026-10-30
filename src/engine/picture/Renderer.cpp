// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/Renderer.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Multicam.h"
#include "core/Retime.h"
#include "core/TimelineOps.h"
#include "engine/BlurFill.h"
#include "engine/ColorGrade.h"
#include "engine/Lumas.h"
#include "engine/StabilizeCorrection.h"
#include "engine/VideoEffects.h"
#include "engine/picture/Effects.h"
#include "engine/video/Compose.h"
#include "engine/video/Convert.h"
#include "engine/video/Reader.h"
#include "engine/video/Text.h"

extern "C" {
#include <libavformat/avformat.h>
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

    std::mutex lutMutex;
    std::map<QString, std::shared_ptr<const ColorGrade::Lut>> luts;

    std::atomic<int> framesRendered{0}, layersDecoded{0}, layersSkipped{0};

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
        const Source s = source(c.mediaPath, QStringLiteral("v"));
        return s.video ? c.retimedLength(s.video->length) : 0;
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
    // frameHeight: the frame's height in pixels of img (blur strength is relative to it)
    void applyEffects(Image& img, const Clip& c, double t, const Ctx& ctx, double frameHeight)
    {
        // Stabilization first (it has to see the camera picture): the analyzed correction at the clip's material
        // frame. Border "keep" has no previous output picture here: edge pixels (as a seek in the MLT filter).
        if (const auto st = StabilizeCorrection::of(c, options.format.rate, ctx.PH > 0 ? ctx.PW / ctx.PH : 0.0)) {
            Image warped(img.w, img.h);
            if (StabilizeCorrection::warpRgba(*st, int(std::floor(c.in + t + 1e-6)), img.data(), warped.data(), img.w, img.h))
                img = std::move(warped);
        }
        if (ColorGrade::active(c) && !ctx.bypassGrade) {
            const ColorGrade::Params p = ColorGrade::at(c, t);
            const QString lp = ColorGrade::lutPath(c);
            const auto l = lp.isEmpty() ? nullptr : lut(lp);
            if (!p.isNeutral() || l) ColorGrade::apply(img.data(), img.w, img.h, *ColorGrade::bake(p, l.get()), 1);
            for (const ColorGrade::Window& w : ColorGrade::windowsAt(c, t)) ColorGrade::applyWindow(img.data(), img.w, img.h, w);
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
            Image out(dw, dh);
            if (img.width() == dw && img.height() == dh) {
                std::memcpy(out.data(), img.constBits(), out.px.size());
            } else {
                Compose::Affine m;
                m.a = double(dw) / img.width();
                m.e = double(dh) / img.height();
                Compose::draw(out.data(), dw, dh, img.constBits(), img.width(), img.height(), m, 1.0);
            }
            return out;
        }
        if (!s.video) return {};
        const VideoSource& v = *s.video;
        // Material frame -> file frame (project rate) -> stream position (as the MLT producer)
        const double m = c.freeze ? c.stillFrame() : c.in + t;
        double file = m;
        if (c.isRetimed()) {
            if (c.freeze) {
                const double at = c.stillFrame() * c.speed;
                file = std::floor(c.reverse ? v.length - 1 - at + 1e-6 : at + 1e-6);
            } else if (c.hasRamp()) {
                file = std::floor(RetimeMap(c, v.length).fileFrameAt(m) + 1e-6);
            } else {
                // MLT's timewarp: the converted material is int(length / speed) frames long, frame p shows
                // p * speed, reversed (length' - 1 - p) * speed; a position between two frames rounds to the
                // nearest (slow motion: every other frame)
                const int warped = std::max(1, int(v.length / c.speed));
                file = std::floor((c.reverse ? warped - 1 - m : m) * c.speed + 0.5);
            }
            file = std::clamp(file, 0.0, double(std::max(0, v.length - 1)));
        }
        if (file < 0) return {};
        return videoFrame(v, file, dw, dh, ctx, native, into);
    }

    // Frame `file` (project rate) of a video at dw x dh (into: straight into that picture, see mediaPicture)
    static Image videoFrame(const VideoSource& v, double file, int dw, int dh, const Ctx& ctx, QSize* native,
                            uint8_t* into = nullptr)
    {
        const int64_t request = int64_t(file / ctx.fps * v.fps + 0.5);
        Video::Reader::Fetch fetch;
        if (t_hints) {
            fetch.nearest = t_hints->nearest;
            fetch.cancel = t_hints->cancel;
        }
        bool exact = true;
        const auto pic = v.reader->picture(request, fetch, &exact);
        if (!exact) t_exact = false;
        if (!pic || !pic->frame) return {};
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
        QFont font(st.fontFamily());
        font.setPixelSize(std::max(1, int(std::lround(size))));
        font.setBold(st.bold);
        font.setItalic(st.italic);
        const QFontMetricsF fm(font);
        double w = 1;
        for (const QString& line : st.text.split('\n')) w = std::max(w, fm.horizontalAdvance(line));
        const bool outline = st.outlineOn && st.outlineWidth > 0;
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
            p.pad = (st.boxOn ? std::max(0.0, st.boxPad) : 0.0) * sy;
            p.outlineColor = st.outlineColor;
            p.outline = (outlineLayer ? 2 * st.outlineWidth : 0.0) * sy;
            p.room = (outline ? 2 * st.outlineWidth : 0.0) * sy;
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

    // One clip's picture at timeline frame f on a transparent output-size layer; empty = nothing visible
    Image clipLayer(const Clip& c, int f, const QString& lane, const Ctx& ctx, const Timeline& tl, int depth)
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
                src = mediaPicture(c, t, lane, dw, dh, ctx, &native);
            }
            if (src.empty()) return {};
            layersDecoded.fetch_add(1, std::memory_order_relaxed);
        }
        if (src.empty()) return {};
        {
            // the frame's height in the picture's pixels: the picture is fit.height() project pixels tall at zoom 1
            const double frameHeight = src.h * ctx.PH / std::max(1.0, fit.height());
            applyEffects(src, c, t, ctx, frameHeight);
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
        // Second lane for the right side of a dissolve between clips of the same file
        auto laneOf = [&](bool right) { return right ? lane + 'x' : lane; };
        for (const auto& s : spans) {
            if (f < s.start || f >= s.end) continue;
            const Clip* a = s.leftId ? byId(s.leftId) : nullptr;
            const Clip* b = s.rightId ? byId(s.rightId) : nullptr;
            const bool sameFile = a && b && a->mediaPath == b->mediaPath;
            Image ia = a ? clipLayer(*a, f, laneOf(false), ctx, tl, depth) : Image();
            Image ib = b ? clipLayer(*b, f, laneOf(sameFile), ctx, tl, depth) : Image();
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

    // The track's picture at f surely covers the whole frame opaquely (then nothing below is decoded). Conservative:
    // false when unsure.
    bool covers(const Track& track, int trackIndex, int f, const Ctx& ctx, const Timeline& tl, const QString& prefix)
    {
        if (track.hidden) return false;
        for (const auto& s : spansOf(track, tl))
            if (f >= s.start && f < s.end) return false;
        const Clip* c = clipAt(track, f);
        if (!c || !c->enabled || c->isTitle() || c->isNested() || c->mediaPath.isEmpty()) return false;
        if (Keys::hasTransform(*c) || Keys::hasCrop(*c) || Keys::hasOpacity(*c)) return false;
        if (TimelineOps::fadeRamp(*c, f - c->start) < 1.0) return false;
        for (const EffectInstance& e : c->effects)
            if (e.enabled && e.effectId != QLatin1String(ColorGrade::EffectId)) return false;
        const QString lane = prefix + QString("v%1").arg(trackIndex);
        const Source s = source(c->mediaPath, lane);
        if (s.still) {
            if (!s.still->opaque) return false;
        } else if (!s.video || s.video->hasAlpha) {
            return false;
        }
        const QSizeF fit = MediaAspect::fit(nativeAspect(*c, lane), QSizeF(ctx.PW, ctx.PH));
        return std::lround(fit.width() * ctx.W / ctx.PW) >= ctx.W && std::lround(fit.height() * ctx.H / ctx.PH) >= ctx.H;
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
        const QString lane = prefix + QString("v%1").arg(trackIndex);
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

    void subtitleLayer(Image& canvas, const Timeline& tl, int f, const Ctx& ctx)
    {
        for (const SubtitleTrack& st : tl.subtitles) {
            if (!st.enabled) continue;
            for (const SubtitleCue& cue : st.cues) {
                if (f < cue.start || f >= cue.end || cue.text.trimmed().isEmpty()) continue;
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
                break;
            }
            break; // one subtitle track at a time is visible
        }
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
        const QString lane = QString("v%1").arg(i);
        const QVector<Clip>& clips = tl->video[i].clips;
        for (const Clip& c : clips) {
            if (!c.enabled || c.mediaPath.isEmpty() || c.isTitle() || c.isNested()) continue;
            if (frame >= c.start - 50 && frame < c.end()) d->source(c.mediaPath, lane);
            // A clip starting soon: decode its first picture now (long-GOP material decodes up to seconds from the
            // keyframe before its in point; in the middle of playback the picture stood that long). Only when the
            // lane's decoder of that file is not in use until then (a cut inside one recording shares it).
            if (c.start <= frame || c.start > soon) continue;
            const bool shared = std::any_of(clips.cbegin(), clips.cend(), [&](const Clip& o) {
                return o.id != c.id && o.mediaPath == c.mediaPath && o.end() > frame && o.start < c.start;
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
    std::lock_guard lock(d->sourceMutex);
    s.sourcesOpen = int(d->sources.size());
    return s;
}

void releasePools()
{
    layerPool().clear();
    pool().clear();
}

QString unsupported(const Timeline& tl)
{
    auto check = [](const Timeline& t) -> QString {
        for (const Track& track : t.video)
            for (const Clip& c : track.clips)
                for (const EffectInstance& e : c.effects) {
                    if (!e.enabled) continue;
                    const EffectDescriptor* d = EffectRegistry::find(e.effectId);
                    if (!d || d->fixed || !d->video) continue;
                    if (e.effectId == QLatin1String("color") || e.effectId == QLatin1String("blur")
                        || e.effectId == QLatin1String("chromakey") || VideoEffects::isOwn(e.effectId))
                        continue;
                    return e.effectId;
                }
        return {};
    };
    QString why = check(tl);
    if (why.isEmpty() && tl.nested)
        for (const Timeline& n : *tl.nested)
            if (why.isEmpty()) why = check(n);
    return why;
}

bool ownPicture()
{
    static const bool own = qgetenv("SCHNEIDI_PICTURE") != "mlt";
    return own;
}

} // namespace Picture
