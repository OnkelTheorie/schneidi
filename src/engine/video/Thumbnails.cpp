#include "engine/video/Thumbnails.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Video {

namespace {

// One open video stream with a software decoder, and the scaler for its pictures
struct Source {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    AVStream* st = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws = nullptr;
    int swsW = 0, swsH = 0, swsOutW = 0, swsOutH = 0, swsFormat = -1;
    int turns = 0;       // quarter turns clockwise to show it upright
    double sar = 1.0;    // pixel aspect of the stored picture
    double startSeconds = 0; // container start time (the ffmpeg command line counts from it)

    ~Source()
    {
        sws_freeContext(sws);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }

    bool open(const QString& path, int threads, bool keysOnly)
    {
        const QByteArray file = path.toUtf8();
        if (avformat_open_input(&fmt, file.constData(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        // First video stream with a decoder that is no cover picture (as the preview's reader picks it)
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* s = fmt->streams[i];
            if (!st && s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(s->disposition & AV_DISPOSITION_ATTACHED_PIC)
                && avcodec_find_decoder(s->codecpar->codec_id))
                st = s;
            else
                s->discard = AVDISCARD_ALL; // the demuxer skips the other streams' packets
        }
        if (!st || st->codecpar->width <= 0 || st->codecpar->height <= 0) return false;
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) return false;
        dec->pkt_timebase = st->time_base;
        dec->thread_count = threads;
        dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (keysOnly) {
            dec->skip_frame = AVDISCARD_NONKEY;
            // A few pixels per block in the end: the deblocking filter changes nothing visible (mean 0.3 of 255 on game
            // recordings), but costs ~10 % of the time
            dec->skip_loop_filter = AVDISCARD_ALL;
            dec->flags2 |= AV_CODEC_FLAG2_FAST;
        }
        if (avcodec_open2(dec, codec, nullptr) < 0) return false;
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        AVRational r = st->sample_aspect_ratio;
        if (r.num <= 0 || r.den <= 0) r = st->codecpar->sample_aspect_ratio;
        if (r.num > 0 && r.den > 0) sar = av_q2d(r);
        // Quarter turns only, as the preview's reader (other angles stay as stored)
        const double theta = streamRotation(st);
        for (int q = 1; q < 4; ++q)
            if (std::abs(theta - 90.0 * q) < 1.0) turns = q;
        if (fmt->start_time != AV_NOPTS_VALUE) startSeconds = double(fmt->start_time) / AV_TIME_BASE;
        return true;
    }

    double seconds(const AVFrame* f) const
    {
        int64_t ts = f->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = f->pts;
        if (ts == AV_NOPTS_VALUE) return 0;
        return double(ts) * av_q2d(st->time_base) - startSeconds;
    }

    // The decoded frame upright in square pixels, fitted into `box`; pad: centered on black at exactly `box`
    QImage picture(const AVFrame* f, const QSize& box, bool pad)
    {
        if (f->width <= 0 || f->height <= 0 || box.isEmpty()) return {};
        const bool sideways = turns % 2 != 0;
        QSizeF shown(f->width * sar, f->height);
        if (sideways) shown.transpose();
        QSize fit = shown.scaled(QSizeF(box), Qt::KeepAspectRatio).toSize().boundedTo(box).expandedTo({1, 1});
        if (sideways) fit.transpose();
        if (!sws || swsW != f->width || swsH != f->height || swsFormat != f->format || swsOutW != fit.width()
            || swsOutH != fit.height()) {
            sws_freeContext(sws);
            // Bicubic as the ffmpeg command line did (bilinear looked a little softer, at no gain in speed)
            sws = sws_getContext(f->width, f->height, AVPixelFormat(f->format), fit.width(), fit.height(), AV_PIX_FMT_RGB32,
                                 SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!sws) return {};
            swsW = f->width;
            swsH = f->height;
            swsFormat = f->format;
            swsOutW = fit.width();
            swsOutH = fit.height();
            // Colors of the stream; without a statement the size decides (HD: BT.709), as the preview reads it
            int space = SWS_CS_ITU601;
            switch (f->colorspace) {
            case AVCOL_SPC_BT709: space = SWS_CS_ITU709; break;
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL: space = SWS_CS_BT2020; break;
            case AVCOL_SPC_SMPTE240M: space = SWS_CS_SMPTE240M; break;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M: break;
            default: space = f->width * f->height > 750000 ? SWS_CS_ITU709 : SWS_CS_ITU601; break;
            }
            const bool full = f->color_range == AVCOL_RANGE_JPEG;
            int* inv = nullptr;
            int* table = nullptr;
            int srcRange = 0, dstRange = 0, brightness = 0, contrast = 0, saturation = 0;
            // Only YUV sources have a matrix (the call fails harmlessly for RGB ones)
            if (sws_getColorspaceDetails(sws, &inv, &srcRange, &table, &dstRange, &brightness, &contrast, &saturation) >= 0)
                sws_setColorspaceDetails(sws, sws_getCoefficients(space), full ? 1 : 0, table, 1, brightness, contrast,
                                         saturation);
        }
        QImage img(fit, QImage::Format_RGB32); // AV_PIX_FMT_RGB32 = native 0xAARRGGBB, like QImage
        uint8_t* dst[4] = {img.bits(), nullptr, nullptr, nullptr};
        int dstStride[4] = {int(img.bytesPerLine()), 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, dstStride);
        if (turns) img = img.transformed(QTransform().rotate(90.0 * turns)); // y down: clockwise
        if (!pad || img.size() == box) return img;
        QImage out(box, QImage::Format_RGB32);
        out.fill(Qt::black);
        const int x = (box.width() - img.width()) / 2, y = (box.height() - img.height()) / 2;
        for (int row = 0; row < img.height(); ++row)
            std::memcpy(out.scanLine(y + row) + 4 * x, img.constScanLine(row), size_t(img.width()) * 4);
        return out;
    }
};

// keyPackets: only the packets marked as key frames reach the decoder (it is set to skip all other frames anyway, but
// would still parse them)
int keyFramePass(const QString& path, const KeyFrameOptions& options, bool keyPackets, bool& sawKeys,
                 const std::function<bool(double, const QImage&)>& onPicture)
{
    Source src;
    if (!src.open(path, options.threads, true)) return -1;
    int count = 0;
    bool stop = false;
    auto drain = [&] {
        while (!stop && avcodec_receive_frame(src.dec, src.frame) >= 0) {
            const QImage img = src.picture(src.frame, options.box, true);
            const double t = src.seconds(src.frame);
            av_frame_unref(src.frame);
            if (img.isNull()) continue;
            ++count;
            if (!onPicture(t, img)) stop = true;
        }
    };
    while (!stop && av_read_frame(src.fmt, src.pkt) >= 0) {
        const bool key = src.pkt->flags & AV_PKT_FLAG_KEY;
        if (src.pkt->stream_index == src.st->index && (key || !keyPackets)) {
            sawKeys = sawKeys || key;
            // EAGAIN: take pictures out first, then the packet fits
            while (avcodec_send_packet(src.dec, src.pkt) == AVERROR(EAGAIN) && !stop) drain();
            drain();
        }
        av_packet_unref(src.pkt);
    }
    if (!stop) {
        avcodec_send_packet(src.dec, nullptr);
        drain();
    }
    return count;
}

} // namespace

int keyFrames(const QString& path, const KeyFrameOptions& options, const std::function<bool(double, const QImage&)>& onPicture)
{
    bool sawKeys = false;
    const int count = keyFramePass(path, options, true, sawKeys, onPicture);
    // A container that marks no packet as a key frame: all packets to the decoder, which still skips the other frames
    if (count == 0 && !sawKeys) return keyFramePass(path, options, false, sawKeys, onPicture);
    return count;
}

QImage stillAt(const QString& path, double seconds, const QSize& box)
{
    Source src;
    if (!src.open(path, 2, false)) return {};
    if (src.fmt->duration != AV_NOPTS_VALUE && double(src.fmt->duration) / AV_TIME_BASE <= seconds) seconds = 0;
    const double tb = av_q2d(src.st->time_base);
    if (seconds > 0) {
        const int64_t target = int64_t(std::floor((seconds + src.startSeconds) / tb));
        if (av_seek_frame(src.fmt, src.st->index, target, AVSEEK_FLAG_BACKWARD) >= 0) avcodec_flush_buffers(src.dec);
    }
    QImage first;
    bool done = false;
    auto drain = [&] {
        while (!done && avcodec_receive_frame(src.dec, src.frame) >= 0) {
            const bool there = src.seconds(src.frame) >= seconds - 1e-6;
            if (there || first.isNull()) {
                const QImage img = src.picture(src.frame, box, false);
                if (there && !img.isNull()) {
                    first = img;
                    done = true;
                } else if (first.isNull()) {
                    first = img; // shown if nothing comes at or after `seconds`
                }
            }
            av_frame_unref(src.frame);
        }
    };
    while (!done && av_read_frame(src.fmt, src.pkt) >= 0) {
        if (src.pkt->stream_index == src.st->index) {
            while (avcodec_send_packet(src.dec, src.pkt) == AVERROR(EAGAIN) && !done) drain();
            drain();
        }
        av_packet_unref(src.pkt);
    }
    if (!done) {
        avcodec_send_packet(src.dec, nullptr);
        drain();
    }
    return first;
}

} // namespace Video
