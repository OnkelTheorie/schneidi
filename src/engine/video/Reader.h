// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Own video decoder (plan 0.11 point 7): reads the pictures of one video stream with the FFmpeg libraries, in a
// thread of its own that decodes a few frames ahead of the last request. MLT's avformat producer decoded inside
// get_image, under the producer's lock: one picture at a time, and nothing while the graph composited the last one.
// Optional hardware decoding (engine/video/Hardware); the pictures are software frames either way.
//
// Frame choice as in MLT's producer_avformat (7.30) so the same position shows the same picture: positions count in
// the stream's frame rate (MLT's guess, see Info::frameRate) from the first key frame; a request for position p
// returns the first decoded picture at p or later. Jumps back, or more than 64 frames forward, seek to the key frame
// before (a few frames earlier still for codecs with B-frames) and decode from there. At the end of the stream the last
// picture.
//
// A reader unused for a while (4 s) that is not among the 4 most recently used closes its decoder and drops its pictures,
// like MLT's cache of open producers; the next request opens it again (and seeks).
//
// Thread-safe: any number of threads may ask at once (MLT's render workers). No GUI dependency.

#include <QString>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

struct AVCodecParameters;
struct AVFormatContext;
struct AVFrame;
struct AVStream;

namespace Video {

struct Info {
    int index = -1;               // absolute stream index
    int width = 0, height = 0;    // decoder size (before rotation), as MLT's video_codec->width/height
    AVPixelFormat format = AV_PIX_FMT_NONE; // software format of the pictures (stream format)
    AVRational frameRate{0, 1};   // MLT's guess_frame_rate (meta.media.frame_rate_num/den)
    AVRational timeBase{0, 1};
    AVRational sar{1, 1};         // sample aspect ratio (1:1 if unknown)
    double rotation = 0;          // degrees clockwise from the metadata; pictures come rotated already
    int colorspace = 709;         // MLT numbers: 601, 709, 240, 2020, 2021
    int colorTrc = 0;             // AVColorTransferCharacteristic
    int colorPrimaries = 709;     // MLT numbers: 709, 601525, 601625, 2020
    bool fullRange = false;
    bool hasBFrames = false;
    int64_t duration = 0;         // container duration in AV_TIME_BASE units, < 0 if unknown
    QString codec;                // decoder name
    int fieldOrder = 0;           // the stream's AVFieldOrder (0 unknown, 1 progressive, else interlaced)
};

struct Picture {
    AVFrame* frame = nullptr;     // software, upright; format = Info::format
    int64_t position = 0;         // stream frame position
    bool progressive = true;
    bool topFieldFirst = false;
    ~Picture();
};

// Is the top field shown first? As the decoder flags an interlaced picture `frame` (may be null), else as the stream's
// AVFieldOrder `fieldOrder` states it; nullopt when neither says (progressive or unknown). The decoder's flag goes
// first: H.264 in MP4/MOV/TS states the parser's guess (TT also for a bottom-first stream without timing SEI), while
// the decoder knows the order from the fields' picture order counts. libavcodec/defs.h documents TB as "top coded
// first, bottom displayed first", but FFmpeg itself treats TB as top field first and BT as bottom first: the ffmpeg
// program writes TB/BT for top/bottom-first pictures (fftools/ffmpeg_enc.c; a bottom-first H.264 in MKV reads back as
// BT), the rawvideo, v210 and FFV1 decoders flag TB pictures top-first, the yuv4mpeg, AVI and MXF muxers write TB as
// top-first. Files carry that meaning, so it is followed here (TT/TB top first, BB/BT bottom first).
std::optional<bool> topFieldShownFirst(const AVFrame* frame, int fieldOrder);

// Degrees clockwise the stream's pictures are turned to show them upright (rotation tag or display matrix), in [0, 360)
double streamRotation(const AVStream* stream);
// Quarter turns clockwise the pictures of a rotation (streamRotation) are turned by, as MLT's autorotate (transpose
// clock / hflip+vflip / transpose cclock): 0 for anything that is no quarter turn (45 degrees stays as it is)
int quarterTurns(double degrees);

// Would a reader with Options::hardware decode this stream on the graphics card? A guess before decoding, for memory
// estimates (a software decoder keeps pictures per thread, a hardware one runs with one thread): false when hardware
// decoding is off or no device for the codec is open yet (never opens one: Hardware::deviceReady), for streams the reader leaves to software anyway (small, other
// formats), H.264 above 4096 pixels (no H.264 decoder of a graphics card takes it: NVDEC "width not within range")
// and AV1 (many cards cannot; counted as software, the safe side)
bool likelyHardware(const AVCodecParameters* par);

// Stream frame position of timestamp `pts` (stream time base) as MLT's producer counts it: from `firstPts` (the first
// key frame, stream time base), else from the container start `startTime` (AV_TIME_BASE units, AV_NOPTS_VALUE =
// none), rounded to the nearest frame. Shared by the reader and smart render so both count the same frames.
int64_t framePosition(int64_t pts, int64_t firstPts, int64_t startTime, AVRational timeBase, double fps);
// The stream timestamp of position `position` (the inverse, unrounded: where a seek aims)
int64_t frameTimestamp(int64_t position, int64_t firstPts, int64_t startTime, AVRational timeBase, double fps);

// Seconds from the container start (what FFmpeg turns into 0 when it converts the file without -copyts) to the
// timestamp the reader counts as position 0 (the first key frame of the video stream); nullopt if the file has no
// video. A game recording whose sound starts a frame before the picture gives ~0.033.
std::optional<double> zeroOffset(const QString& path);

class Reader {
public:
    struct Options {
        int threads = 0;          // decoder threads (0 = FFmpeg's choice), as MLT's "threads"
        bool hardware = true;     // try the hardware decoder (engine/video/Hardware)
        int ahead = 6;            // pictures decoded ahead while frames are fetched in order
        int behind = 4;           // decoded pictures kept before the last request (workers out of order, small steps back)
        // Requests that keep stepping back a little (a reversed clip) get a large window behind (up to
        // backwardBytes; huge pictures such as 4K 4:4:4 get fewer of them, never more than that).
        // Off for single pictures (thumbnails ask newest first: it looked like going backwards)
        bool autoBackward = true;
        int64_t backwardBytes = int64_t(384) * 1024 * 1024;
        // Seek window (preview): after a seek, the pictures decoded on the way from the key frame to the target are kept
        // too (the newest that fit into this many bytes), so steps back and jumps back a little are answered without
        // decoding again. Software decoding only (hardware pictures would have to be downloaded). 0 = off.
        int64_t keepBytes = 0;
    };

    // How picture() may answer
    struct Fetch {
        // Scrubbing: when the exact picture is not decoded yet, a close one will do - the newest decoded picture
        // before the position once the decoder is on its way there (right after a seek: the key frame before it)
        bool nearest = false;
        // Set by the caller when the picture is not wanted any more: picture() returns nullptr soon and the decoder
        // stops working towards it
        const std::atomic<bool>* cancel = nullptr;
    };

    static std::unique_ptr<Reader> open(const QString& path, const Options& options, QString* error = nullptr);
    static std::unique_ptr<Reader> open(const QString& path) { return open(path, Options{}); }
    // Opens `path` ahead of its use, on a helper thread: the app started with a project opens the files of its first
    // picture while the window is built (file, stream analysis, the graphics card's decoder: 100-300 ms). The next
    // open() of the same path with the same threads and hardware options and the same kind of user (autoBackward,
    // keepBytes zero or not: a preview's reader, not a thumbnail's) takes that reader with its own options (waiting
    // for it if it is not open yet) instead of opening the file again; one not taken within kPreparedMs (or at the
    // application's quit) is closed. Thread-safe.
    static void prepare(const QString& path, const Options& options);
    static void prepare(const QString& path);
    // Readers prepared and not taken yet (tests)
    static int preparedCount();
    static constexpr int kPreparedMs = 8000;
    ~Reader();

    const Info& info() const;
    // Before the first picture(): the rate positions count in when the file states none (MLT: the profile's)
    void setFrameRate(AVRational rate);
    // The container (stream metadata for MLT's meta.media.*). Read only, and only until the first picture()
    // (afterwards the decoder thread uses it).
    const AVFormatContext* container() const;

    // Decoded pictures kept before the last request (Options::behind), changeable while reading. More than 16 = playing
    // backwards: a request before the decoded pictures decodes a window of that many pictures up to it (from the key
    // frame before), so the following requests going back are answered from it instead of seeking for each one
    // (the reader also does so by itself after a few requests going back in small steps, e.g. a reversed clip in a
    // forward export, until requests go forwards again)
    void setBehind(int pictures);

    // The picture for stream position `position`; nullptr if the file has none (read error before the first picture)
    std::shared_ptr<const Picture> picture(int64_t position);
    // The same with a Fetch; exact (optional) = false when a close picture answered instead (Fetch::nearest)
    std::shared_ptr<const Picture> picture(int64_t position, const Fetch& fetch, bool* exact = nullptr);

    // Counters for tests and the log
    struct Stats {
        int seeks = 0;
        int reseeks = 0;          // seeks repeated further back (they landed past the target: files without an index)
        int backwardWindows = 0;  // times a window behind was kept for requests going backwards (no setBehind)
        int decoded = 0;          // pictures out of the decoder
        bool hardware = false;    // decoding on the graphics card right now
        int hardwareFailures = 0; // fell back to software
        int hibernations = 0;     // decoder closed after a long pause, reopened on demand
        int nearest = 0;          // requests answered by a close picture instead of the exact one (Fetch::nearest)
        int cancelled = 0;        // requests given up by their caller (Fetch::cancel)
    };
    Stats stats() const;
    // Long stream analyses run by this process (files the first analysis could not describe; once per file and session)
    static int longAnalyses();
    // The decoder is lost for good: it could not be opened again (software after a hardware failure, after
    // hibernating). Every picture() answers nullptr from then on, at once.
    bool failed() const;
    // Tests only: the next decoder thread to look acts as if its hardware decoder failed, and from then on no software
    // decoder opens (false: back to normal)
    static void loseDecoderForTests(bool on);

private:
    Reader();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Video
