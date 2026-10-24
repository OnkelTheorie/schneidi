#include "engine/OutputColor.h"

#include <framework/mlt.h>
#include <Mlt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

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

} // namespace OutputColor
