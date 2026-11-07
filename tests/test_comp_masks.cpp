// SPDX-License-Identifier: AGPL-3.0-only
// Mask nodes of the compositing graph (0.19, package B) through the single-node runner: every shape inside/outside/
// edge, soft edge falling monotonically, invert, combine (add/subtract/intersect), follow a tracker, polygon
// self-intersection and 32 corners, degenerate shapes = empty mask, rect/ellipse equal to the Color page window
// (ColorGrade::applyWindow) within one code value, matte from luma/alpha; time per shape in 1080p.
#include "render_check.h"

#include "core/Masks.h"
#include "engine/ColorGrade.h"
#include "engine/MaskRaster.h"
#include "engine/picture/CompNodes.h"

#include <QElapsedTimer>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Picture::Comp::Buf;
using Picture::Comp::RunOptions;

Buf run(const QVariantMap& p, QSize size = {200, 100}, const QHash<QString, Buf>& in = {}, RunOptions o = {})
{
    o.size = size;
    Buf b;
    if (!Picture::Comp::runNode("mask", p, in, o, &b)) return {};
    return b;
}

int at(const Buf& b, int x, int y) { return b.isNull() ? -1 : b.px[size_t(y) * b.w + x]; }

int sum(const Buf& b)
{
    long s = 0;
    for (uint8_t v : b.px) s += v;
    return int(std::min<long>(s, 1L << 30));
}

QVariantMap rect(double x, double y, double w, double h)
{
    return {{"shape", "rect"}, {"x", x}, {"y", y}, {"w", w}, {"h", h}};
}

QVariantMap points(const QString& shape, const QVector<QPointF>& pts)
{
    QVariantMap p{{"shape", shape}, {"points", int(pts.size())}};
    for (int i = 0; i < pts.size(); ++i) {
        p[QStringLiteral("p%1x").arg(i)] = pts[i].x();
        p[QStringLiteral("p%1y").arg(i)] = pts[i].y();
    }
    return p;
}

void testKinds()
{
    const auto* m = Picture::Comp::find("mask");
    const auto* t = Picture::Comp::find("matte");
    if (!CHECK(m && t)) return;
    CHECK(m->output == Buf::Mask && t->output == Buf::Mask);
    CHECK_EQ(m->inputs, QStringList{"mask"});
    CHECK_EQ(t->inputs, QStringList{"src"});
    CHECK(m->param("p31x") && m->param("h31iy") && !m->param("p32x"));
    // A fresh mask: a rectangle in the middle, hard edge, not inverted
    const Buf b = run({});
    CHECK_EQ(at(b, 100, 50), 255);
    CHECK_EQ(at(b, 5, 5), 0);
}

void testRectEllipse()
{
    // 200 x 100: rect 0.5 x 0.3 of the width = 100 x 60 px around (100, 50) -> x 50..150, y 20..80
    Buf b = run(rect(0.5, 0.5, 0.5, 0.3));
    CHECK_EQ(at(b, 100, 50), 255);
    CHECK_EQ(at(b, 51, 21), 255);
    CHECK_EQ(at(b, 148, 78), 255);
    CHECK_EQ(at(b, 30, 50), 0);
    CHECK_EQ(at(b, 100, 5), 0);
    CHECK(at(b, 150, 50) > 0 && at(b, 150, 50) < 255); // one-pixel anti-aliased edge
    CHECK(b.area.contains(50, 20) && !b.area.contains(5, 5));

    QVariantMap e = rect(0.5, 0.5, 0.5, 0.3);
    e["shape"] = "ellipse";
    b = run(e);
    CHECK_EQ(at(b, 100, 50), 255);
    CHECK_EQ(at(b, 52, 22), 0); // corner of the box is outside the ellipse
    CHECK_EQ(at(b, 148, 50), 255);

    // Rotated by 90 degrees: the wide rect becomes tall
    QVariantMap r = rect(0.5, 0.5, 0.5, 0.1);
    r["rotate"] = 90;
    b = run(r);
    CHECK_EQ(at(b, 100, 10), 255);
    CHECK_EQ(at(b, 140, 50), 0);
}

void testSoftAndInvert()
{
    QVariantMap p = rect(0.5, 0.5, 0.3, 0.2);
    p["soft"] = 15;
    const Buf b = run(p);
    // Along the row through the middle: 255 inside, then falling without ever rising to 0
    bool mono = true;
    int prev = 255, seen = 0;
    for (int x = 100; x < 200; ++x) {
        const int v = at(b, x, 50);
        mono = mono && v <= prev;
        if (v > 0 && v < 255) ++seen;
        prev = v;
    }
    CHECK(mono);
    CHECK(seen >= 10); // a soft edge, not a step
    CHECK_EQ(at(b, 199, 50), 0);
    // Soft edge in project pixels: twice the output scale = twice as wide
    RunOptions o;
    o.scale = 2;
    const Buf b2 = run(p, {200, 100}, {}, o);
    CHECK(at(b2, 140, 50) > at(b, 140, 50));

    p["invert"] = true;
    const Buf inv = run(p);
    bool ok = !inv.isNull();
    for (size_t i = 0; ok && i < inv.px.size(); ++i) ok = inv.px[i] == 255 - b.px[i];
    CHECK(ok);
    CHECK_EQ(inv.contentArea(), QRect(0, 0, 200, 100));
}

void testPolygonBezier()
{
    // Triangle (0.1, 0.9) (0.5, 0.1) (0.9, 0.9)
    Buf b = run(points("polygon", {{0.1, 0.9}, {0.5, 0.1}, {0.9, 0.9}}));
    CHECK_EQ(at(b, 100, 60), 255);
    CHECK_EQ(at(b, 30, 20), 0);
    CHECK_EQ(at(b, 170, 20), 0);
    // Edge pixel: between
    int edge = 0;
    for (int x = 0; x < 100; ++x)
        if (at(b, x, 50) > 0 && at(b, x, 50) < 255) ++edge;
    CHECK(edge >= 1 && edge <= 3);
    // Soft edge monotonic across the slanted left side
    QVariantMap p = points("polygon", {{0.1, 0.9}, {0.5, 0.1}, {0.9, 0.9}});
    p["soft"] = 10;
    b = run(p);
    bool mono = true;
    for (int x = 99; x > 0; --x) mono = mono && at(b, x - 1, 50) <= at(b, x, 50);
    CHECK(mono);

    // Bow-tie (self-intersecting): no crash, both wings inside, outside the crossing's sides empty
    b = run(points("polygon", {{0.1, 0.1}, {0.9, 0.9}, {0.9, 0.1}, {0.1, 0.9}}));
    CHECK_EQ(at(b, 40, 50), 255);
    CHECK_EQ(at(b, 160, 50), 255);
    CHECK_EQ(at(b, 100, 15), 0);

    // 32 corners: a star around the middle
    QVector<QPointF> star;
    for (int i = 0; i < Masks::kMaxPoints; ++i) {
        const double a = i * 2 * M_PI / Masks::kMaxPoints, r = i % 2 ? 0.2 : 0.4;
        star.append({0.5 + r * std::cos(a) * 0.5, 0.5 + r * std::sin(a)});
    }
    b = run(points("polygon", star));
    CHECK_EQ(at(b, 100, 50), 255);
    CHECK_EQ(at(b, 100 + 30, 50), 255); // inside the tip at angle 0 (reaches x 100 + 0.4 * 0.5 * 200 = 140)
    CHECK_EQ(at(b, 100 + 30, 50 + 8), 0); // beside it: between two tips
    CHECK_EQ(at(b, 2, 2), 0);
    // More than 32 asked: only 32 used, no crash
    QVariantMap many = points("polygon", star);
    many["points"] = 99;
    CHECK(!run(many).isNull());

    // Bezier: a square whose sides bulge out through the handles - a point beyond the square's side is inside
    const QVector<QPointF> sq{{0.3, 0.3}, {0.7, 0.3}, {0.7, 0.7}, {0.3, 0.7}};
    QVariantMap bz = points("bezier", sq);
    CHECK_EQ(at(run(bz), 100, 25), 0); // without handles: the square
    bz["h0x"] = 0.0;
    bz["h0y"] = -0.3; // out of p0 upward
    bz["h1ix"] = 0.0;
    bz["h1iy"] = -0.3; // into p1 from above
    b = run(bz);
    CHECK_EQ(at(b, 100, 25), 255);
    CHECK_EQ(at(b, 100, 50), 255);
    CHECK_EQ(at(b, 100, 5), 0);
    CHECK_EQ(at(b, 100, 80), 0);
}

void testGradients()
{
    Buf b = run(points("linear", {{0.25, 0.5}, {0.75, 0.5}}));
    CHECK_EQ(at(b, 10, 50), 255);
    CHECK_EQ(at(b, 190, 50), 0);
    CHECK(std::abs(at(b, 100, 50) - 128) <= 2);
    bool mono = true;
    for (int x = 1; x < 200; ++x) mono = mono && at(b, x, 50) <= at(b, x - 1, 50);
    CHECK(mono);
    CHECK_EQ(at(b, 100, 0), at(b, 100, 99)); // constant across the direction

    b = run(points("radial", {{0.5, 0.5}, {0.75, 0.5}})); // radius 50 px
    CHECK(at(b, 100, 50) >= 250);
    CHECK(std::abs(at(b, 125, 50) - 128) <= 3);
    CHECK_EQ(at(b, 160, 50), 0);
    CHECK_EQ(at(b, 100, 25), at(b, 75, 50)); // round: same distance, same value
}

void testDegenerate()
{
    for (const QVariantMap& p : {rect(0.5, 0.5, 0, 0.2), rect(0.5, 0.5, 0.2, 0),
                                 points("polygon", {{0.5, 0.5}, {0.5, 0.5}, {0.5, 0.5}}),
                                 points("polygon", {{0.1, 0.1}, {0.5, 0.5}, {0.9, 0.9}}), // on a line
                                 points("bezier", {{0.5, 0.5}, {0.5, 0.5}, {0.5, 0.5}}),
                                 points("linear", {{0.5, 0.5}, {0.5, 0.5}}),
                                 points("radial", {{0.5, 0.5}, {0.5, 0.5}})}) {
        QVariantMap q = p;
        q["shape"] = q.value("shape", "rect");
        const Buf b = run(q);
        CHECK(!b.isNull()); // a mask that hides everything, not "no mask"
        CHECK_EQ(sum(b), 0);
        q["invert"] = true;
        const Buf inv = run(q);
        CHECK_EQ(sum(inv), 255 * 200 * 100);
    }
    QVariantMap e = rect(0.5, 0.5, 0, 0);
    e["shape"] = "ellipse";
    CHECK_EQ(sum(run(e)), 0);
    // Zero output size: nothing
    CHECK(run(rect(0.5, 0.5, 0.2, 0.2), {0, 0}).isNull());
}

void testCombine()
{
    const Buf left = run(rect(0.3, 0.5, 0.4, 0.3));  // x 20..100
    QHash<QString, Buf> in{{"mask", left}};
    const auto with = [&](const QString& mode) {
        QVariantMap p = rect(0.7, 0.5, 0.4, 0.3);
        p["combine"] = mode;
        return run(p, {200, 100}, in);
    };
    Buf b = with("add");
    CHECK_EQ(at(b, 40, 50), 255);
    CHECK_EQ(at(b, 160, 50), 255);
    CHECK_EQ(at(b, 5, 50), 0);
    b = with("subtract");
    CHECK_EQ(at(b, 40, 50), 255);
    CHECK_EQ(at(b, 160, 50), 0);
    CHECK_EQ(at(b, 102, 50), 0);
    b = with("intersect");
    CHECK_EQ(at(b, 40, 50), 0);
    CHECK_EQ(at(b, 160, 50), 0);
    QVariantMap mid = rect(0.5, 0.5, 0.4, 0.3); // x 60..140 with left: 60..100
    mid["combine"] = "intersect";
    b = run(mid, {200, 100}, in);
    CHECK_EQ(at(b, 80, 50), 255);
    CHECK_EQ(at(b, 120, 50), 0);
    CHECK_EQ(at(b, 40, 50), 0);
    mid["combine"] = "subtract";
    b = run(mid, {200, 100}, in);
    CHECK_EQ(at(b, 40, 50), 255);
    CHECK_EQ(at(b, 80, 50), 0);
    // Default combine = add: the union of both
    mid.remove("combine");
    b = run(mid, {200, 100}, in);
    CHECK_EQ(at(b, 40, 50), 255);
    CHECK_EQ(at(b, 120, 50), 255);
    CHECK(sum(b) > sum(left));
}

void testFollow()
{
    // Tracker 7 had its reference point at (0.5, 0.5) and is now at (0.7, 0.4), twice the size, turned by 90 degrees.
    // The follow rule: relative to the reference rx/ry, not to the start point A (two points: A is not the middle)
    RunOptions o;
    o.otherNodes[7] = {{"ax", 0.2}, {"ay", 0.3}, {"rx", 0.5}, {"ry", 0.5}, {"x", 0.7}, {"y", 0.4}, {"scale", 1.0}, {"angle", 0.0}};
    QVariantMap p = rect(0.5, 0.5, 0.1, 0.1); // 20 x 20 px around (100, 50)
    p["follow"] = 7;
    Buf b = run(p, {200, 100}, {}, o);
    CHECK_EQ(at(b, 140, 40), 255);
    CHECK_EQ(at(b, 100, 50), 0);
    o.otherNodes[7]["scale"] = 2.0;
    b = run(p, {200, 100}, {}, o);
    CHECK_EQ(at(b, 140 + 18, 40), 255);
    // A long rect turned by 90 degrees around the tracked point
    QVariantMap l = rect(0.6, 0.5, 0.2, 0.05); // x 100..140, y 45..55; to the right of the start point
    l["follow"] = 7;
    o.otherNodes[7]["scale"] = 1.0;
    o.otherNodes[7]["angle"] = 90.0;
    b = run(l, {200, 100}, {}, o);
    CHECK_EQ(at(b, 140, 40 + 20), 255); // clockwise: right of the point turns to below it
    CHECK_EQ(at(b, 160, 40), 0);
    // Polygons follow too
    QVariantMap tri = points("polygon", {{0.45, 0.45}, {0.55, 0.45}, {0.5, 0.55}});
    tri["follow"] = 7;
    o.otherNodes[7]["angle"] = 0.0;
    b = run(tri, {200, 100}, {}, o);
    CHECK_EQ(at(b, 140, 40), 255);
    CHECK_EQ(at(b, 100, 48), 0);
    // A missing tracker (no values): the shape stays where it is
    p["follow"] = 99;
    b = run(p);
    CHECK_EQ(at(b, 100, 50), 255);
}

// The Color page window with a white color fill on black: its pixels are the window's mask
int windowDiff(const Masks::Geometry& g, QSize size, const Buf& b)
{
    ColorGrade::Window w;
    w.geometry = Masks::toWindow(g, size);
    w.fill.fill = GradeWindows::Fill::Color;
    w.fill.color = Qt::white;
    std::vector<uint8_t> rgba(size_t(size.width()) * size.height() * 4, 0);
    ColorGrade::applyWindow(rgba.data(), size.width(), size.height(), w);
    int worst = 0;
    for (size_t i = 0; i < b.px.size(); ++i) worst = std::max(worst, std::abs(int(rgba[i * 4]) - int(b.px[i])));
    return worst;
}

void testEqualsWindow()
{
    const QSize size(320, 180);
    struct Case {
        const char* shape;
        double x, y, w, h, rot, soft;
    };
    for (const Case& c : {Case{"rect", 0.5, 0.5, 0.4, 0.2, 0, 0}, Case{"ellipse", 0.5, 0.5, 0.4, 0.2, 0, 0},
                          Case{"rect", 0.43, 0.61, 0.33, 0.21, 30, 12}, Case{"ellipse", 0.37, 0.4, 0.5, 0.3, -50, 20},
                          Case{"ellipse", 0.9, 0.1, 0.3, 0.3, 10, 4}, Case{"rect", 0.5, 0.5, 1.2, 1.0, 5, 3}}) {
        QVariantMap p{{"shape", c.shape}, {"x", c.x}, {"y", c.y}, {"w", c.w}, {"h", c.h}, {"rotate", c.rot},
                      {"soft", c.soft}};
        const Buf b = run(p, size);
        Masks::Spec s;
        Masks::shapeFromId(c.shape, &s.shape);
        s.center = {c.x, c.y};
        s.width = c.w;
        s.height = c.h;
        s.rotation = c.rot;
        s.soft = c.soft;
        const int d = windowDiff(Masks::toPixels(s, size), size, b);
        Check::report(d <= 1, "mask == window", __FILE__, __LINE__, QStringLiteral("%1: %2").arg(c.shape).arg(d));
    }
}

void testMatte()
{
    Buf src(Buf::Rgba, 4, 1);
    const uint8_t px[] = {255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255, 128};
    std::memcpy(src.data(), px, sizeof(px));
    RunOptions o;
    o.size = {4, 1};
    Buf b;
    CHECK(Picture::Comp::runNode("matte", {}, {{"src", src}}, o, &b));
    CHECK_EQ(at(b, 0, 0), 255);
    CHECK_EQ(at(b, 1, 0), 0); // white but transparent
    CHECK_EQ(at(b, 2, 0), 0);
    CHECK(std::abs(at(b, 3, 0) - 128) <= 1);
    CHECK(Picture::Comp::runNode("matte", {{"channel", "alpha"}, {"invert", true}}, {{"src", src}}, o, &b));
    CHECK_EQ(at(b, 0, 0), 0);
    CHECK_EQ(at(b, 1, 0), 255);
    CHECK_EQ(at(b, 2, 0), 0);
    // A mask at src passes through; nothing at src = nothing
    Buf m(Buf::Mask, 4, 1);
    m.px = {0, 10, 200, 255};
    CHECK(Picture::Comp::runNode("matte", {}, {{"src", m}}, o, &b));
    CHECK(b.px == m.px);
    CHECK(!Picture::Comp::runNode("matte", {}, {}, o, &b));
}

void testTiming()
{
    const QSize size(1920, 1080);
    QVector<QPointF> star;
    for (int i = 0; i < Masks::kMaxPoints; ++i) {
        const double a = i * 2 * M_PI / Masks::kMaxPoints, r = i % 2 ? 0.2 : 0.4;
        star.append({0.5 + r * std::cos(a) * 0.5625, 0.5 + r * std::sin(a)});
    }
    QVariantMap bz = points("bezier", {{0.3, 0.3}, {0.7, 0.3}, {0.7, 0.7}, {0.3, 0.7}});
    for (int i = 0; i < 4; ++i) {
        bz[QStringLiteral("h%1y").arg(i)] = i < 2 ? -0.2 : 0.2;
    }
    struct Case {
        const char* name;
        QVariantMap p;
    };
    QVariantMap ell = rect(0.5, 0.5, 0.4, 0.225);
    ell["shape"] = "ellipse";
    QVariantMap ellSoft = ell;
    ellSoft["soft"] = 40;
    ellSoft["rotate"] = 20;
    QVariantMap poly = points("polygon", star);
    QVariantMap polySoft = poly;
    polySoft["soft"] = 40;
    const Case cases[] = {{"rect", rect(0.5, 0.5, 0.4, 0.225)},
                          {"ellipse", ell},
                          {"ellipse soft 40 rot", ellSoft},
                          {"polygon 32", poly},
                          {"polygon 32 soft 40", polySoft},
                          {"bezier", bz},
                          {"linear", points("linear", {{0.2, 0.2}, {0.8, 0.8}})},
                          {"radial", points("radial", {{0.5, 0.5}, {0.8, 0.5}})}};
    const bool show = std::getenv("SCHNEIDI_TIMING") != nullptr;
    for (const Case& c : cases) {
        const auto* k = Picture::Comp::find("mask");
        Masks::Spec s; // same shape for the single-thread number, straight through MaskRaster
        Masks::shapeFromId(c.p.value("shape").toString(), &s.shape);
        s.center = {Picture::Comp::numValue(*k, c.p, "x"), Picture::Comp::numValue(*k, c.p, "y")};
        s.width = Picture::Comp::numValue(*k, c.p, "w");
        s.height = Picture::Comp::numValue(*k, c.p, "h");
        s.rotation = Picture::Comp::numValue(*k, c.p, "rotate");
        s.soft = Picture::Comp::numValue(*k, c.p, "soft");
        const int n = int(Picture::Comp::numValue(*k, c.p, "points"));
        const bool grad = s.shape == Masks::Shape::Linear || s.shape == Masks::Shape::Radial;
        for (int i = 0; i < (grad ? 2 : n); ++i) {
            s.points.append({Picture::Comp::numValue(*k, c.p, QStringLiteral("p%1x").arg(i)),
                             Picture::Comp::numValue(*k, c.p, QStringLiteral("p%1y").arg(i))});
            s.handlesOut.append({Picture::Comp::numValue(*k, c.p, QStringLiteral("h%1x").arg(i)),
                                 Picture::Comp::numValue(*k, c.p, QStringLiteral("h%1y").arg(i))});
        }
        const Masks::Geometry g = Masks::toPixels(s, size);
        std::vector<uint8_t> plane(size_t(size.width()) * size.height());
        const int reps = 10;
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < reps; ++i) {
            std::fill(plane.begin(), plane.end(), 0);
            MaskRaster::render(g, size.width(), size.height(), plane.data(), false);
        }
        const double single = t.nsecsElapsed() / 1e6 / reps;
        t.restart();
        for (int i = 0; i < reps; ++i) run(c.p, size);
        const double node = t.nsecsElapsed() / 1e6 / reps;
        if (show) std::printf("mask %-22s 1080p: %6.2f ms single thread, %6.2f ms node (forRows)\n", c.name, single, node);
        CHECK(node < 200); // generous: only catches a runaway (debug build, busy machine)
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    testKinds();
    testRectEllipse();
    testSoftAndInvert();
    testPolygonBezier();
    testGradients();
    testDegenerate();
    testCombine();
    testFollow();
    testEqualsWindow();
    testMatte();
    testTiming();
    return Check::result();
}
