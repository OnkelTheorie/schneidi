#include "engine/audio/Reader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>
#include <limits>
#include <numeric>
#include <vector>

namespace Audio {

namespace {

constexpr double kPreroll = 0.2;     // decoded before a jump target: codec (AAC/MP3/Opus) and resampler settle
constexpr double kJitter = 0.010;    // timestamp deviations below this are ignored (like aresample min_hard_comp)
constexpr double kMaxForward = 2.0;  // forward jumps up to this far decode on instead of seeking
constexpr double kKeepBack = 0.25;   // decoded samples kept before the last read (small steps back without a seek)
constexpr double kBackoff[] = {1.0, 4.0}; // seek further back when the first packet came too late
constexpr int kProbePackets = 2000;  // read at most this many packets to find the picture's first key frame
constexpr int kGridFrames = 24;      // frames measured in a stream with coarse timestamps (Matroska)
constexpr int kSilenceChunk = 4096;
// 16-bit WAV: FFmpeg looks for DTS/AC3 hidden in the PCM in the first 32 packets (2.7 s, 20 ms per stereo file, 70 ms
// for 5.1); 64 KB find it just as well
constexpr int64_t kCodecProbeBytes = 1 << 16;
constexpr int64_t kFar = -(int64_t(1) << 42); // "long before anything" (years), safe to multiply by a grid step

int64_t floorDiv(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

QString errorText(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

} // namespace

struct Reader::Impl {
    AVFormatContext* fmt = nullptr;
    QByteArray path;
    bool streamInfo = false; // the header lacked the sound's values: stream analysis on (re)opening
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int index = -1;
    AVRational tb{0, 1};
    long double zero = 0; // position 0 in seconds of the file's time
    bool zeroKnown = false;
    int outRate = 48000;
    int outChannels = 0;
    StreamInfo info;
    Stats stats;
    bool error = false;
    std::deque<AVPacket*> queued; // packets of the stream read while looking for the picture's first key frame

    // Resampler input as the decoded frames come (HE-AAC may differ from the container's values)
    int inRate = 0;
    int inFormat = -1;
    AVChannelLayout frameLayout{}; // as the decoder states it (may be unspecified)
    AVChannelLayout inLayout{};    // what the resampler gets
    AVChannelLayout outLayout{};
    int64_t gridIn = 1, gridOut = 1; // the resampler starts on multiples of these: same samples for any start
    std::vector<uint8_t*> silence;   // kSilenceChunk silent input samples

    // Coarse timestamps (Matroska: whole milliseconds = 48 samples): where the frames really are, measured once
    bool grid = false;
    int64_t frame0 = 0, frame1 = 0; // first and second decoded frame (source samples, relative to position 0)
    int64_t frameSize = 0;          // later frames follow at this step; 0 = sizes differ (no snapping)
    long double tickSamples = 0;
    // Priming the decoder does not drop (Matroska AAC: CodecDelay without skip data): samples before primingEnd
    // (source samples relative to position 0) are dropped here
    bool skipChecked = false;
    int ownSkip = 0;
    int64_t primingEnd = kFar;
    int64_t firstTs = AV_NOPTS_VALUE; // first packet of the stream (AV_TIME_BASE): a jump to the start goes there
    int64_t firstFrame0 = 0;          // position of the first decoded frame (source samples)

    // Position of the decode
    bool positioned = false;
    bool started = false;  // resampler running, srcNext/outNext valid
    bool demuxEnd = false;
    bool ended = false;    // decoder and resampler drained: nothing after outNext
    bool fromStart = false;
    bool seekFirst = false; // no frame decoded since the seek yet
    int attempt = 0;
    int64_t wantOut = 0;   // seek target minus pre-roll: the resampler starts here
    int64_t validFrom = 0; // seek target: earlier samples are still settling and never handed out
    int64_t srcNext = 0, outNext = 0;
    bool firstFrame = true; // the stream's first frame (sets info.start)

    // Decoded output [bufStart, outNext); [silentFrom, bufStart) is known silence
    std::vector<std::vector<float>> buf;
    int64_t bufStart = 0;
    int64_t silentFrom = 0;
    int64_t keepFrom = kFar;
    std::vector<std::vector<float>> tmp;
    std::vector<uint8_t*> tmpPtrs;

    ~Impl()
    {
        for (AVPacket* p : queued) av_packet_free(&p);
        freeSilence();
        swr_free(&swr);
        av_channel_layout_uninit(&frameLayout);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }

    int64_t seconds(double s) const { return std::llround(s * outRate); }

    void freeSilence()
    {
        if (!silence.empty()) av_freep(&silence[0]);
        silence.clear();
    }

    // Next packet of the stream into `pkt`; false at the end of the file
    bool nextPacket()
    {
        if (!queued.empty()) {
            AVPacket* p = queued.front();
            queued.pop_front();
            av_packet_move_ref(pkt, p);
            av_packet_free(&p);
            return true;
        }
        for (;;) {
            const int r = av_read_frame(fmt, pkt);
            if (r == AVERROR(EAGAIN)) continue;
            if (r < 0) {
                if (r != AVERROR_EOF) ++stats.decodeErrors;
                return false;
            }
            if (pkt->stream_index != index) {
                av_packet_unref(pkt);
                continue;
            }
            notePacket(pkt);
            return true;
        }
    }

    // Every packet of the stream read from the file passes here
    void notePacket(const AVPacket* p)
    {
        // FFmpeg probes some streams while reading (16-bit WAV: DTS/AC3 stored as PCM) and may change the codec
        if (dec && fmt->streams[index]->codecpar->codec_id != dec->codec_id && !openDecoder()) error = true;
        if (skipChecked) return;
        skipChecked = true; // the stream's first packet: does the decoder drop the priming itself?
        int64_t ts = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
        if (p->pts != AV_NOPTS_VALUE && ts != AV_NOPTS_VALUE) ts = std::min(ts, p->pts);
        if (ts != AV_NOPTS_VALUE) firstTs = av_rescale_q(ts, tb, AV_TIME_BASE_Q);
        const int padding = fmt->streams[index]->codecpar->initial_padding;
        size_t size = 0;
        if (padding > 0 && !av_packet_get_side_data(p, AV_PKT_DATA_SKIP_SAMPLES, &size)) ownSkip = padding;
    }

    // Coarse timestamps: every frame's rounded timestamp narrows down where frame 0 really is (its decoded samples
    // follow without gaps). Within that range the sound's real start is put on a whole tick when possible – muxers
    // start streams there (an AAC stream written at 0 is stored as -21 ms because of its 1024 priming samples).
    // Afterwards a frame found after a jump is snapped onto the frame grid: sample-exact jumps in Matroska.
    void measureGrid()
    {
        long double lo = -std::numeric_limits<long double>::infinity(), hi = -lo;
        int64_t decoded = 0;
        int rate = 0;
        std::vector<int> sizes;
        // The packets are decoded again afterwards (no seek back: Matroska drops packets before the first key frame
        // of the picture when seeking to the start, e.g. the AAC priming at -21 ms)
        std::deque<AVPacket*> used;
        while (int(sizes.size()) < kGridFrames && nextPacket()) {
            used.push_back(av_packet_clone(pkt));
            const int r = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            if (r < 0) continue;
            while (avcodec_receive_frame(dec, frame) == 0) {
                const int64_t pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
                if (!rate) rate = frame->sample_rate;
                if (pts != AV_NOPTS_VALUE && frame->nb_samples > 0 && frame->sample_rate == rate) {
                    const long double s = (long double)pts * tb.num / tb.den * rate - decoded; // file time of frame 0
                    lo = std::max(lo, s);
                    hi = std::min(hi, s);
                    sizes.push_back(frame->nb_samples);
                }
                decoded += frame->nb_samples;
                av_frame_unref(frame);
            }
        }
        if (!openDecoder()) error = true;
        queued.insert(queued.begin(), used.begin(), used.end());
        if (rate <= 0 || sizes.empty()) return;
        const long double tick = (long double)tb.num * rate / tb.den;
        lo -= tick / 2;
        hi += tick / 2;
        if (lo > hi + 1) return; // the timestamps do not follow the sample counts (gaps, broken): plain timestamps
        const long double rlo = lo + ownSkip, rhi = hi + ownSkip, mid = (rlo + rhi) / 2;
        long double real = mid;
        for (long double k = std::ceil(rlo / tick); k * tick <= rhi; ++k)
            if (std::abs(k * tick - mid) < std::abs(real - mid) || real == mid) real = k * tick;
        if (!zeroKnown) { // file without picture: its first real sample is position 0
            zero = real / rate;
            zeroKnown = true;
        }
        frame0 = std::llround(real - ownSkip - zero * rate);
        primingEnd = frame0 + ownSkip;
        frame1 = frame0 + sizes[0];
        frameSize = 0;
        if (sizes.size() >= 3 && std::all_of(sizes.begin() + 1, sizes.end() - 1, [&](int n) { return n == sizes[1]; }))
            frameSize = sizes[1];
        tickSamples = tick;
        grid = true;
    }

    bool configure(const AVFrame* f)
    {
        swr_free(&swr);
        freeSilence();
        av_channel_layout_uninit(&frameLayout);
        av_channel_layout_copy(&frameLayout, &f->ch_layout);
        av_channel_layout_uninit(&inLayout);
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels <= 0)
            av_channel_layout_default(&inLayout, std::max(1, f->ch_layout.nb_channels));
        else
            av_channel_layout_copy(&inLayout, &f->ch_layout);
        if (outChannels <= 0) outChannels = inLayout.nb_channels;
        if (outLayout.nb_channels != outChannels) {
            av_channel_layout_uninit(&outLayout);
            if (inLayout.nb_channels == outChannels) av_channel_layout_copy(&outLayout, &inLayout);
            else av_channel_layout_default(&outLayout, outChannels);
        }
        if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLTP, outRate, &inLayout, AVSampleFormat(f->format),
                                f->sample_rate, 0, nullptr) < 0)
            return false;
        av_opt_set_int(swr, "exact_rational", 1, 0); // a rational step: the phase never drifts
        if (swr_init(swr) < 0) {
            swr_free(&swr);
            return false;
        }
        inRate = f->sample_rate;
        inFormat = f->format;
        const int64_t g = std::gcd(int64_t(inRate), int64_t(outRate));
        gridIn = inRate / g;
        gridOut = outRate / g;
        silence.assign(size_t(inLayout.nb_channels), nullptr);
        if (av_samples_alloc(silence.data(), nullptr, inLayout.nb_channels, kSilenceChunk, AVSampleFormat(inFormat), 0) < 0) {
            silence.clear();
            return false;
        }
        av_samples_set_silence(silence.data(), 0, kSilenceChunk, inLayout.nb_channels, AVSampleFormat(inFormat));
        tmpPtrs.assign(size_t(outChannels), nullptr);
        tmp.resize(size_t(outChannels));
        if (buf.size() != size_t(outChannels)) buf.assign(size_t(outChannels), {});
        return true;
    }

    // Source sample position of a decoded frame from its timestamp; false without one
    bool position(const AVFrame* f, int64_t& sp) const
    {
        int64_t pts = f->pts;
        if (pts == AV_NOPTS_VALUE) pts = f->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) return false;
        const long double s = ((long double)pts * tb.num / tb.den - zero) * inRate;
        sp = std::llround(s);
        if (grid) {
            const long double tol = tickSamples / 2 + 1;
            if (std::abs(s - frame0) <= tol) {
                sp = frame0;
            } else if (frameSize > 0) {
                const int64_t cand = frame1 + std::llround((s - frame1) / frameSize) * frameSize;
                if (std::abs((long double)cand - s) <= tol) sp = cand;
            }
        }
        return true;
    }

    void append(int count)
    {
        if (count <= 0) return;
        int skip = 0;
        if (outNext < keepFrom) skip = int(std::min<int64_t>(count, keepFrom - outNext));
        if (buf[0].empty()) bufStart = outNext + skip;
        if (skip > 0) silentFrom = std::max(silentFrom, bufStart);
        for (int c = 0; c < outChannels; ++c)
            buf[size_t(c)].insert(buf[size_t(c)].end(), tmp[size_t(c)].begin() + skip, tmp[size_t(c)].begin() + count);
        outNext += count;
    }

    void convert(const uint8_t** in, int n)
    {
        const int cap = swr_get_out_samples(swr, n);
        if (cap <= 0) {
            if (in) swr_convert(swr, nullptr, 0, in, n); // buffered inside
            return;
        }
        for (int c = 0; c < outChannels; ++c) {
            if (int(tmp[size_t(c)].size()) < cap) tmp[size_t(c)].resize(size_t(cap));
            tmpPtrs[size_t(c)] = reinterpret_cast<uint8_t*>(tmp[size_t(c)].data());
        }
        const int got = swr_convert(swr, tmpPtrs.data(), cap, in, n);
        if (got < 0) {
            error = true;
            return;
        }
        append(got);
    }

    void feedSilence(int64_t n)
    {
        while (n > 0) {
            const int k = int(std::min<int64_t>(n, kSilenceChunk));
            convert(const_cast<const uint8_t**>(silence.data()), k);
            srcNext += k;
            n -= k;
        }
    }

    void feed(const AVFrame* f, int offset)
    {
        const int n = f->nb_samples - offset;
        if (n <= 0) return;
        const int bps = av_get_bytes_per_sample(AVSampleFormat(f->format));
        const int ch = f->ch_layout.nb_channels;
        std::vector<const uint8_t*> in;
        if (av_sample_fmt_is_planar(AVSampleFormat(f->format))) {
            for (int c = 0; c < ch; ++c) in.push_back(f->extended_data[c] + size_t(offset) * size_t(bps));
        } else {
            in.push_back(f->extended_data[0] + size_t(offset) * size_t(bps) * size_t(ch));
        }
        convert(in.data(), n);
        srcNext += n;
    }

    void drainResampler()
    {
        if (!swr || !started) return;
        for (int i = 0; i < 64; ++i) {
            const int64_t before = outNext;
            convert(nullptr, 0);
            if (outNext == before || error) break;
        }
    }

    enum class Placed { Fed, Skipped, Retry, Reopen };

    // Starts the resampler for the first usable frame after a seek
    Placed start(int64_t sp, int nb)
    {
        const int64_t want = floorDiv(wantOut, gridOut) * gridIn; // on the grid: an exact source position
        const int64_t jitter = std::llround(kJitter * inRate);
        if (!fromStart && sp > want + jitter && attempt < int(std::size(kBackoff))) return Placed::Retry;
        if (sp + nb <= want) return Placed::Skipped; // before the pre-roll: only warms the decoder up
        int64_t s0 = want;
        bool gap = false;
        const int64_t preIn = std::llround(kPreroll * inRate);
        if (sp - s0 > preIn) { // long silence before the frame (stream start, gap): do not feed all of it
            s0 = floorDiv(sp - preIn, gridIn) * gridIn;
            gap = true;
        }
        srcNext = s0;
        outNext = s0 / gridIn * gridOut;
        for (auto& b : buf) b.clear();
        bufStart = outNext;
        // Silence known between the target and the first output: everything before the stream, or the gap
        silentFrom = fromStart ? kFar : (gap ? validFrom : outNext);
        started = true;
        return Placed::Fed;
    }

    Placed handleFrame(const AVFrame* f)
    {
        if (f->nb_samples <= 0 || f->sample_rate <= 0) return Placed::Skipped;
        if (!swr || f->sample_rate != inRate || f->format != inFormat || av_channel_layout_compare(&f->ch_layout, &frameLayout) != 0) {
            // Rate/layout change inside the stream (rare): finish the old part, restart at this frame's timestamp
            const bool wasStarted = started;
            drainResampler();
            const int64_t outPos = outNext;
            if (!configure(f)) {
                error = true;
                return Placed::Skipped;
            }
            if (wasStarted) {
                started = false;
                int64_t sp = 0;
                if (!position(f, sp)) sp = outPos * inRate / outRate;
                // continue at the next grid point of the new rate after what is already out
                srcNext = floorDiv(outPos * inRate + outRate - 1, outRate);
                srcNext = floorDiv(srcNext + gridIn - 1, gridIn) * gridIn;
                outNext = srcNext / gridIn * gridOut;
                if (buf[0].empty()) bufStart = outNext;
                else // keep the buffer contiguous
                    for (auto& b : buf) b.resize(b.size() + size_t(outNext - outPos), 0.f);
                started = true;
                if (sp > srcNext) feedSilence(sp - srcNext);
                const int off = sp < srcNext ? int(std::min<int64_t>(srcNext - sp, f->nb_samples)) : 0;
                feed(f, off);
                return Placed::Fed;
            }
        }
        if (firstFrame && !zeroKnown) { // file without picture: its first real sample is position 0
            int64_t pts = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
            zero = (pts == AV_NOPTS_VALUE ? 0 : (long double)pts * tb.num / tb.den) + (long double)ownSkip / f->sample_rate;
            zeroKnown = true;
        }
        int64_t sp = 0;
        const bool hasPos = position(f, sp);
        if (!started) {
            if (!hasPos) sp = fromStart ? 0 : floorDiv(wantOut * inRate, outRate);
            const bool first = seekFirst;
            seekFirst = false;
            if (firstFrame) {
                firstFrame0 = sp;
                if (!grid) primingEnd = sp + ownSkip;
            } else if (first && fromStart && attempt == 0 && sp > firstFrame0 + std::llround(tickSamples / 2) + 1) {
                return Placed::Reopen; // the jump to the start skipped packets (they warm the decoder up)
            }
            int cut = 0;
            if (sp < primingEnd) { // priming at the file start
                cut = int(std::min<int64_t>(primingEnd - sp, f->nb_samples));
                if (cut == f->nb_samples) return Placed::Skipped;
                sp += cut;
            }
            if (firstFrame) {
                firstFrame = false;
                info.start = std::llround((long double)sp * outRate / inRate);
            }
            const Placed p = start(sp, f->nb_samples - cut);
            if (p != Placed::Fed) return p;
            if (sp > srcNext) feedSilence(sp - srcNext);
            feed(f, cut + (sp < srcNext ? int(srcNext - sp) : 0));
            return Placed::Fed;
        }
        if (!hasPos) sp = srcNext;
        const int64_t diff = sp - srcNext;
        const int64_t jitter = std::llround(kJitter * inRate);
        if (diff > jitter) {
            ++stats.gaps;
            feedSilence(diff);
            feed(f, 0);
        } else if (diff < -jitter) {
            ++stats.overlaps;
            const int64_t cut = -diff;
            if (cut >= f->nb_samples) return Placed::Skipped;
            feed(f, int(cut));
        } else {
            feed(f, 0);
        }
        return Placed::Fed;
    }

    // Decoder as after opening the file. avcodec_flush_buffers keeps some state (AAC: the previous window shape),
    // which changes the first frame slightly – a clip from the file start must sound the same however it is reached
    bool openDecoder()
    {
        avcodec_free_context(&dec);
        const AVStream* st = fmt->streams[index];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) return false;
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0) return false;
        dec->pkt_timebase = st->time_base;
        dec->thread_count = 1;
        return avcodec_open2(dec, codec, nullptr) >= 0;
    }

    // A fresh demuxer at the file start (some demuxers drop packets before the picture's first key frame on a seek)
    bool reopen()
    {
        ++stats.reopens;
        avformat_close_input(&fmt);
        if (avformat_open_input(&fmt, path.constData(), nullptr, nullptr) < 0) return false;
        if (streamInfo && avformat_find_stream_info(fmt, nullptr) < 0) return false;
        if (index >= int(fmt->nb_streams)) return false;
        fmt->probesize = std::min(fmt->probesize, kCodecProbeBytes);
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
            if (int(i) != index) fmt->streams[i]->discard = AVDISCARD_ALL;
        return true;
    }

    void seek(int64_t target, int tryNo, bool fresh = false)
    {
        ++stats.seeks;
        attempt = tryNo;
        validFrom = target;
        wantOut = target - seconds(kPreroll);
        const long double back = tryNo > 0 ? kBackoff[tryNo - 1] : 0;
        const long double t = zero + (long double)wantOut / outRate - back;
        // The start = the stream's first packet (an AAC priming packet lies before the file's start time)
        int64_t begin = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time : 0;
        if (firstTs != AV_NOPTS_VALUE) begin = std::min(begin, firstTs);
        const int64_t ts = std::llround(t * AV_TIME_BASE);
        fromStart = fresh || wantOut <= info.start || ts <= begin;
        int r = -1;
        if (!fromStart) r = av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0 && fresh) {
            r = reopen() ? 0 : -1;
        } else if (r < 0) {
            fromStart = true;
            r = av_seek_frame(fmt, -1, begin, AVSEEK_FLAG_BACKWARD);
            if (r < 0) r = avformat_seek_file(fmt, index, INT64_MIN, INT64_MIN, INT64_MIN, 0);
            if (r < 0) r = reopen() ? 0 : -1;
        }
        if (r < 0) error = true;
        for (AVPacket* p : queued) av_packet_free(&p);
        queued.clear();
        if (fromStart) {
            if (!openDecoder()) error = true;
        } else {
            avcodec_flush_buffers(dec);
        }
        swr_free(&swr);
        inRate = 0;
        inFormat = -1;
        started = false;
        demuxEnd = false;
        ended = false;
        for (auto& b : buf) b.clear();
        bufStart = outNext = validFrom;
        silentFrom = std::numeric_limits<int64_t>::max();
        keepFrom = validFrom;
        positioned = true;
        seekFirst = true;
    }

    // Decodes one packet (or finishes the stream); false when nothing more comes
    bool step()
    {
        if (ended) return false;
        if (!demuxEnd) {
            if (nextPacket()) {
                const int r = avcodec_send_packet(dec, pkt);
                av_packet_unref(pkt);
                if (r < 0 && r != AVERROR(EAGAIN)) ++stats.decodeErrors;
                receive();
                return true;
            }
            demuxEnd = true;
            avcodec_send_packet(dec, nullptr);
            receive();
            if (!demuxEnd) return true; // a retry seek happened while draining
        }
        drainResampler();
        if (!started) { // nothing after the seek target: all silence
            for (auto& b : buf) b.clear();
            bufStart = outNext = validFrom;
            silentFrom = validFrom;
        }
        ended = true;
        return false;
    }

    void receive()
    {
        for (;;) {
            const int r = avcodec_receive_frame(dec, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
            if (r < 0) {
                ++stats.decodeErrors;
                return;
            }
            const Placed p = handleFrame(frame);
            av_frame_unref(frame);
            if (p == Placed::Retry || p == Placed::Reopen) {
                seek(validFrom, attempt + 1, p == Placed::Reopen);
                return;
            }
        }
    }

    void trim()
    {
        if (buf.empty() || buf[0].empty() || bufStart >= keepFrom) return;
        const int64_t n = std::min<int64_t>(int64_t(buf[0].size()), keepFrom - bufStart);
        for (auto& b : buf) b.erase(b.begin(), b.begin() + n);
        bufStart += n;
        silentFrom = bufStart;
    }
};

Reader::Reader() : d(std::make_unique<Impl>()) {}
Reader::~Reader() = default;

std::unique_ptr<Reader> Reader::open(const QString& path, int stream, const Options& options, QString* error)
{
    auto fail = [&](const QString& why) -> std::unique_ptr<Reader> {
        if (error) *error = why;
        return nullptr;
    };
    std::unique_ptr<Reader> r(new Reader);
    Impl& d = *r->d;
    d.outRate = options.rate > 0 ? options.rate : 48000;
    d.outChannels = options.channels;
    int e = avformat_open_input(&d.fmt, path.toUtf8().constData(), nullptr, nullptr);
    if (e < 0) return fail(errorText(e));

    auto audioStreams = [&] {
        std::vector<int> idx;
        for (unsigned i = 0; i < d.fmt->nb_streams; ++i)
            if (d.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) idx.push_back(int(i));
        return idx;
    };
    std::vector<int> idx = audioStreams();
    auto complete = [&](int i) {
        const AVCodecParameters* p = d.fmt->streams[i]->codecpar;
        return p->codec_id != AV_CODEC_ID_NONE && p->sample_rate > 0 && p->ch_layout.nb_channels > 0;
    };
    // The stream analysis decodes pictures (the slow part of opening): only when the header lacks the sound's values
    d.path = path.toUtf8();
    if (stream < 0 || stream >= int(idx.size()) || !complete(idx[size_t(stream)])) {
        d.streamInfo = true;
        e = avformat_find_stream_info(d.fmt, nullptr);
        if (e < 0) return fail(errorText(e));
        idx = audioStreams();
    }
    if (stream < 0 || stream >= int(idx.size())) return fail(QStringLiteral("no audio stream %1").arg(stream));
    d.index = idx[size_t(stream)];
    d.fmt->probesize = std::min(d.fmt->probesize, kCodecProbeBytes);
    AVStream* st = d.fmt->streams[d.index];
    d.tb = st->time_base;

    int video = -1;
    for (unsigned i = 0; i < d.fmt->nb_streams && video < 0; ++i) {
        const AVStream* s = d.fmt->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(s->disposition & AV_DISPOSITION_ATTACHED_PIC)) video = int(i);
    }
    for (unsigned i = 0; i < d.fmt->nb_streams; ++i)
        if (int(i) != d.index && int(i) != video) d.fmt->streams[i]->discard = AVDISCARD_ALL;

    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return fail(QStringLiteral("no decoder for %1").arg(avcodec_get_name(st->codecpar->codec_id)));
    if (!d.openDecoder()) return fail(QStringLiteral("cannot open the %1 decoder").arg(codec->name));
    d.pkt = av_packet_alloc();
    d.frame = av_frame_alloc();
    d.info.hasPicture = video >= 0;

    // Position 0 = the picture's first key frame, as MLT shows it (producer_avformat find_first_pts: a negative
    // DTS there means 0). The sound read meanwhile waits in the queue.
    if (video >= 0) {
        const AVStream* vs = d.fmt->streams[video];
        for (int n = 0; n < kProbePackets && !d.zeroKnown; ++n) {
            const int rr = av_read_frame(d.fmt, d.pkt);
            if (rr == AVERROR(EAGAIN)) continue;
            if (rr < 0) break;
            if (d.pkt->stream_index == video && (d.pkt->flags & AV_PKT_FLAG_KEY)) {
                int64_t first = d.pkt->pts != AV_NOPTS_VALUE ? d.pkt->pts : d.pkt->dts;
                if (d.pkt->dts != AV_NOPTS_VALUE && d.pkt->dts < 0) first = 0;
                if (first != AV_NOPTS_VALUE) {
                    d.zero = (long double)first * vs->time_base.num / vs->time_base.den;
                    d.zeroKnown = true;
                }
            } else if (d.pkt->stream_index == d.index) {
                d.notePacket(d.pkt);
                d.queued.push_back(av_packet_clone(d.pkt));
            }
            av_packet_unref(d.pkt);
        }
        if (!d.zeroKnown) {
            const int64_t s = vs->start_time != AV_NOPTS_VALUE ? av_rescale_q(vs->start_time, vs->time_base, AV_TIME_BASE_Q)
                                                               : (d.fmt->start_time != AV_NOPTS_VALUE ? d.fmt->start_time : 0);
            d.zero = (long double)s / AV_TIME_BASE;
            d.zeroKnown = true;
        }
        d.fmt->streams[video]->discard = AVDISCARD_ALL;
    }

    // Matroska and other containers with timestamps coarser than a sample
    const AVCodecParameters* par = st->codecpar;
    if (par->sample_rate > 0 && (long double)d.tb.num * par->sample_rate / d.tb.den > 1.5) d.measureGrid();

    // Decode up to the first sound: the real rate/channels, where the stream starts. The decode stays positioned at
    // the file start (a clip from the start reads on without a seek).
    d.positioned = true;
    d.fromStart = true;
    d.validFrom = d.wantOut = kFar;
    d.keepFrom = kFar;
    d.silentFrom = std::numeric_limits<int64_t>::max();
    while (!d.started && d.step()) {}
    if (!d.started) return fail(d.error ? QStringLiteral("cannot decode the audio") : QStringLiteral("no audio samples"));
    d.info.codec = QString::fromUtf8(d.dec->codec->name);
    d.info.sourceRate = d.inRate;
    d.info.sourceChannels = d.inLayout.nb_channels;
    // Valid output begins with the stream (the pre-roll silence before it is not handed out)
    d.validFrom = d.info.start;
    d.keepFrom = d.info.start;
    d.trim();
    if (st->duration != AV_NOPTS_VALUE)
        d.info.end = d.info.start + std::llround((long double)st->duration * d.tb.num / d.tb.den * d.outRate);
    else if (d.fmt->duration != AV_NOPTS_VALUE)
        d.info.end = std::llround(((long double)(d.fmt->duration + std::max<int64_t>(0, d.fmt->start_time == AV_NOPTS_VALUE ? 0 : d.fmt->start_time)) / AV_TIME_BASE - d.zero) * d.outRate);
    else
        d.info.end = d.info.start;
    return r;
}

const StreamInfo& Reader::info() const { return d->info; }
int Reader::rate() const { return d->outRate; }
int Reader::channels() const { return d->outChannels; }
const Reader::Stats& Reader::stats() const { return d->stats; }

bool Reader::read(int64_t pos, int count, float* const* out)
{
    if (count <= 0) return !d->error;
    for (int c = 0; c < d->outChannels; ++c) std::fill(out[c], out[c] + count, 0.f);
    const int64_t a = std::max(pos, d->info.start), b = pos + count;
    if (a >= b) return !d->error;
    if (d->ended && a >= d->outNext && a >= d->silentFrom) return !d->error;
    const bool reachable = d->positioned && d->started && a >= d->silentFrom && a <= d->outNext + d->seconds(kMaxForward);
    if (!reachable && !(d->ended && a >= d->silentFrom)) d->seek(a, 0);
    d->keepFrom = std::max(d->validFrom, a - d->seconds(kKeepBack));
    d->trim();
    while ((!d->started || d->outNext < b) && d->step()) {}
    if (d->buf.empty() || d->buf[0].empty()) return !d->error;
    const int64_t from = std::max(a, d->bufStart), to = std::min(b, d->bufStart + int64_t(d->buf[0].size()));
    for (int c = 0; c < d->outChannels && from < to; ++c)
        std::copy(d->buf[size_t(c)].begin() + (from - d->bufStart), d->buf[size_t(c)].begin() + (to - d->bufStart), out[c] + (from - pos));
    return !d->error;
}

} // namespace Audio
