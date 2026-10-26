// Crop (Inspector, CLI op 'transform'): values are timeline pixels on the clip's picture as it fits into the frame at
// zoom 1, cut off the picture's own edges, the same at every zoom and timeline shape; cropped areas and letterbox bars
// stay transparent (the track below shows). AI test runs of 2026-10-25 found it measured on the frame at zoom 1
// (letterbox included, the bars inside the kept area opaque black) and as a share of the frame size laid onto the
// picture otherwise (x of the width, y of the height). Renders a red 16:9 clip over a green track in a 9:16 timeline,
// in the preview (RGBA) and export (YUV) formats and at half size, and checks the core conversions (new timeline
// resolution, the engine's picture shape probe).

#include "check.h"

#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/ProjectFormat.h"
#include "engine/Engine.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <functional>

namespace {

const ProjectFormat kTall{180, 320, {25, 1}}; // 9:16
const ProjectFormat kWide{320, 180, {25, 1}}; // 16:9, the clip's own shape
QString g_red, g_green, g_greenWide; // green backgrounds in the shape of each timeline

enum class Px { Black, Red, Green, Other };

struct Picture {
    int w = 0, h = 0;
    QVector<Px> px;
    Px at(int x, int y) const { return px[y * w + x]; }
};

Picture render(const Timeline& tl, const ProjectFormat& fmt, mlt_image_format format, int frame, double scale = 1.0)
{
    auto profile = makeProfile(fmt);
    TimelineBuilder b(*profile);
    auto tractor = b.build(tl);
    tractor->seek(frame);
    std::unique_ptr<Mlt::Frame> f(tractor->get_frame());
    Picture pic;
    pic.w = int(fmt.width * scale);
    pic.h = int(fmt.height * scale);
    mlt_image_format got = format;
    int w = pic.w, h = pic.h;
    const uint8_t* img = f->get_image(got, w, h);
    if (!CHECK(img && w == pic.w && h == pic.h && got == format)) return {};
    pic.px.resize(w * h);
    for (int i = 0; i < w * h; ++i) {
        if (format == mlt_image_rgba) {
            const uint8_t* p = img + 4 * i;
            pic.px[i] = p[0] + p[1] + p[2] < 90                ? Px::Black
                        : p[0] > 150 && p[1] < 100            ? Px::Red
                        : p[1] > 150 && p[0] < 100            ? Px::Green
                                                              : Px::Other;
        } else { // yuv420p: luma decides (BT.709 limited: black 16, red ~63, green ~173)
            const int y = img[i];
            pic.px[i] = y < 35 ? Px::Black : y > 45 && y < 100 ? Px::Red : y > 140 && y < 205 ? Px::Green : Px::Other;
        }
    }
    return pic;
}

// Red area: first/last row and column with red in the middle column/row of the red area; every other pixel must
// be green (no opaque black bars, no other colors apart from blended edge pixels)
struct Box {
    int top = -1, bottom = -1, left = -1, right = -1; // inclusive pixel rows/columns
    int black = 0;
};

Box redBox(const Picture& pic)
{
    Box b;
    for (int y = 0; y < pic.h; ++y)
        for (int x = 0; x < pic.w; ++x) {
            const Px p = pic.at(x, y);
            if (p == Px::Black) ++b.black;
            if (p != Px::Red) continue;
            if (b.top < 0) b.top = y;
            b.bottom = y;
            if (b.left < 0 || x < b.left) b.left = x;
            b.right = std::max(b.right, x);
        }
    return b;
}

// Edges in pixel positions (top/left = first red row/column, bottom/right = end, exclusive); blended edge rows count
// either way -> 1.5 px tolerance
bool near(int got, double want) { return std::abs(got - want) <= 1.5; }

void checkBox(const Box& b, double top, double bottom, double left, double right, const char* what)
{
    const bool ok = near(b.top, top) && near(b.bottom + 1, bottom) && near(b.left, left) && near(b.right + 1, right);
    if (!Check::report(ok, what, __FILE__, __LINE__))
        std::printf("       red %d..%d x %d..%d, expected %.1f..%.1f x %.1f..%.1f\n", b.top, b.bottom + 1, b.left,
                    b.right + 1, top, bottom, left, right);
    if (!Check::report(b.black == 0, what, __FILE__, __LINE__))
        std::printf("       %d opaque black pixels (letterbox/crop not transparent)\n", b.black);
}

Timeline overGreen(const std::function<void(Clip&)>& setup, const QString& background = g_green)
{
    Timeline tl;
    tl.video.resize(2);
    Clip low;
    low.id = 1;
    low.mediaPath = background;
    low.out = 49;
    tl.video[0].clips << low;
    Clip up;
    up.id = 2;
    up.mediaPath = g_red;
    up.out = 49;
    setup(up);
    tl.video[1].clips << up;
    return tl;
}

void crop(Clip& c, double l, double r, double t, double b)
{
    c.transform.cropLeft = l;
    c.transform.cropRight = r;
    c.transform.cropTop = t;
    c.transform.cropBottom = b;
}

void testRender()
{
    // 16:9 in 180x320: the picture is 180 x 101.25, rows 109.375..210.625
    const double top = (320 - 101.25) / 2, bottom = top + 101.25;
    for (mlt_image_format f : {mlt_image_rgba, mlt_image_yuv420p}) {
        checkBox(redBox(render(overGreen([](Clip&) {}), kTall, f, 5)), top, bottom, 0, 180, "letterbox transparent");
        // zoom 1: 60/70 px off the sides, 10/5 px off the picture's top/bottom (not the frame's), bars stay see-through
        checkBox(redBox(render(overGreen([](Clip& c) { crop(c, 60, 70, 10, 5); }), kTall, f, 5)), top + 10, bottom - 5,
                 60, 110, "crop at zoom 1");
        // zoom 2: the same crop is twice as large on screen (crop comes before zoom), picture 360x202.5 centered
        const double t2 = 160 - 101.25, b2 = 160 + 101.25;
        checkBox(redBox(render(overGreen([](Clip& c) {
                                   crop(c, 60, 70, 10, 5);
                                   c.transform.zoomX = c.transform.zoomY = 2;
                               }),
                               kTall, f, 5)),
                 t2 + 20, b2 - 10, -90 + 120, 270 - 140, "crop at zoom 2");
    }
    // Position moves the cropped picture along; the crop switched off (red dot) leaves it whole
    checkBox(redBox(render(overGreen([](Clip& c) {
                               crop(c, 60, 70, 10, 5);
                               c.transform.posX = 20;
                               c.transform.posY = 30; // up
                           }),
                           kTall, mlt_image_rgba, 5)),
             top + 10 - 30, bottom - 5 - 30, 80, 130, "crop with position");
    // More than half from one side (the Inspector allows the whole frame): only the right strip stays
    checkBox(redBox(render(overGreen([](Clip& c) { crop(c, 135, 0, 0, 0); }), kTall, mlt_image_rgba, 5)), top, bottom,
             135, 180, "crop past the middle");
    checkBox(redBox(render(overGreen([](Clip& c) {
                               crop(c, 60, 70, 10, 5);
                               c.transform.cropOn = false;
                           }),
                           kTall, mlt_image_rgba, 5)),
             top, bottom, 0, 180, "crop off");
    // A transform switched off keeps zoom 1 under the crop
    checkBox(redBox(render(overGreen([](Clip& c) {
                               crop(c, 60, 70, 10, 5);
                               c.transform.zoomX = c.transform.zoomY = 2;
                               c.transform.transformOn = false;
                           }),
                           kTall, mlt_image_rgba, 5)),
             top + 10, bottom - 5, 60, 110, "crop with the transform off");
    // Preview at half size: everything halves
    checkBox(redBox(render(overGreen([](Clip& c) {
                               crop(c, 60, 70, 10, 5);
                               c.transform.zoomX = c.transform.zoomY = 2;
                           }),
                           kTall, mlt_image_rgba, 5, 0.5)),
             (160 - 101.25 + 20) / 2, (160 + 101.25 - 10) / 2, 15, 65, "half size");
    // Animated: crop_top 0 -> 50 over clip frames 0..20, at frame 10 it is 25
    checkBox(redBox(render(overGreen([](Clip& c) {
                               Keys::setKey(c, AnimParam::CropTop, 0, 0);
                               Keys::setKey(c, AnimParam::CropTop, 20, 50);
                           }),
                           kTall, mlt_image_rgba, 10)),
             top + 25, bottom, 0, 180, "animated crop");
    // A clip shaped like the timeline: timeline pixels of the frame, as before
    checkBox(redBox(render(overGreen([](Clip& c) { crop(c, 60, 70, 10, 5); }, g_greenWide), kWide, mlt_image_rgba, 5)),
             10, 175, 60, 250, "same shape, zoom 1");
    checkBox(redBox(render(overGreen(
                               [](Clip& c) {
                                   crop(c, 60, 70, 50, 45);
                                   c.transform.zoomX = c.transform.zoomY = 2;
                               },
                               g_greenWide),
                           kWide, mlt_image_rgba, 5)),
             -90 + 100, 270 - 90, 0, 320, "same shape, zoom 2");
}

void testProbe(const QString& dir)
{
    // The engine's probe (installed by Engine::initMlt) measures the picture as the TimelineBuilder places it
    CHECK(std::abs(Engine::pictureAspect(g_red) - 16.0 / 9) < 0.01);
    CHECK(std::abs(Engine::pictureAspect(g_green) - 9.0 / 16) < 0.01);
    const QString png = Check::makeMedia(dir + "/square.png", {"-f", "lavfi", "-i", "color=c=blue:s=40x30", "-frames:v", "1"});
    if (CHECK(!png.isEmpty())) CHECK(std::abs(Engine::pictureAspect(png) - 4.0 / 3) < 0.01);
    CHECK_EQ(Engine::pictureAspect(dir + "/missing.mp4"), 0.0);
}

void testNewResolution()
{
    // Crop scales with the fitted picture, which keeps its shape: landscape 1920x1080 -> portrait 1080x1920
    MediaAspect::setProbe([](const QString& path) { return path.endsWith("wide.mp4") ? 16.0 / 9 : 0.0; });
    QTemporaryDir dir;
    const QString wide = dir.filePath("wide.mp4"), unknown = dir.filePath("other.mp4");
    for (const QString& p : {wide, unknown}) {
        QFile f(p);
        CHECK(f.open(QIODevice::WriteOnly));
    }
    Timeline tl;
    tl.video.resize(1);
    Clip m;
    m.id = 1;
    m.mediaPath = wide;
    m.out = 9;
    crop(m, 100, 0, 200, 0);
    Keys::setKey(m, AnimParam::CropBottom, 0, 40);
    Clip u = m;
    u.id = 2;
    u.start = 10;
    u.mediaPath = unknown; // shape unknown: taken as shaped like the old frame
    Clip t;
    t.id = 3;
    t.start = 20;
    t.out = 9;
    t.kind = ClipKind::Title; // a title is the frame itself
    crop(t, 100, 0, 200, 0);
    tl.video[0].clips << m << u << t;
    scaleTimeline(tl, {1920, 1080}, {1080, 1920});
    const auto& c = tl.video[0].clips;
    CHECK(std::abs(c[0].transform.cropLeft - 56.25) < 1e-9);
    CHECK(std::abs(c[0].transform.cropTop - 112.5) < 1e-9);
    CHECK(std::abs(c[0].keys.value(AnimParam::CropBottom).value(0).value - 22.5) < 1e-9);
    CHECK(std::abs(c[1].transform.cropTop - 112.5) < 1e-9);
    CHECK(std::abs(c[2].transform.cropLeft - 56.25) < 1e-9);
    CHECK(std::abs(c[2].transform.cropTop - 200 * 1920.0 / 1080) < 1e-9);
    // Same shape (resolution only): every crop scales like the frame
    Timeline hd = tl;
    scaleTimeline(hd, {1080, 1920}, {2160, 3840});
    CHECK(std::abs(hd.video[0].clips[0].transform.cropTop - 225) < 1e-9);
    MediaAspect::setProbe({});
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("crop");
    testNewResolution();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_red = Check::makeMedia(tmp.filePath("red.mp4"), {"-f", "lavfi", "-i", "color=c=red:s=320x180:r=25:d=2", "-c:v",
                                                        "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    g_green = Check::makeMedia(tmp.filePath("green.mp4"), {"-f", "lavfi", "-i", "color=c=0x00ff00:s=180x320:r=25:d=2",
                                                            "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    g_greenWide = Check::makeMedia(tmp.filePath("greenwide.mp4"), {"-f", "lavfi", "-i", "color=c=0x00ff00:s=320x180:r=25:d=2",
                                                                    "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                                                    "yuv420p"});
    if (!CHECK(!g_red.isEmpty() && !g_green.isEmpty() && !g_greenWide.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testRender();
    testProbe(tmp.path());
    return Check::result();
}
