// Shared distortion kernels of the H.264 encoder: SAD rows and 4x4 Hadamard SATD, SSE2 with a plain
// C++ fallback that gives the same numbers (the bitstream must not depend on the platform).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <initializer_list>

#include "Simd.h"

namespace schneidi::h264 {

inline void hadamard4x4(const int* in, int* out)
{
    int t[16];
    for (int y = 0; y < 4; ++y) {
        const int* r = in + y * 4;
        const int a = r[0] + r[1], b = r[0] - r[1], c = r[2] + r[3], d = r[2] - r[3];
        t[y * 4 + 0] = a + c;
        t[y * 4 + 1] = a - c;
        t[y * 4 + 2] = b - d;
        t[y * 4 + 3] = b + d;
    }
    for (int x = 0; x < 4; ++x) {
        const int a = t[x] + t[4 + x], b = t[x] - t[4 + x], c = t[8 + x] + t[12 + x], d = t[8 + x] - t[12 + x];
        out[x] = a + c;
        out[4 + x] = a - c;
        out[8 + x] = b - d;
        out[12 + x] = b + d;
    }
}

inline int satd4x4(const int* diff)
{
    int t[16];
    hadamard4x4(diff, t);
    int s = 0;
    for (int v : t)
        s += std::abs(v);
    return (s + 1) >> 1;
}

#ifdef SCHNEIDI_H264_NEON
namespace neon_detail {

inline int16x8_t diff8(const uint8_t* a, const uint8_t* b)
{
    return vreinterpretq_s16_u16(vsubl_u8(vld1_u8(a), vld1_u8(b)));
}
inline uint8x8_t load4(const uint8_t* p) // 4 samples in the low half, zeros above
{
    uint32_t w;
    std::memcpy(&w, p, 4);
    return vcreate_u8(uint64_t(w));
}
inline int16x8_t diff4(const uint8_t* a, const uint8_t* b)
{
    return vreinterpretq_s16_u16(vsubl_u8(load4(a), load4(b)));
}

// Two 4x4 blocks side by side (lanes 0-3 and 4-7 of four rows of differences): per lane the
// halved |coefficient| sums of the 4x4 Hadamard transforms. The last butterfly stage is not done:
// |x + y| + |x - y| = 2 max(|x|, |y|), so each block's SATD ((sum + 1) >> 1 of its 16 coefficient
// magnitudes, an even sum) is exactly the sum of its lanes here.
inline uint16x8_t satdLanes(int16x8_t r0, int16x8_t r1, int16x8_t r2, int16x8_t r3)
{
    const int16x8_t s01 = vaddq_s16(r0, r1), d01 = vsubq_s16(r0, r1);
    const int16x8_t s23 = vaddq_s16(r2, r3), d23 = vsubq_s16(r2, r3);
    const int16x8_t t0 = vaddq_s16(s01, s23), t1 = vsubq_s16(s01, s23);
    const int16x8_t t2 = vsubq_s16(d01, d23), t3 = vaddq_s16(d01, d23);
    // Horizontal, columns (0, 1) and (2, 3) of each block.
    const int16x8_t a0 = vtrn1q_s16(t0, t1), b0 = vtrn2q_s16(t0, t1);
    const int16x8_t a1 = vtrn1q_s16(t2, t3), b1 = vtrn2q_s16(t2, t3);
    const int16x8_t h0 = vaddq_s16(a0, b0), h1 = vsubq_s16(a0, b0);
    const int16x8_t h2 = vaddq_s16(a1, b1), h3 = vsubq_s16(a1, b1);
    // Columns 0+1 against 2+3: the 32-bit lanes of h hold (0+1 | 2+3) pairs.
    const int16x8_t u0 = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(h0), vreinterpretq_s32_s16(h1)));
    const int16x8_t u1 = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(h0), vreinterpretq_s32_s16(h1)));
    const int16x8_t u2 = vreinterpretq_s16_s32(vtrn1q_s32(vreinterpretq_s32_s16(h2), vreinterpretq_s32_s16(h3)));
    const int16x8_t u3 = vreinterpretq_s16_s32(vtrn2q_s32(vreinterpretq_s32_s16(h2), vreinterpretq_s32_s16(h3)));
    // |u| <= 4 * 2 * 255: the sum of two maxima fits 16 bits.
    return vaddq_u16(vmaxq_u16(vreinterpretq_u16_s16(vabsq_s16(u0)), vreinterpretq_u16_s16(vabsq_s16(u1))),
                     vmaxq_u16(vreinterpretq_u16_s16(vabsq_s16(u2)), vreinterpretq_u16_s16(vabsq_s16(u3))));
}

inline uint16x8_t satd8x4Lanes(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
    return satdLanes(diff8(a, b), diff8(a + strideA, b + strideB), diff8(a + 2 * strideA, b + 2 * strideB),
                     diff8(a + 3 * strideA, b + 3 * strideB));
}

// SATD of a w x h block (w and h multiples of 4), the sum of its 4x4 blocks' SATDs.
inline int satdBlock(const uint8_t* a, int strideA, const uint8_t* b, int strideB, int w, int h)
{
    uint32x4_t acc = vdupq_n_u32(0);
    for (int y = 0; y < h; y += 4) {
        const uint8_t* pa = a + y * strideA;
        const uint8_t* pb = b + y * strideB;
        int x = 0;
        for (; x + 8 <= w; x += 8)
            acc = vpadalq_u16(acc, satd8x4Lanes(pa + x, strideA, pb + x, strideB));
        if (x < w)
            acc = vpadalq_u16(acc, satdLanes(diff4(pa + x, pb + x), diff4(pa + strideA + x, pb + strideB + x),
                                             diff4(pa + 2 * strideA + x, pb + 2 * strideB + x),
                                             diff4(pa + 3 * strideA + x, pb + 3 * strideB + x)));
    }
    return int(vaddvq_u32(acc));
}

} // namespace neon_detail
#endif

// SATD of a 4x4 block between two 8-bit images; same result as satd4x4 on their difference.
inline int satdU8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#ifdef SCHNEIDI_H264_SSE2
    const __m128i zero = _mm_setzero_si128();
    auto row = [&](int i) {
        int32_t wa, wb;
        std::memcpy(&wa, a + i * strideA, 4);
        std::memcpy(&wb, b + i * strideB, 4);
        return _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(wa), zero), _mm_unpacklo_epi8(_mm_cvtsi32_si128(wb), zero));
    };
    const __m128i r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    // Vertical 4-point Hadamard (lanes = columns).
    const __m128i s01 = _mm_add_epi16(r0, r1), d01 = _mm_sub_epi16(r0, r1);
    const __m128i s23 = _mm_add_epi16(r2, r3), d23 = _mm_sub_epi16(r2, r3);
    const __m128i t0 = _mm_add_epi16(s01, s23), t1 = _mm_sub_epi16(s01, s23);
    const __m128i t2 = _mm_sub_epi16(d01, d23), t3 = _mm_add_epi16(d01, d23);
    // Transpose: v0 = columns 0|1, v1 = columns 2|3.
    const __m128i u0 = _mm_unpacklo_epi16(t0, t1), u1 = _mm_unpacklo_epi16(t2, t3);
    const __m128i v0 = _mm_unpacklo_epi32(u0, u1), v1 = _mm_unpackhi_epi32(u0, u1);
    const __m128i c1 = _mm_unpackhi_epi64(v0, v0), c3 = _mm_unpackhi_epi64(v1, v1);
    // Horizontal 4-point Hadamard on the columns.
    const __m128i ha = _mm_add_epi16(v0, c1), hb = _mm_sub_epi16(v0, c1);
    const __m128i hc = _mm_add_epi16(v1, c3), hd = _mm_sub_epi16(v1, c3);
    const __m128i o01 = _mm_unpacklo_epi64(_mm_add_epi16(ha, hc), _mm_sub_epi16(ha, hc));
    const __m128i o23 = _mm_unpacklo_epi64(_mm_sub_epi16(hb, hd), _mm_add_epi16(hb, hd));
    auto absv = [&](__m128i x) { return _mm_max_epi16(x, _mm_sub_epi16(zero, x)); };
    const __m128i sum = _mm_madd_epi16(_mm_add_epi16(absv(o01), absv(o23)), _mm_set1_epi16(1));
    const __m128i s2 = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2)));
    const __m128i s1 = _mm_add_epi32(s2, _mm_shuffle_epi32(s2, _MM_SHUFFLE(2, 3, 0, 1)));
    return (_mm_cvtsi128_si32(s1) + 1) >> 1;
#elif defined(SCHNEIDI_H264_NEON)
    using namespace neon_detail;
    return int(vaddvq_u16(satdLanes(diff4(a, b), diff4(a + strideA, b + strideB), diff4(a + 2 * strideA, b + 2 * strideB),
                                    diff4(a + 3 * strideA, b + 3 * strideB))));
#else
    int diff[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            diff[y * 4 + x] = int(a[y * strideA + x]) - int(b[y * strideB + x]);
    return satd4x4(diff);
#endif
}

// satdU8 of the two 4x4 blocks side by side at a and a + 4 (same result as two calls), sharing
// the vertical transform across eight lanes.
inline int satd8x4U8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#ifdef SCHNEIDI_H264_SSE2
    const __m128i zero = _mm_setzero_si128();
    auto row = [&](int i) {
        return _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(a + i * strideA)), zero),
                             _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(b + i * strideB)), zero));
    };
    const __m128i r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const __m128i s01 = _mm_add_epi16(r0, r1), d01 = _mm_sub_epi16(r0, r1);
    const __m128i s23 = _mm_add_epi16(r2, r3), d23 = _mm_sub_epi16(r2, r3);
    const __m128i t0 = _mm_add_epi16(s01, s23), t1 = _mm_sub_epi16(s01, s23);
    const __m128i t2 = _mm_sub_epi16(d01, d23), t3 = _mm_add_epi16(d01, d23);
    auto absv = [&](__m128i x) { return _mm_max_epi16(x, _mm_sub_epi16(zero, x)); };
    auto half = [&](__m128i u0, __m128i u1) { // horizontal stage of one block, returns its |coefficient| sum
        const __m128i v0 = _mm_unpacklo_epi32(u0, u1), v1 = _mm_unpackhi_epi32(u0, u1);
        const __m128i c1 = _mm_unpackhi_epi64(v0, v0), c3 = _mm_unpackhi_epi64(v1, v1);
        const __m128i ha = _mm_add_epi16(v0, c1), hb = _mm_sub_epi16(v0, c1);
        const __m128i hc = _mm_add_epi16(v1, c3), hd = _mm_sub_epi16(v1, c3);
        const __m128i o01 = _mm_unpacklo_epi64(_mm_add_epi16(ha, hc), _mm_sub_epi16(ha, hc));
        const __m128i o23 = _mm_unpacklo_epi64(_mm_sub_epi16(hb, hd), _mm_add_epi16(hb, hd));
        const __m128i sum = _mm_madd_epi16(_mm_add_epi16(absv(o01), absv(o23)), _mm_set1_epi16(1));
        const __m128i s2 = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m128i s1 = _mm_add_epi32(s2, _mm_shuffle_epi32(s2, _MM_SHUFFLE(2, 3, 0, 1)));
        return (_mm_cvtsi128_si32(s1) + 1) >> 1;
    };
    return half(_mm_unpacklo_epi16(t0, t1), _mm_unpacklo_epi16(t2, t3)) +
           half(_mm_unpackhi_epi16(t0, t1), _mm_unpackhi_epi16(t2, t3));
#elif defined(SCHNEIDI_H264_NEON)
    return int(vaddvq_u16(neon_detail::satd8x4Lanes(a, strideA, b, strideB)));
#else
    return satdU8(a, strideA, b, strideB) + satdU8(a + 4, strideA, b + 4, strideB);
#endif
}

// SATD of an 8x8 block as the sum of its four 4x4 blocks (the measure the mode decisions use).
inline int satd8x8U8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#ifdef SCHNEIDI_H264_NEON
    return int(vaddlvq_u16(vaddq_u16(neon_detail::satd8x4Lanes(a, strideA, b, strideB),
                                     neon_detail::satd8x4Lanes(a + 4 * strideA, strideA, b + 4 * strideB, strideB))));
#endif
    return satd8x4U8(a, strideA, b, strideB) + satd8x4U8(a + 4 * strideA, strideA, b + 4 * strideB, strideB);
}

#ifdef SCHNEIDI_H264_AVX2
// Four 4x4 blocks side by side (16 x 4): the 8x4 kernel's steps on both 128-bit lanes at once
// (lane 0 = columns 0-7, lane 1 = columns 8-15); same result as four satdU8 calls.
// Core: four rows of differences, each lane holding two 4x4 blocks side by side.
// Per-block sums stay in vector lanes: each 4x4 block's |coefficient| sum is rounded ((s + 1) >> 1)
// in its lane and accumulated; one horizontal sum at the end.
__attribute__((target("avx2"))) inline void satdLanesAccAvx2(const __m256i* r, __m256i& acc)
{
    const __m256i s01 = _mm256_add_epi16(r[0], r[1]), d01 = _mm256_sub_epi16(r[0], r[1]);
    const __m256i s23 = _mm256_add_epi16(r[2], r[3]), d23 = _mm256_sub_epi16(r[2], r[3]);
    const __m256i t0 = _mm256_add_epi16(s01, s23), t1 = _mm256_sub_epi16(s01, s23);
    const __m256i t2 = _mm256_sub_epi16(d01, d23), t3 = _mm256_add_epi16(d01, d23);
    const __m256i u[2][2] = {{_mm256_unpacklo_epi16(t0, t1), _mm256_unpacklo_epi16(t2, t3)},
                             {_mm256_unpackhi_epi16(t0, t1), _mm256_unpackhi_epi16(t2, t3)}};
    __m256i sums[2];
    for (int k = 0; k < 2; ++k) {
        const __m256i v0 = _mm256_unpacklo_epi32(u[k][0], u[k][1]), v1 = _mm256_unpackhi_epi32(u[k][0], u[k][1]);
        const __m256i c1 = _mm256_unpackhi_epi64(v0, v0), c3 = _mm256_unpackhi_epi64(v1, v1);
        const __m256i ha = _mm256_add_epi16(v0, c1), hb = _mm256_sub_epi16(v0, c1);
        const __m256i hc = _mm256_add_epi16(v1, c3), hd = _mm256_sub_epi16(v1, c3);
        const __m256i o01 = _mm256_unpacklo_epi64(_mm256_add_epi16(ha, hc), _mm256_sub_epi16(ha, hc));
        const __m256i o23 = _mm256_unpacklo_epi64(_mm256_sub_epi16(hb, hd), _mm256_add_epi16(hb, hd));
        sums[k] = _mm256_madd_epi16(_mm256_add_epi16(_mm256_abs_epi16(o01), _mm256_abs_epi16(o23)), _mm256_set1_epi16(1));
    }
    const __m256i h1 = _mm256_hadd_epi32(sums[0], sums[1]); // per lane: k0 halves, k1 halves
    const __m256i blk = _mm256_hadd_epi32(h1, h1);          // per lane: block k0, block k1 (twice)
    acc = _mm256_add_epi32(acc, _mm256_srli_epi32(_mm256_add_epi32(blk, _mm256_set1_epi32(1)), 1));
}

__attribute__((target("avx2"))) inline int satdLanesAvx2(const __m256i* r)
{
    __m256i acc = _mm256_setzero_si256();
    satdLanesAccAvx2(r, acc);
    const __m128i lanes = _mm_add_epi32(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
    return _mm_cvtsi128_si32(lanes) + _mm_cvtsi128_si32(_mm_srli_si128(lanes, 4));
}

__attribute__((target("avx2"))) inline int satd16x4U8Avx2(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
    __m256i r[4];
    for (int i = 0; i < 4; ++i)
        r[i] = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i * strideA))),
                                _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i * strideB))));
    return satdLanesAvx2(r);
}

// An 8x8 block: rows 0-3 in lane 0, rows 4-7 in lane 1; same result as satd8x8U8.
__attribute__((target("avx2"))) inline int satd8x8U8Avx2(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
    __m256i r[4];
    for (int i = 0; i < 4; ++i) {
        const __m128i ra = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(a + i * strideA)),
                                              _mm_loadl_epi64(reinterpret_cast<const __m128i*>(a + (i + 4) * strideA)));
        const __m128i rb = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(b + i * strideB)),
                                              _mm_loadl_epi64(reinterpret_cast<const __m128i*>(b + (i + 4) * strideB)));
        r[i] = _mm256_sub_epi16(_mm256_cvtepu8_epi16(ra), _mm256_cvtepu8_epi16(rb));
    }
    return satdLanesAvx2(r);
}

// One 4x4 source block against four predictions side by side (pred: 16 x 4, stride 16):
// out[i] = satdU8(src, stride, pred + 4 * i, 16).
__attribute__((target("avx2"))) inline void satd4x4x4U8Avx2(const uint8_t* src, int stride, const uint8_t* pred, int* out)
{
    __m256i r[4];
    for (int i = 0; i < 4; ++i) {
        int32_t w;
        std::memcpy(&w, src + i * stride, 4);
        r[i] = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm_set1_epi32(w)),
                                _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pred + i * 16))));
    }
    const __m256i s01 = _mm256_add_epi16(r[0], r[1]), d01 = _mm256_sub_epi16(r[0], r[1]);
    const __m256i s23 = _mm256_add_epi16(r[2], r[3]), d23 = _mm256_sub_epi16(r[2], r[3]);
    const __m256i t0 = _mm256_add_epi16(s01, s23), t1 = _mm256_sub_epi16(s01, s23);
    const __m256i t2 = _mm256_sub_epi16(d01, d23), t3 = _mm256_add_epi16(d01, d23);
    const __m256i u[2][2] = {{_mm256_unpacklo_epi16(t0, t1), _mm256_unpacklo_epi16(t2, t3)},
                             {_mm256_unpackhi_epi16(t0, t1), _mm256_unpackhi_epi16(t2, t3)}};
    for (int k = 0; k < 2; ++k) { // k = 0: blocks 0 (lane 0) and 2 (lane 1); k = 1: blocks 1 and 3
        const __m256i v0 = _mm256_unpacklo_epi32(u[k][0], u[k][1]), v1 = _mm256_unpackhi_epi32(u[k][0], u[k][1]);
        const __m256i c1 = _mm256_unpackhi_epi64(v0, v0), c3 = _mm256_unpackhi_epi64(v1, v1);
        const __m256i ha = _mm256_add_epi16(v0, c1), hb = _mm256_sub_epi16(v0, c1);
        const __m256i hc = _mm256_add_epi16(v1, c3), hd = _mm256_sub_epi16(v1, c3);
        const __m256i o01 = _mm256_unpacklo_epi64(_mm256_add_epi16(ha, hc), _mm256_sub_epi16(ha, hc));
        const __m256i o23 = _mm256_unpacklo_epi64(_mm256_sub_epi16(hb, hd), _mm256_add_epi16(hb, hd));
        const __m256i sum = _mm256_madd_epi16(_mm256_add_epi16(_mm256_abs_epi16(o01), _mm256_abs_epi16(o23)), _mm256_set1_epi16(1));
        const __m256i s2 = _mm256_add_epi32(sum, _mm256_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m256i s1 = _mm256_add_epi32(s2, _mm256_shuffle_epi32(s2, _MM_SHUFFLE(2, 3, 0, 1)));
        out[k] = (_mm256_cvtsi256_si32(s1) + 1) >> 1;
        out[2 + k] = (_mm_cvtsi128_si32(_mm256_extracti128_si256(s1, 1)) + 1) >> 1;
    }
}

// SATD of a w x h block (w = 8 or 16, h a multiple of 8 for w = 8, of 4 for w = 16) in one call:
// the same sum as the strip-wise calls of sadPart.
__attribute__((target("avx2"))) inline int satdBlockAvx2(const uint8_t* a, int strideA, const uint8_t* b, int strideB, int w, int h)
{
    __m256i acc = _mm256_setzero_si256();
    __m256i r[4];
    if (w == 16) {
        for (int y = 0; y < h; y += 4) {
            for (int i = 0; i < 4; ++i)
                r[i] = _mm256_sub_epi16(
                    _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + (y + i) * strideA))),
                    _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b + (y + i) * strideB))));
            satdLanesAccAvx2(r, acc);
        }
    } else {
        for (int y = 0; y < h; y += 8) {
            for (int i = 0; i < 4; ++i) {
                const uint8_t* pa = a + (y + i) * strideA;
                const uint8_t* pb = b + (y + i) * strideB;
                const __m128i ra = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(pa)),
                                                      _mm_loadl_epi64(reinterpret_cast<const __m128i*>(pa + 4 * strideA)));
                const __m128i rb = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(pb)),
                                                      _mm_loadl_epi64(reinterpret_cast<const __m128i*>(pb + 4 * strideB)));
                r[i] = _mm256_sub_epi16(_mm256_cvtepu8_epi16(ra), _mm256_cvtepu8_epi16(rb));
            }
            satdLanesAccAvx2(r, acc);
        }
    }
    // Elements 0 and 1 of each 128-bit lane hold the sums (2 and 3 repeat them).
    const __m128i lanes = _mm_add_epi32(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
    return _mm_cvtsi128_si32(lanes) + _mm_cvtsi128_si32(_mm_srli_si128(lanes, 4));
}

inline bool haveAvx2()
{
    static const bool yes = __builtin_cpu_supports("avx2");
    return yes;
}
#endif

// SATD of four 4x4 blocks side by side (16 x 4): AVX2 where the CPU has it, else two 8x4 calls.
inline int satd16x4U8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#ifdef SCHNEIDI_H264_AVX2
    if (haveAvx2())
        return satd16x4U8Avx2(a, strideA, b, strideB);
#endif
#ifdef SCHNEIDI_H264_NEON
    return int(vaddlvq_u16(vaddq_u16(neon_detail::satd8x4Lanes(a, strideA, b, strideB),
                                     neon_detail::satd8x4Lanes(a + 8, strideA, b + 8, strideB))));
#endif
    return satd8x4U8(a, strideA, b, strideB) + satd8x4U8(a + 8, strideA, b + 8, strideB);
}

// One 4x4 source block against four predictions side by side (pred: 16 x 4, stride 16):
// out[i] = satdU8(src, stride, pred + 4 * i, 16). Used to compare intra directions in one go.
inline void satd4x4x4U8(const uint8_t* src, int stride, const uint8_t* pred, int* out)
{
#ifdef SCHNEIDI_H264_AVX2
    if (haveAvx2()) {
        satd4x4x4U8Avx2(src, stride, pred, out);
        return;
    }
#endif
#ifdef SCHNEIDI_H264_NEON
    {
        using namespace neon_detail;
        int16x8_t lo[4], hi[4];
        for (int i = 0; i < 4; ++i) {
            uint32_t w;
            std::memcpy(&w, src + i * stride, 4);
            const uint8x8_t s2 = vreinterpret_u8_u32(vdup_n_u32(w)); // the source row twice
            const uint8x16_t p = vld1q_u8(pred + i * 16);
            lo[i] = vreinterpretq_s16_u16(vsubl_u8(s2, vget_low_u8(p)));
            hi[i] = vreinterpretq_s16_u16(vsubl_u8(s2, vget_high_u8(p)));
        }
        const uint32x4_t blocks = vpaddq_u32(vpaddlq_u16(satdLanes(lo[0], lo[1], lo[2], lo[3])),
                                             vpaddlq_u16(satdLanes(hi[0], hi[1], hi[2], hi[3])));
        vst1q_s32(out, vreinterpretq_s32_u32(blocks));
        return;
    }
#endif
    for (int i = 0; i < 4; ++i)
        out[i] = satdU8(src, stride, pred + 4 * i, 16);
}

// 8x8 Hadamard SATD of the difference of two 8x8 blocks, scaled like satd4x4 (sum / 4 instead of
// sum / 2: the 8-point transform gains 2x more than the 4-point one per dimension). Compared with
// satd8x8U8 it tells whether the 8x8 transform suits a residual better than four 4x4 ones.
inline int sa8dU8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#ifdef SCHNEIDI_H264_SSE2
    // Rows as 8 x int16: 8-point butterflies across the rows, transpose, again in 32 bit.
    const __m128i zero = _mm_setzero_si128();
    __m128i r[8];
    for (int i = 0; i < 8; ++i)
        r[i] = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(a + i * strideA)), zero),
                             _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(b + i * strideB)), zero));
    for (int s = 1; s < 8; s <<= 1)
        for (int i = 0; i < 8; i += 2 * s)
            for (int j = i; j < i + s; ++j) {
                const __m128i x = r[j], y = r[j + s];
                r[j] = _mm_add_epi16(x, y);
                r[j + s] = _mm_sub_epi16(x, y);
            }
    const __m128i a0 = _mm_unpacklo_epi16(r[0], r[1]), a1 = _mm_unpackhi_epi16(r[0], r[1]);
    const __m128i a2 = _mm_unpacklo_epi16(r[2], r[3]), a3 = _mm_unpackhi_epi16(r[2], r[3]);
    const __m128i a4 = _mm_unpacklo_epi16(r[4], r[5]), a5 = _mm_unpackhi_epi16(r[4], r[5]);
    const __m128i a6 = _mm_unpacklo_epi16(r[6], r[7]), a7 = _mm_unpackhi_epi16(r[6], r[7]);
    const __m128i b0 = _mm_unpacklo_epi32(a0, a2), b1 = _mm_unpackhi_epi32(a0, a2);
    const __m128i b2 = _mm_unpacklo_epi32(a1, a3), b3 = _mm_unpackhi_epi32(a1, a3);
    const __m128i b4 = _mm_unpacklo_epi32(a4, a6), b5 = _mm_unpackhi_epi32(a4, a6);
    const __m128i b6 = _mm_unpacklo_epi32(a5, a7), b7 = _mm_unpackhi_epi32(a5, a7);
    const __m128i t[8] = {_mm_unpacklo_epi64(b0, b4), _mm_unpackhi_epi64(b0, b4), _mm_unpacklo_epi64(b1, b5),
                          _mm_unpackhi_epi64(b1, b5), _mm_unpacklo_epi64(b2, b6), _mm_unpackhi_epi64(b2, b6),
                          _mm_unpacklo_epi64(b3, b7), _mm_unpackhi_epi64(b3, b7)};
    __m128i lo[8], hi[8];
    for (int i = 0; i < 8; ++i) {
        const __m128i sign = _mm_srai_epi16(t[i], 15);
        lo[i] = _mm_unpacklo_epi16(t[i], sign);
        hi[i] = _mm_unpackhi_epi16(t[i], sign);
    }
    for (__m128i* v : {lo, hi})
        for (int s = 1; s < 8; s <<= 1)
            for (int i = 0; i < 8; i += 2 * s)
                for (int j = i; j < i + s; ++j) {
                    const __m128i x = v[j], y = v[j + s];
                    v[j] = _mm_add_epi32(x, y);
                    v[j + s] = _mm_sub_epi32(x, y);
                }
    __m128i acc = zero;
    for (int i = 0; i < 8; ++i)
        for (__m128i v : {lo[i], hi[i]}) {
            const __m128i s = _mm_srai_epi32(v, 31);
            acc = _mm_add_epi32(acc, _mm_sub_epi32(_mm_xor_si128(v, s), s));
        }
    acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(1, 0, 3, 2)));
    acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(2, 3, 0, 1)));
    return (_mm_cvtsi128_si32(acc) + 2) >> 2;
#elif defined(SCHNEIDI_H264_NEON)
    // 16 bits suffice throughout: |coefficient| <= 64 * 255.
    int16x8_t r[8];
    for (int i = 0; i < 8; ++i)
        r[i] = neon_detail::diff8(a + i * strideA, b + i * strideB);
    for (int s = 1; s < 8; s <<= 1)
        for (int i = 0; i < 8; i += 2 * s)
            for (int j = i; j < i + s; ++j) {
                const int16x8_t x = r[j], y = r[j + s];
                r[j] = vaddq_s16(x, y);
                r[j + s] = vsubq_s16(x, y);
            }
    // Transpose 8 x 8 (16-bit, then 32-bit, then 64-bit pairs).
    int16x8_t t[8];
    for (int i = 0; i < 8; i += 2) {
        t[i] = vtrn1q_s16(r[i], r[i + 1]);
        t[i + 1] = vtrn2q_s16(r[i], r[i + 1]);
    }
    auto s32 = [](int16x8_t v) { return vreinterpretq_s32_s16(v); };
    auto s64 = [](int32x4_t v) { return vreinterpretq_s64_s32(v); };
    auto i16 = [](int64x2_t v) { return vreinterpretq_s16_s64(v); };
    int32x4_t u[8];
    for (int i = 0; i < 8; i += 4)
        for (int k = 0; k < 2; ++k) {
            u[i + k] = vtrn1q_s32(s32(t[i + k]), s32(t[i + 2 + k]));
            u[i + 2 + k] = vtrn2q_s32(s32(t[i + k]), s32(t[i + 2 + k]));
        }
    int16x8_t c[8];
    for (int k = 0; k < 4; ++k) {
        c[k] = i16(vtrn1q_s64(s64(u[k]), s64(u[4 + k])));
        c[4 + k] = i16(vtrn2q_s64(s64(u[k]), s64(u[4 + k])));
    }
    // c[k] = column k; the same butterflies across the columns, the last stage as 2 max(|x|, |y|).
    for (int s = 1; s < 4; s <<= 1)
        for (int i = 0; i < 8; i += 2 * s)
            for (int j = i; j < i + s; ++j) {
                const int16x8_t x = c[j], y = c[j + s];
                c[j] = vaddq_s16(x, y);
                c[j + s] = vsubq_s16(x, y);
            }
    uint32x4_t acc = vdupq_n_u32(0);
    for (int j = 0; j < 4; ++j)
        acc = vpadalq_u16(acc, vmaxq_u16(vreinterpretq_u16_s16(vabsq_s16(c[j])), vreinterpretq_u16_s16(vabsq_s16(c[j + 4]))));
    return int((vaddvq_u32(acc) + 1) >> 1); // (2m + 2) >> 2
#else
    int d[64];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            d[y * 8 + x] = int(a[y * strideA + x]) - int(b[y * strideB + x]);
    auto line = [](int* v, int step) {
        for (int s = 1; s < 8; s <<= 1)
            for (int i = 0; i < 8; i += 2 * s)
                for (int j = i; j < i + s; ++j) {
                    const int x = v[j * step], y = v[(j + s) * step];
                    v[j * step] = x + y;
                    v[(j + s) * step] = x - y;
                }
    };
    for (int y = 0; y < 8; ++y)
        line(d + y * 8, 1);
    for (int x = 0; x < 8; ++x)
        line(d + x, 8);
    int s = 0;
    for (int v : d)
        s += std::abs(v);
    return (s + 2) >> 2;
#endif
}

// SATD of an 8x8 block (four 4x4 blocks): AVX2 where the CPU has it.
inline int satd8x8U8Fast(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#ifdef SCHNEIDI_H264_AVX2
    if (haveAvx2())
        return satd8x8U8Avx2(a, strideA, b, strideB);
#endif
    return satd8x8U8(a, strideA, b, strideB);
}

// SAD of one row of 8 or 16 samples (SSE2 where available, plain C elsewhere).
inline int sadRow(const uint8_t* a, const uint8_t* b, int w)
{
#ifdef SCHNEIDI_H264_SSE2
    if (w == 16) {
        const __m128i s = _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)),
                                       _mm_loadu_si128(reinterpret_cast<const __m128i*>(b)));
        return _mm_cvtsi128_si32(s) + _mm_extract_epi16(s, 4);
    }
    if (w == 8) {
        const __m128i s = _mm_sad_epu8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(a)),
                                       _mm_loadl_epi64(reinterpret_cast<const __m128i*>(b)));
        return _mm_cvtsi128_si32(s);
    }
#elif defined(SCHNEIDI_H264_NEON)
    if (w == 16)
        return int(vaddlvq_u8(vabdq_u8(vld1q_u8(a), vld1q_u8(b))));
    if (w == 8)
        return int(vaddlv_u8(vabd_u8(vld1_u8(a), vld1_u8(b))));
#endif
    int s = 0;
    for (int x = 0; x < w; ++x)
        s += std::abs(int(a[x]) - int(b[x]));
    return s;
}

// SAD of a w x h block (w = 8 or 16, h even): same as summing sadRow, one horizontal sum at the end.
inline int sadBlock(const uint8_t* a, int strideA, const uint8_t* b, int strideB, int w, int h)
{
#ifdef SCHNEIDI_H264_SSE2
    __m128i acc = _mm_setzero_si128();
    if (w == 16) {
        for (int y = 0; y < h; ++y)
            acc = _mm_add_epi64(acc, _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + y * strideA)),
                                                  _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + y * strideB))));
    } else if (w == 8 && !(h & 1)) {
        for (int y = 0; y < h; y += 2) {
            const __m128i ra = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(a + y * strideA)),
                                                  _mm_loadl_epi64(reinterpret_cast<const __m128i*>(a + (y + 1) * strideA)));
            const __m128i rb = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(b + y * strideB)),
                                                  _mm_loadl_epi64(reinterpret_cast<const __m128i*>(b + (y + 1) * strideB)));
            acc = _mm_add_epi64(acc, _mm_sad_epu8(ra, rb));
        }
    } else {
        int s = 0;
        for (int y = 0; y < h; ++y)
            s += sadRow(a + y * strideA, b + y * strideB, w);
        return s;
    }
    return _mm_cvtsi128_si32(acc) + _mm_cvtsi128_si32(_mm_unpackhi_epi64(acc, acc));
#elif defined(SCHNEIDI_H264_NEON)
    if (w == 16 || w == 8) {
        // 16-bit lanes take 128 rows of 255 (w = 16: two accumulators, one per half).
        int s = 0;
        for (int y0 = 0; y0 < h; y0 += 128) {
            const int y1 = std::min(h, y0 + 128);
            uint16x8_t lo = vdupq_n_u16(0), hi = vdupq_n_u16(0);
            if (w == 16) {
                for (int y = y0; y < y1; ++y) {
                    const uint8x16_t ra = vld1q_u8(a + y * strideA), rb = vld1q_u8(b + y * strideB);
                    lo = vabal_u8(lo, vget_low_u8(ra), vget_low_u8(rb));
                    hi = vabal_high_u8(hi, ra, rb);
                }
            } else {
                for (int y = y0; y < y1; ++y)
                    lo = vabal_u8(lo, vld1_u8(a + y * strideA), vld1_u8(b + y * strideB));
            }
            s += int(vaddlvq_u16(lo) + vaddlvq_u16(hi));
        }
        return s;
    }
    int s = 0;
    for (int y = 0; y < h; ++y)
        s += sadRow(a + y * strideA, b + y * strideB, w);
    return s;
#else
    int s = 0;
    for (int y = 0; y < h; ++y)
        s += sadRow(a + y * strideA, b + y * strideB, w);
    return s;
#endif
}

// Magnitudes of the Hadamard coefficients of an 8x8 block of samples at two scales, without the DC
// ones (set to 0): m[0..63] the 4x4 transforms of its four quarters, m[64..127] its 8x8 transform
// (a 2x2 Hadamard across the quarters' coefficients). What the psycho-visual mode decision compares
// between source and reconstruction (hadamardMagsDistance): texture and grain have these
// magnitudes, a smoothed block has lost them. m: 16-byte aligned. The order of the coefficients
// differs between the SSE2 and the plain version; the distance does not depend on it.
inline void hadamardMags8x8U8(const uint8_t* p, int stride, int16_t* m)
{
#ifdef SCHNEIDI_H264_SSE2
    const __m128i zero = _mm_setzero_si128();
    auto absv = [&](__m128i x) { return _mm_max_epi16(x, _mm_sub_epi16(zero, x)); };
    __m128i c[4][2]; // per quarter (left/right of the upper, then of the lower half) its 16 coefficients
    for (int half = 0; half < 2; ++half) {
        const uint8_t* q = p + half * 4 * stride;
        auto row = [&](int i) { return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(q + i * stride)), zero); };
        const __m128i r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        const __m128i s01 = _mm_add_epi16(r0, r1), d01 = _mm_sub_epi16(r0, r1);
        const __m128i s23 = _mm_add_epi16(r2, r3), d23 = _mm_sub_epi16(r2, r3);
        const __m128i t0 = _mm_add_epi16(s01, s23), t1 = _mm_sub_epi16(s01, s23);
        const __m128i t2 = _mm_sub_epi16(d01, d23), t3 = _mm_add_epi16(d01, d23);
        for (int side = 0; side < 2; ++side) {
            const __m128i u0 = side ? _mm_unpackhi_epi16(t0, t1) : _mm_unpacklo_epi16(t0, t1);
            const __m128i u1 = side ? _mm_unpackhi_epi16(t2, t3) : _mm_unpacklo_epi16(t2, t3);
            const __m128i v0 = _mm_unpacklo_epi32(u0, u1), v1 = _mm_unpackhi_epi32(u0, u1);
            const __m128i c1 = _mm_unpackhi_epi64(v0, v0), c3 = _mm_unpackhi_epi64(v1, v1);
            const __m128i ha = _mm_add_epi16(v0, c1), hb = _mm_sub_epi16(v0, c1);
            const __m128i hc = _mm_add_epi16(v1, c3), hd = _mm_sub_epi16(v1, c3);
            const int qi = half * 2 + side;
            c[qi][0] = _mm_unpacklo_epi64(_mm_add_epi16(ha, hc), _mm_sub_epi16(ha, hc)); // lane 0: the sample sum
            c[qi][1] = _mm_unpacklo_epi64(_mm_sub_epi16(hb, hd), _mm_add_epi16(hb, hd));
            _mm_store_si128(reinterpret_cast<__m128i*>(m + qi * 16), absv(c[qi][0]));
            _mm_store_si128(reinterpret_cast<__m128i*>(m + qi * 16 + 8), absv(c[qi][1]));
            m[qi * 16] = 0;
        }
    }
    // Each |coefficient| of the 8x8 transform is at most 64 * 255: fits 16 bits.
    for (int k = 0; k < 2; ++k) {
        const __m128i a = _mm_add_epi16(c[0][k], c[1][k]), b = _mm_sub_epi16(c[0][k], c[1][k]);
        const __m128i d = _mm_add_epi16(c[2][k], c[3][k]), e = _mm_sub_epi16(c[2][k], c[3][k]);
        int16_t* o = m + 64 + k * 32;
        _mm_store_si128(reinterpret_cast<__m128i*>(o), absv(_mm_add_epi16(a, d)));
        _mm_store_si128(reinterpret_cast<__m128i*>(o + 8), absv(_mm_sub_epi16(a, d)));
        _mm_store_si128(reinterpret_cast<__m128i*>(o + 16), absv(_mm_add_epi16(b, e)));
        _mm_store_si128(reinterpret_cast<__m128i*>(o + 24), absv(_mm_sub_epi16(b, e)));
    }
    m[64] = 0;
#elif defined(SCHNEIDI_H264_NEON)
    // Own coefficient order (as the SSE2 one): per half (rows 0-3, 4-7) the four vectors of the 4x4
    // transforms of both quarters (lanes 0-3 left, 4-7 right), lane 0 / 4 of the first = the sums.
    int16x8_t c[2][4];
    for (int half = 0; half < 2; ++half) {
        const uint8_t* q = p + half * 4 * stride;
        auto row = [&](int i) { return vreinterpretq_s16_u16(vmovl_u8(vld1_u8(q + i * stride))); };
        const int16x8_t r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        const int16x8_t s01 = vaddq_s16(r0, r1), d01 = vsubq_s16(r0, r1);
        const int16x8_t s23 = vaddq_s16(r2, r3), d23 = vsubq_s16(r2, r3);
        const int16x8_t t0 = vaddq_s16(s01, s23), t1 = vsubq_s16(s01, s23);
        const int16x8_t t2 = vsubq_s16(d01, d23), t3 = vaddq_s16(d01, d23);
        const int16x8_t a0 = vtrn1q_s16(t0, t1), b0 = vtrn2q_s16(t0, t1);
        const int16x8_t a1 = vtrn1q_s16(t2, t3), b1 = vtrn2q_s16(t2, t3);
        const int32x4_t h0 = vreinterpretq_s32_s16(vaddq_s16(a0, b0)), h1 = vreinterpretq_s32_s16(vsubq_s16(a0, b0));
        const int32x4_t h2 = vreinterpretq_s32_s16(vaddq_s16(a1, b1)), h3 = vreinterpretq_s32_s16(vsubq_s16(a1, b1));
        const int16x8_t u0 = vreinterpretq_s16_s32(vtrn1q_s32(h0, h1)), u1 = vreinterpretq_s16_s32(vtrn2q_s32(h0, h1));
        const int16x8_t u2 = vreinterpretq_s16_s32(vtrn1q_s32(h2, h3)), u3 = vreinterpretq_s16_s32(vtrn2q_s32(h2, h3));
        c[half][0] = vaddq_s16(u0, u1);
        c[half][1] = vsubq_s16(u0, u1);
        c[half][2] = vaddq_s16(u2, u3);
        c[half][3] = vsubq_s16(u2, u3);
        for (int side = 0; side < 2; ++side) {
            int16_t* o = m + (half * 2 + side) * 16;
            for (int k = 0; k < 4; ++k)
                vst1_s16(o + k * 4, vabs_s16(side ? vget_high_s16(c[half][k]) : vget_low_s16(c[half][k])));
            o[0] = 0;
        }
    }
    // Each |coefficient| of the 8x8 transform is at most 64 * 255: fits 16 bits.
    for (int k = 0; k < 4; ++k) {
        const int16x4_t l0 = vget_low_s16(c[0][k]), r0 = vget_high_s16(c[0][k]);
        const int16x4_t l1 = vget_low_s16(c[1][k]), r1 = vget_high_s16(c[1][k]);
        const int16x4_t a = vadd_s16(l0, r0), b = vsub_s16(l0, r0), d = vadd_s16(l1, r1), e = vsub_s16(l1, r1);
        int16_t* o = m + 64 + k * 16;
        vst1_s16(o, vabs_s16(vadd_s16(a, d)));
        vst1_s16(o + 4, vabs_s16(vsub_s16(a, d)));
        vst1_s16(o + 8, vabs_s16(vadd_s16(b, e)));
        vst1_s16(o + 12, vabs_s16(vsub_s16(b, e)));
    }
    m[64] = 0;
#else
    int c[4][16];
    for (int q = 0; q < 4; ++q) {
        int in[16];
        for (int i = 0; i < 16; ++i)
            in[i] = p[((q >> 1) * 4 + (i >> 2)) * stride + (q & 1) * 4 + (i & 3)];
        hadamard4x4(in, c[q]);
        for (int i = 0; i < 16; ++i)
            m[q * 16 + i] = int16_t(i ? std::abs(c[q][i]) : 0);
    }
    for (int i = 0; i < 16; ++i) {
        const int a = c[0][i] + c[1][i], b = c[0][i] - c[1][i], d = c[2][i] + c[3][i], e = c[2][i] - c[3][i];
        m[64 + i * 4] = int16_t(i ? std::abs(a + d) : 0);
        m[64 + i * 4 + 1] = int16_t(std::abs(a - d));
        m[64 + i * 4 + 2] = int16_t(std::abs(b + e));
        m[64 + i * 4 + 3] = int16_t(std::abs(b - e));
    }
#endif
}

// Distance of two blocks by their coefficient magnitudes (hadamardMags8x8U8), in SATD units: the
// summed differences of the 4x4 magnitudes, (sum + 1) >> 1 like satd4x4, plus those of the 8x8
// ones, (sum + 2) >> 2 like sa8dU8. Coefficient by coefficient, not the difference of the totals: a
// block that makes up for lost fine detail with one strong pattern is no closer.
inline int hadamardMagsDistance(const int16_t* a, const int16_t* b)
{
    int sum[2];
#ifdef SCHNEIDI_H264_SSE2
    const __m128i zero = _mm_setzero_si128(), one = _mm_set1_epi16(1);
    for (int part = 0; part < 2; ++part) {
        __m128i acc = zero;
        for (int i = 0; i < 64; i += 8) {
            const __m128i d = _mm_sub_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(a + part * 64 + i)),
                                            _mm_load_si128(reinterpret_cast<const __m128i*>(b + part * 64 + i)));
            acc = _mm_add_epi32(acc, _mm_madd_epi16(_mm_max_epi16(d, _mm_sub_epi16(zero, d)), one));
        }
        acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(1, 0, 3, 2)));
        sum[part] = _mm_cvtsi128_si32(_mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(2, 3, 0, 1))));
    }
#elif defined(SCHNEIDI_H264_NEON)
    for (int part = 0; part < 2; ++part) {
        uint32x4_t acc = vdupq_n_u32(0);
        for (int i = 0; i < 64; i += 8) // magnitudes are >= 0 and below 2^15: |a - b| fits
            acc = vpadalq_u16(acc, vreinterpretq_u16_s16(vabdq_s16(vld1q_s16(a + part * 64 + i), vld1q_s16(b + part * 64 + i))));
        sum[part] = int(vaddvq_u32(acc));
    }
#else
    for (int part = 0; part < 2; ++part) {
        sum[part] = 0;
        for (int i = 0; i < 64; ++i)
            sum[part] += std::abs(a[part * 64 + i] - b[part * 64 + i]);
    }
#endif
    return ((sum[0] + 1) >> 1) + ((sum[1] + 2) >> 2);
}

} // namespace schneidi::h264
