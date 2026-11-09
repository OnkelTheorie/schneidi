// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Shared parts of the commands that look at the material (Analysis.cpp, AnalysisAudio.cpp, AnalysisVideo.cpp)

#include "cli/CommandsDetail.h"
#include "core/Stutter.h"
#include "engine/ClipFormat.h"

#include <QColor>
#include <QRect>
#include <QRectF>
#include <QSize>

#include <atomic>
#include <functional>
#include <vector>

namespace Cli::detail {

// Stopping a long analysis from outside (the app's review render, Cli::reviewOverlay): a CancelScope sets the flag for
// the calling thread; the decoding loops (pictureSamples, loudPeaks, the review's sound mixes) read it once at their
// start and stop when it turns true, then fail with CANCELED. nullptr = cannot be canceled (the CLI).
const std::atomic<bool>* cancelFlag();
bool canceled(); // the calling thread's flag is set and true
struct CancelScope {
    explicit CancelScope(const std::atomic<bool>* flag);
    ~CancelScope();
    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;

private:
    const std::atomic<bool>* m_old;
};

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

// ---------- Reading the picture (AnalysisPicture.cpp) ----------

// SCHNEIDI_ANALYSIS=ffmpeg: read the picture with an ffmpeg process as before 0.15 (comparison, fallback);
// SCHNEIDI_ANALYSIS=full: in-process, but samples decode every picture (exactly the ffmpeg way's results)
bool ffmpegAnalysis();

// Every decoded picture of a video file scaled to `width` (aspect kept): time from the first picture, scene score
// (difference to the picture before, 0..1, as ffmpeg's select filter: 0 for the first) and mean brightness (0..255)
struct PictureScore {
    double seconds;
    double score;
    double luma;
};
// In time order; throws when the video cannot be read
std::vector<PictureScore> pictureScores(const MediaSource& m, int width, const std::function<void(double)>& progress);

// Samples of `crop` (fractions of the upright picture) scaled to `size` as packed RGB24, `rate` per second from the
// first picture (as ffmpeg's fps filter: the last picture at or before each sample). each(sample, pixels) is called
// from several threads at once and must not throw. `ffmpegFilter`: the same as an ffmpeg filter chain after the
// fps filter (crop and scale) for SCHNEIDI_ANALYSIS=ffmpeg. `everyPicture`: decode B-frames too (exact sample spacing;
// otherwise a sample may get the reference picture a frame or two earlier). Throws when the video cannot be read.
void pictureSamples(const MediaSource& m, const QRectF& crop, QSize size, int rate, const QString& ffmpegFilter,
                    const std::function<void(int, const uchar*)>& each, const std::function<void(double)>& progress,
                    bool everyPicture = false);

// ---------- Picture (AnalysisVideo.cpp) ----------

// Moments where more (appear) or less of one colour shows in `region` and stays (`color`, `tolerance`, `only`,
// `min_area`, `min` as in `scenes`)
struct ColorEvent {
    int frame;
    bool appear;
    double area; // part of the whole picture
    int onset;   // the first frame where the change began to show (a kill feed line slides in over a few frames)
    bool teamkill = false; // a kill feed line whose victim is in the killer's team colour (framedLineEvents)
};
struct ColorEvents {
    QVector<ColorEvent> events; // in time order
    QColor color;
    int tolerance = 0;
    double minArea = 0;
    double most = 0; // the most of the colour at one time, part of the whole picture
};
ColorEvents colorEvents(const MediaSource& m, const QJsonObject& a, const std::function<void(double)>& progress);

// ---------- Kill feed (AnalysisKillFeed.cpp) ----------

// New kill feed lines with a thin outline of `color` (CS2): top and bottom edge `lineHeight` apart (part of the
// picture height), the right edge at `rightEdge` (part of the width), inside `region` (fractions). Only appear events.
// Lines may reach left to `lineLeft` (part of the width): there their names' team colours are read, a line whose victim
// has the killer's team colour is a teamkill (ColorEvent::teamkill).
ColorEvents framedLineEvents(const MediaSource& m, const QRectF& region, const QColor& color, double lineHeight,
                             double rightEdge, double lineLeft, const std::function<void(double)>& progress);

// ---------- Highlights (AnalysisHighlights.cpp) ----------

// `highlights` of one file (`file`, the command's settings): from the cache (CacheLocation/highlights, key = file,
// size, modification time, frame rate and settings; the answer then has cached:true) or analysed and stored there
QJsonObject highlightsFile(const QJsonObject& a, Context& ctx);

} // namespace Cli::detail
