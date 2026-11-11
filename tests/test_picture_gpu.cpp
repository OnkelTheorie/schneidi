// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine on the GPU (engine/picture/gpu) against the CPU path it replaces: Y'CbCr upload + conversion
// (Video::toRgba), layer drawing (Compose::draw: affine, bilinear, mips, opacity, crop), colour grade (ColorGrade::apply
// via the baked 3D LUT), cross dissolve / luma wipe (Renderer's mix/luma), output conversion (OutputColor::fromRgba).
// Tolerance instead of bit equality. Skips (77) where no GPU device works (headless CI, software rasterizer);
// SCHNEIDI_GPU=1 runs it on a software device too. PICTURE_GPU_BENCH=n: time n frames of 3 layers 1080p + grade.

#include "check.h"

#include "engine/ColorGrade.h"
#include "engine/OutputColor.h"
#include "engine/picture/gpu/Gpu.h"
#include "engine/video/Compose.h"
#include "engine/video/Convert.h"

#include <QElapsedTimer>
#include <QGuiApplication>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <cmath>
#include <ctime>
#include <cstring>
#include <random>
#include <vector>

using namespace Picture;

namespace {

struct Diff {
    double mean = 0, psnr = 99;
    int max = 0;
};

Diff compare(const uint8_t* a, const uint8_t* b, size_t n, int channels = 4, int skip = -1)
{
    Diff d;
    double sum = 0, sq = 0;
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (skip >= 0 && int(i % channels) == skip) continue;
        const int e = std::abs(int(a[i]) - int(b[i]));
        sum += e;
        sq += double(e) * e;
        d.max = std::max(d.max, e);
        ++count;
    }
    d.mean = count ? sum / count : 0;
    const double mse = count ? sq / count : 0;
    d.psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99;
    return d;
}

QString text(const Diff& d) { return QString("mean %1 max %2 PSNR %3 dB").arg(d.mean, 0, 'f', 3).arg(d.max).arg(d.psnr, 0, 'f', 1); }

void show(const char* what, const Diff& d) { std::printf("  %-34s %s\n", what, qPrintable(text(d))); }

// A test picture with gradients, a fine pattern and (optionally) soft alpha
std::vector<uint8_t> testRgba(int w, int h, bool alpha, unsigned seed = 1)
{
    std::vector<uint8_t> v(size_t(w) * h * 4);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &v[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 255 / std::max(1, w - 1));
            p[1] = uint8_t(y * 255 / std::max(1, h - 1));
            p[2] = uint8_t(((x / 8 + y / 8) & 1) ? 200 : 40);
            if ((x / 3 + y / 5) % 7 == 0) p[0] = uint8_t(rng() & 255);
            p[3] = alpha ? uint8_t(std::clamp(int(255 * (1.0 - std::hypot(x - w / 2.0, y - h / 2.0) / (0.6 * w))), 0, 255)) : 255;
        }
    return v;
}

// A decoded-looking picture of pixel format fmt (gradients + detail in luma and chroma)
std::shared_ptr<Video::Picture> testYuv(int w, int h, AVPixelFormat fmt, unsigned seed = 2)
{
    auto pic = std::make_shared<Video::Picture>();
    pic->frame = av_frame_alloc();
    pic->frame->format = fmt;
    pic->frame->width = w;
    pic->frame->height = h;
    if (av_frame_get_buffer(pic->frame, 64) < 0) return {};
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    const int depth = desc->comp[0].depth, shift = desc->comp[0].shift;
    const bool wide = depth > 8;
    const int cw = AV_CEIL_RSHIFT(w, desc->log2_chroma_w), ch = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
    std::mt19937 rng(seed);
    auto put = [&](int plane, int x, int y, int step, int value) { // value 0..255 scaled to depth
        uint8_t* row = pic->frame->data[plane] + size_t(y) * pic->frame->linesize[plane];
        if (wide) reinterpret_cast<uint16_t*>(row)[x * step] = uint16_t((value << (depth - 8) | (value >> (16 - depth))) << shift);
        else row[x * step] = uint8_t(value);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = 16 + (x + y) * 219 / (w + h);
            if ((x / 4 + y / 4) % 9 == 0) v = 16 + int(rng() % 220);
            put(0, x, y, 1, v);
        }
    const bool semi = desc->comp[1].plane == desc->comp[2].plane;
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const int u = 16 + x * 224 / std::max(1, cw), v = 240 - y * 224 / std::max(1, ch);
            if (semi) {
                put(1, 2 * x, y, 1, u);
                put(1, 2 * x + 1, y, 1, v);
            } else {
                put(1, x, y, 1, u);
                put(2, x, y, 1, v);
            }
        }
    return pic;
}

Video::Info infoFor(const Video::Picture& pic, int colorspace, bool fullRange)
{
    Video::Info info;
    info.width = pic.frame->width;
    info.height = pic.frame->height;
    info.format = AVPixelFormat(pic.frame->format);
    info.colorspace = colorspace;
    info.fullRange = fullRange;
    return info;
}

// Renderer's mix() and luma() (engine/picture/Renderer.cpp, anonymous there): the CPU reference of the transitions
void cpuMix(uint8_t* d, const uint8_t* s, size_t n, double t)
{
    const int k = int(std::lround(std::clamp(t, 0.0, 1.0) * 256)), j = 256 - k;
    for (size_t i = 0; i < n; ++i, d += 4, s += 4) {
        const int aa = d[3] * j, ba = s[3] * k, alpha = aa + ba;
        if (alpha <= 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
            continue;
        }
        for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp((d[c] * aa + s[c] * ba + alpha / 2) / alpha, 0, 255));
        d[3] = uint8_t((alpha + 128) >> 8);
    }
}

void cpuLuma(uint8_t* d, const uint8_t* s, const uint8_t* grad, size_t n, double progress, double softness)
{
    const double weight = progress * (1 + softness);
    for (size_t i = 0; i < n; ++i, d += 4, s += 4) {
        const double v = grad[i] / 255.0;
        double k;
        if (weight < v) k = 0;
        else if (weight >= v + softness) k = 1;
        else {
            const double t = (weight - v) / softness;
            k = t * t * (3 - 2 * t);
        }
        if (k <= 0) continue;
        for (int c = 0; c < 4; ++c) d[c] = uint8_t(std::lround(d[c] + (s[c] - d[c]) * k));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("picture_gpu");

    QString why;
    const bool force = qgetenv("SCHNEIDI_GPU") == "1";
    std::unique_ptr<Gpu::Context> gpu = Gpu::Context::create(&why, force);
    if (!gpu) {
        std::printf("SKIP: no GPU device (%s)\n", qPrintable(why));
        return Check::kSkip;
    }
    std::printf("GPU: %s on %s\n", qPrintable(gpu->info().api), qPrintable(gpu->info().device));

    // 1. Straight RGBA through an identity draw: upright, alpha kept (premultiplied inside, straight back)
    {
        const int w = 97, h = 61;
        const std::vector<uint8_t> src = testRgba(w, h, true);
        std::vector<uint8_t> out(src.size());
        gpu->begin();
        Gpu::TexturePtr t = gpu->uploadRgba(src.data(), w, h, false);
        Gpu::TexturePtr canvas = gpu->target(QSize(w, h));
        gpu->draw(*canvas, {t.get(), {}, 1.0, nullptr, {}});
        gpu->readRgba(*canvas, out.data());
        gpu->finish();
        // Opaque-ish pixels exact up to premultiply rounding; compare where alpha >= 32
        std::vector<uint8_t> a, b;
        for (size_t i = 0; i < src.size(); i += 4)
            if (src[i + 3] >= 32) a.insert(a.end(), &src[i], &src[i + 4]), b.insert(b.end(), &out[i], &out[i + 4]);
        const Diff d = compare(a.data(), b.data(), a.size());
        show("identity RGBA (alpha >= 32)", d);
        CHECK(d.mean <= 1.0);
        CHECK(d.max <= 8);
        // Not upside down: green grows downwards; compare an opaque pixel near the top
        const size_t top = (size_t(10) * w + w / 2) * 4;
        CHECK(std::abs(int(out[top + 1]) - int(src[top + 1])) <= 1);
    }

    // 2. Compose::draw: scaled + rotated + opacity over an opaque background; and with a crop
    {
        const int W = 320, H = 180, sw = 211, sh = 143;
        const std::vector<uint8_t> bg = testRgba(W, H, false, 7), src = testRgba(sw, sh, true, 9);
        struct Case {
            const char* name;
            Compose::Affine m;
            double opacity;
            QRectF crop;
        };
        const Case cases[] = {
            {"draw scale 0.8 rot 17 op 0.6", Compose::placement(40.3, 20.7, 168.8, 114.4, 17, sw, sh, true, true), 0.6, {}},
            {"draw scale 1.37 (up)", Compose::placement(-30, -10, 289, 196, 0, sw, sh, true, true), 1.0, {}},
            {"draw crop", Compose::placement(10, 10, 211, 143, 0, sw, sh, true, true), 1.0, QRectF(20.5, 10, 150, 100)},
        };
        for (const Case& c : cases) {
            std::vector<uint8_t> cpu = bg, gpuOut(bg.size());
            std::vector<uint8_t> cropped = src;
            if (!c.crop.isEmpty()) {
                const uint8_t none[4] = {0, 0, 0, 0};
                Compose::crop(cropped.data(), sw, sh, c.crop.left(), c.crop.top(), c.crop.right(), c.crop.bottom(), none);
            }
            Compose::draw(cpu.data(), W, H, cropped.data(), sw, sh, c.m, c.opacity);
            gpu->begin();
            Gpu::TexturePtr b = gpu->uploadRgba(bg.data(), W, H, false);
            Gpu::TexturePtr s = gpu->uploadRgba(src.data(), sw, sh, false);
            Gpu::TexturePtr canvas = gpu->target(QSize(W, H));
            gpu->draw(*canvas, {b.get(), {}, 1.0, nullptr, {}});
            gpu->draw(*canvas, {s.get(), c.m, c.opacity, nullptr, c.crop});
            gpu->readRgba(*canvas, gpuOut.data());
            gpu->finish();
            const Diff d = compare(cpu.data(), gpuOut.data(), cpu.size());
            show(c.name, d);
            CHECK(d.mean <= 1.0);
            CHECK(d.psnr >= 38);
        }
    }

    // 3. Y'CbCr planes -> RGB at 1:1 against the CPU conversion (swscale, exact path)
    {
        struct Case {
            const char* name;
            AVPixelFormat fmt;
            int colorspace;
            bool full;
        };
        const Case cases[] = {
            {"yuv420p 709 limited 1:1", AV_PIX_FMT_YUV420P, 709, false},
            {"nv12 709 limited 1:1", AV_PIX_FMT_NV12, 709, false},
            {"yuvj420p 601 full 1:1", AV_PIX_FMT_YUVJ420P, 601, true},
            {"yuv422p 709 1:1", AV_PIX_FMT_YUV422P, 709, false},
            {"yuv444p 709 1:1", AV_PIX_FMT_YUV444P, 709, false},
            {"yuv420p10 709 1:1", AV_PIX_FMT_YUV420P10LE, 709, false},
            {"p010 709 1:1", AV_PIX_FMT_P010LE, 709, false},
            // No BT.2020 case: the CPU conversion has no 2020 matrix for swscale (takes the context's last one)
            {"yuv420p 601 1:1", AV_PIX_FMT_YUV420P, 601, false},
        };
        const int w = 256, h = 144;
        for (const Case& c : cases) {
            auto pic = testYuv(w, h, c.fmt);
            const Video::Info info = infoFor(*pic, c.colorspace, c.full);
            std::vector<uint8_t> cpu(size_t(w) * h * 4), out(cpu.size());
            if (!Video::toRgba(*pic, info, cpu.data(), w, h, false)) {
                std::printf("  %-34s CPU conversion not possible\n", c.name);
                continue;
            }
            gpu->begin();
            Gpu::TexturePtr t = gpu->uploadYuv(pic->frame, info);
            CHECK(t != nullptr);
            if (!t) {
                gpu->finish();
                continue;
            }
            Gpu::TexturePtr canvas = gpu->target(QSize(w, h));
            gpu->draw(*canvas, {t.get(), {}, 1.0, nullptr, {}});
            gpu->readRgba(*canvas, out.data());
            gpu->finish();
            const Diff d = compare(cpu.data(), out.data(), cpu.size());
            show(c.name, d);
            CHECK(d.mean <= 1.0);
            CHECK(d.psnr >= 40);
        }
    }

    // 4. Y'CbCr drawn smaller (mips) against the CPU path (toRgba at full size + Compose::draw)
    {
        const int w = 640, h = 360;
        auto pic = testYuv(w, h, AV_PIX_FMT_YUV420P);
        const Video::Info info = infoFor(*pic, 709, false);
        std::vector<uint8_t> full(size_t(w) * h * 4);
        Video::toRgba(*pic, info, full.data(), w, h, false);
        for (double k : {0.5, 0.3}) {
            const int W = 320, H = 180;
            const Compose::Affine m = Compose::placement(12, 8, w * k, h * k, 0, w, h, true, true);
            std::vector<uint8_t> cpu(size_t(W) * H * 4, 0), out(cpu.size());
            Compose::draw(cpu.data(), W, H, full.data(), w, h, m, 1.0);
            gpu->begin();
            Gpu::TexturePtr t = gpu->uploadYuv(pic->frame, info, true);
            Gpu::TexturePtr canvas = gpu->target(QSize(W, H));
            gpu->draw(*canvas, {t.get(), m, 1.0, nullptr, {}});
            gpu->readRgba(*canvas, out.data());
            gpu->finish();
            const Diff d = compare(cpu.data(), out.data(), cpu.size());
            show(k == 0.5 ? "yuv420p drawn at 0.5 (mip 1)" : "yuv420p drawn at 0.3 (mip 1)", d);
            CHECK(d.mean <= 1.5);
            CHECK(d.psnr >= 32);
        }
    }

    // 4b. Strong downscaling of a fine pattern (zone plate: every frequency up to 1/2 cycle per pixel): the card must
    // average like Compose::draw's halvings, not pick texels (that is stair-stepping / moire). RGBA (stills, layers made
    // on the CPU) and Y'CbCr, evenly and along one axis only (squeezed).
    {
        const int w = 1024, h = 768;
        std::vector<uint8_t> plate(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dx = x - w / 2.0, dy = y - h / 2.0;
                const uint8_t v = uint8_t(std::lround(127.5 + 127.5 * std::cos(M_PI * (dx * dx + dy * dy) / w)));
                uint8_t* p = &plate[(size_t(y) * w + x) * 4];
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
        // The same as Y'CbCr (full range, neutral chroma): toRgba gives back the plate
        auto yuv = std::make_shared<Video::Picture>();
        yuv->frame = av_frame_alloc();
        yuv->frame->format = AV_PIX_FMT_YUVJ444P;
        yuv->frame->width = w;
        yuv->frame->height = h;
        av_frame_get_buffer(yuv->frame, 64);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                yuv->frame->data[0][size_t(y) * yuv->frame->linesize[0] + x] = plate[(size_t(y) * w + x) * 4];
                yuv->frame->data[1][size_t(y) * yuv->frame->linesize[1] + x] = 128;
                yuv->frame->data[2][size_t(y) * yuv->frame->linesize[2] + x] = 128;
            }
        const Video::Info yinfo = infoFor(*yuv, 709, true);
        struct Case {
            const char* name;
            double kx, ky;
            bool planes;
        };
        const Case cases[] = {
            {"zone plate RGBA at 1/4", 0.25, 0.25, false},
            {"zone plate RGBA at 0.18", 0.18, 0.18, false},
            {"zone plate RGBA at 1/4 x, 1 y", 0.25, 1.0, false},
            {"zone plate yuv at 1/4", 0.25, 0.25, true},
            {"zone plate yuv at 1/4 x, 0.9 y", 0.25, 0.9, true},
        };
        for (const Case& c : cases) {
            const int W = int(std::ceil(w * c.kx)) + 8, H = int(std::ceil(h * c.ky)) + 8;
            const Compose::Affine m = Compose::placement(4, 4, w * c.kx, h * c.ky, 0, w, h, true, true);
            std::vector<uint8_t> cpu(size_t(W) * H * 4, 0), out(cpu.size());
            Compose::draw(cpu.data(), W, H, plate.data(), w, h, m, 1.0);
            gpu->begin();
            Gpu::TexturePtr t = c.planes ? gpu->uploadYuv(yuv->frame, yinfo, true) : gpu->uploadRgba(plate.data(), w, h, true);
            if (!CHECK(t != nullptr)) {
                gpu->finish();
                continue;
            }
            Gpu::TexturePtr canvas = gpu->target(QSize(W, H));
            gpu->draw(*canvas, {t.get(), m, 1.0, nullptr, {}});
            gpu->readRgba(*canvas, out.data());
            gpu->finish();
            const Diff d = compare(cpu.data(), out.data(), cpu.size(), 4, 3);
            show(c.name, d);
            // Against the plate itself averaged over each target pixel (8x8 points of the formula, inside only): how
            // much of the moire is left, the card next to the CPU
            std::vector<uint8_t> ideal, cpuIn, gpuIn;
            for (int Y = 6; Y < H - 6 && 4 + (Y + 2) / c.ky < 4 + h; ++Y)
                for (int X = 6; X < W - 6; ++X) {
                    const double x0 = (X - 4) / c.kx, y0 = (Y - 4) / c.ky;
                    if (x0 + 1 / c.kx > w - 2 || y0 + 1 / c.ky > h - 2) continue;
                    double sum = 0;
                    for (int j = 0; j < 8; ++j)
                        for (int i = 0; i < 8; ++i) {
                            const double dx = x0 + (i + 0.5) / 8 / c.kx - 0.5 - w / 2.0, dy = y0 + (j + 0.5) / 8 / c.ky - 0.5 - h / 2.0;
                            sum += 127.5 + 127.5 * std::cos(M_PI * (dx * dx + dy * dy) / w);
                        }
                    ideal.push_back(uint8_t(std::lround(sum / 64)));
                    cpuIn.push_back(cpu[(size_t(Y) * W + X) * 4]);
                    gpuIn.push_back(out[(size_t(Y) * W + X) * 4]);
                }
            const Diff ci = compare(ideal.data(), cpuIn.data(), ideal.size(), 1), gi = compare(ideal.data(), gpuIn.data(), ideal.size(), 1);
            std::printf("  %-34s area average: CPU %s, GPU %s\n", "", qPrintable(text(ci)), qPrintable(text(gi)));
            // Picked texels differ from the average by up to the full contrast in the fine rings (~15 dB both ways
            // before). Whole halvings: the same picture as the CPU. 0.18 (5.6:1): the CPU halves twice and then
            // bilinear at 1.4:1 still skips texels; the card averages the whole footprint - closer to the area
            // average than the CPU, so only that comparison counts there
            if (c.kx != 0.18) {
                CHECK(d.psnr >= 40);
                CHECK(d.mean <= 1.0);
            }
            CHECK(gi.psnr >= 32);
            CHECK(gi.psnr >= ci.psnr - 1.0);
        }
    }

    // 5. Colour grade: baked correction on the CPU vs the 3D LUT on the card
    {
        const int w = 256, h = 160;
        const std::vector<uint8_t> src = testRgba(w, h, false, 11);
        struct Case {
            const char* name;
            ColorGrade::Params p;
        };
        std::vector<Case> cases;
        {
            ColorGrade::Params p;
            p.lift[0] = 0.05, p.gamma[1] = 0.1, p.gain[3] = 1.2, p.saturation = 70, p.contrast = 1.2;
            cases.push_back({"grade lift/gamma/gain/sat/contrast", p});
        }
        {
            ColorGrade::Params p;
            p.temperature = 1500, p.tint = -20, p.exposure = 0.5, p.offset[2] = 30;
            cases.push_back({"grade temperature/tint/exposure", p});
        }
        for (const Case& c : cases) {
            std::vector<uint8_t> cpu = src, out(src.size());
            auto baked = ColorGrade::bake(c.p, nullptr);
            ColorGrade::apply(cpu.data(), w, h, *baked, 1);
            gpu->begin();
            Gpu::LutPtr lut = gpu->gradeLut(*baked);
            Gpu::TexturePtr t = gpu->uploadRgba(src.data(), w, h, false);
            Gpu::TexturePtr canvas = gpu->target(QSize(w, h));
            gpu->draw(*canvas, {t.get(), {}, 1.0, lut.get(), {}});
            gpu->readRgba(*canvas, out.data());
            gpu->finish();
            const Diff d = compare(cpu.data(), out.data(), cpu.size());
            show(c.name, d);
            CHECK(d.mean <= 1.0);
            CHECK(d.max <= 6);
        }
    }

    // 6. Transitions: cross dissolve and luma wipe
    {
        const int w = 200, h = 120;
        const std::vector<uint8_t> a = testRgba(w, h, true, 3), b = testRgba(w, h, false, 4);
        std::vector<uint8_t> grad(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) grad[size_t(y) * w + x] = uint8_t((x * 255 / (w - 1) + y) / 2);
        for (double t : {0.25, 0.6}) {
            std::vector<uint8_t> cpu = a, out(a.size()), cpuL = a, outL(a.size());
            cpuMix(cpu.data(), b.data(), size_t(w) * h, t);
            cpuLuma(cpuL.data(), b.data(), grad.data(), size_t(w) * h, t, 0.2);
            gpu->begin();
            Gpu::TexturePtr ta = gpu->target(QSize(w, h)), tb = gpu->target(QSize(w, h));
            Gpu::TexturePtr ua = gpu->uploadRgba(a.data(), w, h, false), ub = gpu->uploadRgba(b.data(), w, h, false);
            gpu->draw(*ta, {ua.get(), {}, 1.0, nullptr, {}});
            gpu->draw(*tb, {ub.get(), {}, 1.0, nullptr, {}});
            Gpu::TexturePtr g = gpu->uploadGray(grad.data(), w, h);
            Gpu::TexturePtr m = gpu->mix(*ta, *tb, t);
            Gpu::TexturePtr l = gpu->luma(*ta, *tb, *g, t, 0.2);
            gpu->readRgba(*m, out.data());
            gpu->readRgba(*l, outL.data());
            gpu->finish();
            const Diff d = compare(cpu.data(), out.data(), cpu.size()), dl = compare(cpuL.data(), outL.data(), cpuL.size());
            show(t < 0.5 ? "dissolve 0.25" : "dissolve 0.6", d);
            show(t < 0.5 ? "luma wipe 0.25" : "luma wipe 0.6", dl);
            CHECK(d.mean <= 1.0);
            CHECK(dl.mean <= 1.0);
        }
    }

    // 7. Output conversion for the encoder: Y'CbCr planes read back vs OutputColor::fromRgba
    {
        const int w = 320, h = 180;
        const std::vector<uint8_t> src = testRgba(w, h, false, 5);
        for (auto [layout, cs, full] : {std::tuple{OutputColor::Layout::Yuv420, 709, false},
                                        std::tuple{OutputColor::Layout::Nv12, 709, false},
                                        std::tuple{OutputColor::Layout::Yuv420, 601, true}}) {
            const bool nv = layout == OutputColor::Layout::Nv12;
            std::vector<uint8_t> cy(size_t(w) * h), cu(size_t(w / 2) * (h / 2) * (nv ? 2 : 1)), cv(size_t(w / 2) * (h / 2));
            std::vector<uint8_t> gy(cy.size()), gu(cu.size()), gv(cv.size());
            OutputColor::Target ct;
            ct.layout = layout;
            ct.colorspace = cs;
            ct.fullRange = full;
            OutputColor::Target gt = ct;
            ct.planes[0] = cy.data(), ct.planes[1] = cu.data(), ct.planes[2] = cv.data();
            gt.planes[0] = gy.data(), gt.planes[1] = gu.data(), gt.planes[2] = gv.data();
            ct.strides[0] = gt.strides[0] = w;
            ct.strides[1] = gt.strides[1] = nv ? w : w / 2;
            ct.strides[2] = gt.strides[2] = w / 2;
            OutputColor::fromRgba(src.data(), w, h, w * 4, ct);
            gpu->begin();
            Gpu::TexturePtr t = gpu->uploadRgba(src.data(), w, h, false);
            Gpu::TexturePtr canvas = gpu->target(QSize(w, h));
            gpu->draw(*canvas, {t.get(), {}, 1.0, nullptr, {}});
            CHECK(gpu->readYuv(*canvas, gt));
            gpu->finish();
            const Diff dy = compare(cy.data(), gy.data(), cy.size(), 1), du = compare(cu.data(), gu.data(), cu.size(), 1);
            const char* name = nv ? "to nv12 709" : (full ? "to yuv420p 601 full" : "to yuv420p 709");
            std::printf("  %-34s Y %s | C %s\n", name, qPrintable(text(dy)), qPrintable(text(du)));
            CHECK(dy.max <= 1);
            CHECK(du.max <= 1);
            if (!nv) {
                const Diff dv = compare(cv.data(), gv.data(), cv.size(), 1);
                CHECK(dv.max <= 1);
            }
        }
    }

    // 8. Timing: 3 layers 1080p yuv420p (full frame + two pictures in picture), grade on the bottom one, to yuv420p
    const int benchFrames = qEnvironmentVariableIntValue("PICTURE_GPU_BENCH");
    if (benchFrames > 0) {
        const int W = 1920, H = 1080;
        std::vector<std::shared_ptr<Video::Picture>> pics;
        for (int i = 0; i < 3; ++i) pics.push_back(testYuv(W, H, AV_PIX_FMT_YUV420P, 20 + i));
        const Video::Info info = infoFor(*pics[0], 709, false);
        ColorGrade::Params gp;
        gp.lift[0] = 0.03, gp.gain[1] = 1.1, gp.saturation = 60;
        const Compose::Affine pip1 = Compose::placement(1200, 60, 640, 360, 0, W, H, true, true);
        const Compose::Affine pip2 = Compose::placement(80, 640, 800, 450, 8, W, H, true, true);
        std::vector<uint8_t> yp(size_t(W) * H), up(size_t(W / 2) * (H / 2)), vp(up.size());
        OutputColor::Target ot;
        ot.planes[0] = yp.data(), ot.planes[1] = up.data(), ot.planes[2] = vp.data();
        ot.strides[0] = W, ot.strides[1] = ot.strides[2] = W / 2;

        // CPU: what Renderer does per frame (convert, grade, draw two scaled layers, encoder conversion)
        std::vector<uint8_t> canvas(size_t(W) * H * 4), layer(canvas.size());
        QElapsedTimer timer;
        timer.start();
        const std::clock_t cpuStart = std::clock();
        for (int f = 0; f < benchFrames; ++f) {
            Video::toRgba(*pics[0], info, canvas.data(), W, H, false);
            ColorGrade::apply(canvas.data(), W, H, *ColorGrade::bake(gp, nullptr));
            Video::toRgba(*pics[1], info, layer.data(), 640, 360, false); // Renderer scales in swscale directly
            Compose::draw(canvas.data(), W, H, layer.data(), 640, 360, Compose::placement(1200, 60, 640, 360, 0, 640, 360, true, true), 1.0);
            Video::toRgba(*pics[2], info, layer.data(), W, H, false);
            Compose::draw(canvas.data(), W, H, layer.data(), W, H, pip2, 1.0);
            Compose::forRows(H, 64, [&](int r0, int r1) { OutputColor::fromRgba(canvas.data(), W, H, W * 4, ot, r0, r1); });
        }
        const double cpuMs = timer.nsecsElapsed() / 1e6 / benchFrames;
        const std::clock_t cpuClock = std::clock() - cpuStart; // process CPU time (Linux; wall time on Windows)

        // GPU: upload the planes, three draws (grade in the bottom one), then: read back yuv420p (export), read back
        // RGBA (viewer today), nothing (picture stays on the card: viewer sharing the texture later)
        std::vector<uint8_t> rgba(size_t(W) * H * 4);
        const char* names[] = {"yuv420p readback", "RGBA readback", "no readback"};
        double gpuMs[3], gpuCpuMs[3];
        for (int variant = 0; variant < 3; ++variant) {
            timer.restart();
            const std::clock_t c0 = std::clock();
            for (int f = 0; f < benchFrames; ++f) {
                gpu->begin();
                auto lut = gpu->gradeLut(*ColorGrade::bake(gp, nullptr));
                Gpu::TexturePtr t0 = gpu->uploadYuv(pics[0]->frame, info);
                Gpu::TexturePtr t1 = gpu->uploadYuv(pics[1]->frame, info, true);
                Gpu::TexturePtr t2 = gpu->uploadYuv(pics[2]->frame, info);
                Gpu::TexturePtr c = gpu->target(QSize(W, H));
                gpu->draw(*c, {t0.get(), {}, 1.0, lut.get(), {}});
                gpu->draw(*c, {t1.get(), pip1, 1.0, nullptr, {}});
                gpu->draw(*c, {t2.get(), pip2, 1.0, nullptr, {}});
                if (variant == 0) gpu->readYuv(*c, ot);
                else if (variant == 1) gpu->readRgba(*c, rgba.data());
                gpu->finish();
            }
            gpuMs[variant] = timer.nsecsElapsed() / 1e6 / benchFrames;
            gpuCpuMs[variant] = double(std::clock() - c0) * 1000.0 / CLOCKS_PER_SEC / benchFrames;
        }
        const double cpuCpuMs = cpuClock * 1000.0 / CLOCKS_PER_SEC / benchFrames;
        std::printf("BENCH 1080p 3 layers + grade (%d frames), wall / process CPU per frame:\n", benchFrames);
        std::printf("  CPU path -> yuv420p        %6.2f ms / %6.2f ms\n", cpuMs, cpuCpuMs);
        for (int v = 0; v < 3; ++v) std::printf("  GPU %-22s %6.2f ms / %6.2f ms\n", names[v], gpuMs[v], gpuCpuMs[v]);
    }

    std::printf("passes last frame %d, pooled textures %d\n", gpu->stats().passes, gpu->stats().texturesPooled);

    // Pool: textures of sizes nobody asks for any more are released after a while (each title size stayed for good)
    {
        for (int i = 0; i < 20; ++i) { // 20 sizes once (a title being typed)
            gpu->begin();
            Gpu::TexturePtr t = gpu->target(QSize(40 + i, 30));
            gpu->finish();
        }
        const int before = gpu->stats().texturesPooled;
        for (int i = 0; i < 400; ++i) { // then frames of one size only
            gpu->begin();
            Gpu::TexturePtr t = gpu->target(QSize(64, 36));
            gpu->finish();
        }
        const int after = gpu->stats().texturesPooled;
        std::printf("pool: %d textures after 20 sizes, %d after 400 frames of one size\n", before, after);
        CHECK(before >= 20);
        CHECK(after <= 3);
    }
    gpu.reset();
    return Check::result();
}
