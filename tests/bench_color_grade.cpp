// SPDX-License-Identifier: AGPL-3.0-only
// Speed of the Color page correction (engine/ColorGrade) on a 1080p frame: the per-pixel reference against the
// baked tables (1 thread and several), the cost of baking (per params change) and of preparing a LUT (once per
// file), and the deviation from the reference. Per case:
//   ref      applyReference, ms per frame (one thread, as before the tables)
//   bake     bake(), µs (keyframes: once per frame whose values change)
//   1t / Nt  apply(baked) with 1 thread / N threads, ms per frame (best of the runs: the least disturbed one)
//   auto     apply(baked) with the default threads
//   max/mean deviation from the reference in code values (R, G, B)
// Last line: an animated grade (new values every frame: bake + apply) against the reference.
// bench_color_grade [image]: another picture instead of the generated test frame (testsrc2 via ffmpeg).

#include "check.h"

#include "engine/ColorGrade.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <numbers>
#include <thread>
#include <vector>

namespace {

using CP = ColorGrade::Params;
using Fn = std::function<void(float, float, float, float*)>;

std::shared_ptr<ColorGrade::Lut> makeLut(int n, bool is3d, const Fn& fn)
{
    auto lut = std::make_shared<ColorGrade::Lut>();
    lut->size = n;
    lut->is3d = is3d;
    if (is3d) {
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    float o[3];
                    fn(r / float(n - 1), g / float(n - 1), b / float(n - 1), o);
                    lut->data << o[0] << o[1] << o[2];
                }
    } else {
        for (int i = 0; i < n; ++i) {
            const float x = i / float(n - 1);
            float o[3];
            fn(x, x, x, o);
            lut->data << o[0] << o[1] << o[2];
        }
    }
    return lut;
}

// A "film look": channels mixed, a soft S-curve and a hue-dependent shift (smooth, but not separable)
void look(float r, float g, float b, float* o)
{
    auto s = [](float x) { return x + 0.15f * std::sin(float(2 * std::numbers::pi) * x) * (1 - x) * 0.8f; };
    const float y = 0.3f * r + 0.6f * g + 0.1f * b;
    o[0] = s(0.85f * r + 0.15f * y) + 0.04f * std::sin(6 * b);
    o[1] = s(0.9f * g + 0.1f * y);
    o[2] = s(0.8f * b + 0.2f * y) - 0.03f * std::cos(5 * r);
}
void curve1d(float r, float g, float b, float* o)
{
    o[0] = std::pow(r, 0.8f);
    o[1] = g * g * (3 - 2 * g);
    o[2] = std::sqrt(b) * 0.9f + 0.05f;
}

std::vector<uint8_t> testFrame(const QString& path)
{
    QImage img;
    if (!path.isEmpty()) img = QImage(path);
    QTemporaryDir dir;
    if (img.isNull()) {
        const QString png = Check::makeMedia(dir.filePath("f.png"), {"-f", "lavfi", "-i", "testsrc2=size=1920x1080",
                                                                    "-frames:v", "1"});
        if (!png.isEmpty()) img = QImage(png);
    }
    if (img.isNull()) { // without ffmpeg: smooth colors
        img = QImage(1920, 1080, QImage::Format_RGBA8888);
        for (int y = 0; y < 1080; ++y)
            for (int x = 0; x < 1920; ++x) img.setPixel(x, y, qRgb(x * 255 / 1919, y * 255 / 1079, (x + y) % 256));
    }
    img = img.convertToFormat(QImage::Format_RGBA8888).scaled(1920, 1080);
    return std::vector<uint8_t>(img.constBits(), img.constBits() + img.sizeInBytes());
}

constexpr int W = 1920, H = 1080;

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ms per call of fn on a fresh copy of the frame (best of `runs`)
double timeIt(const std::vector<uint8_t>& src, int runs, const std::function<void(uint8_t*)>& fn)
{
    std::vector<uint8_t> buf(src.size());
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) {
        std::memcpy(buf.data(), src.data(), src.size());
        QElapsedTimer e;
        e.start();
        fn(buf.data());
        t.push_back(e.nsecsElapsed() / 1e6);
    }
    return *std::min_element(t.begin(), t.end());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::vector<uint8_t> frame = testFrame(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());
    const int hw = int(std::thread::hardware_concurrency());
    { // warm up: the CPU clock rises first
        QElapsedTimer warm;
        warm.start();
        volatile double x = 0;
        while (warm.elapsed() < 300) x = x + std::sqrt(double(warm.nsecsElapsed()));
    }

    CP wheels;
    wheels.lift[0] = 0.05;
    wheels.gamma[0] = 0.1;
    wheels.gamma[3] = -0.05;
    wheels.gain[0] = 1.15;
    wheels.gain[1] = 1.05;
    wheels.contrast = 1.2;
    wheels.temperature = 600;
    CP sat = wheels;
    sat.saturation = 68;

    QElapsedTimer e;
    auto lut33 = makeLut(33, true, look);
    e.start();
    ColorGrade::prepare(*lut33);
    const double prep33 = e.nsecsElapsed() / 1e3;
    auto lut65 = makeLut(65, true, look);
    e.restart();
    ColorGrade::prepare(*lut65);
    const double prep65 = e.nsecsElapsed() / 1e3;
    auto lut1d = makeLut(1024, false, curve1d);
    ColorGrade::prepare(*lut1d);
    std::printf("prepare LUT (once per file, without parsing): 33^3 %.0f us, 65^3 %.0f us\n", prep33, prep65);

    struct Case {
        const char* name;
        CP p;
        const ColorGrade::Lut* lut;
    };
    const Case cases[] = {
        {"wheels", wheels, nullptr},          {"wheels+sat", sat, nullptr},
        {"1D LUT", wheels, lut1d.get()},      {"1D LUT+sat", sat, lut1d.get()},
        {"3D LUT 33", wheels, lut33.get()},   {"3D LUT 33+sat", sat, lut33.get()},
        {"3D LUT 65+sat", sat, lut65.get()},
    };
    const int threads[] = {2, 4, std::max(1, hw)};
    std::printf("1920x1080, %d hardware threads\n", hw);
    std::printf("%-15s %8s %8s %7s %7s %7s %7s %7s %5s %6s\n", "case", "ref ms", "bake us", "1t ms", "2t ms", "4t ms",
                "Nt ms", "auto ms", "max", "mean");
    for (const Case& c : cases) {
        const bool slow = c.lut != nullptr;
        const double ref = timeIt(frame, slow ? 5 : 21, [&](uint8_t* px) { ColorGrade::applyReference(px, W, H, c.p, c.lut); });
        std::vector<double> bt;
        std::shared_ptr<const ColorGrade::Baked> b;
        for (int i = 0; i < 21; ++i) {
            e.restart();
            b = ColorGrade::bake(c.p, c.lut);
            bt.push_back(e.nsecsElapsed() / 1e3);
        }
        const double t1 = timeIt(frame, 15, [&](uint8_t* px) { ColorGrade::apply(px, W, H, *b, 1); });
        double tn[3];
        for (int i = 0; i < 3; ++i) tn[i] = timeIt(frame, 25, [&](uint8_t* px) { ColorGrade::apply(px, W, H, *b, threads[i]); });
        const double ta = timeIt(frame, 25, [&](uint8_t* px) { ColorGrade::apply(px, W, H, *b); });
        std::vector<uint8_t> want = frame, got = frame;
        ColorGrade::applyReference(want.data(), W, H, c.p, c.lut);
        ColorGrade::apply(got.data(), W, H, *b);
        int mx = 0;
        double sum = 0;
        for (size_t i = 0; i < want.size(); ++i) {
            if (i % 4 == 3) continue;
            const int d = std::abs(int(want[i]) - int(got[i]));
            mx = std::max(mx, d);
            sum += d;
        }
        std::printf("%-15s %8.2f %8.1f %7.2f %7.2f %7.2f %7.2f %7.2f %5d %6.3f\n", c.name, ref, median(bt), t1, tn[0],
                    tn[1], tn[2], ta, mx, sum / (double(W) * H * 3));
    }

    // Animated: new values every frame (lift moves), 3D LUT + saturation
    const int frames = 10;
    std::vector<uint8_t> buf(frame.size());
    QElapsedTimer t;
    double ref = 0, baked = 0;
    for (int f = 0; f < frames; ++f) {
        CP p = sat;
        p.lift[0] = 0.01 * f;
        std::memcpy(buf.data(), frame.data(), frame.size());
        t.start();
        ColorGrade::applyReference(buf.data(), W, H, p, lut33.get());
        ref += t.nsecsElapsed() / 1e6;
        std::memcpy(buf.data(), frame.data(), frame.size());
        t.start();
        ColorGrade::apply(buf.data(), W, H, p, lut33.get());
        baked += t.nsecsElapsed() / 1e6;
    }
    std::printf("animated 3D LUT 33+sat (bake + apply each frame): ref %.2f ms, baked %.2f ms per frame\n", ref / frames,
                baked / frames);
    return 0;
}
