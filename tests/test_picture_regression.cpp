// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine (engine/picture) regression checks that replace the comparison with the MLT path
// (test_picture_parity, test_render_quality and test_compose until 0.15). No stored pictures: every case is checked
// against something computed independently in the test - the source frame decoded by Video::Reader, another render
// whose result is known to be equal (speed 2 at frame k = speed 1 at 2k), or arithmetic on the pixels (a dissolve's
// middle = the mean of both sides, half opacity = the mean with the track below, a wipe = left side A, right side B).
// Also: titles/subtitles with outline (the outline stands its width beyond the letters on every side, the box its
// margin around it), strong downscales prefiltered (1-px stripes shrunk 4.3:1 = even gray), renders from several
// threads = renders one after the other, update() keeps the sources open.
// SCHNEIDI_PICTURE_DUMP=<folder>: saves the rendered pictures there to look at.

#include "render_check.h"

#include "core/EffectRegistry.h"
#include "core/ProjectFormat.h"
#include "engine/picture/Renderer.h"
#include "engine/video/Compose.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <atomic>
#include <clocale>
#include <cmath>
#include <functional>
#include <thread>

namespace {

QString g_pattern, g_pattern2;
const ProjectFormat kFormat{320, 180, {25, 1}};

QImage render(const Timeline& tl, int frame, QSize size = {}, const ProjectFormat& fmt = kFormat)
{
    const QImage img = Check::renderFrame(tl, fmt, frame, size);
    if (const QString dump = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP"); !dump.isEmpty() && !img.isNull()) {
        static int n = 0;
        img.save(QString("%1/regression-%2.png").arg(dump).arg(n++, 3, 10, QChar('0')));
    }
    return img;
}

struct Diff {
    double mean = 0; // per channel (RGB)
    int max = 0;
};

// Over the rectangle r (whole picture if empty), RGB only
Diff compare(const QImage& a, const QImage& b, QRect r = {})
{
    Diff d;
    if (a.isNull() || a.size() != b.size()) return {1e9, 999};
    if (r.isEmpty()) r = a.rect();
    double sum = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            for (int v : {qRed(p) - qRed(q), qGreen(p) - qGreen(q), qBlue(p) - qBlue(q)}) {
                sum += std::abs(v);
                d.max = std::max(d.max, std::abs(v));
            }
        }
    d.mean = sum / (3.0 * r.width() * r.height());
    return d;
}

bool expect(const char* name, const Diff& d, double meanTol, int maxTol = 256)
{
    const bool ok = d.mean <= meanTol && d.max <= maxTol;
    std::printf("  %s %-30s mean %.3f max %d\n", ok ? "  " : "!!", name, d.mean, d.max);
    return Check::report(ok, name, __FILE__, __LINE__);
}

Clip media(int id, const QString& path, int start, int in, int out)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = out;
    return c;
}

Timeline oneTrack(const QVector<Clip>& clips)
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips = clips;
    return tl;
}

// Source frame k of a file as the timeline shows it at 1:1 (a plain clip from in point 0)
QImage source(const QString& path, int k) { return render(oneTrack({media(1, path, 0, 0, 49)}), k); }

// Per pixel mean of two pictures
QImage mean(const QImage& a, const QImage& b, double wb = 0.5)
{
    QImage out(a.size(), QImage::Format_RGBA8888);
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            auto m = [&](int u, int v) { return int(std::lround(u * (1 - wb) + v * wb)); };
            out.setPixel(x, y, qRgba(m(qRed(p), qRed(q)), m(qGreen(p), qGreen(q)), m(qBlue(p), qBlue(q)), 255));
        }
    return out;
}

void testPlainAndTime()
{
    std::printf("plain / timing\n");
    const QImage s7 = source(g_pattern, 7);
    if (!CHECK(!s7.isNull() && s7.size() == QSize(320, 180))) return;
    // Different frames differ (else the timing checks below prove nothing); same frame twice is identical
    expect("frame 7 != frame 14 (sanity)", {compare(s7, source(g_pattern, 14)).mean > 1.0 ? 0.0 : 1.0, 0}, 0.0);
    expect("same frame twice", compare(s7, source(g_pattern, 7)), 0.0, 0);
    // A gap is black
    const QImage gap = render(oneTrack({media(1, g_pattern, 10, 0, 9)}), 3);
    QImage black(gap.size(), QImage::Format_RGBA8888);
    black.fill(Qt::black);
    expect("gap = black", compare(gap, black), 0.0, 0);
    // In point and start: timeline frame f shows source frame in + f - start
    expect("in/start offset", compare(render(oneTrack({media(1, g_pattern, 5, 10, 40)}), 9), source(g_pattern, 14)), 0.0, 0);
    Clip s = media(1, g_pattern, 0, 0, 20);
    s.speed = 2.0;
    expect("speed 2: frame 7 = source 14", compare(render(oneTrack({s}), 7), source(g_pattern, 14)), 0.0, 0);
    s.reverse = true;
    // reversed: source positions count from the file end (50 frames: in 0 = file frame 49); at speed 2 frame f plays
    // the file time from 50 - 2f down to 48 - 2f (as the sound does) and shows the frame where it starts, 49 - 2f
    expect("speed 2 reversed: frame 0 = file 49", compare(render(oneTrack({s}), 0), source(g_pattern, 49)), 0.0, 0);
    expect("speed 2 reversed: frame 5 = file 39", compare(render(oneTrack({s}), 5), source(g_pattern, 39)), 0.0, 0);
    s.speed = 1.0;
    expect("reversed: frame 3 = file 46", compare(render(oneTrack({s}), 3), source(g_pattern, 46)), 0.0, 0);
    s.reverse = false;
    s.speed = 0.5;
    // slow motion by the sound's time: frame f starts at file time f / 2 - frame 7 still shows file frame 3 (it
    // showed 4, half a file frame ahead of the sound)
    expect("speed 0.5: frame 6 = source 3", compare(render(oneTrack({s}), 6), source(g_pattern, 3)), 0.0, 0);
    expect("speed 0.5: frame 7 = source 3", compare(render(oneTrack({s}), 7), source(g_pattern, 3)), 0.0, 0);
    s.reverse = true; // from the end: frame 7 starts at file time 50 - 3.5 = 46.5 -> frame 46
    expect("speed 0.5 reversed: frame 7 = file 46", compare(render(oneTrack({s}), 7), source(g_pattern, 46)), 0.0, 0);
    s.reverse = false;
    s.speed = 1.0;
    s.freeze = true;
    s.freezeFrame = 12;
    expect("freeze = source 12", compare(render(oneTrack({s}), 3), source(g_pattern, 12)), 0.0, 0);
    // Half size: the picture shrunk (against the full picture shrunk smoothly)
    const QImage half = render(oneTrack({media(1, g_pattern, 0, 0, 49)}), 7, QSize(160, 90));
    expect("half size", compare(half, s7.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)), 3.0);
}

void testLayers()
{
    std::printf("layers\n");
    const QImage a = source(g_pattern, 10), b = source(g_pattern2, 10);
    Timeline tl = oneTrack({media(1, g_pattern2, 0, 0, 49)});
    tl.video.resize(2);
    tl.video[1].clips = {media(2, g_pattern, 0, 0, 49)};
    // Half opacity: the mean with the track below
    tl.video[1].clips[0].transform.opacity = 50;
    expect("opacity 50 = mean", compare(render(tl, 10), mean(b, a)), 1.0, 3);
    // Zoom 0.5 at the centre: the middle quarter shows the top picture shrunk, the border the track below
    tl.video[1].clips[0].transform = {};
    tl.video[1].clips[0].transform.zoomX = tl.video[1].clips[0].transform.zoomY = 0.5;
    const QImage z = render(tl, 10);
    const QImage small = a.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage want = b.copy();
    QPainter(&want).drawImage(80, 45, small);
    expect("zoom 0.5: centre", compare(z, want, QRect(82, 47, 156, 86)), 5.0);
    expect("zoom 0.5: border = below", compare(z, b, QRect(0, 0, 320, 40)), 0.0, 0);
    // Position: moved 40 px right, 20 px up (Y positive upward)
    tl.video[1].clips[0].transform.posX = 40;
    tl.video[1].clips[0].transform.posY = 20;
    QImage moved = b.copy();
    QPainter(&moved).drawImage(120, 25, small);
    expect("zoom 0.5 + position", compare(render(tl, 10), moved, QRect(122, 27, 156, 86)), 5.0);
    // Crop: the cut part shows the track below, the rest stays in place
    tl.video[1].clips[0].transform = {};
    tl.video[1].clips[0].transform.cropLeft = 40;
    const QImage cropped = render(tl, 10);
    expect("crop: cut part = below", compare(cropped, b, QRect(0, 0, 38, 180)), 1.0);
    expect("crop: rest = top", compare(cropped, a, QRect(44, 0, 276, 180)), 0.0, 0);
    // Rotation 180: the picture upside down
    tl.video[1].clips[0].transform = {};
    tl.video[1].clips[0].transform.rotation = 180;
    expect("rotation 180", compare(render(tl, 10), a.mirrored(true, true), QRect(2, 2, 316, 176)), 0.5);
}

void testTransitions()
{
    std::printf("fades / transitions\n");
    Clip a = media(1, g_pattern, 0, 0, 49);
    a.fadeIn = 10;
    const QImage black(320, 180, QImage::Format_RGBA8888);
    QImage zero = black.copy();
    zero.fill(Qt::black);
    // Fade from black: frame 5 of 10 = half of the picture
    expect("fade in: half way", compare(render(oneTrack({a}), 5), mean(zero, source(g_pattern, 5))), 1.5, 3);
    // Dissolve of 10 frames centred on the cut at 25: frame 25 = mean of both sides
    Clip b = media(1, g_pattern, 0, 0, 24);
    Clip c = media(2, g_pattern2, 25, 10, 40);
    b.transOut = 10;
    c.transIn = 10;
    const QImage left = source(g_pattern, 25), right = render(oneTrack({media(2, g_pattern2, 0, 0, 49)}), 10);
    expect("dissolve middle = mean", compare(render(oneTrack({b, c}), 25), mean(left, right)), 1.5, 4);
    expect("dissolve before = A", compare(render(oneTrack({b, c}), 19), source(g_pattern, 19)), 0.0, 0);
    // Only A is drawn before the dissolve: no decoder of its own for B (the transition asks for B's length; that
    // used to open a decoder per file that stayed for good)
    for (const Picture::Mode mode : {Picture::Mode::Preview, Picture::Mode::Export}) {
        Picture::Options o;
        o.format = kFormat;
        o.mode = mode;
        Picture::Renderer r(oneTrack({b, c}), o);
        r.render(5);
        CHECK_EQ(r.stats().sourcesOpen, 1);
    }
    // Wipe to the right, half way: left half B, right half A
    b.transOutStyle.type = c.transInStyle.type = TransitionType::WipeRight;
    const QImage w = render(oneTrack({b, c}), 25);
    expect("wipe: left = B", compare(w, right, QRect(0, 0, 150, 180)), 0.0, 0);
    expect("wipe: right = A", compare(w, left, QRect(170, 0, 150, 180)), 0.0, 0);
    // Wipes in every direction and with a soft edge, the iris (luma "Kreis auf"): rendered again in the same
    // renderer they look the same (the gradient is built once and shared), and the iris half way shows B in the
    // middle and A in the corners
    for (const TransitionType type : {TransitionType::WipeLeft, TransitionType::WipeDown, TransitionType::WipeUp,
                                      TransitionType::Luma}) {
        Clip bb = b, cc = c;
        bb.transOutStyle.type = cc.transInStyle.type = type;
        bb.transOutStyle.softness = cc.transInStyle.softness = 30;
        bb.transOutStyle.luma = cc.transInStyle.luma = ":/lumas/kreis";
        const QImage first = render(oneTrack({bb, cc}), 23);
        CHECK(!first.isNull());
        Picture::Options o;
        o.format = kFormat;
        o.mode = Picture::Mode::Export;
        Picture::Renderer r(oneTrack({bb, cc}), o);
        for (int f : {22, 23}) {
            const QImage again = r.render(f).toImage();
            if (f == 23) expect("wipe/iris rendered again = same", compare(again, first), 0.0, 0);
        }
        // The gradient is built once per style and size: ahead by prepare() (playback reaching the transition),
        // not again for its frames (an iris at 1080p took 75 ms per frame to build, the frame itself ~10 ms)
        o.mode = Picture::Mode::Preview;
        Picture::Renderer p(oneTrack({bb, cc}), o);
        p.prepare(5);
        CHECK_EQ(p.stats().gradients, 1);
        for (int f = 20; f < 30; ++f) p.render(f);
        CHECK_EQ(p.stats().gradients, 1);
        if (type == TransitionType::Luma) {
            const QImage iris = render(oneTrack({bb, cc}), 26);
            expect("iris: middle = B", compare(iris, render(oneTrack({media(2, g_pattern2, 0, 0, 49)}), 11),
                                                QRect(140, 70, 40, 40)), 0.0, 0);
            expect("iris: corner = A", compare(iris, source(g_pattern, 26), QRect(0, 0, 20, 20)), 0.0, 0);
        }
    }
    // Dip to black: darkest at the cut
    b.transOutStyle.type = c.transInStyle.type = TransitionType::DipToColor;
    expect("dip: black at the cut", compare(render(oneTrack({b, c}), 25), zero), 3.0);
}

// Bounding box of the pixels that pass a test (empty: right < left)
struct Box {
    int left = 1 << 30, top = 1 << 30, right = -1, bottom = -1;
    bool empty() const { return right < left; }
};

Box bounds(const QImage& img, const std::function<bool(QRgb)>& pass)
{
    Box b;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (pass(img.pixel(x, y))) {
                b.left = std::min(b.left, x);
                b.right = std::max(b.right, x);
                b.top = std::min(b.top, y);
                b.bottom = std::max(b.bottom, y);
            }
    return b;
}

// White text, red outline T px, optional opaque blue box with margin pad, over nothing (transparent): the outline must
// stick out T px beyond the letters on all four sides (also at the line ends), the box pad px around the outline.
// grow: the title's own size, outline and margin are 1/grow of that, its size animated (keyframe) to the full size:
// outline and margin grow with it (the same picture as the plain title)
void testOutline(const char* name, bool subtitle, double T, bool box, double pad, double grow = 1)
{
    const ProjectFormat fmt{1280, 720, {25, 1}};
    TitleStyle t;
    t.text = subtitle ? "HALLO WELT HALLO WELT HALLO" : "HALLO WELT HALLO\nZWEITE ZEILE HH";
    t.size = 72;
    t.color = Qt::white;
    t.outlineOn = true;
    t.outlineWidth = T;
    t.outlineColor = QColor(255, 0, 0);
    t.boxOn = box;
    t.boxPad = pad;
    t.boxColor = QColor(0, 0, 255);
    t.align = 1;
    t.posY = subtitle ? 60 : 0;
    Timeline tl;
    tl.video.resize(1);
    if (subtitle) {
        SubtitleTrack st;
        st.style = t;
        st.cues = {{1, 0, 10, t.text}};
        tl.subtitles = {st};
    } else {
        Clip c;
        c.id = 1;
        c.kind = ClipKind::Title;
        c.out = 9;
        c.title = t;
        if (grow != 1) {
            c.title.size = t.size / grow;
            c.keys[AnimParam::TitleSize] = {Keyframe{0, t.size}};
            c.title.outlineWidth = T / grow;
            c.title.boxPad = pad / grow;
        }
        tl.video[0].clips = {c};
    }
    const QImage img = Check::renderFrame(tl, fmt, 0, {}, true).convertToFormat(QImage::Format_ARGB32);
    if (!CHECK(!img.isNull())) return;
    if (const QString dump = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP"); !dump.isEmpty()) { // over gray to look at
        QImage shown(img.size(), QImage::Format_ARGB32);
        shown.fill(QColor(90, 90, 90));
        QPainter(&shown).drawImage(0, 0, img);
        shown.save(QString("%1/%2.png").arg(dump, QString(name).replace(' ', '_').replace(',', "").replace('+', "and")));
    }
    const Box text = bounds(img, [](QRgb p) { return qAlpha(p) > 128 && qGreen(p) > 128; });
    const Box ink = bounds(img, [](QRgb p) { return qAlpha(p) > 128 && qRed(p) > 128; }); // text + outline
    if (!CHECK(!text.empty() && !ink.empty())) return;
    const int l = text.left - ink.left, r = ink.right - text.right, u = text.top - ink.top, d = ink.bottom - text.bottom;
    std::printf("  %-28s outline beyond the letters: left %d right %d top %d bottom %d (T %.0f)\n", name, l, r, u, d, T);
    const double tol = 1.5;
    CHECK(std::abs(l - T) <= tol && std::abs(r - T) <= tol && std::abs(u - T) <= tol && std::abs(d - T) <= tol);
    if (box) {
        const Box all = bounds(img, [](QRgb p) { return qAlpha(p) > 128; });
        const int bl = ink.left - all.left, br = all.right - ink.right;
        std::printf("  %-28s box beyond the outline: left %d right %d (pad %.0f)\n", name, bl, br, pad);
        // The glyphs' side bearings add a few pixels; both sides alike
        CHECK(bl >= pad - 1 && br >= pad - 1 && std::abs(bl - br) <= 2);
    }
}

// A title with zoom/rotation/position is drawn only as large as its text and placed: the same as the title alone
// rendered, then transformed - checked by its extent and by being unchanged without a transform
void testTitleTransform()
{
    std::printf("title transform\n");
    Clip t;
    t.id = 5;
    t.kind = ClipKind::Title;
    t.title.text = "Hallo";
    t.title.size = 40;
    t.out = 49;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips = {t};
    auto inkOf = [](const QImage& img) { return bounds(img, [](QRgb p) { return qAlpha(p) > 128; }); };
    const Box plain = inkOf(Check::renderFrame(tl, kFormat, 5, {}, true));
    tl.video[0].clips[0].transform.zoomX = tl.video[0].clips[0].transform.zoomY = 2.0;
    tl.video[0].clips[0].transform.posX = 40;
    const Box zoomed = inkOf(Check::renderFrame(tl, kFormat, 5, {}, true));
    if (!CHECK(!plain.empty() && !zoomed.empty())) return;
    const double pw = plain.right - plain.left + 1, zw = zoomed.right - zoomed.left + 1;
    const double pc = (plain.left + plain.right) / 2.0, zc = (zoomed.left + zoomed.right) / 2.0;
    std::printf("  width %.0f -> %.0f, centre %.1f -> %.1f\n", pw, zw, pc, zc);
    CHECK(std::abs(zw / pw - 2.0) < 0.08);
    CHECK(std::abs((zc - pc) - 40) < 2.0);
    // Rotated 90 degrees: the text stands upright, as wide as it was high
    tl.video[0].clips[0].transform = {};
    tl.video[0].clips[0].transform.rotation = 90;
    const Box turned = inkOf(Check::renderFrame(tl, kFormat, 5, {}, true));
    CHECK(!turned.empty() && std::abs((turned.bottom - turned.top + 1) - pw) <= 2);
}

double stddev(const std::vector<uint8_t>& px, int w, int x0, int y0, int x1, int y1)
{
    double sum = 0, sq = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const double v = px[(size_t(y) * w + x) * 4];
            sum += v;
            sq += v * v;
            ++n;
        }
    const double m = sum / n;
    return std::sqrt(std::max(0.0, sq / n - m * m));
}

// 1-px stripes shrunk 4.3:1 (Compose prefilters 2:1 and more with mip steps): even gray instead of random stripes
void testDownscale()
{
    std::printf("downscale prefilter\n");
    const int sw = 1376, sh = 774, dw = 320, dh = 180;
    std::vector<uint8_t> src(size_t(sw) * sh * 4);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            uint8_t* p = src.data() + (size_t(y) * sw + x) * 4;
            p[0] = p[1] = p[2] = (x & 1) ? 255 : 0;
            p[3] = 255;
        }
    std::vector<uint8_t> dst(size_t(dw) * dh * 4, 0);
    Compose::Affine m;
    m.a = double(dw) / sw;
    m.e = double(dh) / sh;
    Compose::draw(dst.data(), dw, dh, src.data(), sw, sh, m, 1.0);
    const double sd = stddev(dst, dw, 2, 2, dw - 2, dh - 2);
    std::printf("  1-px stripes shrunk %.1f:1: spread of the gray %.1f (even gray = 0)\n", double(sw) / dw, sd);
    CHECK(sd < 8);
    CHECK(std::abs(dst[(90 * dw + 160) * 4] - 128) < 20); // the mean of black and white
    // Rotated a little as well: still even
    std::fill(dst.begin(), dst.end(), 0);
    const double rad = 0.2, k = 0.25;
    Compose::Affine r{k * std::cos(rad), -k * std::sin(rad), 60, k * std::sin(rad), k * std::cos(rad), -20};
    Compose::draw(dst.data(), dw, dh, src.data(), sw, sh, r, 1.0);
    const double sdr = stddev(dst, dw, 100, 70, 200, 120);
    std::printf("  1-px stripes shrunk 4:1, rotated: spread %.1f\n", sdr);
    CHECK(sdr < 12);
}

// render() from several threads at once (export workers) = one after the other; update() takes a change, nothing
// reopened
void testThreads()
{
    std::printf("threads\n");
    Timeline tl = oneTrack({media(1, g_pattern2, 0, 0, 49)});
    tl.video.resize(2);
    Clip top = media(2, g_pattern, 0, 0, 49);
    top.transform.zoomX = top.transform.zoomY = 0.6;
    top.transform.rotation = 10;
    top.fadeIn = 8;
    tl.video[1].clips = {top};
    Picture::Options o;
    o.format = kFormat;
    o.parallelFrames = 4;
    Picture::Renderer serial(tl, o);
    QVector<QImage> want;
    for (int f = 0; f < 40; ++f) want << serial.render(f).toImage();
    Picture::Renderer parallel(tl, o);
    QVector<QImage> got(40);
    std::atomic<int> next{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] {
            for (int f; (f = next++) < 40;) got[f] = parallel.render(f).toImage();
        });
    for (std::thread& t : threads) t.join();
    int same = 0;
    for (int f = 0; f < 40; ++f) same += got[f] == want[f];
    CHECK_EQ(same, 40);
    const int open = parallel.stats().sourcesOpen;
    tl.video[1].clips[0].transform = {};
    parallel.update(tl);
    CHECK(parallel.render(20).toImage() == source(g_pattern, 20).convertToFormat(parallel.render(20).toImage().format()));
    CHECK_EQ(parallel.stats().sourcesOpen, open);
}

// A VP9 WebM with alpha (the container says yuv420p, only libvpx gives yuva420p) over a white clip: where it is
// transparent the white below shows (it was taken for opaque and covered the tracks below)
void testAlphaWebm(const QString& dir)
{
    std::printf("alpha WebM\n");
    const QString white = Check::makeMedia(dir + "/white.mp4", {"-f", "lavfi", "-i", "color=c=white:s=320x180:r=25:d=2",
                                                                "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                                                "yuv420p"});
    // Left half red and opaque, right half fully transparent
    const QString alpha = Check::makeMedia(
        dir + "/alpha.webm", {"-f", "lavfi", "-i",
                              "color=c=red:s=320x180:r=25:d=2,format=rgba,geq=r=255:g=0:b=0:a='if(lt(X,160),255,0)'",
                              "-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p"});
    if (!CHECK(!white.isEmpty() && !alpha.isEmpty())) return;
    Timeline tl = oneTrack({media(1, white, 0, 0, 49)});
    tl.video.resize(2);
    tl.video[1].clips = {media(2, alpha, 0, 0, 49)};
    QImage want(320, 180, QImage::Format_RGBA8888);
    want.fill(Qt::white);
    QPainter(&want).fillRect(0, 0, 160, 180, QColor(255, 0, 0));
    for (const Picture::Mode mode : {Picture::Mode::Export, Picture::Mode::Preview}) {
        Picture::Options o;
        o.format = kFormat;
        o.mode = mode;
        Picture::Renderer r(tl, o);
        const QImage img = r.render(10).toImage();
        const char* m = mode == Picture::Mode::Export ? "export" : "preview";
        expect(qPrintable(QString("%1: right half = white below").arg(m)), compare(img, want, QRect(170, 0, 150, 180)), 1.0, 6);
        expect(qPrintable(QString("%1: left half = red").arg(m)), compare(img, want, QRect(0, 0, 150, 180)), 3.0);
    }
}

// Several enabled subtitle tracks are all drawn (two speakers): one low, one high; a hidden one is not
void testSubtitleTracks()
{
    std::printf("subtitle tracks\n");
    auto track = [](const QString& text, double posY, QColor color, bool enabled) {
        SubtitleTrack st;
        st.style.size = 24;
        st.style.color = color;
        st.style.posY = posY;
        st.enabled = enabled;
        st.cues = {{1, 0, 10, text}};
        return st;
    };
    Timeline tl;
    tl.video.resize(1);
    tl.subtitles = {track("UNTEN", 10, Qt::white, true), track("OBEN", 120, Qt::white, true),
                    track("AUS", 70, Qt::white, false)};
    auto inkIn = [](const QImage& img, int y0, int y1) {
        int n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width(); ++x) n += qAlpha(img.pixel(x, y)) > 128 && qRed(img.pixel(x, y)) > 128;
        return n;
    };
    for (const Picture::Mode mode : {Picture::Mode::Export, Picture::Mode::Preview}) {
        Picture::Options o;
        o.format = kFormat;
        o.mode = mode;
        o.transparent = true;
        Picture::Renderer r(tl, o);
        const QImage img = r.render(3).toImage().convertToFormat(QImage::Format_ARGB32);
        // posY = the text's bottom above the frame's bottom edge: 10 -> rows up to 170, 120 -> rows up to 60,
        // the hidden one would end at row 110
        const int low = inkIn(img, 130, 172), high = inkIn(img, 20, 62), hidden = inkIn(img, 75, 112);
        std::printf("  %s: ink low %d, high %d, hidden track %d\n", mode == Picture::Mode::Export ? "export" : "preview",
                    low, high, hidden);
        CHECK(low > 50);
        CHECK(high > 50);
        CHECK_EQ(hidden, 0);
    }
}

// A white title with the blur effect stays white where its edge fades out (the blur works premultiplied: the
// transparent background has no colour to pull in, it turned the soft edge dark gray); over a white clip no dark halo
void testTitleBlur()
{
    std::printf("title blur\n");
    Clip t;
    t.id = 5;
    t.kind = ClipKind::Title;
    t.title.text = "HALLO";
    t.title.size = 60;
    t.title.color = Qt::white;
    t.out = 49;
    EffectRegistry::add(t, "blur");
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips = {t};
    const QImage img = Check::renderFrame(tl, kFormat, 5, {}, true).convertToFormat(QImage::Format_ARGB32);
    if (!CHECK(!img.isNull())) return;
    int soft = 0, darkest = 255;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (qAlpha(p) < 24 || qAlpha(p) > 230) continue;
            ++soft;
            darkest = std::min(darkest, qGray(p));
        }
    std::printf("  soft edge: %d pixels, darkest %d\n", soft, darkest);
    CHECK(soft > 100);
    CHECK(darkest > 220);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("picture_regression");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_pattern = Check::makeMedia(tmp.filePath("pattern.mp4"), {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=2",
                                                               "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                                               "yuv420p"});
    g_pattern2 = Check::makeMedia(tmp.filePath("pattern2.mp4"), {"-f", "lavfi", "-i", "smptebars=s=320x180:r=25:d=2",
                                                                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                                                 "yuv420p"});
    if (!CHECK(!g_pattern.isEmpty() && !g_pattern2.isEmpty())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testPlainAndTime();
    testLayers();
    testTransitions();
    std::printf("outline\n");
    testOutline("title, outline 8", false, 8, false, 0);
    testOutline("title, outline 4 + box 20", false, 4, true, 20);
    testOutline("title size animated x2", false, 4, true, 20, 2);
    testOutline("subtitle, outline 6", true, 6, false, 0);
    testOutline("subtitle, outline 6 + box 10", true, 6, true, 10);
    testTitleTransform();
    testDownscale();
    testThreads();
    testAlphaWebm(tmp.path());
    testSubtitleTracks();
    testTitleBlur();
    return Check::result();
}
