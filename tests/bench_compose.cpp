// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): time per frame of laying tracks over each other in the picture engine (CPU path,
// engine/picture + engine/video/Compose). Three 1080p stills (a JPG, a PNG with alpha, a JPG) on three tracks, each
// with a transform (zoom, position, rotation) over a full-frame track; stills so decoding stays out of the numbers
// (the engine keeps decoded stills). Export size RGBA and YUV 4:2:0 (RGBA + OutputColor), preview size (960x540) RGBA.
// Same output lines as the 0.14 version ("own"), so tools/bench.py compares across versions; the MLT column is gone.
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/bench_compose [frames]
#include "check.h"

#include "core/ProjectFormat.h"
#include "engine/OutputColor.h"
#include "engine/picture/Renderer.h"

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

// Median milliseconds per frame over frames 0..n-1 (after one warm-up frame); yuv = also into a 4:2:0 picture
double perFrame(const Timeline& tl, int w, int h, bool yuv, int n)
{
    Picture::Options o;
    o.format = kFmt;
    o.output = QSize(w, h);
    o.mode = Picture::Mode::Export;
    o.gpu = false; // the CPU path: comparable with 0.14 and between machines
    Picture::Renderer r(tl, o);
    std::vector<uint8_t> planes(size_t(w) * h * 3 / 2);
    OutputColor::Target t;
    t.planes[0] = planes.data();
    t.planes[1] = planes.data() + w * h;
    t.planes[2] = planes.data() + w * h + w * h / 4;
    t.strides[0] = w;
    t.strides[1] = t.strides[2] = w / 2;
    std::vector<double> ms;
    for (int i = -1; i < n; ++i) {
        QElapsedTimer timer;
        timer.start();
        const Picture::Frame f = r.render(std::max(0, i));
        if (f.isNull()) return -1;
        if (yuv) OutputColor::fromRgba(f.bits(), f.width(), f.height(), f.stride(), t);
        if (i >= 0) ms.push_back(timer.nsecsElapsed() / 1e6);
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
    Check::initEngine();
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
    struct Size {
        const char* name;
        bool yuv;
        int w, h;
    };
    const Size sizes[] = {{"1920x1080 RGBA", false, 1920, 1080},
                          {"1920x1080 YUV 4:2:0", true, 1920, 1080},
                          {"960x540 RGBA", false, 960, 540}};
    std::printf("4 tracks (3 with transform), median ms per frame over %d frames\n", frames);
    for (const Size& s : sizes)
        std::printf("  %-22s own %7.2f ms\n", s.name, perFrame(tl, s.w, s.h, s.yuv, frames));
    return 0;
}
