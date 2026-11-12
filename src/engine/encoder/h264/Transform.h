// Integer transforms of the H.264 encoder (8.5.12, 8.5.13 and their forward counterparts): SSE2 with a
// plain C++ fallback that gives the same numbers (the bitstream must not depend on the platform).
#pragma once

#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define SCHNEIDI_H264_SSE2 1
#endif

namespace schneidi::h264 {

namespace transform_detail {

// The core of the forward 4x4 transform on four values.
template <class V, class Add, class Sub, class Dbl>
inline void fwd4(V a, V b, V c, V d, V* o, Add add, Sub sub, Dbl dbl)
{
    const V s03 = add(a, d), d03 = sub(a, d), s12 = add(b, c), d12 = sub(b, c);
    o[0] = add(s03, s12);
    o[1] = add(dbl(d03), d12);
    o[2] = sub(s03, s12);
    o[3] = sub(d03, dbl(d12));
}

#ifdef SCHNEIDI_H264_SSE2
inline void transpose4(__m128i& a, __m128i& b, __m128i& c, __m128i& d)
{
    const __m128i t0 = _mm_unpacklo_epi32(a, b), t1 = _mm_unpacklo_epi32(c, d);
    const __m128i t2 = _mm_unpackhi_epi32(a, b), t3 = _mm_unpackhi_epi32(c, d);
    a = _mm_unpacklo_epi64(t0, t1);
    b = _mm_unpackhi_epi64(t0, t1);
    c = _mm_unpacklo_epi64(t2, t3);
    d = _mm_unpackhi_epi64(t2, t3);
}
inline __m128i ld(const int* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); }
inline void st(int* p, __m128i v) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v); }

// 8x8 int32 block as 8 rows of two halves (columns 0-3, 4-7): in-place transpose.
inline void transpose8(__m128i (*m)[2])
{
    // Quadrants: [r0-3][lo] stays, [r0-3][hi] <-> [r4-7][lo], [r4-7][hi] stays (each transposed).
    transpose4(m[0][0], m[1][0], m[2][0], m[3][0]);
    transpose4(m[4][1], m[5][1], m[6][1], m[7][1]);
    transpose4(m[0][1], m[1][1], m[2][1], m[3][1]);
    transpose4(m[4][0], m[5][0], m[6][0], m[7][0]);
    for (int i = 0; i < 4; ++i) {
        const __m128i t = m[i][1];
        m[i][1] = m[4 + i][0];
        m[4 + i][0] = t;
    }
}

// One-dimensional inverse 8x8 transform (8-361..8-384), exact, across eight vectors.
inline void inv8(const __m128i* d, __m128i* g)
{
    auto add = [](__m128i a, __m128i b) { return _mm_add_epi32(a, b); };
    auto sub = [](__m128i a, __m128i b) { return _mm_sub_epi32(a, b); };
    auto h = [](__m128i a) { return _mm_srai_epi32(a, 1); };
    auto q = [](__m128i a) { return _mm_srai_epi32(a, 2); };
    const __m128i e0 = add(d[0], d[4]), e1 = sub(sub(sub(d[5], d[3]), d[7]), h(d[7])), e2 = sub(d[0], d[4]);
    const __m128i e3 = sub(sub(add(d[1], d[7]), d[3]), h(d[3])), e4 = sub(h(d[2]), d[6]);
    const __m128i e5 = add(add(sub(d[7], d[1]), d[5]), h(d[5]));
    const __m128i e6 = add(d[2], h(d[6])), e7 = add(add(add(d[3], d[5]), d[1]), h(d[1]));
    const __m128i f0 = add(e0, e6), f1 = add(e1, q(e7)), f2 = add(e2, e4), f3 = add(e3, q(e5));
    const __m128i f4 = sub(e2, e4), f5 = sub(q(e3), e5), f6 = sub(e0, e6), f7 = sub(e7, q(e1));
    g[0] = add(f0, f7);
    g[1] = add(f2, f5);
    g[2] = add(f4, f3);
    g[3] = add(f6, f1);
    g[4] = sub(f6, f1);
    g[5] = sub(f4, f3);
    g[6] = sub(f2, f5);
    g[7] = sub(f0, f7);
}

inline __m128i mulConst(__m128i a, int k) // k in {3, 4, 6, 8, 10, 12}: shifts and adds (SSE2 has no 32-bit multiply)
{
    switch (k) {
    case 3: return _mm_add_epi32(_mm_slli_epi32(a, 1), a);
    case 4: return _mm_slli_epi32(a, 2);
    case 6: return _mm_add_epi32(_mm_slli_epi32(a, 2), _mm_slli_epi32(a, 1));
    case 8: return _mm_slli_epi32(a, 3);
    case 10: return _mm_add_epi32(_mm_slli_epi32(a, 3), _mm_slli_epi32(a, 1));
    default: return _mm_add_epi32(_mm_slli_epi32(a, 3), _mm_slli_epi32(a, 2)); // 12
    }
}

// Forward 8x8 core (8 * the transpose of the inverse's basis) across eight vectors.
inline void fwd8(const __m128i* x, __m128i* y)
{
    auto add = [](__m128i a, __m128i b) { return _mm_add_epi32(a, b); };
    auto sub = [](__m128i a, __m128i b) { return _mm_sub_epi32(a, b); };
    const __m128i a0 = add(x[0], x[7]), a1 = add(x[1], x[6]), a2 = add(x[2], x[5]), a3 = add(x[3], x[4]);
    const __m128i a4 = sub(x[0], x[7]), a5 = sub(x[1], x[6]), a6 = sub(x[2], x[5]), a7 = sub(x[3], x[4]);
    const __m128i b0 = add(a0, a3), b1 = add(a1, a2), b2 = sub(a0, a3), b3 = sub(a1, a2);
    y[0] = _mm_slli_epi32(add(b0, b1), 3);
    y[4] = _mm_slli_epi32(sub(b0, b1), 3);
    y[2] = add(mulConst(b2, 8), mulConst(b3, 4));
    y[6] = sub(mulConst(b2, 4), mulConst(b3, 8));
    y[1] = add(add(mulConst(a4, 12), mulConst(a5, 10)), add(mulConst(a6, 6), mulConst(a7, 3)));
    y[3] = sub(sub(mulConst(a4, 10), mulConst(a5, 3)), add(mulConst(a6, 12), mulConst(a7, 6)));
    y[5] = add(sub(mulConst(a4, 6), mulConst(a5, 12)), add(mulConst(a6, 3), mulConst(a7, 10)));
    y[7] = sub(add(sub(mulConst(a4, 3), mulConst(a5, 6)), mulConst(a6, 10)), mulConst(a7, 12));
}
#endif

// Scalar forms of the same.
inline void inv8c(const int* d, int* g)
{
    const int e0 = d[0] + d[4], e1 = -d[3] + d[5] - d[7] - (d[7] >> 1), e2 = d[0] - d[4];
    const int e3 = d[1] + d[7] - d[3] - (d[3] >> 1), e4 = (d[2] >> 1) - d[6], e5 = -d[1] + d[7] + d[5] + (d[5] >> 1);
    const int e6 = d[2] + (d[6] >> 1), e7 = d[3] + d[5] + d[1] + (d[1] >> 1);
    const int f0 = e0 + e6, f1 = e1 + (e7 >> 2), f2 = e2 + e4, f3 = e3 + (e5 >> 2);
    const int f4 = e2 - e4, f5 = (e3 >> 2) - e5, f6 = e0 - e6, f7 = e7 - (e1 >> 2);
    g[0] = f0 + f7;
    g[1] = f2 + f5;
    g[2] = f4 + f3;
    g[3] = f6 + f1;
    g[4] = f6 - f1;
    g[5] = f4 - f3;
    g[6] = f2 - f5;
    g[7] = f0 - f7;
}
inline void fwd8c(const int* x, int* y)
{
    const int a0 = x[0] + x[7], a1 = x[1] + x[6], a2 = x[2] + x[5], a3 = x[3] + x[4];
    const int a4 = x[0] - x[7], a5 = x[1] - x[6], a6 = x[2] - x[5], a7 = x[3] - x[4];
    const int b0 = a0 + a3, b1 = a1 + a2, b2 = a0 - a3, b3 = a1 - a2;
    y[0] = 8 * (b0 + b1);
    y[4] = 8 * (b0 - b1);
    y[2] = 8 * b2 + 4 * b3;
    y[6] = 4 * b2 - 8 * b3;
    y[1] = 12 * a4 + 10 * a5 + 6 * a6 + 3 * a7;
    y[3] = 10 * a4 - 3 * a5 - 12 * a6 - 6 * a7;
    y[5] = 6 * a4 - 12 * a5 + 3 * a6 + 10 * a7;
    y[7] = 3 * a4 - 6 * a5 + 10 * a6 - 12 * a7;
}

} // namespace transform_detail

// Forward 4x4 core transform (8.5.12 inverted, no scaling), raster in and out.
inline void forward4x4(const int* in, int* out)
{
#ifdef SCHNEIDI_H264_SSE2
    using namespace transform_detail;
    auto add = [](__m128i a, __m128i b) { return _mm_add_epi32(a, b); };
    auto sub = [](__m128i a, __m128i b) { return _mm_sub_epi32(a, b); };
    auto dbl = [](__m128i a) { return _mm_slli_epi32(a, 1); };
    // Linear and exact in integers: columns first, then rows gives the same numbers.
    __m128i v[4];
    fwd4(ld(in), ld(in + 4), ld(in + 8), ld(in + 12), v, add, sub, dbl);
    transpose4(v[0], v[1], v[2], v[3]);
    __m128i w[4];
    fwd4(v[0], v[1], v[2], v[3], w, add, sub, dbl);
    transpose4(w[0], w[1], w[2], w[3]);
    for (int i = 0; i < 4; ++i)
        st(out + 4 * i, w[i]);
#else
    int t[16];
    for (int y = 0; y < 4; ++y) {
        const int* r = in + y * 4;
        const int s03 = r[0] + r[3], d03 = r[0] - r[3];
        const int s12 = r[1] + r[2], d12 = r[1] - r[2];
        t[y * 4 + 0] = s03 + s12;
        t[y * 4 + 1] = 2 * d03 + d12;
        t[y * 4 + 2] = s03 - s12;
        t[y * 4 + 3] = d03 - 2 * d12;
    }
    for (int x = 0; x < 4; ++x) {
        const int s03 = t[x] + t[12 + x], d03 = t[x] - t[12 + x];
        const int s12 = t[4 + x] + t[8 + x], d12 = t[4 + x] - t[8 + x];
        out[x] = s03 + s12;
        out[4 + x] = 2 * d03 + d12;
        out[8 + x] = s03 - s12;
        out[12 + x] = d03 - 2 * d12;
    }
#endif
}

// forward4x4 of the difference a - b of two 4x4 blocks of 8-bit samples.
inline void forward4x4Diff(const uint8_t* a, int strideA, const uint8_t* b, int strideB, int* out)
{
#ifdef SCHNEIDI_H264_SSE2
    using namespace transform_detail;
    auto add = [](__m128i x, __m128i y) { return _mm_add_epi32(x, y); };
    auto sub = [](__m128i x, __m128i y) { return _mm_sub_epi32(x, y); };
    auto dbl = [](__m128i x) { return _mm_slli_epi32(x, 1); };
    const __m128i zero = _mm_setzero_si128();
    auto row = [&](int i) {
        int32_t wa, wb;
        std::memcpy(&wa, a + i * strideA, 4);
        std::memcpy(&wb, b + i * strideB, 4);
        const __m128i d = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(wa), zero), _mm_unpacklo_epi8(_mm_cvtsi32_si128(wb), zero));
        return _mm_srai_epi32(_mm_unpacklo_epi16(d, d), 16); // sign-extended to 32 bits
    };
    __m128i v[4];
    fwd4(row(0), row(1), row(2), row(3), v, add, sub, dbl);
    transpose4(v[0], v[1], v[2], v[3]);
    __m128i w[4];
    fwd4(v[0], v[1], v[2], v[3], w, add, sub, dbl);
    transpose4(w[0], w[1], w[2], w[3]);
    for (int i = 0; i < 4; ++i)
        st(out + 4 * i, w[i]);
#else
    int res[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            res[y * 4 + x] = int(a[y * strideA + x]) - int(b[y * strideB + x]);
    forward4x4(res, out);
#endif
}

// True when any |coef[i]| (4x4, raster) exceeds limit[i & 7] (rows alternate between two patterns).
inline bool anyAbove4x4(const int* coef, const int* limit)
{
#ifdef SCHNEIDI_H264_SSE2
    using namespace transform_detail;
    const __m128i l0 = ld(limit), l1 = ld(limit + 4);
    auto over = [](__m128i x, __m128i l) {
        const __m128i m = _mm_srai_epi32(x, 31);
        return _mm_cmpgt_epi32(_mm_sub_epi32(_mm_xor_si128(x, m), m), l);
    };
    const __m128i any = _mm_or_si128(_mm_or_si128(over(ld(coef), l0), over(ld(coef + 4), l1)),
                                     _mm_or_si128(over(ld(coef + 8), l0), over(ld(coef + 12), l1)));
    return _mm_movemask_epi8(any) != 0;
#else
    for (int i = 0; i < 16; ++i)
        if ((coef[i] < 0 ? -coef[i] : coef[i]) > limit[i & 7])
            return true;
    return false;
#endif
}

// 8.5.12.2: rows first, then columns, then (x + 32) >> 6.
inline void inverse4x4(const int* in, int* out)
{
#ifdef SCHNEIDI_H264_SSE2
    using namespace transform_detail;
    // Not linear (the halvings round): rows first as in the standard, so transpose first.
    __m128i c0 = ld(in), c1 = ld(in + 4), c2 = ld(in + 8), c3 = ld(in + 12);
    transpose4(c0, c1, c2, c3); // c_x: lanes = rows
    auto pass = [](__m128i d0, __m128i d1, __m128i d2, __m128i d3, __m128i* o) {
        const __m128i e = _mm_add_epi32(d0, d2), f = _mm_sub_epi32(d0, d2);
        const __m128i g = _mm_sub_epi32(_mm_srai_epi32(d1, 1), d3), h = _mm_add_epi32(d1, _mm_srai_epi32(d3, 1));
        o[0] = _mm_add_epi32(e, h);
        o[1] = _mm_add_epi32(f, g);
        o[2] = _mm_sub_epi32(f, g);
        o[3] = _mm_sub_epi32(e, h);
    };
    __m128i t[4];
    pass(c0, c1, c2, c3, t);
    transpose4(t[0], t[1], t[2], t[3]); // rows again
    __m128i o[4];
    pass(t[0], t[1], t[2], t[3], o);
    const __m128i r = _mm_set1_epi32(32);
    for (int i = 0; i < 4; ++i)
        st(out + 4 * i, _mm_srai_epi32(_mm_add_epi32(o[i], r), 6));
#else
    int t[16];
    for (int y = 0; y < 4; ++y) {
        const int* d = in + y * 4;
        const int e = d[0] + d[2], f = d[0] - d[2];
        const int g = (d[1] >> 1) - d[3], h = d[1] + (d[3] >> 1);
        t[y * 4 + 0] = e + h;
        t[y * 4 + 1] = f + g;
        t[y * 4 + 2] = f - g;
        t[y * 4 + 3] = e - h;
    }
    for (int x = 0; x < 4; ++x) {
        const int e = t[x] + t[8 + x], f = t[x] - t[8 + x];
        const int g = (t[4 + x] >> 1) - t[12 + x], h = t[4 + x] + (t[12 + x] >> 1);
        out[x] = (e + h + 32) >> 6;
        out[4 + x] = (f + g + 32) >> 6;
        out[8 + x] = (f - g + 32) >> 6;
        out[12 + x] = (e - h + 32) >> 6;
    }
#endif
}

// Forward 8x8 core: w[i * 8 + j] = sum over y, x of M[i][y] M[j][x] r[y * 8 + x] with M = 8 * the
// transpose of the inverse transform's basis (integer). Exact.
inline void forward8x8(const int* r, int* w)
{
#ifdef SCHNEIDI_H264_SSE2
    using namespace transform_detail;
    __m128i lo[8], hi[8], m[8][2];
    for (int y = 0; y < 8; ++y) {
        lo[y] = ld(r + y * 8);
        hi[y] = ld(r + y * 8 + 4);
    }
    __m128i vlo[8], vhi[8];
    fwd8(lo, vlo); // vertical frequencies, lanes = columns
    fwd8(hi, vhi);
    for (int i = 0; i < 8; ++i) {
        m[i][0] = vlo[i];
        m[i][1] = vhi[i];
    }
    transpose8(m); // m[x]: lanes = vertical frequency
    for (int x = 0; x < 8; ++x) {
        lo[x] = m[x][0];
        hi[x] = m[x][1];
    }
    fwd8(lo, vlo);
    fwd8(hi, vhi);
    for (int j = 0; j < 8; ++j) {
        m[j][0] = vlo[j];
        m[j][1] = vhi[j];
    }
    transpose8(m); // m[i]: lanes = horizontal frequency
    for (int i = 0; i < 8; ++i) {
        st(w + i * 8, m[i][0]);
        st(w + i * 8 + 4, m[i][1]);
    }
#else
    using namespace transform_detail;
    int t[64];
    for (int y = 0; y < 8; ++y)
        fwd8c(r + y * 8, t + y * 8); // t[y][j]
    for (int j = 0; j < 8; ++j) {
        int col[8], o[8];
        for (int y = 0; y < 8; ++y)
            col[y] = t[y * 8 + j];
        fwd8c(col, o);
        for (int i = 0; i < 8; ++i)
            w[i * 8 + j] = o[i];
    }
#endif
}

// Decoder-exact inverse 8x8 (8.5.13): rows, then columns, then (x + 32) >> 6.
inline void inverse8x8(const int* d, int* out)
{
#ifdef SCHNEIDI_H264_SSE2
    using namespace transform_detail;
    __m128i m[8][2];
    for (int y = 0; y < 8; ++y) {
        m[y][0] = ld(d + y * 8);
        m[y][1] = ld(d + y * 8 + 4);
    }
    transpose8(m); // m[x]: lanes = rows
    __m128i lo[8], hi[8], glo[8], ghi[8];
    for (int x = 0; x < 8; ++x) {
        lo[x] = m[x][0];
        hi[x] = m[x][1];
    }
    inv8(lo, glo); // row transform: glo[k] lanes = rows 0-3
    inv8(hi, ghi);
    for (int k = 0; k < 8; ++k) {
        m[k][0] = glo[k];
        m[k][1] = ghi[k];
    }
    transpose8(m); // m[y]: lanes = columns
    for (int y = 0; y < 8; ++y) {
        lo[y] = m[y][0];
        hi[y] = m[y][1];
    }
    inv8(lo, glo); // column transform
    inv8(hi, ghi);
    const __m128i rnd = _mm_set1_epi32(32);
    for (int i = 0; i < 8; ++i) {
        st(out + i * 8, _mm_srai_epi32(_mm_add_epi32(glo[i], rnd), 6));
        st(out + i * 8 + 4, _mm_srai_epi32(_mm_add_epi32(ghi[i], rnd), 6));
    }
#else
    using namespace transform_detail;
    int t[64];
    for (int i = 0; i < 8; ++i)
        inv8c(d + i * 8, t + i * 8);
    for (int j = 0; j < 8; ++j) {
        int col[8], res[8];
        for (int i = 0; i < 8; ++i)
            col[i] = t[i * 8 + j];
        inv8c(col, res);
        for (int i = 0; i < 8; ++i)
            out[i * 8 + j] = (res[i] + 32) >> 6;
    }
#endif
}

} // namespace schneidi::h264
