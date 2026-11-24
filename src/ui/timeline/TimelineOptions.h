// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QLine>
#include <QRect>
#include <QString>

// The timeline's behaviour where editors differ: the user's choices in Settings › Timeline (QSettings "timeline/…",
// app/TimelineSettingsPage). Each default is what schneidi did before the choice existed. The timelines read them when
// they need them (a play start, a paint, a click), so a change takes effect at once, no restart. The wheel zoom anchor
// and playback after a playhead move have their own helpers (ZoomAnchor, PlayheadMove).
namespace TimelineOptions {

// Follow the playhead while playing (Settings › Timeline › Playback). Off (default, core rule: the view moves only by
// the user's input): the playhead runs out of view. Page: when it leaves the view, the view turns a page so it is at
// the left edge again (reverse: the right edge). Center: the view runs under a playhead kept in the middle (from the
// middle on: the start stays at the left edge). Starting playback is the user's input; the view follows only while it
// plays and only the playback itself (not a model change, not a drag in the ruler). As soon as the user scrolls or
// zooms sideways while it plays, the view stops following until the next start.
enum class Follow { Off, Page, Center };
Follow follow();
void setFollow(Follow follow);

// A click (no drag) into the tracks - an empty spot or a clip - also sets the playhead there, like Vegas, Final Cut,
// Avid, Shotcut, CapCut (Settings › Timeline › Mouse). Off (default): it only selects (a clip, a gap); the playhead
// moves through the ruler, like Premiere, DaVinci, Kdenlive. A drag stays a rubber band resp. a move either way.
bool clickSetsPlayhead();
void setClickSetsPlayhead(bool on);

// Dragging with the middle mouse button in the tracks pans the view sideways and up/down (Settings › Timeline ›
// Mouse), like Kdenlive, Blender, Vegas, Fairlight and schneidi's own viewer and compositing canvas. On by default
// (it changed nothing before: the middle button did nothing in the timeline). Read at every press.
bool middleDragPans();
void setMiddleDragPans(bool on);

// A clip's edge dragged with the selection tool onto its neighbour (Settings › Timeline › Mouse): overwrites it
// (default, as before: the neighbour is shortened or removed where the clip now lies; quick) or stops there like
// Premiere, DaVinci, Kdenlive, Shotcut. The trim tool (T) ripples either way. Read at every press on an edge.
bool trimOverwrites();
void setTrimOverwrites(bool on);

// What the play/pause key (Space) does while it plays (Settings › Timeline › Playback): stop where it is (default,
// like Premiere, Final Cut, Kdenlive, Shotcut, CapCut) or stop and go back to where playback started (like Vegas and
// audio programs: hear a cut again and again). K always stops where it is; the action "Stop and return" does the
// latter whatever is set. Read at every key press.
bool spaceReturns();
void setSpaceReturns(bool on);

// The speeds J/L step through when pressed again (Settings › Timeline › Playback): doubling 1-2-4-8-16 (default, as
// before; DaVinci) or gentle 1-2-3-5-8 (Avid; Premiere 1-2-3-4). shuttleSpeed: the speed after a press of J
// (direction -1) or L (+1) at the current speed - the other direction (or paused) starts at 1x, a slower one (half
// speed of K+J/L) goes to 1x, the top speed stays. Read at every press.
enum class ShuttleSteps { Double, Gentle };
ShuttleSteps shuttleSteps();
void setShuttleSteps(ShuttleSteps steps);
double shuttleSpeed(double current, int direction, ShuttleSteps steps);

// Snapping on or off (the magnet button, N): remembered across starts (default on, as every editor). Stored by the
// main window when the user switches it (not in test runs).
bool snapping();
void setSnapping(bool on);
// What a drag snaps to (Settings › Timeline › Snapping; read on every snap). The defaults are what schneidi did
// before: clip (and subtitle) edges and the playhead. Markers and the In/Out points can be added for clips, edges and
// the blade; the In/Out range and node bars snap to markers whatever is set. The timeline's start always counts.
enum SnapTarget { SnapClipEdges = 1, SnapPlayhead = 2, SnapMarkers = 4, SnapInOut = 8 };
int snapTargets(); // SnapTarget flags
void setSnapTargets(int targets);
constexpr int kDefaultSnapTargets = SnapClipEdges | SnapPlayhead;
// How near (screen pixels) an edge has to come to snap: small 4, medium 8 (default, as before), large 14
enum class SnapReach { Small, Medium, Large };
SnapReach snapReach();
void setSnapReach(SnapReach reach);
int snapPixels(SnapReach reach);
inline int snapPixels() { return snapPixels(snapReach()); }

// The first video clip in an empty timeline whose format (size, frame rate) differs from the project's (Settings ›
// Timeline › New clips): ask (default, as before; DaVinci, Kdenlive), always adapt the project without asking
// (Premiere, Final Cut, CapCut) or never change it. Fixed values (start template, Project Settings) stay either way.
// Read at every such clip.
enum class ClipFormat { Ask, Adapt, Keep };
ClipFormat clipFormat();
void setClipFormat(ClipFormat choice);

// Default lengths (Settings › Timeline › New clips): stills imported from now on (5 s) and transitions added without
// a length (1 s). applyEditDefaults() hands the stored choice to the core (EditDefaults): at the start and after OK.
double stillSeconds();
void setStillSeconds(double seconds);
double transitionSeconds();
void setTransitionSeconds(double seconds);
void applyEditDefaults();

// Step sizes (Settings › Timeline › Keys): the big step of the playhead (Shift+arrow: 1 second, as before; Premiere
// 5 frames, Final Cut and Avid 10) and the big nudge of clips/edit points (Shift+, and Shift+.: 5 frames, as before).
// A number of frames or of seconds; read at every key press.
struct Step {
    int count = 1;
    bool seconds = true;
    bool operator==(const Step&) const = default;
};
constexpr Step kDefaultBigStep{1, true};
constexpr Step kDefaultBigNudge{5, false};
Step bigStep();
void setBigStep(Step step);
Step bigNudge();
void setBigNudge(Step step);
inline int stepFrames(Step step, int fps) { return step.seconds ? step.count * fps : step.count; }
// The menu text of a step action as one whole sentence per unit and direction (translatable with its grammar); the
// defaults give exactly the texts from before ("1 Sekunde zurück", "5 Frames nach links schieben")
enum class StepAction { Back, Forward, NudgeLeft, NudgeRight };
QString stepActionText(StepAction action, Step step);

// Pictures on video clips (Settings › Timeline › Display): a filmstrip (default, as before), the first frame, the
// first and the last frame, none (the clip's colour only: least decoding, for weak computers). Read once per paint.
enum class Thumbnails { Filmstrip, Start, StartEnd, None };
Thumbnails thumbnails();
void setThumbnails(Thumbnails thumbnails);

// Waveforms on audio clips (Settings › Timeline › Display; Edit timeline and Studio lanes, read once per paint):
// shape mirrored around the middle (default, as before; DaVinci, Kdenlive, Audacity) or from the bottom (rectified,
// Premiere and Final Cut: twice as tall on low tracks); height in dB, -48..0 dB (default, as before: speech does not
// look flat) or linear (the plain amplitude, Avid's choice).
enum class WaveShape { Mirrored, Bottom };
enum class WaveScale { Db, Linear };
WaveShape waveShape();
void setWaveShape(WaveShape shape);
WaveScale waveScale();
void setWaveScale(WaveScale scale);
// Height 0..1 of a peak (0..255 as in Waveform's levels) with the clip's gain (dB) on top
double waveHeight(int peak, double gainDb, WaveScale scale);
// The line of one column of the waveform at height v (0..1) inside `body`, `inset` pixels from its edges
QLine waveLine(int x, double v, const QRect& body, WaveShape shape, double inset = 1);

// How the app shows times (Settings › Timeline › Display): timecode "00:00:04:12" (default), seconds "0:04.50"
// (minutes:seconds with hundredths, the UI language's decimal sign; hours in front from one hour on) or frames "112".
// Shown in the timeline (ruler, corner, durations, trim and drag numbers, In/Out tips), the Studio lanes, the
// viewer, the trim view, the Media Pool and the subtitle fields; files (EDL, FCPXML, still names) and the CLI keep
// timecode. Read once per paint resp. per update.
enum class TimeFormat { Timecode, Seconds, Frames };
TimeFormat timeFormat();
void setTimeFormat(TimeFormat format);
// fps = the real frame rate (29.97, 23.976: seconds are real seconds; timecode counts whole frames per second as
// Timecode::format does). Seconds round to the nearest hundredth, so up to 100 fps every frame reads back as itself
QString formatTime(int frames, double fps, TimeFormat format);
inline QString formatTime(int frames, double fps) { return formatTime(frames, fps, timeFormat()); }
// A typed time -> frames, -1 = invalid. Every format reads timecode (Timecode::parse); "4.5s" is always seconds and
// "112f" always frames. Besides, with Seconds a number with a decimal sign or up to two colons is seconds
// ("4,5", "1:30" = 90 s), with Frames plain digits are frames.
int parseTime(const QString& text, double fps, TimeFormat format);
inline int parseTime(const QString& text, double fps) { return parseTime(text, fps, timeFormat()); }

} // namespace TimelineOptions
