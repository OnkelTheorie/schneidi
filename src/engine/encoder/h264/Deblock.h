// Deblocking filter kernels of the H.264 encoder (8.7.2.3, 8.7.2.4): one call filters a whole edge of a
// macroblock (16 luma lines, or 8 Cb + 8 Cr lines). SSE2 with a plain C++ fallback that gives the same
// samples (the bitstream must not depend on the platform), and NEON (the same steps): load the lines
// of one edge, filter 16 lanes at once, store them back.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "Simd.h"
#ifdef SCHNEIDI_H264_SSE2
#define SCHNEIDI_H264_DEBLOCK_SSE2 1
#elif defined(SCHNEIDI_H264_NEON)
#define SCHNEIDI_H264_DEBLOCK_NEON 1
#endif

namespace schneidi::h264 {

// One edge: alpha/beta of the edge's average QP; tc0 per 4-line segment (luma) or per 2-line pair
// (chroma, the same four values), -1 = bS 0 (segment left alone); bs4 = strong filter (an intra side on a
// macroblock edge: then every segment has bS 4 and tc0 is unused).
struct DeblockEdge {
    int alpha = 0, beta = 0;
    int8_t tc0[4] = {-1, -1, -1, -1};
    bool bs4 = false;
};

namespace deblock_detail {

inline uint8_t clipPixel(int v) { return uint8_t(std::clamp(v, 0, 255)); }

// One line across an edge, q at q0, step between samples across the edge (reference version).
inline void lumaLine(uint8_t* q, ptrdiff_t step, int alpha, int beta, int tc0, bool bs4)
{
    const int p0 = q[-step], p1 = q[-2 * step], q0 = q[0], q1 = q[step];
    if (!(std::abs(p0 - q0) < alpha && std::abs(p1 - p0) < beta && std::abs(q1 - q0) < beta))
        return;
    const int p2 = q[-3 * step], q2 = q[2 * step];
    const int ap = std::abs(p2 - p0), aq = std::abs(q2 - q0);
    if (!bs4) {
        const int tc = tc0 + (ap < beta) + (aq < beta);
        const int delta = std::clamp((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tc, tc);
        q[-step] = clipPixel(p0 + delta);
        q[0] = clipPixel(q0 - delta);
        if (ap < beta)
            q[-2 * step] = uint8_t(p1 + std::clamp((p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1, -tc0, tc0));
        if (aq < beta)
            q[step] = uint8_t(q1 + std::clamp((q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1, -tc0, tc0));
        return;
    }
    const int p3 = q[-4 * step], q3 = q[3 * step];
    const bool strong = std::abs(p0 - q0) < ((alpha >> 2) + 2);
    if (ap < beta && strong) {
        q[-step] = uint8_t((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
        q[-2 * step] = uint8_t((p2 + p1 + p0 + q0 + 2) >> 2);
        q[-3 * step] = uint8_t((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
    } else {
        q[-step] = uint8_t((2 * p1 + p0 + q1 + 2) >> 2);
    }
    if (aq < beta && strong) {
        q[0] = uint8_t((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
        q[step] = uint8_t((p0 + q0 + q1 + q2 + 2) >> 2);
        q[2 * step] = uint8_t((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
    } else {
        q[0] = uint8_t((2 * q1 + q0 + p1 + 2) >> 2);
    }
}

inline void chromaLine(uint8_t* q, ptrdiff_t step, int alpha, int beta, int tc0, bool bs4)
{
    const int p0 = q[-step], p1 = q[-2 * step], q0 = q[0], q1 = q[step];
    if (!(std::abs(p0 - q0) < alpha && std::abs(p1 - p0) < beta && std::abs(q1 - q0) < beta))
        return;
    if (!bs4) {
        const int tc = tc0 + 1;
        const int delta = std::clamp((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tc, tc);
        q[-step] = clipPixel(p0 + delta);
        q[0] = clipPixel(q0 - delta);
    } else {
        q[-step] = uint8_t((2 * p1 + p0 + q1 + 2) >> 2);
        q[0] = uint8_t((2 * q1 + q0 + p1 + 2) >> 2);
    }
}

#ifdef SCHNEIDI_H264_DEBLOCK_SSE2
// 16 lanes as two halves of 8 x int16.
struct W {
    __m128i lo, hi;
};
inline W widen(__m128i v)
{
    const __m128i z = _mm_setzero_si128();
    return {_mm_unpacklo_epi8(v, z), _mm_unpackhi_epi8(v, z)};
}
inline __m128i narrow(W v) { return _mm_packus_epi16(v.lo, v.hi); } // saturates to 0..255 = clip1
inline W add(W a, W b) { return {_mm_add_epi16(a.lo, b.lo), _mm_add_epi16(a.hi, b.hi)}; }
inline W sub(W a, W b) { return {_mm_sub_epi16(a.lo, b.lo), _mm_sub_epi16(a.hi, b.hi)}; }
inline W addc(W a, int c) { const __m128i k = _mm_set1_epi16(int16_t(c)); return {_mm_add_epi16(a.lo, k), _mm_add_epi16(a.hi, k)}; }
inline W shl(W a, int s) { return {_mm_slli_epi16(a.lo, s), _mm_slli_epi16(a.hi, s)}; }
inline W sra(W a, int s) { return {_mm_srai_epi16(a.lo, s), _mm_srai_epi16(a.hi, s)}; }
inline W clampw(W a, W lo, W hi)
{
    return {_mm_min_epi16(_mm_max_epi16(a.lo, lo.lo), hi.lo), _mm_min_epi16(_mm_max_epi16(a.hi, lo.hi), hi.hi)};
}
inline W neg(W a)
{
    const __m128i z = _mm_setzero_si128();
    return {_mm_sub_epi16(z, a.lo), _mm_sub_epi16(z, a.hi)};
}
// 8-bit lanes: |a - b| < t as an all-ones mask (t may be 0).
inline __m128i absLess(__m128i a, __m128i b, __m128i t)
{
    const __m128i d = _mm_or_si128(_mm_subs_epu8(a, b), _mm_subs_epu8(b, a));
    return _mm_xor_si128(_mm_cmpeq_epi8(_mm_subs_epu8(t, d), _mm_setzero_si128()), _mm_set1_epi8(-1));
}
inline __m128i blend(__m128i mask, __m128i yes, __m128i no)
{
    return _mm_or_si128(_mm_and_si128(mask, yes), _mm_andnot_si128(mask, no));
}

// Filters 16 lanes; v[0..7] = p3 p2 p1 p0 q0 q1 q2 q3, tc0 per lane (int8, -1 = not filtered).
inline bool lumaCore(__m128i* v, const DeblockEdge& e, __m128i tc0v)
{
    const __m128i p2 = v[1], p1 = v[2], p0 = v[3], q0 = v[4], q1 = v[5], q2 = v[6];
    const __m128i alpha = _mm_set1_epi8(char(e.alpha)), beta = _mm_set1_epi8(char(e.beta));
    __m128i mask = _mm_and_si128(absLess(p0, q0, alpha), _mm_and_si128(absLess(p1, p0, beta), absLess(q1, q0, beta)));
    if (!e.bs4)
        mask = _mm_andnot_si128(_mm_cmpeq_epi8(tc0v, _mm_set1_epi8(-1)), mask);
    if (_mm_movemask_epi8(mask) == 0)
        return false;
    const __m128i ap = absLess(p2, p0, beta), aq = absLess(q2, q0, beta);
    const W P1 = widen(p1), P0 = widen(p0), Q0 = widen(q0), Q1 = widen(q1), P2 = widen(p2), Q2 = widen(q2);
    if (!e.bs4) {
        const __m128i z = _mm_setzero_si128();
        const W tc0 = {_mm_unpacklo_epi8(tc0v, z), _mm_unpackhi_epi8(tc0v, z)}; // masked lanes don't matter
        const W apw = {_mm_unpacklo_epi8(ap, ap), _mm_unpackhi_epi8(ap, ap)};   // -1 / 0 per int16
        const W aqw = {_mm_unpacklo_epi8(aq, aq), _mm_unpackhi_epi8(aq, aq)};
        const W tc = sub(sub(tc0, apw), aqw);
        const W delta = clampw(sra(addc(add(shl(sub(Q0, P0), 2), sub(P1, Q1)), 4), 3), neg(tc), tc);
        v[3] = blend(mask, narrow(add(P0, delta)), p0);
        v[4] = blend(mask, narrow(sub(Q0, delta)), q0);
        const W avg = sra(addc(add(P0, Q0), 1), 1);
        const W nt = neg(tc0);
        const W np1 = add(P1, clampw(sra(sub(add(P2, avg), shl(P1, 1)), 1), nt, tc0));
        const W nq1 = add(Q1, clampw(sra(sub(add(Q2, avg), shl(Q1, 1)), 1), nt, tc0));
        v[2] = blend(_mm_and_si128(mask, ap), narrow(np1), p1);
        v[5] = blend(_mm_and_si128(mask, aq), narrow(nq1), q1);
        return true;
    }
    const W P3 = widen(v[0]), Q3 = widen(v[7]);
    const __m128i strong = absLess(p0, q0, _mm_set1_epi8(char((e.alpha >> 2) + 2)));
    const __m128i sp = _mm_and_si128(mask, _mm_and_si128(ap, strong));
    const __m128i sq = _mm_and_si128(mask, _mm_and_si128(aq, strong));
    const W p0q0 = add(P0, Q0);
    // Weak variants (only p0 / q0).
    const __m128i wp0 = narrow(sra(addc(add(add(shl(P1, 1), P0), Q1), 2), 2));
    const __m128i wq0 = narrow(sra(addc(add(add(shl(Q1, 1), Q0), P1), 2), 2));
    // Strong variants.
    const __m128i sp0 = narrow(sra(addc(add(add(P2, shl(add(P1, p0q0), 1)), Q1), 4), 3));
    const __m128i sp1 = narrow(sra(addc(add(add(P2, P1), p0q0), 2), 2));
    const __m128i sp2 = narrow(sra(addc(add(add(shl(P3, 1), add(shl(P2, 1), P2)), add(P1, p0q0)), 4), 3));
    const __m128i sq0 = narrow(sra(addc(add(add(Q2, shl(add(Q1, p0q0), 1)), P1), 4), 3));
    const __m128i sq1 = narrow(sra(addc(add(add(Q2, Q1), p0q0), 2), 2));
    const __m128i sq2 = narrow(sra(addc(add(add(shl(Q3, 1), add(shl(Q2, 1), Q2)), add(Q1, p0q0)), 4), 3));
    v[3] = blend(mask, blend(sp, sp0, wp0), p0);
    v[2] = blend(sp, sp1, p1);
    v[1] = blend(sp, sp2, p2);
    v[4] = blend(mask, blend(sq, sq0, wq0), q0);
    v[5] = blend(sq, sq1, q1);
    v[6] = blend(sq, sq2, q2);
    return true;
}

// v[0..3] = p1 p0 q0 q1.
inline bool chromaCore(__m128i* v, const DeblockEdge& e, __m128i tc0v)
{
    const __m128i p1 = v[0], p0 = v[1], q0 = v[2], q1 = v[3];
    const __m128i alpha = _mm_set1_epi8(char(e.alpha)), beta = _mm_set1_epi8(char(e.beta));
    __m128i mask = _mm_and_si128(absLess(p0, q0, alpha), _mm_and_si128(absLess(p1, p0, beta), absLess(q1, q0, beta)));
    if (!e.bs4)
        mask = _mm_andnot_si128(_mm_cmpeq_epi8(tc0v, _mm_set1_epi8(-1)), mask);
    if (_mm_movemask_epi8(mask) == 0)
        return false;
    const W P1 = widen(p1), P0 = widen(p0), Q0 = widen(q0), Q1 = widen(q1);
    if (!e.bs4) {
        const __m128i z = _mm_setzero_si128();
        const W tc = addc(W{_mm_unpacklo_epi8(tc0v, z), _mm_unpackhi_epi8(tc0v, z)}, 1);
        const W delta = clampw(sra(addc(add(shl(sub(Q0, P0), 2), sub(P1, Q1)), 4), 3), neg(tc), tc);
        v[1] = blend(mask, narrow(add(P0, delta)), p0);
        v[2] = blend(mask, narrow(sub(Q0, delta)), q0);
        return true;
    }
    v[1] = blend(mask, narrow(sra(addc(add(add(shl(P1, 1), P0), Q1), 2), 2)), p0);
    v[2] = blend(mask, narrow(sra(addc(add(add(shl(Q1, 1), Q0), P1), 2), 2)), q0);
    return true;
}

// tc0 per lane: luma lane i -> segment i / 4, chroma lane i (Cb 0..7, Cr 8..15) -> segment (i & 7) / 2.
inline __m128i lumaTc0(const DeblockEdge& e)
{
    const __m128i t = _mm_cvtsi32_si128(int(uint8_t(e.tc0[0])) | int(uint8_t(e.tc0[1])) << 8 |
                                        int(uint8_t(e.tc0[2])) << 16 | int(uint32_t(uint8_t(e.tc0[3])) << 24));
    const __m128i t2 = _mm_unpacklo_epi8(t, t);  // 00112233
    return _mm_unpacklo_epi16(t2, t2);            // 0000111122223333
}
inline __m128i chromaTc0(const DeblockEdge& e)
{
    const __m128i t = _mm_cvtsi32_si128(int(uint8_t(e.tc0[0])) | int(uint8_t(e.tc0[1])) << 8 |
                                        int(uint8_t(e.tc0[2])) << 16 | int(uint32_t(uint8_t(e.tc0[3])) << 24));
    const __m128i t2 = _mm_unpacklo_epi8(t, t); // 00112233 (8 lanes)
    return _mm_unpacklo_epi64(t2, t2);
}

inline __m128i ld8(const uint8_t* p) { return _mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)); }
inline void st8(uint8_t* p, __m128i v) { _mm_storel_epi64(reinterpret_cast<__m128i*>(p), v); }
inline __m128i ld4(const uint8_t* p)
{
    int32_t x;
    std::memcpy(&x, p, 4);
    return _mm_cvtsi32_si128(x);
}
inline void st4(uint8_t* p, __m128i v)
{
    const int32_t x = _mm_cvtsi128_si32(v);
    std::memcpy(p, &x, 4);
}
#endif

#ifdef SCHNEIDI_H264_DEBLOCK_NEON
// The SSE2 steps with NEON (16 lanes as two halves of 8 x int16; the same 16-bit wrap-around).
struct Wn {
    int16x8_t lo, hi;
};
inline Wn widen(uint8x16_t v) { return {vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(v))), vreinterpretq_s16_u16(vmovl_high_u8(v))}; }
inline uint8x16_t narrow(Wn v) { return vcombine_u8(vqmovun_s16(v.lo), vqmovun_s16(v.hi)); } // = clip1
inline Wn add(Wn a, Wn b) { return {vaddq_s16(a.lo, b.lo), vaddq_s16(a.hi, b.hi)}; }
inline Wn sub(Wn a, Wn b) { return {vsubq_s16(a.lo, b.lo), vsubq_s16(a.hi, b.hi)}; }
inline Wn addc(Wn a, int c) { const int16x8_t k = vdupq_n_s16(int16_t(c)); return {vaddq_s16(a.lo, k), vaddq_s16(a.hi, k)}; }
template <int S> inline Wn shl(Wn a) { return {vshlq_n_s16(a.lo, S), vshlq_n_s16(a.hi, S)}; }
template <int S> inline Wn sra(Wn a) { return {vshrq_n_s16(a.lo, S), vshrq_n_s16(a.hi, S)}; }
inline Wn clampw(Wn a, Wn lo, Wn hi) { return {vminq_s16(vmaxq_s16(a.lo, lo.lo), hi.lo), vminq_s16(vmaxq_s16(a.hi, lo.hi), hi.hi)}; }
inline Wn neg(Wn a) { return {vnegq_s16(a.lo), vnegq_s16(a.hi)}; }
inline uint8x16_t absLess(uint8x16_t a, uint8x16_t b, uint8x16_t t) { return vcltq_u8(vabdq_u8(a, b), t); }
inline Wn maskWiden(uint8x16_t m) // all-ones / zero bytes -> -1 / 0 per int16
{
    const int8x16_t s = vreinterpretq_s8_u8(m);
    return {vmovl_s8(vget_low_s8(s)), vmovl_high_s8(s)};
}

// Filters 16 lanes; v[0..7] = p3 p2 p1 p0 q0 q1 q2 q3, tc0 per lane (int8, -1 = not filtered).
inline bool lumaCoreN(uint8x16_t* v, const DeblockEdge& e, uint8x16_t tc0v)
{
    const uint8x16_t p2 = v[1], p1 = v[2], p0 = v[3], q0 = v[4], q1 = v[5], q2 = v[6];
    const uint8x16_t alpha = vdupq_n_u8(uint8_t(e.alpha)), beta = vdupq_n_u8(uint8_t(e.beta));
    uint8x16_t mask = vandq_u8(absLess(p0, q0, alpha), vandq_u8(absLess(p1, p0, beta), absLess(q1, q0, beta)));
    if (!e.bs4)
        mask = vbicq_u8(mask, vceqq_u8(tc0v, vdupq_n_u8(0xff)));
    if (vmaxvq_u8(mask) == 0)
        return false;
    const uint8x16_t ap = absLess(p2, p0, beta), aq = absLess(q2, q0, beta);
    const Wn P1 = widen(p1), P0 = widen(p0), Q0 = widen(q0), Q1 = widen(q1), P2 = widen(p2), Q2 = widen(q2);
    if (!e.bs4) {
        const Wn tc0 = widen(tc0v); // masked lanes don't matter
        const Wn tc = sub(sub(tc0, maskWiden(ap)), maskWiden(aq));
        const Wn delta = clampw(sra<3>(addc(add(shl<2>(sub(Q0, P0)), sub(P1, Q1)), 4)), neg(tc), tc);
        v[3] = vbslq_u8(mask, narrow(add(P0, delta)), p0);
        v[4] = vbslq_u8(mask, narrow(sub(Q0, delta)), q0);
        const Wn avg = sra<1>(addc(add(P0, Q0), 1));
        const Wn nt = neg(tc0);
        const Wn np1 = add(P1, clampw(sra<1>(sub(add(P2, avg), shl<1>(P1))), nt, tc0));
        const Wn nq1 = add(Q1, clampw(sra<1>(sub(add(Q2, avg), shl<1>(Q1))), nt, tc0));
        v[2] = vbslq_u8(vandq_u8(mask, ap), narrow(np1), p1);
        v[5] = vbslq_u8(vandq_u8(mask, aq), narrow(nq1), q1);
        return true;
    }
    const Wn P3 = widen(v[0]), Q3 = widen(v[7]);
    const uint8x16_t strong = absLess(p0, q0, vdupq_n_u8(uint8_t((e.alpha >> 2) + 2)));
    const uint8x16_t sp = vandq_u8(mask, vandq_u8(ap, strong));
    const uint8x16_t sq = vandq_u8(mask, vandq_u8(aq, strong));
    const Wn p0q0 = add(P0, Q0);
    const uint8x16_t wp0 = narrow(sra<2>(addc(add(add(shl<1>(P1), P0), Q1), 2)));
    const uint8x16_t wq0 = narrow(sra<2>(addc(add(add(shl<1>(Q1), Q0), P1), 2)));
    const uint8x16_t sp0 = narrow(sra<3>(addc(add(add(P2, shl<1>(add(P1, p0q0))), Q1), 4)));
    const uint8x16_t sp1 = narrow(sra<2>(addc(add(add(P2, P1), p0q0), 2)));
    const uint8x16_t sp2 = narrow(sra<3>(addc(add(add(shl<1>(P3), add(shl<1>(P2), P2)), add(P1, p0q0)), 4)));
    const uint8x16_t sq0 = narrow(sra<3>(addc(add(add(Q2, shl<1>(add(Q1, p0q0))), P1), 4)));
    const uint8x16_t sq1 = narrow(sra<2>(addc(add(add(Q2, Q1), p0q0), 2)));
    const uint8x16_t sq2 = narrow(sra<3>(addc(add(add(shl<1>(Q3), add(shl<1>(Q2), Q2)), add(Q1, p0q0)), 4)));
    v[3] = vbslq_u8(mask, vbslq_u8(sp, sp0, wp0), p0);
    v[2] = vbslq_u8(sp, sp1, p1);
    v[1] = vbslq_u8(sp, sp2, p2);
    v[4] = vbslq_u8(mask, vbslq_u8(sq, sq0, wq0), q0);
    v[5] = vbslq_u8(sq, sq1, q1);
    v[6] = vbslq_u8(sq, sq2, q2);
    return true;
}

// v[0..3] = p1 p0 q0 q1.
inline bool chromaCoreN(uint8x16_t* v, const DeblockEdge& e, uint8x16_t tc0v)
{
    const uint8x16_t p1 = v[0], p0 = v[1], q0 = v[2], q1 = v[3];
    const uint8x16_t alpha = vdupq_n_u8(uint8_t(e.alpha)), beta = vdupq_n_u8(uint8_t(e.beta));
    uint8x16_t mask = vandq_u8(absLess(p0, q0, alpha), vandq_u8(absLess(p1, p0, beta), absLess(q1, q0, beta)));
    if (!e.bs4)
        mask = vbicq_u8(mask, vceqq_u8(tc0v, vdupq_n_u8(0xff)));
    if (vmaxvq_u8(mask) == 0)
        return false;
    const Wn P1 = widen(p1), P0 = widen(p0), Q0 = widen(q0), Q1 = widen(q1);
    if (!e.bs4) {
        const Wn tc = addc(widen(tc0v), 1);
        const Wn delta = clampw(sra<3>(addc(add(shl<2>(sub(Q0, P0)), sub(P1, Q1)), 4)), neg(tc), tc);
        v[1] = vbslq_u8(mask, narrow(add(P0, delta)), p0);
        v[2] = vbslq_u8(mask, narrow(sub(Q0, delta)), q0);
        return true;
    }
    v[1] = vbslq_u8(mask, narrow(sra<2>(addc(add(add(shl<1>(P1), P0), Q1), 2))), p0);
    v[2] = vbslq_u8(mask, narrow(sra<2>(addc(add(add(shl<1>(Q1), Q0), P1), 2))), q0);
    return true;
}

// tc0 per lane: luma lane i -> segment i / 4, chroma lane i (Cb 0..7, Cr 8..15) -> segment (i & 7) / 2.
inline uint8x16_t lumaTc0N(const DeblockEdge& e)
{
    uint8_t t[16];
    for (int i = 0; i < 16; ++i)
        t[i] = uint8_t(e.tc0[i >> 2]);
    return vld1q_u8(t);
}
inline uint8x16_t chromaTc0N(const DeblockEdge& e)
{
    uint8_t t[16];
    for (int i = 0; i < 16; ++i)
        t[i] = uint8_t(e.tc0[(i & 7) >> 1]);
    return vld1q_u8(t);
}

// Transposes two 8 x 8 byte matrices at once (low and high halves of x[0..7]); its own inverse.
inline void transpose8x8x2(uint8x16_t* x)
{
    uint8x16_t a[8], b[8];
    for (int i = 0; i < 8; i += 2) {
        a[i] = vtrn1q_u8(x[i], x[i + 1]);
        a[i + 1] = vtrn2q_u8(x[i], x[i + 1]);
    }
    auto u16 = [](uint8x16_t v) { return vreinterpretq_u16_u8(v); };
    auto u32 = [](uint8x16_t v) { return vreinterpretq_u32_u8(v); };
    for (int i = 0; i < 8; i += 4)
        for (int k = 0; k < 2; ++k) {
            b[i + k] = vreinterpretq_u8_u16(vtrn1q_u16(u16(a[i + k]), u16(a[i + 2 + k])));
            b[i + 2 + k] = vreinterpretq_u8_u16(vtrn2q_u16(u16(a[i + k]), u16(a[i + 2 + k])));
        }
    for (int k = 0; k < 4; ++k) {
        x[k] = vreinterpretq_u8_u32(vtrn1q_u32(u32(b[k]), u32(b[4 + k])));
        x[4 + k] = vreinterpretq_u8_u32(vtrn2q_u32(u32(b[k]), u32(b[4 + k])));
    }
}
#endif

} // namespace deblock_detail

// Vertical edge (filtering across columns): q = first line's q0 sample, 16 lines down by stride.
inline void deblockLumaEdgeV(uint8_t* q, ptrdiff_t stride, const DeblockEdge& e)
{
    using namespace deblock_detail;
#ifdef SCHNEIDI_H264_DEBLOCK_SSE2
    __m128i r[16];
    for (int i = 0; i < 16; ++i)
        r[i] = ld8(q + i * stride - 4);
    // 16 x 8 -> 8 x 16.
    __m128i a[8], b[8], c[8], v[8];
    for (int i = 0; i < 8; ++i)
        a[i] = _mm_unpacklo_epi8(r[2 * i], r[2 * i + 1]);
    for (int i = 0; i < 4; ++i) {
        b[i] = _mm_unpacklo_epi16(a[2 * i], a[2 * i + 1]);     // columns 0..3, rows 4i..4i+3
        b[4 + i] = _mm_unpackhi_epi16(a[2 * i], a[2 * i + 1]); // columns 4..7
    }
    for (int h = 0; h < 2; ++h) { // h: column half
        for (int g = 0; g < 2; ++g) { // g: rows 0..7 / 8..15
            c[h * 4 + g * 2] = _mm_unpacklo_epi32(b[h * 4 + g * 2], b[h * 4 + g * 2 + 1]);     // cols 0,1 (+4h)
            c[h * 4 + g * 2 + 1] = _mm_unpackhi_epi32(b[h * 4 + g * 2], b[h * 4 + g * 2 + 1]); // cols 2,3
        }
        v[h * 4 + 0] = _mm_unpacklo_epi64(c[h * 4 + 0], c[h * 4 + 2]);
        v[h * 4 + 1] = _mm_unpackhi_epi64(c[h * 4 + 0], c[h * 4 + 2]);
        v[h * 4 + 2] = _mm_unpacklo_epi64(c[h * 4 + 1], c[h * 4 + 3]);
        v[h * 4 + 3] = _mm_unpackhi_epi64(c[h * 4 + 1], c[h * 4 + 3]);
    }
    if (!lumaCore(v, e, lumaTc0(e)))
        return; // no line filtered
    // 8 x 16 -> 16 x 8.
    __m128i el[4], eh[4];
    for (int i = 0; i < 4; ++i) {
        el[i] = _mm_unpacklo_epi8(v[2 * i], v[2 * i + 1]); // rows 0..7
        eh[i] = _mm_unpackhi_epi8(v[2 * i], v[2 * i + 1]); // rows 8..15
    }
    for (int g = 0; g < 2; ++g) {
        const __m128i* e0 = g ? eh : el;
        const __m128i f0 = _mm_unpacklo_epi16(e0[0], e0[1]), f1 = _mm_unpackhi_epi16(e0[0], e0[1]); // cols 0..3
        const __m128i f2 = _mm_unpacklo_epi16(e0[2], e0[3]), f3 = _mm_unpackhi_epi16(e0[2], e0[3]); // cols 4..7
        const __m128i g0 = _mm_unpacklo_epi32(f0, f2), g1 = _mm_unpackhi_epi32(f0, f2); // rows 0,1 / 2,3
        const __m128i g2 = _mm_unpacklo_epi32(f1, f3), g3 = _mm_unpackhi_epi32(f1, f3); // rows 4,5 / 6,7
        uint8_t* o = q + (g * 8) * stride - 4;
        st8(o, g0);
        st8(o + stride, _mm_unpackhi_epi64(g0, g0));
        st8(o + 2 * stride, g1);
        st8(o + 3 * stride, _mm_unpackhi_epi64(g1, g1));
        st8(o + 4 * stride, g2);
        st8(o + 5 * stride, _mm_unpackhi_epi64(g2, g2));
        st8(o + 6 * stride, g3);
        st8(o + 7 * stride, _mm_unpackhi_epi64(g3, g3));
    }
#elif defined(SCHNEIDI_H264_DEBLOCK_NEON)
    uint8x16_t v[8]; // lines i and i + 8 in the halves, then transposed: v[k] = column k (lane = line)
    for (int i = 0; i < 8; ++i)
        v[i] = vcombine_u8(vld1_u8(q + i * stride - 4), vld1_u8(q + (i + 8) * stride - 4));
    transpose8x8x2(v);
    if (!lumaCoreN(v, e, lumaTc0N(e)))
        return;
    transpose8x8x2(v);
    for (int i = 0; i < 8; ++i) {
        vst1_u8(q + i * stride - 4, vget_low_u8(v[i]));
        vst1_u8(q + (i + 8) * stride - 4, vget_high_u8(v[i]));
    }
#else
    for (int i = 0; i < 16; ++i)
        if (e.bs4 || e.tc0[i >> 2] >= 0)
            lumaLine(q + i * stride, 1, e.alpha, e.beta, e.tc0[i >> 2], e.bs4);
#endif
}

// Horizontal edge (filtering across rows): q = q0 row, 16 samples to the right.
inline void deblockLumaEdgeH(uint8_t* q, ptrdiff_t stride, const DeblockEdge& e)
{
    using namespace deblock_detail;
#ifdef SCHNEIDI_H264_DEBLOCK_SSE2
    __m128i v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q + (i - 4) * stride));
    if (!lumaCore(v, e, lumaTc0(e)))
        return;
    for (int i = e.bs4 ? 1 : 2; i < (e.bs4 ? 7 : 6); ++i)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(q + (i - 4) * stride), v[i]);
#elif defined(SCHNEIDI_H264_DEBLOCK_NEON)
    uint8x16_t v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = vld1q_u8(q + (i - 4) * stride);
    if (!lumaCoreN(v, e, lumaTc0N(e)))
        return;
    for (int i = e.bs4 ? 1 : 2; i < (e.bs4 ? 7 : 6); ++i)
        vst1q_u8(q + (i - 4) * stride, v[i]);
#else
    for (int i = 0; i < 16; ++i)
        if (e.bs4 || e.tc0[i >> 2] >= 0)
            lumaLine(q + i, stride, e.alpha, e.beta, e.tc0[i >> 2], e.bs4);
#endif
}

// Chroma (4:2:0): the same edge of Cb and Cr, 8 lines each.
inline void deblockChromaEdgeV(uint8_t* qu, uint8_t* qv, ptrdiff_t stride, const DeblockEdge& e)
{
    using namespace deblock_detail;
#ifdef SCHNEIDI_H264_DEBLOCK_SSE2
    __m128i a[8];
    for (int i = 0; i < 8; ++i) {
        const uint8_t* p0 = i < 4 ? qu + (2 * i) * stride : qv + (2 * i - 8) * stride;
        a[i] = _mm_unpacklo_epi8(ld4(p0 - 2), ld4(p0 + stride - 2));
    }
    __m128i b[4];
    for (int i = 0; i < 4; ++i)
        b[i] = _mm_unpacklo_epi16(a[2 * i], a[2 * i + 1]); // cols 0..3 of lines 4i..4i+3
    const __m128i c0 = _mm_unpacklo_epi32(b[0], b[1]), c1 = _mm_unpackhi_epi32(b[0], b[1]);
    const __m128i c2 = _mm_unpacklo_epi32(b[2], b[3]), c3 = _mm_unpackhi_epi32(b[2], b[3]);
    __m128i v[4] = {_mm_unpacklo_epi64(c0, c2), _mm_unpackhi_epi64(c0, c2), _mm_unpacklo_epi64(c1, c3),
                    _mm_unpackhi_epi64(c1, c3)};
    if (!chromaCore(v, e, chromaTc0(e)))
        return;
    const __m128i el0 = _mm_unpacklo_epi8(v[0], v[1]), eh0 = _mm_unpackhi_epi8(v[0], v[1]);
    const __m128i el1 = _mm_unpacklo_epi8(v[2], v[3]), eh1 = _mm_unpackhi_epi8(v[2], v[3]);
    const __m128i f[4] = {_mm_unpacklo_epi16(el0, el1), _mm_unpackhi_epi16(el0, el1), _mm_unpacklo_epi16(eh0, eh1),
                          _mm_unpackhi_epi16(eh0, eh1)}; // lines 0..3, 4..7, 8..11, 12..15
    for (int i = 0; i < 4; ++i) {
        uint8_t* base = i < 2 ? qu + (4 * i) * stride : qv + (4 * i - 8) * stride;
        st4(base - 2, f[i]);
        st4(base + stride - 2, _mm_shuffle_epi32(f[i], 1));
        st4(base + 2 * stride - 2, _mm_shuffle_epi32(f[i], 2));
        st4(base + 3 * stride - 2, _mm_shuffle_epi32(f[i], 3));
    }
#elif defined(SCHNEIDI_H264_DEBLOCK_NEON)
    // The 16 lines' four samples side by side, split into p1 / p0 / q0 / q1 by the interleaved load.
    uint8_t buf[64];
    for (int i = 0; i < 16; ++i)
        std::memcpy(buf + 4 * i, (i < 8 ? qu + i * stride : qv + (i - 8) * stride) - 2, 4);
    uint8x16x4_t t = vld4q_u8(buf);
    if (!chromaCoreN(t.val, e, chromaTc0N(e)))
        return;
    vst4q_u8(buf, t);
    for (int i = 0; i < 16; ++i)
        std::memcpy((i < 8 ? qu + i * stride : qv + (i - 8) * stride) - 2, buf + 4 * i, 4);
#else
    for (int i = 0; i < 8; ++i)
        if (e.bs4 || e.tc0[i >> 1] >= 0) {
            chromaLine(qu + i * stride, 1, e.alpha, e.beta, e.tc0[i >> 1], e.bs4);
            chromaLine(qv + i * stride, 1, e.alpha, e.beta, e.tc0[i >> 1], e.bs4);
        }
#endif
}

inline void deblockChromaEdgeH(uint8_t* qu, uint8_t* qv, ptrdiff_t stride, const DeblockEdge& e)
{
    using namespace deblock_detail;
#ifdef SCHNEIDI_H264_DEBLOCK_SSE2
    __m128i v[4];
    for (int i = 0; i < 4; ++i)
        v[i] = _mm_unpacklo_epi64(ld8(qu + (i - 2) * stride), ld8(qv + (i - 2) * stride));
    if (!chromaCore(v, e, chromaTc0(e)))
        return;
    for (int i = 1; i < 3; ++i) {
        st8(qu + (i - 2) * stride, v[i]);
        st8(qv + (i - 2) * stride, _mm_unpackhi_epi64(v[i], v[i]));
    }
#elif defined(SCHNEIDI_H264_DEBLOCK_NEON)
    uint8x16_t v[4];
    for (int i = 0; i < 4; ++i)
        v[i] = vcombine_u8(vld1_u8(qu + (i - 2) * stride), vld1_u8(qv + (i - 2) * stride));
    if (!chromaCoreN(v, e, chromaTc0N(e)))
        return;
    for (int i = 1; i < 3; ++i) {
        vst1_u8(qu + (i - 2) * stride, vget_low_u8(v[i]));
        vst1_u8(qv + (i - 2) * stride, vget_high_u8(v[i]));
    }
#else
    for (int i = 0; i < 8; ++i)
        if (e.bs4 || e.tc0[i >> 1] >= 0) {
            chromaLine(qu + i, stride, e.alpha, e.beta, e.tc0[i >> 1], e.bs4);
            chromaLine(qv + i, stride, e.alpha, e.beta, e.tc0[i >> 1], e.bs4);
        }
#endif
}

} // namespace schneidi::h264
