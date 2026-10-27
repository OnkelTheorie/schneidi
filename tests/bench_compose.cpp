// Benchmark (not a CTest test): time per frame of laying tracks over each other, own compositing (engine/video/Compose)
// against MLT's qtblend (Qt module, if it loads). Three 1080p stills (a JPG, a PNG with alpha, a JPG) on three tracks,
// each with a transform (zoom, position, rotation) over a full-frame video track; stills so decoding stays out of
// the numbers (MLT caches the decoded picture). Export size RGBA and YUV 4:2:0, preview size (960x540) RGBA.
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/bench_compose [frames]
#include "check.h"

#include "core/ProjectFormat.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"
#include "engine/video/Compose.h"

#include <Mlt.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <algorithm>
#include <clocale>
#include <cstdio>
#include <vector>

namespace {

const ProjectFormat kFmt{1920, 1080, {25, 1}};

// Median milliseconds per frame over frames 0..n-1 (after one warm-up frame)
double perFrame(const Timeline& tl, mlt_image_format format, int w, int h, int n)
{
    auto profile = makeProfile(kFmt);
    TimelineBuilder b(*profile);
    auto tractor = b.build(tl);
    std::vector<double> ms;
    for (int i = -1; i < n; ++i) {
        tractor->seek(std::max(0, i));
        std::unique_ptr<Mlt::Frame> f(tractor->get_frame());
        mlt_image_format got = format;
        int ww = w, hh = h;
        QElapsedTimer t;
        t.start();
        if (!f->get_image(got, ww, hh)) return -1;
        if (i >= 0) ms.push_back(t.nsecsElapsed() / 1e6);
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench_compose");
    const int frames = argc > 1 ? std::max(3, atoi(argv[1])) : 30;
    QTemporaryDir tmp;
    QImage a(1920, 1080, QImage::Format_RGB32), c(1920, 1080, QImage::Format_RGB32);
    for (int y = 0; y < 1080; ++y)
        for (int x = 0; x < 1920; ++x) {
            a.setPixel(x, y, qRgb(x & 255, y & 255, (x ^ y) & 255));
            c.setPixel(x, y, qRgb((x * 3) & 255, 128, (y * 5) & 255));
        }
    QImage b(1920, 1080, QImage::Format_ARGB32);
    b.fill(Qt::transparent);
    {
        QPainter p(&b);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(255, 120, 0, 200));
        p.drawEllipse(QRectF(200, 100, 1500, 880));
    }
    const QString pa = tmp.filePath("a.jpg"), pb = tmp.filePath("b.png"), pc = tmp.filePath("c.jpg");
    a.save(pa, "JPG", 90);
    b.save(pb);
    c.save(pc, "JPG", 90);
    qputenv("SCHNEIDI_MLT_QT", "1"); // MLT's Qt module too: the old services to compare with
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");

    Timeline tl;
    tl.video.resize(4);
    const QString media[4] = {pa, pc, pb, pa};
    for (int i = 0; i < 4; ++i) {
        Clip k;
        k.id = i + 1;
        k.mediaPath = media[i];
        k.out = frames + 5;
        if (i > 0) { // the three upper tracks with a transform
            k.transform.zoomX = k.transform.zoomY = 0.9 - 0.15 * i;
            k.transform.posX = 120.0 * i - 200;
            k.transform.posY = 40.0 * i;
            k.transform.rotation = 7.5 * i;
        }
        tl.video[i].clips << k;
    }
    const bool haveQt = mlt_repository_metadata(mlt_factory_repository(), mlt_service_transition_type, "qtblend");
    struct Size {
        const char* name;
        mlt_image_format format;
        int w, h;
    };
    const Size sizes[] = {{"1920x1080 RGBA", mlt_image_rgba, 1920, 1080},
                          {"1920x1080 YUV 4:2:0", mlt_image_yuv420p, 1920, 1080},
                          {"960x540 RGBA", mlt_image_rgba, 960, 540}};
    std::printf("4 tracks (3 with transform), median ms per frame over %d frames\n", frames);
    for (const Size& s : sizes) {
        double qt = -1;
        if (haveQt) {
            Compose::setQtReference(true);
            qt = perFrame(tl, s.format, s.w, s.h, frames);
            Compose::setQtReference(false);
        }
        const double own = perFrame(tl, s.format, s.w, s.h, frames);
        std::printf("  %-22s qtblend %7.2f ms   own %7.2f ms\n", s.name, qt, own);
    }
    return 0;
}
