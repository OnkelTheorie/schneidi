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
#include "core/TranscriptCut.h"

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
    : QObject(parent), m_project(project), m_selection(selection)
{
    connect(m_project, &Project::mediaChanged, this, &Editor::adoptPreviewSource);
}

QVector<int> Editor::withLinked(const QVector<int>& ids) const
{
    if (!m_linkedSelection) return editable(ids);
    QVector<int> out;
    for (int id : editable(ids)) // a locked clip does not take its partners along either
        for (int g : TimelineOps::linkedGroup(m_project->timeline(), id))
            if (!out.contains(g)) out << g;
    return editable(out); // partner on a locked track stays in place (like DaVinci)
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
    // a locked target track gets nothing (like DaVinci); missing tracks are created unlocked
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
            // One clip per placed audio stream on the next track (like DaVinci), from a0 on (streamTrackStart);
            // locked tracks get nothing
            const QVector<StreamPlacement> placed = m_streamPresets.placements(*info);
            const int a0 = streamTrackStart(tl, *info, idx, start, info->length);
            QVector<int> streams; // index into placed = track offset
            for (int k = 0; k < placed.size(); ++k)
                if (!isTrackLocked({TrackKind::Audio, a0 + k})) streams << k;
            c.linkId = int(video) + streams.size() > 1 ? p->newLinkId() : 0;
            if (video) {
                TimelineOps::ensureTracks(tl, TrackKind::Video, idx + 1);
                Clip v = c;
                v.id = newId();
                TimelineOps::placeClip(tl.video[idx], v, newId);
            }
            QVector<int> sound;
            for (int k : streams) {
                TimelineOps::ensureTracks(tl, TrackKind::Audio, a0 + k + 1);
                Clip a = c;
                a.id = newId();
                a.audioStream = placed[k].stream;
                a.enabled = placed[k].enabled;
                a.volumeDb = placed[k].volumeDb;
                if (tl.audio[a0 + k].name.isEmpty()) tl.audio[a0 + k].name = placed[k].trackLabel;
                TimelineOps::placeClip(tl.audio[a0 + k], a, newId);
                sound << a.id;
            }
            muteRepeatedSound(tl, sound);
            start += info->length;
        }
    });
}

void Editor::addTitle(int frame, int track, int length)
{
    Project* p = m_project;
    const Timeline& cur = p->timeline();
    Clip c;
    c.kind = ClipKind::Title;
    c.title.text = T("Titel");
    c.start = std::max(0, frame);
    c.in = 0;
    // Placed with its final length: placing 5 s first and shortening afterwards overwrote what followed on the track
    c.out = (length > 0 ? length : 5 * p->fps()) - 1;
    if (track < 0) {
        track = 0;
        for (int i = 0; i < cur.video.size(); ++i)
            for (const Clip& x : cur.video[i].clips)
                if (x.start < c.end() && x.end() > c.start) track = i + 1;
        while (isTrackLocked({TrackKind::Video, track})) ++track; // skip locked track
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

void Editor::setTitleLength(int clipId, int length)
{
    TrackRef ref;
    const Clip* cur = TimelineOps::findClip(m_project->timeline(), clipId, &ref);
    if (!cur || !cur->isTitle() || length <= 0 || cur->length() == length || isTrackLocked(ref)) return;
    Clip c = *cur;
    c.out = c.in + length - 1;
    Project* p = m_project;
    p->edit(T("Titellänge"), [&](Timeline& tl) {
        Track& t = tl.track(ref);
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip& x) { return x.id == clipId; }),
                      t.clips.end());
        TimelineOps::placeClip(t, c, [p] { return p->newClipId(); }); // a longer title overwrites its new range only
    });
}

void Editor::moveClips(const QVector<int>& idsIn, int deltaFrames, TrackKind kind, int trackDelta)
{
    const QVector<int> ids = editable(idsIn);
    if (ids.isEmpty() || (deltaFrames == 0 && trackDelta == 0)) return;
    // never move onto a locked track
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
    // Target per clip; whole frames via moveClips (overwriting as usual), then the fine position
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

int Editor::duck(const QVector<int>& ids, const Ducking::Ranges& ranges, const Ducking::Params& params)
{
    const Timeline& cur = m_project->timeline();
    QVector<int> music;
    for (int id : ids) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(cur, id, &ref);
        if (c && ref.kind == TrackKind::Audio && !TimelineOps::isLocked(cur, id) && !music.contains(id)) music << id;
    }
    if (music.isEmpty() || ranges.isEmpty()) return 0;
    int ducked = 0;
    m_project->edit(T("Ducking"), [&](Timeline& tl) {
        for (int id : music)
            if (Clip* c = TimelineOps::findClip(tl, id); c && Ducking::apply(*c, ranges, params)) ++ducked;
    });
    return ducked;
}

void Editor::normalizeAudio(const QHash<int, double>& peakDb, double targetDb, bool relative,
                            std::optional<double> relativeRef)
{
    const Timeline& cur = m_project->timeline();
    QHash<int, double> gain; // new clip volume (dB)
    double loudest = -1e9;
    for (auto it = peakDb.begin(); it != peakDb.end(); ++it) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(cur, it.key(), &ref);
        if (!c || ref.kind != TrackKind::Audio || c->isTitle() || TimelineOps::isLocked(cur, c->id)) continue;
        if (it.value() <= -100.0) continue; // silence: nothing to raise
        gain[c->id] = targetDb - it.value();
        loudest = std::max(loudest, it.value());
    }
    if (gain.isEmpty()) return;
    if (relative && relativeRef && *relativeRef <= -100.0) return; // measured jointly: silence
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
    if (const SubtitleCue* c = Subtitles::find(p->timeline(), clipId, &st)) { // subtitle: only this entry
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
    // Like DaVinci: with a selection only the selected clips, otherwise all under the playhead.
    // Optional: all clips on the tracks of the selection (playhead is already on the neighboring clip).
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
              ids.end()); // selected subtitles are also part of the selection
    // Subtitles like clips: without a selection the entry under the playhead on every unlocked subtitle track,
    // otherwise the selected entries (or all subtitle tracks with a selected entry)
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
    // After shortening, the other fade can be longer than the clip -> upper limit never negative
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
    deleteRanges({{from, to}}, ripple, ripple ? T("Bereich löschen mit Ripple") : T("Bereich löschen"));
}

void Editor::deleteRanges(const QVector<QPair<int, int>>& input, bool ripple, const QString& text)
{
    const QVector<QPair<int, int>> ranges = TranscriptCut::mergeRanges(input);
    if (ranges.isEmpty()) return;
    Project* p = m_project;
    p->edit(text, [&](Timeline& tl) {
        // From the last range back: earlier ranges keep their positions while later ones ripple
        for (auto r = ranges.crbegin(); r != ranges.crend(); ++r) {
            const int from = std::max(0, r->first), to = r->second;
            if (to <= from) continue;
            for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
                for (Track& t : tl.tracks(k)) {
                    if (t.locked) continue;
                    TimelineOps::clearDissolveAt(t, from);
                    TimelineOps::clearDissolveAt(t, to);
                    TimelineOps::clearRange(t, from, to, [p] { return p->newClipId(); });
                }
            TimelineOps::resolvePendingLinks(tl, [p] { return p->newLinkId(); });
            if (ripple) TimelineOps::rippleTracks(tl, {{to, from - to}}, {});
        }
    });
    m_selection->clear();
}

void Editor::rippleDeleteSelection()
{
    const QVector<int> cues = selectedSubtitles();
    const QVector<int> ids = editable(clipIdsOf(m_selection->ids()));
    if (ids.isEmpty() && cues.isEmpty()) return;
    m_project->edit(T("Löschen mit Ripple"), [&](Timeline& tl) {
        removeCues(tl, cues); // subtitles: without ripple (gap stays)
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
            if (!t.locked) // locked tracks cannot be selected
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
            // the clip under the playhead belongs to it (like DaVinci)
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
    frames = std::max(frames, -minStart); // not before frame 0
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
    // Like DaVinci: if one is active, all get deactivated
    bool anyEnabled = false;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) anyEnabled |= c->enabled;
    modifyClips(ids, anyEnabled ? T("Clip deaktivieren") : T("Clip aktivieren"),
                [anyEnabled](Clip& c) { c.enabled = !anyEnabled; });
}

namespace {
// Video clip with media (titles and audio clips are not cached)
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
    // A transition to a clip that is not copied along does not belong to it (otherwise the copy fades out over black)
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
    // Only delete what also goes to the clipboard (clips): selected subtitles/keyframes/transitions would
    // otherwise be lost, and pasting would bring the old clipboard
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
    // do not paste onto locked tracks
    QVector<ClipboardItem> items;
    for (const auto& it : m_clipboard)
        if (!isTrackLocked(it.ref)
            && (!it.clip.isNested() || m_project->canNest(it.clip.sequenceId, m_project->currentSequence())))
            items << it; // never a compound clip inside itself (loop)
    if (items.isEmpty()) return;
    Project* p = m_project;
    QSet<int> pasted;
    p->edit(T("Einfügen"), [&](Timeline& tl) {
        QHash<int, int> linkMap; // copy gets its own link
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
    if (m_selection->keyClip()) { // selected keyframe diamonds take precedence (like DaVinci)
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
    // Delete without ripple: a gap remains, nothing moves up
    m_project->edit(T("Löschen"), [&](Timeline& tl) {
        removeCues(tl, cues);
        TimelineOps::detachTransitions(tl, ids);
        for (int id : ids) TimelineOps::removeClip(tl, id);
    });
    m_selection->clear();
}
