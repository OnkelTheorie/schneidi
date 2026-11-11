// SPDX-License-Identifier: AGPL-3.0-only
// Editor: Auto-Reframe (core/Reframe.h)
#include "core/Editor.h"
#include "core/GameHud.h"
#include "core/GradeWindows.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Project.h"

#include <QSet>

#include <algorithm>
#include <cmath>

QVector<int> Editor::reframeTargets(const QVector<int>& ids, QVector<int>* overlays) const
{
    const Timeline& tl = m_project->timeline();
    const QSet<int> wanted(ids.begin(), ids.end());
    const QSizeF frame = m_project->timelineFormat().size();
    QVector<int> out;
    for (int t = 0; t < tl.video.size(); ++t) {
        if (isTrackLocked({TrackKind::Video, t})) continue;
        for (const Clip& c : tl.video[t].clips) {
            if (!Reframe::canReframe(c) || (!ids.isEmpty() && !wanted.contains(c.id))) continue;
            if (ids.isEmpty() && overlays && Reframe::isOverlay(c, t, MediaAspect::ofClip(c), frame)) *overlays << c.id;
            else out << c.id;
        }
    }
    return out;
}

QVector<int> Editor::followingCopies(const QVector<int>& ids) const
{
    const Timeline& tl = m_project->timeline();
    // Of the video clips among ids that are no copies themselves: the copy shares its link with the clip's sound too,
    // which edited alone (Alt-click, linked selection off) must not take the copy away from its picture
    QSet<QPair<int, QString>> leads;
    for (int id : ids)
        if (TrackRef r; const Clip* c = TimelineOps::findClip(tl, id, &r))
            if (r.kind == TrackKind::Video && c->linkId && c->keepRegion.isEmpty()) leads.insert({c->linkId, c->mediaPath});
    QVector<int> out;
    if (leads.isEmpty()) return out;
    for (const Track& t : tl.video)
        for (const Clip& x : t.clips)
            if (!x.keepRegion.isEmpty() && leads.contains({x.linkId, x.mediaPath})) out << x.id;
    return editable(out);
}

QVector<int> Editor::withCopies(const QVector<int>& ids) const
{
    QVector<int> out = ids;
    for (int id : followingCopies(ids))
        if (!out.contains(id)) out << id;
    return out;
}

int Editor::clampTrackDelta(const QVector<int>& ids, TrackKind kind, int trackDelta) const
{
    const QVector<int> copies = followingCopies(ids);
    QVector<int> lead;
    for (int id : ids)
        if (!copies.contains(id)) lead << id;
    trackDelta = TimelineOps::clampTrackDelta(m_project->timeline(), lead, kind, trackDelta);
    // The copies (video clips) only not below V1: anchored on audio, their tracks set no upper limit
    return TimelineOps::clampTrackDelta(m_project->timeline(), copies, TrackKind::Audio, trackDelta);
}

QVector<int> Editor::withKeptCopies(const QVector<int>& ids) const
{
    const Timeline& tl = m_project->timeline();
    QVector<int> out = withCopies(ids);
    QVector<const Clip*> bases;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(tl, id); c && c->keepRegion.isEmpty()) bases << c;
    if (bases.isEmpty()) return out;
    // Copies made before they were linked to their clip: the ones in sync with it
    for (const Track& t : tl.video)
        for (const Clip& x : t.clips)
            if (!x.keepRegion.isEmpty() && !out.contains(x.id)
                && std::any_of(bases.begin(), bases.end(), [&](const Clip* b) { return Reframe::isKeepAlong(x, *b); }))
                out << x.id;
    return out;
}

int Editor::reframeClips(const QVector<int>& ids, Reframe::Mode mode, const QHash<int, QVector<Reframe::Key>>& paths,
                         const QHash<QString, QRectF>& contents, const QHash<int, QVector<Reframe::Keep>>* keeps)
{
    QVector<int> overlays; // left out of a whole timeline
    const QVector<int> targets = reframeTargets(ids, &overlays);
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
            Clip* found = TimelineOps::findClip(tl, id, &ref);
            if (!found) continue;
            // The copies are linked to their clip: they follow it (Editor::followingCopies)
            if (!keeps->value(id).isEmpty() && !found->linkId) found->linkId = p->newLinkId();
            const Clip base = *found;
            const auto linkedCopy = [&](const Clip& x) {
                return !x.keepRegion.isEmpty() && base.linkId && x.linkId == base.linkId;
            };
            // The earlier copies of this clip go (a new reframe brings its own): exact ones as a whole, ones that
            // outlast a since trimmed or bladed clip over its range; what is left of them stays only over another
            // part of the bladed clip (in sync with it), not over frames the clip was trimmed off
            const auto overAnotherPart = [&](const Clip& x) {
                for (const Track& vt : tl.video)
                    for (const Clip& y : vt.clips)
                        if (y.id != base.id && Reframe::isKeepAlong(x, y)) return true;
                return false;
            };
            for (Track& t : tl.video) {
                if (t.locked) continue;
                Track along;
                QVector<Clip> rest;
                for (const Clip& x : t.clips)
                    if (Reframe::isKeepOf(x, base) || linkedCopy(x)) continue;
                    else if (Reframe::isKeepAlong(x, base)) along.clips << x;
                    else rest << x;
                if (rest.size() == t.clips.size()) continue;
                TimelineOps::clearRange(along, base.start, base.end(), [p] { return p->newClipId(); }, tl.frameRate);
                for (const Clip& x : along.clips)
                    if (overAnotherPart(x)) rest << x;
                std::sort(rest.begin(), rest.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
                t.clips = rest;
            }
            for (const Reframe::Keep& k : keeps->value(id)) {
                Clip copy = Reframe::keepCopy(base, k, MediaAspect::ofClip(base), frame);
                copy.id = p->newClipId();
                copy.linkId = base.linkId;
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
                TimelineOps::placeClip(tl.video[track], copy, [p] { return p->newClipId(); }, tl.frameRate);
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
    // Gives the clip the new shape; returns its old aspect
    const auto setShape = [&](Clip& c) {
        // Crop values are pixels on the fitted picture: keep their share of it
        const double was = MediaAspect::ofClip(c);
        const QSizeF a = MediaAspect::fit(was, frame);
        c.pictureAspect = aspect;
        const QSizeF b = MediaAspect::fit(MediaAspect::ofClip(c), frame);
        if (a.width() > 0 && a.height() > 0 && a != b) {
            const double kx = b.width() / a.width(), ky = b.height() / a.height();
            for (AnimParam p : {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom}) {
                const double k = p == AnimParam::CropLeft || p == AnimParam::CropRight ? kx : ky;
                Keys::convert(c, p, [k](double v) { return v * k; }, k);
            }
            // Windows (blurred names) stay on the same part of the picture, which now fills another share of the frame
            GradeWindows::refit(c, kx, ky);
        }
        return was;
    };
    m_project->edit(aspect > 0 ? T("Seitenverhältnis entzerren") : T("Seitenverhältnis der Datei"), [&](Timeline& tl) {
        for (int id : changed)
            if (Clip* c = TimelineOps::findClip(tl, id)) {
                const double was = setShape(*c);
                // A reframed clip keeps filling the frame (its zoom and camera path follow the new shape), before a
                // fill given here applies
                Reframe::reshape(*c, was, MediaAspect::ofClip(*c), frame);
                if (fill) c->transform.fill = *fill;
                // Its kill feed copies show the same picture: the same shape, their region placed anew
                const Clip base = *c;
                for (Track& t : tl.video) {
                    if (t.locked) continue;
                    for (Clip& x : t.clips)
                        if ((Reframe::isKeepAlong(x, base) || (!x.keepRegion.isEmpty() && base.linkId && x.linkId == base.linkId))
                            && std::abs(x.pictureAspect - aspect) > 1e-9) {
                            const double before = setShape(x);
                            Reframe::reshapeKeep(x, before, MediaAspect::ofClip(x), frame);
                        }
                }
            }
    });
    return int(changed.size());
}
