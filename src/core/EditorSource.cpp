// SPDX-License-Identifier: AGPL-3.0-only
// Editor: source edits (source in/out, insert, overwrite, replace, place on top, append, fit to fill).
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
#include "core/Voiceover.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <utility>

void Editor::setPreviewSource(const MediaInfo& info)
{
    m_preview = info;
    emit previewSourceChanged();
}

const MediaInfo* Editor::sourceMedia(const QString& path) const
{
    if (const MediaInfo* m = m_project->mediaInfo(path)) return m;
    return !path.isEmpty() && path == m_preview.path ? &m_preview : nullptr;
}

bool Editor::importPreviewSource(const QString& path, int bin)
{
    if (m_project->mediaInfo(path)) return true;
    if (path.isEmpty() || path != m_preview.path || m_preview.length <= 0 || (!m_preview.hasVideo && !m_preview.hasAudio))
        return false;
    MediaInfo info = std::exchange(m_preview, {}); // before addMedia: its mediaChanged finds no preview to adopt
    info.bin = bin;
    m_project->addMedia(info);
    emit previewSourceChanged();
    return m_project->mediaInfo(path) != nullptr;
}

void Editor::adoptPreviewSource()
{
    if (m_preview.path.isEmpty() || !m_project->mediaInfo(m_preview.path)) return;
    const MediaInfo preview = std::exchange(m_preview, {});
    const MediaInfo* m = m_project->mediaInfo(preview.path);
    if (m->markIn < 0 && m->markOut < 0 && (preview.markIn >= 0 || preview.markOut >= 0))
        m_project->setMediaMarks(preview.path, preview.markIn, preview.markOut);
    if (m->markers.isEmpty() && !preview.markers.isEmpty()) m_project->setMediaMarkers(preview.path, preview.markers);
    emit previewSourceChanged();
}

void Editor::setSourceMarks(const QString& path, int markIn, int markOut)
{
    if (m_project->mediaInfo(path)) {
        m_project->setMediaMarks(path, markIn, markOut);
    } else if (!path.isEmpty() && path == m_preview.path && (m_preview.markIn != markIn || m_preview.markOut != markOut)) {
        m_preview.markIn = markIn;
        m_preview.markOut = markOut;
        emit previewSourceChanged();
    }
}

void Editor::setSourceMarkIn(const QString& path, int frame)
{
    const MediaInfo* m = sourceMedia(path);
    if (!m) return;
    const int out = frame >= 0 && m->markOut >= 0 && m->markOut < frame ? -1 : m->markOut;
    setSourceMarks(path, frame, out);
}

void Editor::toggleSourceMarker(const QString& path, int frame)
{
    const MediaInfo* m = sourceMedia(path);
    if (!m || frame < 0 || (m->length > 0 && frame >= m->length)) return;
    QVector<int> markers = m->markers;
    if (markers.contains(frame)) markers.removeAll(frame);
    else markers.insert(std::lower_bound(markers.begin(), markers.end(), frame) - markers.begin(), frame);
    if (m_project->mediaInfo(path)) {
        m_project->setMediaMarkers(path, markers);
    } else {
        m_preview.markers = markers;
        emit previewSourceChanged();
    }
}

void Editor::setSourceMarkOut(const QString& path, int frame)
{
    const MediaInfo* m = sourceMedia(path);
    if (!m) return;
    const int in = frame >= 0 && m->markIn > frame ? -1 : m->markIn;
    setSourceMarks(path, in, frame);
}

void Editor::clearSourceMarks(const QString& path)
{
    setSourceMarks(path, -1, -1);
}

int Editor::streamTrackStart(const Timeline& tl, const MediaInfo& m, int aTrack, int start, int length) const
{
    const QVector<StreamPlacement> placed = m_streamPresets.placements(m);
    const int n = int(placed.size());
    if (n < 2) return aTrack;
    const auto same = [](const QString& a, const QString& b) {
        return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0;
    };
    const auto name = [&](int i) { return i < tl.audio.size() ? tl.audio[i].name.trimmed() : QString(); };
    // A track from j on is named after another stream of the file than the one it would get
    const auto crossed = [&](int j) {
        for (int k = 0; k < n; ++k) {
            const QString t = name(j + k);
            if (t.isEmpty() || same(t, placed[k].trackLabel)) continue;
            for (const StreamPlacement& o : placed)
                if (same(t, o.trackLabel)) return true;
        }
        return false;
    };
    if (aTrack < 0 || !crossed(aTrack)) return aTrack;
    const auto free = [&](int j) { // unlocked and empty over the range
        for (int k = 0; k < n && j + k < tl.audio.size(); ++k) {
            const Track& t = tl.audio[j + k];
            if (t.locked) return false;
            for (const Clip& c : t.clips)
                if (c.start < start + length && c.end() > start) return false;
        }
        return true;
    };
    const auto named = [&](int j) {
        for (int k = 0; k < n; ++k)
            if (!same(name(j + k), placed[k].trackLabel)) return false;
        return true;
    };
    for (int j = 0; j < tl.audio.size(); ++j)
        if (named(j) && free(j)) return j;
    int j = aTrack + 1;
    while (crossed(j) || !free(j)) ++j; // ends below the last track at the latest
    return j;
}

QVector<int> Editor::streamTracksInOrder(const Timeline& tl, const MediaInfo& m, int aTrack, int start,
                                        int length) const
{
    const int n = int(m_streamPresets.placements(m).size());
    const int a0 = streamTrackStart(tl, m, aTrack, start, length);
    QVector<int> out(n, -1);
    for (int k = 0; k < n; ++k)
        if ((k == 0 && a0 == aTrack) || !(a0 + k < tl.audio.size() && tl.audio[a0 + k].locked)) out[k] = a0 + k;
    return out;
}

QVector<int> Editor::streamTracks(const Timeline& tl, const MediaInfo& m, int aTrack, int start, int length,
                                  int vTrack) const
{
    const QVector<StreamPlacement> placed = m_streamPresets.placements(m);
    const int n = int(placed.size());
    const auto locked = [&](int j) { return j >= 0 && j < tl.audio.size() && tl.audio[j].locked; };
    const auto overlaps = [&](const Clip& c) { return c.start < start + length && c.end() > start; };
    const auto emptyIn = [&](const QVector<Track>& tracks, int j) {
        if (j < 0 || j >= tracks.size()) return true;
        return std::none_of(tracks[j].clips.begin(), tracks[j].clips.end(), overlaps);
    };
    const auto empty = [&](int j) { return emptyIn(tl.audio, j); };
    // One after another from streamTrackStart (the target itself is checked by the caller): every edit over
    // material (overwrite, replace, fit to fill, ripple overwrite, a drop whose picture lands on clips) and every
    // single stream
    const int a0 = streamTrackStart(tl, m, aTrack, start, length);
    QVector<int> out = streamTracksInOrder(tl, m, aTrack, start, length);
    if (n < 2) return out;
    const bool overMaterial = m.hasVideo && vTrack >= 0 ? !emptyIn(tl.video, vTrack) : !empty(aTrack);
    if (overMaterial) return out;

    // Into empty space. The tracks of each stream of the file: holding a clip of it, or named after it
    const auto same = [](const QString& a, const QString& b) {
        return a.trimmed().compare(b.trimmed(), Qt::CaseInsensitive) == 0;
    };
    QVector<QVector<int>> homes(n);
    for (int j = 0; j < tl.audio.size(); ++j) {
        const Track& t = tl.audio[j];
        for (int k = 0; k < n; ++k) {
            const bool named = !placed[k].trackLabel.trimmed().isEmpty() && same(t.name, placed[k].trackLabel);
            const bool holds = std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) {
                return c.mediaPath == m.path && c.audioStream == placed[k].stream;
            });
            if (named || holds) homes[k] << j;
        }
    }
    const auto someonesHome = [&](int j, int except) {
        for (int k = 0; k < n; ++k)
            if (k != except && homes[k].contains(j)) return true;
        return false;
    };
    const auto hasFreeHome = [&](int k) {
        return std::any_of(homes[k].begin(), homes[k].end(), [&](int j) { return !locked(j) && empty(j); });
    };
    // The layout above fits when it lands on nothing and no stream passes by a free track of its own
    bool fits = true;
    for (int k = 0; k < n && fits; ++k) {
        if (out[k] < 0) continue;
        if (!empty(out[k]) || someonesHome(out[k], k) || (!homes[k].contains(out[k]) && hasFreeHome(k))) fits = false;
    }
    if (fits) return out;
    // Each stream on a free track of its own, else on a free unnamed track (from the target on), else a new one at
    // the bottom; never onto material
    QVector<int> taken;
    for (int k = 0; k < n; ++k) {
        out[k] = -1;
        for (int j : homes[k])
            if (!taken.contains(j) && !locked(j) && empty(j)) {
                out[k] = j;
                taken << j;
                break;
            }
    }
    const auto usable = [&](int j) {
        if (taken.contains(j)) return false;
        if (j >= tl.audio.size()) return true;
        return !locked(j) && tl.audio[j].name.trimmed().isEmpty() && empty(j) && !someonesHome(j, -1);
    };
    for (int k = 0; k < n; ++k) {
        if (out[k] >= 0) continue;
        int j = std::max(0, a0);
        while (j < tl.audio.size() && !usable(j)) ++j;
        if (j >= tl.audio.size()) { // a new track: right below the last one or the new ones before it
            j = int(tl.audio.size());
            while (taken.contains(j)) ++j;
        }
        out[k] = j;
        taken << j;
    }
    return out;
}

void Editor::placeSource(Timeline& tl, const MediaInfo& m, int sIn, int len, int start, int vTrack, int aTrack,
                         double speed, const QVector<int>* given)
{
    Project* p = m_project;
    auto newId = [p] { return p->newClipId(); };
    Clip c;
    c.mediaPath = m.path;
    c.start = start;
    c.in = sIn;
    c.out = sIn + len - 1;
    c.speed = speed;
    // Targets: picture, then one track per placed audio stream (import presets, streamTracks)
    struct Target { TrackKind kind; int idx; StreamPlacement stream; };
    QVector<Target> targets;
    if (m.hasVideo) targets << Target{TrackKind::Video, vTrack, {}};
    if (m.hasAudio) {
        const QVector<StreamPlacement> placed = m_streamPresets.placements(m);
        const QVector<int> tracks = given ? *given : streamTracks(tl, m, aTrack, start, len, m.hasVideo ? vTrack : -1);
        for (int k = 0; k < placed.size(); ++k)
            if (tracks.value(k, -1) >= 0) targets << Target{TrackKind::Audio, tracks[k], placed[k]};
    }
    c.linkId = targets.size() > 1 ? p->newLinkId() : 0;
    QVector<int> sound; // the new audio clips
    for (const Target& tg : targets) {
        TimelineOps::ensureTracks(tl, tg.kind, tg.idx + 1);
        Track& t = tl.tracks(tg.kind)[tg.idx];
        Clip x = c;
        x.id = newId();
        if (tg.kind == TrackKind::Video) x.pictureAspect = m.pictureAspect; // the file's shape set in the Media Pool
        if (tg.kind == TrackKind::Audio) {
            x.audioStream = tg.stream.stream;
            x.enabled = tg.stream.enabled;
            x.volumeDb = tg.stream.volumeDb;
            if (t.name.isEmpty()) t.name = tg.stream.trackLabel;
            sound << x.id;
        }
        TimelineOps::clearDissolveAt(t, x.start);
        TimelineOps::clearDissolveAt(t, x.end());
        TimelineOps::placeClip(t, x, newId);
    }
    muteRepeatedSound(tl, sound);
}

int Editor::placeRecording(const QString& path, int track, double fileStart, int from, int* placed)
{
    const MediaInfo* m = m_project->mediaInfo(path);
    if (!m || !m->hasAudio || m->length <= 0) return 0;
    const std::optional<Voiceover::Placement> at = Voiceover::place(fileStart, std::max(0, from), m->length);
    if (!at) return 0;
    // The clip's range (a subframe reaches into one more frame)
    const int end = at->start + (m->length - at->in) + (at->subframe > 0 ? 1 : 0);
    const int target = Voiceover::recordTrack(m_project->timeline(), track, at->start, end);
    int id = 0;
    m_project->edit(T("Voiceover aufnehmen"), [&](Timeline& tl) {
        const bool fresh = target >= tl.audio.size();
        TimelineOps::ensureTracks(tl, TrackKind::Audio, target + 1);
        Track& t = tl.audio[target];
        if (fresh) t.name = T("Voiceover");
        Clip c;
        c.id = id = m_project->newClipId();
        c.mediaPath = m->path;
        c.start = at->start;
        c.subframe = at->subframe;
        c.in = at->in;
        c.out = m->length - 1;
        TimelineOps::clearDissolveAt(t, c.start);
        TimelineOps::clearDissolveAt(t, c.end());
        TimelineOps::placeClip(t, c, [this] { return m_project->newClipId(); });
    });
    if (id) m_selection->set({id});
    if (placed) *placed = id ? target : -1;
    return id;
}

void Editor::muteRepeatedSound(Timeline& tl, const QVector<int>& ids)
{
    const QVector<int> repeated = TimelineOps::repeatedSound(tl, ids);
    if (repeated.isEmpty()) return;
    TimelineOps::changeClips(
        tl, [&](const Clip& c) { return repeated.contains(c.id); }, [](Clip& c) { c.enabled = false; });
}

int Editor::sourceEdit(SourceEditMode mode, const QString& path, int srcPos, int playhead)
{
    using M = SourceEditMode;
    using namespace TimelineOps;
    Project* p = m_project;
    importPreviewSource(path, m_preview.bin); // looked at in the Media Storage: into the Media Pool first
    const MediaInfo* info = p->mediaInfo(path);
    if (!info || info->length <= 0 || (!info->hasVideo && !info->hasAudio)) return -1;
    const MediaInfo m = *info;
    const Timeline& cur = p->timeline();

    // Source range: source in/out, if one is missing -> clip start or end (like DaVinci)
    const int last = m.length - 1;
    int sIn = std::clamp(m.markIn >= 0 ? m.markIn : 0, 0, last);
    const int sOut = std::clamp(m.markOut >= 0 ? m.markOut : last, 0, last);
    if (sOut < sIn) return -1;
    int len = sOut - sIn + 1;
    int start = std::max(0, playhead);
    int vTrack = m_targetVideo, aTrack = m_targetAudio;
    bool usedTimelineMarks = false;
    QVector<TrackRef> rippleTracks; // Insert / Ripple Overwrite
    int rippleFrom = 0, rippleDelta = 0;
    int replaceEnd = 0;             // Ripple Overwrite: end of the replaced clip
    double speed = 1.0;             // Fit to Fill

    // Clip under the playhead on the target track (video, for audio-only audio) including the tracks of its partners
    auto clipUnderPlayhead = [&]() -> const Clip* {
        const TrackRef ref{m.hasVideo ? TrackKind::Video : TrackKind::Audio, m.hasVideo ? m_targetVideo : m_targetAudio};
        if (ref.index >= cur.tracks(ref.kind).size()) return nullptr;
        for (const Clip& c : cur.track(ref).clips)
            if (c.start <= playhead && playhead < c.end()) return &c;
        return nullptr;
    };
    auto partnerTracks = [&](const Clip& c) {
        QVector<TrackRef> refs;
        int firstAudio = INT_MAX; // several audio streams: stream 0 goes on the topmost audio track of the group
        for (int id : linkedGroup(cur, c.id)) {
            TrackRef r;
            if (!findClip(cur, id, &r) || refs.contains(r)) continue;
            refs << r;
            if (r.kind == TrackKind::Video) vTrack = r.index;
            else firstAudio = std::min(firstAudio, r.index);
        }
        if (firstAudio != INT_MAX) aTrack = firstAudio;
        return refs;
    };

    switch (mode) {
    case M::Insert:
    case M::Overwrite:
    case M::PlaceOnTop: {
        // Timeline in/out: in = target, in+out limits the length, out only = backwards from out
        const int tIn = cur.markIn, tOut = cur.markOut;
        if (tIn >= 0 && tOut >= tIn) {
            start = tIn;
            len = std::min(len, tOut - tIn + 1);
        } else if (tIn >= 0) {
            start = tIn;
        } else if (tOut >= 0) {
            start = tOut + 1 - len;
            if (start < 0) { // shorten at the front, the end stays at the out
                sIn -= start;
                len += start;
                start = 0;
            }
        }
        usedTimelineMarks = tIn >= 0 || tOut >= 0;
        if (mode == M::PlaceOnTop) {
            // First track above all clips in the range (missing track is created)
            auto freeTrack = [&](TrackKind kind) {
                int idx = 0;
                const auto& tracks = cur.tracks(kind);
                for (int i = 0; i < tracks.size(); ++i)
                    for (const Clip& c : tracks[i].clips)
                        if (c.start < start + len && c.end() > start) idx = i + 1;
                while (idx < tracks.size() && tracks[idx].locked) ++idx; // skip locked track
                return idx;
            };
            vTrack = freeTrack(TrackKind::Video);
            aTrack = freeTrack(TrackKind::Audio);
        }
        if (mode == M::Insert) {
            // Like DaVinci: all unlocked tracks move along (stay in sync), the target tracks anyway (the streams'
            // tracks are added below)
            if (m.hasVideo) rippleTracks << TrackRef{TrackKind::Video, vTrack};
            for (TrackKind kind : {TrackKind::Video, TrackKind::Audio})
                for (int i = 0; i < cur.tracks(kind).size(); ++i)
                    if (!cur.tracks(kind)[i].locked && !rippleTracks.contains(TrackRef{kind, i})) rippleTracks << TrackRef{kind, i};
        }
        break;
    }
    case M::AppendAtEnd:
        start = endFrame(cur);
        break;
    case M::FitToFill: {
        // 4-point edit like DaVinci: needs timeline in and out; speed = source length / target length
        const int tIn = cur.markIn, tOut = cur.markOut;
        if (tIn < 0 || tOut < tIn) return -1;
        const int target = tOut - tIn + 1;
        speed = double(len) / target;
        if (std::abs(speed - 1.0) < 1e-9) speed = 1.0;
        // in/out count in the converted material (Clip::speed)
        const int avail = std::max(1, int(m.length / speed));
        if (avail < target) return -1;
        sIn = std::clamp(RetimeMap::materialForPicture(sIn, speed), 0, avail - target);
        len = target;
        start = tIn;
        usedTimelineMarks = true;
        break;
    }
    case M::Replace: {
        // Length and position stay; source in (otherwise source playhead) coincides with the timeline playhead
        const Clip* c = clipUnderPlayhead();
        if (!c) return -1;
        partnerTracks(*c);
        const int anchor = m.markIn >= 0 ? m.markIn : srcPos;
        sIn = anchor - (playhead - c->start);
        len = c->length();
        if (sIn < 0 || sIn + len - 1 > last) return -1; // too little material
        start = c->start;
        break;
    }
    case M::RippleOverwrite: {
        const Clip* c = clipUnderPlayhead();
        if (!c) return -1;
        rippleTracks = partnerTracks(*c);
        start = c->start;
        replaceEnd = c->end();
        rippleFrom = c->end();
        rippleDelta = len - c->length();
        break;
    }
    }
    if (len <= 0) return -1;
    // Locked target track: this part is dropped (as when dropping), locked tracks never move
    MediaInfo m2 = m;
    if (m2.hasVideo && isTrackLocked(TrackRef{TrackKind::Video, vTrack})) m2.hasVideo = false;
    if (m2.hasAudio && isTrackLocked(TrackRef{TrackKind::Audio, aTrack})) m2.hasAudio = false;
    if (!m2.hasVideo && !m2.hasAudio) return -1;
    rippleTracks.erase(std::remove_if(rippleTracks.begin(), rippleTracks.end(), [&](const TrackRef& r) { return isTrackLocked(r); }),
                       rippleTracks.end());
    if (m2.hasVideo && !rippleTracks.contains(TrackRef{TrackKind::Video, vTrack}) && mode == M::RippleOverwrite)
        rippleTracks << TrackRef{TrackKind::Video, vTrack};
    // The streams' tracks (streamTracks) on the timeline as the placement will find it: after an insert's gap (every
    // unlocked track moved on), else as it is; a ripple overwrite keeps the target tracks one after another
    QVector<int> streams;
    if (m2.hasAudio) {
        const int pic = m2.hasVideo ? vTrack : -1;
        if (mode == M::Insert) {
            Timeline gap = cur;
            for (TrackKind kind : {TrackKind::Video, TrackKind::Audio})
                for (Track& t : gap.tracks(kind))
                    if (!t.locked)
                        for (Clip& c : t.clips)
                            if (c.start >= start) c.start += len;
                            else if (c.end() > start) c.out = c.in + (start - c.start) - 1; // cut there (its rest moves)
            streams = streamTracks(gap, m, aTrack, start, len, pic);
        } else if (mode == M::RippleOverwrite) {
            streams = QVector<int>(m_streamPresets.placements(m).size(), -1); // made after the ripple (below)
        } else {
            streams = streamTracks(cur, m, aTrack, start, len, pic);
        }
        if (mode == M::Insert)
            for (int a : streams)
                if (const TrackRef r{TrackKind::Audio, a}; a >= 0 && !rippleTracks.contains(r) && !isTrackLocked(r))
                    rippleTracks << r;
        if (mode == M::RippleOverwrite) // over the replaced clip: the target tracks one after another
            for (int k = 0; k < streams.size(); ++k)
                if (const TrackRef r{TrackKind::Audio, aTrack + k}; !rippleTracks.contains(r) && !isTrackLocked(r))
                    rippleTracks << r;
    }

    QString text;
    switch (mode) {
    case M::Insert: text = T("Clip einfügen: %1"); break;
    case M::Overwrite: text = T("Clip überschreiben: %1"); break;
    case M::Replace: text = T("Clip ersetzen: %1"); break;
    case M::PlaceOnTop: text = T("Oben platzieren: %1"); break;
    case M::RippleOverwrite: text = T("Überschreiben mit Nachrücken: %1"); break;
    case M::AppendAtEnd: text = T("Am Ende anhängen: %1"); break;
    case M::FitToFill: text = T("Einpassen: %1"); break;
    }
    p->edit(text.arg(m.name), [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        for (const TrackRef& r : rippleTracks) ensureTracks(tl, r.kind, r.index + 1);
        if (mode == M::Insert) {
            insertGap(tl, rippleTracks, start, len, newId, [p] { return p->newLinkId(); });
            insertSubtitleGap(tl, start, len, newId); // subtitles stay in sync (locked tracks not)
        }
        if (mode == M::RippleOverwrite) {
            for (const TrackRef& r : rippleTracks) {
                clearRange(tl.track(r), start, replaceEnd, newId);
                shiftFrom(tl.track(r), rippleFrom, rippleDelta);
            }
            // other unlocked tracks stay in sync (stay put if they would overwrite)
            TimelineOps::rippleTracks(tl, {{rippleFrom, rippleDelta}}, rippleTracks);
        }
        // Ripple overwrite: the tracks one after another, on the timeline as the ripple left it (as always)
        if (mode == M::RippleOverwrite && m2.hasAudio) streams = streamTracksInOrder(tl, m2, aTrack, start, len);
        placeSource(tl, m2, sIn, len, start, vTrack, aTrack, speed, &streams);
        if (usedTimelineMarks) tl.markIn = tl.markOut = -1; // like DaVinci: used marks are consumed
    });
    return mode == M::Replace ? playhead : start + len;
}

int Editor::placeSourceRange(const QString& path, int in, int out, int frame, int track, double speed, bool audioOnly)
{
    importPreviewSource(path, m_preview.bin); // looked at in the Media Storage: into the Media Pool first
    const MediaInfo* info = m_project->mediaInfo(path);
    if (!info || info->length <= 0) return 0;
    const MediaInfo m = *info;
    in = std::clamp(in, 0, m.length - 1);
    out = m.isImage ? std::max(out, in) : std::clamp(out, in, m.length - 1); // a still takes any length (as trimmed)
    MediaInfo m2 = m; // locked track: this part is dropped
    track = std::max(0, track);
    if (m2.hasVideo && (audioOnly || isTrackLocked(TrackRef{TrackKind::Video, track}))) m2.hasVideo = false;
    if (m2.hasAudio && isTrackLocked(TrackRef{TrackKind::Audio, track})) m2.hasAudio = false;
    if (!m2.hasVideo && !m2.hasAudio) return 0;
    // Retimed: in and length in material frames (file frame / speed, see Clip::speed); a still has no speed
    if (m.isImage || speed <= 0) speed = 1.0;
    const int sIn = RetimeMap::materialForPicture(in, speed);
    const int len = std::max(1, RetimeMap::materialForPicture(out + 1, speed) - sIn); // no picture past `out`
    m_project->edit(T("Clip überschreiben: %1").arg(m.name), [&](Timeline& tl) {
        placeSource(tl, m2, sIn, len, std::max(0, frame), track, track, speed);
    });
    return len;
}
