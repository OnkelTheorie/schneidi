// SPDX-License-Identifier: AGPL-3.0-only
// Editor: brand kit (core/BrandKit.h) - references resolved, the project's own kit, logo and intro/outro placed
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/MediaAspect.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QSet>
#include <cmath>

namespace {

// Resolve every reference in one timeline; true when something changed. Untouched tracks stay shared.
bool resolveTimeline(Timeline& tl, const BrandKit& kit)
{
    bool changed = false;
    TimelineOps::changeClips(
        tl,
        [&](const Clip& c) {
            if (!c.isTitle() || !c.title.hasBrandRef()) return false;
            Clip copy = c;
            return Brand::resolve(copy, kit);
        },
        [&](Clip& c) { changed |= Brand::resolve(c, kit); });
    for (int i = 0; i < tl.subtitles.size(); ++i) {
        if (!std::as_const(tl.subtitles)[i].style.hasBrandRef()) continue;
        TitleStyle t = std::as_const(tl.subtitles)[i].style;
        if (Brand::resolve(t, kit)) {
            tl.subtitles[i].style = t;
            changed = true;
        }
    }
    return changed;
}

} // namespace

bool Editor::applyBrand()
{
    const BrandKit kit = m_project->effectiveBrand();
    QVector<Sequence> after = m_project->sequences();
    bool changed = false;
    for (Sequence& s : after) changed |= resolveTimeline(s.timeline, kit);
    if (!changed) return false;
    m_project->closeMerge();
    m_project->editSequences(T("Markenpaket anwenden"), [&](QVector<Sequence>& seqs, int&) { seqs = after; });
    return true;
}

void Editor::setProjectBrand(const std::optional<BrandKit>& brand)
{
    const std::optional<BrandKit> b = brand ? std::optional<BrandKit>(brand->clamped()) : std::nullopt;
    if (b == m_project->brand()) return;
    QUndoStack* undo = m_project->undoStack();
    m_project->closeMerge();
    undo->beginMacro(b ? T("Markenpaket des Projekts") : T("Globales Markenpaket verwenden"));
    m_project->setBrand(b);
    applyBrand();
    undo->endMacro();
}

int Editor::addBrandLogo(int sequence)
{
    const BrandKit kit = m_project->effectiveBrand();
    const MediaInfo* m = kit.logo.isEmpty() ? nullptr : m_project->mediaInfo(kit.logo);
    if (!m || !m->hasVideo || m->length <= 0) return 0;
    const int id = sequence > 0 ? sequence : m_project->currentSequence();
    const Sequence* seq = m_project->sequence(id);
    if (!seq) return 0;
    const int end = TimelineOps::endFrame(seq->timeline);
    if (end <= 0) return 0;
    const ProjectFormat f = m_project->sequenceFormat(id);

    Clip c;
    c.mediaPath = m->path;
    c.start = 0;
    c.in = 0;
    c.out = (m->isImage ? end : std::min(end, m->length)) - 1; // a still for the whole timeline, a video as long as it is
    c.pictureAspect = m->pictureAspect;
    // Size: the picture's width is logoSize of the frame's; the corner keeps a margin of 4 % of the frame height
    const QSizeF frame(f.width, f.height);
    const QSizeF fitted = MediaAspect::fit(MediaAspect::ofClip(c), frame);
    if (fitted.width() > 0) c.transform.zoomX = c.transform.zoomY = kit.logoSize * f.width / fitted.width();
    const double w = fitted.width() * c.transform.zoomX, h = fitted.height() * c.transform.zoomY;
    const double margin = 0.04 * f.height;
    const double x = std::max(0.0, f.width / 2.0 - margin - w / 2), y = std::max(0.0, f.height / 2.0 - margin - h / 2);
    switch (kit.logoCorner) { // Y positive upward
    case 1: c.transform.posX = x, c.transform.posY = y; break;
    case 2: c.transform.posX = -x, c.transform.posY = y; break;
    case 3: c.transform.posX = x, c.transform.posY = -y; break;
    case 4: c.transform.posX = -x, c.transform.posY = -y; break;
    default: break; // centre
    }
    c.transform.posX = std::round(c.transform.posX * 10) / 10;
    c.transform.posY = std::round(c.transform.posY * 10) / 10;

    m_project->closeMerge();
    int clipId = 0;
    m_project->editSequences(T("Brand-Logo einfügen"), [&](QVector<Sequence>& seqs, int&) {
        for (Sequence& s : seqs) {
            if (s.id != id) continue;
            Timeline& tl = s.timeline;
            const int track = int(tl.video.size());
            TimelineOps::ensureTracks(tl, TrackKind::Video, track + 1);
            c.id = clipId = m_project->newClipId();
            TimelineOps::placeClip(tl.video[track], c, [this] { return m_project->newClipId(); });
        }
    });
    if (clipId && id == m_project->currentSequence()) m_selection->set({clipId});
    return clipId;
}

bool Editor::addIntroOutro()
{
    const BrandKit kit = m_project->effectiveBrand();
    auto usable = [this](const QString& path) -> const MediaInfo* {
        const MediaInfo* m = path.isEmpty() ? nullptr : m_project->mediaInfo(path);
        return m && (m->hasVideo || m->hasAudio) && m->length > 0 ? m : nullptr;
    };
    const MediaInfo* intro = usable(kit.intro);
    const MediaInfo* outro = usable(kit.outro);
    if ((!intro && !outro) || isTrackLocked({TrackKind::Video, 0})) return false;
    // A still as intro/outro gets 3 s (it has no length of its own)
    auto lengthOf = [this](const MediaInfo& m) { return m.isImage ? 3 * m_project->fps() : m.length; };
    Project* p = m_project;
    p->closeMerge();
    p->edit(T("Intro/Outro einfügen"), [&](Timeline& tl) {
        auto newId = [p] { return p->newClipId(); };
        if (outro) { // first: the end before the intro moves it
            const int end = TimelineOps::endFrame(tl);
            placeSource(tl, *outro, 0, lengthOf(*outro), end, 0, 0);
        }
        if (intro) {
            const int len = lengthOf(*intro);
            QVector<TrackRef> tracks;
            for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
                for (int i = 0; i < tl.tracks(k).size(); ++i)
                    if (!tl.tracks(k)[i].locked) tracks << TrackRef{k, i};
            TimelineOps::insertGap(tl, tracks, 0, len, newId, [p] { return p->newLinkId(); });
            TimelineOps::insertSubtitleGap(tl, 0, len, newId);
            placeSource(tl, *intro, 0, len, 0, 0, 0);
        }
    });
    return true;
}
