// SPDX-License-Identifier: AGPL-3.0-only
// Transitions look like the clips on both sides at every output size. An AI test run of 2026-10-25 saw a cross
// dissolve show the incoming clip as a small rectangle in the top left corner (frames of a 1920x1080 timeline at
// 640 px): the outgoing clip had keyframed zoom ending at exactly 1. Where the rectangle is the whole frame, MLT's
// qtblend filter passes the picture through in YUV at its own (media) size instead of the requested one, and luma
// composites at the size of its first input with the second at the requested size -> the second ended up in a
// corner (or only its corner showed, when it was the larger one).
// Renders the middle of a dissolve from a red to a green clip (both with a white box in the bottom right quarter)
// where one side is at zoom 1 through keyframes, and compares it with the same timeline without those keyframes
// (same picture, no qtblend on that side). Wide and tall timelines, 16:9 letterboxed in 9:16, sources larger than the
// timeline (so the export size is affected too), preview sizes, RGBA (preview) and YUV (export), crop, a wipe and the
// reported effect mix.

#include "check.h"

#include "core/Keyframes.h"
#include "core/ProjectFormat.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <cstring>
#include <functional>

namespace {

const ProjectFormat kWide{320, 180, {25, 1}};
const ProjectFormat kTall{180, 320, {25, 1}};
// Twice the timeline size: qtblend's pass-through hands out this size even at export size
QString g_redWide, g_greenWide, g_redTall, g_greenTall;

struct Picture {
    int w = 0, h = 0;
    int channels = 4; // RGBA, or 1 = the luma plane of YUV
    QVector<uint8_t> px;
    int value(int x, int y, int ch = 0) const { return px[channels * (y * w + x) + ch]; }
};

Picture render(const Timeline& tl, const ProjectFormat& fmt, mlt_image_format format, int frame, double scale)
{
    auto profile = makeProfile(fmt);
    TimelineBuilder b(*profile);
    auto tractor = b.build(tl);
    tractor->seek(frame);
    std::unique_ptr<Mlt::Frame> f(tractor->get_frame());
    Picture pic;
    pic.w = int(fmt.width * scale);
    pic.h = int(fmt.height * scale);
    pic.channels = format == mlt_image_rgba ? 4 : 1;
    mlt_image_format got = format;
    int w = pic.w, h = pic.h;
    const uint8_t* img = f->get_image(got, w, h);
    if (!CHECK(img && w == pic.w && h == pic.h && got == format)) return {};
    pic.px.assign(img, img + pic.channels * w * h);
    return pic;
}

// Red clip with a transition into a green clip at frame 25 (frames 20..29), both with handles
Timeline transition(const std::function<void(Clip&, Clip&)>& setup, const QString& red, const QString& green,
                    TransitionType type)
{
    Timeline tl;
    tl.video.resize(1);
    Clip a;
    a.id = 1;
    a.mediaPath = red;
    a.out = 24;
    a.transOut = 10;
    a.transOutStyle.type = type;
    Clip b;
    b.id = 2;
    b.mediaPath = green;
    b.start = 25;
    b.in = 10;
    b.out = 39;
    b.transIn = 10;
    b.transInStyle.type = type;
    setup(a, b);
    tl.video[0].clips << a << b;
    return tl;
}

// Zoom keyframes that reach exactly 1 inside the transition (at frame 25 of the timeline the zoom is 1)
void outgoingToOne(Clip& a, Clip&)
{
    for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY}) {
        Keys::setKey(a, p, 0, 1.15);
        Keys::setKey(a, p, 24, 1.0);
    }
}

void incomingFromOne(Clip&, Clip& b)
{
    for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY}) {
        Keys::setKey(b, p, 0, 1.0);
        Keys::setKey(b, p, 29, 1.2);
    }
}

// TRANSITION_DUMP=<folder>: both pictures of a failed comparison as PNG
void dump(const Picture& pic, const QString& name)
{
    const QString dir = qEnvironmentVariable("TRANSITION_DUMP");
    if (dir.isEmpty() || pic.w == 0) return;
    QDir().mkpath(dir);
    QImage img(pic.w, pic.h, pic.channels == 4 ? QImage::Format_RGBA8888 : QImage::Format_Grayscale8);
    for (int y = 0; y < pic.h; ++y)
        std::memcpy(img.scanLine(y), pic.px.constData() + y * pic.w * pic.channels, size_t(pic.w) * pic.channels);
    img.save(QDir(dir).filePath(QString(name).replace(QRegularExpression("[^A-Za-z0-9]+"), "_") + ".png"));
}

bool close(const Picture& a, int x, int y, const Picture& b, int bx, int by)
{
    for (int ch = 0; ch < std::min(a.channels, 3); ++ch)
        if (std::abs(a.value(x, y, ch) - b.value(bx, by, ch)) > 24) return false;
    return true;
}

// Pixel of the control away from edges (box, letterbox bars, frame border): the scaling paths differ only there
bool flat(const Picture& p, int x, int y)
{
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= p.w || ny >= p.h || !close(p, x, y, p, nx, ny)) return false;
        }
    return true;
}

// The same picture as the control in its flat areas (the bug put a whole other picture there)
void checkSame(const Picture& got, const Picture& want, const QString& what)
{
    if (got.w == 0 || want.w == 0) return; // render() reported it
    int off = 0, firstX = -1, firstY = -1;
    for (int y = 0; y < got.h; ++y)
        for (int x = 0; x < got.w; ++x)
            if (flat(want, x, y) && !close(got, x, y, want, x, y) && off++ == 0) firstX = x, firstY = y;
    if (!Check::report(off == 0, qPrintable(what), __FILE__, __LINE__,
                       QString("%1 of %2 pixels differ from the clips' picture, first at %3,%4")
                           .arg(off).arg(got.w * got.h).arg(firstX).arg(firstY))) {
        dump(got, what + " got");
        dump(want, what + " want");
    }
}

void testDissolve()
{
    struct Case {
        const char* name;
        std::function<void(Clip&, Clip&)> trigger; // brings zoom 1 through keyframes
        std::function<void(Clip&, Clip&)> setup;   // the rest, in the control too
        // Both pictures cover the same area. Otherwise (zoomed b over a's letterbox bars) a's bars are transparent
        // with qtblend and black without it, and luma blends over transparency differently (docs/dev/mlt.md)
        bool sameArea = true;
    };
    auto none = [](Clip&, Clip&) {};
    const QVector<Case> cases{
        {"outgoing zoom ends at 1", outgoingToOne, none},
        {"incoming zoom starts at 1", incomingFromOne, none},
        // The reported clip: zoom, blur, color effect and grade on the incoming side
        {"reported", outgoingToOne,
         [](Clip&, Clip& b) {
             b.transform.zoomX = b.transform.zoomY = 1.1;
             b.effects << EffectInstance{"blur", {{"strength", 20}}}
                       << EffectInstance{"color", {{"brightness", -35}, {"saturation", -10}}}
                       << EffectInstance{"grade", {{"liftY", 0.05}}};
         },
         false},
        // A crop always brings the qtblend (zoom 1 too)
        {"crop on the incoming side", outgoingToOne, [](Clip&, Clip& b) { b.transform.cropRight = 8; }},
        {"crop on the zooming side", outgoingToOne, [](Clip& a, Clip&) { a.transform.cropRight = 8; }},
    };
    struct Shape {
        const char* name;
        ProjectFormat fmt;
        QString red, green;
    };
    const QVector<Shape> shapes{
        {"16:9", kWide, g_redWide, g_greenWide},
        {"9:16", kTall, g_redTall, g_greenTall},
        {"16:9 in 9:16", kTall, g_redWide, g_greenWide}, // letterboxed: the zoom never makes it the frame
    };
    for (const Shape& s : shapes)
        for (mlt_image_format f : {mlt_image_rgba, mlt_image_yuv420p})
            for (double scale : {1.0, 0.5, 1.0 / 3}) {
                const QString size = QString("%1 %2 px").arg(f == mlt_image_rgba ? "RGBA" : "YUV")
                                         .arg(int(s.fmt.width * scale));
                const Picture plain = render(transition(none, s.red, s.green, TransitionType::CrossDissolve), s.fmt, f,
                                             25, scale);
                const bool letterbox = s.red == g_redWide && s.fmt.height > s.fmt.width;
                if (plain.w > 0 && f == mlt_image_rgba) { // the control itself: a blend, the box bottom right
                    const int x = plain.w / 4, y = plain.h / 2 - plain.h / 8;
                    const int yBox = letterbox ? plain.h / 2 + plain.h / 16 : plain.h * 3 / 4;
                    CHECK(plain.value(x, y, 0) > 80 && plain.value(x, y, 1) > 80 && plain.value(x, y, 2) < 40);
                    CHECK(plain.value(plain.w * 3 / 4, yBox, 2) > 200);
                }
                for (const Case& c : cases) {
                    if (!c.sameArea && letterbox) continue;
                    auto both = [&](Clip& a, Clip& b) {
                        c.setup(a, b);
                        c.trigger(a, b);
                    };
                    const Picture got = render(transition(both, s.red, s.green, TransitionType::CrossDissolve), s.fmt,
                                               f, 25, scale);
                    const Picture want = render(transition(c.setup, s.red, s.green, TransitionType::CrossDissolve),
                                                s.fmt, f, 25, scale);
                    checkSame(got, want, QString("%1, %2, %3").arg(s.name, c.name, size));
                }
            }
}

void testWipe()
{
    // A wipe runs through luma as well
    for (mlt_image_format f : {mlt_image_rgba, mlt_image_yuv420p})
        for (double scale : {1.0, 0.5}) {
            const Picture got = render(transition(outgoingToOne, g_redWide, g_greenWide, TransitionType::WipeDown), kWide,
                                       f, 25, scale);
            const Picture want = render(transition([](Clip&, Clip&) {}, g_redWide, g_greenWide, TransitionType::WipeDown),
                                        kWide, f, 25, scale);
            checkSame(got, want, QString("wipe, %1 px").arg(got.w));
        }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("transition_size");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // Solid color with a white box in the bottom right quarter (shows a picture in the wrong place or size)
    auto make = [&](const char* name, const char* color, const char* size) {
        return Check::makeMedia(tmp.filePath(name),
                                {"-f", "lavfi", "-i", QString("color=c=%1:s=%2:r=25:d=2").arg(color, size), "-vf",
                                 "drawbox=x=iw/2:y=ih/2:w=iw/2:h=ih/2:color=white:t=fill", "-c:v", "libx264", "-preset",
                                 "ultrafast", "-pix_fmt", "yuv420p"});
    };
    g_redWide = make("redwide.mp4", "red", "640x360");
    g_greenWide = make("greenwide.mp4", "0x00ff00", "640x360");
    g_redTall = make("redtall.mp4", "red", "360x640");
    g_greenTall = make("greentall.mp4", "0x00ff00", "360x640");
    if (!CHECK(!g_redWide.isEmpty() && !g_greenWide.isEmpty() && !g_redTall.isEmpty() && !g_greenTall.isEmpty()))
        return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testDissolve();
    testWipe();
    return Check::result();
}
