#include "core/Editor.h"

#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <algorithm>

Editor::Editor(Project* project, Selection* selection, QObject* parent)
    : QObject(parent), m_project(project), m_selection(selection) {}

QVector<int> Editor::withLinked(const QVector<int>& ids) const
{
    if (!m_linkedSelection) return ids;
    QVector<int> out;
    for (int id : ids)
        for (int g : TimelineOps::linkedGroup(m_project->timeline(), id))
            if (!out.contains(g)) out << g;
    return out;
}

static void ensureTracks(Timeline& tl, TrackKind kind, int count)
{
    auto& tracks = tl.tracks(kind);
    while (tracks.size() < count) {
        Track t;
        t.kind = kind;
        t.name = QString("%1%2").arg(kind == TrackKind::Video ? "V" : "A").arg(tracks.size() + 1);
        tracks << t;
    }
}

void Editor::addMediaAt(const QStringList& paths, int frame, int track)
{
    Project* p = m_project;
    QVector<const MediaInfo*> infos;
    for (const QString& path : paths)
        if (const MediaInfo* info = p->mediaInfo(path); info && info->length > 0) infos << info;
    if (infos.isEmpty()) return;

    const QString text = infos.size() == 1 ? QString("Einfügen: %1").arg(infos.first()->name)
                                           : QString("%1 Clips einfügen").arg(infos.size());
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
            c.linkId = (info->hasVideo && info->hasAudio) ? p->newLinkId() : 0;
            if (info->hasVideo) {
                ensureTracks(tl, TrackKind::Video, idx + 1);
                Clip v = c;
                v.id = newId();
                TimelineOps::placeClip(tl.video[idx], v, newId);
            }
            if (info->hasAudio) {
                ensureTracks(tl, TrackKind::Audio, idx + 1);
                Clip a = c;
                a.id = newId();
                TimelineOps::placeClip(tl.audio[idx], a, newId);
            }
            start += info->length;
        }
    });
}

void Editor::toggleTrackMute(TrackRef ref)
{
    m_project->edit("Spur stumm", [&](Timeline& tl) { tl.track(ref).muted = !tl.track(ref).muted; });
}

void Editor::toggleTrackHidden(TrackRef ref)
{
    m_project->edit("Spur ausblenden", [&](Timeline& tl) { tl.track(ref).hidden = !tl.track(ref).hidden; });
}

void Editor::moveClips(const QVector<int>& ids, int deltaFrames, TrackKind kind, int trackDelta)
{
    if (ids.isEmpty() || (deltaFrames == 0 && trackDelta == 0)) return;
    Project* p = m_project;
    p->edit("Clips verschieben", [&](Timeline& tl) {
        TimelineOps::moveClips(tl, ids, deltaFrames, kind, trackDelta,
                               [p] { return p->newClipId(); });
    });
}

TimelineOps::SourceLength Editor::sourceLength() const
{
    return [p = m_project](const QString& path) {
        const MediaInfo* m = p->mediaInfo(path);
        return m && !m->isImage ? m->length : 0; // Standbilder beliebig lang ziehbar
    };
}

int Editor::clampTrim(int clipId, TimelineOps::Edge edge, int delta) const
{
    return TimelineOps::clampTrim(m_project->timeline(), withLinked({clipId}), edge, delta, sourceLength());
}

void Editor::trimClip(int clipId, TimelineOps::Edge edge, int delta)
{
    const QVector<int> ids = withLinked({clipId});
    if (TimelineOps::clampTrim(m_project->timeline(), ids, edge, delta, sourceLength()) == 0) return;
    m_project->edit("Trimmen", [&](Timeline& tl) { TimelineOps::trimClips(tl, ids, edge, delta, sourceLength()); });
}

void Editor::setClipVolume(int clipId, double db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || c->volumeDb == db) return;
    m_project->edit("Lautstärke", [&](Timeline& tl) {
        if (Clip* clip = TimelineOps::findClip(tl, clipId)) clip->volumeDb = db;
    });
}

void Editor::bladeAt(int clipId, int frame)
{
    Project* p = m_project;
    const QVector<int> ids = withLinked({clipId});
    p->edit("Schnitt", [&](Timeline& tl) {
        TimelineOps::splitAt(tl, ids, frame, [p] { return p->newClipId(); },
                             [p] { return p->newLinkId(); });
    });
}

void Editor::splitAtPlayhead(int frame)
{
    // Wie DaVinci: mit Auswahl nur die ausgewählten Clips, sonst alle unter dem Playhead
    QVector<int> ids;
    if (!m_selection->isEmpty()) {
        ids = withLinked(m_selection->ids().values().toVector());
    } else {
        const Timeline& tl = m_project->timeline();
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const auto& t : tl.tracks(k))
                for (const auto& c : t.clips)
                    if (frame > c.start && frame < c.end()) ids << c.id;
    }
    if (ids.isEmpty()) return;
    Project* p = m_project;
    p->edit("Schnitt am Playhead", [&](Timeline& tl) {
        TimelineOps::splitAt(tl, ids, frame, [p] { return p->newClipId(); },
                             [p] { return p->newLinkId(); });
    });
}

void Editor::deleteSelection()
{
    if (m_selection->isEmpty()) return;
    const QVector<int> ids = m_selection->ids().values().toVector();
    // Löschen ohne Ripple: es bleibt eine Lücke, nichts rutscht nach
    m_project->edit("Löschen", [&](Timeline& tl) {
        for (int id : ids) TimelineOps::removeClip(tl, id);
    });
    m_selection->clear();
}
