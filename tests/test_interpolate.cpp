// SPDX-License-Identifier: AGPL-3.0-only
// In-between pictures for slow motion (engine/picture/Interpolate.h, Clip::interpolate): a moving textured square
// lands within a pixel of its ideal in-between position, a static HUD over a panning background stays identical,
// blending is the exact average, a cut (also one to the same brightness in other colours, or to other content in
// the same colours) gives no ghost; through the renderer: blend at 25 % equals the average of
// the two source frames, a freeze frame and the parts of a ramp at full speed stay as before. Prints the time per
// 1080p in-between picture.

#include "check.h"

#include "engine/OutputColor.h"
#include "engine/picture/Interpolate.h"
#include "engine/picture/Renderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <vector>

namespace {

using Pic = std::vector<uint8_t>;

uint8_t noise(int x, int y, uint32_t seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return uint8_t((h ^ (h >> 16)) & 0xff);
}

// Smooth-ish texture (noise on a 4 px grid, so block matching has structure and sub-pixel shifts stay meaningful)
uint8_t texture(int x, int y, uint32_t seed) { return noise(x >> 2, y >> 2, seed) / 2 + 64; }

void put(Pic& p, int w, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t* d = &p[(size_t(y) * w + x) * 4];
    d[0] = r, d[1] = g, d[2] = b, d[3] = 255;
}

// Textured background shifted by (bx, by), a textured square at (sx, sy), optionally a static HUD box
Pic scene(int w, int h, int bx, int by, int sx, int sy, int side, bool hud)
{
    Pic p(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t v = texture(x - bx + 1000, y - by + 1000, 1);
            put(p, w, x, y, v, uint8_t(v / 2 + 40), uint8_t(255 - v));
            if (x >= sx && x < sx + side && y >= sy && y < sy + side) {
                const uint8_t s = texture(x - sx, y - sy, 7);
                put(p, w, x, y, uint8_t(255 - s / 3), s, uint8_t(s / 4));
            }
            if (hud && x >= 20 && x < 220 && y >= h - 60 && y < h - 20) { // health/ammo box: white glyphs on black
                const bool on = noise(x / 2, y / 3, 11) > 150;
                put(p, w, x, y, on ? 250 : 10, on ? 250 : 10, on ? 250 : 10);
            }
        }
    return p;
}

// Where the square sits in `p`: the offset with the smallest difference to its texture, near (gx, gy)
QPoint findSquare(const Pic& p, int w, int gx, int gy, int side)
{
    double best = 1e18;
    QPoint at;
    for (int oy = -6; oy <= 6; ++oy)
        for (int ox = -6; ox <= 6; ++ox) {
            double sum = 0;
            for (int y = 4; y < side - 4; ++y)
                for (int x = 4; x < side - 4; ++x) {
                    const uint8_t s = texture(x, y, 7);
                    const uint8_t* d = &p[(size_t(gy + oy + y) * w + gx + ox + x) * 4];
                    sum += std::abs(int(d[1]) - int(s)) + std::abs(int(d[0]) - int(255 - s / 3));
                }
            if (sum < best) best = sum, at = QPoint(gx + ox, gy + oy);
        }
    return at;
}

// INTERPOLATE_DUMP=<folder>: the pictures as PNG for looking at them
void dump(const QString& name, const Pic& p, int w, int h)
{
    const QString dir = qEnvironmentVariable("INTERPOLATE_DUMP");
    if (dir.isEmpty()) return;
    QImage(p.data(), w, h, QImage::Format_RGBA8888).save(dir + "/" + name + ".png");
}

void algorithmChecks()
{
    using namespace Picture::Interp;
    const int w = 640, h = 360, side = 64;
    // Square moving 16 px right and 8 px down per source frame over a background panning 6 px left
    const Pic a = scene(w, h, 0, 0, 200, 120, side, false), b = scene(w, h, -6, 0, 216, 128, side, false);
    const auto motion = estimate(a.data(), b.data(), w, h);
    dump("square-a", a, w, h);
    dump("square-b", b, w, h);
    CHECK(!cut(*motion));
    if (!CHECK(unreliable(*motion) < 0.2)) std::printf("       unreliable share %.2f\n", unreliable(*motion));
    Pic out(a.size());
    for (double t : {0.25, 0.5, 0.75}) {
        flow(*motion, a.data(), b.data(), out.data(), w, h, t);
        dump(QString("square-%1").arg(t), out, w, h);
        const QPoint ideal(int(std::lround(200 + 16 * t)), int(std::lround(120 + 8 * t)));
        const QPoint got = findSquare(out, w, ideal.x(), ideal.y(), side);
        if (!CHECK(std::abs(got.x() - ideal.x()) <= 1 && std::abs(got.y() - ideal.y()) <= 1))
            std::printf("       t %.2f: square at %d,%d instead of %d,%d\n", t, got.x(), got.y(), ideal.x(), ideal.y());
        // And the whole picture close to the ideal in-between (background pan included)
        const Pic want = scene(w, h, int(std::lround(-6 * t)), 0, ideal.x(), ideal.y(), side, false);
        double diff = 0;
        for (int y = 8; y < h - 8; ++y)
            for (int x = 8; x < w - 8; ++x) diff += std::abs(int(out[(size_t(y) * w + x) * 4 + 1]) - int(want[(size_t(y) * w + x) * 4 + 1]));
        diff /= double(w - 16) * (h - 16);
        if (!CHECK(diff < 4)) std::printf("       t %.2f: mean difference to the ideal picture %.2f\n", t, diff);
    }

    // A static HUD over a fast pan (12 px per frame) stays exactly as it is
    {
        const Pic ha = scene(w, h, 0, 0, 400, 60, side, true), hb = scene(w, h, -12, 0, 400, 60, side, true);
        const auto m = estimate(ha.data(), hb.data(), w, h);
        flow(*m, ha.data(), hb.data(), out.data(), w, h, 0.5);
        dump("hud-a", ha, w, h);
        dump("hud-mid", out, w, h);
        // Inside exactly; its outer pixel row (whose 3x3 neighbourhood sees the moving background) may take a little
        // of the soft mix (19 levels at most when written)
        int changed = 0, total = 0, worst = 0;
        for (int y = h - 60; y < h - 20; ++y)
            for (int x = 20; x < 220; ++x, ++total) {
                const bool rim = x == 20 || x == 219 || y == h - 60 || y == h - 21;
                for (int c = 0; c < 3; ++c) {
                    const int d = std::abs(out[(size_t(y) * w + x) * 4 + c] - ha[(size_t(y) * w + x) * 4 + c]);
                    worst = std::max(worst, d);
                    if (d && !rim) {
                        if (++changed < 4) std::printf("       HUD pixel %d,%d: off by %d\n", x, y, d);
                        break;
                    }
                }
            }
        CHECK_EQ(changed, 0);
        if (!CHECK(worst <= 32)) std::printf("       HUD rim: off by up to %d\n", worst);
        if (!CHECK(changed == 0)) std::printf("       HUD: %d of %d pixels changed\n", changed, total);
    }

    // Blend: the exact rounded average
    {
        Pic c(a.size());
        blend(a.data(), b.data(), c.data(), w, h, 0.5);
        int wrong = 0;
        for (size_t i = 0; i < a.size(); ++i) wrong += c[i] != uint8_t((a[i] + b[i] + 1) / 2);
        CHECK_EQ(wrong, 0);
        blend(a.data(), b.data(), c.data(), w, h, 0.0);
        CHECK(c == a);
        blend(a.data(), b.data(), c.data(), w, h, 1.0);
        CHECK(c == b);
    }

    // A cut between the two frames: no in-between, the nearer frame (no ghost)
    {
        Pic dark(a.size());
        for (size_t i = 0; i < dark.size(); i += 4) dark[i] = 20, dark[i + 1] = 30, dark[i + 2] = 25, dark[i + 3] = 255;
        for (int y = 0; y < h; ++y) // some structure, still clearly another shot
            for (int x = 0; x < w; ++x) dark[(size_t(y) * w + x) * 4] = uint8_t(texture(x, y, 3) / 6);
        CHECK(isCut(a.data(), dark.data(), w, h));
        CHECK(!isCut(a.data(), b.data(), w, h));
        const auto m = estimate(a.data(), dark.data(), w, h);
        CHECK(cut(*m));
        flow(*m, a.data(), dark.data(), out.data(), w, h, 0.25);
        CHECK(out == a);
        flow(*m, a.data(), dark.data(), out.data(), w, h, 0.75);
        CHECK(out == dark);
    }

    // A cut to a shot of the same brightness in other colours (a map change in a game; a luma histogram saw no cut):
    // detected, no blended ghost in blend nor flow
    {
        Pic grey(a.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) { // the luma of `a`'s background, but grey and another texture
                const uint8_t g = uint8_t(texture(x, y, 5) * 0.495 + 46);
                put(grey, w, x, y, g, g, g);
            }
        CHECK(isCut(a.data(), grey.data(), w, h));
        const auto m = estimate(a.data(), grey.data(), w, h);
        CHECK(cut(*m));
        flow(*m, a.data(), grey.data(), out.data(), w, h, 0.4);
        CHECK(out == a);
        flow(*m, a.data(), grey.data(), out.data(), w, h, 0.6);
        CHECK(out == grey);
    }

    // Same colours, other content (another part of the same map): the motion explains nothing -> a cut
    {
        Pic other(a.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t v = texture(x, y, 9);
                put(other, w, x, y, v, uint8_t(v / 2 + 40), uint8_t(255 - v));
            }
        const Pic plain = scene(w, h, 0, 0, -side, -side, side, false); // background only
        const auto m = estimate(plain.data(), other.data(), w, h);
        if (!CHECK(cut(*m))) std::printf("       unreliable share %.2f\n", unreliable(*m));
    }

    // Time per 1080p in-between picture (motion once per pair, then the picture per t)
    {
        const int W = 1920, H = 1080;
        const Pic A = scene(W, H, 0, 0, 800, 400, 192, true), B = scene(W, H, -30, 0, 848, 424, 192, true);
        Pic O(A.size());
        QElapsedTimer t;
        t.start();
        const auto m = estimate(A.data(), B.data(), W, H);
        const double est = t.nsecsElapsed() / 1e6;
        t.restart();
        for (double s : {0.25, 0.5, 0.75}) flow(*m, A.data(), B.data(), O.data(), W, H, s);
        const double pic = t.nsecsElapsed() / 3e6;
        std::printf("1080p: motion %.1f ms per frame pair, in-between picture %.1f ms (at 25 %%: %.1f ms per output frame)\n",
                    est, pic, (est + 3 * pic) / 4);
        CHECK(!cut(*m));
        // Preview size (playback renders smaller)
        const int pw = 960, ph = 540;
        const Pic PA = scene(pw, ph, 0, 0, 400, 200, 96, true), PB = scene(pw, ph, -15, 0, 424, 212, 96, true);
        Pic PO(PA.size());
        t.restart();
        const auto pm = estimate(PA.data(), PB.data(), pw, ph);
        const double pest = t.nsecsElapsed() / 1e6;
        t.restart();
        flow(*pm, PA.data(), PB.data(), PO.data(), pw, ph, 0.5);
        std::printf("960x540: motion %.1f ms, in-between picture %.1f ms\n", pest, t.nsecsElapsed() / 1e6);
    }
}

const ProjectFormat kFmt{320, 180, {25, 1}};

QImage render(const Timeline& tl, int pos)
{
    Picture::Options o;
    o.format = kFmt;
    o.mode = Picture::Mode::Export;
    o.gpu = false;
    Picture::Renderer r(tl, o);
    return r.render(pos).toImage().convertToFormat(QImage::Format_RGBA8888);
}

Timeline one(const QString& media, double speed, FrameInterp mode, int length)
{
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.speed = speed;
    c.interpolate = mode;
    c.out = length - 1;
    tl.video[0].clips << c;
    return tl;
}

int maxDiff(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull()) return 999;
    int d = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width() * 4; ++x) d = std::max(d, std::abs(int(a.constScanLine(y)[x]) - int(b.constScanLine(y)[x])));
    return d;
}

void rendererChecks(const QString& video)
{
    // 25 % of a 25 fps video in a 25 fps project: output frame 2 lies halfway between source frames 0 and 1
    const QImage f0 = render(one(video, 0.25, FrameInterp::Repeat, 40), 0);
    const QImage f1 = render(one(video, 0.25, FrameInterp::Repeat, 40), 4);
    const QImage rep = render(one(video, 0.25, FrameInterp::Repeat, 40), 2);
    CHECK(maxDiff(rep, f0) == 0); // repeat: the earlier frame
    const QImage mid = render(one(video, 0.25, FrameInterp::Blend, 40), 2);
    QImage avg = f0;
    for (int y = 0; y < avg.height(); ++y)
        for (int x = 0; x < avg.width() * 4; ++x)
            avg.scanLine(y)[x] = uint8_t((f0.constScanLine(y)[x] + f1.constScanLine(y)[x] + 1) / 2);
    if (!CHECK(maxDiff(mid, avg) <= 1)) std::printf("       blend vs average: %d\n", maxDiff(mid, avg));
    CHECK(maxDiff(mid, f0) > 10); // really in between (the test video moves)
    // The export's shortcut for a plain full-size video (decoded planes handed on as they are) must not skip it
    {
        Picture::Options o;
        o.format = kFmt;
        o.mode = Picture::Mode::Export;
        o.gpu = false;
        Picture::Renderer r(one(video, 0.25, FrameInterp::Blend, 40), o);
        std::vector<uint8_t> planes(size_t(320) * 180 * 3 / 2);
        OutputColor::Target target;
        target.planes[0] = planes.data();
        target.planes[1] = planes.data() + 320 * 180;
        target.planes[2] = planes.data() + 320 * 180 + 160 * 90;
        target.strides[0] = 320;
        target.strides[1] = target.strides[2] = 160;
        Picture::RenderHints hints;
        hints.yuv = &target;
        CHECK(!r.render(2, hints).yuvWritten());
    }
    // On a source frame the in-between modes show it unchanged
    CHECK_EQ(maxDiff(render(one(video, 0.25, FrameInterp::Flow, 40), 4), f1), 0);
    // Flow: closer to the average than to either side (a picture in between, not a repeat)
    const QImage fl = render(one(video, 0.25, FrameInterp::Flow, 40), 2);
    CHECK(!fl.isNull() && fl.size() == f0.size());
    CHECK(maxDiff(fl, f0) > 10 && maxDiff(fl, f1) > 10);

    // Full speed and faster: nothing in between to compute, identical to repeat
    CHECK_EQ(maxDiff(render(one(video, 1.0, FrameInterp::Flow, 20), 7), render(one(video, 1.0, FrameInterp::Repeat, 20), 7)), 0);
    // Freeze frame: the frozen picture as before
    Timeline fz = one(video, 1.0, FrameInterp::Flow, 20), fzr = one(video, 1.0, FrameInterp::Repeat, 20);
    for (Timeline* t : {&fz, &fzr}) {
        t->video[0].clips[0].freeze = true;
        t->video[0].clips[0].freezeFrame = 5;
    }
    CHECK_EQ(maxDiff(render(fz, 3), render(fzr, 3)), 0);
    // Ramp: 100 % up to source 10, then 25 %; before the point unchanged, after it in between
    Timeline rp = one(video, 1.0, FrameInterp::Blend, 30), rpr = one(video, 1.0, FrameInterp::Repeat, 30);
    for (Timeline* t : {&rp, &rpr}) t->video[0].clips[0].ramp = {SpeedPoint{10, 0.25, 0}};
    CHECK_EQ(maxDiff(render(rp, 5), render(rpr, 5)), 0);
    CHECK_EQ(maxDiff(render(rp, 14), render(rpr, 14)), 0); // source 11.0: on a frame
    CHECK(maxDiff(render(rp, 12), render(rpr, 12)) > 10);   // source 10.5: in between
}

// A source faster than the project (50 fps in 25 fps, like 60 fps gameplay in a 30 fps edit) at 25 %: the stream
// position runs 0, 0.5, 1, 1.5, 2 ... per output frame. On a stream frame its own picture, never the one before
// (the picture jumped back: 0, 0-1, 0, 1-2, 2, 2-3, 2 ...)
void fasterSourceChecks(const QString& video50)
{
    for (FrameInterp mode : {FrameInterp::Blend, FrameInterp::Flow}) {
        const QImage s0 = render(one(video50, 0.25, mode, 40), 0);
        const QImage s1 = render(one(video50, 0.25, mode, 40), 2); // stream frame 1
        const QImage s2 = render(one(video50, 0.25, mode, 40), 4); // stream frame 2
        if (!CHECK(maxDiff(s1, s0) > 10)) std::printf("       mode %d: stream frame 1 shows frame 0\n", int(mode));
        CHECK(maxDiff(s1, s2) > 10);
        if (mode != FrameInterp::Blend) continue;
        // Output frame 1 lies halfway between stream frames 0 and 1
        const QImage mid = render(one(video50, 0.25, mode, 40), 1);
        QImage avg = s0;
        for (int y = 0; y < avg.height(); ++y)
            for (int x = 0; x < avg.width() * 4; ++x)
                avg.scanLine(y)[x] = uint8_t((s0.constScanLine(y)[x] + s1.constScanLine(y)[x] + 1) / 2);
        if (!CHECK(maxDiff(mid, avg) <= 1)) std::printf("       50 fps blend vs average: %d\n", maxDiff(mid, avg));
    }
    // Repeat mode: every stream frame shows, twice at 25 % (0, 0, 1, 1, 2, 2); rounded in project frames every other
    // one was skipped (0, 0, 0, 0, 2, 2, 2, 2)
    const QImage r0 = render(one(video50, 0.25, FrameInterp::Repeat, 40), 0);
    const QImage r2 = render(one(video50, 0.25, FrameInterp::Repeat, 40), 2);
    const QImage r3 = render(one(video50, 0.25, FrameInterp::Repeat, 40), 3);
    const QImage s1 = render(one(video50, 0.25, FrameInterp::Blend, 40), 2); // stream frame 1 itself
    if (!CHECK(maxDiff(r2, r0) > 10)) std::printf("       repeat: stream frame 1 skipped\n");
    CHECK_EQ(maxDiff(r2, s1), 0);
    CHECK_EQ(maxDiff(r3, r2), 0);
    // Backwards likewise: the last of 40 project frames is stream frame 79, then 79, 78, 78 ...
    Timeline back = one(video50, 0.25, FrameInterp::Repeat, 40);
    back.video[0].clips[0].reverse = true;
    CHECK(maxDiff(render(back, 2), render(back, 0)) > 10);
    CHECK_EQ(maxDiff(render(back, 1), render(back, 0)), 0);
    CHECK(maxDiff(render(back, 4), render(back, 2)) > 10);
    // ... and the in-between modes start on the same last stream frame (not one before it)
    Timeline backBlend = one(video50, 0.25, FrameInterp::Blend, 40);
    backBlend.video[0].clips[0].reverse = true;
    CHECK_EQ(maxDiff(render(backBlend, 0), render(back, 0)), 0);
    CHECK_EQ(maxDiff(render(backBlend, 2), render(back, 2)), 0);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("interpolate");
    std::setlocale(LC_NUMERIC, "C");
    algorithmChecks();
    if (!Check::haveFfmpeg()) {
        std::printf("ffmpeg nicht gefunden: Renderer-Prüfungen übersprungen\n");
        return Check::result();
    }
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // A pattern that moves every frame (testsrc2 scrolls), intra-only so every frame decodes on its own
    const QString video = Check::makeMedia(tmp.filePath("v.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2", "-c:v",
                                            "libx264", "-preset", "ultrafast", "-g", "1", "-pix_fmt", "yuv420p"});
    if (!CHECK(!video.isEmpty())) return Check::result();
    Check::initEngine();
    rendererChecks(video);
    const QString video50 = Check::makeMedia(tmp.filePath("v50.mp4"),
                                             {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=50:duration=2", "-c:v",
                                              "libx264", "-preset", "ultrafast", "-g", "1", "-pix_fmt", "yuv420p"});
    if (CHECK(!video50.isEmpty())) fasterSourceChecks(video50);
    return Check::result();
}
