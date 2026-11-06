// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/video/Scan.h"

#include "engine/Background.h"
#include "engine/OpenMedia.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Video::Scan {

namespace {

constexpr double kMinRange = 10.0; // seconds per range at least (each range pays a seek and a warm-up)

QString errorText(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

struct Input {
    AVFormatContext* fmt = nullptr;
    AVStream* stream = nullptr;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    bool flushed = false; // end of file sent to the decoder

    ~Input()
    {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }

    bool open(const QByteArray& path, int threads, bool skipNonReference, QString* error)
    {
        if (const int r = OpenMedia::input(&fmt, path.constData()); r < 0) {
            *error = QString("cannot open %1: %2").arg(QString::fromUtf8(path), errorText(r));
            return false;
        }
        if (const int r = avformat_find_stream_info(fmt, nullptr); r < 0) {
            *error = QString("no stream info: %1").arg(errorText(r));
            return false;
        }
        // First video stream with a decoder that is no cover picture (as the own reader picks it)
        for (unsigned i = 0; i < fmt->nb_streams && !stream; ++i) {
            AVStream* st = fmt->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)
                && avcodec_find_decoder(st->codecpar->codec_id))
                stream = st;
        }
        if (!stream) {
            *error = "no video stream";
            return false;
        }
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (fmt->streams[i] != stream) fmt->streams[i]->discard = AVDISCARD_ALL;
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, stream->codecpar) < 0) {
            *error = "cannot set up the decoder";
            return false;
        }
        dec->pkt_timebase = stream->time_base;
        dec->thread_count = std::max(1, threads);
        if (skipNonReference) dec->skip_frame = AVDISCARD_NONREF;
        if (const int r = avcodec_open2(dec, codec, nullptr); r < 0) {
            *error = QString("cannot open the decoder: %1").arg(errorText(r));
            return false;
        }
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        return true;
    }

    // Next decoded picture into `frame`; false at the end or on a read error
    bool next()
    {
        for (;;) {
            const int r = avcodec_receive_frame(dec, frame);
            if (r == 0) return true;
            if (r == AVERROR_EOF) return false;
            if (r != AVERROR(EAGAIN)) return false;
            if (flushed) return false;
            const int rd = av_read_frame(fmt, pkt);
            if (rd < 0) {
                avcodec_send_packet(dec, nullptr);
                flushed = true;
                continue;
            }
            if (pkt->stream_index == stream->index) avcodec_send_packet(dec, pkt); // broken packets: skipped
            av_packet_unref(pkt);
        }
    }

    void seek(int64_t ts)
    {
        av_seek_frame(fmt, stream->index, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        flushed = false;
    }
};

int64_t timestampOf(const AVFrame* f)
{
    return f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
}

// One output picture's planes
struct Buffer {
    std::vector<uint8_t> bytes;
    uint8_t* data[4] = {};
    int linesize[4] = {};
    int width = 0, height = 0;
    bool fullRange = true; // YUV/gray output: the levels of the source (video range 16..235 or full)

    void allocate(AVPixelFormat format, int w, int h)
    {
        if (w == width && h == height && !bytes.empty()) return;
        width = w;
        height = h;
        av_image_fill_linesizes(linesize, format, w);
        for (int& l : linesize) l = (l + 31) & ~31;
        size_t sizes[4] = {};
        ptrdiff_t lines[4] = {linesize[0], linesize[1], linesize[2], linesize[3]};
        av_image_fill_plane_sizes(sizes, format, h, lines);
        bytes.assign(sizes[0] + sizes[1] + sizes[2] + sizes[3] + 64, 0);
        uint8_t* p = bytes.data();
        for (int i = 0; i < 4; ++i) {
            data[i] = sizes[i] ? p : nullptr;
            p += sizes[i];
        }
    }
};

// Plane `plane` of `src` (w x h pixels of `bpp` bytes) turned by quarter turns clockwise into dst
void turnPlane(const uint8_t* src, int srcStride, int w, int h, int bpp, int quarters, uint8_t* dst, int dstStride)
{
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + size_t(y) * size_t(srcStride);
        for (int x = 0; x < w; ++x, s += bpp) {
            int dx = x, dy = y;
            if (quarters == 1) { dx = h - 1 - y; dy = x; }
            else if (quarters == 2) { dx = w - 1 - x; dy = h - 1 - y; }
            else if (quarters == 3) { dx = y; dy = w - 1 - x; }
            std::memcpy(dst + size_t(dy) * size_t(dstStride) + size_t(dx) * size_t(bpp), s, size_t(bpp));
        }
    }
}

struct Plan {
    QByteArray path;
    Options opt;
    AVRational timeBase{1, 1};
    int64_t firstPts = 0;
    int quarters = 0;              // turns clockwise to stand upright
    QRect sourceCrop;              // in the decoded (not turned) picture
    int scaledW = 0, scaledH = 0;  // before turning
    int outW = 0, outH = 0;        // upright output
    int decoderThreads = 1;
};

struct Range {
    double from = 0, to = 0;       // seconds from the first picture; to = infinity for the last
    bool first = false;
};

struct Shared {
    std::atomic<bool> cancel{false};
    std::atomic<int64_t> decoded{0};
    std::vector<std::atomic<double>> done; // per range: share finished (0..1)
    std::mutex mutex;
    QString error;
    explicit Shared(size_t n) : done(n) {}
    void fail(const QString& e)
    {
        std::lock_guard lock(mutex);
        if (error.isEmpty()) error = e;
        cancel = true;
    }
};

class Worker {
public:
    Worker(const Plan& plan, const Range& range, size_t index, const Each& each, Shared& shared)
        : m_plan(plan), m_range(range), m_index(index), m_each(each), m_shared(shared)
    {
    }
    ~Worker()
    {
        sws_freeContext(m_sws);
        av_frame_free(&m_held);
        av_frame_free(&m_cropped);
    }

    void run()
    {
        QString error;
        if (!m_in.open(m_plan.path, m_plan.decoderThreads, m_plan.opt.rate > 0 && m_plan.opt.skipNonReference, &error)) {
            m_shared.fail(error);
            return;
        }
        m_held = av_frame_alloc();
        m_cropped = av_frame_alloc();
        // A later range starts decoding a little before its start: the picture before it is its first one's
        // `previous`. The seek lands on the key frame before; when that is not early enough, from further back.
        const double need = m_plan.opt.rate > 0 ? 1.5 / m_plan.opt.rate : 0.0;
        double warm = 1.0;
        for (int attempt = 0;; ++attempt) {
            if (!m_range.first) {
                const double at = std::max(0.0, m_range.from - warm);
                m_in.seek(m_plan.firstPts + int64_t(std::floor(at / av_q2d(m_plan.timeBase))));
                if (!m_in.next()) {
                    m_shared.fail("could not read the video");
                    return;
                }
                const double t = secondsOf(m_in.frame);
                if (t > m_range.from - need && at > 0 && attempt < 4) {
                    warm *= 4;
                    continue;
                }
                m_pending = true;
            }
            break;
        }
        if (m_plan.opt.rate > 0) samples();
        else everyPicture();
        m_shared.done[m_index] = 1.0;
    }

private:
    // A picture without a timestamp (the last one of an MPEG-2 program stream) comes right after the one before: its
    // "time" was AV_NOPTS_VALUE - firstPts, an overflow, and the picture went to the first range at about -1e14 s
    double secondsOf(const AVFrame* f)
    {
        const int64_t ts = timestampOf(f);
        if (ts == AV_NOPTS_VALUE) return m_lastSeconds += f->duration > 0 ? double(f->duration) * av_q2d(m_plan.timeBase) : 0.0;
        return m_lastSeconds = double(ts - m_plan.firstPts) * av_q2d(m_plan.timeBase);
    }
    double m_lastSeconds = 0; // secondsOf's last picture

    bool nextPicture()
    {
        if (m_pending) {
            m_pending = false;
            return true;
        }
        if (m_shared.cancel || !m_in.next()) return false;
        ++m_shared.decoded;
        return true;
    }

    void report(double t)
    {
        if (std::isinf(m_range.to)) return;
        m_shared.done[m_index] = std::clamp((t - m_range.from) / (m_range.to - m_range.from), 0.0, 1.0);
    }

    // Crop, scale and turn `src` into `out`
    bool convert(const AVFrame* src, Buffer& out)
    {
        const Plan& p = m_plan;
        av_frame_unref(m_cropped);
        if (av_frame_ref(m_cropped, src) < 0) return false;
        if (!p.sourceCrop.isNull()) {
            m_cropped->crop_left = size_t(p.sourceCrop.x());
            m_cropped->crop_top = size_t(p.sourceCrop.y());
            m_cropped->crop_right = size_t(std::max(0, src->width - p.sourceCrop.x() - p.sourceCrop.width()));
            m_cropped->crop_bottom = size_t(std::max(0, src->height - p.sourceCrop.y() - p.sourceCrop.height()));
            if (av_frame_apply_cropping(m_cropped, AV_FRAME_CROP_UNALIGNED) < 0) return false;
        }
        const auto fmt = AVPixelFormat(m_cropped->format);
        m_sws = sws_getCachedContext(m_sws, m_cropped->width, m_cropped->height, fmt, p.scaledW, p.scaledH, p.opt.format,
                                     SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!m_sws) return false;
        // The picture's own matrix and range (ffmpeg's scale filter does the same), BT.601 when untagged
        const bool srcYuv = !(av_pix_fmt_desc_get(fmt)->flags & AV_PIX_FMT_FLAG_RGB);
        if (srcYuv) {
            const int cs = m_cropped->colorspace == AVCOL_SPC_BT709        ? SWS_CS_ITU709
                           : m_cropped->colorspace == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020
                           : m_cropped->colorspace == AVCOL_SPC_SMPTE240M  ? SWS_CS_SMPTE240M
                                                                           : SWS_CS_DEFAULT;
            const bool full = m_cropped->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P
                              || fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUVJ444P;
            const bool dstFull = p.opt.format == AV_PIX_FMT_RGB24 ? true : full;
            sws_setColorspaceDetails(m_sws, sws_getCoefficients(cs), full, sws_getCoefficients(cs), dstFull, 0, 1 << 16,
                                     1 << 16);
        }
        out.fullRange = true;
        if (srcYuv && p.opt.format != AV_PIX_FMT_RGB24)
            out.fullRange = m_cropped->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P
                            || fmt == AV_PIX_FMT_YUVJ422P || fmt == AV_PIX_FMT_YUVJ444P;
        Buffer& target = p.quarters ? m_turn : out;
        target.allocate(p.opt.format, p.scaledW, p.scaledH);
        sws_scale(m_sws, m_cropped->data, m_cropped->linesize, 0, m_cropped->height, target.data, target.linesize);
        if (!p.quarters) return true;
        out.allocate(p.opt.format, p.outW, p.outH);
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(p.opt.format);
        const int planes = av_pix_fmt_count_planes(p.opt.format);
        for (int i = 0; i < planes; ++i) {
            const bool chroma = i == 1 || i == 2;
            const int w = chroma ? -((-p.scaledW) >> d->log2_chroma_w) : p.scaledW;
            const int h = chroma ? -((-p.scaledH) >> d->log2_chroma_h) : p.scaledH;
            const int bpp = p.opt.format == AV_PIX_FMT_RGB24 ? 3 : 1;
            turnPlane(m_turn.data[i], m_turn.linesize[i], w, h, bpp, p.quarters, out.data[i], out.linesize[i]);
        }
        return true;
    }

    static Frame frameOf(const Buffer& b, int64_t sample, double seconds)
    {
        Frame f;
        f.sample = sample;
        f.seconds = seconds;
        for (int i = 0; i < 4; ++i) {
            f.data[i] = b.data[i];
            f.linesize[i] = b.linesize[i];
        }
        f.width = b.width;
        f.height = b.height;
        f.fullRange = b.fullRange;
        return f;
    }

    void everyPicture()
    {
        Buffer bufs[2];
        int cur = 0;
        bool havePrevious = false;
        Frame previous;
        while (nextPicture()) {
            const double t = secondsOf(m_in.frame);
            if (t >= m_range.to) break;
            if (!convert(m_in.frame, bufs[cur])) {
                m_shared.fail("could not convert a picture");
                return;
            }
            const Frame f = frameOf(bufs[cur], -1, t);
            if (t >= m_range.from || m_range.first) m_each(f, havePrevious ? &previous : nullptr);
            previous = f;
            havePrevious = true;
            cur ^= 1;
            report(t);
        }
    }

    void samples()
    {
        const double rate = m_plan.opt.rate;
        const int64_t first = m_range.first ? 0 : int64_t(std::ceil(m_range.from * rate - 1e-9));
        const int64_t end = std::isinf(m_range.to) ? std::numeric_limits<int64_t>::max()
                                                   : int64_t(std::ceil(m_range.to * rate - 1e-9));
        int64_t next = std::max<int64_t>(0, first - 1); // the sample before the range: `previous` of its first
        bool held = false, converted = false;
        Buffer bufs[2];
        int cur = 0;
        bool havePrevious = false;
        Frame previous;
        const auto deliver = [&](int64_t slot) {
            if (!converted) {
                if (!convert(m_held, bufs[cur])) return false;
                converted = true;
                const Frame f = frameOf(bufs[cur], slot, double(slot) / rate);
                if (slot >= first) m_each(f, havePrevious ? &previous : nullptr);
                previous = f;
                cur ^= 1;
            } else {
                // The same picture again (fewer pictures than samples)
                Frame f = previous;
                f.sample = slot;
                f.seconds = double(slot) / rate;
                if (slot >= first) m_each(f, havePrevious ? &previous : nullptr);
                previous = f;
            }
            havePrevious = true;
            return true;
        };
        while (nextPicture()) {
            const double t = secondsOf(m_in.frame);
            const int64_t slot = std::llround(t * rate);
            if (held)
                for (; next < slot && next < end; ++next)
                    if (!deliver(next)) return m_shared.fail("could not convert a picture");
            if (slot >= end) return;
            av_frame_unref(m_held); // the newest picture is the one for the samples up to its own
            av_frame_move_ref(m_held, m_in.frame);
            held = true;
            converted = false;
            report(t);
        }
        // End of the file: the last picture for its own sample
        if (held && !m_shared.cancel)
            for (const int64_t last = std::llround(secondsOf(m_held) * rate); next <= last && next < end; ++next)
                if (!deliver(next)) return m_shared.fail("could not convert a picture");
    }

    const Plan& m_plan;
    Range m_range;
    size_t m_index;
    const Each& m_each;
    Shared& m_shared;
    Input m_in;
    bool m_pending = false; // m_in.frame holds a picture not handed out yet
    AVFrame* m_held = nullptr;
    AVFrame* m_cropped = nullptr;
    SwsContext* m_sws = nullptr;
    Buffer m_turn;
};

} // namespace

Result run(const QString& path, const Options& options, const Each& each, const Progress& progress)
{
    Result result;
    Plan plan;
    plan.path = path.toUtf8();
    plan.opt = options;
    if (options.format != AV_PIX_FMT_YUV420P && options.format != AV_PIX_FMT_GRAY8 && options.format != AV_PIX_FMT_RGB24) {
        result.error = "unsupported output format";
        return result;
    }
    // The first picture: where time 0 is, and the picture's size
    Input probe;
    if (!probe.open(plan.path, 1, false, &result.error)) return result;
    if (!probe.next()) {
        result.error = "no picture in the video";
        return result;
    }
    plan.timeBase = probe.stream->time_base;
    plan.firstPts = timestampOf(probe.frame);
    if (plan.firstPts == AV_NOPTS_VALUE) plan.firstPts = 0;
    plan.quarters = Video::quarterTurns(Video::streamRotation(probe.stream)); // as the reader (preview, export)
    const int srcW = probe.frame->width, srcH = probe.frame->height;
    const int uprightW = plan.quarters % 2 ? srcH : srcW, uprightH = plan.quarters % 2 ? srcW : srcH;
    // Crop in the upright picture -> in the decoded one
    const QRectF& c = options.crop;
    QRect crop(0, 0, uprightW, uprightH);
    if (!c.isNull()) {
        // As ffmpeg's crop filter: truncated, and on the chroma grid of a subsampled picture
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(probe.frame->format));
        const int sx = d ? (plan.quarters % 2 ? d->log2_chroma_h : d->log2_chroma_w) : 0;
        const int sy = d ? (plan.quarters % 2 ? d->log2_chroma_w : d->log2_chroma_h) : 0;
        const auto grid = [](double v, int shift) { return std::max(0, int(v) & ~((1 << shift) - 1)); };
        crop = QRect(grid(c.x() * uprightW, sx), grid(c.y() * uprightH, sy), grid(c.width() * uprightW, sx),
                     grid(c.height() * uprightH, sy));
    }
    crop = crop.intersected(QRect(0, 0, uprightW, uprightH));
    if (crop.isEmpty()) {
        result.error = "the region is outside the picture";
        return result;
    }
    const int cx = crop.x(), cy = crop.y(), cw = crop.width(), ch = crop.height();
    switch (plan.quarters) {
    case 1: plan.sourceCrop = QRect(cy, srcH - cx - cw, ch, cw); break;
    case 2: plan.sourceCrop = QRect(srcW - cx - cw, srcH - cy - ch, cw, ch); break;
    case 3: plan.sourceCrop = QRect(srcW - cy - ch, cx, ch, cw); break;
    default: plan.sourceCrop = crop; break;
    }
    if (plan.sourceCrop == QRect(0, 0, srcW, srcH)) plan.sourceCrop = QRect();
    plan.outW = options.width > 0 ? options.width : cw;
    if (options.height > 0) plan.outH = options.height;
    else if (options.height < 0) {
        const int n = -options.height;
        plan.outH = std::max(n, int(std::lround(double(plan.outW) * ch / (double(cw) * n))) * n);
    } else plan.outH = ch;
    plan.scaledW = plan.quarters % 2 ? plan.outH : plan.outW;
    plan.scaledH = plan.quarters % 2 ? plan.outW : plan.outH;

    // Time ranges: one per core, each at least kMinRange long
    const AVFormatContext* fmt = probe.fmt;
    const double firstSec = double(plan.firstPts) * av_q2d(plan.timeBase);
    const double startSec = fmt->start_time != AV_NOPTS_VALUE ? double(fmt->start_time) / AV_TIME_BASE : 0.0;
    const double length = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE - (firstSec - startSec) : 0.0;
    const int cores = options.threads > 0 ? options.threads : Background::threads(QThread::idealThreadCount());
    const int ranges = std::clamp(int(length / kMinRange), 1, std::max(1, cores));
    plan.decoderThreads = std::max(1, cores / ranges);
    std::vector<Range> list;
    for (int i = 0; i < ranges; ++i) {
        Range r;
        r.first = i == 0;
        r.from = length * i / ranges;
        r.to = i + 1 == ranges ? std::numeric_limits<double>::infinity() : length * (i + 1) / ranges;
        list.push_back(r);
    }
    result.ranges = ranges;

    Shared shared(list.size());
    std::vector<std::thread> threads;
    std::atomic<int> running{int(list.size())};
    std::mutex wakeMutex;
    std::condition_variable wake;
    for (size_t i = 0; i < list.size(); ++i)
        threads.emplace_back([&, i] {
            Worker(plan, list[i], i, each, shared).run();
            std::lock_guard lock(wakeMutex);
            --running;
            wake.notify_all();
        });
    for (;;) {
        std::unique_lock lock(wakeMutex);
        if (wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return running == 0; })) break;
        lock.unlock();
        if (progress) {
            double sum = 0;
            for (size_t i = 0; i < list.size(); ++i) sum += shared.done[i];
            if (!progress(sum / double(list.size()))) {
                shared.cancel = true;
                result.canceled = true;
            }
        }
    }
    for (std::thread& t : threads) t.join();
    result.decoded = shared.decoded + 1;
    if (result.canceled) return result;
    result.error = shared.error;
    result.ok = result.error.isEmpty();
    if (result.ok && progress) progress(1.0);
    return result;
}

} // namespace Video::Scan
