#pragma once
// Shared parts of the commands that look at the material (Analysis.cpp, AnalysisAudio.cpp, AnalysisVideo.cpp)

#include "cli/CommandsDetail.h"
#include "core/Stutter.h"
#include "engine/Profiles.h"

#include <QColor>
#include <QRect>
#include <QSize>

#include <functional>
#include <vector>

namespace Cli::detail {

// Media file + the frame rate frames count in (project format, or the file's own rate)
struct MediaSource {
    ProjectFormat format;
    MediaInfo info;
    ClipFormat clip; // the file's own format (detected when no project gives the frame rate)
};

// `file` (+ optional `project` for its frame rate); throws if it cannot be read
MediaSource openMedia(const QJsonObject& a);

// The format to look at the file's picture in: its own size, the frame rate frames count in. A project only gives
// the rate - a 16:9 recording looked at with a 9:16 project is not letterboxed into the project's frame, so crops
// and regions are parts of the recording's picture
ProjectFormat pictureFormat(const MediaSource& m);

// "x,y,w,h" in pixels of the full picture, or fractions of it when all four are <= 1 ("0.75,0,0.25,0.3")
QRect cropRect(const QString& spec, QSize full);

// One file (`file`) or several (`files`, one result each, looked at in parallel up to `parallel` at once)
QJsonObject perFile(const QJsonObject& a, Context& ctx, QJsonObject (*one)(const QJsonObject&, Context&),
                    int parallel = 6);

// ---------- Stutter (Analysis.cpp; core/Stutter.h, engine/FrameGaps.h) ----------

// Frames [from, to) in `f` where a jump (or a stretch of them) between source seconds `at` and `end` holds the picture:
// from the frame after the last good one up to the next one (at least one frame)
QPair<int, int> frozenFrames(double at, double end, const ProjectFormat& f);
// A stretch of stutter as a range with its jumps, missing frames and the longest jump
QJsonObject stretchJson(const Stutter::Stretch& st, const ProjectFormat& f);

// ---------- Sound (AnalysisAudio.cpp) ----------

struct Peak {
    int frame;
    double db;
};
struct LoudPeaks {
    QVector<Peak> peaks; // in time order
    double medianDb = 0, thresholdDb = 0;
    std::vector<double> db; // the loudest sample of each frame, dBFS
};
// Loud moments: the loudest frame of each burst above the threshold (`threshold_db`, default 12 dB above the
// median), at least `min` apart (default 0.5 s); `audio_stream`. Throws without sound
LoudPeaks loudPeaks(const MediaSource& m, const QJsonObject& a, const std::function<void(double)>& progress);

// Peaks less than `gap` frames apart as one burst (a fight, a rally); `to` = the last peak + 1 frame
struct PeakGroup {
    int from, to, hits;
    double peakDb;
};
QVector<PeakGroup> groupPeaks(const QVector<Peak>& peaks, int gap);

// ---------- Picture (AnalysisVideo.cpp) ----------

// Moments where more (appear) or less of one colour shows in `region` and stays (`color`, `tolerance`, `only`,
// `min_area`, `min` as in `scenes`)
struct ColorEvent {
    int frame;
    bool appear;
    double area; // part of the whole picture
    int onset;   // the first frame where the change began to show (a kill feed line slides in over a few frames)
};
struct ColorEvents {
    QVector<ColorEvent> events; // in time order
    QColor color;
    int tolerance = 0;
    double minArea = 0;
    double most = 0; // the most of the colour at one time, part of the whole picture
};
ColorEvents colorEvents(const MediaSource& m, const QJsonObject& a, const std::function<void(double)>& progress);

} // namespace Cli::detail
