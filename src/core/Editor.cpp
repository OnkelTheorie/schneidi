#include "core/Editor.h"

#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
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
                TimelineOps::ensureTracks(tl, TrackKind::Video, idx + 1);
                Clip v = c;
                v.id = newId();
                TimelineOps::placeClip(tl.video[idx], v, newId);
            }
            if (info->hasAudio) {
                TimelineOps::ensureTracks(tl, TrackKind::Audio, idx + 1);
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

QVector<int> Editor::targetIds(int frame) const
{
    if (!m_selection->isEmpty()) return withLinked(m_selection->ids().values().toVector());
    QVector<int> ids;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips)
                if (frame > c.start && frame < c.end()) ids << c.id;
    return ids;
}

void Editor::splitAtPlayhead(int frame)
{
    // Wie DaVinci: mit Auswahl nur die ausgewählten Clips, sonst alle unter dem Playhead
    const QVector<int> ids = targetIds(frame);
    if (ids.isEmpty()) return;
    Project* p = m_project;
    p->edit("Schnitt am Playhead", [&](Timeline& tl) {
        TimelineOps::splitAt(tl, ids, frame, [p] { return p->newClipId(); },
                             [p] { return p->newLinkId(); });
    });
}

void Editor::modifyClips(const QVector<int>& ids, const QString& text, const std::function<void(Clip&)>& fn,
                         const QString& mergeKey)
{
    if (ids.isEmpty()) return;
    m_project->edit(text, [&](Timeline& tl) {
        for (int id : ids)
            if (Clip* c = TimelineOps::findClip(tl, id)) fn(*c);
    }, mergeKey);
}

void Editor::rippleDeleteSelection()
{
    if (m_selection->isEmpty()) return;
    const QVector<int> ids = m_selection->ids().values().toVector();
    m_project->edit("Löschen mit Ripple", [&](Timeline& tl) { TimelineOps::rippleDelete(tl, ids); });
    m_selection->clear();
}

void Editor::selectAll()
{
    QSet<int> ids;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips) ids.insert(c.id);
    m_selection->set(ids);
}

void Editor::nudgeSelection(int frames)
{
    if (m_selection->isEmpty()) return;
    const QVector<int> ids = m_selection->ids().values().toVector();
    int minStart = INT_MAX;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) minStart = std::min(minStart, c->start);
    frames = std::max(frames, -minStart); // nicht vor Frame 0
    if (frames == 0) return;
    Project* p = m_project;
    p->edit("Nudge", [&](Timeline& tl) {
        TimelineOps::moveClips(tl, ids, frames, TrackKind::Video, 0, [p] { return p->newClipId(); });
    });
}

void Editor::trimToPlayhead(TimelineOps::Edge edge, int frame)
{
    const Timeline& cur = m_project->timeline();
    QVector<int> ids;
    for (int id : targetIds(frame))
        if (const Clip* c = TimelineOps::findClip(cur, id); c && frame > c->start && frame < c->end()) ids << id;
    if (ids.isEmpty()) return;
    const auto len = sourceLength();
    m_project->edit(edge == TimelineOps::Edge::Start ? "Anfang trimmen" : "Ende trimmen", [&](Timeline& tl) {
        for (int id : ids) {
            const Clip* c = TimelineOps::findClip(tl, id);
            if (!c) continue;
            const int delta = frame - (edge == TimelineOps::Edge::Start ? c->start : c->end());
            TimelineOps::trimClips(tl, {id}, edge, delta, len);
        }
    });
}

void Editor::toggleSelectionEnabled()
{
    if (m_selection->isEmpty()) return;
    const QVector<int> ids = m_selection->ids().values().toVector();
    // Wie DaVinci: ist einer aktiv, werden alle deaktiviert
    bool anyEnabled = false;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) anyEnabled |= c->enabled;
    modifyClips(ids, anyEnabled ? "Clip deaktivieren" : "Clip aktivieren",
                [anyEnabled](Clip& c) { c.enabled = !anyEnabled; });
}

void Editor::toggleLinkSelection()
{
    const QVector<int> ids = m_selection->ids().values().toVector();
    if (ids.isEmpty()) return;
    bool anyLinked = false;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) anyLinked |= c->linkId != 0;
    if (!anyLinked && ids.size() < 2) return;
    const int link = anyLinked ? 0 : m_project->newLinkId();
    modifyClips(ids, anyLinked ? "Verknüpfung lösen" : "Clips verknüpfen", [link](Clip& c) { c.linkId = link; });
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

void Editor::copySelection()
{
    const Timeline& tl = m_project->timeline();
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
    copySelection();
    deleteSelection();
}

void Editor::paste(int frame)
{
    if (m_clipboard.isEmpty()) return;
    Project* p = m_project;
    QSet<int> pasted;
    p->edit("Einfügen", [&](Timeline& tl) {
        QHash<int, int> linkMap; // Kopie bekommt eigene Verknüpfung
        for (const auto& it : m_clipboard) {
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
