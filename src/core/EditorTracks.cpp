// Editor: track operations (mute, hide, lock, name, color, add, remove, move).
#include "core/Editor.h"

#include "core/Dynamics.h"
#include "core/EffectFolders.h"
#include "core/Equalizer.h"
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
    setTracksLocked({ref}, {}, !cur.track(ref).locked);
}

void Editor::setTracksLocked(const QVector<TrackRef>& tracksIn, const QVector<int>& subtitleTracksIn, bool lock)
{
    const Timeline& cur = m_project->timeline();
    QVector<TrackRef> tracks; // only those that change
    for (TrackRef ref : tracksIn)
        if (ref.index >= 0 && ref.index < cur.tracks(ref.kind).size() && cur.track(ref).locked != lock
            && !tracks.contains(ref))
            tracks << ref;
    QVector<int> subtitles;
    for (int i : subtitleTracksIn)
        if (i >= 0 && i < cur.subtitles.size() && cur.subtitles[i].locked != lock && !subtitles.contains(i)) subtitles << i;
    if (tracks.isEmpty() && subtitles.isEmpty()) return;
    // Clear the selection on the tracks (clips, subtitles, transition, edit point, keyframe diamonds) before they
    // get locked
    if (lock) {
        QSet<int> onTracks;
        for (TrackRef ref : tracks)
            for (const Clip& c : cur.track(ref).clips) onTracks.insert(c.id);
        for (int i : subtitles)
            for (const SubtitleCue& c : cur.subtitles[i].cues) onTracks.insert(c.id);
        const TransitionKey t = m_selection->transition();
        const EditPoint e = m_selection->editPoint();
        if (onTracks.contains(t.leftId) || onTracks.contains(t.rightId) || onTracks.contains(e.leftId)
            || onTracks.contains(e.rightId) || onTracks.contains(m_selection->keyClip())) {
            m_selection->clear();
        } else if (m_selection->ids().intersects(onTracks)) {
            m_selection->set(m_selection->ids() - onTracks);
        }
    }
    const bool one = tracks.size() + subtitles.size() == 1;
    const QString text = one ? (lock ? T("Spur sperren") : T("Spur entsperren"))
                             : (lock ? T("Spuren sperren") : T("Spuren entsperren"));
    m_project->edit(text, [&](Timeline& tl) {
        for (TrackRef ref : tracks) tl.track(ref).locked = lock;
        for (int i : subtitles) tl.subtitles[i].locked = lock;
    });
}

void Editor::toggleSelectionTracksLock()
{
    const Timeline& tl = m_project->timeline();
    QVector<int> ids = m_selection->ids().values().toVector();
    const TransitionKey t = m_selection->transition();
    const EditPoint e = m_selection->editPoint();
    for (int id : {t.leftId, t.rightId, e.leftId, e.rightId})
        if (id) ids << id;
    QVector<TrackRef> tracks = TimelineOps::tracksOf(tl, ids);
    QVector<int> subtitles;
    for (int id : ids)
        if (int index = -1; Subtitles::find(tl, id, &index) && !subtitles.contains(index)) subtitles << index;
    if (tracks.isEmpty() && subtitles.isEmpty())
        for (TrackRef ref : {TrackRef{TrackKind::Video, m_targetVideo}, TrackRef{TrackKind::Audio, m_targetAudio}})
            if (ref.index < tl.tracks(ref.kind).size()) tracks << ref;
    bool anyUnlocked = false;
    for (TrackRef ref : tracks) anyUnlocked |= !tl.track(ref).locked;
    for (int i : subtitles) anyUnlocked |= !tl.subtitles[i].locked;
    setTracksLocked(tracks, subtitles, anyUnlocked);
}

void Editor::renameTrack(TrackRef ref, const QString& name)
{
    const Timeline& cur = m_project->timeline();
    if (ref.index < 0 || ref.index >= cur.tracks(ref.kind).size()) return;
    // Default name entered (or empty) = name automatically again
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

void Editor::setTrackEq(int track, const TrackEq& eq, const QString& mergeKey)
{
    if (track < 0 || track >= m_project->timeline().audio.size()) return;
    const TrackEq e = Equalizer::clamped(eq);
    if (m_project->timeline().audio[track].eq == e) return;
    emit mixerOnlyEdit();
    m_project->edit(T("Spur-EQ"), [&](Timeline& tl) { tl.audio[track].eq = e; }, mergeKey);
}

void Editor::setTrackDynamics(int track, const TrackDynamics& dynamics, const QString& mergeKey)
{
    if (track < 0 || track >= m_project->timeline().audio.size()) return;
    const TrackDynamics d = Compressor::clamped(dynamics);
    if (m_project->timeline().audio[track].dynamics == d) return;
    emit mixerOnlyEdit();
    m_project->edit(T("Spur-Kompressor"), [&](Timeline& tl) { tl.audio[track].dynamics = d; }, mergeKey);
}

void Editor::setTrackVolume(int track, double db, const QString& mergeKey)
{
    const Timeline& tl = m_project->timeline();
    if (track >= tl.audio.size() || track < -1) return;
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    if ((track < 0 ? tl.masterVolumeDb : tl.audio[track].volumeDb) == db) return;
    emit mixerOnlyEdit();
    m_project->edit(track < 0 ? T("Master-Lautstärke") : T("Spurlautstärke"), [&](Timeline& t) {
        (track < 0 ? t.masterVolumeDb : t.audio[track].volumeDb) = db;
    }, mergeKey);
}

void Editor::setTrackPan(int track, double pan, const QString& mergeKey)
{
    if (track < 0 || track >= m_project->timeline().audio.size()) return;
    pan = std::clamp(pan, -100.0, 100.0);
    if (m_project->timeline().audio[track].pan == pan) return;
    emit mixerOnlyEdit();
    m_project->edit(T("Spur-Pan"), [&](Timeline& t) { t.audio[track].pan = pan; }, mergeKey);
}

void Editor::setTrackMuted(int track, bool on)
{
    if (track < 0 || track >= m_project->timeline().audio.size() || m_project->timeline().audio[track].muted == on) return;
    emit mixerOnlyEdit(); // switch live, no rebuild (otherwise playback stutters)
    m_project->edit(T("Spur stumm"), [&](Timeline& t) { t.audio[track].muted = on; });
}

void Editor::setTrackSolo(int track, bool on)
{
    if (track < 0 || track >= m_project->timeline().audio.size() || m_project->timeline().audio[track].solo == on) return;
    emit mixerOnlyEdit();
    m_project->edit(T("Spur solo"), [&](Timeline& t) { t.audio[track].solo = on; });
}

void Editor::setMasterLimiter(bool on)
{
    if (m_project->timeline().masterLimiter == on) return;
    emit mixerOnlyEdit();
    m_project->edit(on ? T("Limiter an") : T("Limiter aus"), [&](Timeline& t) { t.masterLimiter = on; });
}

void Editor::setMasterLimiterCeiling(double db)
{
    db = std::clamp(db, -20.0, 0.0);
    const Timeline& tl = m_project->timeline();
    if (tl.masterLimiterDb == db && tl.masterLimiter) return;
    emit mixerOnlyEdit();
    m_project->edit(T("Limiter-Ceiling"), [&](Timeline& t) {
        t.masterLimiterDb = db;
        t.masterLimiter = true;
    });
}

void Editor::addTrack(TrackKind kind, int index)
{
    const int n = m_project->timeline().tracks(kind).size();
    index = std::clamp(index, 0, n);
    if (inMulticamSequence()) index = n; // the angle table points at the tracks: new ones only on top
    m_project->edit(kind == TrackKind::Video ? T("Videospur hinzufügen") : T("Audiospur hinzufügen"), [&](Timeline& tl) {
        Track t;
        t.kind = kind;
        tl.tracks(kind).insert(index, t);
    });
    // Target track stays on "its" track
    int& target = kind == TrackKind::Video ? m_targetVideo : m_targetAudio;
    if (target >= index) {
        ++target;
        emit targetTracksChanged();
    }
}

bool Editor::canRemoveTrack(TrackRef ref) const
{
    if (inMulticamSequence()) return false; // the angle table points at the tracks
    const auto& tracks = m_project->timeline().tracks(ref.kind);
    return tracks.size() > 1 && ref.index >= 0 && ref.index < tracks.size() && !tracks[ref.index].locked;
}

void Editor::removeTrack(TrackRef ref)
{
    if (!canRemoveTrack(ref)) return;
    const Timeline& cur = m_project->timeline();
    // Clear the selection on the track (clips, transition, keyframe diamonds)
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
    if (from < 0 || from >= n || inMulticamSequence()) return;
    to = std::clamp(to, 0, n - 1);
    if (to == from) return;
    m_project->edit(T("Spur verschieben"), [&](Timeline& tl) { tl.tracks(kind).move(from, to); });
    // Target track follows its track
    int& target = kind == TrackKind::Video ? m_targetVideo : m_targetAudio;
    const int old = target;
    if (target == from) target = to;
    else if (from < target && target <= to) --target;
    else if (to <= target && target < from) ++target;
    if (target != old) emit targetTracksChanged();
}
