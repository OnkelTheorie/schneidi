#include "engine/video/Reader.h"
#include "engine/video/Hardware.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
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

// Quarter turns MLT applies (autorotate: transpose clock / hflip+vflip / transpose cclock), 0 = none
int quarterTurns(double theta)
{
    if (std::abs(theta - 90) < 1.0) return 1;
    if (std::abs(theta - 180) < 1.0) return 2;
    if (std::abs(theta - 270) < 1.0) return 3;
    return 0;
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

// The same picture turned by quarter turns (per plane, as FFmpeg's transpose/hflip/vflip)
AVFrame* turned(const AVFrame* src, int quarters)
{
    const auto fmt = AVPixelFormat(src->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL)))
        return nullptr;
    AVFrame* dst = av_frame_alloc();
    dst->format = fmt;
    dst->width = quarters == 2 ? src->width : src->height;
    dst->height = quarters == 2 ? src->height : src->width;
    if (av_frame_get_buffer(dst, 0) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }
    av_frame_copy_props(dst, src);
    const int planes = av_pix_fmt_count_planes(fmt);
    const bool chroma = desc->nb_components >= 3 && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
    for (int p = 0; p < planes; ++p) {
        const bool sub = chroma && (p == 1 || p == 2);
        const int w = sub ? AV_CEIL_RSHIFT(src->width, desc->log2_chroma_w) : src->width;
        const int h = sub ? AV_CEIL_RSHIFT(src->height, desc->log2_chroma_h) : src->height;
        const int bpp = av_image_get_linesize(fmt, src->width, p) / std::max(1, w);
        const uint8_t* s = src->data[p];
        uint8_t* d = dst->data[p];
        const int ss = src->linesize[p], ds = dst->linesize[p];
        auto pixel = [&](int dr, int dc, int sr, int sc) {
            std::memcpy(d + ptrdiff_t(dr) * ds + ptrdiff_t(dc) * bpp, s + ptrdiff_t(sr) * ss + ptrdiff_t(sc) * bpp, bpp);
        };
        if (quarters == 2) {
            for (int r = 0; r < h; ++r)
                for (int c = 0; c < w; ++c) pixel(r, c, h - 1 - r, w - 1 - c);
        } else if (quarters == 1) { // clockwise: destination is h wide, w tall
            for (int r = 0; r < w; ++r)
                for (int c = 0; c < h; ++c) pixel(r, c, h - 1 - c, r);
        } else {
            for (int r = 0; r < w; ++r)
                for (int c = 0; c < h; ++c) pixel(r, c, c, w - 1 - r);
        }
    }
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
    std::atomic<int64_t> lastUse{0};

    // Shared, guarded by m
    struct Request {
        int64_t position;
        std::shared_ptr<const Picture> result;
        bool done = false;
    };
    mutable std::mutex m;
    std::condition_variable work, done, finishWork;
    std::deque<std::shared_ptr<const Picture>> pics; // decoded since the last seek, in order
    std::shared_ptr<const Picture> tail;             // last picture finished (also before the target)
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

    int64_t positionOf(int64_t pts) const
    {
        if (firstPts != AV_NOPTS_VALUE) pts -= firstPts;
        else if (fmt->start_time != AV_NOPTS_VALUE) pts -= fmt->start_time;
        return int64_t(av_q2d(info.timeBase) * double(pts) * fps + 0.5);
    }

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
        return false;
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
        return !ended && position - frontier() >= kSeekThreshold;
    }

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
        while (before > opt.behind && pics.size() > 1) {
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
        int64_t ts = int64_t(double(position) / (av_q2d(info.timeBase) * fps));
        if (position <= 0) ts = 0;
        else if (firstPts != AV_NOPTS_VALUE) ts += firstPts;
        else if (fmt->start_time != AV_NOPTS_VALUE) ts += fmt->start_time;
        // B-frames: start a little earlier. Index times can be decode times (MP4), which run ahead of the shown time
        // by the reorder delay: a key frame decoded before the target may be shown after it. MLT starts 2 s earlier
        // (preseek), which decodes up to two more seconds per seek – on a weak computer every catch-up jump.
        if (mustDecode && dec->has_b_frames && av_q2d(info.timeBase) != 0 && fps > 0)
            ts -= int64_t((dec->has_b_frames + 2) / fps / av_q2d(info.timeBase));
        ts = std::max<int64_t>(ts, 0);
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
            if (AVFrame* t = turned(sw, turns)) {
                av_frame_free(&sw);
                sw = t;
            }
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
            // Intra-only codecs (ProRes, DNxHR, MJPEG): frames before the target need no decoding
            if (!mustDecode && pos < target) {
                av_packet_unref(pkt);
                return;
            }
            if (pos >= target) dec->skip_loop_filter = AVDISCARD_NONE;
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
            if (pts != AV_NOPTS_VALUE) {
                const bool key = (frame->flags & AV_FRAME_FLAG_KEY) || frame->pict_type == AV_PICTURE_TYPE_I;
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
            if (d.position >= target) dec->skip_loop_filter = AVDISCARD_NONE;
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
            if (r && hibernating) {
                wake(lock);
                continue;
            }
            if (r && needsSeek(r->position)) {
                seek(lock, r->position);
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
            lock.unlock();
            bool reachedEnd = false, hwFailed = false;
            fresh.clear();
            finished.clear();
            step(fresh, reachedEnd, hwFailed, target);
            // Keep the frames from the target on; of those before it only the newest (the tail)
            Decoded before;
            std::vector<Decoded> keep;
            for (Decoded& d : fresh) {
                if (d.position < target) {
                    av_frame_free(&before.frame);
                    before = d;
                } else {
                    keep.push_back(d);
                }
            }
            if (!hardware && !hwFailed)
                for (Decoded& d : keep) finished.push_back(finish(d)); // software: no download, done here
            lock.lock();
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

Reader::Reader() : d(std::make_unique<Impl>()) {}
Reader::~Reader() = default;

std::unique_ptr<Reader> Reader::open(const QString& path, const Options& options, QString* error)
{
    std::unique_ptr<Reader> reader(new Reader);
    Impl& d = *reader->d;
    d.path = path.toUtf8();
    d.opt = options;
    if (const int r = avformat_open_input(&d.fmt, d.path.constData(), nullptr, nullptr); r < 0) {
        if (error) *error = QString("cannot open %1: %2").arg(path, errorText(r));
        return nullptr;
    }
    if (const int r = avformat_find_stream_info(d.fmt, nullptr); r < 0) {
        if (error) *error = QString("no stream info in %1: %2").arg(path, errorText(r));
        return nullptr;
    }
    // First video stream with a decoder that is no cover picture (MLT's find_default_streams)
    for (unsigned i = 0; i < d.fmt->nb_streams; ++i) {
        AVStream* st = d.fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || !avcodec_find_decoder(st->codecpar->codec_id)) continue;
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
        d.stream = st;
        d.info.index = int(i);
        break;
    }
    if (!d.stream) {
        if (error) *error = QString("no video stream in %1").arg(path);
        return nullptr;
    }
    const AVCodecParameters* par = d.stream->codecpar;
    if (par->width <= 0 || par->height <= 0 || par->format == AV_PIX_FMT_NONE) {
        if (error) *error = QString("unknown picture format in %1").arg(path);
        return nullptr;
    }
    // VP8/VP9: libvpx (it decodes the alpha channel). MLT means to switch back to FFmpeg's own decoder without alpha,
    // but opens it on the already opened context, which keeps libvpx; same pictures here.
    d.codec = avcodec_find_decoder(par->codec_id);
    if (par->codec_id == AV_CODEC_ID_VP9) {
        if (const AVCodec* c = avcodec_find_decoder_by_name("libvpx-vp9")) d.codec = c;
    } else if (par->codec_id == AV_CODEC_ID_VP8) {
        if (const AVCodec* c = avcodec_find_decoder_by_name("libvpx")) d.codec = c;
    }
    d.wantHardware = options.hardware && hardwareFriendly(par);
    if (!d.openDecoder(d.wantHardware, error)) return nullptr;
    d.pkt = av_packet_alloc();
    d.frame = av_frame_alloc();

    Info& in = d.info;
    in.width = d.dec->width;
    in.height = d.dec->height;
    in.format = AVPixelFormat(par->format);
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

std::shared_ptr<const Picture> Reader::picture(int64_t position)
{
    d->lastUse = nowMs();
    std::unique_lock lock(d->m);
    if (!d->started) {
        d->started = true;
        d->fps = av_q2d(d->info.frameRate);
        Impl* impl = d.get();
        d->thread = std::thread([impl] { impl->run(); });
    }
    // Read-ahead follows requests in order; out-of-order workers stay within a few frames
    d->sequential = d->high >= 0 && position >= d->high - 8 && position <= d->high + 2;
    if (!d->sequential || position > d->high) d->high = position;
    Impl::Request req{position, nullptr, false};
    if (!d->answer(req)) {
        d->pending.push_back(&req);
        d->work.notify_one();
        d->done.wait(lock, [&] { return req.done; });
    } else {
        d->work.notify_one(); // keep reading ahead
    }
    return req.result;
}

Reader::Stats Reader::stats() const
{
    std::lock_guard lock(d->m);
    return d->stats;
}

} // namespace Video
