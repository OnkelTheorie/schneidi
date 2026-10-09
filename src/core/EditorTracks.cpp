// Editor: track operations (mute, hide, lock, name, color, add, remove, move).
#include "core/Editor.h"

#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Retime.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <algorithm>

void Editor::toggleTrackMute(TrackRef ref)
{
    if (ref.kind == TrackKind::Audio) emit mixerOnlyEdit();
    m_project->edit(T("Spur stumm"), [&](Timeline& tl) { tl.track(ref).muted = !tl.track(ref).muted; });
}

void Editor::toggleTrackHidden(TrackRef ref)
{
    m_project->edit(T("Spur ausblenden"), [&](Timeline& tl) { tl.track(ref).hidden = !tl.track(ref).hidden; });
}

void Editor::toggleTrackLock(TrackRef ref)
{
    const Timeline& cur = m_project->timeline();
    if (ref.index < 0 || ref.index >= cur.tracks(ref.kind).size()) return;
    const bool lock = !cur.track(ref).locked;
    // Auswahl auf der Spur aufheben (Clips, Übergang, Keyframe-Rauten), bevor sie gesperrt wird
    if (lock) {
        QSet<int> onTrack;
        for (const Clip& c : cur.track(ref).clips) onTrack.insert(c.id);
        const TransitionKey t = m_selection->transition();
        if (onTrack.contains(t.leftId) || onTrack.contains(t.rightId) || onTrack.contains(m_selection->keyClip())) {
            m_selection->clear();
        } else if (m_selection->ids().intersects(onTrack)) {
            m_selection->set(m_selection->ids() - onTrack);
        }
    }
    m_project->edit(lock ? T("Spur sperren") : T("Spur entsperren"), [&](Timeline& tl) { tl.track(ref).locked = lock; });
}

void Editor::renameTrack(TrackRef ref, const QString& name)
{
    const Timeline& cur = m_project->timeline();
    if (ref.index < 0 || ref.index >= cur.tracks(ref.kind).size()) return;
    // Standardname eingegeben (oder leer) = wieder automatisch benennen
    QString n = name.simplified();
    if (n == trackDisplayName(Track{}, ref)) n.clear();
    if (n == cur.track(ref).name) return;
    m_project->edit(T("Spur umbenennen"), [&](Timeline& tl) { tl.track(ref).name = n; });
}

void Editor::setTrackColor(TrackRef ref, const QString& colorId)
{
    const Timeline& cur = m_project->timeline();
    if (ref.index < 0 || ref.index >= cur.tracks(ref.kind).size() || cur.track(ref).color == colorId) return;
    if (!colorId.isEmpty() && !trackColorInfo(colorId)) return;
    m_project->edit(T("Spurfarbe ändern"), [&](Timeline& tl) { tl.track(ref).color = colorId; });
}

void Editor::addTrack(TrackKind kind, int index)
{
    const int n = m_project->timeline().tracks(kind).size();
    index = std::clamp(index, 0, n);
    m_project->edit(kind == TrackKind::Video ? T("Videospur hinzufügen") : T("Audiospur hinzufügen"), [&](Timeline& tl) {
        Track t;
        t.kind = kind;
        tl.tracks(kind).insert(index, t);
    });
    // Zielspur bleibt an „ihrer“ Spur
    int& target = kind == TrackKind::Video ? m_targetVideo : m_targetAudio;
    if (target >= index) {
        ++target;
        emit targetTracksChanged();
    }
}

bool Editor::canRemoveTrack(TrackRef ref) const
{
    const auto& tracks = m_project->timeline().tracks(ref.kind);
    return tracks.size() > 1 && ref.index >= 0 && ref.index < tracks.size() && !tracks[ref.index].locked;
}

void Editor::removeTrack(TrackRef ref)
{
    if (!canRemoveTrack(ref)) return;
    const Timeline& cur = m_project->timeline();
    // Auswahl auf der Spur aufheben (Clips, Übergang, Keyframe-Rauten)
    QSet<int> onTrack;
    for (const Clip& c : cur.track(ref).clips) onTrack.insert(c.id);
    const TransitionKey t = m_selection->transition();
    if (onTrack.contains(t.leftId) || onTrack.contains(t.rightId) || onTrack.contains(m_selection->keyClip()))
        m_selection->clear();
    else if (m_selection->ids().intersects(onTrack))
        m_selection->set(m_selection->ids() - onTrack);
    m_project->edit(ref.kind == TrackKind::Video ? T("Videospur löschen") : T("Audiospur löschen"),
                    [&](Timeline& tl) { tl.tracks(ref.kind).removeAt(ref.index); });
    int& target = ref.kind == TrackKind::Video ? m_targetVideo : m_targetAudio;
    if (target > ref.index || target >= m_project->timeline().tracks(ref.kind).size()) {
        target = std::max(0, target - 1);
        emit targetTracksChanged();
    }
}

void Editor::moveTrack(TrackKind kind, int from, int to)
{
    const int n = m_project->timeline().tracks(kind).size();
    if (from < 0 || from >= n) return;
    to = std::clamp(to, 0, n - 1);
    if (to == from) return;
    m_project->edit(T("Spur verschieben"), [&](Timeline& tl) { tl.tracks(kind).move(from, to); });
    // Zielspur folgt ihrer Spur
    int& target = kind == TrackKind::Video ? m_targetVideo : m_targetAudio;
    const int old = target;
    if (target == from) target = to;
    else if (from < target && target <= to) --target;
    else if (to <= target && target < from) ++target;
    if (target != old) emit targetTracksChanged();
}
