// SPDX-License-Identifier: AGPL-3.0-only
// Frame timing of media between the paths that read them: a proxy shows the same picture at frame k as the original
// (a recording whose sound starts a frame before the picture, as OBS/phones/games write them), a source shorter than
// the model expects does not move the clips after it, and the frame position of a timestamp
// counts from the container start in the stream's time base (reader and smart render alike).
#include "render_check.h"

#include "core/Types.h"
#include "engine/ClipFormat.h"
#include "engine/ProxyManager.h"
#include "engine/video/Reader.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace {

// Mean luma difference of two pictures of the same size (8-bit planar), 1e9 if they cannot be compared
double lumaDiff(const Video::Picture* a, const Video::Picture* b)
{
    if (!a || !b || a->frame->width != b->frame->width || a->frame->height != b->frame->height) return 1e9;
    double sum = 0;
    const int w = a->frame->width, h = a->frame->height;
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = a->frame->data[0] + y * a->frame->linesize[0];
        const uint8_t* q = b->frame->data[0] + y * b->frame->linesize[0];
        for (int x = 0; x < w; ++x) sum += std::abs(int(p[x]) - int(q[x]));
    }
    return sum / (double(w) * h);
}

// Proxy frame `n` matches original frame `n` better than its neighbours
void checkProxyFrames(const QString& original, const QString& proxy, const char* what)
{
    Video::Reader::Options o;
    o.hardware = false;
    const auto orig = Video::Reader::open(original, o);
    const auto prox = Video::Reader::open(proxy, o);
    if (!CHECK(orig && prox)) return;
    for (const int n : {1, 5, 12, 30}) {
        const auto p = prox->picture(n);
        const double same = lumaDiff(p.get(), orig->picture(n).get());
        const double before = lumaDiff(p.get(), orig->picture(n - 1).get());
        const double after = lumaDiff(p.get(), orig->picture(n + 1).get());
        Check::report(same < before && same < after, "proxy frame n = original frame n", __FILE__, __LINE__,
                      QString("%1, frame %2: same %3, n-1 %4, n+1 %5").arg(what).arg(n).arg(same).arg(before).arg(after));
    }
}

QString makeProxy(const QString& original)
{
    ProxyManager pm;
    QString message;
    QObject::connect(&pm, &ProxyManager::failed, [&](const QString&, const QString& m) { message = m; });
    pm.generate({original});
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!pm.isPending(original)) loop.quit();
    });
    poll.start(50);
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!message.isEmpty()) std::printf("proxy failed: %s\n", qPrintable(message));
    return pm.hasProxy(original) ? ProxyManager::proxyPath(original) : QString();
}

// Mean of the red and blue channel of the rendered picture at `pos`
std::pair<double, double> redBlue(const Timeline& tl, const ProjectFormat& fmt, int pos)
{
    const QImage img = Check::renderFrame(tl, fmt, pos).convertToFormat(QImage::Format_RGBA8888);
    if (img.isNull()) return {-1, -1};
    double r = 0, b = 0;
    for (int y = 0; y < img.height(); ++y) {
        const uchar* d = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            r += d[x * 4];
            b += d[x * 4 + 2];
        }
    }
    const double n = double(img.width()) * img.height();
    return {r / n, b / n};
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("media_timing");

    // --- Frame position of a timestamp: from the first key frame, else from the container start, which is in µs and
    // has to be converted to the stream's time base (MPEG-TS: 1/90000, starts at 1.4 s)
    {
        const AVRational ts{1, 90000};
        const int64_t start = 1400000; // µs
        CHECK_EQ(Video::framePosition(126000, AV_NOPTS_VALUE, start, ts, 25), int64_t(0));
        CHECK_EQ(Video::framePosition(126000 + 3600 * 10, AV_NOPTS_VALUE, start, ts, 25), int64_t(10));
        CHECK_EQ(Video::framePosition(126000 + 3600 * 10, 126000 - 3600, start, ts, 25), int64_t(11)); // key frame wins
        CHECK_EQ(Video::framePosition(3600 * 4, AV_NOPTS_VALUE, AV_NOPTS_VALUE, ts, 25), int64_t(4));
        // Seeking aims at the timestamp of the position, the position of that timestamp is the position again
        for (const int64_t pos : {int64_t(0), int64_t(1), int64_t(17), int64_t(250)}) {
            const int64_t t = Video::frameTimestamp(pos, AV_NOPTS_VALUE, start, ts, 25);
            CHECK_EQ(Video::framePosition(t, AV_NOPTS_VALUE, start, ts, 25), pos);
        }
        CHECK_EQ(Video::frameTimestamp(10, AV_NOPTS_VALUE, start, ts, 25), int64_t(126000 + 36000));
        CHECK_EQ(Video::frameTimestamp(10, 1000, start, ts, 25), int64_t(1000 + 36000));
    }

    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();
    QTemporaryDir dir;

    // --- Proxy of a recording whose picture starts one frame after its sound: proxy frame k = original frame k
    {
        const QStringList sound{"-f", "lavfi", "-i", "sine=frequency=440:duration=3"};
        const QStringList picture{"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=3"};
        const QStringList x264{"-map", "1:v", "-map", "0:a", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                               "yuv420p", "-g", "10",
                               // the picture's own time stamps: FFmpeg 6.1 fills a late start in MP4 with copies of
                               // the first picture (constant frame rate), the picture is then not late at all
                               "-fps_mode", "passthrough"};
        const QString late = Check::makeMedia(dir.filePath("late_picture.mkv"),
                                              sound + QStringList{"-itsoffset", "0.04"} + picture + x264
                                                  + QStringList{"-c:a", "flac"});
        const QString lateMp4 = Check::makeMedia(dir.filePath("late_picture.mp4"),
                                                 sound + QStringList{"-itsoffset", "0.08"} + picture + x264
                                                     + QStringList{"-c:a", "aac"});
        const QString plain = Check::makeMedia(dir.filePath("plain.mp4"),
                                               sound + picture + x264 + QStringList{"-c:a", "aac"});
        if (CHECK(!late.isEmpty() && !lateMp4.isEmpty() && !plain.isEmpty())) {
            const std::optional<double> zero = Video::zeroOffset(late);
            CHECK(zero && std::abs(*zero - 0.04) < 0.002);
            for (const QString& f : {late, lateMp4, plain}) {
                const QString proxy = makeProxy(f);
                if (CHECK(!proxy.isEmpty())) checkProxyFrames(f, proxy, qPrintable(QFileInfo(f).fileName()));
            }
        }
    }

    // --- Proxy of a video with alpha keeps the alpha (an H.264 proxy played opaque, covering the tracks below)
    {
        const QString alpha = Check::makeMedia(
            dir.filePath("alpha.webm"), {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2,format=yuva420p,"
                                                              "geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='if(lt(X,160),0,255)'",
                                         "-c:v", "libvpx-vp9", "-auto-alt-ref", "0", "-deadline", "realtime"});
        if (CHECK(!alpha.isEmpty())) {
            const QString proxy = makeProxy(alpha);
            Video::Reader::Options o;
            o.hardware = false;
            const auto r = proxy.isEmpty() ? nullptr : Video::Reader::open(proxy, o);
            if (CHECK(r != nullptr)) {
                CHECK_EQ(r->info().format, AV_PIX_FMT_YUVA420P);
                const auto p = r->picture(10);
                if (CHECK(p && p->frame->data[3])) {
                    const uint8_t* row = p->frame->data[3] + size_t(90) * p->frame->linesize[3];
                    CHECK(row[40] < 20 && row[280] > 235); // left half transparent, right half opaque
                }
            }
        }
    }

    // --- An alpha format with only opaque pixels (QuickTime Animation / ProRes 4444 from many editors, screen
    // recordings in bgra) keeps the fast H.264 proxy: VP9 with alpha encodes and decodes several times slower
    {
        const QString opaque = Check::makeMedia(dir.filePath("opaque-alpha.mov"),
                                                {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=1", "-pix_fmt",
                                                 "argb", "-c:v", "qtrle"});
        if (CHECK(!opaque.isEmpty())) {
            const QString proxy = makeProxy(opaque);
            Video::Reader::Options o;
            o.hardware = false;
            const auto r = proxy.isEmpty() ? nullptr : Video::Reader::open(proxy, o);
            if (CHECK(r != nullptr)) CHECK(r->info().codec == "h264" && r->info().format != AV_PIX_FMT_YUVA420P);
        }
    }

    // --- A source shorter than the clip in the model (proxy a few frames short, relinked shorter file): the clip ends
    // at the file's end, the clip after it stays where the model has it (a gap fills the rest)
    {
        const QString red = Check::makeMedia(dir.filePath("red.mp4"), {"-f", "lavfi", "-i", "color=c=red:size=320x180:rate=25:duration=1",
                                                                       "-c:v", "libx264", "-pix_fmt", "yuv420p"});
        const QString blue = Check::makeMedia(dir.filePath("blue.mp4"), {"-f", "lavfi", "-i", "color=c=blue:size=320x180:rate=25:duration=2",
                                                                         "-c:v", "libx264", "-pix_fmt", "yuv420p"});
        if (CHECK(!red.isEmpty() && !blue.isEmpty())) {
            ProjectFormat fmt;
            fmt.width = 320;
            fmt.height = 180;
            fmt.rate = {25, 1};
            Timeline tl;
            tl.video.resize(1);
            Clip a;
            a.id = 1;
            a.mediaPath = red; // 25 frames in the file
            a.out = 39;        // 40 in the model
            Clip b;
            b.id = 2;
            b.mediaPath = blue;
            b.start = 40;
            b.out = 29;
            tl.video[0].clips << a << b;
            {
                const auto [r10, b10] = redBlue(tl, fmt, 10);
                CHECK(r10 > 150 && b10 < 60);
                const auto [r30, b30] = redBlue(tl, fmt, 30); // past the file's end: the gap, not the next clip
                Check::report(b30 < 60, "next clip not moved forward", __FILE__, __LINE__, QString("blue %1 at 30").arg(b30));
                const auto [r45, b45] = redBlue(tl, fmt, 45);
                CHECK(b45 > 150 && r45 < 60);
                const auto [r69, b69] = redBlue(tl, fmt, 69); // last frame of b
                CHECK(b69 > 150);
            }
        }
    }

    // --- The same compound clip A on V1 and V2 (picture in picture), A holding another compound clip B: renders
    {
        const QString blue = dir.filePath("blue.mp4");
        if (CHECK(QFile::exists(blue))) {
            const auto compound = [](int id, int seq, int start) {
                Clip c;
                c.id = id;
                c.kind = ClipKind::Compound;
                c.sequenceId = seq;
                c.start = start;
                c.out = 29;
                return c;
            };
            auto nested = std::make_shared<NestedTimelines>();
            Timeline b;
            b.video.resize(1);
            Clip file;
            file.id = 1;
            file.mediaPath = blue;
            file.out = 29;
            b.video[0].clips << file;
            Timeline a;
            a.video.resize(1);
            a.video[0].clips << compound(1, 200, 0);
            (*nested)[200] = b;
            (*nested)[100] = a;
            Timeline tl;
            tl.video.resize(2);
            tl.video[0].clips << compound(1, 100, 0);
            tl.video[1].clips << compound(2, 100, 5);
            tl.nested = nested;
            ProjectFormat fmt;
            fmt.width = 320;
            fmt.height = 180;
            fmt.rate = {25, 1};
            const auto [r20, b20] = redBlue(tl, fmt, 20);
            CHECK(b20 > 150);
        }
    }
    return Check::result();
}
