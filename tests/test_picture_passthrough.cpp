// SPDX-License-Identifier: AGPL-3.0-only
// Export of a frame that is just one plain video at output size: the renderer hands the decoded YUV planes to the
// encoder's picture as they are (RenderHints::yuv, Frame::yuvWritten), bit-exact instead of YUV -> RGBA -> YUV. Checks
// that it happens only where nothing changes the picture (transform, opacity, fade, a layer above, other size or
// range), that the planes match the conversion closely, and a full-frame still at the bottom (drawn straight into
// the frame) is not lost (0.15: it came out black).

#include "check.h"

#include "engine/OutputColor.h"
#include "engine/picture/Renderer.h"

#include <QApplication>
#include <QImage>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <functional>
#include <vector>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
QString g_video, g_small, g_still;

struct Planes {
    std::vector<uint8_t> data = std::vector<uint8_t>(size_t(640) * 360 * 3 / 2, 0);
    OutputColor::Target target()
    {
        OutputColor::Target t;
        t.planes[0] = data.data();
        t.planes[1] = data.data() + 640 * 360;
        t.planes[2] = data.data() + 640 * 360 + 320 * 180;
        t.strides[0] = 640;
        t.strides[1] = t.strides[2] = 320;
        return t;
    }
};

Clip clip(int id, const QString& media)
{
    Clip c;
    c.id = id;
    c.mediaPath = media;
    c.out = 24;
    return c;
}

Timeline one(const std::function<void(Clip&)>& change, const QString& media = g_video)
{
    Timeline tl;
    tl.video.resize(1);
    Clip c = clip(1, media);
    change(c);
    tl.video[0].clips << c;
    return tl;
}

// Frame 5 into 4:2:0 planes; passed = taken over as decoded (else rendered to RGBA and converted here)
Planes render(const Timeline& tl, bool* passed)
{
    Picture::Options o;
    o.format = kFmt;
    o.mode = Picture::Mode::Export;
    o.gpu = false;
    Picture::Renderer r(tl, o);
    Planes p;
    const OutputColor::Target t = p.target();
    Picture::RenderHints h;
    h.yuv = &t;
    const Picture::Frame f = r.render(5, h);
    CHECK(!f.isNull());
    *passed = f.yuvWritten();
    if (!f.yuvWritten() && !f.isNull()) OutputColor::fromRgba(f.bits(), f.width(), f.height(), f.stride(), t);
    return p;
}

double meanDiff(const Planes& a, const Planes& b)
{
    double sum = 0;
    for (size_t i = 0; i < a.data.size(); ++i) sum += std::abs(int(a.data[i]) - int(b.data[i]));
    return sum / a.data.size();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("picture-passthrough");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // Converted with the BT.709 matrix it is tagged with: FFmpeg before 7.1 converts RGB sources with BT.601 whatever
    // the tag (no colorspace negotiation in its filters), the file then holds colors outside the RGB cube that the
    // conversion here clips and the planes passed through do not (FFmpeg 6.1, Ubuntu 24.04: mean difference 3.25)
    g_video = Check::makeMedia(tmp.filePath("v.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=2",
                                                       "-vf", "scale=out_color_matrix=bt709", "-c:v", "libx264",
                                                       "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-colorspace",
                                                       "bt709"});
    g_small = Check::makeMedia(tmp.filePath("s.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2",
                                                       "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_video.isEmpty() && !g_small.isEmpty())) return Check::result();
    {
        QImage still(640, 360, QImage::Format_RGB32);
        still.fill(QColor(200, 40, 40));
        g_still = tmp.filePath("still.png");
        CHECK(still.save(g_still));
    }
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");

    // Plain video: passed through, and close to what the conversion makes of it
    bool passed = false;
    const Planes plain = render(one([](Clip&) {}), &passed);
    CHECK(passed);
    bool converted = true;
    const Planes zoomed = render(one([](Clip& c) { c.transform.zoomX = c.transform.zoomY = 1.0001; }), &converted);
    CHECK(!converted);
    const double diff = meanDiff(plain, zoomed);
    if (!CHECK(diff < 1.5)) std::printf("       passed through vs converted: mean difference %.2f\n", diff);

    struct Case {
        const char* name;
        Timeline tl;
    };
    Timeline above = one([](Clip&) {});
    above.video.resize(2);
    Clip top = clip(2, g_small);
    top.transform.zoomX = top.transform.zoomY = 0.5;
    above.video[1].clips << top;
    const QVector<Case> converts = {
        {"moved", one([](Clip& c) { c.transform.posX = 10; })},
        {"transparent", one([](Clip& c) { c.transform.opacity = 70; })},
        {"fading in", one([](Clip& c) { c.fadeIn = 10; })},
        {"layer above", above},
        {"other size", one([](Clip&) {}, g_small)},
        {"still", one([](Clip&) {}, g_still)},
    };
    for (const Case& c : converts) {
        bool p = true;
        render(c.tl, &p);
        if (!CHECK(!p)) std::printf("       %s: passed through\n", c.name);
    }

    // A copy of the clip kept on the track above (same file, part and time) reads the lower clip's decoder when its
    // frame is passed through too: one decoder, not two
    {
        Timeline kept = one([](Clip&) {});
        kept.video.resize(2);
        kept.video[1].clips << clip(2, g_video);
        Picture::Options o;
        o.format = kFmt;
        o.mode = Picture::Mode::Export;
        o.gpu = false;
        Picture::Renderer r(kept, o);
        Planes p;
        const OutputColor::Target t = p.target();
        Picture::RenderHints h;
        h.yuv = &t;
        CHECK(r.render(5, h).yuvWritten());
        if (!CHECK_EQ(r.stats().sourcesOpen, 1)) std::printf("       kept copy passed through: own decoder\n");
    }

    // A full-frame still at the bottom is drawn straight into the frame: red, not black
    {
        Picture::Options o;
        o.format = kFmt;
        o.mode = Picture::Mode::Export;
        o.gpu = false;
        Picture::Renderer r(one([](Clip&) {}, g_still), o);
        const QImage img = r.render(5).toImage();
        const QColor px = img.pixelColor(320, 180);
        if (!CHECK(std::abs(px.red() - 200) <= 2 && std::abs(px.green() - 40) <= 2))
            std::printf("       full-frame still: %d,%d,%d\n", px.red(), px.green(), px.blue());
    }
    return Check::result();
}
