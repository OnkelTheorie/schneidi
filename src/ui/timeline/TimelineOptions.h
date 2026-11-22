// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

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

// What the play/pause key (Space) does while it plays (Settings › Timeline › Playback): stop where it is (default,
// like Premiere, Final Cut, Kdenlive, Shotcut, CapCut) or stop and go back to where playback started (like Vegas and
// audio programs: hear a cut again and again). K always stops where it is; the action "Stop and return" does the
// latter whatever is set. Read at every key press.
bool spaceReturns();
void setSpaceReturns(bool on);

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

// How the app shows times (Settings › Timeline › Display): timecode "00:00:04:12" (default), seconds "0:04.50"
// (minutes:seconds with hundredths, the UI language's decimal sign; hours in front from one hour on) or frames "112".
// Shown in the timeline (ruler, corner, durations, trim and drag numbers, In/Out tips), the Studio lanes, the
// viewer, the trim view, the Media Pool and the subtitle fields; files (EDL, FCPXML, still names) and the CLI keep
// timecode. Read once per paint resp. per update.
enum class TimeFormat { Timecode, Seconds, Frames };
TimeFormat timeFormat();
void setTimeFormat(TimeFormat format);
QString formatTime(int frames, int fps, TimeFormat format);
inline QString formatTime(int frames, int fps) { return formatTime(frames, fps, timeFormat()); }
// A typed time -> frames, -1 = invalid. Every format reads timecode (Timecode::parse); "4.5s" is always seconds and
// "112f" always frames. Besides, with Seconds a number with a decimal sign or up to two colons is seconds
// ("4,5", "1:30" = 90 s), with Frames plain digits are frames.
int parseTime(const QString& text, int fps, TimeFormat format);
inline int parseTime(const QString& text, int fps) { return parseTime(text, fps, timeFormat()); }

} // namespace TimelineOptions
