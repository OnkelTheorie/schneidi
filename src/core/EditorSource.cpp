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

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <algorithm>

void Editor::setSourceMarkIn(const QString& path, int frame)
{
    const MediaInfo* m = m_project->mediaInfo(path);
    if (!m) return;
    const int out = frame >= 0 && m->markOut >= 0 && m->markOut < frame ? -1 : m->markOut;
    m_project->setMediaMarks(path, frame, out);
}

void Editor::setSourceMarkOut(const QString& path, int frame)
{
    const MediaInfo* m = m_project->mediaInfo(path);
    if (!m) return;
    const int in = frame >= 0 && m->markIn > frame ? -1 : m->markIn;
    m_project->setMediaMarks(path, in, frame);
}

void Editor::clearSourceMarks(const QString& path)
{
    m_project->setMediaMarks(path, -1, -1);
}

void Editor::placeSource(Timeline& tl, const MediaInfo& m, int sIn, int len, int start, int vTrack, int aTrack,
                         double speed)
{
    Project* p = m_project;
    auto newId = [p] { return p->newClipId(); };
    Clip c;
    c.mediaPath = m.path;
    c.start = start;
    c.in = sIn;
    c.out = sIn + len - 1;
    c.speed = speed;
    // Ziele: Bild, dann pro Ton-Stream eine Spur ab aTrack (weitere Streams auf gesperrten Spuren entfallen)
    struct Target { TrackKind kind; int idx; int stream; };
    QVector<Target> targets;
    if (m.hasVideo) targets << Target{TrackKind::Video, vTrack, 0};
    if (m.hasAudio)
        for (int k = 0; k < m.audioStreamCount(); ++k)
            if (k == 0 || !isTrackLocked({TrackKind::Audio, aTrack + k})) targets << Target{TrackKind::Audio, aTrack + k, k};
    c.linkId = targets.size() > 1 ? p->newLinkId() : 0;
    for (const Target& tg : targets) {
        TimelineOps::ensureTracks(tl, tg.kind, tg.idx + 1);
        Track& t = tl.tracks(tg.kind)[tg.idx];
        Clip x = c;
        x.id = newId();
        x.audioStream = tg.stream;
        TimelineOps::clearDissolveAt(t, x.start);
        TimelineOps::clearDissolveAt(t, x.end());
        TimelineOps::placeClip(t, x, newId);
    }
}

int Editor::sourceEdit(SourceEditMode mode, const QString& path, int srcPos, int playhead)
{
    using M = SourceEditMode;
    using namespace TimelineOps;
    Project* p = m_project;
    const MediaInfo* info = p->mediaInfo(path);
    if (!info || info->length <= 0 || (!info->hasVideo && !info->hasAudio)) return -1;
    const MediaInfo m = *info;
    const Timeline& cur = p->timeline();

    // Quellbereich: Quell-In/Out, fehlt einer -> Clipanfang bzw. -ende (wie DaVinci)
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
    int replaceEnd = 0;             // Ripple Overwrite: Ende des ersetzten Clips
    double speed = 1.0;             // Fit to Fill

    // Clip unter dem Playhead auf der Zielspur (Video, bei reinem Ton Audio) samt Spuren seiner Partner
    auto clipUnderPlayhead = [&]() -> const Clip* {
        const TrackRef ref{m.hasVideo ? TrackKind::Video : TrackKind::Audio, m.hasVideo ? m_targetVideo : m_targetAudio};
        if (ref.index >= cur.tracks(ref.kind).size()) return nullptr;
        for (const Clip& c : cur.track(ref).clips)
            if (c.start <= playhead && playhead < c.end()) return &c;
        return nullptr;
    };
    auto partnerTracks = [&](const Clip& c) {
        QVector<TrackRef> refs;
        int firstAudio = INT_MAX; // mehrere Ton-Streams: Stream 0 kommt auf die oberste Audiospur der Gruppe
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
        // Timeline-In/Out: In = Ziel, In+Out begrenzt die Länge, nur Out = rückwärts ab Out
        const int tIn = cur.markIn, tOut = cur.markOut;
        if (tIn >= 0 && tOut >= tIn) {
            start = tIn;
            len = std::min(len, tOut - tIn + 1);
        } else if (tIn >= 0) {
            start = tIn;
        } else if (tOut >= 0) {
            start = tOut + 1 - len;
            if (start < 0) { // vorne kürzen, das Ende bleibt am Out
                sIn -= start;
                len += start;
                start = 0;
            }
        }
        usedTimelineMarks = tIn >= 0 || tOut >= 0;
        if (mode == M::PlaceOnTop) {
            // Erste Spur über allen Clips im Bereich (fehlende Spur wird angelegt)
            auto freeTrack = [&](TrackKind kind) {
                int idx = 0;
                const auto& tracks = cur.tracks(kind);
                for (int i = 0; i < tracks.size(); ++i)
                    for (const Clip& c : tracks[i].clips)
                        if (c.start < start + len && c.end() > start) idx = i + 1;
                while (idx < tracks.size() && tracks[idx].locked) ++idx; // gesperrte Spur überspringen
                return idx;
            };
            vTrack = freeTrack(TrackKind::Video);
            aTrack = freeTrack(TrackKind::Audio);
        }
        if (mode == M::Insert) {
            // Wie DaVinci: alle nicht gesperrten Spuren rücken mit (bleiben synchron), die Zielspuren sowieso
            if (m.hasVideo) rippleTracks << TrackRef{TrackKind::Video, vTrack};
            for (int k = 0; k < m.audioStreamCount(); ++k) rippleTracks << TrackRef{TrackKind::Audio, aTrack + k};
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
        // 4-Punkt-Schnitt wie DaVinci: braucht Timeline-In und -Out; Geschwindigkeit = Quelllänge / Ziellänge
        const int tIn = cur.markIn, tOut = cur.markOut;
        if (tIn < 0 || tOut < tIn) return -1;
        const int target = tOut - tIn + 1;
        speed = double(len) / target;
        if (std::abs(speed - 1.0) < 1e-9) speed = 1.0;
        // in/out zählen im umgerechneten Material (Clip::speed)
        const int avail = std::max(1, int(m.length / speed));
        if (avail < target) return -1;
        sIn = std::clamp(int(std::lround(sIn / speed)), 0, avail - target);
        len = target;
        start = tIn;
        usedTimelineMarks = true;
        break;
    }
    case M::Replace: {
        // Länge und Lage bleiben; Quell-In (sonst Quell-Playhead) deckt sich mit dem Timeline-Playhead
        const Clip* c = clipUnderPlayhead();
        if (!c) return -1;
        partnerTracks(*c);
        const int anchor = m.markIn >= 0 ? m.markIn : srcPos;
        sIn = anchor - (playhead - c->start);
        len = c->length();
        if (sIn < 0 || sIn + len - 1 > last) return -1; // zu wenig Material
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
    // Gesperrte Zielspur: dieser Teil entfällt (wie beim Ablegen), gesperrte Spuren rücken nie
    MediaInfo m2 = m;
    if (m2.hasVideo && isTrackLocked(TrackRef{TrackKind::Video, vTrack})) m2.hasVideo = false;
    if (m2.hasAudio && isTrackLocked(TrackRef{TrackKind::Audio, aTrack})) m2.hasAudio = false;
    if (!m2.hasVideo && !m2.hasAudio) return -1;
    rippleTracks.erase(std::remove_if(rippleTracks.begin(), rippleTracks.end(), [&](const TrackRef& r) { return isTrackLocked(r); }),
                       rippleTracks.end());
    if (m2.hasVideo && !rippleTracks.contains(TrackRef{TrackKind::Video, vTrack}) && mode == M::RippleOverwrite)
        rippleTracks << TrackRef{TrackKind::Video, vTrack};
    if (m2.hasAudio && mode == M::RippleOverwrite)
        for (int k = 0; k < m2.audioStreamCount(); ++k)
            if (const TrackRef r{TrackKind::Audio, aTrack + k}; !rippleTracks.contains(r) && !isTrackLocked(r)) rippleTracks << r;

    QString text;
    switch (mode) {
    case M::Insert: text = T("Clip einfügen: %1"); break;
    case M::Overwrite: text = T("Clip überschreiben: %1"); break;
    case M::Replace: text = T("Clip ersetzen: %1"); break;
    case M::PlaceOnTop: text = T("Oben platzieren: %1"); break;
    case M::RippleOverwrite: text = T("Ripple-Überschreiben: %1"); break;
    case M::AppendAtEnd: text = T("Am Ende anhängen: %1"); break;
    case M::FitToFill: text = T("Einpassen: %1"); break;
    }
    p->edit(text.arg(m.name), [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        for (const TrackRef& r : rippleTracks) ensureTracks(tl, r.kind, r.index + 1);
        if (mode == M::Insert) {
            insertGap(tl, rippleTracks, start, len, newId, [p] { return p->newLinkId(); });
            insertSubtitleGap(tl, start, len, newId); // Untertitel bleiben synchron (gesperrte Spuren nicht)
        }
        if (mode == M::RippleOverwrite) {
            for (const TrackRef& r : rippleTracks) {
                clearRange(tl.track(r), start, replaceEnd, newId);
                shiftFrom(tl.track(r), rippleFrom, rippleDelta);
            }
            // übrige nicht gesperrte Spuren bleiben synchron (bleiben stehen, falls sie überschreiben würden)
            TimelineOps::rippleTracks(tl, {{rippleFrom, rippleDelta}}, rippleTracks);
        }
        placeSource(tl, m2, sIn, len, start, vTrack, aTrack, speed);
        if (usedTimelineMarks) tl.markIn = tl.markOut = -1; // wie DaVinci: benutzte Marken sind verbraucht
    });
    return mode == M::Replace ? playhead : start + len;
}

void Editor::placeSourceRange(const QString& path, int in, int out, int frame, int track)
{
    const MediaInfo* info = m_project->mediaInfo(path);
    if (!info || info->length <= 0) return;
    const MediaInfo m = *info;
    in = std::clamp(in, 0, m.length - 1);
    out = std::clamp(out, in, m.length - 1);
    MediaInfo m2 = m; // gesperrte Spur: dieser Teil entfällt
    track = std::max(0, track);
    if (m2.hasVideo && isTrackLocked(TrackRef{TrackKind::Video, track})) m2.hasVideo = false;
    if (m2.hasAudio && isTrackLocked(TrackRef{TrackKind::Audio, track})) m2.hasAudio = false;
    if (!m2.hasVideo && !m2.hasAudio) return;
    m_project->edit(T("Clip überschreiben: %1").arg(m.name), [&](Timeline& tl) {
        placeSource(tl, m2, in, out - in + 1, std::max(0, frame), track, track);
    });
}
