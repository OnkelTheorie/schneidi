#include "engine/OutputColor.h"

#include <framework/mlt.h>
#include <Mlt.h>

#include <algorithm>
#include <cmath>

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

// Planar, chroma subsampled by sx x sy (1 or 2)
template <typename T>
void planar(const Matrix& m, const T* src, T* dst, int width, int height, int sx, int sy, int bits)
{
    const int mid = 1 << (bits - 1), max = (1 << bits) - 1;
    const int cw = width / sx, ch = height / sy;
    const T* sy0 = src;
    const T* scb = src + size_t(width) * height;
    const T* scr = scb + size_t(cw) * ch;
    T* dy0 = dst;
    T* dcb = dst + size_t(width) * height;
    T* dcr = dcb + size_t(cw) * ch;
    for (int cy = 0; cy < ch; ++cy) {
        for (int cx = 0; cx < cw; ++cx) {
            const size_t ci = size_t(cy) * cw + cx;
            int dy;
            chroma<T>(m, int(scb[ci]) - mid, int(scr[ci]) - mid, mid, max, dy, dcb[ci], dcr[ci]);
            for (int j = 0; j < sy; ++j)
                for (int i = 0; i < sx; ++i) {
                    const size_t li = size_t(cy * sy + j) * width + cx * sx + i;
                    dy0[li] = clampTo<T>(int(sy0[li]) + dy, max);
                }
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
    // Full range sources stay with the consumer (it also knows the target range)
    if (from <= 0 || from == to || mlt_properties_get_int(fp, "full_range")) return 0;
    const int size = mlt_image_format_size(*format, *width, *height, nullptr);
    if (size <= 0) return 0;
    // Never in place: the image may be cached by the producer (repeated frames would be converted twice)
    auto* out = static_cast<uint8_t*>(mlt_pool_alloc(size));
    if (!out) return 0;
    if (!convert(*image, out, *format, *width, *height, from, to)) {
        mlt_pool_release(out);
        return 0;
    }
    mlt_frame_set_image(frame, out, size, mlt_pool_release);
    *image = out;
    mlt_properties_set_int(fp, "colorspace", to);
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
    case mlt_image_yuv420p: planar<uint8_t>(m, src, dst, width, height, 2, 2, 8); return true;
    case mlt_image_yuv420p10:
        planar<uint16_t>(m, reinterpret_cast<const uint16_t*>(src), reinterpret_cast<uint16_t*>(dst), width, height, 2,
                         2, 10);
        return true;
    case mlt_image_yuv444p10:
        planar<uint16_t>(m, reinterpret_cast<const uint16_t*>(src), reinterpret_cast<uint16_t*>(dst), width, height, 1,
                         1, 10);
        return true;
    default: return false;
    }
}

} // namespace OutputColor
