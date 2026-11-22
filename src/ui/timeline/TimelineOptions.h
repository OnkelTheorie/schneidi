// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

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

} // namespace TimelineOptions
