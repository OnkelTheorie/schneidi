// SPDX-License-Identifier: AGPL-3.0-only
// Editor: Auto-Reframe (core/Reframe.h)
#include "core/Editor.h"
#include "core/GameHud.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Project.h"

#include <QSet>

#include <algorithm>
#include <cmath>

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

int Editor::reframeClips(const QVector<int>& ids, Reframe::Mode mode, const QHash<int, QVector<Reframe::Key>>& paths,
                         const QHash<QString, QRectF>& contents, const QHash<int, QVector<Reframe::Keep>>* keeps)
{
    const QVector<int> targets = reframeTargets(ids);
    if (targets.isEmpty()) return 0;
    const QSizeF frame = m_project->timelineFormat().size();
    Project* p = m_project;
    m_project->edit(T("Auto-Reframe"), [&](Timeline& tl) {
        for (int id : targets)
            if (Clip* c = TimelineOps::findClip(tl, id))
                Reframe::apply(*c, mode, paths.value(id), MediaAspect::ofClip(*c), frame,
                               contents.value(c->mediaPath, QRectF(0, 0, 1, 1)));
        if (!keeps) return;
        for (int id : targets) {
            TrackRef ref;
            const Clip* found = TimelineOps::findClip(tl, id, &ref);
            if (!found) continue;
            const Clip base = *found;
            // The earlier copies of this clip go (a new reframe brings its own)
            for (Track& t : tl.video)
                if (!t.locked)
                    t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                                 [&](const Clip& x) { return Reframe::isKeepOf(x, base); }),
                                  t.clips.end());
            for (const Reframe::Keep& k : keeps->value(id)) {
                Clip copy = Reframe::keepCopy(base, k, MediaAspect::ofClip(base), frame);
                copy.id = p->newClipId();
                // The first unlocked track above the clip that is free over its range (a missing one is added)
                int track = ref.index + 1;
                const auto busy = [&](int i) {
                    if (i >= tl.video.size()) return false;
                    if (tl.video[i].locked) return true;
                    return std::any_of(tl.video[i].clips.begin(), tl.video[i].clips.end(),
                                       [&](const Clip& x) { return x.start < copy.end() && x.end() > copy.start; });
                };
                while (busy(track)) ++track;
                TimelineOps::ensureTracks(tl, TrackKind::Video, track + 1);
                TimelineOps::placeClip(tl.video[track], copy, [p] { return p->newClipId(); });
            }
        }
    });
    return int(targets.size());
}

QHash<int, QVector<Reframe::Keep>> Editor::reframeKeeps(const QVector<int>& ids, Reframe::Mode mode,
                                                         const QHash<QString, QRectF>& contents,
                                                         const Reframe::KeepChoice& choice, QVector<int>* noGame) const
{
    QHash<int, QVector<Reframe::Keep>> out;
    const QSizeF frame = m_project->timelineFormat().size();
    for (int id : ids) {
        const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
        if (!c) continue;
        QVector<Reframe::Keep>& list = out[id];
        if (choice.kind == Reframe::KeepChoice::None) continue;
        if (choice.kind == Reframe::KeepChoice::Custom) {
            list << Reframe::Keep{QStringLiteral("custom"), choice.region};
            continue;
        }
        const GameHud::Game* game = choice.game.isEmpty() ? GameHud::guess(c->mediaPath) : GameHud::find(choice.game);
        const QRectF content = contents.value(c->mediaPath, QRectF(0, 0, 1, 1));
        if (!game) {
            if (choice.kind == Reframe::KeepChoice::KillFeed && noGame) *noGame << id;
            continue;
        }
        if (choice.kind == Reframe::KeepChoice::KillFeed || Reframe::cropsSides(mode, MediaAspect::ofClip(*c), frame, content))
            list << Reframe::Keep{QStringLiteral("killfeed"), Reframe::inContent(game->feed, content)};
    }
    return out;
}

int Editor::cropBars(const QVector<int>& ids, const QHash<QString, QRectF>& contents)
{
    const QSizeF frame = m_project->timelineFormat().size();
    QVector<int> changed;
    {
        Timeline probe = m_project->timeline();
        for (int id : reframeTargets(ids))
            if (Clip* c = TimelineOps::findClip(probe, id); c && contents.contains(c->mediaPath)
                && Reframe::cropBars(*c, contents.value(c->mediaPath), MediaAspect::ofClip(*c), frame))
                changed << id;
    }
    if (changed.isEmpty()) return 0;
    m_project->edit(T("Schwarze Balken wegschneiden"), [&](Timeline& tl) {
        for (int id : changed)
            if (Clip* c = TimelineOps::findClip(tl, id))
                Reframe::cropBars(*c, contents.value(c->mediaPath), MediaAspect::ofClip(*c), frame);
    });
    return int(changed.size());
}

int Editor::setPictureAspect(const QVector<int>& ids, double aspect, std::optional<FitFill> fill)
{
    aspect = aspect > 0 ? std::clamp(aspect, 0.1, 10.0) : 0;
    QVector<int> changed;
    for (int id : reframeTargets(ids))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
            c && (std::abs(c->pictureAspect - aspect) > 1e-9 || (fill && c->transform.fill != *fill)))
            changed << id;
    if (changed.isEmpty()) return 0;
    const QSizeF frame = m_project->timelineFormat().size();
    m_project->edit(aspect > 0 ? T("Seitenverhältnis entzerren") : T("Seitenverhältnis der Datei"), [&](Timeline& tl) {
        for (int id : changed)
            if (Clip* c = TimelineOps::findClip(tl, id)) {
                // Crop values are pixels on the fitted picture: keep their share of it
                if (fill) c->transform.fill = *fill;
                const QSizeF a = MediaAspect::fit(MediaAspect::ofClip(*c), frame);
                c->pictureAspect = aspect;
                const QSizeF b = MediaAspect::fit(MediaAspect::ofClip(*c), frame);
                if (a.width() > 0 && a.height() > 0 && a != b) {
                    const double kx = b.width() / a.width(), ky = b.height() / a.height();
                    for (AnimParam p : {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom}) {
                        const double k = p == AnimParam::CropLeft || p == AnimParam::CropRight ? kx : ky;
                        Keys::convert(*c, p, [k](double v) { return v * k; }, k);
                    }
                }
            }
    });
    return int(changed.size());
}
