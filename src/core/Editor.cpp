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

namespace {

void removeCues(Timeline& tl, const QVector<int>& ids)
{
    for (SubtitleTrack& t : tl.subtitles)
        t.cues.erase(std::remove_if(t.cues.begin(), t.cues.end(), [&](const SubtitleCue& c) { return ids.contains(c.id); }),
                     t.cues.end());
}

} // namespace

QVector<int> Editor::clipIdsOf(const QSet<int>& ids) const
{
    QVector<int> out;
    for (int id : ids)
        if (TimelineOps::findClip(m_project->timeline(), id)) out << id;
    return out;
}

Editor::Editor(Project* project, Selection* selection, QObject* parent)
    : QObject(parent), m_project(project), m_selection(selection) {}

QVector<int> Editor::withLinked(const QVector<int>& ids) const
{
    if (!m_linkedSelection) return editable(ids);
    QVector<int> out;
    for (int id : editable(ids)) // gesperrter Clip nimmt auch seine Partner nicht mit
        for (int g : TimelineOps::linkedGroup(m_project->timeline(), id))
            if (!out.contains(g)) out << g;
    return editable(out); // Partner auf gesperrter Spur bleibt liegen (wie DaVinci)
}

QVector<int> Editor::editable(const QVector<int>& ids) const
{
    return TimelineOps::unlocked(m_project->timeline(), ids);
}

bool Editor::isTrackLocked(TrackRef ref) const
{
    const Timeline& tl = m_project->timeline();
    return ref.index >= 0 && ref.index < tl.tracks(ref.kind).size() && tl.track(ref).locked;
}

void Editor::addMediaAt(const QStringList& paths, int frame, int track)
{
    Project* p = m_project;
    QVector<const MediaInfo*> infos;
    for (const QString& path : paths)
        if (const MediaInfo* info = p->mediaInfo(path); info && info->length > 0) infos << info;
    if (infos.isEmpty()) return;
    // gesperrte Zielspur bekommt nichts (wie DaVinci); fehlende Spuren entstehen ungesperrt
    const int idx0 = std::max(0, track);
    const bool videoOk = !isTrackLocked({TrackKind::Video, idx0});
    const bool audioOk = !isTrackLocked({TrackKind::Audio, idx0});
    infos.erase(std::remove_if(infos.begin(), infos.end(),
                               [&](const MediaInfo* i) { return !(i->hasVideo && videoOk) && !(i->hasAudio && audioOk); }),
                infos.end());
    if (infos.isEmpty()) return;

    const QString text = infos.size() == 1 ? T("Einfügen: %1").arg(infos.first()->name)
                                           : T("%1 Clips einfügen").arg(infos.size());
    p->edit(text, [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        const int idx = std::max(0, track);
        int start = std::max(0, frame);
        for (const MediaInfo* info : infos) {
            Clip c;
            c.mediaPath = info->path;
            c.start = start;
            c.in = 0;
            c.out = info->length - 1;
            const bool video = info->hasVideo && videoOk;
            // Pro Ton-Stream ein Clip auf der nächsten Spur (wie DaVinci); gesperrte Spuren bekommen nichts
            QVector<int> streams;
            for (int k = 0; k < info->audioStreamCount(); ++k)
                if (k == 0 ? audioOk : !isTrackLocked({TrackKind::Audio, idx + k})) streams << k;
            c.linkId = int(video) + streams.size() > 1 ? p->newLinkId() : 0;
            if (video) {
                TimelineOps::ensureTracks(tl, TrackKind::Video, idx + 1);
                Clip v = c;
                v.id = newId();
                TimelineOps::placeClip(tl.video[idx], v, newId);
            }
            for (int k : streams) {
                TimelineOps::ensureTracks(tl, TrackKind::Audio, idx + k + 1);
                Clip a = c;
                a.id = newId();
                a.audioStream = k;
                TimelineOps::placeClip(tl.audio[idx + k], a, newId);
            }
            start += info->length;
        }
    });
}

void Editor::addTitle(int frame, int track)
{
    Project* p = m_project;
    const Timeline& cur = p->timeline();
    Clip c;
    c.kind = ClipKind::Title;
    c.title.text = T("Titel");
    c.start = std::max(0, frame);
    c.in = 0;
    c.out = 5 * p->fps() - 1;
    if (track < 0) {
        track = 0;
        for (int i = 0; i < cur.video.size(); ++i)
            for (const Clip& x : cur.video[i].clips)
                if (x.start < c.end() && x.end() > c.start) track = i + 1;
        while (isTrackLocked({TrackKind::Video, track})) ++track; // gesperrte Spur überspringen
    } else if (isTrackLocked({TrackKind::Video, track})) {
        return;
    }
    c.id = p->newClipId();
    p->edit(T("Titel einfügen"), [&](Timeline& tl) {
        TimelineOps::ensureTracks(tl, TrackKind::Video, track + 1);
        TimelineOps::placeClip(tl.video[track], c, [p] { return p->newClipId(); });
    });
    m_selection->set({c.id});
}

void Editor::moveClips(const QVector<int>& idsIn, int deltaFrames, TrackKind kind, int trackDelta)
{
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty() || (deltaFrames == 0 && trackDelta == 0)) return;
    // nie auf eine gesperrte Spur verschieben
    const Timeline& cur = m_project->timeline();
    const int dt = TimelineOps::clampTrackDelta(cur, ids, kind, trackDelta);
    for (TrackRef ref : TimelineOps::tracksOf(cur, ids))
        if (isTrackLocked({ref.kind, ref.index + dt})) return;
    Project* p = m_project;
    p->edit(T("Clips verschieben"), [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::moveClips(tl, ids, deltaFrames, kind, trackDelta,
                               [p] { return p->newClipId(); });
    });
}

void Editor::moveClipsFine(const QVector<int>& idsIn, int fine, TrackKind kind, int trackDelta)
{
    if (fine % 100 == 0) return moveClips(idsIn, fine / 100, kind, trackDelta);
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty()) return;
    const Timeline& cur = m_project->timeline();
    const int dt = TimelineOps::clampTrackDelta(cur, ids, kind, trackDelta);
    for (TrackRef ref : TimelineOps::tracksOf(cur, ids))
        if (isTrackLocked({ref.kind, ref.index + dt})) return;
    // Ziel je Clip; ganze Frames über moveClips (Überschreiben wie gewohnt), danach die Feinposition
    QHash<int, Clip> target;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(cur, id)) {
            Clip t = *c;
            TimelineOps::shiftFine(t, fine);
            target.insert(id, t);
        }
    if (target.isEmpty()) return;
    const int frames = target.begin()->start - TimelineOps::findClip(cur, target.begin().key())->start;
    Project* p = m_project;
    p->edit(T("Clips verschieben"), [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::moveClips(tl, ids, frames, kind, trackDelta, [p] { return p->newClipId(); });
        for (auto it = target.cbegin(); it != target.cend(); ++it)
            if (Clip* c = TimelineOps::findClip(tl, it.key())) c->subframe = it->subframe;
    });
}

void Editor::setClipVolume(int clipId, double db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || c->volumeDb == db || TimelineOps::isLocked(m_project->timeline(), clipId)) return;
    m_project->edit(T("Lautstärke"), [&](Timeline& tl) {
        if (Clip* clip = TimelineOps::findClip(tl, clipId)) clip->volumeDb = db;
    });
}

void Editor::normalizeAudio(const QHash<int, double>& peakDb, double targetDb, bool relative,
                            std::optional<double> relativeRef)
{
    const Timeline& cur = m_project->timeline();
    QHash<int, double> gain; // neue Clip-Lautstärke (dB)
    double loudest = -1e9;
    for (auto it = peakDb.begin(); it != peakDb.end(); ++it) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(cur, it.key(), &ref);
        if (!c || ref.kind != TrackKind::Audio || c->isTitle() || TimelineOps::isLocked(cur, c->id)) continue;
        if (it.value() <= -100.0) continue; // Stille: nichts anzuheben
        gain[c->id] = targetDb - it.value();
        loudest = std::max(loudest, it.value());
    }
    if (gain.isEmpty()) return;
    if (relative && relativeRef && *relativeRef <= -100.0) return; // gemeinsam gemessen: Stille
    if (relative)
        for (double& g : gain) g = targetDb - (relativeRef ? *relativeRef : loudest);
    for (double& g : gain) g = std::clamp(g, kMinVolumeDb, kMaxVolumeDb);
    m_project->edit(T("Audiopegel normalisieren"), [&](Timeline& tl) {
        for (auto it = gain.begin(); it != gain.end(); ++it) {
            Clip* c = TimelineOps::findClip(tl, it.key());
            if (!c) continue;
            if (Keys::animated(*c, AnimParam::Volume)) {
                KeyTrack& keys = c->keys[AnimParam::Volume];
                double top = kMinVolumeDb;
                for (const Keyframe& k : keys) top = std::max(top, k.value);
                const double delta = it.value() - top;
                for (Keyframe& k : keys) k.value = std::clamp(k.value + delta, kMinVolumeDb, kMaxVolumeDb);
            } else {
                c->volumeDb = it.value();
            }
        }
    });
}

QVector<int> Editor::selectedAudioClips() const
{
    const Timeline& tl = m_project->timeline();
    QVector<int> out;
    for (int id : withLinked(m_selection->ids().values().toVector())) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (c && ref.kind == TrackKind::Audio && !c->mediaPath.isEmpty() && !TimelineOps::isLocked(tl, id)) out << id;
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Editor::bladeAt(int clipId, int frame)
{
    Project* p = m_project;
    int st = -1;
    if (const SubtitleCue* c = Subtitles::find(p->timeline(), clipId, &st)) { // Untertitel: nur dieser Eintrag
        if (isSubtitleTrackLocked(st) || frame <= c->start || frame >= c->end) return;
        p->edit(T("Schnitt"), [&](Timeline& tl) { Subtitles::splitAt(tl.subtitles[st], frame, [p] { return p->newClipId(); }); });
        return;
    }
    const QVector<int> ids = withLinked({clipId});
    if (ids.isEmpty()) return;
    p->edit(T("Schnitt"), [&](Timeline& tl) {
        TimelineOps::splitAt(tl, ids, frame, [p] { return p->newClipId(); },
                             [p] { return p->newLinkId(); });
    });
}

QVector<int> Editor::targetIds(int frame) const
{
    if (!m_selection->isEmpty()) return withLinked(m_selection->ids().values().toVector());
    QVector<int> ids;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips)
                if (frame > c.start && frame < c.end()) ids << c.id;
    return editable(ids);
}

void Editor::splitAtPlayhead(int frame)
{
    // Wie DaVinci: mit Auswahl nur die ausgewählten Clips, sonst alle unter dem Playhead.
    // Optional: alle Clips auf den Spuren der Auswahl (Playhead steht schon auf dem Nachbarclip).
    QVector<int> ids;
    if (m_splitOnSelectedTracks && !m_selection->isEmpty()) {
        const Timeline& tl = m_project->timeline();
        const QVector<int> selected = withLinked(m_selection->ids().values().toVector());
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const auto& t : tl.tracks(k)) {
                const bool hasSelected = std::any_of(t.clips.begin(), t.clips.end(),
                                                     [&](const Clip& c) { return selected.contains(c.id); });
                if (!hasSelected) continue;
                for (const auto& c : t.clips)
                    if (frame > c.start && frame < c.end()) ids << c.id;
            }
        ids = withLinked(ids);
    } else {
        ids = targetIds(frame);
    }
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&](int id) { return !TimelineOps::findClip(m_project->timeline(), id); }),
              ids.end()); // ausgewählte Untertitel stecken auch in der Auswahl
    // Untertitel wie Clips: ohne Auswahl der Eintrag unter dem Playhead auf jeder nicht gesperrten Untertitelspur,
    // sonst die ausgewählten Einträge (bzw. alle Untertitelspuren mit ausgewähltem Eintrag)
    QVector<int> cueTracks;
    const Timeline& cur = m_project->timeline();
    const QVector<int> selectedCues = selectedSubtitles();
    for (int t = 0; t < cur.subtitles.size(); ++t) {
        const SubtitleTrack& st = cur.subtitles[t];
        const int i = Subtitles::cueAt(st, frame);
        if (st.locked || i < 0 || st.cues[i].start == frame) continue;
        const bool take = m_selection->isEmpty() || selectedCues.contains(st.cues[i].id)
                          || (m_splitOnSelectedTracks && std::any_of(st.cues.begin(), st.cues.end(), [&](const SubtitleCue& c) {
                                 return selectedCues.contains(c.id);
                             }));
        if (take) cueTracks << t;
    }
    if (ids.isEmpty() && cueTracks.isEmpty()) return;
    Project* p = m_project;
    p->edit(T("Schnitt am Playhead"), [&](Timeline& tl) {
        TimelineOps::splitAt(tl, ids, frame, [p] { return p->newClipId(); },
                             [p] { return p->newLinkId(); });
        for (int t : cueTracks) Subtitles::splitAt(tl.subtitles[t], frame, [p] { return p->newClipId(); });
    });
}

void Editor::setClipFade(int clipId, TimelineOps::Edge edge, int frames, const QString& mergeKey)
{
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || TimelineOps::isLocked(m_project->timeline(), clipId)) return;
    const bool in = edge == TimelineOps::Edge::Start;
    // Der andere Fade kann nach dem Kürzen länger als der Clip sein -> Obergrenze nie negativ
    frames = std::clamp(frames, 0, std::max(0, c->length() - (in ? c->fadeOut : c->fadeIn)));
    if (frames == (in ? c->fadeIn : c->fadeOut)) return;
    m_project->edit(in ? T("Einblenden") : T("Ausblenden"), [&](Timeline& tl) {
        if (Clip* x = TimelineOps::findClip(tl, clipId)) (in ? x->fadeIn : x->fadeOut) = frames;
    }, mergeKey);
}

void Editor::modifyClips(const QVector<int>& idsIn, const QString& text, const std::function<void(Clip&)>& fn,
                         const QString& mergeKey)
{
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty()) return;
    m_project->edit(text, [&](Timeline& tl) {
        for (int id : ids)
            if (Clip* c = TimelineOps::findClip(tl, id)) fn(*c);
    }, mergeKey);
}

void Editor::deleteRange(int from, int to, bool ripple)
{
    from = std::max(0, from);
    if (to <= from) return;
    Project* p = m_project;
    p->edit(ripple ? T("Bereich löschen mit Ripple") : T("Bereich löschen"), [&](Timeline& tl) {
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (Track& t : tl.tracks(k)) {
                if (t.locked) continue;
                TimelineOps::clearDissolveAt(t, from);
                TimelineOps::clearDissolveAt(t, to);
                TimelineOps::clearRange(t, from, to, [p] { return p->newClipId(); });
            }
        TimelineOps::resolvePendingLinks(tl, [p] { return p->newLinkId(); });
        if (ripple) TimelineOps::rippleTracks(tl, {{to, from - to}}, {});
    });
    m_selection->clear();
}

void Editor::rippleDeleteSelection()
{
    const QVector<int> cues = selectedSubtitles();
    const QVector<int> ids = editable(clipIdsOf(m_selection->ids()));
    if (ids.isEmpty() && cues.isEmpty()) return;
    m_project->edit(T("Löschen mit Ripple"), [&](Timeline& tl) {
        removeCues(tl, cues); // Untertitel: ohne Ripple (Lücke bleibt)
        if (ids.isEmpty()) return;
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::rippleDelete(tl, ids);
    });
    m_selection->clear();
}

void Editor::selectAll()
{
    QSet<int> ids;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            if (!t.locked) // gesperrte Spuren lassen sich nicht auswählen
                for (const auto& c : t.clips) ids.insert(c.id);
    m_selection->set(ids);
}

void Editor::selectFromPlayhead(int frame, bool forward, bool allTracks)
{
    const Timeline& tl = m_project->timeline();
    QVector<TrackRef> tracks;
    if (!allTracks) {
        tracks = TimelineOps::tracksOf(tl, m_selection->ids().values().toVector());
        if (tracks.isEmpty()) tracks = {{TrackKind::Video, m_targetVideo}, {TrackKind::Audio, m_targetAudio}};
    }
    QVector<int> ids;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (int i = 0; i < tl.tracks(k).size(); ++i) {
            const Track& t = tl.tracks(k)[i];
            if (t.locked || (!allTracks && !tracks.contains({k, i}))) continue;
            // der Clip unter dem Playhead gehört dazu (wie DaVinci)
            for (const Clip& c : t.clips)
                if (forward ? c.end() > frame : c.start <= frame) ids << c.id;
        }
    ids = withLinked(ids);
    m_selection->set(QSet<int>(ids.begin(), ids.end()));
}

void Editor::nudgeSelection(int frames)
{
    const QVector<int> ids = editable(m_selection->ids().values().toVector());
    if (ids.isEmpty()) return;
    int minStart = INT_MAX;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) minStart = std::min(minStart, c->start);
    frames = std::max(frames, -minStart); // nicht vor Frame 0
    if (frames == 0) return;
    Project* p = m_project;
    p->edit("Nudge", [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        TimelineOps::moveClips(tl, ids, frames, TrackKind::Video, 0, [p] { return p->newClipId(); });
    });
}

void Editor::toggleSelectionEnabled()
{
    const QVector<int> ids = editable(m_selection->ids().values().toVector());
    if (ids.isEmpty()) return;
    // Wie DaVinci: ist einer aktiv, werden alle deaktiviert
    bool anyEnabled = false;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) anyEnabled |= c->enabled;
    modifyClips(ids, anyEnabled ? T("Clip deaktivieren") : T("Clip aktivieren"),
                [anyEnabled](Clip& c) { c.enabled = !anyEnabled; });
}

namespace {
// Videoclip mit Medium (Titel und Audioclips werden nicht gecacht)
bool cacheable(const Timeline& tl, int id)
{
    TrackRef ref;
    const Clip* c = TimelineOps::findClip(tl, id, &ref);
    return c && ref.kind == TrackKind::Video && !c->isTitle() && !c->mediaPath.isEmpty();
}
} // namespace

int Editor::selectionRenderCacheState() const
{
    int on = 0, n = 0;
    for (int id : m_selection->ids())
        if (cacheable(m_project->timeline(), id)) {
            ++n;
            on += TimelineOps::findClip(m_project->timeline(), id)->renderCache ? 1 : 0;
        }
    if (n == 0) return -1;
    return on == n ? 1 : on == 0 ? 0 : 2;
}

void Editor::toggleSelectionRenderCache()
{
    QVector<int> ids;
    for (int id : editable(m_selection->ids().values().toVector()))
        if (cacheable(m_project->timeline(), id)) ids << id;
    if (ids.isEmpty()) return;
    const bool on = selectionRenderCacheState() != 1;
    modifyClips(ids, on ? T("Render-Cache an") : T("Render-Cache aus"), [on](Clip& c) { c.renderCache = on; });
}

void Editor::toggleLinkSelection()
{
    const QVector<int> ids = editable(m_selection->ids().values().toVector());
    if (ids.isEmpty()) return;
    bool anyLinked = false;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) anyLinked |= c->linkId != 0;
    if (!anyLinked && ids.size() < 2) return;
    const int link = anyLinked ? 0 : m_project->newLinkId();
    modifyClips(ids, anyLinked ? T("Verknüpfung lösen") : T("Clips verknüpfen"), [link](Clip& c) { c.linkId = link; });
}

void Editor::toggleMarker(int frame)
{
    m_project->edit("Marker", [&](Timeline& tl) {
        if (tl.markers.contains(frame)) {
            tl.markers.removeAll(frame);
        } else {
            tl.markers << frame;
            std::sort(tl.markers.begin(), tl.markers.end());
        }
    });
}

void Editor::setMarkIn(int frame)
{
    if (frame == m_project->timeline().markIn) return;
    m_project->edit(frame < 0 ? T("In-Punkt entfernen") : T("In-Punkt setzen"), [&](Timeline& tl) {
        tl.markIn = frame;
        if (frame >= 0 && tl.markOut >= 0 && tl.markOut < frame) tl.markOut = -1;
    });
}

void Editor::setMarkOut(int frame)
{
    if (frame == m_project->timeline().markOut) return;
    m_project->edit(frame < 0 ? T("Out-Punkt entfernen") : T("Out-Punkt setzen"), [&](Timeline& tl) {
        tl.markOut = frame;
        if (frame >= 0 && tl.markIn > frame) tl.markIn = -1;
    });
}

void Editor::clearMarks()
{
    const Timeline& t = m_project->timeline();
    if (t.markIn < 0 && t.markOut < 0) return;
    m_project->edit(T("In/Out entfernen"), [](Timeline& tl) { tl.markIn = tl.markOut = -1; });
}

void Editor::setTargetTracks(int video, int audio)
{
    video = std::max(0, video);
    audio = std::max(0, audio);
    if (video == m_targetVideo && audio == m_targetAudio) return;
    m_targetVideo = video;
    m_targetAudio = audio;
    emit targetTracksChanged();
}

void Editor::copySelection()
{
    // Überblendung zu einem nicht mitkopierten Clip gehört nicht dazu (sonst blendet die Kopie über Schwarz aus)
    Timeline tl = m_project->timeline();
    TimelineOps::detachTransitions(tl, clipIdsOf(m_selection->ids()));
    QVector<ClipboardItem> items;
    int minStart = INT_MAX;
    for (int id : m_selection->ids()) {
        TrackRef ref;
        if (const Clip* c = TimelineOps::findClip(tl, id, &ref)) {
            items << ClipboardItem{*c, ref};
            minStart = std::min(minStart, c->start);
        }
    }
    if (items.isEmpty()) return;
    for (auto& it : items) it.clip.start -= minStart;
    m_clipboard = items;
}

void Editor::cutSelection()
{
    // Nur löschen, was auch in die Zwischenablage kommt (Clips): ausgewählte Untertitel/Keyframes/Übergänge gingen
    // sonst verloren, und Einfügen brachte die alte Zwischenablage
    const QVector<int> ids = editable(clipIdsOf(m_selection->ids()));
    if (ids.isEmpty()) return;
    copySelection();
    m_project->edit(T("Ausschneiden"), [&](Timeline& tl) {
        TimelineOps::detachTransitions(tl, ids);
        for (int id : ids) TimelineOps::removeClip(tl, id);
    });
    m_selection->clear();
}

void Editor::paste(int frame)
{
    if (m_clipboard.isEmpty()) return;
    // nicht auf gesperrte Spuren einfügen
    QVector<ClipboardItem> items;
    for (const auto& it : m_clipboard)
        if (!isTrackLocked(it.ref)
            && (!it.clip.isCompound() || m_project->canNest(it.clip.sequenceId, m_project->currentSequence())))
            items << it; // Compound Clip nie in sich selbst (Schleife)
    if (items.isEmpty()) return;
    Project* p = m_project;
    QSet<int> pasted;
    p->edit(T("Einfügen"), [&](Timeline& tl) {
        QHash<int, int> linkMap; // Kopie bekommt eigene Verknüpfung
        for (const auto& it : items) {
            Clip c = it.clip;
            c.id = p->newClipId();
            c.start += frame;
            if (c.linkId) {
                if (!linkMap.contains(c.linkId)) linkMap[c.linkId] = p->newLinkId();
                c.linkId = linkMap[c.linkId];
            }
            TimelineOps::ensureTracks(tl, it.ref.kind, it.ref.index + 1);
            TimelineOps::placeClip(tl.track(it.ref), c, [p] { return p->newClipId(); });
            pasted.insert(c.id);
        }
    });
    m_selection->set(pasted);
}

void Editor::setKeyframes(const QVector<int>& idsIn, const QVector<AnimParam>& params, int frame, bool on)
{
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty() || params.isEmpty()) return;
    m_project->edit(on ? T("Keyframe setzen") : T("Keyframe entfernen"), [&](Timeline& tl) {
        for (int id : ids) {
            Clip* c = TimelineOps::findClip(tl, id);
            if (!c) continue;
            const int t = std::clamp(frame - c->start, 0, c->length() - 1);
            for (AnimParam p : params) {
                if (on) Keys::setKey(*c, p, t, Keys::valueAt(*c, p, t));
                else Keys::removeKey(*c, p, t);
            }
        }
    });
}

void Editor::setKeyframeEase(const QVector<int>& idsIn, const QVector<AnimParam>& params, int frame, KeyEase ease)
{
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty()) return;
    m_project->edit(T("Keyframe-Verlauf"), [&](Timeline& tl) {
        for (int id : ids)
            if (Clip* c = TimelineOps::findClip(tl, id))
                Keys::setEase(*c, {std::clamp(frame - c->start, 0, c->length() - 1)}, ease, params);
    });
}

void Editor::moveKeyframes(int clipId, const QVector<int>& times, int delta)
{
    if (times.isEmpty() || delta == 0 || TimelineOps::isLocked(m_project->timeline(), clipId)) return;
    m_project->edit(T("Keyframes verschieben"), [&](Timeline& tl) {
        if (Clip* c = TimelineOps::findClip(tl, clipId)) Keys::move(*c, times, delta);
    });
    QSet<int> moved;
    for (int t : times) moved.insert(t + delta);
    m_selection->setKeyframes(clipId, moved);
}

void Editor::removeKeyframes(int clipId, const QVector<int>& times, const QVector<AnimParam>& params)
{
    if (times.isEmpty() || TimelineOps::isLocked(m_project->timeline(), clipId)) return;
    m_project->edit(T("Keyframes löschen"), [&](Timeline& tl) {
        if (Clip* c = TimelineOps::findClip(tl, clipId)) Keys::removeAt(*c, times, params);
    });
    m_selection->setKeyframes(0, {});
}

void Editor::deleteSelection()
{
    if (m_selection->keyClip()) { // ausgewählte Keyframe-Rauten gehen vor (wie DaVinci)
        const int param = m_selection->keyParam();
        removeKeyframes(m_selection->keyClip(), m_selection->keyTimes().values().toVector(),
                        param < 0 ? QVector<AnimParam>{} : QVector<AnimParam>{AnimParam(param)});
        return;
    }
    if (const TransitionKey t = m_selection->transition(); !t.isNull()) {
        removeTransition(t.leftId, t.rightId);
        m_selection->clear();
        return;
    }
    const QVector<int> cues = selectedSubtitles();
    const QVector<int> ids = editable(clipIdsOf(m_selection->ids()));
    if (ids.isEmpty() && cues.isEmpty()) return;
    // Löschen ohne Ripple: es bleibt eine Lücke, nichts rutscht nach
    m_project->edit(T("Löschen"), [&](Timeline& tl) {
        removeCues(tl, cues);
        TimelineOps::detachTransitions(tl, ids);
        for (int id : ids) TimelineOps::removeClip(tl, id);
    });
    m_selection->clear();
}
