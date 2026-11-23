// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <functional>

class Engine;

// What playback does after the user moves the playhead: the choice in Settings › General › Timeline (QSettings
// "timeline/afterPlayheadMove"). Read at the start of every move, so a change takes effect at once, no restart.
//  - Standard (default, the behaviour before the setting existed): a click or drag in the timeline ruler, the overview
//    strip or the Studio lanes plays on if it was playing; the viewer's scrub bar and jump keys stop playback.
//  - Continue: every move keeps the state from before (playing plays on from the new position, paused stays paused).
//  - Stop: every move stops playback.
//  - Play: playback starts from the new position after the move (a drag: on release; during it the usual scrubbing).
// Moves it covers: the mouse (and touch, which arrives as mouse events) in the Edit timeline's ruler, the overview
// strip, the Studio lanes and the viewer's scrub bar; the jump keys and buttons (Home/End and the viewer's ⏮, previous/
// next edit point, previous/next marker, go to In/Out). Not covered: frame and second steps (arrow keys, always
// paused: they look at single frames), J/K/L (they set the speed, not the position), keyframe arrows and other panels
// that show a moment (Inspector, transcript, checks) and seeks by the AI (live): those keep their own behaviour.
namespace PlayheadMove {

enum class Mode { Standard, Continue, Stop, Play };
// Where a move comes from (Standard treats them differently)
enum class Source { Timeline, ViewerBar, Key };

Mode mode(); // the stored choice, Standard when none is stored
void setMode(Mode mode);

// Whether playback runs after a move from `source`, given whether it ran before
constexpr bool playsAfter(Mode mode, Source source, bool wasPlaying)
{
    switch (mode) {
    case Mode::Standard: return wasPlaying && source == Source::Timeline;
    case Mode::Continue: return wasPlaying;
    case Mode::Stop: return false;
    case Mode::Play: return true;
    }
    return wasPlaying;
}

// One move of the playhead: begin() before its first seek, end() after its last (on release). begin() stops
// playback at once when the move ends stopped; end() starts it when the move ends playing and it was paused (not at
// the end of what is shown: playing from there would jump to the front). A drag keeps the usual scrubbing between
// the two: paused it scrubs, playing it plays on from each new position.
class Gesture {
public:
    void begin(Engine* engine, Source source);
    void begin(Engine* engine, Source source, Mode mode);
    void end(Engine* engine);
    bool active() const { return m_active; }

private:
    bool m_active = false;
    bool m_startAfter = false; // paused before and the rule says play: start on end()
};

// A move in one step (a key, a button): begin, `seek`, end
void jump(Engine* engine, Source source, const std::function<void()>& seek);

} // namespace PlayheadMove
