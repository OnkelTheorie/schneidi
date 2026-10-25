#include "engine/ExportWriter.h"

#include "engine/OutputColor.h"
#include "engine/video/Reader.h"

#include <Mlt.h>
#include <QFileInfo>
#include <QSettings>
#include <framework/mlt.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
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

// Smart render: decode time = packet number - kDelay (frames). Copied GOPs may show a picture up to kDelay packets
// after it was decoded (B-frames; x264/NVENC reorder 1-2, x265 up to 3)
constexpr int kDelay = 4;
constexpr size_t kMaxGopBytes = size_t(512) << 20; // longer GOPs are encoded instead of held in memory

// Does this Annex B packet hold an IDR picture (H.264 type 5, H.265 IDR_W_RADL/IDR_N_LP)? Only those start a GOP
// that decodes on its own: H.264 I-frames without IDR and H.265 CRA may be followed by pictures referring back.
bool hasIdr(const AVPacket* p, AVCodecID codec)
{
    const uint8_t* b = p->data;
    const int n = p->size;
    for (int i = 0; i + 3 < n; ++i) {
        if (b[i] != 0 || b[i + 1] != 0 || b[i + 2] != 1) continue;
        const uint8_t h = b[i + 3];
        if (codec == AV_CODEC_ID_H264 ? (h & 0x1f) == 5 : (((h >> 1) & 0x3f) == 19 || ((h >> 1) & 0x3f) == 20))
            return true;
        i += 2;
    }
    return false;
}

// Software format of the stream an encoder writes for this input format (NVENC takes nv12/p010)
AVPixelFormat streamFormat(AVPixelFormat f)
{
    switch (f) {
    case AV_PIX_FMT_NV12: return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_P010LE: return AV_PIX_FMT_YUV420P10LE;
    default: return f;
    }
}

void freePackets(std::vector<AVPacket*>& packets)
{
    for (AVPacket*& p : packets) av_packet_free(&p);
    packets.clear();
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

void ExportWriter::setWanted(bool own)
{
    QSettings().setValue("render/writer", own ? "own" : "mlt");
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

    // Smart render
    bool smart = false;
    mlt_profile mltProfile = nullptr;
    mlt_producer source = nullptr;      // pictures; seeked to every encoded piece
    mlt_producer soundSource = nullptr; // sound of every output frame
    mlt_consumer soundConsumer = nullptr;
    int soundNext = 0; // fetch thread: output frame whose sound comes next
    AVCodecID codecId = AV_CODEC_ID_NONE;
    std::atomic<int> copied{0};
    std::atomic<bool> smartBroke{false};

    // fetch thread -> encode thread
    struct Item {
        AVFrame* picture = nullptr;
        std::vector<uint8_t> pcm; // interleaved, encoder sample type
        bool end = false;
        // Smart render: first picture of an encoded piece (new encoder, starts with a key frame)
        bool restart = false;
        // Smart render: a copied GOP (Annex B, pts in output frames, decode order) and its stream
        std::vector<AVPacket*> packets;
        std::shared_ptr<const AVCodecParameters> params;
        int frames = 1;
    };
    static void freeItem(Item& i)
    {
        av_frame_free(&i.picture);
        freePackets(i.packets);
    }
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
    bool headerWritten = false;
    // Smart render
    std::vector<uint8_t> pieceHeader; // parameter sets of the piece encoder (Annex B)
    bool pieceStart = false;          // the next encoded packet is the piece's first: parameter sets before it
    int piecesOpened = 0;             // x265 prints its whole configuration per encoder: only for the first piece
    bool pieceFed = false;            // the piece encoder has pictures
    int64_t videoPackets = 0;

    static int isStopped(mlt_consumer c) { return static_cast<Impl*>(c->child)->consumerStarted ? 0 : 1; }
    // Consumers of the smart path (pieces, sound): running = property set after the start
    static int pieceStopped(mlt_consumer c) { return mlt_properties_get_int(MLT_CONSUMER_PROPERTIES(c), "_running") ? 0 : 1; }

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
            freeItem(item);
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

    // Render workers over a service: pictures (in the image format of the encoder) and/or sound, as MLT's consumer
    mlt_consumer newConsumer(mlt_service service, bool pictures, bool sound, int realTime)
    {
        mlt_consumer c = mlt_consumer_new(mltProfile);
        if (!c) return nullptr;
        mlt_service_set_profile(MLT_CONSUMER_SERVICE(c), mltProfile);
        mlt_properties p = MLT_CONSUMER_PROPERTIES(c);
        mlt_properties_set_int(p, "real_time", realTime);
        mlt_properties_set_int(p, "prefill", 1); // like MLT's avformat consumer
        mlt_properties_set(p, "mlt_image_format", mlt_image_format_name(plan.mlt));
        if (sound && audioFmt != mlt_audio_none) {
            mlt_properties_set(p, "mlt_audio_format", mlt_audio_format_name(audioFmt));
            mlt_properties_set_int(p, "frequency", s.audioRate);
            mlt_properties_set_int(p, "channels", s.audioChannels);
        } else {
            mlt_properties_set_int(p, "audio_off", 1);
        }
        if (!pictures) mlt_properties_set_int(p, "video_off", 1);
        // MLT's consumer sets these before the first frame; filters see them as consumer.color_trc
        mlt_properties_set_int(p, "color_trc", transferOf(colorspace));
        mlt_properties_set_int(p, "color_primaries", primariesOf(colorspace, height));
        mlt_consumer_connect(c, service);
        return c;
    }

    bool startPiece(mlt_consumer c)
    {
        c->is_stopped = pieceStopped;
        if (mlt_consumer_start(c) != 0) return false;
        mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "_running", 1);
        return true;
    }

    static void stopPiece(mlt_consumer& c)
    {
        if (!c) return;
        mlt_consumer_stop(c);
        mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "_running", 0);
        mlt_consumer_close(c);
        c = nullptr;
    }

    // Output frame i's picture from the consumer as an AVFrame for the encoder; nullptr = failed (fail() called)
    AVFrame* fetchPicture(mlt_consumer c, int i, mlt_frame* keep)
    {
        mlt_frame frame = mlt_consumer_rt_frame(c);
        if (stop) {
            if (frame) mlt_frame_close(frame);
            return nullptr;
        }
        if (!frame) {
            fail(QStringLiteral("no frame %1 from the timeline").arg(i));
            return nullptr;
        }
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
            return nullptr;
        }
        AVFrame* f = picture(frame, image);
        if (!f) {
            mlt_frame_close(frame);
            fail(QStringLiteral("out of memory"));
            return nullptr;
        }
        f->pts = i;
        *keep = frame;
        return f;
    }

    // Sound of output frame i from its MLT frame, appended to pcm
    void soundOf(mlt_frame frame, int i, std::vector<uint8_t>& pcm, bool& warned)
    {
        // Same count as MLT's consumer and its workers (from the first exported frame): the sound is cached in
        // the frame already when the workers fetched it ahead
        int samples = mlt_audio_calculate_frame_samples(float(fps), s.audioRate, i);
        int frequency = s.audioRate, channels = s.audioChannels;
        mlt_audio_format af = audioFmt;
        void* data = nullptr;
        mlt_frame_get_audio(frame, &data, &af, &frequency, &channels, &samples);
        // Like MLT's consumer: what the frame delivers (repeated frames of a frame rate conversion deliver
        // none), silence when not at normal speed
        if (!data || samples <= 0) return;
        const size_t at = pcm.size(), bytes = size_t(samples) * s.audioChannels * sampleBytes;
        pcm.resize(at + bytes, 0);
        if (af != audioFmt || channels != s.audioChannels) {
            if (!warned) qWarning("Export: frame %d delivered sound as format %d with %d channels, silence", i, int(af), channels);
            warned = true;
        } else if (mlt_properties_get_double(MLT_FRAME_PROPERTIES(frame), "_speed") == 1.0) {
            std::memcpy(pcm.data() + at, data, bytes);
        }
    }

    void fetch()
    {
        if (smart) {
            fetchSmart();
        } else {
            bool warnedSound = false;
            for (int i = 0; i < s.length && !stop; ++i) {
                mlt_frame frame = nullptr;
                Item item;
                item.picture = fetchPicture(consumer, i, &frame);
                if (!item.picture) break;
                if (audioFmt != mlt_audio_none) soundOf(frame, i, item.pcm, warnedSound);
                mlt_frame_close(frame); // the picture holds its own reference
                if (!push(std::move(item))) break;
            }
        }
        Item end;
        end.end = true;
        push(std::move(end));
    }

    // ---- Smart render, fetch thread ----

    // A file whose pictures may be copied: its own demuxer, packets as Annex B with parameter sets before key frames
    struct Source {
        AVFormatContext* fmt = nullptr;
        AVBSFContext* bsf = nullptr;
        AVPacket* pkt = nullptr;
        int index = -1;
        double tb = 0, fps = 0;
        int64_t firstPts = AV_NOPTS_VALUE;
        std::shared_ptr<AVCodecParameters> params; // the stream after the bit stream filter
        std::deque<AVPacket*> ready;
        bool eof = false;
        ~Source()
        {
            for (AVPacket*& p : ready) av_packet_free(&p);
            av_packet_free(&pkt);
            av_bsf_free(&bsf);
            avformat_close_input(&fmt);
        }
        // Stream frame position like engine/video/Reader (MLT's producer): from the first key frame, rounded
        int64_t position(const AVPacket* p) const
        {
            int64_t pts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
            if (firstPts != AV_NOPTS_VALUE) pts -= firstPts;
            else if (fmt->start_time != AV_NOPTS_VALUE) pts -= av_rescale_q(fmt->start_time, AV_TIME_BASE_Q, fmt->streams[index]->time_base);
            return int64_t(tb * double(pts) * fps + 0.5);
        }
        // Next packet of the stream (caller frees it), nullptr at the end or on a read error
        AVPacket* next()
        {
            for (;;) {
                if (!ready.empty()) {
                    AVPacket* p = ready.front();
                    ready.pop_front();
                    return p;
                }
                if (eof) return nullptr;
                const int r = av_read_frame(fmt, pkt);
                if (r < 0) {
                    eof = true;
                    av_bsf_send_packet(bsf, nullptr);
                } else if (pkt->stream_index != index) {
                    av_packet_unref(pkt);
                    continue;
                } else if (av_bsf_send_packet(bsf, pkt) < 0) { // takes the packet
                    av_packet_unref(pkt);
                    eof = true;
                    return nullptr;
                }
                for (;;) {
                    AVPacket* out = av_packet_alloc();
                    if (!out || av_bsf_receive_packet(bsf, out) < 0) {
                        av_packet_free(&out);
                        break;
                    }
                    if (out->pts == AV_NOPTS_VALUE && out->dts == AV_NOPTS_VALUE) av_packet_free(&out);
                    else ready.push_back(out);
                }
            }
        }
        // Read on from the key frame at or before stream position `position`
        void seek(int64_t position)
        {
            for (AVPacket*& p : ready) av_packet_free(&p);
            ready.clear();
            eof = false;
            int64_t ts = int64_t(double(position) / (tb * fps));
            if (firstPts != AV_NOPTS_VALUE) ts += firstPts;
            av_seek_frame(fmt, index, std::max<int64_t>(ts, 0), AVSEEK_FLAG_BACKWARD);
            av_bsf_flush(bsf);
        }
    };
    std::map<QString, std::unique_ptr<Source>> sources; // nullptr = does not fit, encode

    Source* sourceFor(const QString& path)
    {
        auto it = sources.find(path);
        if (it != sources.end()) return it->second.get();
        auto src = openSource(path);
        Source* result = src.get();
        sources.emplace(path, std::move(src));
        return result;
    }

    std::unique_ptr<Source> openSource(const QString& path)
    {
        // What the render path makes of the file (frame rate guess, color, rotation like MLT): the copy must show
        // the same pictures
        Video::Info info;
        AVCodecID codec = AV_CODEC_ID_NONE;
        AVFieldOrder fields = AV_FIELD_UNKNOWN;
        {
            Video::Reader::Options o;
            o.threads = 1;
            o.hardware = false;
            QString error;
            auto reader = Video::Reader::open(path, o, &error);
            if (!reader) {
                qInfo("Export: smart render encodes %s (%s)", qPrintable(QFileInfo(path).fileName()), qPrintable(error));
                return nullptr;
            }
            info = reader->info();
            const AVCodecParameters* par = reader->container()->streams[info.index]->codecpar;
            codec = par->codec_id;
            fields = par->field_order;
        }
        auto why = [&]() -> const char* {
            if (codec != codecId) return "other codec";
            if (info.width != width || info.height != height) return "other size";
            if (av_cmp_q(info.sar, aspect) != 0) return "other pixel aspect";
            if (av_cmp_q(info.frameRate, rate) != 0) return "other frame rate";
            if (info.format != streamFormat(pixFmt)) return "other pixel format";
            if (info.fullRange || info.colorspace != colorspace) return "other color";
            if (info.rotation != 0) return "rotated";
            if (fields != AV_FIELD_PROGRESSIVE && fields != AV_FIELD_UNKNOWN) return "interlaced";
            return nullptr;
        };
        if (const char* reason = why()) {
            qInfo("Export: smart render encodes %s (%s)", qPrintable(QFileInfo(path).fileName()), reason);
            return nullptr;
        }
        auto src = std::make_unique<Source>();
        const QByteArray file = path.toUtf8();
        auto cannot = [&](const char* what) {
            qInfo("Export: smart render encodes %s (%s)", qPrintable(QFileInfo(path).fileName()), what);
            return nullptr;
        };
        if (avformat_open_input(&src->fmt, file.constData(), nullptr, nullptr) < 0
            || avformat_find_stream_info(src->fmt, nullptr) < 0 || info.index >= int(src->fmt->nb_streams))
            return cannot("cannot be opened");
        src->index = info.index;
        AVStream* st = src->fmt->streams[info.index];
        src->tb = av_q2d(st->time_base);
        src->fps = av_q2d(info.frameRate);
        src->pkt = av_packet_alloc();
        const AVBitStreamFilter* filter = av_bsf_get_by_name(codec == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb");
        if (!src->pkt || !filter || av_bsf_alloc(filter, &src->bsf) < 0) return cannot("no bitstream filter");
        avcodec_parameters_copy(src->bsf->par_in, st->codecpar);
        src->bsf->time_base_in = st->time_base;
        if (av_bsf_init(src->bsf) < 0) return cannot("bitstream filter failed");
        src->params = std::shared_ptr<AVCodecParameters>(avcodec_parameters_alloc(),
                                                          [](AVCodecParameters* p) { avcodec_parameters_free(&p); });
        if (!src->params || avcodec_parameters_copy(src->params.get(), src->bsf->par_out) < 0) return nullptr;
        src->params->codec_tag = 0;
        // MLT's find_first_pts (as the Reader): frame 0 is the first key frame
        for (int n = 0; n < 500 && src->firstPts == AV_NOPTS_VALUE; ++n) {
            if (av_read_frame(src->fmt, src->pkt) < 0) break;
            if (src->pkt->stream_index == src->index && (src->pkt->flags & AV_PKT_FLAG_KEY))
                src->firstPts = src->pkt->dts != AV_NOPTS_VALUE && src->pkt->dts < 0
                                    ? 0
                                    : (src->pkt->pts != AV_NOPTS_VALUE ? src->pkt->pts : src->pkt->dts);
            av_packet_unref(src->pkt);
        }
        av_seek_frame(src->fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
        return src;
    }

    // Sound of output frame i (frames come in order) appended to pcm
    bool nextSound(int i, std::vector<uint8_t>& pcm)
    {
        static thread_local bool warned = false;
        if (!soundConsumer) return true;
        if (i != soundNext) {
            fail(QStringLiteral("sound of frame %1 asked, %2 is next").arg(i).arg(soundNext));
            return false;
        }
        mlt_frame frame = mlt_consumer_rt_frame(soundConsumer);
        if (!frame) {
            if (!stop) fail(QStringLiteral("no sound for frame %1").arg(i));
            return false;
        }
        soundOf(frame, i, pcm, warned);
        mlt_frame_close(frame);
        ++soundNext;
        return true;
    }

    // Output frames [a, b) encoded: the timeline from a, one new encoder for the piece
    bool encodePiece(int a, int b)
    {
        if (b <= a || stop) return !stop;
        mlt_producer_seek(source, a); // position counts from the in point
        // The last piece's workers may have read ahead up to the end, where the producer paused (speed 0)
        mlt_producer_set_speed(source, 1);
        mlt_consumer c = newConsumer(MLT_PRODUCER_SERVICE(source), true, false, s.realTime);
        if (!c || !startPiece(c)) {
            if (c) mlt_consumer_close(c);
            fail(QStringLiteral("render workers cannot be started"));
            return false;
        }
        bool ok = true;
        for (int i = a; i < b && ok; ++i) {
            mlt_frame frame = nullptr;
            Item item;
            item.picture = fetchPicture(c, i, &frame);
            if (!item.picture) {
                ok = false;
                break;
            }
            mlt_frame_close(frame);
            item.restart = i == a;
            ok = nextSound(i, item.pcm) && push(std::move(item));
        }
        stopPiece(c);
        return ok;
    }

    // One GOP from the IDR packet `first` up to the next IDR (returned in `next`, nullptr at the end of the file)
    struct Gop {
        std::vector<AVPacket*> packets;
        int64_t start = 0; // stream position of the IDR picture
        int count = 0;
        bool whole = false; // contiguous positions start … start + count - 1, decodable with kDelay
        const char* why = nullptr; // not whole: the reason, for the log
        ~Gop() { freePackets(packets); }
    };

    void readGop(Source& src, AVPacket* first, Gop& g, AVPacket*& next, int64_t limit)
    {
        g.packets.push_back(first);
        g.start = src.position(first);
        size_t bytes = size_t(first->size);
        next = nullptr;
        for (;;) {
            AVPacket* p = src.next();
            if (!p) break;
            if (hasIdr(p, codecId)) {
                next = p;
                break;
            }
            bytes += size_t(p->size);
            g.packets.push_back(p);
            // Way past the span or too large to hold: not copied anyway
            if (bytes > kMaxGopBytes || src.position(p) > limit + 1000) {
                g.why = bytes > kMaxGopBytes ? "GOP too large" : "GOP too long";
                return;
            }
        }
        std::vector<int64_t> pos;
        pos.reserve(g.packets.size());
        for (size_t j = 0; j < g.packets.size(); ++j) {
            const int64_t q = src.position(g.packets[j]) - g.start;
            if (int64_t(j) - q > kDelay) { // shown later than the decode times allow
                g.why = "pictures reordered too far";
                return;
            }
            pos.push_back(q);
        }
        std::sort(pos.begin(), pos.end());
        for (size_t j = 0; j < pos.size(); ++j)
            if (pos[j] != int64_t(j)) { // gap, double or earlier picture (open GOP, frame rate changes)
                g.why = "pictures missing or doubled (open GOP or variable frame rate)";
                return;
            }
        g.count = int(pos.size());
        // Next GOP right behind (a gap in between: the Reader shows the next picture there, not ours)
        g.whole = !next || src.position(next) == g.start + g.count;
        if (!g.whole) g.why = "gap before the next key frame";
    }

    // Copy what fits of span, encode the rest
    bool copySpan(const SmartRender::Span& span)
    {
        Source* src = sourceFor(span.file);
        if (!src) return encodePiece(span.from, span.to);
        const int64_t s0 = span.sourceFrame, s1 = s0 + (span.to - span.from);
        src->seek(s0);
        AVPacket* p = nullptr;
        while ((p = src->next())) { // first IDR at or after s0
            const int64_t at = src->position(p);
            if (at >= s1) break;
            if (at >= s0 && hasIdr(p, codecId)) break;
            av_packet_free(&p);
        }
        if (!p || src->position(p) >= s1) {
            av_packet_free(&p);
            qInfo("Export: smart render encodes %s at %d..%d (no IDR key frame there)", qPrintable(QFileInfo(span.file).fileName()),
                  span.from, span.to);
            return encodePiece(span.from, span.to);
        }
        int out = span.from + int(src->position(p) - s0);
        if (!encodePiece(span.from, out)) {
            av_packet_free(&p);
            return false;
        }
        while (p && !stop) {
            auto g = std::make_unique<Gop>();
            AVPacket* next = nullptr;
            readGop(*src, p, *g, next, s1);
            p = next;
            if (!g->whole || g->start + g->count > s1) { // ends behind the span: encode from its start
                if (g->why)
                    qInfo("Export: smart render encodes %s from %d (%s)", qPrintable(QFileInfo(span.file).fileName()), out, g->why);
                break;
            }
            Item item;
            item.frames = g->count;
            item.params = src->params;
            const int64_t shift = int64_t(out) - g->start;
            for (AVPacket* q : g->packets) {
                q->pts = src->position(q) + shift;
                q->dts = AV_NOPTS_VALUE;
                q->duration = 1;
            }
            item.packets = std::move(g->packets);
            for (int i = 0; i < item.frames; ++i)
                if (!nextSound(out + i, item.pcm)) {
                    freeItem(item);
                    av_packet_free(&p);
                    return false;
                }
            out += item.frames;
            copied += item.frames;
            if (!push(std::move(item))) {
                av_packet_free(&p);
                return false;
            }
            if (!p || src->position(p) >= s1) break;
        }
        av_packet_free(&p);
        return encodePiece(out, span.to);
    }

    void fetchSmart()
    {
        if (audioFmt != mlt_audio_none) {
            mlt_producer_seek(soundSource, 0);
            soundConsumer = newConsumer(MLT_PRODUCER_SERVICE(soundSource), false, true, 1);
            if (!soundConsumer || !startPiece(soundConsumer)) {
                fail(QStringLiteral("sound workers cannot be started"));
                return;
            }
        }
        int pos = 0;
        bool ok = true;
        for (const SmartRender::Span& span : s.copy) {
            if (!ok || stop) break;
            const int from = std::max(span.from, pos), to = std::min(span.to, s.length);
            if (from >= to) continue;
            ok = encodePiece(pos, from) && copySpan({from, to, span.file, span.sourceFrame + (from - span.from)});
            pos = to;
        }
        if (ok) encodePiece(pos, s.length);
        stopPiece(soundConsumer);
        sources.clear();
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
        // Video (smart render: one encoder per piece, the stream gets the first item's parameters)
        vst = avformat_new_stream(oc, nullptr);
        if (!vst || !packet) {
            fail(QStringLiteral("out of memory"));
            return false;
        }
        vst->time_base = av_inv_q(rate);
        vst->avg_frame_rate = rate;
        vst->sample_aspect_ratio = aspect;
        if (!smart) {
            if (!(venc = newVideoEncoder())) return false;
            avcodec_parameters_from_context(vst->codecpar, venc);
        }
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
        return smart || writeHeader();
    }

    bool writeHeader()
    {
        const QByteArray path = s.path.toUtf8();
        int r = 0;
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
            smartBroke = smart;
            return false;
        }
        headerWritten = true;
        return true;
    }

    // The video encoder as the settings want it. Smart render: no B-frames (decode order = picture order, see
    // kDelay), parameter sets kept aside (pieceHeader) and put in-band before each piece's first picture.
    AVCodecContext* newVideoEncoder()
    {
        const AVCodec* vcodec = avcodec_find_encoder_by_name(s.videoCodec.toUtf8().constData());
        AVCodecContext* c = vcodec ? avcodec_alloc_context3(vcodec) : nullptr;
        if (!c) {
            fail(QStringLiteral("video encoder %1 not available").arg(s.videoCodec), true);
            return nullptr;
        }
        AVCodecContext* venc = c;
        int r = 0;
        venc->thread_count = std::max(0, s.threads);
        venc->width = width;
        venc->height = height;
        venc->time_base = av_inv_q(rate);
        venc->framerate = rate;
        venc->pix_fmt = pixFmt;
        venc->colorspace = matrixOf(colorspace, height);
        venc->color_trc = transferOf(colorspace);
        venc->color_primaries = primariesOf(colorspace, height);
        venc->color_range = AVCOL_RANGE_MPEG; // MLT's consumer left it unspecified; it is limited range
        venc->sample_aspect_ratio = aspect;
        venc->field_order = AV_FIELD_PROGRESSIVE;
        if (smart || (oc->oformat->flags & AVFMT_GLOBALHEADER)) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        for (const auto& [key, value] : s.videoOptions)
            applyOption(venc, key, value, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM);
        if (smart) venc->max_b_frames = 0;
        if (smart && piecesOpened++ > 0 && std::strcmp(vcodec->name, "libx265") == 0) {
            uint8_t* params = nullptr;
            av_opt_get(venc->priv_data, "x265-params", 0, &params);
            const QByteArray given(params ? reinterpret_cast<const char*>(params) : "");
            av_free(params);
            av_opt_set(venc->priv_data, "x265-params", (given.isEmpty() ? given : given + ':') + "log-level=warning", 0);
        }
        if (!venc->rc_initial_buffer_occupancy) venc->rc_initial_buffer_occupancy = venc->rc_buffer_size * 3 / 4;
        if ((r = avcodec_open2(venc, vcodec, nullptr)) < 0) {
            fail(QStringLiteral("video encoder %1 cannot be opened: %2").arg(s.videoCodec, avError(r)), true);
            avcodec_free_context(&c);
            return nullptr;
        }
        if (smart) {
            pieceHeader.assign(venc->extradata, venc->extradata + venc->extradata_size);
            if (pieceHeader.size() < 4 || (pieceHeader[0] != 0 || pieceHeader[1] != 0)) { // not Annex B
                fail(QStringLiteral("encoder %1 delivers no Annex B parameter sets").arg(s.videoCodec));
                smartBroke = true;
                avcodec_free_context(&c);
                return nullptr;
            }
            pieceStart = true;
        }
        return c;
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
            if (video && smart) {
                if (!writeVideo(packet, std::exchange(pieceStart, false))) return false;
                continue;
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

    // Smart render: one picture into the single stream. pts in output frames; decode time = packet number - kDelay.
    // withHeader: the piece encoder's parameter sets in front (its first, key frame packet)
    bool writeVideo(AVPacket* p, bool withHeader)
    {
        if (withHeader) {
            const int extra = int(pieceHeader.size()), size = p->size;
            if (av_grow_packet(p, extra) < 0) {
                fail(QStringLiteral("out of memory"));
                return false;
            }
            std::memmove(p->data + extra, p->data, size_t(size));
            std::memcpy(p->data, pieceHeader.data(), size_t(extra));
        }
        p->dts = videoPackets - kDelay;
        if (p->pts == AV_NOPTS_VALUE || p->dts > p->pts) {
            fail(QStringLiteral("picture %1 shown before it is decoded").arg(videoPackets));
            smartBroke = true;
            return false;
        }
        p->duration = 1;
        ++videoPackets;
        av_packet_rescale_ts(p, av_inv_q(rate), vst->time_base);
        p->stream_index = vst->index;
        const int w = av_interleaved_write_frame(oc, p);
        if (w < 0) {
            fail(QStringLiteral("writing failed: %1").arg(avError(w)));
            smartBroke = true;
            return false;
        }
        return true;
    }

    // Smart render: the encoded piece is complete (all its packets written)
    bool finishPiece()
    {
        if (!venc) return true;
        bool ok = avcodec_send_frame(venc, nullptr) < 0 || drain(venc, vst, true);
        avcodec_free_context(&venc);
        return ok;
    }

    // Smart render: the stream takes the parameters of its first item (copied GOP or encoded piece)
    bool startStream(const Item& item)
    {
        if (item.params) {
            avcodec_parameters_copy(vst->codecpar, item.params.get());
        } else {
            if (!(venc = newVideoEncoder())) return false;
            avcodec_parameters_from_context(vst->codecpar, venc);
        }
        vst->codecpar->codec_tag = 0;
        vst->codecpar->video_delay = kDelay;
        return writeHeader();
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
            if (smart && !headerWritten && !item.end && !startStream(item)) {
                freeItem(item);
                break;
            }
            if (item.end) {
                if (smart && !finishPiece()) break;
                if (!headerWritten) {
                    fail(QStringLiteral("nothing to write"));
                    break;
                }
                if (aenc && !flushSound()) break;
                if (venc && avcodec_send_frame(venc, nullptr) >= 0 && !drain(venc, vst, true)) break;
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
                    freeItem(item);
                    break;
                }
            }
            if (!item.packets.empty()) { // copied GOP
                pieceFed = false;
                bool ok = finishPiece();
                for (AVPacket* q : item.packets)
                    if (ok) ok = writeVideo(q, false);
                freeItem(item);
                if (!ok) break;
                written += item.frames;
                continue;
            }
            if (smart && item.restart && pieceFed) {
                pieceFed = false;
                if (!finishPiece()) {
                    freeItem(item);
                    break;
                }
            }
            if (smart && !venc) {
                if (!(venc = newVideoEncoder())) {
                    freeItem(item);
                    break;
                }
            }
            pieceFed = true;
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
        for (Item& i : queue) freeItem(i);
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

ExportWriter::ExportWriter(Mlt::Profile& profile, Mlt::Producer& source, const Settings& s, Mlt::Producer* sound)
    : d(std::make_unique<Impl>())
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
    if (const AVCodec* c = avcodec_find_encoder_by_name(s.videoCodec.toUtf8().constData())) d->codecId = c->id;
    // Smart render: H.264/H.265 in 4:2:0, pictures as they come (no frame rate conversion)
    d->smart = !s.copy.isEmpty() && sound && sound->is_valid() && s.realTime != 0
               && (d->codecId == AV_CODEC_ID_H264 || d->codecId == AV_CODEC_ID_HEVC)
               && (streamFormat(d->pixFmt) == AV_PIX_FMT_YUV420P || streamFormat(d->pixFmt) == AV_PIX_FMT_YUV420P10LE);
    d->mltProfile = profile.get_profile();
    d->source = source.get_producer();
    if (d->smart) d->soundSource = sound->get_producer();

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
    if (d->smart) { // consumers per piece in the fetch thread
        d->encoder = std::thread([this] { d->encode(); });
        d->fetcher = std::thread([this] { d->fetch(); });
        return;
    }
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

int ExportWriter::framesCopied() const
{
    return d->copied;
}

bool ExportWriter::smartFailed() const
{
    return d->smartBroke;
}
