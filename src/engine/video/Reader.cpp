// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/video/Reader.h"
#include "engine/video/Hardware.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace Video {

namespace {

constexpr int kSeekThreshold = 64;      // forward jumps up to this many frames decode on (MLT's seek_threshold)
constexpr int kFirstPtsPackets = 500;   // packets read to find the first key frame (MLT's find_first_pts)
constexpr int64_t kNothing = INT64_MAX; // validFrom before the first seek: nothing decoded is valid
// A reader not asked for this long that is not among the most recently used lets go of its decoder and pictures
// (MLT keeps 4 avformat producers open; a project with many clips would otherwise hold every decoder it ever used)
constexpr int64_t kIdleMs = 4000; // longer than Preroll looks ahead (3 s): a prepared decoder stays until it is used
constexpr int kKeepOpen = 4;
// Seek window: at most this many pictures right before the target are kept (hardware: each one is a download, they
// overlap with decoding in the finisher thread; software: they are decoded with the loop filter, which jumps skip on
// frames nothing refers to; with 32 software jumps on 1080p60 game footage looked slower)
constexpr int kHardwareWindow = 12;
constexpr int kSoftwareWindow = 16;
// Going backwards without setBehind (a reversed clip played or exported forwards): after this many seeks in a row
// that each land a little (up to kStepBack frames) before the decoded pictures, the reader keeps a window behind as
// if playing backwards, of at most kBackwardBytes (as the renderer's for playing backwards). Else every few pictures
// back decode from the key frame again: a long GOP quadratically.
constexpr int kBackSteps = 2;
constexpr int kStepBack = 64;
constexpr double kBackwardBytes = 384.0 * 1024 * 1024;

// SCHNEIDI_DECODER_IDLE_MS shortens it (tests)
int64_t idleMs()
{
    static const int64_t ms = qEnvironmentVariableIntValue("SCHNEIDI_DECODER_IDLE_MS") > 0
                                  ? qEnvironmentVariableIntValue("SCHNEIDI_DECODER_IDLE_MS")
                                  : kIdleMs;
    return ms;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Last use of every running reader (for "among the most recently used")
struct Registry {
    std::mutex m;
    std::vector<const std::atomic<int64_t>*> uses;
};

Registry& registry()
{
    static Registry r;
    return r;
}

bool amongRecent(const std::atomic<int64_t>& mine, int keep)
{
    Registry& r = registry();
    std::lock_guard lock(r.m);
    const int64_t t = mine.load();
    int newer = 0;
    for (const auto* u : r.uses) newer += u->load() > t;
    return newer < keep;
}

QString errorText(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// MLT's guess_frame_rate (producer_avformat.c); 0/1 = unknown (the producer then uses the profile's rate)
AVRational guessFrameRate(AVFormatContext* fmt, AVStream* st)
{
    AVRational rate = av_guess_frame_rate(fmt, st, nullptr);
    double fps = av_q2d(rate);
    if (std::isnan(fps) || std::isinf(fps) || fps == 0) {
        rate = st->avg_frame_rate;
        fps = av_q2d(rate);
    }
    if (av_q2d(st->r_frame_rate) >= 1000 && av_q2d(st->avg_frame_rate) > 0) {
        rate = av_d2q(av_q2d(st->avg_frame_rate), 1024);
        fps = av_q2d(rate);
    }
    if (std::isnan(fps) || std::isinf(fps) || fps < 1.0) rate = {0, 1};
    // Broadcast rates in Matroska (1 ms ticks)
    if (rate.den > 0 && fmt->iformat && fmt->iformat->name && std::strstr(fmt->iformat->name, "matroska")) {
        switch (std::lrint(100000.0 * rate.num / rate.den)) {
        case 2997003: rate = {30000, 1001}; break;
        case 5994006: rate = {60000, 1001}; break;
        case 2397602: rate = {24000, 1001}; break;
        case 4795204: rate = {48000, 1001}; break;
        default: break;
        }
    }
    // Phone slow motion: the capture rate (MLT reads the container tag as meta.attr.com.android.capture.fps)
    if (const AVDictionaryEntry* e = av_dict_get(fmt->metadata, "com.android.capture.fps", nullptr, 0)) {
        const double capture = std::atof(e->value);
        if (capture > 0.0 && std::isfinite(capture)) rate = av_d2q(capture, 1024);
    }
    return rate;
}

// MLT's get_rotation without an override: degrees clockwise, in [0, 360)
double rotationOf(const AVStream* st)
{
    double theta = 0;
    bool tagged = false;
    if (const AVDictionaryEntry* tag = av_dict_get(st->metadata, "rotate", nullptr, 0);
        tag && *tag->value && std::strcmp(tag->value, "0") != 0) {
        char* tail = nullptr;
        theta = std::strtod(tag->value, &tail);
        tagged = !*tail;
        if (!tagged) theta = 0;
    }
    if (!tagged) {
        const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                             st->codecpar->nb_coded_side_data,
                                                             AV_PKT_DATA_DISPLAYMATRIX);
        if (sd && sd->size >= 9 * sizeof(int32_t))
            theta = -av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data));
    }
    theta -= 360 * std::floor(theta / 360 + 0.9 / 360);
    return theta;
}

int mltColorspace(AVColorSpace cs, int width, int height)
{
    switch (cs) {
    case AVCOL_SPC_SMPTE240M: return 240;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return 601;
    case AVCOL_SPC_BT709: return 709;
    case AVCOL_SPC_BT2020_NCL: return 2020;
    case AVCOL_SPC_BT2020_CL: return 2021;
    default: return width * height > 750000 ? 709 : 601; // Poynton's heuristic, as MLT
    }
}

int mltPrimaries(AVColorPrimaries p)
{
    switch (p) {
    case AVCOL_PRI_BT470BG: return 601625;
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_SMPTE240M: return 601525;
    case AVCOL_PRI_BT2020: return 2020;
    default: return 709;
    }
}

// The format of a picture turned by quarter turns: the same, except sideways with chroma subsampled unevenly (4:2:2,
// 4:1:1, 4:4:0: turned it would need the transposed subsampling): its 4:4:4 counterpart (same depth, layout, range and
// alpha). AV_PIX_FMT_NONE = turned() cannot turn it (it is converted to RGBA first).
AVPixelFormat turnedFormat(AVPixelFormat fmt, int quarters)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return AV_PIX_FMT_NONE;
    if ((desc->flags & AV_PIX_FMT_FLAG_BITSTREAM) && (desc->nb_components != 1 || 8 % desc->comp[0].step != 0))
        return AV_PIX_FMT_NONE; // packed bits other than 1-bit black and white: rare
    if (quarters % 2 == 0 || desc->log2_chroma_w == desc->log2_chroma_h) return fmt;
    for (const AVPixFmtDescriptor* c = av_pix_fmt_desc_next(nullptr); c; c = av_pix_fmt_desc_next(c)) {
        if (c->log2_chroma_w || c->log2_chroma_h || c->flags != desc->flags || c->nb_components != desc->nb_components)
            continue;
        bool same = true;
        for (int i = 0; i < desc->nb_components && same; ++i) {
            const AVComponentDescriptor &x = c->comp[i], &y = desc->comp[i];
            same = x.plane == y.plane && x.step == y.step && x.offset == y.offset && x.shift == y.shift
                   && x.depth == y.depth;
        }
        if (same) return av_pix_fmt_desc_get_id(c);
    }
    return AV_PIX_FMT_NONE;
}

// The same picture turned by quarter turns (per plane, as FFmpeg's transpose/hflip/vflip), in turnedFormat() (4:2:2
// sideways: each chroma sample taken for both pixels it covers). Palette pictures keep their palette, 1-bit pictures
// are turned bit by bit. nullptr if turnedFormat() has none.
AVFrame* turned(const AVFrame* src, int quarters)
{
    const auto fmt = AVPixelFormat(src->format);
    const auto outFmt = turnedFormat(fmt, quarters);
    if (outFmt == AV_PIX_FMT_NONE) return nullptr;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    const AVPixFmtDescriptor* outDesc = av_pix_fmt_desc_get(outFmt);
    AVFrame* dst = av_frame_alloc();
    dst->format = outFmt;
    dst->width = quarters == 2 ? src->width : src->height;
    dst->height = quarters == 2 ? src->height : src->width;
    if (av_frame_get_buffer(dst, 0) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }
    av_frame_copy_props(dst, src);
    const int W = src->width, H = src->height;
    const int planes = av_pix_fmt_count_planes(fmt);
    const bool chroma = desc->nb_components >= 3 && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
    const bool bits = desc->flags & AV_PIX_FMT_FLAG_BITSTREAM;
    for (int p = 0; p < planes; ++p) {
        const bool sub = chroma && (p == 1 || p == 2);
        // Subsampling of this plane in the source and in the result (shifts)
        const int sx = sub ? desc->log2_chroma_w : 0, sy = sub ? desc->log2_chroma_h : 0;
        const int dx = sub ? outDesc->log2_chroma_w : 0, dy = sub ? outDesc->log2_chroma_h : 0;
        const int w = AV_CEIL_RSHIFT(dst->width, dx), h = AV_CEIL_RSHIFT(dst->height, dy); // result plane
        const int bpp = bits ? 0 : av_image_get_linesize(fmt, W, p) / std::max(1, AV_CEIL_RSHIFT(W, sx));
        const uint8_t* s = src->data[p];
        uint8_t* d = dst->data[p];
        const int ss = src->linesize[p], ds = dst->linesize[p];
        // Result plane (r, c) -> its full-size pixel -> the source's full-size pixel -> the source plane's sample
        auto from = [&](int r, int c, int& sr, int& sc) {
            const int R = r << dy, C = c << dx;
            if (quarters == 2) {
                sr = H - 1 - R;
                sc = W - 1 - C;
            } else if (quarters == 1) { // clockwise: the result is H wide, W tall
                sr = H - 1 - C;
                sc = R;
            } else {
                sr = C;
                sc = W - 1 - R;
            }
            sr >>= sy;
            sc >>= sx;
        };
        if (bits) {
            const int n = desc->comp[0].step, mask = (1 << n) - 1; // bits per pixel, most significant first
            for (int r = 0; r < h; ++r) {
                uint8_t* row = d + ptrdiff_t(r) * ds;
                std::memset(row, 0, ds);
                for (int c = 0; c < w; ++c) {
                    int sr, sc;
                    from(r, c, sr, sc);
                    const int sb = sc * n, db = c * n;
                    const int v = (s[ptrdiff_t(sr) * ss + sb / 8] >> (8 - n - sb % 8)) & mask;
                    row[db / 8] |= uint8_t(v << (8 - n - db % 8));
                }
            }
            continue;
        }
        auto pixel = [&](int dr, int dc, int sr, int sc) {
            std::memcpy(d + ptrdiff_t(dr) * ds + ptrdiff_t(dc) * bpp, s + ptrdiff_t(sr) * ss + ptrdiff_t(sc) * bpp, bpp);
        };
        if (sx != dx || sy != dy) { // 4:2:2 sideways -> 4:4:4
            for (int r = 0; r < h; ++r)
                for (int c = 0; c < w; ++c) {
                    int sr, sc;
                    from(r, c, sr, sc);
                    pixel(r, c, sr, sc);
                }
            continue;
        }
        // The same subsampling (the usual case: 4:2:0 phone video): plane by plane, source plane size sw x sh
        const int sw = AV_CEIL_RSHIFT(W, sx), sh = AV_CEIL_RSHIFT(H, sy);
        if (quarters == 2) {
            for (int r = 0; r < sh; ++r)
                for (int c = 0; c < sw; ++c) pixel(r, c, sh - 1 - r, sw - 1 - c);
        } else if (quarters == 1) { // clockwise: destination is sh wide, sw tall
            for (int r = 0; r < sw; ++r)
                for (int c = 0; c < sh; ++c) pixel(r, c, sh - 1 - c, r);
        } else {
            for (int r = 0; r < sw; ++r)
                for (int c = 0; c < sh; ++c) pixel(r, c, c, sw - 1 - r);
        }
    }
    if (desc->flags & AV_PIX_FMT_FLAG_PAL) std::memcpy(dst->data[1], src->data[1], AVPALETTE_SIZE);
    return dst;
}

// The picture in RGBA (formats turned() cannot turn); nullptr on failure
AVFrame* rgba(const AVFrame* src)
{
    SwsContext* sws = sws_getContext(src->width, src->height, AVPixelFormat(src->format), src->width, src->height,
                                     AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
    if (!sws) return nullptr;
    AVFrame* dst = av_frame_alloc();
    dst->format = AV_PIX_FMT_RGBA;
    dst->width = src->width;
    dst->height = src->height;
    if (av_frame_get_buffer(dst, 0) < 0
        || sws_scale(sws, src->data, src->linesize, 0, src->height, dst->data, dst->linesize) <= 0)
        av_frame_free(&dst);
    else
        av_frame_copy_props(dst, src);
    sws_freeContext(sws);
    return dst;
}

// Row loops of planar() (this file builds with -O3: vectorized)
void splitChroma(const uint8_t* __restrict uv, uint8_t* __restrict u, uint8_t* __restrict v, int n)
{
    for (int x = 0; x < n; ++x) {
        u[x] = uv[2 * x];
        v[x] = uv[2 * x + 1];
    }
}

void splitChroma10(const uint16_t* __restrict uv, uint16_t* __restrict u, uint16_t* __restrict v, int n)
{
    for (int x = 0; x < n; ++x) {
        u[x] = uv[2 * x] >> 6;
        v[x] = uv[2 * x + 1] >> 6;
    }
}

void shiftDown(const uint16_t* __restrict s, uint16_t* __restrict d, int n)
{
    for (int x = 0; x < n; ++x) d[x] = s[x] >> 6;
}

// Hardware picture (after the transfer: NV12 or P010) into the stream's planar format (yuv420p / yuvj420p / yuv420p10le).
// NV12/P010 carry the same samples as the software decoder's output (P010: 10 bits in the high bits). NV12's luma plane
// is taken over as it is (no copy), only the interleaved chroma is split.
AVFrame* planar(AVFrame* src, AVPixelFormat target)
{
    const bool tenBit = target == AV_PIX_FMT_YUV420P10LE;
    if (src->format != (tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12)) return nullptr; // VAAPI may differ: software then
    const int w = src->width, h = src->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    const int bytes = tenBit ? 2 : 1;
    AVFrame* dst = av_frame_alloc();
    dst->format = target;
    dst->width = w;
    dst->height = h;
    av_frame_copy_props(dst, src);
    int first = 0; // first plane to allocate
    if (!tenBit && src->buf[0]) {
        dst->buf[0] = av_buffer_ref(src->buf[0]);
        dst->data[0] = src->data[0];
        dst->linesize[0] = src->linesize[0];
        first = 1;
    }
    for (int p = first; p < 3; ++p) {
        const int pw = p ? cw : w, ph = p ? ch : h;
        const int stride = FFALIGN(pw * bytes, 64);
        dst->buf[p] = av_buffer_alloc(size_t(stride) * ph + 64);
        if (!dst->buf[p]) {
            av_frame_free(&dst);
            return nullptr;
        }
        dst->data[p] = dst->buf[p]->data;
        dst->linesize[p] = stride;
    }
    if (!tenBit) {
        for (int y = 0; y < ch; ++y)
            splitChroma(src->data[1] + ptrdiff_t(y) * src->linesize[1], dst->data[1] + ptrdiff_t(y) * dst->linesize[1],
                        dst->data[2] + ptrdiff_t(y) * dst->linesize[2], cw);
    } else {
        for (int y = 0; y < h; ++y)
            shiftDown(reinterpret_cast<const uint16_t*>(src->data[0] + ptrdiff_t(y) * src->linesize[0]),
                      reinterpret_cast<uint16_t*>(dst->data[0] + ptrdiff_t(y) * dst->linesize[0]), w);
        for (int y = 0; y < ch; ++y)
            splitChroma10(reinterpret_cast<const uint16_t*>(src->data[1] + ptrdiff_t(y) * src->linesize[1]),
                          reinterpret_cast<uint16_t*>(dst->data[1] + ptrdiff_t(y) * dst->linesize[1]),
                          reinterpret_cast<uint16_t*>(dst->data[2] + ptrdiff_t(y) * dst->linesize[2]), cw);
    }
    return dst;
}

bool hardwareFriendly(const AVCodecParameters* par)
{
    switch (par->codec_id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_HEVC:
    case AV_CODEC_ID_VP9:
    case AV_CODEC_ID_AV1: break;
    default: return false;
    }
    // Small pictures decode fast enough in software (no upload/download, no GPU memory per decoder)
    if (par->width * par->height < 1280 * 720) return false;
    const auto f = AVPixelFormat(par->format);
    return f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUV420P10LE;
}

// Errors MLT keeps decoding after (start of an MPEG file, B-frames before the first I-frame)
bool ignorable(int r)
{
    return r >= 0 || r == AVERROR(EAGAIN) || r == AVERROR_EOF || r == AVERROR_INVALIDDATA || r == AVERROR(EINVAL);
}

} // namespace

Picture::~Picture()
{
    av_frame_free(&frame);
}

struct Reader::Impl {
    // A frame out of the decoder, not yet a software picture (hardware frames are downloaded by the finisher thread)
    struct Decoded {
        AVFrame* frame = nullptr; // own reference
        int64_t position = 0;
        bool progressive = true, topFieldFirst = false;
        uint64_t generation = 0;
        bool tailOnly = false;    // only the last picture before the target (answers requests past the end)
        bool key = false;         // a key frame (the seek window starts at one)
    };
    static constexpr int kQueue = 3; // hardware frames waiting for the download (each holds a surface)

    // Fixed after open
    QByteArray path;
    Options opt;
    Info info;
    AVFormatContext* fmt = nullptr;
    AVStream* stream = nullptr;
    const AVCodec* codec = nullptr;
    int turns = 0;
    double fps = 0; // source frame rate as a number
    bool mustDecode = true;
    AVPixelFormat planarFormat = AV_PIX_FMT_NONE; // hardware pictures are converted to this

    // Decoder thread only
    AVCodecContext* dec = nullptr;
    Hardware::Device hw;
    bool hwRefused = false; // the decoder offered no hardware format (profile): software from now on
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int64_t firstPts = AV_NOPTS_VALUE;
    bool firstPtsProbed = false;
    int invalidPts = 0, invalidDts = 0;
    bool draining = false;
    int64_t lastPacketPos = INT64_MIN;
    int64_t lastFramePos = INT64_MIN;
    bool wantHardware = false; // decode on the card (until it fails once)
    bool hibernating = false;  // decoder closed after a long pause (see kIdleMs)
    // Seeks that land past the target: files without an index (MPEG-TS/PS) seek by bytes and land on any packet, the
    // decoder then drops everything up to the next key frame. The seek is repeated this much earlier (stream time
    // base), doubled until it lands before the target; kept for the next seeks (learnt once per file).
    int64_t preroll = 0;
    int64_t seekTs = 0;              // where the last seek aimed
    bool seekChecked = true;         // the first packet/picture after the last seek came at or before the target
    int64_t firstPacketPos = INT64_MIN; // position of the first video packet read since the last seek
    std::atomic<int64_t> lastUse{0};

    // Shared, guarded by m
    struct Request {
        int64_t position;
        std::shared_ptr<const Picture> result;
        bool done = false;
        bool nearest = false; // Fetch::nearest
        bool exact = true;    // answered by the picture at the position (not a close one)
    };
    mutable std::mutex m;
    std::condition_variable work, done, finishWork;
    std::deque<std::shared_ptr<const Picture>> pics; // decoded since the last seek, in order
    std::shared_ptr<const Picture> tail;             // last picture finished (also before the target)
    uint64_t tailGeneration = 0;                     // the generation `tail` was decoded in
    bool keeping = false;    // since the last seek: pictures before the target are kept from the first key frame on
    bool keyStarted = false; // decoder thread: that key frame came
    int keepFrames = 0;      // pictures Options::keepBytes holds (0 = not computed yet)
    int64_t seekTarget = -1; // the request the last seek was for
    int64_t keepReach = 0;   // decoder thread: pictures this close before the target are kept (exact decoding)
    Decoded rawTail;                                 // newest frame before the target, not finished yet
    std::deque<Decoded> queue;                       // for the finisher thread
    int64_t queuedBack = INT64_MIN;                  // position of the last frame queued
    int busy = 0;                                    // frames the finisher is working on
    uint64_t generation = 0;                         // +1 at every seek: queued frames of before are dropped
    bool finishFailed = false;                       // a download failed: software from now on
    int64_t validFrom = kNothing;                    // requests from here on can be answered by pics
    bool ended = false;                              // the decoder gave its last frame
    bool stop = false;
    bool started = false;
    std::list<Request*> pending;
    int64_t high = -1;       // highest position asked for recently (read-ahead reference)
    bool sequential = false; // requests come in order (playback, export)
    Stats stats;
    std::thread thread, finisher;

    ~Impl()
    {
        {
            std::lock_guard lock(m);
            stop = true;
        }
        work.notify_all();
        finishWork.notify_all();
        if (thread.joinable()) thread.join();
        if (finisher.joinable()) finisher.join();
        {
            Registry& r = registry();
            std::lock_guard lock(r.m);
            std::erase(r.uses, &lastUse);
        }
        clearQueued();
        pics.clear();
        tail.reset();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        av_buffer_unref(&hw.context);
        avformat_close_input(&fmt);
    }

    // Under the lock (or after the threads ended)
    void clearQueued()
    {
        for (Decoded& q : queue) av_frame_free(&q.frame);
        queue.clear();
        av_frame_free(&rawTail.frame);
        queuedBack = INT64_MIN;
    }

    static AVPixelFormat getFormat(AVCodecContext* ctx, const AVPixelFormat* fmts)
    {
        auto* self = static_cast<Impl*>(ctx->opaque);
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == self->hw.pixFmt) return *p;
        self->hwRefused = true;
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return AV_PIX_FMT_NONE;
    }

    bool openDecoder(bool hardware, QString* error)
    {
        avcodec_free_context(&dec);
        av_buffer_unref(&hw.context);
        hw = {};
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, stream->codecpar) < 0) {
            if (error) *error = QStringLiteral("decoder context");
            return false;
        }
        dec->thread_count = opt.threads;
        if (hardware) hw = Hardware::deviceFor(codec);
        if (hw.context) {
            dec->hw_device_ctx = av_buffer_ref(hw.context);
            dec->opaque = this;
            dec->get_format = getFormat;
            dec->thread_count = 1;            // FFmpeg offers no CUDA decoding with frame threads
            dec->extra_hw_frames = kQueue + 3; // queued, being downloaded, the tail
        }
        if (const int r = avcodec_open2(dec, codec, nullptr); r < 0) {
            if (hw.context) return openDecoder(false, error); // driver refused this stream
            if (error) *error = QString("cannot open decoder %1: %2").arg(codec->name, errorText(r));
            return false;
        }
        return true;
    }

    int64_t bestPts(int64_t pts, int64_t dts)
    {
        invalidPts += pts == AV_NOPTS_VALUE;
        invalidDts += dts == AV_NOPTS_VALUE;
        if ((invalidPts <= invalidDts || dts == AV_NOPTS_VALUE) && pts != AV_NOPTS_VALUE) return pts;
        return dts;
    }

    int64_t positionOf(int64_t pts) const { return framePosition(pts, firstPts, fmt->start_time, info.timeBase, fps); }

    // MLT's find_first_pts: timestamp of the first key frame (frame 0)
    void findFirstPts()
    {
        firstPtsProbed = true;
        for (int n = 0; n < kFirstPtsPackets && firstPts == AV_NOPTS_VALUE; ++n) {
            if (av_read_frame(fmt, pkt) < 0) break;
            if (pkt->stream_index == info.index && (pkt->flags & AV_PKT_FLAG_KEY)) {
                // Negative DTS (MP4 with B-frames): the first PTS would be the third frame's -> assume 0
                firstPts = pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0 ? 0 : bestPts(pkt->pts, pkt->dts);
            }
            av_packet_unref(pkt);
        }
        av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    }

    // Under the lock: the decoder ended and every frame is finished
    bool atEnd() const { return ended && queue.empty() && busy == 0 && !rawTail.frame; }

    // Under the lock: can this position be answered now?
    bool answer(Request& r)
    {
        if (r.position >= validFrom && validFrom != kNothing) {
            const auto it = std::lower_bound(pics.begin(), pics.end(), r.position,
                                             [](const auto& p, int64_t pos) { return p->position < pos; });
            if (it != pics.end()) {
                r.result = *it;
                r.done = true;
                return true;
            }
            if (atEnd()) { // past the end: the last picture (MLT: last good frame)
                r.result = pics.empty() ? tail : pics.back();
                r.done = true;
                return true;
            }
        }
        if (stop) {
            r.done = true;
            return true;
        }
        if (r.nearest && !needsSeek(r.position)) {
            // The decoder is on its way there (or past it): the newest picture of this generation before the position
            std::shared_ptr<const Picture> best;
            if (validFrom != kNothing && r.position >= validFrom) {
                auto it = std::lower_bound(pics.begin(), pics.end(), r.position,
                                           [](const auto& p, int64_t pos) { return p->position < pos; });
                if (it != pics.begin()) best = *std::prev(it);
            }
            if (!best && tail && tailGeneration == generation && tail->position <= r.position) best = tail;
            if (best) {
                r.result = best;
                r.exact = false;
                r.done = true;
                ++stats.nearest;
                return true;
            }
        }
        return false;
    }

    // Under the lock: is a request waiting that a close picture would answer?
    bool nearestWanted() const
    {
        for (const Request* r : pending)
            if (r->nearest) return true;
        return false;
    }

    // Pictures the seek window keeps (Options::keepBytes), from the stream's size
    int seekWindow()
    {
        if (opt.keepBytes <= 0) return 0;
        if (keepFrames == 0) {
            const int bytes = av_image_get_buffer_size(info.format, std::max(1, info.width), std::max(1, info.height), 1);
            keepFrames = std::max(1, int(opt.keepBytes / std::max(1, bytes)));
        }
        return keepFrames;
    }

    void fulfil()
    {
        bool any = false;
        for (auto it = pending.begin(); it != pending.end();) {
            if (answer(**it)) {
                it = pending.erase(it);
                any = true;
            } else {
                ++it;
            }
        }
        if (any) done.notify_all();
    }

    Request* firstPending() const
    {
        Request* best = nullptr;
        for (Request* r : pending)
            if (!best || r->position < best->position) best = r;
        return best;
    }

    int64_t frontier() const
    {
        int64_t f = validFrom == kNothing ? INT64_MIN : validFrom - 1;
        if (!pics.empty()) f = std::max(f, pics.back()->position);
        return std::max(f, queuedBack);
    }

    bool needsSeek(int64_t position) const
    {
        if (validFrom == kNothing || position < validFrom) return true;
        if (position <= windowEnd) return false; // a backward window being decoded up to there
        return !ended && position - frontier() >= kSeekThreshold;
    }

    // Where decoding after a seek for `position` keeps pictures from: the position itself, or with a large window
    // behind (setBehind: playing backwards) and a request before the decoded ones that many pictures earlier, so the
    // next requests going back are answered without seeking again
    int64_t seekStart(int64_t position) const
    {
        constexpr int kWindowFrom = 16;
        const bool back = validFrom == kNothing || position < validFrom;
        if (behind() <= kWindowFrom || !back) return position;
        return std::max<int64_t>(0, position - (behind() - 2));
    }
    int64_t windowEnd = -1; // the request a backward window was started for (seekStart)

    // Window behind found by going backwards (kBackSteps), 0 = none; decoder thread, under the lock
    int autoBehind = 0;
    int backSteps = 0;       // seeks in a row a little before the decoded pictures
    int64_t backTarget = -1; // the request of the last one
    int behind() const { return std::max(opt.behind, autoBehind); }

    // Under the lock, before seeking for `want`: going backwards in small steps -> a window behind; else none
    void noteSeek(int64_t want, bool nearest)
    {
        // Scrubbing back (close pictures will do) keeps its quick seeks
        const bool stepBack = !nearest && validFrom != kNothing && want < validFrom && validFrom - want <= kStepBack;
        backSteps = stepBack ? backSteps + 1 : 0;
        if (!stepBack) {
            autoBehind = 0;
            return;
        }
        backTarget = want;
        if (backSteps >= kBackSteps && autoBehind == 0) {
            const int bytes = av_image_get_buffer_size(info.format, std::max(1, info.width), std::max(1, info.height), 1);
            autoBehind = std::clamp(int(kBackwardBytes / std::max(1, bytes)), kKeepWindowMin, 250);
            ++stats.backwardWindows;
        }
    }
    static constexpr int kKeepWindowMin = 17; // more than seekStart's kWindowFrom: a window is decoded

    bool wantAhead() const
    {
        if (ended || validFrom == kNothing || high < 0) return false;
        const int target = sequential ? opt.ahead : 1;
        int ahead = 0;
        for (auto it = pics.rbegin(); it != pics.rend() && (*it)->position > high; ++it) ++ahead;
        for (const Decoded& q : queue) ahead += q.position > high;
        return ahead < target;
    }

    // Drops pictures well before the last requests (they stay answerable only from validFrom on)
    void evict()
    {
        int64_t ref = high;
        for (const Request* r : pending) ref = std::min(ref, r->position);
        int before = 0;
        for (const auto& p : pics)
            if (p->position < ref) ++before;
        // The seek window is for steps and jumps back after a seek: once requests went on further than it holds
        // (playing on), the usual few
        const int window = seekWindow();
        const bool nearSeek = window > 0 && seekTarget >= 0 && high >= 0 && high - seekTarget < window;
        const int limit = nearSeek ? std::max(behind(), window) : behind();
        while (before > limit && pics.size() > 1) {
            validFrom = std::max(validFrom, pics.front()->position + 1);
            pics.pop_front();
            --before;
        }
    }

    // Under the lock: forget everything decoded (seek, fallback to software)
    void restart(int64_t from)
    {
        ++generation;
        clearQueued();
        pics.clear();
        validFrom = from;
        ended = false;
    }

    // Decoder thread, lock held on entry and exit
    void seek(std::unique_lock<std::mutex>& lock, int64_t position)
    {
        restart(position);
        ++stats.seeks;
        lock.unlock();
        int64_t ts = position <= 0 ? 0 : frameTimestamp(position, firstPts, fmt->start_time, info.timeBase, fps);
        // B-frames: start a little earlier. Index times can be decode times (MP4), which run ahead of the shown time
        // by the reorder delay: a key frame decoded before the target may be shown after it. MLT starts 2 s earlier
        // (preseek), which decodes up to two more seconds per seek – on a weak computer every catch-up jump.
        if (mustDecode && dec->has_b_frames && av_q2d(info.timeBase) != 0 && fps > 0)
            ts -= int64_t((dec->has_b_frames + 2) / fps / av_q2d(info.timeBase));
        ts = std::max<int64_t>(ts - preroll, 0);
        seekTs = ts;
        seekChecked = false;
        firstPacketPos = INT64_MIN;
        // Pictures before the target are only a way there (no loop filter on frames nothing refers to) - except the
        // last ones the seek window keeps: they are decoded exactly (step())
        keeping = opt.keepBytes > 0 && mustDecode;
        keyStarted = false;
        keepReach = !keeping ? 0 : hw.context ? kHardwareWindow : std::min(kSoftwareWindow, seekWindow());
        dec->skip_loop_filter = AVDISCARD_NONREF;
        av_seek_frame(fmt, info.index, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        draining = false;
        lastPacketPos = INT64_MIN;
        lock.lock();
    }

    // The software picture of a decoded frame (download + planar for hardware frames, rotation); takes the frame.
    // nullptr if a download failed. Any thread, lock not held.
    std::shared_ptr<const Picture> finish(Decoded& d) const
    {
        AVFrame* sw = d.frame;
        d.frame = nullptr;
        if (sw->hw_frames_ctx) {
            AVFrame* down = av_frame_alloc();
            AVFrame* out = nullptr;
            if (av_hwframe_transfer_data(down, sw, 0) >= 0) {
                av_frame_copy_props(down, sw);
                down->width = sw->width;
                down->height = sw->height;
                out = planar(down, planarFormat);
            }
            av_frame_free(&down);
            av_frame_free(&sw);
            if (!out) return nullptr;
            sw = out;
        }
        if (turns) {
            // Never the picture unturned (squeezed into the upright size): what turned() cannot turn in RGBA
            if (turnedFormat(AVPixelFormat(sw->format), turns) == AV_PIX_FMT_NONE) {
                AVFrame* c = rgba(sw);
                av_frame_free(&sw);
                if (!c) return nullptr;
                sw = c;
            }
            AVFrame* t = turned(sw, turns);
            av_frame_free(&sw);
            if (!t) return nullptr;
            sw = t;
        }
        auto pic = std::make_shared<Picture>();
        pic->frame = sw;
        pic->position = d.position;
        pic->progressive = d.progressive;
        pic->topFieldFirst = d.topFieldFirst;
        return pic;
    }

    // Under the lock: a finished picture into the list (in order) and as the tail
    void place(const std::shared_ptr<const Picture>& pic, bool tailOnly)
    {
        tail = pic;
        tailGeneration = generation;
        if (!tailOnly && pic->position >= validFrom && (pics.empty() || pic->position > pics.back()->position))
            pics.push_back(pic);
    }

    // Decoder thread: reads one packet, decodes it (lock not held)
    void step(std::vector<Decoded>& out, bool& reachedEnd, bool& hwFailed, int64_t target)
    {
        AVPacket* send = pkt;
        if (!draining) {
            const int r = av_read_frame(fmt, pkt);
            if (r == AVERROR(EAGAIN)) return;
            if (r < 0) {
                draining = true;
                send = nullptr;
            } else if (pkt->stream_index != info.index) {
                av_packet_unref(pkt);
                return;
            }
        } else {
            send = nullptr;
        }
        if (send) {
            int64_t pos = INT64_MIN;
            if (const int64_t pts = bestPts(pkt->pts, pkt->dts); pts != AV_NOPTS_VALUE) {
                if (firstPts == AV_NOPTS_VALUE && !(fmt->pb && fmt->pb->seekable)) firstPts = pts;
                pos = positionOf(pts);
                if (pos == lastPacketPos) pos = lastPacketPos + 1;
            }
            if (pos != INT64_MIN && std::llabs(target - pos) > 999) pos = target; // wild timestamps (MLT)
            lastPacketPos = pos;
            if (firstPacketPos == INT64_MIN) firstPacketPos = pos;
            // Intra-only codecs (ProRes, DNxHR, MJPEG): frames before the target need no decoding
            if (!mustDecode && pos < target) {
                av_packet_unref(pkt);
                return;
            }
            if (pos >= target - keepReach) dec->skip_loop_filter = AVDISCARD_NONE;
        }
        for (;;) {
            const int r = avcodec_send_packet(dec, send);
            if (r == AVERROR(EAGAIN)) {
                if (!receive(out, reachedEnd, target)) break;
                continue;
            }
            if (!ignorable(r) && hw.context) hwFailed = true;
            break;
        }
        if (send) av_packet_unref(pkt);
        receive(out, reachedEnd, target);
        // The decoder offered no hardware format for this stream (profile): software, with threads
        if (hwRefused && hw.context) hwFailed = true;
    }

    // Returns false when nothing came out
    bool receive(std::vector<Decoded>& out, bool& reachedEnd, int64_t target)
    {
        bool got = false;
        for (;;) {
            const int r = avcodec_receive_frame(dec, frame);
            if (r == AVERROR_EOF) {
                reachedEnd = true;
                return got;
            }
            if (r < 0) {
                // EAGAIN after the drain packet: the decoder has nothing left
                if (draining && r == AVERROR(EAGAIN)) reachedEnd = true;
                return got;
            }
            got = true;
            Decoded d;
            const int64_t pts = bestPts(frame->pts, frame->pkt_dts);
            d.key = (frame->flags & AV_FRAME_FLAG_KEY) || frame->pict_type == AV_PICTURE_TYPE_I;
            if (pts != AV_NOPTS_VALUE) {
                const bool key = d.key;
                if (firstPts == AV_NOPTS_VALUE && key) firstPts = pts;
                d.position = positionOf(pts);
            } else {
                d.position = lastFramePos != INT64_MIN ? lastFramePos + 1 : target;
            }
            lastFramePos = d.position;
            const AVCodecParameters* par = stream->codecpar;
            d.progressive = !(frame->flags & AV_FRAME_FLAG_INTERLACED)
                            && (par->field_order == AV_FIELD_PROGRESSIVE || par->field_order == AV_FIELD_UNKNOWN);
            d.topFieldFirst = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) || par->field_order == AV_FIELD_TT
                              || par->field_order == AV_FIELD_TB;
            if (d.position >= target - keepReach) dec->skip_loop_filter = AVDISCARD_NONE;
            d.frame = av_frame_clone(frame);
            av_frame_unref(frame);
            if (d.frame) out.push_back(d);
        }
    }

    // Decoder thread: back to software decoding for good; every request seeks anew. Lock held.
    void fallBack(std::unique_lock<std::mutex>& lock)
    {
        ++stats.hardwareFailures;
        wantHardware = false;
        restart(kNothing);
        lock.unlock();
        QString ignored;
        openDecoder(false, &ignored);
        lock.lock();
        stats.hardware = false;
    }

    // Decoder thread, lock held: close the decoder and drop every picture (opened again on the next request)
    void hibernate(std::unique_lock<std::mutex>& lock)
    {
        restart(kNothing);
        tail.reset();
        hibernating = true;
        ++stats.hibernations;
        lock.unlock();
        avcodec_free_context(&dec);
        av_buffer_unref(&hw.context);
        hw = {};
        lock.lock();
    }

    void wake(std::unique_lock<std::mutex>& lock)
    {
        lock.unlock();
        QString ignored;
        hwRefused = false;
        const bool ok = openDecoder(wantHardware, &ignored);
        lock.lock();
        hibernating = false;
        if (!ok) stop = true; // cannot happen (it opened before); answer every request with nothing
    }

    void finishLoop()
    {
        std::unique_lock lock(m);
        while (!stop) {
            if (queue.empty()) {
                finishWork.wait(lock);
                continue;
            }
            Decoded d = queue.front();
            queue.pop_front();
            ++busy;
            lock.unlock();
            work.notify_all(); // room in the queue
            const std::shared_ptr<const Picture> pic = finish(d);
            lock.lock();
            --busy;
            if (d.generation == generation) {
                if (!pic) finishFailed = true;
                else place(pic, d.tailOnly);
            }
            evict();
            fulfil();
            work.notify_all();
        }
    }

    void run()
    {
        {
            Registry& r = registry();
            std::lock_guard g(r.m);
            r.uses.push_back(&lastUse);
        }
        std::unique_lock lock(m);
        if (!firstPtsProbed) {
            lock.unlock();
            findFirstPts();
            lock.lock();
        }
        std::vector<Decoded> fresh;
        std::vector<std::shared_ptr<const Picture>> finished;
        while (!stop) {
            if (finishFailed) {
                finishFailed = false;
                fallBack(lock);
                continue;
            }
            fulfil();
            const Request* r = firstPending();
            // Going forwards again (beyond the last step back): the usual few behind
            if (autoBehind && high > backTarget + kSeekThreshold) autoBehind = 0;
            if (r && hibernating) {
                wake(lock);
                continue;
            }
            if (r && needsSeek(r->position)) {
                // r lives on the caller's stack: seek() unlocks, a cancelled caller may be gone when it returns
                const int64_t want = r->position;
                noteSeek(want, r->nearest);
                const int64_t from = seekStart(want);
                seekTarget = want;
                seek(lock, from);
                windowEnd = from < want ? want : -1;
                continue;
            }
            // Nothing asked, or the downloads lag behind: wait (the finisher wakes us). Long unused: close.
            if (!r && !hibernating && busy == 0 && queue.empty() && nowMs() - lastUse.load() > idleMs()
                && !amongRecent(lastUse, kKeepOpen)) {
                hibernate(lock);
                continue;
            }
            if ((!r && (hibernating || !wantAhead())) || int(queue.size()) >= kQueue || (ended && !r)) {
                work.wait_for(lock, std::chrono::milliseconds(std::min<int64_t>(2000, idleMs())));
                continue;
            }
            if (ended) { // the request waits for the last frames being finished
                work.wait(lock);
                continue;
            }
            const int64_t target = validFrom;
            const uint64_t gen = generation;
            const bool hardware = hw.context != nullptr;
            const bool keepAll = keeping;
            // A scrubbing request waits: the newest picture before the target answers it (hardware: only the first
            // after the seek, each one is a download)
            const bool nearTail = nearestWanted() && (!hardware || !(tail && tailGeneration == generation));
            lock.unlock();
            bool reachedEnd = false, hwFailed = false;
            fresh.clear();
            finished.clear();
            step(fresh, reachedEnd, hwFailed, target);
            // Keep the frames from the target on (seek window: from the first key frame on); of those before it only
            // the newest (the tail)
            Decoded before;
            std::vector<Decoded> keep;
            int64_t keptFrom = kNothing;
            // Hardware: only the last few before the target (downloads); software: as many as the window holds
            const int64_t reach = keepReach;
            for (Decoded& d : fresh) {
                if (d.position < target && keepAll && d.key) keyStarted = true;
                if (d.position < target && keepAll && keyStarted && target - d.position <= reach) {
                    keptFrom = std::min(keptFrom, d.position);
                    keep.push_back(d);
                } else if (d.position < target) {
                    av_frame_free(&before.frame);
                    before = d;
                } else {
                    keep.push_back(d);
                }
            }
            std::shared_ptr<const Picture> nearPic;
            if (!hardware && !hwFailed) {
                for (Decoded& d : keep) finished.push_back(finish(d)); // software: no download, done here
                if (nearTail && before.frame) {
                    Decoded copy = before;
                    copy.frame = av_frame_clone(before.frame);
                    if (copy.frame) nearPic = finish(copy);
                }
            }
            lock.lock();
            if (gen == generation && !seekChecked && !hwFailed) {
                // Intra-only codecs skip the packets before the target: the first packet tells; others: the first
                // picture (the decoder drops what comes before a key frame)
                const int64_t first = !mustDecode ? firstPacketPos : !fresh.empty() ? fresh.front().position : INT64_MIN;
                if (first != INT64_MIN || reachedEnd) {
                    // Past the target, or the end with nothing at all: earlier, unless the seek went to the start
                    if ((first == INT64_MIN || first > target) && seekTs > 0) {
                        for (Decoded& d : keep) av_frame_free(&d.frame);
                        av_frame_free(&before.frame);
                        const double second = av_q2d(info.timeBase) > 0 ? 1.0 / av_q2d(info.timeBase) : 0;
                        preroll = std::max<int64_t>(preroll * 2, int64_t(second));
                        ++stats.reseeks;
                        seek(lock, target);
                        continue;
                    }
                    seekChecked = true;
                }
            }
            if (gen == generation && keptFrom < validFrom) validFrom = keptFrom;
            if (nearPic && gen == generation) {
                place(nearPic, true);
                fulfil();
            } else if (hardware && nearTail && before.frame && gen == generation && !hwFailed) {
                Decoded copy = before;
                copy.frame = av_frame_clone(before.frame);
                copy.tailOnly = true;
                copy.generation = gen;
                if (copy.frame) {
                    queue.push_back(copy);
                    if (!finisher.joinable()) finisher = std::thread([this] { finishLoop(); });
                    finishWork.notify_one();
                }
            }
            stats.decoded += int(fresh.size());
            stats.hardware = hardware && !hwFailed;
            if (hwFailed || gen != generation) {
                for (Decoded& d : keep) av_frame_free(&d.frame);
                av_frame_free(&before.frame);
                if (hwFailed) fallBack(lock);
                continue;
            }
            if (before.frame) {
                av_frame_free(&rawTail.frame);
                rawTail = before;
            }
            if (hardware) {
                for (Decoded& d : keep) {
                    d.generation = gen;
                    queuedBack = std::max(queuedBack, d.position);
                    queue.push_back(d);
                }
                if (!keep.empty()) av_frame_free(&rawTail.frame); // a later picture is the tail now
                if (!finisher.joinable()) finisher = std::thread([this] { finishLoop(); });
                finishWork.notify_one();
            } else {
                for (const auto& p : finished) {
                    if (p) place(p, false);
                }
                if (!finished.empty()) av_frame_free(&rawTail.frame);
            }
            if (reachedEnd) {
                ended = true;
                // Nothing at or after the target: the newest frame before it answers (past the end)
                if (rawTail.frame) {
                    Decoded t = rawTail;
                    rawTail = {};
                    t.tailOnly = true;
                    t.generation = gen;
                    if (hardware) {
                        queue.push_back(t);
                        finishWork.notify_one();
                    } else if (pics.empty()) {
                        if (auto p = finish(t)) place(p, true);
                    } else {
                        av_frame_free(&t.frame);
                    }
                }
            }
            evict();
        }
        // Wake everyone still waiting
        for (Request* q : pending) q->done = true;
        pending.clear();
        done.notify_all();
    }
};

double streamRotation(const AVStream* stream)
{
    return rotationOf(stream);
}

int quarterTurns(double theta)
{
    if (std::abs(theta - 90) < 1.0) return 1;
    if (std::abs(theta - 180) < 1.0) return 2;
    if (std::abs(theta - 270) < 1.0) return 3;
    return 0;
}

namespace {

// The zero point in stream time base: first key frame's timestamp, else the container start converted (it is in µs)
int64_t zeroOf(int64_t firstPts, int64_t startTime, AVRational timeBase)
{
    if (firstPts != AV_NOPTS_VALUE) return firstPts;
    if (startTime != AV_NOPTS_VALUE && timeBase.num > 0 && timeBase.den > 0)
        return av_rescale_q(startTime, AV_TIME_BASE_Q, timeBase);
    return 0;
}

} // namespace

int64_t framePosition(int64_t pts, int64_t firstPts, int64_t startTime, AVRational timeBase, double fps)
{
    pts -= zeroOf(firstPts, startTime, timeBase);
    return int64_t(av_q2d(timeBase) * double(pts) * fps + 0.5);
}

int64_t frameTimestamp(int64_t position, int64_t firstPts, int64_t startTime, AVRational timeBase, double fps)
{
    const double step = av_q2d(timeBase) * fps;
    const int64_t ts = step > 0 ? int64_t(double(position) / step) : 0;
    return ts + zeroOf(firstPts, startTime, timeBase);
}

std::optional<double> zeroOffset(const QString& path)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) return std::nullopt;
    const std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> guard(fmt, [](AVFormatContext* f) {
        avformat_close_input(&f);
    });
    if (avformat_find_stream_info(fmt, nullptr) < 0) return std::nullopt;
    int index = -1; // the stream Reader::open picks
    for (unsigned i = 0; i < fmt->nb_streams && index < 0; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && avcodec_find_decoder(st->codecpar->codec_id)
            && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
            index = int(i);
    }
    if (index < 0) return std::nullopt;
    // As the reader's findFirstPts
    int64_t first = AV_NOPTS_VALUE;
    AVPacket* pkt = av_packet_alloc();
    for (int n = 0; pkt && n < kFirstPtsPackets && first == AV_NOPTS_VALUE; ++n) {
        if (av_read_frame(fmt, pkt) < 0) break;
        if (pkt->stream_index == index && (pkt->flags & AV_PKT_FLAG_KEY))
            first = pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0 ? 0 : (pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    const AVRational tb = fmt->streams[index]->time_base;
    const int64_t zeroUs = av_rescale_q(zeroOf(first, fmt->start_time, tb), tb, AV_TIME_BASE_Q);
    const int64_t start = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
    return double(zeroUs - start) / AV_TIME_BASE;
}

Reader::Reader() : d(std::make_unique<Impl>()) {}
Reader::~Reader() = default;

std::unique_ptr<Reader> Reader::open(const QString& path, const Options& options, QString* error)
{
    std::unique_ptr<Reader> reader(new Reader);
    Impl& d = *reader->d;
    d.path = path.toUtf8();
    d.opt = options;
    // Opens the file and picks the first video stream with a decoder that is no cover picture (MLT's
    // find_default_streams). analyze = seconds the stream analysis reads at most (0 = FFmpeg's default, 5 s).
    auto openStream = [&](int analyze) {
        avformat_close_input(&d.fmt);
        d.stream = nullptr;
        AVDictionary* o = nullptr;
        if (analyze > 0) {
            av_dict_set_int(&o, "analyzeduration", int64_t(analyze) * AV_TIME_BASE, 0);
            av_dict_set_int(&o, "probesize", int64_t(analyze) * 2 * 1024 * 1024, 0);
        }
        const int opened = avformat_open_input(&d.fmt, d.path.constData(), nullptr, &o);
        av_dict_free(&o);
        if (opened < 0) {
            if (error) *error = QString("cannot open %1: %2").arg(path, errorText(opened));
            return false;
        }
        if (const int r = avformat_find_stream_info(d.fmt, nullptr); r < 0) {
            if (error) *error = QString("no stream info in %1: %2").arg(path, errorText(r));
            return false;
        }
        for (unsigned i = 0; i < d.fmt->nb_streams; ++i) {
            AVStream* st = d.fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || !avcodec_find_decoder(st->codecpar->codec_id)) continue;
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
            d.stream = st;
            d.info.index = int(i);
            break;
        }
        if (!d.stream && error) *error = QString("no video stream in %1").arg(path);
        return d.stream != nullptr;
    };
    auto described = [&] {
        const AVCodecParameters* p = d.stream->codecpar;
        return p->width > 0 && p->height > 0 && p->format != AV_PIX_FMT_NONE;
    };
    if (!openStream(0)) return nullptr;
    // Video that starts late (a recording whose picture comes after seconds of sound): the analysis ended before its
    // first frame. Once more, reading further (only such files pay for it).
    if (!described() && !openStream(60)) return nullptr;
    if (!described()) {
        if (error) *error = QString("unknown picture format in %1").arg(path);
        return nullptr;
    }
    const AVCodecParameters* par = d.stream->codecpar;
    // A WebM with alpha: the container states yuv420p (the alpha is a side stream, tagged alpha_mode=1), only libvpx
    // decodes it (yuva420p pictures). The format says so, else the picture counted as opaque (covering the tracks
    // below, uploaded to the card without alpha); no hardware decoder, it drops the alpha. Without alpha FFmpeg's own
    // decoders: they recover after a damaged key frame (libvpx gives nothing until the next one) and can decode on the
    // card.
    d.codec = avcodec_find_decoder(par->codec_id);
    const AVDictionaryEntry* alphaMode = av_dict_get(d.stream->metadata, "alpha_mode", nullptr, 0);
    bool alpha = false;
    if ((par->codec_id == AV_CODEC_ID_VP9 || par->codec_id == AV_CODEC_ID_VP8) && alphaMode
        && std::strcmp(alphaMode->value, "1") == 0 && AVPixelFormat(par->format) == AV_PIX_FMT_YUV420P) {
        if (const AVCodec* c = avcodec_find_decoder_by_name(par->codec_id == AV_CODEC_ID_VP9 ? "libvpx-vp9" : "libvpx")) {
            d.codec = c;
            alpha = true;
        }
    }
    d.wantHardware = options.hardware && !alpha && hardwareFriendly(par);
    if (!d.openDecoder(d.wantHardware, error)) return nullptr;
    d.pkt = av_packet_alloc();
    d.frame = av_frame_alloc();

    Info& in = d.info;
    in.width = d.dec->width;
    in.height = d.dec->height;
    in.format = alpha ? AV_PIX_FMT_YUVA420P : AVPixelFormat(par->format);
    in.frameRate = guessFrameRate(d.fmt, d.stream);
    in.timeBase = d.stream->time_base;
    AVRational sar = d.stream->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = par->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = {1, 1};
    in.sar = sar;
    in.rotation = rotationOf(d.stream);
    d.turns = quarterTurns(in.rotation);
    in.colorspace = mltColorspace(d.dec->colorspace, d.dec->width, d.dec->height);
    in.colorTrc = d.dec->color_trc;
    in.colorPrimaries = mltPrimaries(d.dec->color_primaries);
    in.fullRange = d.dec->color_range == AVCOL_RANGE_JPEG;
    in.hasBFrames = d.dec->has_b_frames > 0;
    in.duration = d.fmt->duration == AV_NOPTS_VALUE ? -1 : d.fmt->duration;
    in.codec = QString::fromLatin1(d.codec->name);
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    d.mustDecode = !desc || !(desc->props & AV_CODEC_PROP_INTRA_ONLY);
    d.planarFormat = in.format;
    // Turned pictures can come in another format (4:2:2 sideways as 4:4:4; RGBA what cannot be turned as it is)
    if (d.turns) {
        const AVPixelFormat t = turnedFormat(in.format, d.turns);
        in.format = t != AV_PIX_FMT_NONE ? t : turnedFormat(AV_PIX_FMT_RGBA, d.turns);
    }
    return reader;
}

void Reader::setFrameRate(AVRational rate)
{
    d->info.frameRate = rate;
}

const Info& Reader::info() const
{
    return d->info;
}

const AVFormatContext* Reader::container() const
{
    return d->fmt;
}

void Reader::setBehind(int pictures)
{
    std::lock_guard lock(d->m);
    d->opt.behind = std::max(1, pictures);
}

std::shared_ptr<const Picture> Reader::picture(int64_t position)
{
    return picture(position, Fetch{});
}

std::shared_ptr<const Picture> Reader::picture(int64_t position, const Fetch& fetch, bool* exact)
{
    d->lastUse = nowMs();
    std::unique_lock lock(d->m);
    if (!d->started) {
        d->started = true;
        d->fps = av_q2d(d->info.frameRate);
        Impl* impl = d.get();
        d->thread = std::thread([impl] { impl->run(); });
    }
    if (fetch.cancel && fetch.cancel->load()) return nullptr; // before it moves the read-ahead's target
    // Read-ahead follows requests in order; out-of-order workers stay within a few frames
    d->sequential = d->high >= 0 && position >= d->high - 8 && position <= d->high + 2;
    if (!d->sequential || position > d->high) d->high = position;
    Impl::Request req{position, nullptr, false};
    req.nearest = fetch.nearest;
    if (!d->answer(req)) {
        d->pending.push_back(&req);
        d->work.notify_one();
        if (!fetch.cancel) {
            d->done.wait(lock, [&] { return req.done; });
        } else {
            // Nobody signals the cancel flag: look at it now and then
            while (!req.done) {
                if (fetch.cancel->load()) {
                    d->pending.remove(&req);
                    ++d->stats.cancelled;
                    // Nothing else asked: stop decoding towards it (the next request sets the reference again)
                    if (d->pending.empty()) d->high = -1;
                    d->work.notify_one();
                    return nullptr;
                }
                d->done.wait_for(lock, std::chrono::milliseconds(4));
            }
        }
    } else {
        d->work.notify_one(); // keep reading ahead
    }
    if (exact) *exact = req.exact;
    return req.result;
}

Reader::Stats Reader::stats() const
{
    std::lock_guard lock(d->m);
    return d->stats;
}

} // namespace Video
