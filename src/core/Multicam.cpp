// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Multicam.h"

#include "core/Project.h"

#include <QFileInfo>

#include <cmath>

namespace Multicam {

QString defaultAngleName(const QString& path) { return QFileInfo(path).completeBaseName(); }

QVector<Angle> resolve(const Project& project, const QVector<Source>& sources, SyncMethod method,
                       const QVector<AudioSync::Match>& sound, bool fallback)
{
    QVector<Angle> out;
    const MediaInfo* ref = nullptr;
    for (const Source& s : sources)
        if (!s.files.isEmpty()) {
            ref = project.mediaInfo(s.files.first());
            break;
        }
    const double fps = project.frameRate();
    auto byMarker = [&](const MediaInfo* m, FilePlacement* f) {
        if (!ref || !m || ref->markers.isEmpty() || m->markers.isEmpty()) return false;
        f->offset = ref->markers.first() - m->markers.first();
        f->via = SyncMethod::Marker;
        return true;
    };

    int index = 0; // file index over all sources (matches `sound`)
    bool first = true;
    for (const Source& s : sources) {
        Angle a;
        a.name = s.name.trimmed();
        if (a.name.isEmpty() && !s.files.isEmpty()) a.name = defaultAngleName(s.files.first());
        for (int j = 0; j < s.files.size(); ++j, ++index) {
            FilePlacement f;
            f.path = s.files[j];
            const MediaInfo* m = project.mediaInfo(f.path);
            bool synced = false;
            if (first) { // the reference itself
                f.via = method;
                f.confidence = method == SyncMethod::Sound ? 1.0 : 0.0;
                synced = true;
                first = false;
            } else {
                switch (method) {
                case SyncMethod::Sound:
                    if (const AudioSync::Match mt = sound.value(index); mt.found) {
                        f.offset = int(std::lround(mt.offsetS * fps));
                        f.via = SyncMethod::Sound;
                        f.confidence = mt.confidence;
                        synced = true;
                    } else if (fallback) {
                        synced = byMarker(m, &f);
                    }
                    break;
                case SyncMethod::Marker: synced = byMarker(m, &f); break;
                case SyncMethod::InPoint:
                    f.offset = std::max(0, ref ? ref->markIn : 0) - std::max(0, m ? m->markIn : 0);
                    f.via = SyncMethod::InPoint;
                    synced = true;
                    break;
                case SyncMethod::Start:
                    synced = j == 0; // later files of a split recording have no sync info
                    break;
                }
            }
            if (!synced) { // at the start of the angle, or right after its previous file
                f.via = SyncMethod::Start;
                f.offset = 0;
                f.confidence = 0;
                if (j > 0) {
                    const FilePlacement& prev = a.files.last();
                    const MediaInfo* pm = project.mediaInfo(prev.path);
                    f.offset = prev.offset + (pm ? pm->length : 0);
                }
            }
            f.synced = synced;
            a.files << f;
        }
        out << a;
    }
    return out;
}

bool angleHas(const Sequence& s, int angle, TrackKind kind)
{
    if (angle < 0 || angle >= s.angles.size()) return false;
    const MulticamAngle& a = s.angles[angle];
    return kind == TrackKind::Video ? a.videoTrack >= 0 && a.videoTrack < s.timeline.video.size()
                                    : !a.audioTracks.isEmpty();
}

Timeline angleTimeline(const Timeline& content, const MulticamAngle& angle, TrackKind kind)
{
    Timeline t = content;
    t.video.clear();
    t.audio.clear();
    t.subtitles.clear();
    t.nested.reset();
    t.angles.clear();
    if (kind == TrackKind::Video) {
        if (angle.videoTrack >= 0 && angle.videoTrack < content.video.size()) t.video << content.video[angle.videoTrack];
    } else {
        for (int i : angle.audioTracks)
            if (i >= 0 && i < content.audio.size()) t.audio << content.audio[i];
    }
    return t;
}

std::optional<SourcePos> sourceAt(const Sequence& s, int angle, int frame, TrackKind kind)
{
    if (!angleHas(s, angle, kind)) return std::nullopt;
    const MulticamAngle& a = s.angles[angle];
    const int track = kind == TrackKind::Video ? a.videoTrack : a.audioTracks.first();
    const QVector<Track>& tracks = s.timeline.tracks(kind);
    if (track < 0 || track >= tracks.size()) return std::nullopt;
    for (const Clip& c : tracks[track].clips)
        if (c.start <= frame && frame < c.end()) return SourcePos{c.mediaPath, c.in + (frame - c.start), c.id};
    return std::nullopt;
}

int sequenceFrame(const Clip& segment, int frame) { return segment.in + (frame - segment.start); }

const Clip* clipAt(const Timeline& tl, int frame, const QSet<int>& selected)
{
    auto covers = [&](const Clip& c) { return c.isMulticam() && c.start <= frame && frame < c.end(); };
    if (!selected.isEmpty())
        for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            const QVector<Track>& tracks = tl.tracks(kind);
            for (int i = tracks.size() - 1; i >= 0; --i)
                for (const Clip& c : tracks[i].clips)
                    if (covers(c) && selected.contains(c.id)) return &c;
        }
    for (int i = tl.video.size() - 1; i >= 0; --i) { // what the viewer shows: the top-most picture
        if (tl.video[i].hidden) continue;
        for (const Clip& c : tl.video[i].clips)
            if (covers(c) && c.enabled) return &c;
    }
    for (const Track& t : tl.audio)
        for (const Clip& c : t.clips)
            if (covers(c) && c.enabled) return &c;
    return nullptr;
}

} // namespace Multicam
