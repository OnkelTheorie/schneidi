// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Check before export: what typically goes wrong in a finished video, so a person (Deliver page) or an AI (`check`)
// sees it before handing the video over. Pure functions on the timeline and on measured numbers; decoding and
// measuring is done by the callers (cli/AnalysisCheck.cpp, ui/DeliverPanel.cpp; the shared measurements in
// engine/CheckMeasure.h). Messages are UI texts (T()), the check id stays stable.

#include "core/ProjectFormat.h"
#include "core/Stutter.h"
#include "core/Types.h"

#include <QSize>
#include <cstdint>
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
                     // "slow_hook", "stutter", "missing_effect", "encoder_quality", "black_bars", "too_long",
                     // "pace_slow", "story_no_hook", "story_hook_late", "story_resolution_late",
                     // "story_open_question", "story_no_cta", "story_gap", "retention_drop", "comp_unused_input",
                     // "comp_output_mask", "comp_slow", "key_edges", "custom_effect_skipped", "custom_code_untrusted"
    QString message; // for people, in the UI language
    int from = -1;   // frames (exclusive end), -1 = whole video
    int to = -1;
    int clip = 0;    // the clip it is about (title outside the safe area, recording with stutter), 0 = none
    QVector<int> clips; // all clips when one finding stands for several (titles of a track in the same zone)
    int at = -1;        // a moment it points at (the cut before a retention drop), -1 = none
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
    // The platform the video is for (RenderPresets: ShortPlatform): longer than maxLengthS = warning "too_long"
    // (0 = no limit); `platform` = its name for the message
    double maxLengthS = 0;
    QString platform;
    // Pace: a stretch longer than this without a change (cut, zoom/position keyframe, accent, text, overlay) = slow
    double maxStillS = 3;
    // Story: the first strong moment (a loud hit, the first word, a text) later than this = late hook (vertical
    // videos and videos for a platform: hookLateShortS, others hookLateS)
    double hookLateShortS = 2, hookLateS = 5;
    // The range is the whole video (the Deliver page's In/Out export): the opening checks (story_hook_late,
    // story_no_hook) start at its `from`, not only at frame 0
    bool rangeIsVideo = false;
};

// Picture, media and subtitles of the range [from, to) without decoding: too long for the platform, offline media, effects this version does
// not have (skipped), frames without any picture (black), subtitles that are too short, too fast to read or run past the end
QVector<Issue> timeline(const Timeline& tl, int from, int to, const ProjectFormat& f, const Options& o = {});
// Only the length: [from, to) longer than Options::maxLengthS ("too_long"); part of timeline(), for rendered files alone
QVector<Issue> length(int from, int to, const ProjectFormat& f, const Options& o = {});

// The sound is measured at most kMaxSoundHours from `from` (a clip far out on the timeline - frame 2^30 is over a
// year - must not make the check mix and keep peaks of months of silence): the end of the measured range, and with a
// shorter one a warning "sound_check_limited" in `issues`
inline constexpr double kMaxSoundHours = 24;
int soundEnd(int from, int to, const ProjectFormat& f, QVector<Issue>* issues);

// Sound of a video: per-frame peaks (linear, 0..1+) starting at frame `offset`, integrated loudness and true peak
// (LoudnessMeter::kSilence or lower = no sound). limiter = the master limiter is on (no advice to switch it on).
QVector<Issue> sound(const std::vector<float>& framePeaks, int offset, double integratedLufs, double truePeakDb,
                     const ProjectFormat& f, const Options& o = {}, bool limiter = false);

// What the master needs so the sound issues of sound() go away (the Deliver page's Fix button, `check`'s "fix"): the
// limiter at the true-peak ceiling (peaks, or the fader goes up) and the master fader by the difference to the
// target loudness, as far as it goes
struct SoundFix {
    bool limiter = false;  // switch the master limiter on at `ceiling` (dBTP)
    double ceiling = -1;
    bool level = false;    // set the master fader to `volumeDb` (rounded to 0.1 dB), `change` = the difference
    double volumeDb = 0;
    double change = 0;
    bool any() const { return limiter || level; }
};
SoundFix soundFix(const QVector<Issue>& sound, double integratedLufs, const Timeline& tl, const Options& o = {});
void apply(const SoundFix& fix, Timeline& tl);
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

// ---- Pace (0.17): how often the picture changes

// A change the viewer sees: a cut or a clip edge on a visible video track (titles: text on/off; V2+: an overlay),
// a zoom/position/rotation/exposure keyframe where the value moves (accents are such keyframes), a subtitle that
// starts. Changes within 0.4 s of the first count once (an accent writes several keyframes around its hit).
enum class ChangeKind { Cut, Overlay, Text, Motion };
const char* changeKindId(ChangeKind k); // "cut", "overlay", "text", "motion"
struct Change {
    int frame = 0;
    ChangeKind kind = ChangeKind::Cut;
    int clip = 0; // the clip that starts or moves there (0 = a subtitle)
};
// Changes strictly inside (from, to), sorted
QVector<Change> changes(const Timeline& tl, int from, int to, const ProjectFormat& f);

struct Stretch {
    int from = 0, to = 0;
    int length() const { return to - from; }
};
struct SectionPace {
    StorySection section; // clipped to the range
    double changesPerS = 0;
    Stretch longest;
};
struct Pace {
    QVector<Change> changes;
    double changesPerS = 0;   // changes per second over the range
    Stretch longest;          // longest stretch without a change (the range itself when there is none)
    QVector<Stretch> still;   // stretches longer than Options::maxStillS, in time order
    double curveStepS = 1;    // changes per second in steps of this many seconds (at most ~60 steps)
    QVector<double> curve;
    QVector<SectionPace> sections; // per story section (Timeline::sections) in the range
};
Pace pace(const Timeline& tl, int from, int to, const ProjectFormat& f, const Options& o = {});
// pace_slow: vertical videos up to Options::hookMaxS = a warning per still stretch (viewers swipe away), other
// videos = one info at the longest that names how many there are
QVector<Issue> paceIssues(const Pace& p, int from, int to, const ProjectFormat& f, const Options& o = {});
// Where a still stretch could get a change: evenly inside it so no part stays longer than 90 % of maxStillS, only at frames
// a video clip with a picture covers (an accent needs one)
QVector<int> breakPoints(const Timeline& tl, const Stretch& s, const ProjectFormat& f, const Options& o = {});

// ---- Story (0.17): sections (Timeline::sections) and the strong moments of the video

// Loud hits in per-frame peaks (from frame `offset`): at least -24 dBFS and 12 dB above the median of the half
// second before (a sound that plays from the first frame on is no hit). At most one per 0.25 s.
QVector<int> loudMoments(const std::vector<float>& framePeaks, int offset, double fps);
// The first frame >= from where a text starts (title clip on a visible track, subtitle of an enabled track), -1 = none
int firstText(const Timeline& tl, int from, int to);
// The strong moments of [from, to), sorted: the loud hits of framePeaks (from frame `from`; null = sound not
// measured), the first spoken word (words = timeline frames), the first text. by = what the earliest one is: "loud",
// "word", "text" (empty = none)
QVector<int> strongMoments(const Timeline& tl, int from, int to, const ProjectFormat& f,
                           const std::vector<float>* framePeaks, const QVector<int>& words, QString* by = nullptr);
// Story checks of [from, to). strong = strong moments (strongMoments; text starts are added from the timeline
// again, harmless). strongKnown = the caller could look for them (the sound was measured or there are
// transcripts): without, a missing strong moment is not reported. words = timeline frames of spoken words (for the
// call to action without sections). With sections: story_no_hook, story_resolution_late, story_open_question,
// story_no_cta (info), story_gap (info); always (range from the start): story_hook_late; without sections and with
// a platform: story_no_cta (info) when no text or word comes near the end.
QVector<Issue> story(const Timeline& tl, int from, int to, const ProjectFormat& f, QVector<int> strong,
                     bool strongKnown, const QVector<int>& words, const Options& o = {});

// Jump cuts in [from, to) (core/JumpCut): two clips of one take that touch and skip a part of it (at least 0.3 s)
// with the same framing on a visible video track - jump_cut (info), one per cut; at most 20, then one summary
QVector<Issue> jumpCuts(const Timeline& tl, int from, int to, const ProjectFormat& f);

// ---- Retention curve (YouTube Studio / TikTok export): where viewers leave

struct RetentionPoint {
    double seconds = 0;
    double percent = 0; // of the viewers at the start (100 = all)
};
// CSV text: a time column (seconds, "M:SS", "H:MM:SS"; or a position in % of the video when its header says
// position/%), a retention column (percent; fractions 0..1 are scaled; "85,3 %" works). Header optional, separator
// , ; or tab. lengthS = video length (for positions in %). Empty + error when nothing could be read.
QVector<RetentionPoint> retentionFromCsv(const QString& text, double lengthS, QString* error = nullptr);
// retention_drop (info): stretches where the curve falls at least 5 points (and 3 x the usual fall) within 2 s,
// after the first second; `at` = the last change before it (pace changes of the timeline, from frame `from`)
QVector<Issue> retention(const QVector<RetentionPoint>& curve, const QVector<Change>& changes, int from,
                         const ProjectFormat& f);

// ---- Compositing graphs (0.19, effect "comp" on a clip; core/ExportCheckComp.cpp)

// Rough CPU time of a clip's graph per frame in ms on one core at `size`: a fixed cost per node kind at 1080p (measured
// on a 5600X core, blur scaled by its radius) times the pixels, only the nodes the output needs; unknown kinds 5 ms
double compCostMs(const EffectInstance& comp, QSize size);
// Above this a graph is slow: about the share of a frame one core has for it at 30 fps export
inline constexpr double kCompBudgetMs = 40;
// Graphs of the video clips in [from, to): comp_unused_input (info) per clip with nodes that do not reach the output
// (their work is lost or a link is missing), comp_output_mask (warning) per clip whose output node gives a mask (the
// engine ignores such a graph: the clip shows its plain picture), comp_slow (info) per clip whose graph is estimated above kCompBudgetMs at
// the format's size and that does not use the render cache yet
QVector<Issue> comp(const Timeline& tl, int from, int to, const ProjectFormat& f);
// key_edges (info): a key's matte (alpha 0..255, w x h) with many half-transparent pixels inside the subject (its
// bounding box of solid pixels): more than 8 % of the subject's area between 16 and 239 = a soft or noisy key
std::optional<Issue> keyEdges(const uint8_t* alpha, int w, int h, int clip, int from, int to);

int errors(const QVector<Issue>& issues);
int warnings(const QVector<Issue>& issues);

} // namespace ExportCheck
