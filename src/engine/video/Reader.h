#pragma once
// Own video decoder (plan 0.11 point 7): reads the pictures of one video stream with the FFmpeg libraries, in a
// thread of its own that decodes a few frames ahead of the last request. MLT's avformat producer decoded inside
// get_image, under the producer's lock: one picture at a time, and nothing while the graph composited the last one.
// Optional hardware decoding (engine/video/Hardware); the pictures are software frames either way.
//
// Frame choice as in MLT's producer_avformat (7.30) so the same position shows the same picture: positions count in
// the stream's frame rate (MLT's guess, see Info::frameRate) from the first key frame; a request for position p
// returns the first decoded picture at p or later. Jumps back, or more than 64 frames forward, seek to the key frame
// before (2 s earlier still for codecs with B-frames) and decode from there. At the end of the stream the last picture.
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

#include <cstdint>
#include <memory>

struct AVFormatContext;
struct AVFrame;

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
};

struct Picture {
    AVFrame* frame = nullptr;     // software, upright; format = Info::format
    int64_t position = 0;         // stream frame position
    bool progressive = true;
    bool topFieldFirst = false;
    ~Picture();
};

class Reader {
public:
    struct Options {
        int threads = 0;          // decoder threads (0 = FFmpeg's choice), as MLT's "threads"
        bool hardware = true;     // try the hardware decoder (engine/video/Hardware)
        int ahead = 6;            // pictures decoded ahead while frames are fetched in order
        int behind = 4;           // decoded pictures kept before the last request (workers out of order, small steps back)
    };

    static std::unique_ptr<Reader> open(const QString& path, const Options& options, QString* error = nullptr);
    static std::unique_ptr<Reader> open(const QString& path) { return open(path, Options{}); }
    ~Reader();

    const Info& info() const;
    // Before the first picture(): the rate positions count in when the file states none (MLT: the profile's)
    void setFrameRate(AVRational rate);
    // The container (stream metadata for MLT's meta.media.*). Read only, and only until the first picture()
    // (afterwards the decoder thread uses it).
    const AVFormatContext* container() const;

    // The picture for stream position `position`; nullptr if the file has none (read error before the first picture)
    std::shared_ptr<const Picture> picture(int64_t position);

    // Counters for tests and the log
    struct Stats {
        int seeks = 0;
        int decoded = 0;          // pictures out of the decoder
        bool hardware = false;    // decoding on the graphics card right now
        int hardwareFailures = 0; // fell back to software
        int hibernations = 0;     // decoder closed after a long pause, reopened on demand
    };
    Stats stats() const;

private:
    Reader();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Video
