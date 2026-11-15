// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Export quality "Like source" (RenderSettings::kQualitySource): the video bit rate the export aims at, so that the file
// is about as big as the recordings it was cut from. The average of the video bit rates of the media files the range
// shows on the video tracks, weighted by how long each one is visible; each file's rate scaled by the output's pixel
// rate (width x height x fps) over its own, so a 4K source in a 1080p export, or a 60 fps one exported at 30, asks for
// fewer bits; scaled up only by the square root of that factor (no new detail: 1080p -> 4K = 2x, not 4x). Titles, stills, freeze frames, generated media and files whose bit rate is unknown do not count. Pure
// computation: the media's numbers come from a lookup (engine: MediaProbe::videoRate).

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QSize>
#include <QString>

#include <functional>
#include <optional>

namespace SourceBitrate {

// A media file's video stream
struct Video {
    double kbit = 0; // video bit rate, kbit/s (> 0)
    QSize size;      // picture size (empty = unknown: not scaled by it)
    double fps = 0;  // frame rate (0 = unknown: not scaled by it)
};
// nullopt = not a video with a known bit rate (still, sound only, unreadable)
using Lookup = std::function<std::optional<Video>(const QString& path)>;

struct Result {
    int targetK = 0;      // kbit/s for the export's video, 0 = nothing known (export falls back to "High")
    int frames = 0;       // timeline frames with a known source (the weights)
    int files = 0;        // different files that counted
};

// Range [from, to] in timeline frames (to inclusive, < 0 = until the end); output = the export's picture and rate
Result target(const Timeline& tl, int from, int to, QSize output, FrameRate outputRate, const Lookup& lookup);

// "≈ 30 Mbit/s", "≈ 850 kbit/s" (translated unit)
QString label(int kbit);

} // namespace SourceBitrate
