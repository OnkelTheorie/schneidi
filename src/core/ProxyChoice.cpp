// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ProxyChoice.h"

#include "core/I18n.h"
#include "core/Project.h"

#include <QFileInfo>

namespace ProxyChoice {

namespace {

struct Collector {
    const Project& project;
    QStringList out;
    QSet<int> seenSequences; // a sequence's clips once (a compound clip used twice, nesting)

    void addFile(const QString& path)
    {
        if (path.isEmpty() || out.contains(path)) return;
        const MediaInfo* m = project.mediaInfo(path);
        if (m && m->hasVideo && !m->isImage && QFileInfo::exists(path)) out << path;
    }
    void addClip(const Clip& c)
    {
        if (c.kind == ClipKind::Media) return addFile(c.mediaPath);
        if ((c.kind == ClipKind::Compound || c.kind == ClipKind::Multicam) && !seenSequences.contains(c.sequenceId)) {
            seenSequences.insert(c.sequenceId);
            if (const Sequence* s = project.sequence(c.sequenceId)) addTimeline(s->timeline, nullptr);
        }
    }
    // All clips of `tl`, or only those with an id in `ids`
    void addTimeline(const Timeline& tl, const QSet<int>* ids)
    {
        for (const QVector<Track>* tracks : {&tl.video, &tl.audio})
            for (const Track& track : *tracks)
                for (const Clip& c : track.clips)
                    if (!ids || ids->contains(c.id)) addClip(c);
    }
};

} // namespace

QStringList files(const Project& project, Scope scope, const QSet<int>& clipIds)
{
    Collector c{project, {}, {}};
    switch (scope) {
    case Scope::Pool:
        for (const MediaInfo& m : project.media()) c.addFile(m.path);
        break;
    case Scope::Timeline:
        c.addTimeline(project.timeline(), nullptr);
        break;
    case Scope::Selected:
        c.addTimeline(project.timeline(), &clipIds);
        break;
    }
    return c.out;
}

QString label(const QString& action, int files, int missing, int pending)
{
    if (files == 0) return action + " " + T("(keine Videodateien)");
    if (missing > 0) return QString("%1 (%2)").arg(action).arg(missing);
    if (pending > 0) return action + " " + T("(werden erzeugt)");
    return action + " " + T("(schon vorhanden)");
}

} // namespace ProxyChoice
