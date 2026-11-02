// SPDX-License-Identifier: AGPL-3.0-only
// Editor: Auto-Reframe (core/Reframe.h)
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/MediaAspect.h"
#include "core/Project.h"

#include <QSet>

QVector<int> Editor::reframeTargets(const QVector<int>& ids) const
{
    const Timeline& tl = m_project->timeline();
    const QSet<int> wanted(ids.begin(), ids.end());
    QVector<int> out;
    for (int t = 0; t < tl.video.size(); ++t) {
        if (isTrackLocked({TrackKind::Video, t})) continue;
        for (const Clip& c : tl.video[t].clips)
            if (Reframe::canReframe(c) && (ids.isEmpty() || wanted.contains(c.id))) out << c.id;
    }
    return out;
}

int Editor::reframeClips(const QVector<int>& ids, Reframe::Mode mode, const QHash<int, QVector<Reframe::Key>>& paths)
{
    const QVector<int> targets = reframeTargets(ids);
    if (targets.isEmpty()) return 0;
    const QSizeF frame = m_project->timelineFormat().size();
    m_project->edit(T("Auto-Reframe"), [&](Timeline& tl) {
        for (int id : targets)
            if (Clip* c = TimelineOps::findClip(tl, id))
                Reframe::apply(*c, mode, paths.value(id), MediaAspect::of(c->mediaPath), frame);
    });
    return int(targets.size());
}
