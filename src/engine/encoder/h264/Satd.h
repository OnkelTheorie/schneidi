// Shared distortion kernels of the H.264 encoder: SAD rows and 4x4 Hadamard SATD, SSE2 with a plain
// C++ fallback that gives the same numbers (the bitstream must not depend on the platform).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
// AVX2 kernels are compiled for that target only and picked at run time (GCC/Clang on x86).
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#define SCHNEIDI_H264_AVX2 1
#include <immintrin.h>
#endif

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

// SATD of a 4x4 block between two 8-bit images; same result as satd4x4 on their difference.
inline int satdU8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#if defined(__SSE2__) || defined(_M_X64)
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
#if defined(__SSE2__) || defined(_M_X64)
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
#else
    return satdU8(a, strideA, b, strideB) + satdU8(a + 4, strideA, b + 4, strideB);
#endif
}

// SATD of an 8x8 block as the sum of its four 4x4 blocks (the measure the mode decisions use).
inline int satd8x8U8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
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
    for (int i = 0; i < 4; ++i)
        out[i] = satdU8(src, stride, pred + 4 * i, 16);
}

// 8x8 Hadamard SATD of the difference of two 8x8 blocks, scaled like satd4x4 (sum / 4 instead of
// sum / 2: the 8-point transform gains 2x more than the 4-point one per dimension). Compared with
// satd8x8U8 it tells whether the 8x8 transform suits a residual better than four 4x4 ones.
inline int sa8dU8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#if defined(__SSE2__) || defined(_M_X64)
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
#if defined(__SSE2__) || defined(_M_X64)
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
#endif
    int s = 0;
    for (int x = 0; x < w; ++x)
        s += std::abs(int(a[x]) - int(b[x]));
    return s;
}

// SAD of a w x h block (w = 8 or 16, h even): same as summing sadRow, one horizontal sum at the end.
inline int sadBlock(const uint8_t* a, int strideA, const uint8_t* b, int strideB, int w, int h)
{
#if defined(__SSE2__) || defined(_M_X64)
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
#else
    int s = 0;
    for (int y = 0; y < h; ++y)
        s += sadRow(a + y * strideA, b + y * strideB, w);
    return s;
#endif
}

// AC energy of an 8x8 block of samples at two scales, in SATD units: e4 = the 4x4 Hadamard sums of
// its four quarters, e8 = its 8x8 Hadamard sum, each without the DC coefficients (which are the
// plain sample sums). What the psycho-visual mode decision compares between source and
// reconstruction: texture and grain have it, a smoothed block has lost it.
inline void acEnergy8x8U8(const uint8_t* p, int stride, int& e4, int& e8)
{
    static constexpr uint8_t kZero[64] = {};
    const int sum = sadBlock(p, stride, kZero, 8, 8, 8);
    e4 = satd8x8U8Fast(p, stride, kZero, 8) - (sum >> 1);
    e8 = sa8dU8(p, stride, kZero, 8) - (sum >> 2);
}

} // namespace schneidi::h264
