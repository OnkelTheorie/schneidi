// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kinds of package B (0.19): mask, matte
//
// mask: a shape (core/Masks: rect, ellipse, polygon, bezier, linear/radial gradient) as a Mask plane; soft edge in
// project pixels, invert, rotate (clockwise degrees); combined with the mask at its own `mask` input (add, subtract,
// intersect); `follow` = id of a tracker node: the shape moves by the tracker's (x, y) - (rx, ry) and turns/scales
// around (x, y) with its angle/scale (follow rule, core/CompTrack.h); a four-corner tracker (mode quad, 0.22) carries
// it in perspective: every point through the homography from its start corners to the corners found (quadMap), so a
// polygon drawn on a wall stays on it while the camera turns.
// matte: a Mask plane from the luma (of the premultiplied color) or the alpha of the picture at `src`.
#include "engine/picture/CompNodes.h"

#include "core/CompTrack.h"
#include "core/I18n.h"
#include "core/Masks.h"
#include "engine/MaskRaster.h"
#include "engine/video/Compose.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Picture::Comp {

namespace {

EffectParam number(const QString& key, const QString& label, double def, double lo, double hi, double step,
                   int decimals, bool sparse = false)
{
    EffectParam p;
    p.key = key;
    p.label = label;
    p.type = EffectParam::Double;
    p.defaultValue = def;
    p.min = lo;
    p.max = hi;
    p.step = step;
    p.decimals = decimals;
    p.sparse = sparse;
    return p;
}

EffectParam flag(const QString& key, const QString& label)
{
    EffectParam p;
    p.key = key;
    p.label = label;
    p.type = EffectParam::Bool;
    p.defaultValue = false;
    return p;
}

EffectParam choice(const QString& key, const QString& label, const QStringList& values, const QStringList& labels)
{
    EffectParam p;
    p.key = key;
    p.label = label;
    p.type = EffectParam::Choice;
    p.choices = values;
    p.choiceLabels = labels;
    p.defaultValue = values.first();
    return p;
}

// Default corners: a rectangle in the middle (polygon), p0 -> p1 left to right (gradients)
QPointF defaultPoint(int i)
{
    static constexpr double kX[] = {0.3, 0.7, 0.7, 0.3}, kY[] = {0.3, 0.3, 0.7, 0.7};
    return i < 4 ? QPointF(kX[i], kY[i]) : QPointF(0.5, 0.5);
}

} // namespace

bool quadMap(const std::function<double(const QString&)>& value, double W, double H, Compose::Homography* out)
{
    double from[8], to[8];
    for (int i = 0; i < 8; ++i) {
        const double scale = i % 2 ? H : W;
        from[i] = value(QString::fromLatin1(::Comp::kQuadStartKeys[i])) * scale;
        to[i] = value(QString::fromLatin1(::Comp::kCornerKeys[i])) * scale;
        if (!std::isfinite(from[i]) || !std::isfinite(to[i])) return false;
    }
    return Compose::homography(from, to, out);
}

namespace {

QRect fullArea(const Buf& b) { return QRect(0, 0, b.w, b.h); }

Buf maskBuf(NodeCtx& ctx, int W, int H)
{
    Buf b = ctx.take ? ctx.take(Buf::Mask) : Buf();
    if (b.w != W || b.h != H || b.kind != Buf::Mask || b.isNull()) b = Buf(Buf::Mask, W, H);
    return b;
}

bool evalMask(NodeCtx& ctx, Buf* out)
{
    using Masks::Shape;
    const int W = ctx.size.width(), H = ctx.size.height();
    if (W <= 0 || H <= 0) return false;
    Masks::Spec s;
    Masks::shapeFromId(ctx.text("shape"), &s.shape);
    s.center = QPointF(ctx.num("x"), ctx.num("y"));
    s.width = ctx.num("w");
    s.height = ctx.num("h");
    s.rotation = ctx.num("rotate");
    s.soft = ctx.num("soft") * ctx.scale;
    s.invert = ctx.num("invert") != 0;
    if (s.shape != Shape::Rect && s.shape != Shape::Ellipse) {
        const bool gradient = s.shape == Shape::Linear || s.shape == Shape::Radial;
        const int n = gradient ? 2 : std::clamp(int(ctx.num("points")), 0, Masks::kMaxPoints);
        for (int i = 0; i < n; ++i) {
            const QString p = QStringLiteral("p%1").arg(i), h = QStringLiteral("h%1").arg(i);
            s.points.append(QPointF(ctx.num(p + 'x'), ctx.num(p + 'y')));
            if (s.shape == Shape::Bezier) {
                s.handlesOut.append(QPointF(ctx.num(h + 'x'), ctx.num(h + 'y')));
                s.handlesIn.append(QPointF(ctx.num(h + "ix"), ctx.num(h + "iy")));
            }
        }
    }
    Masks::Geometry g = Masks::toPixels(s, ctx.size);
    double seen = 1; // the tracker's visibility (core/CompTrack.h): a lost point fades its follower out
    if (const int follow = ::Comp::followId(ctx.num("follow")); follow > 0 && ctx.numOf) {
        const double v = ctx.numOf(follow, ::Comp::kSeenKey);
        seen = std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 1.0;
        Compose::Homography h;
        if (ctx.numOf(follow, ::Comp::kQuadKey) > 0.5) { // four corners (0.22): in perspective, start corners -> found
            if (quadMap([&](const QString& key) { return ctx.numOf(follow, key); }, W, H, &h))
                g = Masks::mapped(g, [&h](QPointF p, QPointF* out) {
                    double x = 0, y = 0;
                    if (!Compose::mapPoint(h, p.x(), p.y(), &x, &y)) return false;
                    *out = QPointF(x, y);
                    return true;
                });
            else
                g.empty = true; // the corners make no quad (crossed, folded): nothing, like a corner pin
        } else {
            Masks::Follow f;
            // The follow rule (core/CompTrack.h): moved by (x, y) - (rx, ry), scaled and turned around (x, y)
            f.from = QPointF(ctx.numOf(follow, "rx") * W, ctx.numOf(follow, "ry") * H);
            f.to = QPointF(ctx.numOf(follow, "x") * W, ctx.numOf(follow, "y") * H);
            f.scale = ctx.numOf(follow, "scale");
            f.angle = ctx.numOf(follow, "angle");
            g = Masks::followed(g, f);
        }
    }
    Buf b = maskBuf(ctx, W, H);
    QRect area = MaskRaster::render(g, W, H, b.data());
    if (s.invert) {
        MaskRaster::invert(b.data(), W, H);
        area = fullArea(b);
    }
    if (seen < 1) { // this mask's own share fades (what comes in at "mask" stays)
        const uint32_t k = uint32_t(std::lround(seen * 256));
        if (k == 0) {
            std::memset(b.data(), 0, b.px.size());
            area = QRect();
        } else {
            for (uint8_t& v : b.px) v = uint8_t((v * k + 128) >> 8);
        }
    }
    if (const Buf* in = ctx.input ? ctx.input("mask") : nullptr;
        in && in->kind == Buf::Mask && in->w == W && in->h == H) {
        Masks::Combine mode = Masks::Combine::Add;
        Masks::combineFromId(ctx.text("combine"), &mode);
        area = MaskRaster::combine(b.data(), area, in->data(), in->contentArea(), W, H, mode);
    }
    // Nothing inside: a valid, empty mask (not "no mask", which would let the effect act everywhere)
    b.area = area.isEmpty() ? QRect(0, 0, 1, 1) : area;
    *out = std::move(b);
    return true;
}

bool evalMatte(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input ? ctx.input("src") : nullptr;
    const int W = ctx.size.width(), H = ctx.size.height();
    if (!src || src->w != W || src->h != H) return false;
    const bool alpha = ctx.text("channel") == QLatin1String("alpha");
    const bool inv = ctx.num("invert") != 0;
    Buf b = maskBuf(ctx, W, H);
    // Outside the source's content area the source is 0 / transparent: the plane stays 0 there (255 inverted)
    const QRect c = src->contentArea() & fullArea(b);
    const QRect area = inv ? fullArea(b) : c;
    if (inv) std::memset(b.data(), 255, b.px.size());
    const int x0 = c.left(), cw = c.width();
    Compose::forRows(c.height(), 64, [&](int ya, int yb) {
        for (int y = c.top() + ya; y < c.top() + yb; ++y) {
            uint8_t* d = b.data() + size_t(y) * W + x0;
            if (src->kind == Buf::Mask) {
                const uint8_t* s = src->data() + size_t(y) * W + x0;
                for (int i = 0; i < cw; ++i) d[i] = inv ? uint8_t(255 - s[i]) : s[i];
                continue;
            }
            const uint8_t* s = src->data() + (size_t(y) * W + x0) * 4;
            for (int i = 0; i < cw; ++i, s += 4) {
                unsigned v;
                if (alpha) {
                    v = s[3];
                } else { // Rec.709 luma (weights in 1/256) of the premultiplied color: transparent = 0
                    const unsigned l = (54u * s[0] + 183u * s[1] + 19u * s[2] + 128u) >> 8;
                    v = (l * s[3] + 127u) / 255u;
                }
                d[i] = uint8_t(inv ? 255 - v : v);
            }
        }
    });
    b.area = area.isEmpty() ? QRect(0, 0, 1, 1) : area;
    *out = std::move(b);
    return true;
}

} // namespace

void registerMaskNodes(QVector<NodeType>& out)
{
    {
        NodeType k;
        k.id = "mask";
        k.name = T("Maske");
        k.description = T("Form als Maske: Rechteck, Ellipse, Polygon, Kurve oder Verlauf; weiche Kante, umkehren, "
                          "mit der Maske am Eingang verbinden, einem Tracker folgen");
        k.inputs = {"mask"};
        k.output = Buf::Mask;
        // Following a tracker (numOf) the engine caches it only while the tracker's values are fixed
        k.varying = false;
        k.params.append(choice("shape", T("Form"), {"rect", "ellipse", "polygon", "bezier", "linear", "radial"},
                               {T("Rechteck"), T("Ellipse"), T("Polygon"), T("Kurve"), T("Linearer Verlauf"),
                                T("Radialer Verlauf")}));
        k.params.append(number("x", T("Position X"), 0.5, -1, 2, 0.001, 3));
        k.params.append(number("y", T("Position Y"), 0.5, -1, 2, 0.001, 3));
        k.params.append(number("w", T("Breite"), 0.4, 0, 3, 0.001, 3));
        k.params.append(number("h", T("Höhe"), 0.225, 0, 3, 0.001, 3));
        k.params.append(number("rotate", T("Drehung"), 0, -360, 360, 0.1, 1));
        k.params.append(number("soft", T("Weiche Kante"), 0, 0, 500, 0.5, 1));
        k.params.append(flag("invert", T("Umkehren")));
        k.params.append(choice("combine", T("Verbinden"), {"add", "subtract", "intersect"},
                               {T("Hinzufügen"), T("Abziehen"), T("Schnittmenge")}));
        k.params.append(number("follow", T("Folgt Tracker"), 0, 0, 9999, 1, 0));
        k.params.append(number("points", T("Punkte"), 4, 3, Masks::kMaxPoints, 1, 0));
        for (int i = 0; i < Masks::kMaxPoints; ++i) {
            const QString p = QStringLiteral("p%1").arg(i);
            const QPointF d = defaultPoint(i);
            k.params.append(number(p + 'x', T("Punkt %1 X").arg(i + 1), d.x(), -1, 2, 0.001, 3, i >= 4));
            k.params.append(number(p + 'y', T("Punkt %1 Y").arg(i + 1), d.y(), -1, 2, 0.001, 3, i >= 4));
        }
        for (int i = 0; i < Masks::kMaxPoints; ++i) { // bezier handles, offsets from their point
            const QString h = QStringLiteral("h%1").arg(i);
            k.params.append(number(h + 'x', T("Griff %1 X").arg(i + 1), 0, -2, 2, 0.001, 3, true));
            k.params.append(number(h + 'y', T("Griff %1 Y").arg(i + 1), 0, -2, 2, 0.001, 3, true));
            k.params.append(number(h + "ix", T("Griff %1 X (davor)").arg(i + 1), 0, -2, 2, 0.001, 3, true));
            k.params.append(number(h + "iy", T("Griff %1 Y (davor)").arg(i + 1), 0, -2, 2, 0.001, 3, true));
        }
        k.eval = evalMask;
        out.append(k);
    }
    {
        NodeType k;
        k.id = "matte";
        k.name = T("Alphamaske");
        k.description = T("Maske aus der Helligkeit oder dem Alpha eines Bildes (Spurmaske, Text als Fenster)");
        k.inputs = {"src"};
        k.output = Buf::Mask;
        k.params.append(choice("channel", T("Kanal"), {"luma", "alpha"}, {T("Helligkeit"), T("Alpha")}));
        k.params.append(flag("invert", T("Umkehren")));
        k.eval = evalMatte;
        out.append(k);
    }
}

} // namespace Picture::Comp
