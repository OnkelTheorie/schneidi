// SPDX-License-Identifier: AGPL-3.0-only
// Every file a project names, see ProjectRefs.h
#include "core/ProjectRefs.h"

#include "core/CompGraph.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/ProjectFile.h"

namespace ProjectRefs {

void forEach(ProjectData& d, const std::function<void(QString& path, Kind kind)>& fn)
{
    const auto file = [&](QString& path, Kind kind) {
        if (!path.isEmpty() && !EffectFolders::isBuiltin(path)) fn(path, kind);
    };
    const auto scan = [&](Timeline& tl) {
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (Track& t : tl.tracks(k))
                for (Clip& c : t.clips) {
                    if (c.kind == ClipKind::Media) file(c.mediaPath, Kind::ClipMedia);
                    if (c.isTitle()) file(c.title.look.texture, Kind::TitleTexture);
                    file(c.transInStyle.luma, Kind::TransitionWipe);
                    file(c.transOutStyle.luma, Kind::TransitionWipe);
                    for (EffectInstance& e : c.effects) {
                        if (e.effectId == QLatin1String(Comp::kEffectId)) {
                            Comp::forEachFile(e, [&](const QString&, QString& path) { file(path, Kind::CompFile); });
                            continue;
                        }
                        const EffectDescriptor* desc = EffectRegistry::find(e.effectId);
                        if (!desc) continue;
                        for (const EffectParam& p : desc->params) {
                            if (p.type != EffectParam::Path || !e.params.contains(p.key)) continue;
                            QString path = e.params.value(p.key).toString();
                            const QString before = path;
                            file(path, Kind::EffectFile);
                            if (path != before) e.params[p.key] = path;
                        }
                    }
                }
        for (SubtitleTrack& t : tl.subtitles) file(t.style.look.texture, Kind::TitleTexture);
    };
    scan(d.timeline);
    for (Sequence& s : d.sequences)
        if (s.id != d.currentSequence) scan(s.timeline);
    // The open timeline is `timeline`; its copy in `sequences` follows it (a save writes it from there)
    for (Sequence& s : d.sequences)
        if (s.id == d.currentSequence) s.timeline = d.timeline;
    if (d.brand) {
        file(d.brand->logo, Kind::BrandFile);
        file(d.brand->intro, Kind::BrandFile);
        file(d.brand->outro, Kind::BrandFile);
    }
    for (MediaInfo& m : d.media) file(m.path, Kind::PoolMedia);
    for (WatchFolder& w : d.watchFolders) file(w.path, Kind::WatchFolder);
}

} // namespace ProjectRefs
