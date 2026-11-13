// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): what a shape title costs per frame, by its size. A video on V1 (a file, or a
// synthetic still), a shape on V2 (blur fill by default: it reads the picture below it), the frames rendered one after
// another like playback in the preview (Mode::Preview, CPU path unless SCHNEIDI_GPU=1). Prints the median and the
// worst ms per frame for each size, and the cost of ShapePaint::below alone on a frame of the output size.
// Run: ./build/tests/bench_shape [--media <file>] [--out WxH] [--fill blur|color|pixelate] [--kind ellipse|rect|star]
//      [--amount 50] [--frames 30] [--shadow] [--outline] [--sizes 100,400,800,...]
#include "check.h"

#include "core/Project.h"
#include "core/Shapes.h"
#include "core/TimelineOps.h"
#include "engine/picture/Renderer.h"
#include "engine/picture/ShapePaint.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-shape");
    QString media;
    QSize out(960, 540);
    ShapeFill fill = ShapeFill::Blur;
    ShapeKind kind = ShapeKind::Ellipse;
    double amount = 50;
    int frames = 30;
    bool shadow = false, outline = false;
    QVector<int> sizes{0, 100, 200, 300, 400, 600, 800, 1000, 1200, 1600, 2000};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&] { return i + 1 < argc ? QString::fromLocal8Bit(argv[++i]) : QString(); };
        if (a == "--media") media = next();
        else if (a == "--out") {
            const QStringList p = next().split('x');
            if (p.size() == 2) out = QSize(p[0].toInt(), p[1].toInt());
        } else if (a == "--fill") Shapes::fillFromId(next(), &fill);
        else if (a == "--kind") Shapes::kindFromId(next(), &kind);
        else if (a == "--amount") amount = next().toDouble();
        else if (a == "--frames") frames = next().toInt();
        else if (a == "--shadow") shadow = true;
        else if (a == "--outline") outline = true;
        else if (a == "--sizes") {
            sizes.clear();
            for (const QString& s : next().split(',')) sizes << s.toInt();
        }
    }
    const ProjectFormat format{1920, 1080, {60, 1}};
    QTemporaryDir dir;
    if (media.isEmpty()) { // a noisy still: the blur has something to do
        QImage img(1920, 1080, QImage::Format_RGB32);
        std::mt19937 rng(1);
        for (int y = 0; y < img.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) row[x] = qRgb(rng() & 255, (x * 7) & 255, (y * 5) & 255);
        }
        media = dir.filePath("noise.png");
        img.save(media);
    }
    const int length = 600;
    std::printf("output %dx%d, fill %s, kind %s, amount %.0f, shadow %d, outline %d, %d frames each\n", out.width(),
                out.height(), Shapes::fillId(fill), Shapes::kindId(kind), amount, int(shadow), int(outline), frames);
    std::printf("%6s %10s %10s %12s\n", "size", "median ms", "worst ms", "below() ms");
    for (const int size : sizes) {
        Timeline tl = emptyTimeline();
        TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
        Clip v;
        v.id = 1;
        v.mediaPath = media;
        v.out = length - 1;
        tl.video[0].clips = {v};
        Clip s;
        s.id = 2;
        s.kind = ClipKind::Title;
        s.title.kind = TitleKind::Shape;
        s.title.text.clear();
        s.title.color = Qt::red;
        s.title.shape.kind = kind;
        s.title.shape.width = size;
        s.title.shape.height = size;
        s.title.shape.fill = fill;
        s.title.shape.amount = amount;
        s.title.outlineOn = outline;
        s.title.outlineColor = QColor("#ffd600");
        s.title.outlineWidth = 0;
        s.title.look.shadowOn = shadow;
        s.title.look.shadowColor = Qt::black;
        s.title.look.shadowOpacity = 0.031;
        s.title.look.shadowSoft = 8.1;
        s.title.look.shadowX = 6;
        s.title.look.shadowY = 6;
        s.out = length - 1;
        if (size > 0) tl.video[1].clips = {s};
        Picture::Options o;
        o.format = format;
        o.output = out;
        o.mode = Picture::Mode::Preview;
        Picture::Renderer r(tl, o);
        r.render(0); // open the decoder
        std::vector<double> ms;
        for (int f = 1; f <= frames; ++f) {
            QElapsedTimer t;
            t.start();
            const Picture::Frame fr = r.render(f);
            ms.push_back(t.nsecsElapsed() / 1e6);
        }
        // below() alone on a frame of the output size
        double belowMs = 0;
        if (size > 0) {
            std::vector<uint8_t> rgba(size_t(out.width()) * out.height() * 4);
            std::mt19937 rng(2);
            for (auto& b : rgba) b = uint8_t(rng());
            for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
            const double sc = double(out.height()) / format.height;
            Shapes::Place p;
            p.center = QPointF(format.width / 2.0, format.height / 2.0);
            p.w = p.h = size;
            Shapes::Outline ol = Shapes::placed(s.title.shape, p);
            for (QPointF& q : ol.points) q *= sc;
            std::vector<double> b;
            for (int k = 0; k < 5; ++k) {
                QElapsedTimer t;
                t.start();
                Picture::ShapePaint::below(rgba.data(), out.width(), out.height(), ol, s.title.shape, sc, 1.0);
                b.push_back(t.nsecsElapsed() / 1e6);
            }
            belowMs = median(b);
        }
        std::printf("%6d %10.2f %10.2f %12.2f\n", size, median(ms), *std::max_element(ms.begin(), ms.end()), belowMs);
        std::fflush(stdout);
    }
    return 0;
}
