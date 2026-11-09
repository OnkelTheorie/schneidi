// SPDX-License-Identifier: AGPL-3.0-only
// Tilting a picture in depth (0.20, engine/video/Compose homography/tilted/draw and the comp node "transform"):
// - homography(): the four corners land exactly where asked; a degenerate quad is refused
// - tilted() with no tilt = the flat placement; tilt Y positive = the right side goes away (shorter, nearer the
//   middle), tilt X positive = the top goes away (narrower); a corner behind the camera = false
// - draw() through a homography that is affine = the affine draw (same sampling)
// - node "transform": tilt 0 = exactly the old picture; tilted = a trapezoid (opaque middle, transparent corners)
#include "render_check.h"

#include "engine/picture/CompNodes.h"
#include "engine/video/Compose.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

using Picture::Comp::Buf;
using Picture::Comp::RunOptions;
using Picture::Comp::runNode;

namespace {

const uint8_t* px(const Buf& b, int x, int y) { return b.data() + (size_t(y) * b.w + x) * 4; }

void map(const Compose::Homography& h, double x, double y, double* X, double* Y)
{
    const double* m = h.m;
    const double w = m[6] * x + m[7] * y + m[8];
    *X = (m[0] * x + m[1] * y + m[2]) / w;
    *Y = (m[3] * x + m[4] * y + m[5]) / w;
}

// A picture with a pattern (every pixel different enough to see sampling differences), opaque
Buf pattern(int w, int h)
{
    Buf b(Buf::Rgba, w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = b.data() + (size_t(y) * w + x) * 4;
            p[0] = uint8_t(x * 7), p[1] = uint8_t(y * 5), p[2] = uint8_t((x + y) * 3), p[3] = 255;
        }
    return b;
}

RunOptions sized(int w, int h)
{
    RunOptions o;
    o.size = QSize(w, h);
    return o;
}

void testHomography()
{
    const double to[8] = {10, 20, 300, 5, 280, 190, 30, 170};
    Compose::Homography h;
    if (!CHECK(Compose::homography(320, 180, to, &h))) return;
    const double from[8] = {0, 0, 320, 0, 320, 180, 0, 180};
    for (int i = 0; i < 4; ++i) {
        double X = 0, Y = 0;
        map(h, from[2 * i], from[2 * i + 1], &X, &Y);
        CHECK(std::abs(X - to[2 * i]) < 1e-6 && std::abs(Y - to[2 * i + 1]) < 1e-6);
    }
    const double line[8] = {0, 0, 100, 0, 200, 0, 300, 0}; // all on one line
    CHECK(!Compose::homography(320, 180, line, &h));
}

void testTilted()
{
    const int W = 640, H = 360;
    Compose::Affine flat; // the picture fills the frame
    Compose::Homography h;
    // No tilt: the corners stay where the flat placement put them
    if (CHECK(Compose::tilted(flat, W, H, W / 2.0, H / 2.0, 0, 0, 40, W, H, &h))) {
        double X = 0, Y = 0;
        map(h, W, H, &X, &Y);
        CHECK(std::abs(X - W) < 1e-6 && std::abs(Y - H) < 1e-6);
    }
    // Tilt Y +40: the right edge goes away -> shorter than the left one, nearer the middle
    if (CHECK(Compose::tilted(flat, W, H, W / 2.0, H / 2.0, 0, 40, 40, W, H, &h))) {
        double lx, ly0, ly1, rx, ry0, ry1;
        map(h, 0, 0, &lx, &ly0);
        map(h, 0, H, &lx, &ly1);
        map(h, W, 0, &rx, &ry0);
        map(h, W, H, &rx, &ry1);
        CHECK(ry1 - ry0 < ly1 - ly0);
        CHECK(W - rx > lx); // the far side moved further in than the near one out
    }
    // Tilt X +40: the top goes away -> narrower than the bottom
    if (CHECK(Compose::tilted(flat, W, H, W / 2.0, H / 2.0, 40, 0, 40, W, H, &h))) {
        double tx0, tx1, bx0, bx1, y;
        map(h, 0, 0, &tx0, &y);
        map(h, W, 0, &tx1, &y);
        map(h, 0, H, &bx0, &y);
        map(h, W, H, &bx1, &y);
        CHECK(tx1 - tx0 < bx1 - bx0);
    }
    // Huge and almost edge-on with a wide lens: a corner comes behind the camera -> nothing
    Compose::Affine big;
    big.a = big.e = 20;
    big.c = -W * 9.5, big.f = -H * 9.5;
    CHECK(!Compose::tilted(big, W, H, W / 2.0, H / 2.0, 0, 89, 120, W, H, &h));
}

void testDrawAffineEqual()
{
    const Buf src = pattern(200, 120);
    Compose::Affine m; // shrink to 0.8 and move
    m.a = m.e = 0.8;
    m.c = 30, m.f = 20;
    Compose::Homography h;
    h.m[0] = m.a, h.m[1] = m.b, h.m[2] = m.c, h.m[3] = m.d, h.m[4] = m.e, h.m[5] = m.f;
    Buf a(Buf::Rgba, 240, 160), b(Buf::Rgba, 240, 160);
    Compose::draw(a.data(), a.w, a.h, src.data(), src.w, src.h, m, 1.0);
    Compose::draw(b.data(), b.w, b.h, src.data(), src.w, src.h, h, 1.0);
    int worst = 0;
    for (size_t i = 0; i < a.px.size(); ++i) worst = std::max(worst, std::abs(int(a.px[i]) - int(b.px[i])));
    CHECK_EQ(worst <= 2, true);
}

void testNode()
{
    const int W = 320, H = 180;
    const Buf src = pattern(W, H);
    Buf flat, zero;
    if (!CHECK(runNode("transform", {{"size", 0.6}}, {{"src", src}}, sized(W, H), &flat))) return;
    if (!CHECK(runNode("transform", {{"size", 0.6}, {"tilt_x", 0.0}, {"tilt_y", 0.0}}, {{"src", src}}, sized(W, H),
                       &zero)))
        return;
    CHECK(flat.px == zero.px); // tilt 0 = the old path, bit for bit
    Buf tilted;
    if (!CHECK(runNode("transform", {{"size", 0.8}, {"tilt_y", 50.0}}, {{"src", src}}, sized(W, H), &tilted))) return;
    CHECK(tilted.px != flat.px);
    CHECK_EQ(int(px(tilted, W / 2, H / 2)[3]), 255); // the middle is covered
    CHECK_EQ(int(px(tilted, 0, 0)[3]), 0);           // the corners not
    // The right side (gone away) is shorter: count covered rows in a column near each edge of the picture
    auto covered = [&](int x) {
        int n = 0;
        for (int y = 0; y < H; ++y) n += px(tilted, x, y)[3] > 128;
        return n;
    };
    int left = 0, right = W - 1;
    while (left < W && covered(left) == 0) ++left;
    while (right > 0 && covered(right) == 0) --right;
    CHECK(right > left + 20);
    CHECK(covered(right - 2) < covered(left + 2));
    // Out of range (a hand-edited project): 90 counts as 89 (edge-on would be a degenerate quad), fov 0 as 5
    Buf over, at89;
    if (!CHECK(runNode("transform", {{"size", 0.8}, {"tilt_x", 90.0}, {"fov", 0.0}}, {{"src", src}}, sized(W, H), &over)))
        return;
    if (!CHECK(runNode("transform", {{"size", 0.8}, {"tilt_x", 89.0}, {"fov", 5.0}}, {{"src", src}}, sized(W, H), &at89)))
        return;
    CHECK(over.px == at89.px);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    testHomography();
    testTilted();
    testDrawAffineEqual();
    testNode();
    return Check::result();
}
