// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/ExportWriter.h"

#include "engine/OutputColor.h"
#include "engine/audio/Mixer.h"
#include "engine/picture/Renderer.h"
#include "engine/video/Reader.h"

#include <QFileInfo>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <cmath>
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
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

// Sample formats the writer delivers: interleaved, the encode thread splits planar formats
bool soundFormat(AVSampleFormat f)
{
    switch (av_get_packed_sample_fmt(f)) {
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_U8: return true;
    default: return false;
    }
}

// Color tags of the file from its color standard (colorspace numbers 601 family, 240, 709, 2020)
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

// An option by name: AVOption of the context or its codec (children first); "vb"/"ab" style names also without their
// prefix (the names of the export formats, from the time MLT's consumer read them). Unknown names are left out.
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

} // namespace

bool ExportWriter::supportsOwn(const QString& path, const QString& muxer, const QString& videoCodec, const QString& pixFmt,
                               const QString& audioCodec, const QString& sampleFormat)
{
    const AVOutputFormat* f = muxer.isEmpty() ? av_guess_format(nullptr, path.toUtf8().constData(), nullptr)
                                              : av_guess_format(muxer.toUtf8().constData(), nullptr, nullptr);
    if (!f) return false;
    if (!videoCodec.isEmpty()) {
        const AVCodec* video = avcodec_find_encoder_by_name(videoCodec.toUtf8().constData());
        if (!video || video->type != AVMEDIA_TYPE_VIDEO) return false;
        const AVPixelFormat pf = av_get_pix_fmt(pixFmt.toUtf8().constData());
        if (pf == AV_PIX_FMT_NONE) return false;
        // Converted by OutputColor, uploaded (VAAPI) or by swscale from RGBA
        OutputColor::Target t;
        if (pf != AV_PIX_FMT_VAAPI && !OutputColor::targetOf(pf, t) && !sws_isSupportedOutput(pf)) return false;
    } else if (audioCodec.isEmpty()) {
        return false;
    }
    if (audioCodec.isEmpty()) return true;
    const AVCodec* audio = avcodec_find_encoder_by_name(audioCodec.toUtf8().constData());
    return audio && audio->type == AVMEDIA_TYPE_AUDIO && soundFormat(av_get_sample_fmt(sampleFormat.toUtf8().constData()));
}

struct ExportWriter::Impl {
    Settings s;
    int width = 0, height = 0, colorspace = 709;
    AVRational rate{25, 1}, aspect{1, 1};
    double fps = 25;
    AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
    AVSampleFormat sampleFmt = AV_SAMPLE_FMT_NONE;
    bool sound = false; // a sound stream in a format soundOwn() writes
    int sampleBytes = 0;

    // Smart render
    bool smart = false;
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
    bool started = false;

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

    // ---- Pictures and sound ----
    Own o;
    AVPixelFormat inputFmt = AV_PIX_FMT_NONE; // what the workers write (VAAPI: NV12, uploaded by the encode thread)
    OutputColor::Target target;               // inputFmt as OutputColor layout (planes filled per frame)
    bool direct = false;                      // OutputColor converts (else swscale)
    AVBufferPool* framePool = nullptr;        // buffers of converted pictures (one per picture, all planes)
    int frameBytes = 0;
    AVBufferRef* hwDevice = nullptr;          // VAAPI
    AVBufferRef* hwFrames = nullptr;
    std::vector<float> mixL, mixR;            // fetch thread: the mix of one output frame

    // Timeline frame shown at output frame i (frame rate conversion): the one showing at that time, floor(i * fps_tl /
    // fps_out). Rounding to the nearest even (lrint, as RateConverter did) repeated and dropped frames unevenly (30 ->
    // 60: 2,1,3,1,3 times) and could land one past the range: the last frame black, or the one after Out.
    int64_t timelineFrame(int64_t i) const
    {
        if (o.rate == o.timelineRate) return o.from + i;
        return o.from + int64_t(std::floor(double(i) * o.timelineRate.fps() / o.rate.fps() + 1e-9));
    }

    // Worker threads render output frames [a, b) and hand them over in order; at most `window` frames ahead of the
    // one taken last (memory: frames in flight stay bounded)
    struct Pump {
        Impl* d = nullptr;
        std::vector<std::thread> threads;
        std::mutex mutex;
        std::condition_variable work, ready;
        int first = 0, next = 0, end = 0, taken = 0, window = 4;
        uint64_t generation = 0;
        bool quit = false;
        struct Done {
            AVFrame* picture = nullptr;
            bool repeat = false; // same timeline frame as the one before: the fetcher reuses that picture
        };
        std::map<int, Done> done;
        AVFrame* last = nullptr; // fetcher: the picture taken last (for repeats)

        void startThreads(int count)
        {
            for (int t = 0; t < count; ++t) threads.emplace_back([this] { loop(); });
        }
        void stopThreads()
        {
            {
                std::lock_guard lock(mutex);
                quit = true;
            }
            work.notify_all();
            ready.notify_all();
            for (std::thread& t : threads)
                if (t.joinable()) t.join();
            threads.clear();
            clear();
            av_frame_free(&last);
        }
        void clear()
        {
            for (auto& [i, f] : done) av_frame_free(&f.picture);
            done.clear();
        }
        // Renders [a, b) from now on (what was left of an earlier range is dropped)
        void run(int a, int b)
        {
            {
                std::lock_guard lock(mutex);
                clear();
                first = next = taken = a;
                end = b;
                ++generation;
            }
            work.notify_all();
        }
        void loop()
        {
            for (;;) {
                int i = 0;
                uint64_t gen = 0;
                {
                    std::unique_lock lock(mutex);
                    work.wait(lock, [this] { return quit || d->stop || (next < end && next < taken + window); });
                    if (quit || d->stop) return;
                    i = next++;
                    gen = generation;
                }
                Done result;
                if (i > first && d->timelineFrame(i) == d->timelineFrame(i - 1)) result.repeat = true;
                else result.picture = d->makePicture(i);
                {
                    std::lock_guard lock(mutex);
                    if (gen == generation) {
                        done[i] = result;
                        result.picture = nullptr;
                    }
                }
                av_frame_free(&result.picture);
                ready.notify_all();
            }
        }
        // Output frame i's picture (caller owns it); nullptr = stopped or rendering failed
        AVFrame* take(int i)
        {
            Done got;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return quit || d->stop || done.count(i); });
                if (quit || d->stop) return nullptr;
                got = done[i];
                done.erase(i);
                taken = i + 1;
            }
            work.notify_all();
            if (got.repeat) {
                if (!last) return nullptr;
                got.picture = av_frame_clone(last); // shares the buffer: no copy
                if (got.picture) got.picture->pts = i;
            } else if (!got.picture) {
                return nullptr;
            }
            av_frame_free(&last);
            last = av_frame_clone(got.picture);
            return got.picture;
        }
        void wake()
        {
            work.notify_all();
            ready.notify_all();
        }
    };
    std::unique_ptr<Pump> pump;

    // Worker thread: output frame i rendered and converted into a pooled AVFrame (pts = i); nullptr = failed
    AVFrame* makePicture(int i)
    {
        AVFrame* f = av_frame_alloc();
        if (!f) return nullptr;
        f->format = inputFmt;
        f->width = width;
        f->height = height;
        f->buf[0] = av_buffer_pool_get(framePool);
        if (!f->buf[0]) {
            av_frame_free(&f);
            return nullptr;
        }
        av_image_fill_arrays(f->data, f->linesize, f->buf[0]->data, inputFmt, width, height, 32);
        OutputColor::Target t = target;
        for (int p = 0; p < 4; ++p) {
            t.planes[p] = f->data[p];
            t.strides[p] = f->linesize[p];
        }
        // GPU path: the card writes the encoder's planes itself where it can (8-bit 4:2:0 / NV12)
        Picture::RenderHints hints;
        if (direct) hints.yuv = &t;
        const Picture::Frame frame = o.renderer->render(timelineFrame(i), hints);
        if (frame.isNull() || frame.width() != width || frame.height() != height) {
            av_frame_free(&f);
            return nullptr;
        }
        if (frame.yuvWritten()) {
            // done
        } else if (direct) {
            OutputColor::fromRgba(frame.bits(), width, height, frame.stride(), t);
        } else {
            // Rare formats: swscale, one context per worker thread
            thread_local SwsContext* sws = nullptr;
            sws = sws_getCachedContext(sws, width, height, AV_PIX_FMT_RGBA, width, height, inputFmt, SWS_BICUBIC, nullptr,
                                       nullptr, nullptr);
            if (!sws) {
                av_frame_free(&f);
                return nullptr;
            }
            const int cs = colorspace == 601 ? SWS_CS_ITU601 : colorspace == 2020 ? SWS_CS_BT2020 : SWS_CS_ITU709;
            const int* table = sws_getCoefficients(cs);
            sws_setColorspaceDetails(sws, table, 1, table, 0, 0, 1 << 16, 1 << 16);
            const uint8_t* src[4] = {frame.bits(), nullptr, nullptr, nullptr};
            const int srcStride[4] = {frame.stride(), 0, 0, 0};
            sws_scale(sws, src, srcStride, 0, height, f->data, f->linesize);
        }
        f->pts = i;
        return f;
    }

    // Fetch thread: the mix of output frame i appended to pcm (interleaved, encoder sample type)
    void soundOwn(int i, std::vector<uint8_t>& pcm)
    {
        Audio::Mixer& mixer = *o.mixer;
        int64_t a = 0, b = 0;
        if (o.rate == o.timelineRate) {
            a = mixer.frameStart(o.from + i);
            b = mixer.frameStart(o.from + i + 1);
        } else { // the mixer's rounding on the output's frame grid
            const int64_t base = mixer.frameStart(o.from);
            auto start = [&](int64_t k) {
                const int64_t v = k * int64_t(mixer.rate()) * o.rate.den;
                const int64_t num = 2 * v + o.rate.num, den = 2 * int64_t(o.rate.num);
                return base + (num >= 0 ? num / den : -((-num + den - 1) / den));
            };
            a = start(i);
            b = start(i + 1);
        }
        const int count = int(std::max<int64_t>(0, b - a));
        if (count <= 0) return;
        mixL.resize(size_t(count));
        mixR.resize(size_t(count));
        float* planes[] = {mixL.data(), mixR.data()};
        mixer.render(a, count, planes);
        const int channels = s.audioChannels;
        const size_t at = pcm.size();
        pcm.resize(at + size_t(count) * channels * sampleBytes);
        uint8_t* out = pcm.data() + at;
        const AVSampleFormat packed = av_get_packed_sample_fmt(sampleFmt);
        const int bits = packed == AV_SAMPLE_FMT_S16 ? 16 : std::clamp(o.audioBits > 0 ? o.audioBits : 32, 8, 32);
        for (int k = 0; k < count; ++k)
            for (int c = 0; c < channels; ++c, out += sampleBytes) {
                const double v = channels == 1 ? 0.5 * (double(mixL[k]) + double(mixR[k])) : c == 0 ? mixL[k] : c == 1 ? mixR[k] : 0.0;
                switch (packed) {
                case AV_SAMPLE_FMT_S16: {
                    const auto q = int16_t(std::clamp(std::nearbyint(v * 32768.0), -32768.0, 32767.0));
                    std::memcpy(out, &q, 2);
                    break;
                }
                case AV_SAMPLE_FMT_S32: { // rounded to the file's bits, placed in the top bits
                    const double scale = std::ldexp(1.0, bits - 1);
                    const auto q = int64_t(std::clamp(std::nearbyint(v * scale), -scale, scale - 1));
                    const auto w = int32_t(q * (int64_t(1) << (32 - bits)));
                    std::memcpy(out, &w, 4);
                    break;
                }
                case AV_SAMPLE_FMT_U8: *out = uint8_t(std::clamp(std::nearbyint(v * 128.0 + 128.0), 0.0, 255.0)); break;
                default: { // float
                    const float fv = float(v);
                    std::memcpy(out, &fv, 4);
                    break;
                }
                }
            }
    }

    // Fetch thread: output frames [a, b) in order (picture and sound); restart marks a's picture as the
    // first of an encoded piece (smart render)
    bool fetchOwn(int a, int b, bool restart)
    {
        if (b <= a || stop) return !stop;
        if (pump) pump->run(a, b);
        for (int i = a; i < b && !stop; ++i) {
            Item item;
            if (pump) {
                item.picture = pump->take(i);
                if (!item.picture) {
                    if (!stop) fail(QStringLiteral("no frame %1 from the timeline").arg(i));
                    return false;
                }
                item.restart = restart && i == a;
            }
            if (o.mixer && sound) soundOwn(i, item.pcm);
            if (!push(std::move(item))) return false;
        }
        return !stop;
    }

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
        if (pump) pump->wake();
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

    void fetch()
    {
        if (smart) fetchSmart();
        else fetchOwn(0, s.length, false);
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
        double fps = 0;
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
        // Stream frame position like engine/video/Reader: from the first key frame, rounded
        int64_t position(const AVPacket* p) const
        {
            const int64_t pts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
            return Video::framePosition(pts, firstPts, fmt->start_time, fmt->streams[index]->time_base, fps);
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
            const int64_t ts = Video::frameTimestamp(position, firstPts, fmt->start_time, fmt->streams[index]->time_base, fps);
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
        // What the render path makes of the file (frame rate guess, color, rotation): the copy must show
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
        // As the Reader: frame 0 is the first key frame
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
        if (i != soundNext) {
            fail(QStringLiteral("sound of frame %1 asked, %2 is next").arg(i).arg(soundNext));
            return false;
        }
        if (o.mixer && sound) soundOwn(i, pcm);
        ++soundNext;
        return true;
    }

    // Output frames [a, b) encoded: the timeline from a, one new encoder for the piece
    bool encodePiece(int a, int b)
    {
        if (b <= a || stop) return !stop;
        // The pictures from the pump, the sound in order (nextSound)
        pump->run(a, b);
        for (int i = a; i < b && !stop; ++i) {
            Item item;
            item.picture = pump->take(i);
            if (!item.picture) {
                if (!stop) fail(QStringLiteral("no frame %1 from the timeline").arg(i));
                return false;
            }
            item.restart = i == a;
            if (!nextSound(i, item.pcm) || !push(std::move(item))) return false;
        }
        return !stop;
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
            if (g->whole && s.maxBitrateK > 0) { // "Web" quality: a copy must stay below the cap like the encoder
                int64_t bytes = 0;
                for (const AVPacket* q : g->packets) bytes += q->size;
                if (double(bytes) * 8 / 1000 * fps / g->count > s.maxBitrateK) {
                    g->whole = false;
                    g->why = "above the bit rate cap";
                }
            }
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
        sources.clear();
    }

    bool open()
    {
        const QByteArray path = s.path.toUtf8();
        const QByteArray muxer = s.muxer.toUtf8();
        int r = avformat_alloc_output_context2(&oc, nullptr, muxer.isEmpty() ? nullptr : muxer.constData(), path.constData());
        if (r < 0 || !oc) {
            fail(QStringLiteral("no muxer for %1: %2").arg(s.path, avError(r)));
            return false;
        }
        packet = av_packet_alloc();
        if (!packet) {
            fail(QStringLiteral("out of memory"));
            return false;
        }
        // Video (smart render: one encoder per piece, the stream gets the first item's parameters)
        if (!s.videoCodec.isEmpty()) {
            if (pixFmt == AV_PIX_FMT_VAAPI && !openHardware()) return false;
            vst = avformat_new_stream(oc, nullptr);
            if (!vst) {
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
        if (!(oc->oformat->flags & AVFMT_NOFILE) && (r = avio_open(&oc->pb, path.constData(), AVIO_FLAG_WRITE)) < 0) {
            fail(QStringLiteral("cannot write %1: %2").arg(s.path, avError(r)));
            return false;
        }
        AVDictionary* options = nullptr;
        if (s.fastStart) av_dict_set(&options, "movflags", "+faststart", 0);
        // MP4: B-frames and smart render's decode delay as negative composition offsets (ctts version 1) instead of an
        // edit list. Media Foundation (Windows Media Player) ignores edit lists: the picture came 2-4 frames after the
        // sound there (smart render at 30 fps: 112 ms late); FFmpeg-based players were in sync either way.
        if (std::strcmp(oc->oformat->name, "mp4") == 0)
            av_dict_set(&options, "movflags", "+negative_cts_offsets", AV_DICT_APPEND);
        // HEVC in MP4/MOV as 'hvc1' instead of FFmpeg's default 'hev1': Apple players (QuickTime, Safari, iPhone) play
        // no 'hev1'. Smart render's in-band parameter sets per piece then work as they do in H.264's 'avc1'.
        const bool mov = std::strcmp(oc->oformat->name, "mp4") == 0 || std::strcmp(oc->oformat->name, "mov") == 0;
        if (mov && vst && vst->codecpar->codec_id == AV_CODEC_ID_HEVC) vst->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
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
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(hwFrames ? inputFmt : pixFmt);
        if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_RGB)) {
            venc->colorspace = matrixOf(colorspace, height);
            venc->color_trc = transferOf(colorspace);
            venc->color_primaries = primariesOf(colorspace, height);
            // Limited range (JPEG formats: full)
            venc->color_range = target.fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        }
        if (hwFrames) venc->hw_frames_ctx = av_buffer_ref(hwFrames);
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
            // Codecs with a fixed frame size get the last frame filled with silence
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
            if (!venc) { // audio only
                freeItem(item);
                ++written;
                continue;
            }
            pieceFed = true;
            if (hwFrames && item.picture && !upload(item.picture)) {
                freeItem(item);
                fail(QStringLiteral("cannot upload the picture to the card"), true);
                break;
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

    // VAAPI: the converted NV12 picture onto the card (replaces *picture)
    bool upload(AVFrame*& picture)
    {
        AVFrame* hw = av_frame_alloc();
        if (!hw || av_hwframe_get_buffer(hwFrames, hw, 0) < 0 || av_hwframe_transfer_data(hw, picture, 0) < 0) {
            av_frame_free(&hw);
            return false;
        }
        av_frame_copy_props(hw, picture);
        av_frame_free(&picture);
        picture = hw;
        return true;
    }

    // VAAPI device and frame pool for the encoder; false = not available (fail() called, encoder problem)
    bool openHardware()
    {
        const QByteArray device = s.hwDevice.toUtf8();
        if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_VAAPI, device.isEmpty() ? nullptr : device.constData(), nullptr, 0) < 0) {
            fail(QStringLiteral("VAAPI device %1 cannot be opened").arg(s.hwDevice), true);
            return false;
        }
        hwFrames = av_hwframe_ctx_alloc(hwDevice);
        if (!hwFrames) {
            fail(QStringLiteral("out of memory"), true);
            return false;
        }
        auto* fc = reinterpret_cast<AVHWFramesContext*>(hwFrames->data);
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = width;
        fc->height = height;
        fc->initial_pool_size = 20;
        if (av_hwframe_ctx_init(hwFrames) < 0) {
            av_buffer_unref(&hwFrames);
            fail(QStringLiteral("VAAPI frames cannot be set up"), true);
            return false;
        }
        return true;
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
        avcodec_free_context(&venc); // frees pictures still held by the encoder
        avcodec_free_context(&aenc);
        av_buffer_unref(&hwFrames);
        av_buffer_unref(&hwDevice);
        av_buffer_pool_uninit(&framePool); // freed once the last picture is back
        if (oc) {
            if (oc->pb) avio_closep(&oc->pb);
            avformat_free_context(oc);
            oc = nullptr;
        }
        av_packet_free(&packet);
    }
};

ExportWriter::ExportWriter(const Settings& s, Own own) : d(std::make_unique<Impl>())
{
    d->s = s;
    d->width = own.size.width();
    d->height = own.size.height();
    d->colorspace = own.colorspace;
    // JPEG (JFIF) has no place for the matrix and every reader takes BT.601: written as 709 the colours shifted
    if (s.videoCodec == QLatin1String("mjpeg")) d->colorspace = 601;
    d->rate = AVRational{own.rate.num, std::max(1, own.rate.den)};
    d->fps = std::max(1.0, own.rate.fps());
    d->aspect = AVRational{1, 1};
    if (s.videoCodec.isEmpty()) own.renderer.reset();
    if (s.audioCodec.isEmpty()) own.mixer.reset();
    d->o = std::move(own);
    if (!s.videoCodec.isEmpty()) {
        d->pixFmt = av_get_pix_fmt(s.pixFmt.toUtf8().constData());
        d->inputFmt = d->pixFmt == AV_PIX_FMT_VAAPI ? AV_PIX_FMT_NV12 : d->pixFmt;
        d->target.colorspace = d->colorspace;
        d->direct = OutputColor::targetOf(d->inputFmt, d->target);
        d->target.colorspace = d->colorspace; // targetOf only sets the layout
        d->frameBytes = av_image_get_buffer_size(d->inputFmt, d->width, d->height, 32);
        if (d->frameBytes > 0) d->framePool = av_buffer_pool_init(size_t(d->frameBytes), nullptr);
    }
    if (!s.audioCodec.isEmpty()) {
        d->sampleFmt = av_get_sample_fmt(s.sampleFormat.toUtf8().constData());
        d->sound = soundFormat(d->sampleFmt);
        d->sampleBytes = av_get_bytes_per_sample(d->sampleFmt);
    }
    const int parallel = std::max(1, d->o.parallel);
    d->queueMax = size_t(std::clamp(parallel, 2, 8));
    if (const AVCodec* c = avcodec_find_encoder_by_name(s.videoCodec.toUtf8().constData())) d->codecId = c->id;
    d->smart = !s.copy.isEmpty() && d->o.renderer && d->o.rate == d->o.timelineRate
               && (d->codecId == AV_CODEC_ID_H264 || d->codecId == AV_CODEC_ID_HEVC)
               && (streamFormat(d->pixFmt) == AV_PIX_FMT_YUV420P || streamFormat(d->pixFmt) == AV_PIX_FMT_YUV420P10LE);
    if (d->o.renderer) {
        d->pump = std::make_unique<Impl::Pump>();
        d->pump->d = d.get();
        // A couple of frames more than the workers: one waits for the encoder while the others render
        d->pump->window = parallel + 2;
    }
}

ExportWriter::~ExportWriter()
{
    cancel();
    d->closeEncoders();
}

void ExportWriter::start()
{
    if (d->started) return;
    d->started = true;
    if (d->o.renderer && (!d->framePool || d->frameBytes <= 0)) {
        d->fail(QStringLiteral("pixel format %1 cannot be written").arg(d->s.pixFmt));
        return;
    }
    if (d->pump) d->pump->startThreads(std::max(1, d->o.parallel));
    d->encoder = std::thread([this] { d->encode(); });
    d->fetcher = std::thread([this] { d->fetch(); });
}

void ExportWriter::cancel()
{
    if (d->state == State::Running) d->stop = true;
    d->cond.notify_all();
    if (d->pump) d->pump->wake();
    // The fetch thread may wait for the frame being rendered: the workers still run and deliver it
    if (d->fetcher.joinable()) d->fetcher.join();
    if (d->pump) d->pump->stopThreads();
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
