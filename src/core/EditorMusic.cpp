// SPDX-License-Identifier: AGPL-3.0-only
// Editor: music made to fit (core/MusicFit: the parts of a song cut to a length) onto an audio track
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QSet>

QVector<int> Editor::placeAudioClips(const QVector<Clip>& clips, int track, int replace, const QString& text)
{
    if (clips.isEmpty() || track < 0) return {};
    if (track < m_project->timeline().audio.size() && isTrackLocked({TrackKind::Audio, track})) return {};
    if (replace > 0) {
        TrackRef r;
        if (!TimelineOps::findClip(m_project->timeline(), replace, &r) || isTrackLocked(r)) return {};
    }
    QVector<int> ids;
    m_project->edit(text, [&](Timeline& tl) {
        if (replace > 0) TimelineOps::removeClip(tl, replace);
        TimelineOps::ensureTracks(tl, TrackKind::Audio, track + 1);
        Track& t = tl.audio[track];
        const int from = clips.first().start, to = clips.last().end();
        TimelineOps::clearDissolveAt(t, from);
        TimelineOps::clearDissolveAt(t, to);
        for (Clip c : clips) {
            c.id = m_project->newClipId();
            ids << c.id;
            c.transIn = c.transOut = 0; // placing the next one would clear a dissolve at its start: set afterwards
            TimelineOps::placeClip(t, c, [this] { return m_project->newClipId(); }, tl.frameRate);
        }
        for (int i = 0; i < clips.size(); ++i)
            TimelineOps::changeClips(
                tl, [&](const Clip& c) { return c.id == ids[i]; },
                [&](Clip& c) {
                    c.transIn = clips[i].transIn;
                    c.transOut = clips[i].transOut;
                });
    });
    if (!ids.isEmpty()) m_selection->set(QSet<int>(ids.begin(), ids.end()));
    return ids;
}
