// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Reading a whole video for an analysis (scenes, overview, region and colour events): every picture, or samples at a
// fixed rate, upright and scaled down (optionally a part of the picture), in-process with the FFmpeg libraries.
// Replaces one ffmpeg process per analysis that decoded everything at full size on its own threads and piped the
// pictures out. The file is split into time ranges decoded at once on several cores; with samples, pictures no other
// picture depends on can stay undecoded (non-reference frames: about two thirds of a recording with B-frames).
//
// Timing as the ffmpeg command line did it (setpts=PTS-STARTPTS, fps=<rate>): times count from the first decoded
// picture; sample k is the last picture whose time rounds to k / rate or earlier (a picture repeats when the file has
// fewer pictures than samples). Scaling with libswscale bicubic, the colour matrix and range from the picture's tags,
// pictures turned upright like ffmpeg's autorotate. No MLT, no GUI dependency.

#include <QRectF>
#include <QString>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <cstdint>
#include <functional>

namespace Video::Scan {

struct Options {
    // Output size; height < 0 = keep the aspect ratio with a height divisible by -height (ffmpeg's scale=W:-2),
    // width <= 0 = the (cropped) picture's own size
    int width = 320;
    int height = -2;
    QRectF crop;                  // part of the upright picture, as fractions of its size (null = all of it)
    AVPixelFormat format = AV_PIX_FMT_YUV420P; // YUV420P, GRAY8 or RGB24
    double rate = 0;              // samples per second; 0 = every decoded picture
    bool skipNonReference = false; // only with rate: leave pictures no other picture depends on undecoded
    int threads = 0;              // cores to use (0 = Background::threads(all))
};

struct Frame {
    int64_t sample = 0;           // sample number (with rate), else -1
    double seconds = 0;           // time from the first picture
    const uint8_t* data[4] = {};
    int linesize[4] = {};
    int width = 0, height = 0;
    bool fullRange = true;        // YUV/gray: full levels 0..255 (else video range 16..235)
};

// Called for every picture/sample, from several threads at once (each in time order within its range). `previous` is
// the picture/sample right before in time (decoded before the range for the first one of a range), nullptr for the
// very first. The pixels are only valid during the call.
using Each = std::function<void(const Frame& frame, const Frame* previous)>;
// progress(0..1); false = cancel
using Progress = std::function<bool(double)>;

struct Result {
    bool ok = false;
    bool canceled = false;
    QString error;
    int ranges = 0;               // time ranges decoded at once
    int64_t decoded = 0;          // pictures out of the decoders (warm-up before the ranges included)
};

Result run(const QString& path, const Options& options, const Each& each, const Progress& progress = {});

} // namespace Video::Scan
