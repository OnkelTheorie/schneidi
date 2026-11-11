// Shared distortion kernels of the H.264 encoder: SAD rows and 4x4 Hadamard SATD, SSE2 with a plain
// C++ fallback that gives the same numbers (the bitstream must not depend on the platform).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

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
__attribute__((target("avx2"))) inline int satd16x4U8Avx2(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
    __m256i r[4];
    for (int i = 0; i < 4; ++i)
        r[i] = _mm256_sub_epi16(_mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i * strideA))),
                                _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i * strideB))));
    const __m256i s01 = _mm256_add_epi16(r[0], r[1]), d01 = _mm256_sub_epi16(r[0], r[1]);
    const __m256i s23 = _mm256_add_epi16(r[2], r[3]), d23 = _mm256_sub_epi16(r[2], r[3]);
    const __m256i t0 = _mm256_add_epi16(s01, s23), t1 = _mm256_sub_epi16(s01, s23);
    const __m256i t2 = _mm256_sub_epi16(d01, d23), t3 = _mm256_add_epi16(d01, d23);
    const __m256i u[2][2] = {{_mm256_unpacklo_epi16(t0, t1), _mm256_unpacklo_epi16(t2, t3)},
                             {_mm256_unpackhi_epi16(t0, t1), _mm256_unpackhi_epi16(t2, t3)}};
    int total = 0;
    for (int k = 0; k < 2; ++k) { // horizontal stage, one block per lane, rounded per block
        const __m256i v0 = _mm256_unpacklo_epi32(u[k][0], u[k][1]), v1 = _mm256_unpackhi_epi32(u[k][0], u[k][1]);
        const __m256i c1 = _mm256_unpackhi_epi64(v0, v0), c3 = _mm256_unpackhi_epi64(v1, v1);
        const __m256i ha = _mm256_add_epi16(v0, c1), hb = _mm256_sub_epi16(v0, c1);
        const __m256i hc = _mm256_add_epi16(v1, c3), hd = _mm256_sub_epi16(v1, c3);
        const __m256i o01 = _mm256_unpacklo_epi64(_mm256_add_epi16(ha, hc), _mm256_sub_epi16(ha, hc));
        const __m256i o23 = _mm256_unpacklo_epi64(_mm256_sub_epi16(hb, hd), _mm256_add_epi16(hb, hd));
        const __m256i sum = _mm256_madd_epi16(_mm256_add_epi16(_mm256_abs_epi16(o01), _mm256_abs_epi16(o23)), _mm256_set1_epi16(1));
        const __m256i s2 = _mm256_add_epi32(sum, _mm256_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m256i s1 = _mm256_add_epi32(s2, _mm256_shuffle_epi32(s2, _MM_SHUFFLE(2, 3, 0, 1)));
        total += ((_mm256_cvtsi256_si32(s1) + 1) >> 1) + ((_mm_cvtsi128_si32(_mm256_extracti128_si256(s1, 1)) + 1) >> 1);
    }
    return total;
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

} // namespace schneidi::h264
