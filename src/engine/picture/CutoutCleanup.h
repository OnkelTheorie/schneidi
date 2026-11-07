// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Cleaning up the cutout analyses (CacheLocation/cutout/<id>.matte, engine/picture/CompCutout): a file stays while a
// cutout node of the open or a recent project names it (its `cache` value); the others go with "Proxy-Medien
// aufräumen" (app/ProxyCleanupDialog). An analysis can always be made again (the node's button).

#include "core/ProjectFile.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace Picture::Comp {

// The analyses the cutout nodes of a project use (every timeline and compound clip)
QStringList cutoutCachesOf(const ProjectData& data);
// The same for project files read from disk (unreadable ones are skipped)
QStringList cutoutCachesOfProjects(const QStringList& projectFiles);

struct CutoutCacheFile {
    QString id, file;
    qint64 bytes = 0;
};
// Analysis files in `dir` (empty = cutoutCacheDir()) none of `keep` names, largest first
QVector<CutoutCacheFile> unusedCutoutCaches(const QStringList& keep, const QString& dir = {});

} // namespace Picture::Comp
