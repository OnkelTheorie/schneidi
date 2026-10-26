#pragma once
// Check before export: what typically goes wrong in a finished video, so a person (Deliver page) or an AI (`check`)
// sees it before handing the video over. Pure functions on the timeline and on measured numbers; decoding and
// measuring is done by the callers (cli/AnalysisCheck.cpp, ui/DeliverPanel.cpp; the shared measurements in
// engine/CheckMeasure.h). Messages are UI texts (T()), the check id stays stable.

#include "core/ProjectFormat.h"
#include "core/Stutter.h"
#include "core/Types.h"

#include <QString>
#include <QVector>

#include <functional>
#include <optional>
#include <vector>

namespace ExportCheck {

enum class Severity { Error, Warning, Info };
const char* severityId(Severity s); // "error", "warning", "info"

struct Issue {
    Severity severity = Severity::Warning;
    QString check;   // stable id: "offline", "black", "subtitle_end", "subtitle_short", "subtitle_fast", "no_sound",
                     // "clipping", "true_peak", "loudness", "silence_end", "silence_start", "silence", "black_file",
                     // "frozen", "no_video", "no_audio", "under_music", "safe_area", "subtitle_safe_area",
                     // "slow_hook", "stutter"
    QString message; // for people, in the UI language
    int from = -1;   // frames (exclusive end), -1 = whole video
    int to = -1;
    int clip = 0;    // the clip it is about (title outside the safe area, recording with stutter), 0 = none
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
    // Mix: a track with events (game sound, speech) further below the music than this, while its events play = buried
    double underMusicLu = 15;
    // Vertical video: what TikTok, Reels and Shorts cover, as parts of the picture (bottom: caption and buttons, a
    // column of buttons on the right from rightFrom of the height down, the bar at the top)
    double safeBottom = 0.20, safeTop = 0.08, safeRight = 0.12, safeRightFrom = 0.35;
    double safeMaxCovered = 0.2; // a text block with at least this part covered (or outside the picture) = warning
    // Hook: the first hookS seconds of a vertical video up to hookMaxS long; less change than hookMinMotion = slow
    // start. Motion = mean difference of frames 0.1 s apart (gray, 96 px, black bars left out) in % of full scale:
    // a still 0, a calm or frozen game moment 1.1-1.3, a static scope view with a slow zoom 1.5, gameplay 2-10
    double hookS = 1.0, hookMinMotion = 1.8, hookMaxS = 180;
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

// ---- Layout and hook of vertical videos (9:16 and narrower: what phones show full screen)

bool isVertical(const ProjectFormat& f);
// Titles and subtitles in [from, to) that TikTok, Reels and Shorts would cover (Options::safe*) or that stick out of
// the picture, one issue per title clip / subtitle track. Nothing for other formats.
QVector<Issue> layout(const Timeline& tl, int from, int to, const ProjectFormat& f, const Options& o = {});
// The hook check applies: vertical, the range starts at the beginning, the video is short (Options::hookMaxS)
bool wantsHook(int from, int to, const ProjectFormat& f, const Options& o = {});
// Frames to compare for the hook: 0.1 s apart over the first hookS seconds of [from, to)
QVector<int> hookFrames(int from, int to, const ProjectFormat& f, const Options& o = {});
// Verdict on the measured change between those frames (mean difference, % of full scale)
QVector<Issue> hook(double motion, int from, int to, const ProjectFormat& f, const Options& o = {});

// ---- Mix: music against the sound with events (game sound, speech)

// What an audio track carries. By its name first (music, Musik, song, BGM … / voice, mic, game, Spiel …), else by
// its clips: the sound of a recording (a file with a picture, multicam/compound clips) has events; a track of sound
// files only is Undecided - music if it sounds without pauses (continuous), speech or a voice-over otherwise.
// Muted tracks (and the others while one is solo) are Other.
enum class SoundRole { Other, Music, Events, Undecided };
SoundRole soundRole(const Timeline& tl, int track, const std::function<bool(const QString& path)>& hasPicture);
// An undecided track sounds without pauses where its clips play: at most 10 % of those frames 30 dB under its
// loud frames. framePeaks start at frame `offset`.
bool continuous(const Timeline& tl, int track, const std::vector<float>& framePeaks, int offset);
// One audio track measured alone (the others muted), from frame `from` on
struct TrackSound {
    int track = -1;             // index in Timeline::audio
    bool music = false;
    std::vector<double> blocks; // 400 ms block powers every 100 ms (LoudnessMeter::blocks)
};
// Where a track with events and the music play together: the music's loudness in the moments the track has its
// events (its blocks within 10 LU of its 95th percentile: shots, words) against the track's there. More than
// underMusicLu = warning per stretch of playing together (pauses up to 2 s bridged). Tracks without events there
// (flat noise: the 95th percentile less than 6 LU above the median; silence: below -70 LUFS) are left out.
QVector<Issue> mix(const Timeline& tl, const QVector<TrackSound>& sounds, int from, const ProjectFormat& f,
                   const Options& o = {});

// ---- Stutter in recordings (core/Stutter.h)

// Video clips in [from, to) whose source range contains jumps of their file: one warning per stretch of stutter
// (timeline frames where it shows). scanOf = the file's scan (nullopt: none), fileLength = its length in project
// frames (for reversed clips, <= 0 = unknown)
QVector<Issue> stutter(const Timeline& tl, int from, int to, const ProjectFormat& f,
                       const std::function<std::optional<Stutter::Scan>(const QString& path)>& scanOf,
                       const std::function<int(const QString& path)>& fileLength);

int errors(const QVector<Issue>& issues);
int warnings(const QVector<Issue>& issues);

} // namespace ExportCheck
