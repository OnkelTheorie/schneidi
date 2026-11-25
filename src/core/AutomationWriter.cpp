// SPDX-License-Identifier: AGPL-3.0-only
#include "core/AutomationWriter.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"

#include <algorithm>

double AutomationWriter::fps() const { return m_editor->project()->format().rate.fps(); }

bool AutomationWriter::isJump(int last, int frame, double secondsSince) const
{
    if (frame < last - 1) return true; // back: a loop wrap, a seek back
    const double played = std::max(kJumpSeconds, 3 * std::max(0.0, secondsSince)) * fps();
    return frame - last > played;
}

bool AutomationWriter::jumpFrom(const QPair<int, int>& k, int frame) const
{
    const auto it = m_touches.constFind(k);
    if (it == m_touches.cend() || it->heard.isEmpty()) return false;
    const auto t = m_recorded.constFind(k);
    const double since = t != m_recorded.cend() && t->isValid() ? t->elapsed() / 1000.0 : 0.0;
    return isJump(it->heard.last().first, frame, since);
}

void AutomationWriter::dropStale()
{
    const Project* p = m_editor->project();
    const QVector<Track>& audio = p->timeline().audio;
    bool dropped = false;
    for (auto it = m_owners.begin(); it != m_owners.end();) {
        const int track = it.key().first;
        if (it->sequence == p->currentSequence() && track < audio.size() && audio[track].uid.value == it->track) {
            ++it;
            continue;
        }
        m_touches.remove(it.key());
        m_recorded.remove(it.key());
        it = m_owners.erase(it);
        dropped = true;
    }
    if (!dropped) return;
    const auto keys = m_touches.keys();
    for (const auto& k : keys) {
        const Automation::Touch& t = m_touches[k];
        release(k.first, AutoTarget(k.second), t.heard.isEmpty() ? 0 : t.heard.last().first);
    }
    if (!m_mergeKey.isEmpty()) m_editor->project()->closeMerge();
    m_mergeKey.clear();
}

void AutomationWriter::move(int track, AutoTarget target, int frame, double heard)
{
    dropStale();
    const Timeline& tl = m_editor->project()->timeline();
    if (track < 0 || track >= tl.audio.size() || tl.audio[track].locked) return;
    const auto k = key(track, target);
    if (jumpFrom(k, frame)) {
        // A jump (a seek or a loop wrap while touching): what was played goes in up to its last frame (gliding back
        // there), the stretch jumped over keeps its curve, a new touch starts here
        release(track, target, m_touches.value(k).heard.last().first);
    }
    auto it = m_touches.find(k);
    if (it == m_touches.end()) {
        if (m_mergeKey.isEmpty()) { // a new pass: its own undo step
            m_editor->project()->closeMerge();
            m_mergeKey = QString("automation-write-%1").arg(++m_passes);
        }
        it = m_touches.insert(k, Automation::beginTouch(m_editor->project()->timeline().audio[track], target));
        m_owners.insert(k, {m_editor->project()->currentSequence(), tl.audio[track].uid.value});
    }
    Automation::record(*it, frame, heard);
    m_recorded[k].start();
    m_editor->setTrackAutomation(track, Automation::touchingCurve(*it, fps()), T("Automation schreiben"), m_mergeKey, target);
}

void AutomationWriter::release(int track, AutoTarget target, int frame)
{
    dropStale();
    const auto k = key(track, target);
    const auto it = m_touches.find(k);
    if (it == m_touches.end()) return;
    // Let go after a jump (the fader held still through a seek): the recording ends where it was played, the stretch
    // jumped over is not overwritten with the held value
    if (jumpFrom(k, frame)) frame = it->heard.last().first;
    const Automation::Touch touch = *it;
    m_touches.erase(it);
    m_recorded.remove(k);
    m_owners.remove(k);
    m_editor->setTrackAutomation(track, Automation::releasedCurve(touch, frame, fps()), T("Automation schreiben"),
                                 m_mergeKey, target);
}

void AutomationWriter::hold(int frame)
{
    dropStale();
    for (auto it = m_touches.begin(); it != m_touches.end(); ++it) {
        Automation::Touch& t = it.value();
        if (t.heard.isEmpty()) continue;
        const auto [last, value] = t.heard.last();
        // Only playing on (a jump stays a jump for move() and release(); the curve shown already holds the value)
        if (frame > last && !jumpFrom(it.key(), frame)) {
            Automation::record(t, frame, value);
            m_recorded[it.key()].start();
        }
    }
}

void AutomationWriter::jumped()
{
    dropStale();
    const auto keys = m_touches.keys();
    for (const auto& k : keys) {
        const Automation::Touch& t = m_touches[k];
        release(k.first, AutoTarget(k.second), t.heard.isEmpty() ? 0 : t.heard.last().first);
    }
}

void AutomationWriter::endPass(int frame)
{
    dropStale();
    const auto keys = m_touches.keys();
    for (const auto& k : keys) release(k.first, AutoTarget(k.second), frame);
    if (!m_mergeKey.isEmpty()) m_editor->project()->closeMerge();
    m_mergeKey.clear();
}
