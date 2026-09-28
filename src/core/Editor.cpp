#include "core/Editor.h"

#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QFileInfo>

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

void Editor::addMediaAt(const QString& path, int frame, int videoTrack)
{
    const MediaInfo* info = m_project->mediaInfo(path);
    if (!info || info->length <= 0) return;

    Project* p = m_project;
    const int start = std::max(0, frame);
    const int linkId = (info->hasVideo && info->hasAudio) ? p->newLinkId() : 0;
    const QString name = QFileInfo(path).fileName();

    p->edit(QString("Einfügen: %1").arg(name), [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        Clip c;
        c.mediaPath = path;
        c.start = start;
        c.in = 0;
        c.out = info->length - 1;
        c.linkId = linkId;
        if (info->hasVideo && !tl.video.isEmpty()) {
            Clip v = c;
            v.id = newId();
            const int idx = std::clamp(videoTrack, 0, int(tl.video.size()) - 1);
            TimelineOps::placeClip(tl.video[idx], v, newId);
        }
        if (info->hasAudio && !tl.audio.isEmpty()) {
            Clip a = c;
            a.id = newId();
            TimelineOps::placeClip(tl.audio[0], a, newId);
        }
    });
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
