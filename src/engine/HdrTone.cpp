// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/HdrTone.h"
#include "engine/video/Compose.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

namespace HdrTone {

namespace {

// AVColorTransferCharacteristic numbers (libavutil/pixfmt.h; fixed by ITU-T H.273)
constexpr int kTrcSmpte2084 = 16;
constexpr int kTrcAribStdB67 = 18;

// HLG (BT.2100)
constexpr double kHlgA = 0.17883277, kHlgB = 0.28466892, kHlgC = 0.55991073;
constexpr double kHlgGamma = 1.2; // system gamma for a 1000-nit display
// PQ (ST 2084)
constexpr double kPqM1 = 2610.0 / 16384, kPqM2 = 2523.0 / 4096 * 128;
constexpr double kPqC1 = 3424.0 / 4096, kPqC2 = 2413.0 / 4096 * 32, kPqC3 = 2392.0 / 4096 * 32;

// Luma weights in linear light
constexpr double k2020[3] = {0.2627, 0.6780, 0.0593};
constexpr double k709[3] = {0.2126, 0.7152, 0.0722};

// BT.2020 -> BT.709 primaries, linear light (rows: R, G, B of 709)
constexpr double kToRec709[3][3] = {{1.660491, -0.587641, -0.072850},
                                    {-0.124550, 1.132900, -0.008349},
                                    {-0.018151, -0.100579, 1.118730}};

// Tone curve: light scaled by kGain (reference white -> SDR linear 0.57, code ~80 % as an HLG picture shown as it is:
// shadows and mid-tones then match it, the top keeps room for the sky), linear up to the knee, above it a rational
// shoulder that meets the line with the same slope and reaches SDR white at the peak. (Reference white at ~92 % with
// a knee at 0.6 looked washed out: bright, a blue sky pressed to white.)
constexpr double kGain = 0.6;
constexpr double kKnee = 0.45;
constexpr double kPeakRel = kPeak / kReferenceWhite * kGain;
constexpr double kSlope = (kPeakRel - kKnee) / (1 - kKnee); // shoulder's slope at its start in its own unit
constexpr double kDisplayGamma = 2.4;                        // BT.1886

double hlgInverseOetf(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.5 ? v * v / 3 : (std::exp((v - kHlgC) / kHlgA) + kHlgB) / 12;
}

// PQ code value -> nits
double pqEotf(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    const double p = std::pow(v, 1 / kPqM2);
    return 10000 * std::pow(std::max(p - kPqC1, 0.0) / (kPqC2 - kPqC3 * p), 1 / kPqM1);
}

double curve(double y)
{
    if (y <= kKnee) return y;
    const double x = (y - kKnee) / (kPeakRel - kKnee);
    return kKnee + (1 - kKnee) * (x * kSlope / (1 + (kSlope - 1) * x));
}

// Relative display light (1 = reference white, source primaries) -> tone mapped linear BT.709 0..1
// T = double (the exact function) or float (the per-pixel way: enough for 8-bit results, twice as fast)
template <typename T>
void mapLinear(const Tone& tone, T rgb[3])
{
    if (tone.bt2020) {
        const T r = rgb[0], g = rgb[1], b = rgb[2];
        for (int i = 0; i < 3; ++i) rgb[i] = T(kToRec709[i][0]) * r + T(kToRec709[i][1]) * g + T(kToRec709[i][2]) * b;
    }
    for (int i = 0; i < 3; ++i) rgb[i] *= T(kGain);
    const T y = T(k709[0]) * rgb[0] + T(k709[1]) * rgb[1] + T(k709[2]) * rgb[2];
    if (y <= 0) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    const T mapped = T(curve(y));
    const T k = mapped / y;
    for (int i = 0; i < 3; ++i) rgb[i] *= k;
    if (mapped >= 1) {
        rgb[0] = rgb[1] = rgb[2] = 1;
        return;
    }
    // Out of the BT.709 gamut (saturated BT.2020 colours below zero, bright colours above one): towards the
    // luminance, which stays
    const T lo = std::min({rgb[0], rgb[1], rgb[2]});
    if (lo < 0) {
        const T t = mapped / (mapped - lo);
        for (int i = 0; i < 3; ++i) rgb[i] = mapped + (rgb[i] - mapped) * t;
    }
    const T hi = std::max({rgb[0], rgb[1], rgb[2]});
    if (hi > 1) {
        const T t = (1 - mapped) / (hi - mapped);
        for (int i = 0; i < 3; ++i) rgb[i] = mapped + (rgb[i] - mapped) * t;
    }
    for (int i = 0; i < 3; ++i) rgb[i] = std::clamp(rgb[i], T(0), T(1));
}

// Signal -> relative display light, the exact way
void decode(const Tone& tone, const float in[3], double rgb[3])
{
    if (tone.transfer == Transfer::Pq) {
        for (int i = 0; i < 3; ++i) rgb[i] = pqEotf(in[i]) / kReferenceWhite;
        return;
    }
    double e[3];
    for (int i = 0; i < 3; ++i) e[i] = hlgInverseOetf(in[i]);
    const double* w = tone.bt2020 ? k2020 : k709;
    const double ys = w[0] * e[0] + w[1] * e[1] + w[2] * e[2];
    const double k = ys > 0 ? kPeak * std::pow(ys, kHlgGamma - 1) / kReferenceWhite : 0.0;
    for (int i = 0; i < 3; ++i) rgb[i] = e[i] * k;
}

// Tables for the per-pixel way
constexpr int kRootSteps = 4096; // tables over sqrt(x) for x in 0..1 (fine near black)

struct Tables {
    std::vector<float> decode16; // 65536: PQ -> relative light, HLG -> scene light E
    std::array<float, kRootSteps + 1> hlgOotf{}; // sqrt(Ys) -> peak/white * Ys^(gamma-1)
    std::array<float, kRootSteps + 1> encode{};  // sqrt(linear) -> code value 0..1 (BT.1886 inverse)
};

float lookupRoot(const std::array<float, kRootSteps + 1>& t, float x)
{
    const float u = std::sqrt(std::clamp(x, 0.0f, 1.0f)) * kRootSteps;
    const int i = std::min(int(u), kRootSteps - 1);
    const float f = u - float(i);
    return t[i] + (t[i + 1] - t[i]) * f;
}

const Tables& tables(Transfer t)
{
    static std::once_flag once[2];
    static std::unique_ptr<Tables> made[2];
    const int k = t == Transfer::Pq ? 1 : 0;
    std::call_once(once[k], [&] {
        auto tb = std::make_unique<Tables>();
        tb->decode16.resize(65536);
        for (int v = 0; v < 65536; ++v) {
            const double s = v / 65535.0;
            tb->decode16[v] = float(k == 1 ? pqEotf(s) / kReferenceWhite : hlgInverseOetf(s));
        }
        for (int i = 0; i <= kRootSteps; ++i) {
            const double u = double(i) / kRootSteps, x = u * u;
            tb->hlgOotf[i] = float(x > 0 ? kPeak * std::pow(x, kHlgGamma - 1) / kReferenceWhite : 0.0);
            tb->encode[i] = float(std::pow(x, 1 / kDisplayGamma));
        }
        made[k] = std::move(tb);
    });
    return *made[k];
}

// One pixel through the tables: signal as 16-bit codes -> 8-bit SDR
inline void pixel(const Tone& tone, const Tables& tb, const uint16_t s[3], uint8_t o[3])
{
    float rgb[3] = {tb.decode16[s[0]], tb.decode16[s[1]], tb.decode16[s[2]]};
    if (tone.transfer == Transfer::Hlg) {
        const double* w = tone.bt2020 ? k2020 : k709;
        const float ys = float(w[0]) * rgb[0] + float(w[1]) * rgb[1] + float(w[2]) * rgb[2];
        const float k = lookupRoot(tb.hlgOotf, ys);
        for (float& c : rgb) c *= k;
    }
    mapLinear(tone, rgb);
    for (int i = 0; i < 3; ++i) o[i] = uint8_t(lookupRoot(tb.encode, rgb[i]) * 255 + 0.5f);
}

int threadRows(int width, int height, int threads)
{
    if (threads == 1) return height;
    const int rows = std::max(1, 65536 / std::max(1, width)); // ~64k pixels per job at least
    if (threads > 1) return std::max(rows, (height + threads - 1) / threads);
    return rows;
}

} // namespace

Transfer ofColorTrc(int colorTrc)
{
    if (colorTrc == kTrcAribStdB67) return Transfer::Hlg;
    if (colorTrc == kTrcSmpte2084) return Transfer::Pq;
    return Transfer::None;
}

const char* name(Transfer t)
{
    switch (t) {
    case Transfer::Hlg: return "HLG";
    case Transfer::Pq: return "PQ";
    default: return "";
    }
}

void toSdr(const Tone& tone, const float in[3], float out[3])
{
    if (!tone.active()) {
        for (int i = 0; i < 3; ++i) out[i] = in[i];
        return;
    }
    double rgb[3];
    decode(tone, in, rgb);
    mapLinear(tone, rgb);
    for (int i = 0; i < 3; ++i) out[i] = float(std::pow(rgb[i], 1 / kDisplayGamma));
}

void toSdrRgba64(const Tone& tone, const uint16_t* in, int inStride, uint8_t* out, int outStride, int width, int height,
                 int threads)
{
    if (!tone.active() || width <= 0 || height <= 0) return;
    const Tables& tb = tables(tone.transfer);
    Compose::forRows(height, threadRows(width, height, threads), [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(in) + size_t(y) * inStride);
            uint8_t* o = out + size_t(y) * outStride;
            for (int x = 0; x < width; ++x, s += 4, o += 4) {
                pixel(tone, tb, s, o);
                o[3] = uint8_t((s[3] + 128) / 257);
            }
        }
    });
}

void toSdrRgba8(const Tone& tone, uint8_t* rgba, int width, int height, int threads, bool bgra, int stride)
{
    if (!tone.active() || width <= 0 || height <= 0) return;
    const Tables& tb = tables(tone.transfer);
    if (stride <= 0) stride = width * 4;
    const int r = bgra ? 2 : 0, b = bgra ? 0 : 2;
    Compose::forRows(height, threadRows(width, height, threads), [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            uint8_t* p = rgba + size_t(y) * stride;
            for (int x = 0; x < width; ++x, p += 4) {
                const uint16_t s[3] = {uint16_t(p[r] * 257), uint16_t(p[1] * 257), uint16_t(p[b] * 257)};
                uint8_t o[3];
                pixel(tone, tb, s, o);
                p[r] = o[0], p[1] = o[1], p[b] = o[2];
            }
        }
    });
}

Tone ofFrame(int colorTrc, int colorPrimaries, int colorSpace)
{
    // AVCOL_PRI_BT2020 = 9, AVCOL_PRI_UNSPECIFIED = 2, AVCOL_SPC_BT709 = 1: unstated primaries are BT.2020 unless BT.709 is
    return {ofColorTrc(colorTrc), colorPrimaries == 9 || (colorPrimaries == 2 && colorSpace != 1)};
}

} // namespace HdrTone
