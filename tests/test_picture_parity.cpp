// SPDX-License-Identifier: AGPL-3.0-only
// Own picture engine (engine/picture, 0.15) against the MLT path: the same timelines rendered through TimelineBuilder +
// MLT tractor and through Picture::Renderer, compared pixel by pixel (RGBA, straight alpha). Each case states its
// tolerance: mean absolute difference per channel and the share of pixels off by more than 24 steps (edges of moved
// pictures, text antialiasing, MLT's dissolve in YUV 4:2:2). Also checks that a covered track is not decoded.
// With SCHNEIDI_PICTURE_BENCH=1: ms per 1080p frame for a 3-track project, MLT vs own.

#include "check.h"

#include "core/ProjectFormat.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"
#include "engine/picture/Renderer.h"

#include <Mlt.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>

namespace {

QString g_pattern, g_pattern2, g_tall, g_still;
const ProjectFormat kFormat{320, 180, {25, 1}};

QImage renderMlt(const Timeline& tl, const ProjectFormat& fmt, int frame, QSize size = {})
{
    if (size.isEmpty()) size = fmt.size();
    auto profile = makeProfile(fmt);
    TimelineBuilder b(*profile);
    auto tractor = b.build(tl);
    tractor->seek(frame);
    std::unique_ptr<Mlt::Frame> f(tractor->get_frame());
    f->set("consumer.rescale", "bilinear"); // as the preview consumer (Engine) and the export ask
    mlt_image_format format = mlt_image_rgba;
    int w = size.width(), h = size.height();
    const uint8_t* img = f->get_image(format, w, h);
    if (!img || format != mlt_image_rgba) return {};
    return QImage(img, w, h, w * 4, QImage::Format_RGBA8888).copy();
}

QImage renderOwn(const Timeline& tl, const ProjectFormat& fmt, int frame, QSize size = {})
{
    Picture::Options o;
    o.format = fmt;
    o.output = size;
    Picture::Renderer r(tl, o);
    return r.render(frame).toImage();
}

struct Diff {
    double mean = 0;  // per channel
    double off = 0;   // share of pixels with a channel off by more than 24
    int max = 0;
};

Diff compare(const QImage& a, const QImage& b)
{
    Diff d;
    if (a.size() != b.size() || a.isNull()) {
        d.mean = d.off = 1e9;
        return d;
    }
    double sum = 0;
    qint64 off = 0;
    const qint64 n = qint64(a.width()) * a.height();
    for (int y = 0; y < a.height(); ++y) {
        const uchar* p = a.constScanLine(y);
        const uchar* q = b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x, p += 4, q += 4) {
            int worst = 0;
            for (int c = 0; c < 4; ++c) {
                // RGB of fully transparent pixels does not matter
                const int diff = (c < 3 && p[3] == 0 && q[3] == 0) ? 0 : std::abs(p[c] - q[c]);
                sum += diff;
                worst = std::max(worst, diff);
            }
            d.max = std::max(d.max, worst);
            if (worst > 24) ++off;
        }
    }
    d.mean = sum / (n * 4.0);
    d.off = double(off) / n;
    return d;
}

void parity(const char* name, const Timeline& tl, int frame, double meanTol, double offTol, QSize size = {})
{
    const QImage mlt = renderMlt(tl, kFormat, frame, size);
    const QImage own = renderOwn(tl, kFormat, frame, size);
    const Diff d = compare(mlt, own);
    std::printf("  %-22s frame %3d: mean %.3f  off %.4f  max %d\n", name, frame, d.mean, d.off, d.max);
    if (qEnvironmentVariableIsSet("SCHNEIDI_PICTURE_DUMP")) {
        mlt.save(QString("/tmp/parity-%1-%2-mlt.png").arg(name).arg(frame));
        own.save(QString("/tmp/parity-%1-%2-own.png").arg(name).arg(frame));
    }
    CHECK(d.mean <= meanTol);
    CHECK(d.off <= offTol);
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

void testPlain()
{
    std::printf("plain\n");
    const Timeline tl = oneTrack({media(1, g_pattern, 0, 0, 49)});
    for (int f : {0, 7, 30}) parity("plain", tl, f, 0.01, 0.0);
    parity("plain-half", tl, 7, 3.0, 0.02, QSize(160, 90));
    parity("letterbox", oneTrack({media(1, g_tall, 0, 0, 49)}), 5, 1.5, 0.02);
    parity("gap", oneTrack({media(1, g_pattern, 10, 0, 9)}), 3, 0.0, 0.0);
}

void testTransform()
{
    std::printf("transform\n");
    Timeline tl = oneTrack({media(1, g_pattern2, 0, 0, 49)});
    tl.video.resize(2);
    Clip top = media(2, g_pattern, 0, 0, 49);
    top.transform.zoomX = top.transform.zoomY = 0.5;
    top.transform.posX = 40;
    top.transform.posY = 20;
    tl.video[1].clips = {top};
    parity("zoom-pos", tl, 10, 1.5, 0.03);
    tl.video[1].clips[0].transform.rotation = 30;
    parity("rotate", tl, 10, 2.0, 0.04);
    tl.video[1].clips[0].transform = {};
    tl.video[1].clips[0].transform.cropLeft = 40;
    tl.video[1].clips[0].transform.cropTop = 20;
    parity("crop", tl, 10, 1.5, 0.03);
    tl.video[1].clips[0].transform = {};
    tl.video[1].clips[0].transform.opacity = 50;
    parity("opacity", tl, 10, 1.5, 0.02);
}

void testFadesAndTransitions()
{
    std::printf("fades/transitions\n");
    Clip a = media(1, g_pattern, 0, 0, 49);
    a.fadeIn = 10;
    parity("fade-handle", oneTrack({a}), 4, 1.5, 0.02);
    Clip b = media(1, g_pattern, 0, 0, 24);
    Clip c = media(2, g_pattern2, 25, 10, 40);
    b.transOut = 10;
    c.transIn = 10;
    const Timeline tl = oneTrack({b, c});
    for (int f : {20, 24, 28}) parity("dissolve", tl, f, 2.0, 0.03);
    Clip d = media(1, g_pattern, 0, 0, 24);
    d.transIn = 10;
    parity("fade-in", oneTrack({d}), 3, 1.5, 0.02);
    b.transOutStyle.type = c.transInStyle.type = TransitionType::WipeRight;
    parity("wipe", oneTrack({b, c}), 24, 3.0, 0.05);
    b.transOutStyle.type = c.transInStyle.type = TransitionType::DipToColor;
    parity("dip", oneTrack({b, c}), 22, 2.0, 0.03);
    b.transOutStyle.type = c.transInStyle.type = TransitionType::Luma;
    b.transOutStyle.luma = c.transInStyle.luma = ":/lumas/uhr";
    parity("luma", oneTrack({b, c}), 24, 3.0, 0.05);
    Clip e = media(1, g_pattern, 0, 0, 24);
    e.transOut = 10;
    e.transOutStyle.type = TransitionType::WipeDown;
    parity("wipe-out", oneTrack({e}), 20, 1.0, 0.02);
    e.transOutStyle.type = TransitionType::DipToColor;
    e.transOutStyle.color = Qt::red;
    parity("dip-out", oneTrack({e}), 20, 1.0, 0.02);
}

void testTitleStillSpeed()
{
    std::printf("title/still/speed\n");
    Clip t;
    t.id = 5;
    t.kind = ClipKind::Title;
    t.title.text = "Hallo";
    t.title.size = 40;
    t.start = 0;
    t.in = 0;
    t.out = 49;
    Timeline tl = oneTrack({media(1, g_pattern, 0, 0, 49)});
    tl.video.resize(2);
    tl.video[1].clips = {t};
    parity("title", tl, 5, 1.0, 0.02);
    tl.video[1].clips[0].title.outlineOn = true;
    tl.video[1].clips[0].title.boxOn = true;
    parity("title-box", tl, 5, 1.0, 0.02);
    parity("still", oneTrack({media(1, g_still, 0, 0, 49)}), 3, 2.0, 0.03);
    Clip s = media(1, g_pattern, 0, 0, 20);
    s.speed = 2.0;
    parity("speed2", oneTrack({s}), 7, 0.01, 0.0);
    s.reverse = true;
    parity("reverse", oneTrack({s}), 7, 0.01, 0.0);
    s.reverse = false;
    s.speed = 1.0;
    s.freeze = true;
    s.freezeFrame = 12;
    parity("freeze", oneTrack({s}), 3, 0.01, 0.0);
}

void testSubtitles()
{
    std::printf("subtitles\n");
    Timeline tl = oneTrack({media(1, g_pattern, 0, 0, 49)});
    SubtitleTrack st;
    st.style.size = 16;
    st.style.posY = 10;
    st.style.boxPad = 4;
    st.cues = {{7, 0, 20, "Ein Untertitel"}};
    tl.subtitles = {st};
    parity("subtitle", tl, 5, 1.0, 0.02);
}

void testOcclusion()
{
    std::printf("occlusion\n");
    Timeline tl = oneTrack({media(1, g_pattern2, 0, 0, 49)});
    tl.video.resize(2);
    tl.video[1].clips = {media(2, g_pattern, 0, 0, 49)};
    Picture::Options o;
    o.format = kFormat;
    Picture::Renderer r(tl, o);
    r.render(3);
    const auto s = r.stats();
    CHECK_EQ(s.layersDecoded, 1);
    CHECK_EQ(s.layersSkipped, 1);
}

void bench(const QString& dir)
{
    if (!qEnvironmentVariableIsSet("SCHNEIDI_PICTURE_BENCH")) return;
    const ProjectFormat hd{1920, 1080, {25, 1}};
    const QString a = Check::makeMedia(dir + "/a.mp4", {"-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=25:d=4", "-c:v",
                                                        "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    const QString b = Check::makeMedia(dir + "/b.mp4", {"-f", "lavfi", "-i", "mandelbrot=s=1920x1080:r=25", "-t", "4",
                                                        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    Timeline tl;
    tl.video.resize(3);
    tl.video[0].clips = {media(1, a, 0, 0, 99)};
    Clip pip = media(2, b, 0, 0, 99);
    pip.transform.zoomX = pip.transform.zoomY = 0.4;
    pip.transform.posX = 500;
    tl.video[1].clips = {pip};
    Clip t;
    t.id = 3;
    t.kind = ClipKind::Title;
    t.title.text = "Benchmark";
    t.out = 99;
    tl.video[2].clips = {t};
    {
        auto profile = makeProfile(hd);
        TimelineBuilder builder(*profile);
        auto tractor = builder.build(tl);
        QElapsedTimer timer;
        timer.start();
        for (int f = 0; f < 50; ++f) {
            tractor->seek(f);
            std::unique_ptr<Mlt::Frame> fr(tractor->get_frame());
            mlt_image_format fmt = mlt_image_rgba;
            int w = 1920, h = 1080;
            fr->get_image(fmt, w, h);
        }
        std::printf("bench MLT: %.1f ms/frame\n", timer.elapsed() / 50.0);
    }
    {
        Picture::Options o;
        o.format = hd;
        Picture::Renderer r(tl, o);
        QElapsedTimer timer;
        timer.start();
        for (int f = 0; f < 50; ++f) r.render(f);
        std::printf("bench own: %.1f ms/frame\n", timer.elapsed() / 50.0);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("picture_parity");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_pattern = Check::makeMedia(tmp.filePath("pattern.mp4"), {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=2",
                                                               "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                                               "yuv420p"});
    g_pattern2 = Check::makeMedia(tmp.filePath("pattern2.mp4"), {"-f", "lavfi", "-i", "smptebars=s=320x180:r=25:d=2",
                                                                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                                                 "yuv420p"});
    g_tall = Check::makeMedia(tmp.filePath("tall.mp4"), {"-f", "lavfi", "-i", "testsrc2=s=180x320:r=25:d=2", "-c:v",
                                                         "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    QImage still(400, 300, QImage::Format_RGB32);
    for (int y = 0; y < still.height(); ++y)
        for (int x = 0; x < still.width(); ++x) still.setPixel(x, y, qRgb(x * 255 / 400, y * 255 / 300, 128));
    g_still = tmp.filePath("still.png");
    still.save(g_still);
    if (!CHECK(!g_pattern.isEmpty() && !g_pattern2.isEmpty() && !g_tall.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testPlain();
    testTransform();
    testFadesAndTransitions();
    testTitleStillSpeed();
    testSubtitles();
    testOcclusion();
    bench(tmp.path());
    return Check::result();
}
