#pragma once
// Check before export: what typically goes wrong in a finished video, so a person (Deliver page) or an AI (`check`)
// sees it before handing the video over. Pure functions on the timeline and on measured numbers; decoding and
// measuring is done by the caller (cli/AnalysisCheck.cpp). Messages are UI texts (T()), the check id stays stable.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QString>
#include <QVector>

#include <vector>

namespace ExportCheck {

enum class Severity { Error, Warning, Info };
const char* severityId(Severity s); // "error", "warning", "info"

struct Issue {
    Severity severity = Severity::Warning;
    QString check;   // stable id: "offline", "black", "subtitle_end", "subtitle_short", "subtitle_fast", "no_sound",
                     // "clipping", "true_peak", "loudness", "silence_end", "silence_start", "silence", "black_file",
                     // "frozen", "no_video", "no_audio"
    QString message; // for people, in the UI language
    int from = -1;   // frames (exclusive end), -1 = whole video
    int to = -1;
};

struct Options {
    double targetLufs = -14;    // web/streaming; broadcast -23
    double toleranceLu = 2;     // integrated loudness further away than this from the target = warning
    double maxTruePeakDb = -1;  // above = warning (codecs clip it), above 0 = error
    double silenceDb = -50;     // per-frame peak below this counts as silence
    double silenceEndS = 2;     // silence at the end longer than this = warning (at the start: info)
    double silenceMiddleS = 5;  // silent stretch inside the video longer than this = info
    double subtitleMinS = 0.7;  // shorter cue = hard to read
    double subtitleMaxCps = 21; // characters per second above this = too fast to read
};

// Picture, media and subtitles of the range [from, to) without decoding: offline media, frames without any picture
// (black), subtitles that are too short, too fast to read or run past the end
QVector<Issue> timeline(const Timeline& tl, int from, int to, const ProjectFormat& f, const Options& o = {});

// Sound of a video: per-frame peaks (linear, 0..1+) starting at frame `offset`, integrated loudness and true peak
// (LoudnessMeter::kSilence or lower = no sound). limiter = the master limiter is on (no advice to switch it on).
QVector<Issue> sound(const std::vector<float>& framePeaks, int offset, double integratedLufs, double truePeakDb,
                     const ProjectFormat& f, const Options& o = {}, bool limiter = false);

// Stretches longer than minFrames where the per-frame peak stays below thresholdDb, as [from, to) with `offset`
QVector<QPair<int, int>> silentStretches(const std::vector<float>& framePeaks, int offset, double thresholdDb,
                                         int minFrames);

int errors(const QVector<Issue>& issues);
int warnings(const QVector<Issue>& issues);

} // namespace ExportCheck
