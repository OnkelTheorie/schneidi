// SPDX-License-Identifier: AGPL-3.0-only
// Keyer nodes of the compositing graph (0.19, engine/picture/CompNodesKey.cpp), through the single-node runner:
// - key chroma on a synthetic figure on green (also shaded green): background alpha 0, figure 255 and unchanged,
//   a soft edge in between, no green fringe left; bit-identical to the effect "chromakey" with the same values (1080p,
//   so the node's bands of rows on several threads are covered)
// - key luma on a gray ramp: exactly at the thresholds (hard), the soft ramp, invert
// - matte_tune: grow/shrink by N pixels (measured, also scaled), holes closed by clip white, haze cleared by clip
//   black, softened edge, despill takes out the fringe a key without spill left, a mask stays a mask, defaults change
//   nothing
// - speed: ms per 1080p frame (printed)
#include "check.h"

#include "engine/VideoEffects.h"
#include "engine/picture/CompNodes.h"

#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <cstring>

using Picture::Comp::Buf;
using Picture::Comp::RunOptions;
using Picture::Comp::runNode;

namespace {

const QColor kSkin(224, 172, 140);
const QColor kScreen(40, 230, 50); // the green screen; in shade 63 % of it

void setPx(Buf& b, int x, int y, int r, int g, int bl, int a = 255)
{
    uint8_t* p = b.data() + (size_t(y) * b.w + x) * 4;
    p[0] = uint8_t(r), p[1] = uint8_t(g), p[2] = uint8_t(bl), p[3] = uint8_t(a);
}
const uint8_t* px(const Buf& b, int x, int y) { return b.data() + (size_t(y) * b.w + x) * b.channels(); }

// A disc of skin (radius r) in the middle of a green screen, its edge blended into the green over `edge` pixels
// (the green spill a real edge has); the left quarter of the green in shade
Buf figureOnGreen(int w, int h, double r, double edge)
{
    Buf b(Buf::Rgba, w, h);
    const double cx = w / 2.0, cy = h / 2.0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool shade = x < w / 4;
            const int rr = shade ? 25 : 40, gr = shade ? 145 : 230, br = shade ? 32 : 50;
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double f = std::clamp((r - d) / edge, 0.0, 1.0); // 1 = figure
            setPx(b, x, y, int(std::lround(rr + (kSkin.red() - rr) * f)), int(std::lround(gr + (kSkin.green() - gr) * f)),
                  int(std::lround(br + (kSkin.blue() - br) * f)));
        }
    return b;
}

RunOptions sized(const Buf& b, double scale = 1)
{
    RunOptions o;
    o.size = QSize(b.w, b.h);
    o.scale = scale;
    return o;
}

void testChroma()
{
    const Buf src = figureOnGreen(320, 180, 50, 8);
    Buf out;
    if (!CHECK(runNode("key", {{"color", kScreen}}, {{"src", src}}, sized(src), &out))) return;
    CHECK(out.kind == Buf::Rgba && out.w == 320 && out.h == 180);
    CHECK_EQ(int(px(out, 5, 5)[3]), 0);     // green
    CHECK_EQ(int(px(out, 300, 170)[3]), 0); // green
    CHECK_EQ(int(px(out, 10, 90)[3]), 0);   // shaded green
    const uint8_t* c = px(out, 160, 90);    // figure: opaque, color kept
    CHECK_EQ(int(c[3]), 255);
    CHECK(std::abs(c[0] - kSkin.red()) <= 2 && std::abs(c[1] - kSkin.green()) <= 2 && std::abs(c[2] - kSkin.blue()) <= 2);
    // Soft edge: alpha between 0 and 255 somewhere along the radius, rising inward; no green fringe there
    int soft = 0, fringe = 0, lastA = 0;
    bool monotone = true;
    for (int x = 160 + 60; x >= 160 + 40; --x) {
        const uint8_t* p = px(out, x, 90);
        if (p[3] > 0 && p[3] < 255) ++soft;
        if (p[3] > 8 && p[1] > std::max(p[0], p[2]) + 6) ++fringe;
        if (p[3] + 1 < lastA) monotone = false;
        lastA = p[3];
    }
    CHECK(soft >= 2);
    CHECK(monotone);
    CHECK_EQ(fringe, 0);
    // No key node input = nothing
    CHECK(!runNode("key", {}, {}, sized(src), &out));
}

// The node and the effect "chromakey" with the same values: the same bytes
void testChromaEqualsEffect()
{
    const Buf src = figureOnGreen(1920, 1080, 300, 12);
    const QVariantMap values{{"color", QColor(30, 220, 60)}, {"tolerance", 0.27}, {"softness", 0.31}, {"spill", 0.7}};
    Buf node;
    if (!CHECK(runNode("key", values, {{"src", src}}, sized(src), &node))) return;
    std::vector<uint8_t> fx = src.px;
    VideoEffects::Params p;
    p.has = [](const char*) { return true; };
    p.num = [&](const char* k) {
        const QString key = QString::fromLatin1(k);
        return key == "1" ? 0.27 : key == "softness" ? 0.31 : 0.7;
    };
    p.color = [](const char*) { return QColor(30, 220, 60); };
    p.text = [](const char*) { return QString(); };
    CHECK(VideoEffects::apply("chromakey", fx.data(), 1920, 1080, p, 0));
    CHECK(node.px == fx);
    // Also when only the content rectangle of the input is worked on (outside it is transparent anyway)
    Buf framed = src;
    for (int y = 0; y < framed.h; ++y)
        for (int x = 0; x < framed.w; ++x)
            if (y < 100 || y >= 900) framed.data()[(size_t(y) * framed.w + x) * 4 + 3] = 0;
    framed.area = QRect(0, 100, 1920, 800);
    Buf nodeFramed;
    CHECK(runNode("key", values, {{"src", framed}}, sized(framed), &nodeFramed));
    CHECK(nodeFramed.area == framed.area);
    std::vector<uint8_t> fxFramed = framed.px;
    VideoEffects::apply("chromakey", fxFramed.data(), 1920, 1080, p, 0);
    bool same = true;
    for (int y = 100; y < 900 && same; ++y)
        same = std::memcmp(nodeFramed.data() + size_t(y) * 1920 * 4, fxFramed.data() + size_t(y) * 1920 * 4, 1920 * 4) == 0;
    CHECK(same);
}

// A ramp 0..255 from left to right (gray level = x)
Buf grayRamp()
{
    Buf b(Buf::Rgba, 256, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 256; ++x) setPx(b, x, y, x, x, x);
    return b;
}

void testLuma()
{
    const Buf ramp = grayRamp();
    Buf out;
    QVariantMap v{{"mode", "luma"}, {"low", 0.4}, {"high", 0.8}, {"soft", 0.0}};
    if (!CHECK(runNode("key", v, {{"src", ramp}}, sized(ramp), &out))) return;
    // 0.4 * 255 = 102, 0.8 * 255 = 204: inside from 102 to 204, hard outside
    CHECK_EQ(int(px(out, 101, 1)[3]), 0);
    CHECK_EQ(int(px(out, 102, 1)[3]), 255);
    CHECK_EQ(int(px(out, 204, 1)[3]), 255);
    CHECK_EQ(int(px(out, 205, 1)[3]), 0);
    CHECK_EQ(int(px(out, 0, 0)[3]), 0);
    CHECK_EQ(int(px(out, 255, 0)[3]), 0);
    CHECK(px(out, 150, 2)[0] == 150 && px(out, 150, 2)[1] == 150); // color kept
    // Soft ramp of 0.1 below low: half way (0.35 = level 89.25) about half transparent, rising
    v["soft"] = 0.1;
    CHECK(runNode("key", v, {{"src", ramp}}, sized(ramp), &out));
    CHECK(std::abs(int(px(out, 89, 0)[3]) - 128) <= 8);
    CHECK_EQ(int(px(out, 76, 0)[3]), 0);  // below low - soft (76.5)
    CHECK_EQ(int(px(out, 102, 0)[3]), 255);
    CHECK(px(out, 95, 0)[3] > px(out, 85, 0)[3]);
    CHECK(std::abs(int(px(out, 217, 0)[3]) - 128) <= 8); // above high: 0.85 = level 216.75
    // Invert: the range becomes transparent
    v["soft"] = 0.0;
    v["invert"] = true;
    CHECK(runNode("key", v, {{"src", ramp}}, sized(ramp), &out));
    CHECK_EQ(int(px(out, 101, 1)[3]), 255);
    CHECK_EQ(int(px(out, 150, 1)[3]), 0);
    CHECK_EQ(int(px(out, 205, 1)[3]), 255);
}

// A mask with a filled rectangle; width/height of the 255 run through its middle
Buf rectMask(int w, int h, QRect r)
{
    Buf b(Buf::Mask, w, h);
    for (int y = r.top(); y <= r.bottom(); ++y) std::memset(b.data() + size_t(y) * w + r.left(), 255, size_t(r.width()));
    return b;
}
int runX(const Buf& b, int y)
{
    int n = 0;
    for (int x = 0; x < b.w; ++x) n += px(b, x, y)[b.channels() - 1] == 255;
    return n;
}
int runY(const Buf& b, int x)
{
    int n = 0;
    for (int y = 0; y < b.h; ++y) n += px(b, x, y)[b.channels() - 1] == 255;
    return n;
}

void testGrowShrink()
{
    const Buf mask = rectMask(200, 120, QRect(60, 40, 80, 40));
    Buf out;
    if (!CHECK(runNode("matte_tune", {{"grow", 5.0}}, {{"src", mask}}, sized(mask), &out))) return;
    CHECK(out.kind == Buf::Mask); // a mask stays a mask
    CHECK_EQ(runX(out, 60), 90);
    CHECK_EQ(runY(out, 100), 50);
    CHECK_EQ(int(px(out, 55, 60)[0]), 255);
    CHECK_EQ(int(px(out, 54, 60)[0]), 0);
    CHECK(runNode("matte_tune", {{"grow", -5.0}}, {{"src", mask}}, sized(mask), &out));
    CHECK_EQ(runX(out, 60), 70);
    CHECK_EQ(runY(out, 100), 30);
    // Project pixels times the output scale: grow 4 at half size = 2 pixels
    CHECK(runNode("matte_tune", {{"grow", 4.0}}, {{"src", mask}}, sized(mask, 0.5), &out));
    CHECK_EQ(runX(out, 60), 84);
    // On the alpha of a picture: the color stays, only the alpha grows
    Buf pic(Buf::Rgba, 200, 120);
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 200; ++x) setPx(pic, x, y, 10, 20, 30, px(mask, x, y)[0]);
    CHECK(runNode("matte_tune", {{"grow", 3.0}}, {{"src", pic}}, sized(pic), &out));
    CHECK(out.kind == Buf::Rgba);
    CHECK_EQ(runX(out, 60), 86);
    CHECK(px(out, 58, 60)[0] == 10 && px(out, 58, 60)[2] == 30);
}

void testClip()
{
    // Figure alpha 255 with half-transparent holes (a shirt close to the key color), haze of 20 in the background
    Buf pic(Buf::Rgba, 120, 80);
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 120; ++x) {
            int a = 20;
            if (x >= 30 && x < 90 && y >= 20 && y < 60) a = ((x / 6 + y / 6) % 3 == 0) ? 160 : 255;
            setPx(pic, x, y, 200, 150, 120, a);
        }
    Buf out;
    if (!CHECK(runNode("matte_tune", {{"white", 0.6}, {"black", 0.1}}, {{"src", pic}}, sized(pic), &out))) return;
    int holes = 0, haze = 0;
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 120; ++x) {
            const int a = px(out, x, y)[3];
            if (x >= 30 && x < 90 && y >= 20 && y < 60) holes += a != 255;
            else haze += a != 0;
        }
    CHECK_EQ(holes, 0);
    CHECK_EQ(haze, 0);
    // Without clip white the holes stay
    CHECK(runNode("matte_tune", {{"black", 0.1}}, {{"src", pic}}, sized(pic), &out));
    CHECK(px(out, 30, 24)[3] < 255);
    // In between: a linear stretch (a = 0.35 between black 0.1 and white 0.6 -> 0.5)
    Buf mid(Buf::Mask, 4, 4);
    std::fill(mid.px.begin(), mid.px.end(), uint8_t(89)); // 0.349
    CHECK(runNode("matte_tune", {{"white", 0.6}, {"black", 0.1}}, {{"src", mid}}, sized(mid), &out));
    CHECK(std::abs(int(px(out, 1, 1)[0]) - 127) <= 1);
}

void testSoftAndDefaults()
{
    const Buf mask = rectMask(200, 120, QRect(60, 40, 80, 40));
    Buf out;
    if (!CHECK(runNode("matte_tune", {{"soft", 3.0}}, {{"src", mask}}, sized(mask), &out))) return;
    const int edge = px(out, 60, 60)[0], inside = px(out, 100, 60)[0], outside = px(out, 40, 60)[0];
    CHECK(edge > 60 && edge < 200);
    CHECK_EQ(inside, 255);
    CHECK_EQ(outside, 0);
    // The same bytes as alphaOps' grow hard / blur (radius = amount * height / 72; 720 rows: amount 0.5 = 5 px), so
    // the bands of rows and columns on threads add up to the one-thread filter
    Buf pic(Buf::Rgba, 300, 720);
    for (int y = 0; y < pic.h; ++y)
        for (int x = 0; x < pic.w; ++x) {
            const bool in = std::hypot(x - 150.0, y - 360.0) < 90 || (x > 20 && x < 40 && y % 50 < 30);
            setPx(pic, x, y, x % 256, y % 256, 99, in ? 255 : (x * 7 + y * 3) % 40);
        }
    const auto viaAlphaOps = [&](double op) {
        Buf b = pic;
        VideoEffects::AlphaParams a;
        a.operation = op;
        a.amount = 0.5;
        VideoEffects::alphaOps(b.data(), b.w, b.h, a);
        return b.px;
    };
    CHECK(runNode("matte_tune", {{"soft", 5.0}}, {{"src", pic}}, sized(pic), &out));
    CHECK(out.px == viaAlphaOps(1.0));
    CHECK(runNode("matte_tune", {{"grow", 5.0}}, {{"src", pic}}, sized(pic), &out));
    CHECK(out.px == viaAlphaOps(0.6));
    CHECK(runNode("matte_tune", {{"grow", -5.0}}, {{"src", pic}}, sized(pic), &out));
    CHECK(out.px == viaAlphaOps(0.3));
    CHECK(runNode("matte_tune", {{"grow", 5.0}, {"soft", 2.5}}, {{"src", pic}}, sized(pic), &out));
    CHECK(out.px == viaAlphaOps(0.7));
    // Defaults: nothing changes (mask and picture)
    CHECK(runNode("matte_tune", {}, {{"src", mask}}, sized(mask), &out));
    CHECK(out.px == mask.px);
    const Buf fig = figureOnGreen(160, 90, 30, 6);
    CHECK(runNode("matte_tune", {}, {{"src", fig}}, sized(fig), &out));
    CHECK(out.px == fig.px);
}

void testDespill()
{
    // Key without spill: the edge keeps its green fringe; matte_tune despill takes it out, the skin stays
    const Buf src = figureOnGreen(320, 180, 50, 8);
    Buf keyed, clean;
    if (!CHECK(runNode("key", {{"spill", 0.0}, {"color", kScreen}}, {{"src", src}}, sized(src), &keyed))) return;
    const auto fringeCount = [](const Buf& b) {
        int n = 0;
        for (int x = 160 + 40; x <= 160 + 60; ++x) {
            const uint8_t* p = px(b, x, 90);
            n += p[3] > 8 && p[1] > std::max(p[0], p[2]) + 6;
        }
        return n;
    };
    CHECK(fringeCount(keyed) > 0);
    CHECK(runNode("matte_tune", {{"despill", 1.0}, {"color", kScreen}}, {{"src", keyed}}, sized(keyed), &clean));
    CHECK_EQ(fringeCount(clean), 0);
    const uint8_t* c = px(clean, 160, 90);
    CHECK(std::abs(c[0] - kSkin.red()) <= 2 && std::abs(c[1] - kSkin.green()) <= 2 && std::abs(c[2] - kSkin.blue()) <= 2);
    // Alpha untouched by despill
    bool alphaSame = true;
    for (size_t i = 3; i < keyed.px.size() && alphaSame; i += 4) alphaSame = keyed.px[i] == clean.px[i];
    CHECK(alphaSame);
}

void testSpeed()
{
    const Buf src = figureOnGreen(1920, 1080, 300, 12);
    const QHash<QString, Buf> in{{"src", src}};
    const auto ms = [&](const char* kind, const QVariantMap& v) {
        Buf out;
        runNode(kind, v, in, sized(src), &out); // warm (tables, threads)
        QElapsedTimer t;
        t.start();
        constexpr int n = 10;
        for (int i = 0; i < n; ++i) runNode(kind, v, in, sized(src), &out);
        return t.nsecsElapsed() / 1e6 / n;
    };
    const auto msSingle = [&](const std::function<void(uint8_t*)>& fn) {
        std::vector<uint8_t> img = src.px;
        fn(img.data());
        QElapsedTimer t;
        t.start();
        constexpr int n = 10;
        for (int i = 0; i < n; ++i) {
            std::memcpy(img.data(), src.data(), img.size());
            fn(img.data());
        }
        return t.nsecsElapsed() / 1e6 / n;
    };
    std::printf("1080p ms/frame (node with forRows | one thread, incl. copy):\n");
    std::printf("  copy only (matte_tune defaults; runner allocates fresh buffers, the engine pools) %6.2f\n",
                ms("matte_tune", {}));
    std::printf("  key chroma      %6.2f | %6.2f\n", ms("key", {}),
                msSingle([](uint8_t* p) { VideoEffects::key(p, 1920, 1080, {}); }));
    std::printf("  key luma        %6.2f | %6.2f\n", ms("key", {{"mode", "luma"}}),
                msSingle([](uint8_t* p) { VideoEffects::lumaKey(p, 1920, 1080, {}); }));
    std::printf("  matte_tune clip %6.2f\n", ms("matte_tune", {{"white", 0.8}, {"black", 0.1}}));
    std::printf("  matte_tune grow3 %6.2f\n", ms("matte_tune", {{"grow", 3.0}}));
    std::printf("  matte_tune soft2 %6.2f\n", ms("matte_tune", {{"soft", 2.0}}));
    std::printf("  matte_tune grow3 soft2 %6.2f | alphaOps grow soft (one thread) %6.2f\n",
                ms("matte_tune", {{"grow", 3.0}, {"soft", 2.0}}), msSingle([](uint8_t* p) {
                    VideoEffects::AlphaParams a;
                    a.operation = 0.7;
                    a.amount = 0.2;
                    VideoEffects::alphaOps(p, 1920, 1080, a);
                }));
    std::printf("  matte_tune despill %6.2f | %6.2f\n", ms("matte_tune", {{"despill", 1.0}}),
                msSingle([](uint8_t* p) { VideoEffects::despill(p, 1920, 1080, QColor(0, 255, 0), 1.0); }));
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    testChroma();
    testChromaEqualsEffect();
    testLuma();
    testGrowShrink();
    testClip();
    testSoftAndDefaults();
    testDespill();
    testSpeed();
    return Check::result();
}
