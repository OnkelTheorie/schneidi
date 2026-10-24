#include "engine/ExportWriter.h"

#include "engine/OutputColor.h"

#include <Mlt.h>
#include <QSettings>
#include <framework/mlt.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace {

// MLT image format the render workers deliver for an encoder pixel format (as MLT's consumer asks for it: the Exporter
// sets yuv420p for 4:2:0, MLT itself picks yuv420p10/yuv444p10, everything else comes as packed yuv422) and whether
// the image goes to the encoder as it is
struct ImagePlan {
    mlt_image_format mlt;
    bool direct;
};

std::optional<ImagePlan> imagePlan(AVPixelFormat f)
{
    switch (f) {
    case AV_PIX_FMT_YUV420P: return ImagePlan{mlt_image_yuv420p, true};
    case AV_PIX_FMT_YUV420P10LE: return ImagePlan{mlt_image_yuv420p10, true};
    case AV_PIX_FMT_YUV444P10LE: return ImagePlan{mlt_image_yuv444p10, true};
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV422P10LE: return ImagePlan{mlt_image_yuv422, false};
    default: return std::nullopt;
    }
}

// MLT sound format for an encoder sample format: always interleaved, the encode thread splits planar formats
mlt_audio_format mltAudioFormat(AVSampleFormat f)
{
    switch (av_get_packed_sample_fmt(f)) {
    case AV_SAMPLE_FMT_S16: return mlt_audio_s16;
    case AV_SAMPLE_FMT_S32: return mlt_audio_s32le;
    case AV_SAMPLE_FMT_FLT: return mlt_audio_f32le;
    case AV_SAMPLE_FMT_U8: return mlt_audio_u8;
    default: return mlt_audio_none;
    }
}

// Color tags of the file from the profile's MLT color standard, like MLT's consumer (color_trc_from_colorspace,
// color_primaries_from_colorspace, add_video_stream)
AVColorTransferCharacteristic transferOf(int colorspace)
{
    switch (colorspace) {
    case 709: return AVCOL_TRC_BT709;
    case 470: return AVCOL_TRC_GAMMA28;
    case 240: return AVCOL_TRC_SMPTE240M;
    case 0: return AVCOL_TRC_IEC61966_2_1;
    case 601:
    case 170: return AVCOL_TRC_SMPTE170M;
    case 2020: return AVCOL_TRC_BT2020_10;
    default: return AVCOL_TRC_UNSPECIFIED;
    }
}

AVColorPrimaries primariesOf(int colorspace, int height)
{
    switch (colorspace) {
    case 0:
    case 709: return AVCOL_PRI_BT709;
    case 470: return AVCOL_PRI_BT470M;
    case 240: return AVCOL_PRI_SMPTE240M;
    case 601: return height == 576 ? AVCOL_PRI_BT470BG : AVCOL_PRI_SMPTE170M;
    case 170: return AVCOL_PRI_SMPTE170M;
    case 2020: return AVCOL_PRI_BT2020;
    default: return AVCOL_PRI_UNSPECIFIED;
    }
}

AVColorSpace matrixOf(int colorspace, int height)
{
    switch (colorspace) {
    case 170: return AVCOL_SPC_SMPTE170M;
    case 240: return AVCOL_SPC_SMPTE240M;
    case 470: return AVCOL_SPC_BT470BG;
    case 601: return (576 % height) ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT470BG;
    case 709: return AVCOL_SPC_BT709;
    case 2020: return AVCOL_SPC_BT2020_NCL;
    case 2021: return AVCOL_SPC_BT2020_CL;
    default: return AVCOL_SPC_UNSPECIFIED;
    }
}

// The matrix MLT's consumer converts with (mlt_set_luma_transfer): swscale only knows 601, 240 and 709, everything
// else counts as its default (601). Frames differ from the output only when these differ (OutputColor converts the
// usual cases in the workers already).
int swscaleMatrix(int colorspace)
{
    return colorspace == 709 || colorspace == 240 ? colorspace : 601;
}

// An option like MLT's apply_properties: AVOption of the context or its codec (children first); "vb"/"ab" style names
// also without their prefix. Unknown names are left out, as MLT does (consumer properties that are no option).
void applyOption(void* ctx, const QByteArray& key, const QByteArray& value, int flags)
{
    const char* name = key.constData();
    const AVOption* o = av_opt_find(ctx, name, nullptr, flags, AV_OPT_SEARCH_CHILDREN);
    if (!o && ((name[0] == 'v' && (flags & AV_OPT_FLAG_VIDEO_PARAM)) || (name[0] == 'a' && (flags & AV_OPT_FLAG_AUDIO_PARAM))))
        o = av_opt_find(ctx, ++name, nullptr, flags, AV_OPT_SEARCH_CHILDREN);
    if (o && av_opt_set(ctx, name, value.constData(), AV_OPT_SEARCH_CHILDREN) < 0)
        qWarning("Export: option %s=%s not accepted", key.constData(), value.constData());
}

QString avError(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

bool isMainMuxer(const AVOutputFormat* f)
{
    if (!f) return false;
    for (const char* name : {"mp4", "mov", "matroska", "webm"})
        if (std::strcmp(f->name, name) == 0) return true;
    return false;
}

// Packed Y0 Cb Y1 Cr (MLT yuv422) -> planar 4:2:2, 8 or 10 bit (10 bit = code value * 4, like swscale's point
// conversion)
void unpack422(const uint8_t* src, AVFrame* dst, int width, int height)
{
    const bool ten = dst->format == AV_PIX_FMT_YUV422P10LE;
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + size_t(y) * width * 2;
        if (ten) {
            auto* py = reinterpret_cast<uint16_t*>(dst->data[0] + size_t(y) * dst->linesize[0]);
            auto* pu = reinterpret_cast<uint16_t*>(dst->data[1] + size_t(y) * dst->linesize[1]);
            auto* pv = reinterpret_cast<uint16_t*>(dst->data[2] + size_t(y) * dst->linesize[2]);
            for (int x = 0; x < width / 2; ++x, s += 4) {
                py[2 * x] = uint16_t(s[0] << 2);
                pu[x] = uint16_t(s[1] << 2);
                py[2 * x + 1] = uint16_t(s[2] << 2);
                pv[x] = uint16_t(s[3] << 2);
            }
        } else {
            uint8_t* py = dst->data[0] + size_t(y) * dst->linesize[0];
            uint8_t* pu = dst->data[1] + size_t(y) * dst->linesize[1];
            uint8_t* pv = dst->data[2] + size_t(y) * dst->linesize[2];
            for (int x = 0; x < width / 2; ++x, s += 4) {
                py[2 * x] = s[0];
                pu[x] = s[1];
                py[2 * x + 1] = s[2];
                pv[x] = s[3];
            }
        }
    }
}

void releaseFrame(void* opaque, uint8_t*)
{
    mlt_frame_close(static_cast<mlt_frame>(opaque));
}

} // namespace

bool ExportWriter::wanted()
{
    QString choice = qEnvironmentVariable("SCHNEIDI_EXPORT").trimmed().toLower();
    if (choice.isEmpty()) choice = QSettings().value("render/writer").toString().trimmed().toLower();
    return choice != QLatin1String("mlt");
}

bool ExportWriter::supports(const QString& path, const QString& videoCodec, const QString& pixFmt, const QString& audioCodec,
                            const QString& sampleFormat)
{
    if (!isMainMuxer(av_guess_format(nullptr, path.toUtf8().constData(), nullptr))) return false;
    const AVCodec* video = avcodec_find_encoder_by_name(videoCodec.toUtf8().constData());
    if (!video || video->type != AVMEDIA_TYPE_VIDEO || !imagePlan(av_get_pix_fmt(pixFmt.toUtf8().constData())))
        return false;
    if (audioCodec.isEmpty()) return true;
    const AVCodec* audio = avcodec_find_encoder_by_name(audioCodec.toUtf8().constData());
    return audio && audio->type == AVMEDIA_TYPE_AUDIO
           && mltAudioFormat(av_get_sample_fmt(sampleFormat.toUtf8().constData())) != mlt_audio_none;
}

struct ExportWriter::Impl {
    Settings s;
    mlt_consumer consumer = nullptr;
    int width = 0, height = 0, colorspace = 709;
    AVRational rate{25, 1}, aspect{1, 1};
    double fps = 25;
    AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
    ImagePlan plan{mlt_image_yuv420p, true};
    AVSampleFormat sampleFmt = AV_SAMPLE_FMT_NONE;
    mlt_audio_format audioFmt = mlt_audio_none;
    int sampleBytes = 0;

    // fetch thread -> encode thread
    struct Item {
        AVFrame* picture = nullptr;
        std::vector<uint8_t> pcm; // interleaved, encoder sample type
        bool end = false;
    };
    std::mutex mutex;
    std::condition_variable cond;
    std::deque<Item> queue;
    size_t queueMax = 4;

    std::thread fetcher, encoder;
    std::atomic<bool> stop{false};
    std::atomic<int> written{0};
    std::atomic<State> state{State::Running};
    std::atomic<bool> encoderBroke{false};
    mutable std::mutex errorMutex;
    QString error;
    bool started = false, consumerStarted = false;

    // encode thread only
    AVFormatContext* oc = nullptr;
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    AVStream* vst = nullptr;
    AVStream* ast = nullptr;
    AVPacket* packet = nullptr;
    std::vector<uint8_t> fifo; // sound waiting for a full encoder frame (interleaved)
    int64_t audioPts = 0;
    int audioFrameSize = 1024;

    static int isStopped(mlt_consumer c) { return static_cast<Impl*>(c->child)->consumerStarted ? 0 : 1; }

    void fail(const QString& message, bool encoderProblem = false)
    {
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (error.isEmpty()) error = message;
        }
        if (encoderProblem) encoderBroke = true;
        state = State::Failed;
        stop = true;
        cond.notify_all();
    }

    bool push(Item item)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [this] { return stop || queue.size() < queueMax; });
        if (stop) {
            av_frame_free(&item.picture);
            return false;
        }
        queue.push_back(std::move(item));
        cond.notify_all();
        return true;
    }

    // The frame's image as an AVFrame for the encoder: the MLT buffer itself when it fits, else a converted copy
    AVFrame* picture(mlt_frame frame, uint8_t* image)
    {
        mlt_properties fp = MLT_FRAME_PROPERTIES(frame);
        const int from = mlt_properties_get_int(fp, "colorspace");
        const bool fullRange = mlt_properties_get_int(fp, "full_range");
        const bool otherMatrix = swscaleMatrix(from) != swscaleMatrix(colorspace);
        AVFrame* f = av_frame_alloc();
        if (!f) return nullptr;
        f->format = pixFmt;
        f->width = width;
        f->height = height;
        if (plan.direct && !otherMatrix && !fullRange) {
            const int size = mlt_image_format_size(plan.mlt, width, height, nullptr);
            mlt_properties_inc_ref(fp); // released with the buffer (by the encoder, possibly in its own threads)
            f->buf[0] = av_buffer_create(image, size, releaseFrame, frame, AV_BUFFER_FLAG_READONLY);
            if (!f->buf[0]) {
                mlt_frame_close(frame);
                av_frame_free(&f);
                return nullptr;
            }
            int strides[4] = {};
            mlt_image_format_planes(plan.mlt, width, height, image, f->data, strides);
            for (int i = 0; i < 4; ++i) f->linesize[i] = strides[i];
            return f;
        }
        // Rare: what MLT's encoder thread converted with swscale. Matrix and range on a copy in MLT's layout first.
        std::vector<uint8_t> copy, converted;
        const uint8_t* src = image;
        if (otherMatrix || fullRange) {
            const int size = mlt_image_format_size(plan.mlt, width, height, nullptr);
            copy.assign(image, image + size);
            if (fullRange) OutputColor::toLimitedRange(copy.data(), plan.mlt, width, height);
            src = copy.data();
            converted.resize(copy.size());
            if (otherMatrix && OutputColor::convert(copy.data(), converted.data(), plan.mlt, width, height,
                                                    swscaleMatrix(from), swscaleMatrix(colorspace)))
                src = converted.data();
        }
        if (av_frame_get_buffer(f, 0) < 0) {
            av_frame_free(&f);
            return nullptr;
        }
        if (plan.mlt == mlt_image_yuv422) {
            unpack422(src, f, width, height);
        } else {
            uint8_t* planes[4] = {};
            int strides[4] = {};
            mlt_image_format_planes(plan.mlt, width, height, const_cast<uint8_t*>(src), planes, strides);
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt);
            for (int i = 0; i < 3; ++i) {
                const int h = i == 0 ? height : AV_CEIL_RSHIFT(height, desc->log2_chroma_h);
                av_image_copy_plane(f->data[i], f->linesize[i], planes[i], strides[i], strides[i], h);
            }
        }
        return f;
    }

    void fetch()
    {
        bool warnedSound = false;
        for (int i = 0; i < s.length && !stop; ++i) {
            mlt_frame frame = mlt_consumer_rt_frame(consumer);
            if (stop) {
                if (frame) mlt_frame_close(frame);
                break;
            }
            if (!frame) {
                fail(QStringLiteral("no frame %1 from the timeline").arg(i));
                break;
            }
            Item item;
            mlt_image_format format = plan.mlt;
            int w = width, h = height;
            uint8_t* image = nullptr;
            if (mlt_frame_get_image(frame, &image, &format, &w, &h, 0) || !image || format != plan.mlt || w != width
                || h != height) {
                mlt_frame_close(frame);
                fail(QStringLiteral("frame %1: no image in %2 %3x%4 (got %5 %6x%7)")
                         .arg(i)
                         .arg(mlt_image_format_name(plan.mlt))
                         .arg(width)
                         .arg(height)
                         .arg(mlt_image_format_name(format))
                         .arg(w)
                         .arg(h));
                break;
            }
            item.picture = picture(frame, image);
            if (!item.picture) {
                mlt_frame_close(frame);
                fail(QStringLiteral("out of memory"));
                break;
            }
            item.picture->pts = i;
            if (audioFmt != mlt_audio_none) {
                // Same count as MLT's consumer and its workers (from the first exported frame): the sound is cached in
                // the frame already when the workers fetched it ahead
                int samples = mlt_audio_calculate_frame_samples(float(fps), s.audioRate, i);
                int frequency = s.audioRate, channels = s.audioChannels;
                mlt_audio_format af = audioFmt;
                void* pcm = nullptr;
                mlt_frame_get_audio(frame, &pcm, &af, &frequency, &channels, &samples);
                // Like MLT's consumer: what the frame delivers (repeated frames of a frame rate conversion deliver
                // none), silence when not at normal speed
                if (pcm && samples > 0) {
                    item.pcm.assign(size_t(samples) * s.audioChannels * sampleBytes, 0);
                    if (af != audioFmt || channels != s.audioChannels) {
                        if (!warnedSound)
                            qWarning("Export: frame %d delivered sound as format %d with %d channels, silence", i,
                                     int(af), channels);
                        warnedSound = true;
                    } else if (mlt_properties_get_double(MLT_FRAME_PROPERTIES(frame), "_speed") == 1.0) {
                        std::memcpy(item.pcm.data(), pcm, item.pcm.size());
                    }
                }
            }
            mlt_frame_close(frame); // the picture holds its own reference
            if (!push(std::move(item))) break;
        }
        Item end;
        end.end = true;
        push(std::move(end));
    }

    bool open()
    {
        const QByteArray path = s.path.toUtf8();
        int r = avformat_alloc_output_context2(&oc, nullptr, nullptr, path.constData());
        if (r < 0 || !oc) {
            fail(QStringLiteral("no muxer for %1: %2").arg(s.path, avError(r)));
            return false;
        }
        packet = av_packet_alloc();
        // Video
        const AVCodec* vcodec = avcodec_find_encoder_by_name(s.videoCodec.toUtf8().constData());
        vst = vcodec ? avformat_new_stream(oc, vcodec) : nullptr;
        venc = vcodec ? avcodec_alloc_context3(vcodec) : nullptr;
        if (!vst || !venc || !packet) {
            fail(QStringLiteral("video encoder %1 not available").arg(s.videoCodec), true);
            return false;
        }
        venc->thread_count = std::max(0, s.threads);
        venc->width = width;
        venc->height = height;
        venc->time_base = av_inv_q(rate);
        venc->framerate = rate;
        vst->time_base = venc->time_base;
        vst->avg_frame_rate = rate;
        venc->pix_fmt = pixFmt;
        venc->colorspace = matrixOf(colorspace, height);
        venc->color_trc = transferOf(colorspace);
        venc->color_primaries = primariesOf(colorspace, height);
        venc->color_range = AVCOL_RANGE_MPEG; // MLT's consumer left it unspecified; it is limited range
        venc->sample_aspect_ratio = aspect;
        vst->sample_aspect_ratio = aspect;
        venc->field_order = AV_FIELD_PROGRESSIVE;
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        for (const auto& [key, value] : s.videoOptions)
            applyOption(venc, key, value, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM);
        if (!venc->rc_initial_buffer_occupancy) venc->rc_initial_buffer_occupancy = venc->rc_buffer_size * 3 / 4;
        if ((r = avcodec_open2(venc, vcodec, nullptr)) < 0) {
            fail(QStringLiteral("video encoder %1 cannot be opened: %2").arg(s.videoCodec, avError(r)), true);
            return false;
        }
        avcodec_parameters_from_context(vst->codecpar, venc);
        // Sound
        if (!s.audioCodec.isEmpty()) {
            const AVCodec* acodec = avcodec_find_encoder_by_name(s.audioCodec.toUtf8().constData());
            ast = acodec ? avformat_new_stream(oc, acodec) : nullptr;
            aenc = acodec ? avcodec_alloc_context3(acodec) : nullptr;
            if (!ast || !aenc) {
                fail(QStringLiteral("audio encoder %1 not available").arg(s.audioCodec));
                return false;
            }
            aenc->sample_fmt = sampleFmt;
            aenc->sample_rate = s.audioRate;
            av_channel_layout_default(&aenc->ch_layout, s.audioChannels);
            aenc->time_base = AVRational{1, s.audioRate};
            ast->time_base = aenc->time_base;
            if (s.audioBitrateK > 0) aenc->bit_rate = int64_t(s.audioBitrateK) * 1000;
            if (oc->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if ((r = avcodec_open2(aenc, acodec, nullptr)) < 0) {
                fail(QStringLiteral("audio encoder %1 cannot be opened: %2").arg(s.audioCodec, avError(r)));
                return false;
            }
            avcodec_parameters_from_context(ast->codecpar, aenc);
            audioFrameSize = aenc->frame_size > 0 ? aenc->frame_size : 1024; // PCM takes any size
        }
        if ((r = avio_open(&oc->pb, path.constData(), AVIO_FLAG_WRITE)) < 0) {
            fail(QStringLiteral("cannot write %1: %2").arg(s.path, avError(r)));
            return false;
        }
        AVDictionary* options = nullptr;
        if (s.fastStart) av_dict_set(&options, "movflags", "+faststart", 0);
        r = avformat_write_header(oc, &options);
        av_dict_free(&options);
        if (r < 0) {
            fail(QStringLiteral("cannot write the header of %1: %2").arg(s.path, avError(r)));
            return false;
        }
        return true;
    }

    // Packets the encoder has ready -> muxer; false = failed
    bool drain(AVCodecContext* c, AVStream* st, bool video)
    {
        for (;;) {
            const int r = avcodec_receive_packet(c, packet);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) {
                fail(QStringLiteral("encoding failed: %1").arg(avError(r)), video);
                return false;
            }
            av_packet_rescale_ts(packet, c->time_base, st->time_base);
            packet->stream_index = st->index;
            const int w = av_interleaved_write_frame(oc, packet); // takes the packet's data
            if (w < 0) {
                fail(QStringLiteral("writing failed: %1").arg(avError(w)));
                return false;
            }
        }
    }

    // One encoder frame of sound from the start of the fifo (fewer samples only at the very end)
    bool encodeSound(int samples)
    {
        AVFrame* f = av_frame_alloc();
        if (!f) return false;
        f->format = aenc->sample_fmt;
        f->nb_samples = samples;
        f->sample_rate = aenc->sample_rate;
        av_channel_layout_copy(&f->ch_layout, &aenc->ch_layout);
        if (av_frame_get_buffer(f, 0) < 0) {
            av_frame_free(&f);
            return false;
        }
        const int channels = s.audioChannels;
        const int available = int(fifo.size() / (size_t(channels) * sampleBytes));
        const int take = std::min(samples, available);
        if (av_sample_fmt_is_planar(aenc->sample_fmt)) {
            for (int ch = 0; ch < channels; ++ch) {
                uint8_t* out = f->extended_data[ch];
                const uint8_t* in = fifo.data() + size_t(ch) * sampleBytes;
                for (int i = 0; i < take; ++i, out += sampleBytes, in += size_t(channels) * sampleBytes)
                    std::memcpy(out, in, sampleBytes);
                std::memset(out, 0, size_t(samples - take) * sampleBytes);
            }
        } else {
            std::memcpy(f->data[0], fifo.data(), size_t(take) * channels * sampleBytes);
            std::memset(f->data[0] + size_t(take) * channels * sampleBytes, 0, size_t(samples - take) * channels * sampleBytes);
        }
        fifo.erase(fifo.begin(), fifo.begin() + ptrdiff_t(size_t(take) * channels * sampleBytes));
        f->pts = audioPts;
        audioPts += samples;
        const int r = avcodec_send_frame(aenc, f);
        av_frame_free(&f);
        if (r < 0) {
            fail(QStringLiteral("audio encoding failed: %1").arg(avError(r)));
            return false;
        }
        return drain(aenc, ast, false);
    }

    bool flushSound()
    {
        const size_t frameBytes = size_t(audioFrameSize) * s.audioChannels * sampleBytes;
        while (fifo.size() >= frameBytes)
            if (!encodeSound(audioFrameSize)) return false;
        if (!fifo.empty()) {
            const int rest = int(fifo.size() / (size_t(s.audioChannels) * sampleBytes));
            // Codecs with a fixed frame size get the last frame filled with silence, as MLT's consumer did it
            const bool small = aenc->codec->capabilities & (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE);
            if (!encodeSound(small || aenc->frame_size <= 0 ? rest : audioFrameSize)) return false;
        }
        if (avcodec_send_frame(aenc, nullptr) < 0) return true;
        return drain(aenc, ast, false);
    }

    void encode()
    {
        if (!open()) {
            discardQueue();
            return;
        }
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cond.wait(lock, [this] { return stop || !queue.empty(); });
                if (stop) break;
                item = std::move(queue.front());
                queue.pop_front();
                cond.notify_all();
            }
            if (item.end) {
                if (aenc && !flushSound()) break;
                if (avcodec_send_frame(venc, nullptr) >= 0 && !drain(venc, vst, true)) break;
                const int r = av_write_trailer(oc);
                if (r < 0) {
                    fail(QStringLiteral("cannot finish %1: %2").arg(s.path, avError(r)));
                    break;
                }
                if (avio_closep(&oc->pb) < 0) {
                    fail(QStringLiteral("cannot finish %1").arg(s.path));
                    break;
                }
                state = State::Done;
                return;
            }
            if (aenc) {
                fifo.insert(fifo.end(), item.pcm.begin(), item.pcm.end());
                const size_t frameBytes = size_t(audioFrameSize) * s.audioChannels * sampleBytes;
                bool ok = true;
                while (ok && fifo.size() >= frameBytes) ok = encodeSound(audioFrameSize);
                if (!ok) {
                    av_frame_free(&item.picture);
                    break;
                }
            }
            const int r = avcodec_send_frame(venc, item.picture);
            av_frame_free(&item.picture);
            if (r < 0) {
                fail(QStringLiteral("video encoding failed: %1").arg(avError(r)), true);
                break;
            }
            if (!drain(venc, vst, true)) break;
            ++written;
        }
        discardQueue();
    }

    void discardQueue()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (Item& i : queue) av_frame_free(&i.picture);
        queue.clear();
        cond.notify_all();
    }

    void closeEncoders()
    {
        avcodec_free_context(&venc); // frees pictures still held by the encoder (MLT frames)
        avcodec_free_context(&aenc);
        if (oc) {
            if (oc->pb) avio_closep(&oc->pb);
            avformat_free_context(oc);
            oc = nullptr;
        }
        av_packet_free(&packet);
    }
};

ExportWriter::ExportWriter(Mlt::Profile& profile, Mlt::Producer& source, const Settings& s) : d(std::make_unique<Impl>())
{
    d->s = s;
    d->width = profile.width();
    d->height = profile.height();
    d->colorspace = profile.colorspace();
    d->rate = AVRational{profile.frame_rate_num(), std::max(1, profile.frame_rate_den())};
    d->fps = std::max(1.0, profile.fps());
    d->aspect = AVRational{std::max(1, profile.sample_aspect_num()), std::max(1, profile.sample_aspect_den())};
    d->pixFmt = av_get_pix_fmt(s.pixFmt.toUtf8().constData());
    if (const auto plan = imagePlan(d->pixFmt)) d->plan = *plan;
    if (!s.audioCodec.isEmpty()) {
        d->sampleFmt = av_get_sample_fmt(s.sampleFormat.toUtf8().constData());
        d->audioFmt = mltAudioFormat(d->sampleFmt);
        d->sampleBytes = av_get_bytes_per_sample(d->sampleFmt);
    }
    d->queueMax = size_t(std::clamp(std::abs(s.realTime), 2, 8));

    d->consumer = mlt_consumer_new(profile.get_profile());
    mlt_service_set_profile(MLT_CONSUMER_SERVICE(d->consumer), profile.get_profile());
    d->consumer->child = d.get();
    d->consumer->is_stopped = Impl::isStopped; // mlt_consumer_start only starts a stopped consumer
    mlt_properties p = MLT_CONSUMER_PROPERTIES(d->consumer);
    mlt_properties_set_int(p, "real_time", s.realTime);
    mlt_properties_set_int(p, "prefill", 1); // like MLT's avformat consumer
    mlt_properties_set(p, "mlt_image_format", mlt_image_format_name(d->plan.mlt));
    if (d->audioFmt != mlt_audio_none) {
        mlt_properties_set(p, "mlt_audio_format", mlt_audio_format_name(d->audioFmt));
        mlt_properties_set_int(p, "frequency", s.audioRate);
        mlt_properties_set_int(p, "channels", s.audioChannels);
    } else {
        mlt_properties_set_int(p, "audio_off", 1);
    }
    // MLT's consumer sets these before the first frame; filters see them as consumer.color_trc
    mlt_properties_set_int(p, "color_trc", transferOf(d->colorspace));
    mlt_properties_set_int(p, "color_primaries", primariesOf(d->colorspace, d->height));
    mlt_consumer_connect(d->consumer, MLT_PRODUCER_SERVICE(source.get_producer()));
}

ExportWriter::~ExportWriter()
{
    cancel();
    d->closeEncoders();
    if (d->consumer) {
        mlt_consumer_close(d->consumer);
        d->consumer = nullptr;
    }
}

void ExportWriter::start()
{
    if (d->started) return;
    d->started = true;
    if (mlt_consumer_start(d->consumer) != 0) {
        d->fail(QStringLiteral("render workers cannot be started"));
        return;
    }
    d->consumerStarted = true;
    d->encoder = std::thread([this] { d->encode(); });
    d->fetcher = std::thread([this] { d->fetch(); });
}

void ExportWriter::cancel()
{
    if (d->state == State::Running) d->stop = true;
    d->cond.notify_all();
    // The fetch thread may wait in mlt_consumer_rt_frame for the frame being rendered: the workers still run and
    // deliver it. Only then stop the workers (mlt_consumer_stop frees what the waiting thread uses).
    if (d->fetcher.joinable()) d->fetcher.join();
    if (d->consumerStarted) {
        mlt_consumer_stop(d->consumer);
        d->consumerStarted = false;
    }
    d->stop = true;
    d->cond.notify_all();
    if (d->encoder.joinable()) d->encoder.join();
    d->discardQueue();
}

ExportWriter::State ExportWriter::state() const
{
    return d->state;
}

bool ExportWriter::encoderFailed() const
{
    return d->encoderBroke;
}

QString ExportWriter::error() const
{
    std::lock_guard<std::mutex> lock(d->errorMutex);
    return d->error;
}

int ExportWriter::framesWritten() const
{
    return d->written;
}
