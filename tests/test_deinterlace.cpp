// SPDX-License-Identifier: AGPL-3.0-only
// Interlaced sources are deinterlaced by the engine's reader (Video::Reader::Options::deinterlace, bwdif as the
// proxies use it), so the preview of the original, the export and the proxy show the same picture. A synthetic 1080i-like
// clip (a bright bar moving 12 px per field, the two fields woven into each frame): no combing in the reader's picture
// nor in the rendered frame, the same picture whether played to or seeked to (also onto a key frame, the file's first
// and last frame), the export's frame = the preview's, progressive files untouched.
#include "check.h"

#include "engine/picture/Renderer.h"
#include "engine/video/Deinterlace.h"
#include "engine/video/Reader.h"

#include <QApplication>
#include <QByteArray>
#include <QImage>
#include <QTemporaryDir>

extern "C" {
#include <libavutil/frame.h>
}

#include <cmath>
#include <cstdlib>

namespace {

constexpr int kW = 720, kH = 576, kFrames = 50, kKey = 12;

// Combing: mean |2 Y(y) - Y(y-1) - Y(y+1)| over the luma plane (woven fields of a moving edge alternate per line)
double comb(const AVFrame* f)
{
    double sum = 0;
    int n = 0;
    for (int y = 1; y + 1 < f->height; ++y) {
        const uint8_t* a = f->data[0] + size_t(y - 1) * f->linesize[0];
        const uint8_t* b = a + f->linesize[0];
        const uint8_t* c = b + f->linesize[0];
        for (int x = 0; x < f->width; ++x, ++n) sum += std::abs(2 * b[x] - a[x] - c[x]);
    }
    return n ? sum / n : 0;
}

// The same on the green channel of a rendered frame
double comb(const QImage& img)
{
    const QImage g = img.convertToFormat(QImage::Format_RGB32);
    double sum = 0;
    int n = 0;
    for (int y = 1; y + 1 < g.height(); ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(g.constScanLine(y - 1));
        const auto* b = reinterpret_cast<const QRgb*>(g.constScanLine(y));
        const auto* c = reinterpret_cast<const QRgb*>(g.constScanLine(y + 1));
        for (int x = 0; x < g.width(); ++x, ++n) sum += std::abs(2 * qGreen(b[x]) - qGreen(a[x]) - qGreen(c[x]));
    }
    return n ? sum / n : 0;
}

QByteArray luma(const AVFrame* f)
{
    QByteArray out;
    for (int y = 0; y < f->height; ++y)
        out.append(reinterpret_cast<const char*>(f->data[0] + size_t(y) * f->linesize[0]), f->width);
    return out;
}

QByteArray luma(Video::Reader& r, int64_t position)
{
    const auto p = r.picture(position);
    return p && p->frame && p->position == position ? luma(p->frame) : QByteArray();
}

std::unique_ptr<Video::Reader> open(const QString& path, bool deinterlace = true)
{
    Video::Reader::Options o;
    o.hardware = false;
    o.deinterlace = deinterlace;
    return Video::Reader::open(path, o);
}

double difference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull()) return 255;
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32);
    double sum = 0;
    for (int j = 0; j < x.height(); ++j) {
        const auto* p = reinterpret_cast<const QRgb*>(x.constScanLine(j));
        const auto* q = reinterpret_cast<const QRgb*>(y.constScanLine(j));
        for (int i = 0; i < x.width(); ++i)
            sum += std::abs(qRed(p[i]) - qRed(q[i])) + std::abs(qGreen(p[i]) - qGreen(q[i]))
                   + std::abs(qBlue(p[i]) - qBlue(q[i]));
    }
    return sum / (3.0 * x.width() * x.height());
}

void testReader(const QString& clip)
{
    // The file as decoded: woven fields, combing at the bar's edges
    const auto raw = open(clip, false);
    if (!CHECK(raw)) return;
    const auto rawPic = raw->picture(20);
    if (!CHECK(rawPic && rawPic->frame)) return;
    CHECK(!rawPic->progressive);
    CHECK(!rawPic->deinterlaced);
    const double woven = comb(rawPic->frame);

    const auto r = open(clip);
    if (!CHECK(r)) return;
    const auto pic = r->picture(20);
    if (!CHECK(pic && pic->frame)) return;
    CHECK(pic->progressive);
    CHECK(pic->deinterlaced);
    CHECK(pic->topFieldFirst);
    CHECK_EQ(pic->frame->width, kW);
    CHECK_EQ(pic->frame->height, kH);
    const double clean = comb(pic->frame);
    std::printf("combing: woven %.2f, deinterlaced %.2f\n", woven, clean);
    CHECK(woven > 4 * clean);
    CHECK(r->stats().deinterlaced >= 1);
    // The bar is still there, as wide as drawn (one field's moment, not a blend of two)
    for (const int y : {100, 101}) {
        int bright = 0;
        for (int x = 0; x < kW; ++x) bright += pic->frame->data[0][size_t(y) * pic->frame->linesize[0] + x] > 180;
        if (!CHECK(bright >= 56 && bright <= 72)) std::printf("  row %d: %d bright pixels\n", y, bright);
    }
    // Hardware decoding (when this machine has it) takes the same route: deinterlaced the same way
    {
        Video::Reader::Options o;
        o.hardware = true;
        const auto hw = Video::Reader::open(clip, o);
        const auto hp = hw ? hw->picture(20) : nullptr;
        if (CHECK(hp && hp->frame)) {
            CHECK(hp->deinterlaced);
            CHECK(comb(hp->frame) < woven / 4);
            std::printf("hardware decoding %s, same picture as software: %s\n", hw->stats().hardware ? "on" : "off",
                        luma(hp->frame) == luma(pic->frame) ? "yes" : "no");
        }
    }

    // Played to every frame in order ...
    const auto played = open(clip);
    QVector<QByteArray> want;
    for (int i = 0; i < kFrames; ++i) want << luma(*played, i);
    for (int i = 0; i < kFrames; ++i)
        if (!CHECK(!want[i].isEmpty())) return;
    // ... = seeked to: a fresh reader, onto a key frame (its picture before lies in the GOP before), jumps back and
    // forth, the first and the last frame
    for (const int64_t at : {int64_t(kKey), int64_t(2 * kKey), int64_t(13), int64_t(kFrames - 1), int64_t(0), int64_t(31)}) {
        const auto fresh = open(clip);
        if (!CHECK(luma(*fresh, at) == want[at])) std::printf("  frame %lld seeked to != played\n", (long long)at);
    }
    const auto jumper = open(clip);
    for (const int64_t at : {40, 3, 36, 24, 25, 11, 48, 12}) {
        if (!CHECK(luma(*jumper, at) == want[at])) std::printf("  frame %lld after jumps != played\n", (long long)at);
    }
}

void testRenderer(const QString& clip)
{
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = clip;
    c.start = 0;
    c.in = 0;
    c.out = kFrames - 1;
    tl.video[0].clips = {c};
    ProjectFormat f;
    f.width = kW;
    f.height = kH;
    f.rate = FrameRate{25, 1};

    Picture::Options o;
    o.format = f;
    o.mode = Picture::Mode::Export;
    Picture::Renderer exporter(tl, o);
    QVector<QImage> exported;
    for (int i = 0; i < 30; ++i) exported << exporter.render(i).toImage();

    o.mode = Picture::Mode::Preview;
    Picture::Renderer preview(tl, o);
    for (const int at : {24, 7, 25, 26, 12}) {
        const QImage shown = preview.render(at).toImage();
        const double d = difference(shown, exported[at]);
        if (!CHECK(d < 0.5)) std::printf("  frame %d: preview differs from export by %.3f\n", at, d);
    }
    const double clean = comb(exported[20]);
    std::printf("rendered combing at frame 20: %.2f\n", clean);
    CHECK(clean < 3);
}

void testProgressive(const QString& clip)
{
    const auto r = open(clip);
    if (!CHECK(r)) return;
    for (int i = 0; i < 10; ++i) {
        const auto p = r->picture(i);
        if (!CHECK(p && p->frame)) return;
        CHECK(p->progressive && !p->deinterlaced);
    }
    CHECK_EQ(r->stats().deinterlaced, 0);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("deinterlace");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    if (!Video::canDeinterlace()) return Check::skip("kein Deinterlacer (libavfilter ohne bwdif/yadif)");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // 50 fields per second: a bar moving 12 px per field, woven top field first into 25 frames per second
    const QString interlaced = Check::makeMedia(
        tmp.filePath("interlaced.mp4"),
        {"-f", "lavfi", "-i",
         QString("color=c=black:size=%1x%2:rate=50:duration=%3,format=yuv420p,"
                 "geq=lum='if(lt(mod(X+N*12\\,%1)\\,64)\\,235\\,16)':cb=128:cr=128,"
                 "tinterlace=mode=interleave_top,setfield=tff")
             .arg(kW)
             .arg(kH)
             .arg(kFrames * 2 / 50.0 + 0.04),
         "-frames:v", QString::number(kFrames), "-flags", "+ilme+ildct", "-c:v", "libx264", "-preset", "ultrafast",
         "-crf", "8", "-x264-params", QString("keyint=%1:min-keyint=%1:scenecut=0").arg(kKey), "-pix_fmt", "yuv420p"});
    const QString progressive = Check::makeMedia(
        tmp.filePath("progressive.mp4"), {"-f", "lavfi", "-i", "testsrc2=s=320x240:r=25:d=1", "-c:v", "libx264",
                                          "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!interlaced.isEmpty() && !progressive.isEmpty())) return Check::result();
    testReader(interlaced);
    testRenderer(interlaced);
    testProgressive(progressive);
    return Check::result();
}
