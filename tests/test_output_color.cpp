// SPDX-License-Identifier: AGPL-3.0-only
// OutputColor::fromRgba (export: finished RGBA frames into the encoder's picture format in the render workers) against
// the Y'CbCr formula (ITU-R BT.601/709/2020, SMPTE 240M; limited and full range), for every layout targetOf() knows:
// yuv420p/422p/444p and their full-range "j" variants, 10-bit planar (low bits), NV12, P010 (10 bit in the high bits),
// RGB24, RGBA. Chroma of subsampled layouts = the mean of the pixels it covers; a picture split into row bands (the
// workers) = the picture in one go; unknown pixel formats are refused.

#include "check.h"

#include "engine/OutputColor.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <QCoreApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct Ycc {
    double y, cb, cr;
};

void weights(int cs, double& kr, double& kb)
{
    switch (cs) {
    case 240: kr = 0.212, kb = 0.087; break;
    case 709: kr = 0.2126, kb = 0.0722; break;
    case 2020: kr = 0.2627, kb = 0.0593; break;
    default: kr = 0.299, kb = 0.114; break; // 601 family
    }
}

// R'G'B' 0..255 -> Y'CbCr code values of `bits` bits (exact, unrounded)
Ycc encode(double r, double g, double b, int cs, int bits, bool full)
{
    double kr, kb;
    weights(cs, kr, kb);
    r /= 255, g /= 255, b /= 255;
    const double y = kr * r + (1 - kr - kb) * g + kb * b;
    const double pb = (b - y) / (2 * (1 - kb)), pr = (r - y) / (2 * (1 - kr));
    const double s = double(1 << (bits - 8));
    if (full) return {s * 255 * y, s * (128 + 255 * pb), s * (128 + 255 * pr)};
    return {s * (16 + 219 * y), s * (128 + 224 * pb), s * (128 + 224 * pr)};
}

struct Format {
    const char* name;
    int av;
    OutputColor::Layout layout;
    int bits;
    bool msb, full;
};

const Format kFormats[] = {
    {"yuv420p", AV_PIX_FMT_YUV420P, OutputColor::Layout::Yuv420, 8, false, false},
    {"yuvj420p", AV_PIX_FMT_YUVJ420P, OutputColor::Layout::Yuv420, 8, false, true},
    {"yuv422p", AV_PIX_FMT_YUV422P, OutputColor::Layout::Yuv422, 8, false, false},
    {"yuvj422p", AV_PIX_FMT_YUVJ422P, OutputColor::Layout::Yuv422, 8, false, true},
    {"yuv444p", AV_PIX_FMT_YUV444P, OutputColor::Layout::Yuv444, 8, false, false},
    {"yuvj444p", AV_PIX_FMT_YUVJ444P, OutputColor::Layout::Yuv444, 8, false, true},
    {"yuv420p10le", AV_PIX_FMT_YUV420P10LE, OutputColor::Layout::Yuv420, 10, false, false},
    {"yuv422p10le", AV_PIX_FMT_YUV422P10LE, OutputColor::Layout::Yuv422, 10, false, false},
    {"yuv444p10le", AV_PIX_FMT_YUV444P10LE, OutputColor::Layout::Yuv444, 10, false, false},
    {"nv12", AV_PIX_FMT_NV12, OutputColor::Layout::Nv12, 8, false, false},
    {"p010le", AV_PIX_FMT_P010LE, OutputColor::Layout::Nv12, 10, true, false},
};

// A converted picture with its planes (sizes from the layout)
struct Planes {
    int w = 0, h = 0, cw = 0, ch = 0, bytes = 1;
    bool nv = false;
    std::vector<uint8_t> p[3];
    OutputColor::Target target;

    Planes(const Format& f, int width, int height, int colorspace)
    {
        w = width, h = height;
        bytes = f.bits > 8 ? 2 : 1;
        nv = f.layout == OutputColor::Layout::Nv12;
        cw = f.layout == OutputColor::Layout::Yuv444 ? w : w / 2;
        ch = f.layout == OutputColor::Layout::Yuv420 || nv ? h / 2 : h;
        CHECK(OutputColor::targetOf(f.av, target));
        CHECK(target.layout == f.layout && target.bits == f.bits && target.msb == f.msb && target.fullRange == f.full);
        target.colorspace = colorspace;
        p[0].assign(size_t(w) * h * bytes, 0xee);
        target.planes[0] = p[0].data();
        target.strides[0] = w * bytes;
        if (nv) {
            p[1].assign(size_t(cw) * 2 * ch * bytes, 0xee);
            target.planes[1] = p[1].data();
            target.strides[1] = cw * 2 * bytes;
        } else {
            for (int i : {1, 2}) {
                p[i].assign(size_t(cw) * ch * bytes, 0xee);
                target.planes[i] = p[i].data();
                target.strides[i] = cw * bytes;
            }
        }
    }
    int read(int plane, int index, int shift) const
    {
        const uint8_t* d = p[plane].data();
        return bytes == 2 ? (d[2 * index] | d[2 * index + 1] << 8) >> shift : d[index];
    }
    int y(int x, int yy, int shift) const { return read(0, yy * w + x, shift); }
    int cb(int cx, int cy, int shift) const { return nv ? read(1, cy * cw * 2 + 2 * cx, shift) : read(1, cy * cw + cx, shift); }
    int cr(int cx, int cy, int shift) const { return nv ? read(1, cy * cw * 2 + 2 * cx + 1, shift) : read(2, cy * cw + cx, shift); }
};

const std::array<std::array<int, 3>, 9> kColors{{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {0, 0, 0},
                                                 {191, 191, 0}, {0, 191, 191}, {128, 128, 128}, {25, 13, 51}}};

// Every format, every standard: a picture of 2x2 blocks of one colour each -> every sample within rounding of the formula
void testFlatBlocks()
{
    const int w = 2 * int(kColors.size()), h = 4;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const auto& c = kColors[size_t(x / 2)];
            uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(c[0]), p[1] = uint8_t(c[1]), p[2] = uint8_t(c[2]), p[3] = uint8_t(x * 40); // alpha ignored
        }
    for (const Format& f : kFormats)
        for (int cs : {601, 709, 2020, 240}) {
            Planes out(f, w, h, cs);
            OutputColor::fromRgba(rgba.data(), w, h, w * 4, out.target);
            const int shift = f.msb ? 6 : 0;
            // 10-bit low bits: nothing above bit 9; P010: the low 6 bits empty
            bool wordsOk = true;
            if (f.bits == 10)
                for (int i = 0; i < w * h; ++i) {
                    const int raw = out.read(0, i, 0);
                    wordsOk &= f.msb ? (raw & 63) == 0 : raw < 1024;
                }
            double worst = 0;
            const int sx = w / out.cw, sy = h / out.ch;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const auto& c = kColors[size_t(x / 2)];
                    const Ycc want = encode(c[0], c[1], c[2], cs, f.bits, f.full);
                    worst = std::max(worst, std::abs(out.y(x, y, shift) - want.y));
                    if (x % sx == 0 && y % sy == 0) {
                        const int max = (1 << f.bits) - 1;
                        worst = std::max(worst, std::abs(out.cb(x / sx, y / sy, shift) - std::clamp(want.cb, 0.0, double(max))));
                        worst = std::max(worst, std::abs(out.cr(x / sx, y / sy, shift) - std::clamp(want.cr, 0.0, double(max))));
                    }
                }
            // Fixed point: within rounding (0.5) plus a hair of the 16-bit coefficients
            if (!CHECK(worst <= 0.51 && wordsOk))
                std::printf("       %s, BT.%d: largest difference %.3f%s\n", f.name, cs, worst, wordsOk ? "" : ", bad words");
        }
}

// Reference values by hand for the common case (yuv420p BT.709 limited): white, black, 75 % bars as encoders know them
void testKnownValues()
{
    const int w = 2, h = 2;
    auto one = [&](int r, int g, int b, const Format& f, int cs) {
        std::vector<uint8_t> rgba(size_t(w) * h * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) rgba[i] = uint8_t(r), rgba[i + 1] = uint8_t(g), rgba[i + 2] = uint8_t(b);
        Planes out(f, w, h, cs);
        OutputColor::fromRgba(rgba.data(), w, h, w * 4, out.target);
        const int shift = f.msb ? 6 : 0;
        return std::array<int, 3>{out.y(0, 0, shift), out.cb(0, 0, shift), out.cr(0, 0, shift)};
    };
    const Format& yuv420 = kFormats[0];
    const Format& yuvj420 = kFormats[1];
    const Format& p10 = kFormats[6];
    CHECK((one(255, 255, 255, yuv420, 709) == std::array<int, 3>{235, 128, 128}));
    CHECK((one(0, 0, 0, yuv420, 709) == std::array<int, 3>{16, 128, 128}));
    CHECK((one(0, 0, 0, yuvj420, 709) == std::array<int, 3>{0, 128, 128}));
    CHECK((one(255, 255, 255, yuvj420, 709) == std::array<int, 3>{255, 128, 128}));
    CHECK((one(255, 255, 255, p10, 709) == std::array<int, 3>{940, 512, 512}));
    CHECK((one(0, 0, 0, p10, 709) == std::array<int, 3>{64, 512, 512}));
    // 100 % red: BT.709 Y 63, Cb 102, Cr 240; BT.601 Y 81, Cb 90, Cr 240
    CHECK((one(255, 0, 0, yuv420, 709) == std::array<int, 3>{63, 102, 240}));
    CHECK((one(255, 0, 0, yuv420, 601) == std::array<int, 3>{81, 90, 240}));
    // P010 = the same values as 10-bit planar, shifted up
    CHECK((one(255, 0, 0, kFormats[10], 709) == one(255, 0, 0, p10, 709)));
    // The 601 family (170M, 470BG, 624) is one matrix; an unknown standard falls back to 709
    CHECK((one(10, 200, 90, yuv420, 170) == one(10, 200, 90, yuv420, 601)));
    CHECK((one(10, 200, 90, yuv420, 470) == one(10, 200, 90, yuv420, 601)));
    CHECK((one(10, 200, 90, yuv420, 999) == one(10, 200, 90, yuv420, 709)));
}

// Chroma of a subsampled layout is the mean of its pixels: a 2x2 block of black and white -> mid gray chroma of the
// mean colour (here neutral: 128), the luma per pixel; a red/blue pair -> the mean of their chroma
void testChromaMean()
{
    const int w = 2, h = 2;
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
    auto set = [&](int x, int y, int r, int g, int b) {
        uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
        p[0] = uint8_t(r), p[1] = uint8_t(g), p[2] = uint8_t(b);
    };
    set(0, 0, 255, 0, 0);
    set(1, 0, 0, 0, 255);
    set(0, 1, 255, 0, 0);
    set(1, 1, 0, 0, 255);
    for (const Format& f : kFormats) {
        if (f.layout == OutputColor::Layout::Yuv444) continue;
        Planes out(f, w, h, 709);
        OutputColor::fromRgba(rgba.data(), w, h, w * 4, out.target);
        const int shift = f.msb ? 6 : 0;
        const Ycc red = encode(255, 0, 0, 709, f.bits, f.full), blue = encode(0, 0, 255, 709, f.bits, f.full);
        const double cb = (red.cb + blue.cb) / 2, cr = (red.cr + blue.cr) / 2;
        const bool ok = std::abs(out.cb(0, 0, shift) - cb) <= 0.51 && std::abs(out.cr(0, 0, shift) - cr) <= 0.51
            && std::abs(out.y(0, 0, shift) - red.y) <= 0.51 && std::abs(out.y(1, 0, shift) - blue.y) <= 0.51;
        if (!CHECK(ok))
            std::printf("       %s: Cb %d (want %.2f), Cr %d (want %.2f)\n", f.name, out.cb(0, 0, shift), cb,
                        out.cr(0, 0, shift), cr);
    }
}

// Row bands (the export splits a frame over threads) = the whole picture at once; odd sizes of noise
void testBands()
{
    const int w = 64, h = 38;
    std::vector<uint8_t> rgba(size_t(w) * (h + 1) * 4); // stride wider than the row: one pixel of padding
    const int stride = (w + 1) * 4;
    uint32_t s = 12345;
    for (uint8_t& v : rgba) v = uint8_t((s = s * 1103515245 + 12345) >> 16);
    for (const Format& f : kFormats) {
        Planes whole(f, w, h, 709), bands(f, w, h, 709);
        OutputColor::fromRgba(rgba.data(), w, h, stride, whole.target);
        for (int y0 = 0; y0 < h; y0 += 10) OutputColor::fromRgba(rgba.data(), w, h, stride, bands.target, y0, y0 + 10);
        bool same = true;
        for (int i = 0; i < 3; ++i) same &= whole.p[i] == bands.p[i];
        if (!CHECK(same)) std::printf("       %s: bands differ from one pass\n", f.name);
        // nothing left unwritten
        bool written = true;
        for (int i = 0; i < 3; ++i)
            if (!whole.p[i].empty()) written &= !std::all_of(whole.p[i].begin(), whole.p[i].begin() + 4, [](uint8_t v) { return v == 0xee; });
        CHECK(written);
    }
}

void testPacked()
{
    const int w = 3, h = 2;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = uint8_t(i * 11 + 3);
    OutputColor::Target rgb, rgbaT;
    CHECK(OutputColor::targetOf(AV_PIX_FMT_RGB24, rgb) && rgb.layout == OutputColor::Layout::Rgb24);
    CHECK(OutputColor::targetOf(AV_PIX_FMT_RGBA, rgbaT) && rgbaT.layout == OutputColor::Layout::Rgba);
    std::vector<uint8_t> a(size_t(w) * h * 3), b(rgba.size());
    rgb.planes[0] = a.data();
    rgb.strides[0] = w * 3;
    rgbaT.planes[0] = b.data();
    rgbaT.strides[0] = w * 4;
    OutputColor::fromRgba(rgba.data(), w, h, w * 4, rgb);
    OutputColor::fromRgba(rgba.data(), w, h, w * 4, rgbaT);
    bool same = b == rgba;
    for (int i = 0; i < w * h; ++i) same &= std::memcmp(&a[size_t(i) * 3], &rgba[size_t(i) * 4], 3) == 0;
    CHECK(same);
    // Not one of the layouts: the caller converts another way
    OutputColor::Target t;
    CHECK(!OutputColor::targetOf(AV_PIX_FMT_YUYV422, t));
    CHECK(!OutputColor::targetOf(AV_PIX_FMT_GBRP, t));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("output_color");
    testFlatBlocks();
    testKnownValues();
    testChromaMean();
    testBands();
    testPacked();
    return Check::result();
}
