// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/OutputColor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace OutputColor {

namespace {

// Luma weights (Kr, Kb) of a standard; false = unknown
bool weights(int colorspace, double& kr, double& kb)
{
    switch (colorspace) {
    case 601:
    case 170:
    case 470:
    case 624: kr = 0.299, kb = 0.114; return true;
    case 240: kr = 0.212, kb = 0.087; return true;
    case 709: kr = 0.2126, kb = 0.0722; return true;
    case 2020: kr = 0.2627, kb = 0.0593; return true;
    default: return false;
    }
}

} // namespace

// ---- RGBA -> encoder picture ----

bool targetOf(int avPixelFormat, Target& t)
{
    switch (static_cast<AVPixelFormat>(avPixelFormat)) {
    case AV_PIX_FMT_YUV420P: t.layout = Layout::Yuv420, t.bits = 8; return true;
    case AV_PIX_FMT_YUVJ420P: t.layout = Layout::Yuv420, t.bits = 8, t.fullRange = true; return true;
    case AV_PIX_FMT_YUV422P: t.layout = Layout::Yuv422, t.bits = 8; return true;
    case AV_PIX_FMT_YUVJ422P: t.layout = Layout::Yuv422, t.bits = 8, t.fullRange = true; return true;
    case AV_PIX_FMT_YUV444P: t.layout = Layout::Yuv444, t.bits = 8; return true;
    case AV_PIX_FMT_YUVJ444P: t.layout = Layout::Yuv444, t.bits = 8, t.fullRange = true; return true;
    case AV_PIX_FMT_YUV420P10LE: t.layout = Layout::Yuv420, t.bits = 10; return true;
    case AV_PIX_FMT_YUV422P10LE: t.layout = Layout::Yuv422, t.bits = 10; return true;
    case AV_PIX_FMT_YUV444P10LE: t.layout = Layout::Yuv444, t.bits = 10; return true;
    case AV_PIX_FMT_NV12: t.layout = Layout::Nv12, t.bits = 8; return true;
    case AV_PIX_FMT_P010LE: t.layout = Layout::Nv12, t.bits = 10, t.msb = true; return true;
    case AV_PIX_FMT_RGB24: t.layout = Layout::Rgb24; return true;
    case AV_PIX_FMT_RGBA: t.layout = Layout::Rgba; return true;
    default: return false;
    }
}

namespace {

// Fixed-point RGB -> Y'CbCr coefficients for code values of `bits` bits. Y = (yr R + yg G + yb B + yOff) >> kS,
// Cb = (br R + bg G + bb B + cOff) >> kS (same for Cr); R, G, B are 8-bit (sums of n pixels: the offsets are times n)
struct Coeffs {
    static constexpr int kS = 16;
    int yr, yg, yb, br, bg, bb, rr, rg, rb;
    int64_t yOff, cOff;
};

Coeffs coeffs(int colorspace, int bits, bool fullRange)
{
    double kr, kb;
    if (!weights(colorspace, kr, kb)) weights(709, kr, kb);
    const double kg = 1 - kr - kb;
    const double scale = double(1 << (bits - 8));
    const double ly = (fullRange ? 255.0 : 219.0) / 255.0 * scale, lc = (fullRange ? 255.0 : 224.0) / 255.0 * scale;
    const double one = 1 << Coeffs::kS;
    Coeffs c;
    c.yr = int(std::lround(kr * ly * one));
    c.yg = int(std::lround(kg * ly * one));
    c.yb = int(std::lround(kb * ly * one));
    // Pb = (B - Y) / (2 (1 - Kb)), Pr = (R - Y) / (2 (1 - Kr))
    c.br = int(std::lround(-kr / (2 * (1 - kb)) * lc * one));
    c.bg = int(std::lround(-kg / (2 * (1 - kb)) * lc * one));
    c.bb = int(std::lround(0.5 * lc * one));
    c.rr = int(std::lround(0.5 * lc * one));
    c.rg = int(std::lround(-kg / (2 * (1 - kr)) * lc * one));
    c.rb = int(std::lround(-kb / (2 * (1 - kr)) * lc * one));
    c.yOff = int64_t(std::lround((fullRange ? 0 : 16) * scale * one)) + (int64_t(1) << (Coeffs::kS - 1));
    c.cOff = int64_t(std::lround(128 * scale * one)) + (int64_t(1) << (Coeffs::kS - 1));
    return c;
}

template <typename T>
void put(uint8_t* row, int x, int v, int max, int shift)
{
    reinterpret_cast<T*>(row)[x] = T(std::clamp(v, 0, max) << shift);
}

// One pass over rows [y0, y1) for planar / semi-planar YUV of 8 or 16-bit words. sx, sy: chroma subsampling (1 or 2)
template <typename T>
void yuvRows(const uint8_t* rgba, int width, int stride, const Target& t, const Coeffs& c, int sx, int sy, int y0, int y1)
{
    const int max = (1 << t.bits) - 1, shift = t.msb ? 16 - t.bits : 0;
    const bool nv = t.layout == Layout::Nv12;
    for (int y = y0; y < y1; ++y) {
        const uint8_t* s = rgba + size_t(y) * stride;
        uint8_t* yr = t.planes[0] + size_t(y) * t.strides[0];
        for (int x = 0; x < width; ++x, s += 4)
            put<T>(yr, x, int((c.yr * s[0] + c.yg * s[1] + c.yb * s[2] + c.yOff) >> Coeffs::kS), max, shift);
        if (y % sy) continue;
        const uint8_t* a = rgba + size_t(y) * stride;
        const uint8_t* b = sy == 2 ? rgba + size_t(std::min(y + 1, y1 - 1)) * stride : a;
        const int cy = y / sy, n = sx * sy;
        uint8_t* cb = t.planes[1] + size_t(cy) * t.strides[1];
        uint8_t* cr = nv ? nullptr : t.planes[2] + size_t(cy) * t.strides[2];
        const int64_t off = c.cOff * n; // offset and rounding term, times the n pixels summed
        for (int x = 0, cx = 0; x < width; x += sx, ++cx) {
            int r = 0, g = 0, bl = 0;
            for (int dx = 0; dx < sx; ++dx) {
                const uint8_t* p = a + 4 * (x + dx);
                const uint8_t* q = b + 4 * (x + dx);
                r += p[0];
                g += p[1];
                bl += p[2];
                if (sy == 2) r += q[0], g += q[1], bl += q[2];
            }
            const int vb = int((c.br * int64_t(r) + c.bg * int64_t(g) + c.bb * int64_t(bl) + off) / n >> Coeffs::kS);
            const int vr = int((c.rr * int64_t(r) + c.rg * int64_t(g) + c.rb * int64_t(bl) + off) / n >> Coeffs::kS);
            if (nv) {
                put<T>(cb, 2 * cx, vb, max, shift);
                put<T>(cb, 2 * cx + 1, vr, max, shift);
            } else {
                put<T>(cb, cx, vb, max, shift);
                put<T>(cr, cx, vr, max, shift);
            }
        }
    }
}

} // namespace

void fromRgba(const uint8_t* rgba, int width, int height, int stride, const Target& t, int rowBegin, int rowEnd)
{
    const int y0 = std::clamp(rowBegin, 0, height), y1 = rowEnd < 0 ? height : std::clamp(rowEnd, y0, height);
    if (t.layout == Layout::Rgb24 || t.layout == Layout::Rgba) {
        const bool alpha = t.layout == Layout::Rgba;
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = rgba + size_t(y) * stride;
            uint8_t* d = t.planes[0] + size_t(y) * t.strides[0];
            if (alpha) {
                std::memcpy(d, s, size_t(width) * 4);
                continue;
            }
            for (int x = 0; x < width; ++x, s += 4, d += 3) d[0] = s[0], d[1] = s[1], d[2] = s[2];
        }
        return;
    }
    const Coeffs c = coeffs(t.colorspace, t.bits, t.fullRange);
    const int sx = t.layout == Layout::Yuv444 ? 1 : 2;
    const int sy = t.layout == Layout::Yuv420 || t.layout == Layout::Nv12 ? 2 : 1;
    if (t.bits > 8) yuvRows<uint16_t>(rgba, width, stride, t, c, sx, sy, y0, y1);
    else yuvRows<uint8_t>(rgba, width, stride, t, c, sx, sy, y0, y1);
}

} // namespace OutputColor
