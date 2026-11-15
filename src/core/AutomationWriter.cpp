// SPDX-License-Identifier: AGPL-3.0-only
#include "core/AutomationWriter.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"

double AutomationWriter::fps() const { return m_editor->project()->format().rate.fps(); }

void AutomationWriter::move(int track, AutoTarget target, int frame, double heard)
{
    const Timeline& tl = m_editor->project()->timeline();
    if (track < 0 || track >= tl.audio.size() || tl.audio[track].locked) return;
    auto it = m_touches.find(key(track, target));
    if (it == m_touches.end()) {
        if (m_mergeKey.isEmpty()) { // a new pass: its own undo step
            m_editor->project()->closeMerge();
            m_mergeKey = QString("automation-write-%1").arg(++m_passes);
        }
        it = m_touches.insert(key(track, target), Automation::beginTouch(tl.audio[track], target));
    } else if (!it->heard.isEmpty() && frame - it->heard.last().first > kJumpSeconds * fps()) {
        // A jump forward (a seek while touching): what was played goes in up to its last frame (gliding back there),
        // the stretch jumped over keeps its curve, a new touch starts here
        const int last = it->heard.last().first;
        release(track, target, last);
        it = m_touches.insert(key(track, target), Automation::beginTouch(m_editor->project()->timeline().audio[track], target));
    }
    Automation::record(*it, frame, heard);
    m_editor->setTrackAutomation(track, Automation::touchingCurve(*it, fps()), T("Automation schreiben"), m_mergeKey, target);
}

void AutomationWriter::release(int track, AutoTarget target, int frame)
{
    const auto it = m_touches.find(key(track, target));
    if (it == m_touches.end()) return;
    const Automation::Touch touch = *it;
    m_touches.erase(it);
    m_editor->setTrackAutomation(track, Automation::releasedCurve(touch, frame, fps()), T("Automation schreiben"),
                                 m_mergeKey, target);
}

void AutomationWriter::hold(int frame)
{
    for (Automation::Touch& t : m_touches) {
        if (t.heard.isEmpty()) continue;
        const auto [last, value] = t.heard.last();
        // Only playing on (a jump stays a jump for move(); the curve shown already holds the last value)
        if (frame > last && frame - last <= kJumpSeconds * fps()) Automation::record(t, frame, value);
    }
}

void AutomationWriter::endPass(int frame)
{
    const auto keys = m_touches.keys();
    for (const auto& k : keys) release(k.first, AutoTarget(k.second), frame);
    if (!m_mergeKey.isEmpty()) m_editor->project()->closeMerge();
    m_mergeKey.clear();
}
