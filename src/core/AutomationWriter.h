// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Write mode of the Studio page (0.23, "touch"): while playing, moving a track's fader or pan knob records into that
// track's automation (core/Automation.h Touch: thinned keys), on release the value glides back to the curve that was
// there. Every change of a write pass (all touches from the first one until endPass, usually playback stopping) goes
// into one undo step ("Automation schreiben"). The caller says what is heard and when (timeline frame); no Qt widgets
// here, so tests drive it directly.

#include "core/Automation.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPair>
#include <QString>

class Editor;

class AutomationWriter {
public:
    explicit AutomationWriter(Editor* editor) : m_editor(editor) {}

    // The fader / knob of audio track `track` moved to `heard` (dB heard / pan heard) at timeline frame `frame` while
    // playing: the first move starts a touch (and the pass), every move records and updates the curve live
    // A move at a jump from the touch's last recorded frame (isJump: a seek or a loop wrap while touching) ends the
    // touch at its last frame and starts a new one here: the stretch jumped over keeps its curve
    void move(int track, AutoTarget target, int frame, double heard);
    // Playing on with the controls held still (the mixer's tick, every few frames): every open touch records its last
    // value up to `frame`, so a gap between two moves is a hold and only a jump leaves one
    void hold(int frame);
    // The playhead jumped (Engine::seeked: a seek, a loop wrap): every open touch ends at its last recorded frame; a
    // control still held starts a new touch with its next move
    void jumped();
    // A jump from `last` to `frame`: backwards (more than a frame), or forward by more than kJumpSeconds and more than
    // three times the playing time since `last` was recorded (a stalled UI that misses a few ticks is no jump)
    bool isJump(int last, int frame, double secondsSince) const;
    static constexpr double kJumpSeconds = 1.0;
    // The fader / knob let go at `frame`: the recording goes in, gliding back to the old curve; a `frame` at a jump
    // from the last recorded one (released after a seek): at that last frame, the stretch jumped over is left alone
    void release(int track, AutoTarget target, int frame);
    // Playback stopped (or write mode off): open touches are released at `frame`, the undo step is closed
    void endPass(int frame);

    bool touching(int track, AutoTarget target) const { return m_touches.contains(key(track, target)); }
    bool touching() const { return !m_touches.isEmpty(); }
    bool inPass() const { return !m_mergeKey.isEmpty(); }

private:
    static QPair<int, int> key(int track, AutoTarget t) { return {track, int(t)}; }
    double fps() const;
    Editor* m_editor;
    QHash<QPair<int, int>, Automation::Touch> m_touches;
    QHash<QPair<int, int>, QElapsedTimer> m_recorded; // since the touch's last record (isJump)
    bool jumpFrom(const QPair<int, int>& k, int frame) const; // isJump from that touch's last record
    QString m_mergeKey; // of the running pass (empty = none)
    int m_passes = 0;
};
