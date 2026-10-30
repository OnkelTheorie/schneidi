// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/OutputColor.h"

#include <framework/mlt.h>
#include <Mlt.h>

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

// Y'CbCr(from) -> R'G'B' -> Y'CbCr(to) as one matrix on (Y, Cb, Cr) in code values. Luma stays luma plus a share of
// the chroma; chroma only depends on chroma (both standards share Y = R = G = B for gray).
struct Matrix {
    static constexpr int kShift = 14;
    int yCb, yCr; // added to Y: (yCb * (Cb - mid) + yCr * (Cr - mid)) >> kShift
    int cbCb, cbCr, crCb, crCr;
};

bool matrix(int from, int to, Matrix& m)
{
    double kr1, kb1, kr2, kb2;
    if (!weights(from, kr1, kb1) || !weights(to, kr2, kb2)) return false;
    const double kg1 = 1 - kr1 - kb1, kg2 = 1 - kr2 - kb2;
    // Source: R = Y + r_pr*Pr, G = Y + g_pb*Pb + g_pr*Pr, B = Y + b_pb*Pb (Y 0..1, Pb/Pr -0.5..0.5)
    const double rPr = 2 * (1 - kr1), bPb = 2 * (1 - kb1);
    const double gPb = -2 * kb1 * (1 - kb1) / kg1, gPr = -2 * kr1 * (1 - kr1) / kg1;
    // Target luma: Y' = Y + yPb*Pb + yPr*Pr
    const double yPb = kg2 * gPb + kb2 * bPb, yPr = kr2 * rPr + kg2 * gPr;
    // Pb' = (B - Y') / (2(1 - Kb')), Pr' = (R - Y') / (2(1 - Kr'))
    const double pbPb = (bPb - yPb) / (2 * (1 - kb2)), pbPr = -yPr / (2 * (1 - kb2));
    const double prPb = -yPb / (2 * (1 - kr2)), prPr = (rPr - yPr) / (2 * (1 - kr2));
    // Code values: luma steps 219, chroma steps 224 (times 4 for 10 bit, the ratio is the same)
    const double one = 1 << Matrix::kShift, lumaPerChroma = 219.0 / 224.0;
    m.yCb = int(std::lround(yPb * lumaPerChroma * one));
    m.yCr = int(std::lround(yPr * lumaPerChroma * one));
    m.cbCb = int(std::lround(pbPb * one));
    m.cbCr = int(std::lround(pbPr * one));
    m.crCb = int(std::lround(prPb * one));
    m.crCr = int(std::lround(prPr * one));
    return true;
}

constexpr int kRound = 1 << (Matrix::kShift - 1);

template <typename T>
inline T clampTo(int v, int max)
{
    return T(std::clamp(v, 0, max));
}

// One chroma sample (cb, cr around mid) -> luma offset and new chroma
template <typename T>
inline void chroma(const Matrix& m, int cb, int cr, int mid, int max, int& dy, T& outCb, T& outCr)
{
    dy = (m.yCb * cb + m.yCr * cr + kRound) >> Matrix::kShift;
    outCb = clampTo<T>(mid + ((m.cbCb * cb + m.cbCr * cr + kRound) >> Matrix::kShift), max);
    outCr = clampTo<T>(mid + ((m.crCb * cb + m.crCr * cr + kRound) >> Matrix::kShift), max);
}

void packed422(const Matrix& m, const uint8_t* src, uint8_t* dst, int width, int height)
{
    const int pairs = width / 2 * height; // Y0 Cb Y1 Cr
    for (int i = 0; i < pairs; ++i, src += 4, dst += 4) {
        int dy;
        chroma<uint8_t>(m, src[1] - 128, src[3] - 128, 128, 255, dy, dst[1], dst[3]);
        dst[0] = clampTo<uint8_t>(src[0] + dy, 255);
        dst[2] = clampTo<uint8_t>(src[2] + dy, 255);
    }
}

// One row of chroma samples: new Cb/Cr, and the luma offset for each of the S luma columns of a sample. __restrict
// and plain loops so the compiler uses SIMD (this file is built with full vectorization, see CMakeLists.txt).
template <typename T, int S>
void chromaRow(const Matrix& m, const T* __restrict cbIn, const T* __restrict crIn, T* __restrict cbOut,
               T* __restrict crOut, int16_t* __restrict dy, int n, int mid, int max)
{
    const int yCb = m.yCb, yCr = m.yCr, cbCb = m.cbCb, cbCr = m.cbCr, crCb = m.crCb, crCr = m.crCr;
    for (int i = 0; i < n; ++i) {
        const int cb = int(cbIn[i]) - mid, cr = int(crIn[i]) - mid;
        const int d = (yCb * cb + yCr * cr + kRound) >> Matrix::kShift;
        for (int k = 0; k < S; ++k) dy[i * S + k] = int16_t(d);
        cbOut[i] = clampTo<T>(mid + ((cbCb * cb + cbCr * cr + kRound) >> Matrix::kShift), max);
        crOut[i] = clampTo<T>(mid + ((crCb * cb + crCr * cr + kRound) >> Matrix::kShift), max);
    }
}

// One luma row with the offsets of its chroma samples; 16 bit is enough (10 bit code values plus a small offset) and
// lets plain SSE2 clamp 8 samples at once
template <typename T>
void lumaRow(const T* __restrict in, T* __restrict out, const int16_t* __restrict dy, int width, int16_t max)
{
    for (int x = 0; x < width; ++x) out[x] = T(std::clamp<int16_t>(int16_t(in[x] + dy[x]), 0, max));
}

// Planar, chroma subsampled by S x S (1 or 2), row by row (per-sample loops took ~3 ms per 1080p frame; export runs
// this on every frame of a BT.601 source)
template <typename T, int S>
void planar(const Matrix& m, const T* src, T* dst, int width, int height, int bits)
{
    const int mid = 1 << (bits - 1), max = (1 << bits) - 1;
    const int cw = width / S, ch = height / S;
    const T* scb = src + size_t(width) * height;
    const T* scr = scb + size_t(cw) * ch;
    T* dcb = dst + size_t(width) * height;
    T* dcr = dcb + size_t(cw) * ch;
    std::vector<int16_t> dy(static_cast<size_t>(width)); // luma offset per column of the row
    for (int cy = 0; cy < ch; ++cy) {
        const size_t c0 = size_t(cy) * cw;
        chromaRow<T, S>(m, scb + c0, scr + c0, dcb + c0, dcr + c0, dy.data(), cw, mid, max);
        for (int j = 0; j < S; ++j) {
            const size_t l0 = size_t(cy * S + j) * width;
            lumaRow<T>(src + l0, dst + l0, dy.data(), width, int16_t(max));
        }
    }
}

int getImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int error = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (error || !*image) return error;
    mlt_properties fp = MLT_FRAME_PROPERTIES(frame);
    const int from = mlt_properties_get_int(fp, "colorspace");
    const int to = mlt_properties_get_int(MLT_FILTER_PROPERTIES(filter), "colorspace");
    // The export is always limited range (MLT's consumer has no color_range set)
    const bool fullRange = mlt_properties_get_int(fp, "full_range");
    const bool otherMatrix = from > 0 && from != to;
    if (!otherMatrix && !fullRange) return 0;
    const int size = mlt_image_format_size(*format, *width, *height, nullptr);
    if (size <= 0) return 0;
    // Never in place: the image may be cached by the producer (repeated frames would be converted twice)
    auto* out = static_cast<uint8_t*>(mlt_pool_alloc(size));
    if (!out) return 0;
    // Range first (into out), then the matrix (from a copy of it); either one alone writes out directly
    bool ranged = false, converted = false;
    if (fullRange) {
        std::memcpy(out, *image, size_t(size));
        ranged = toLimitedRange(out, *format, *width, *height);
    }
    if (otherMatrix) {
        if (ranged) {
            std::vector<uint8_t> limited(out, out + size);
            converted = convert(limited.data(), out, *format, *width, *height, from, to);
            if (!converted) std::memcpy(out, limited.data(), size_t(size)); // matrix left to the consumer
        } else {
            converted = convert(*image, out, *format, *width, *height, from, to);
        }
    }
    if (!ranged && !converted) { // format not handled: leave it to the consumer
        mlt_pool_release(out);
        return 0;
    }
    mlt_frame_set_image(frame, out, size, mlt_pool_release);
    *image = out;
    if (converted) mlt_properties_set_int(fp, "colorspace", to);
    if (ranged) mlt_properties_set_int(fp, "full_range", 0);
    return 0;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, getImage);
    return frame;
}

} // namespace

std::unique_ptr<Mlt::Filter> make(Mlt::Profile& profile)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    f->process = process;
    mlt_properties_set_int(MLT_FILTER_PROPERTIES(f), "colorspace", profile.colorspace());
    auto filter = std::make_unique<Mlt::Filter>(f); // holds its own reference
    mlt_filter_close(f);
    return filter;
}

bool convert(const uint8_t* src, uint8_t* dst, mlt_image_format format, int width, int height, int fromColorspace,
             int toColorspace)
{
    Matrix m;
    if (width <= 0 || height <= 0 || width % 2 || height % 2 || !matrix(fromColorspace, toColorspace, m)) return false;
    switch (format) {
    case mlt_image_yuv422: packed422(m, src, dst, width, height); return true;
    case mlt_image_yuv420p: planar<uint8_t, 2>(m, src, dst, width, height, 8); return true;
    case mlt_image_yuv420p10:
        planar<uint16_t, 2>(m, reinterpret_cast<const uint16_t*>(src), reinterpret_cast<uint16_t*>(dst), width, height, 10);
        return true;
    case mlt_image_yuv444p10:
        planar<uint16_t, 1>(m, reinterpret_cast<const uint16_t*>(src), reinterpret_cast<uint16_t*>(dst), width, height, 10);
        return true;
    default: return false;
    }
}

namespace {

// Lookup tables full -> limited range for one bit depth (luma, chroma)
struct RangeTables {
    std::vector<uint16_t> luma, chroma;
    explicit RangeTables(int bits)
    {
        const int n = 1 << bits, max = n - 1, scale = 1 << (bits - 8), mid = n / 2;
        luma.resize(n);
        chroma.resize(n);
        for (int v = 0; v < n; ++v) {
            luma[v] = uint16_t(16 * scale + std::lround(v * (219.0 * scale) / max));
            chroma[v] = uint16_t(std::clamp<long>(mid + std::lround((v - mid) * (224.0 * scale) / max), 16 * scale, 240 * scale));
        }
    }
};

template <typename T>
void mapPlane(T* p, size_t n, const std::vector<uint16_t>& table)
{
    for (size_t i = 0; i < n; ++i) p[i] = T(table[p[i]]);
}

} // namespace

bool toLimitedRange(uint8_t* image, mlt_image_format format, int width, int height)
{
    if (width <= 0 || height <= 0 || width % 2 || height % 2) return false;
    static const RangeTables eight(8), ten(10);
    const size_t luma = size_t(width) * height;
    switch (format) {
    case mlt_image_yuv422:
        for (size_t i = 0; i < luma * 2; i += 2) {
            image[i] = uint8_t(eight.luma[image[i]]);
            image[i + 1] = uint8_t(eight.chroma[image[i + 1]]);
        }
        return true;
    case mlt_image_yuv420p:
        mapPlane(image, luma, eight.luma);
        mapPlane(image + luma, luma / 2, eight.chroma);
        return true;
    case mlt_image_yuv420p10:
    case mlt_image_yuv444p10: {
        auto* p = reinterpret_cast<uint16_t*>(image);
        const size_t chroma = format == mlt_image_yuv420p10 ? luma / 2 : luma * 2;
        for (size_t i = 0; i < luma + chroma; ++i) p[i] = uint16_t(std::min<int>(p[i], 1023)); // stray bits
        mapPlane(p, luma, ten.luma);
        mapPlane(p + luma, chroma, ten.chroma);
        return true;
    }
    default: return false;
    }
}

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
