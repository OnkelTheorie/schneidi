// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/CutoutCleanup.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "engine/picture/CompCutout.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace Picture::Comp {

namespace {

void addTimeline(const Timeline& tl, QStringList* out)
{
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips) {
            const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
            CompGraph g;
            if (!e || !::Comp::graphOf(*e, &g)) continue;
            for (const CompNode& n : g.nodes)
                if (n.type == QLatin1String(kCutoutType))
                    if (const QString id = e->params.value(CompGraph::paramKey(n.id, kCutoutCacheKey)).toString(); !id.isEmpty())
                        *out << id;
        }
}

} // namespace

QStringList cutoutCachesOf(const ProjectData& data)
{
    QStringList out;
    addTimeline(data.timeline, &out);
    for (const Sequence& s : data.sequences) addTimeline(s.timeline, &out);
    out.removeDuplicates();
    return out;
}

QStringList cutoutCachesOfProjects(const QStringList& projectFiles)
{
    QStringList out;
    for (const QString& file : projectFiles) {
        ProjectData data;
        QString error;
        if (QFileInfo::exists(file) && ProjectFile::load(file, &data, &error)) out << cutoutCachesOf(data);
    }
    out.removeDuplicates();
    return out;
}

QVector<CutoutCacheFile> unusedCutoutCaches(const QStringList& keep, const QString& dirPath)
{
    const QSet<QString> kept(keep.begin(), keep.end());
    QVector<CutoutCacheFile> out;
    const QDir dir(dirPath.isEmpty() ? cutoutCacheDir() : dirPath);
    for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*.matte")}, QDir::Files)) {
        const QString id = f.completeBaseName();
        if (!kept.contains(id)) out << CutoutCacheFile{id, f.absoluteFilePath(), f.size()};
    }
    std::sort(out.begin(), out.end(), [](const CutoutCacheFile& a, const CutoutCacheFile& b) { return a.bytes > b.bytes; });
    return out;
}

} // namespace Picture::Comp
