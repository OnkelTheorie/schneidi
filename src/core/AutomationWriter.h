// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Write mode of the Studio page (0.23, "touch"): while playing, moving a track's fader or pan knob records into that
// track's automation (core/Automation.h Touch: thinned keys), on release the value glides back to the curve that was
// there. Every change of a write pass (all touches from the first one until endPass, usually playback stopping) goes
// into one undo step ("Automation schreiben"). The caller says what is heard and when (timeline frame); no Qt widgets
// here, so tests drive it directly.

#include "core/Automation.h"

#include <QHash>
#include <QPair>
#include <QString>

class Editor;

class AutomationWriter {
public:
    explicit AutomationWriter(Editor* editor) : m_editor(editor) {}

    // The fader / knob of audio track `track` moved to `heard` (dB heard / pan heard) at timeline frame `frame` while
    // playing: the first move starts a touch (and the pass), every move records and updates the curve live
    void move(int track, AutoTarget target, int frame, double heard);
    // The fader / knob let go at `frame`: the recording goes in, gliding back to the old curve
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
    QString m_mergeKey; // of the running pass (empty = none)
    int m_passes = 0;
};
