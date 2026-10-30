// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/ClipFormat.h"
#include "engine/MediaProbe.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <cmath>
#include <memory>
#include <optional>

ClipFormat detectClipFormat(const QString& path)
{
    ClipFormat cf;
    // Read with the own decoder (stream description only, nothing decoded) as the preview reads the file
    if (const std::optional<MediaInfo> m = MediaProbe::read(path, FrameRate{25, 1}); m && m->isImage)
        return cf; // still images have no format of their own
    if (MediaProbe::isAudioFile(path)) return cf;
    Video::Reader::Options options;
    options.hardware = false;
    const std::unique_ptr<Video::Reader> reader = Video::Reader::open(path, options);
    if (!reader) return cf;
    const Video::Info& in = reader->info();
    int w = in.width, h = in.height;
    const double sar = av_q2d(in.sar);
    if (sar > 0 && std::abs(sar - 1.0) > 0.01) w = int(std::lround(w * sar)); // anamorphic pixels
    if (int(std::lround(in.rotation)) % 180 == 90) std::swap(w, h); // phone portrait: file landscape, rotation in the metadata
    const int num = in.frameRate.num, den = in.frameRate.den;
    if (w <= 0 || h <= 0 || num <= 0 || den <= 0) return cf;
    // The average frame rate (the stream's rate above is the highest one with variable frame rate)
    const AVRational avg = reader->container()->streams[in.index]->avg_frame_rate;

    cf.ok = true;
    cf.width = w & ~1; // even dimensions (yuv420p)
    cf.height = h & ~1;
    cf.rate = {num, den};
    cf.averageFps = avg.num > 0 && avg.den > 0 ? av_q2d(avg) : 0.0;
    // If the average differs noticeably the frame rate is variable (phones, game capture that skips frames).
    // The nominal rate is what the recording aimed for (DVR clips: nominal 60 or 120, average ~45) -> nearest
    // 24/25/30/50/60 of it; only a nominal rate that is a time base rather than a rate (mkv/webm 1000, 90000)
    // falls back to the average.
    cf.variable = cf.averageFps > 0 && std::abs(cf.averageFps - cf.rate.fps()) > 0.01 * cf.rate.fps();
    cf.suggested = !cf.variable ? nearestFrameRate(cf.rate.fps())
                   : cf.rate.fps() <= 240 ? nearestFrameRate(std::min(cf.rate.fps(), 60.0), true)
                                          : nearestFrameRate(cf.averageFps, true);
    return cf;
}
