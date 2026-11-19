// SPDX-License-Identifier: AGPL-3.0-only
#include "core/OverlayFit.h"

#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Reframe.h"

#include <algorithm>
#include <cmath>

namespace OverlayFit {

namespace {

// A picture overlay's rectangle at zoom/position (crop and rotation left out), pixels y down
QRectF pictureRect(const Clip& c, QSizeF frame)
{
    double aspect = MediaAspect::ofClip(c);
    if (aspect <= 0) aspect = frame.width() / frame.height();
    const QSizeF fit = MediaAspect::fit(aspect, frame);
    const ClipTransform& t = c.transform;
    const double zx = t.transformOn ? t.zoomX : 1, zy = t.transformOn ? t.zoomY : 1;
    const double x = t.transformOn ? t.posX : 0, y = t.transformOn ? t.posY : 0;
    const QSizeF size(fit.width() * std::abs(zx), fit.height() * std::abs(zy));
    const QPointF centre(frame.width() / 2 + x, frame.height() / 2 - y);
    return QRectF(centre.x() - size.width() / 2, centre.y() - size.height() / 2, size.width(), size.height());
}

bool movesOrSizes(const Clip& c)
{
    for (AnimParam p : {AnimParam::PosX, AnimParam::PosY, AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::TitlePosX,
                        AnimParam::TitlePosY, AnimParam::TitleSize, AnimParam::ShapeWidth, AnimParam::ShapeHeight})
        if (Keys::animated(c, p)) return true;
    return false;
}

// The shift (pixels, y down) that brings r into s as little as possible; centred where r is larger than s
QPointF into(const QRectF& r, const QRectF& s)
{
    const auto axis = [](double lo, double hi, double slo, double shi) {
        if (hi - lo > shi - slo) return (slo + shi) / 2 - (lo + hi) / 2;
        if (lo < slo) return slo - lo;
        if (hi > shi) return shi - hi;
        return 0.0;
    };
    return {axis(r.left(), r.right(), s.left(), s.right()), axis(r.top(), r.bottom(), s.top(), s.bottom())};
}

Clip* clipById(Timeline& tl, int id, int* track)
{
    for (int i = 0; i < tl.video.size(); ++i)
        for (Clip& c : tl.video[i].clips)
            if (c.id == id) {
                if (track) *track = i;
                return &c;
            }
    return nullptr;
}

} // namespace

QVector<Before> collect(const Timeline& tl, QSizeF from, const std::function<bool(const Clip&)>& isStill,
                        QVector<int>* animated)
{
    QVector<Before> out;
    for (int i = 0; i < tl.video.size(); ++i)
        for (const Clip& c : tl.video[i].clips) {
            const bool title = c.isTitle() && (c.title.kind == TitleKind::Text || c.title.isCounter() || c.title.isShape())
                               && !(c.title.isShape() && c.title.shape.followClip > 0);
            const bool picture = !title && isStill && isStill(c) && Reframe::isOverlay(c, i, MediaAspect::ofClip(c), from);
            if (!title && !picture) continue;
            if (movesOrSizes(c)) {
                if (animated) *animated << c.id;
                continue;
            }
            Before b;
            b.clip = c.id;
            b.picture = picture;
            b.block = picture ? pictureRect(c, from) : ExportCheck::overlayBlock(c, from.width(), from.height());
            b.fullFrame = title && c.title.isShape() && b.block.contains(QRectF(QPointF(0, 0), from));
            out << b;
        }
    return out;
}

QRectF safeRect(QSizeF frame, const ExportCheck::Options& o)
{
    const double W = frame.width(), H = frame.height();
    double left = W * 0.05, right = W * 0.95, top = H * 0.05, bottom = H * 0.95;
    if (ExportCheck::isVertical(ProjectFormat{int(W), int(H)})) {
        top = std::max(top, H * o.safeTop);
        bottom = std::min(bottom, H * (1 - o.safeBottom));
        right = std::min(right, W * (1 - o.safeRight)); // the buttons' strip (all the way: simple and safe)
    }
    return QRectF(left, top, right - left, bottom - top);
}

QVector<int> apply(Timeline& tl, const QVector<Before>& before, QSizeF from, QSizeF to, const ExportCheck::Options& o)
{
    QVector<int> changed;
    if (from.isEmpty() || to.isEmpty()) return changed;
    const double s = std::min(to.width(), to.height()) / std::min(from.width(), from.height());
    // 1. Full-frame shapes fill the new frame; picture overlays get their size by the short side
    struct Item {
        int clip;
        QRectF block; // in the new frame, now
        int start, end;
    };
    QVector<Item> items;
    for (const Before& b : before) {
        int track = 0;
        Clip* c = clipById(tl, b.clip, &track);
        if (!c) continue;
        if (b.fullFrame) {
            // As far over each edge as before (a matte 2 px over each edge stays so)
            ShapeStyle& sh = c->title.shape;
            sh.width = to.width() + (b.block.width() - from.width());
            sh.height = to.height() + (b.block.height() - from.height());
            c->title.posX = c->title.posY = 0;
            c->transform.posX = c->transform.posY = 0;
            changed << c->id;
            continue;
        }
        if (b.picture && c->transform.transformOn) {
            const QRectF now = pictureRect(*c, to);
            if (now.width() > 0 && now.height() > 0) {
                const double k = b.block.width() * s / now.width();
                if (std::abs(k - 1) > 1e-3) {
                    c->transform.zoomX *= k;
                    c->transform.zoomY *= k;
                    changed << c->id;
                }
            }
        }
        const QRectF block = b.picture ? pictureRect(*c, to) : ExportCheck::overlayBlock(*c, to.width(), to.height());
        items << Item{c->id, block, c->start, c->end()};
    }
    // 2. Groups: overlays on screen together whose blocks touch (a logo on its plate, a text on its box) move as one
    QVector<int> group(items.size());
    for (int i = 0; i < items.size(); ++i) group[i] = i;
    const auto root = [&](int i) {
        while (group[i] != i) i = group[i] = group[group[i]];
        return i;
    };
    for (int i = 0; i < items.size(); ++i)
        for (int j = i + 1; j < items.size(); ++j)
            if (items[i].start < items[j].end && items[j].start < items[i].end && items[i].block.intersects(items[j].block))
                group[root(i)] = root(j);
    const QRectF safe = safeRect(to, o);
    for (int g = 0; g < items.size(); ++g) {
        if (root(g) != g) continue;
        QRectF all;
        for (int i = 0; i < items.size(); ++i)
            if (root(i) == g) all = all.isNull() ? items[i].block : all.united(items[i].block);
        const QPointF d = into(all, safe);
        if (std::abs(d.x()) < 0.5 && std::abs(d.y()) < 0.5) continue;
        for (int i = 0; i < items.size(); ++i) {
            if (root(i) != g) continue;
            Clip* c = clipById(tl, items[i].clip, nullptr);
            // Titles move by their own position (inside the clip's zoom), pictures by the transform's (y up)
            if (c->isTitle()) {
                const bool tr = Keys::hasTransform(*c);
                const double zx = tr ? c->transform.zoomX : 1, zy = tr ? c->transform.zoomY : 1;
                c->title.posX += d.x() / (zx != 0 ? zx : 1);
                c->title.posY -= d.y() / (zy != 0 ? zy : 1);
            } else {
                c->transform.posX += d.x();
                c->transform.posY -= d.y();
            }
            if (!changed.contains(c->id)) changed << c->id;
        }
    }
    return changed;
}

} // namespace OverlayFit
