// Lookahead analysis at half resolution: per macroblock the SATD of the best of three intra
// predictions and of a small motion search (full-pel diamond on SAD, half-pel on SATD) against
// the previous picture, plus the macroblock tree that propagates these costs backwards.
#include "Simd.h" // first: no floating-point contraction (see there)
#include "Lookahead.h"

#include "Satd.h"
#include "ThreadErrors.h"

#include <climits>
#include <cstdlib>
#include <algorithm>
#include <bit>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace schneidi::h264 {

namespace {

constexpr int kMargin = 32; // replicated border of the half picture = the search range in half-res pixels

int seBits(int v)
{
    const uint32_t k = v > 0 ? uint32_t(2 * v - 1) : uint32_t(-2 * v);
    int len = 0;
    while (((k + 1) >> len) > 1)
        ++len;
    return 2 * len + 1;
}

int median3(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

} // namespace

Lookahead::Lookahead(int width, int height, int mbW, int mbH)
    : width_(width), height_(height), mbW_(mbW), mbH_(mbH), halfW_(mbW * 8), halfH_(mbH * 8)
{
}

void Lookahead::analyse(LookaheadFrame& f, const LookaheadFrame* prev) const
{
    prepare(f);
    search(f, prev);
}

void Lookahead::prepare(LookaheadFrame& f) const
{
    const int stride = halfW_ + 2 * kMargin;
    const int rows = halfH_ + 2 * kMargin;
    f.half.assign(size_t(stride) * rows, 0);
    // 2x2 average with edge replication beyond the picture, then the margin.
    for (int y = 0; y < halfH_; ++y) {
        const int sy0 = std::min(2 * y, height_ - 1), sy1 = std::min(2 * y + 1, height_ - 1);
        const uint8_t* r0 = f.y.data() + size_t(sy0) * width_;
        const uint8_t* r1 = f.y.data() + size_t(sy1) * width_;
        uint8_t* out = f.half.data() + size_t(y + kMargin) * stride + kMargin;
        const int inside = std::min(halfW_, width_ / 2); // both source columns inside the picture
        int x = 0;
#ifdef SCHNEIDI_H264_SSE2
        // Eight averages per step: even and odd samples of both rows as 16-bit sums (same numbers).
        const __m128i lowByte = _mm_set1_epi16(0x00ff), two = _mm_set1_epi16(2);
        for (; x + 8 <= inside; x += 8) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + 2 * x));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + 2 * x));
            const __m128i sum = _mm_add_epi16(_mm_add_epi16(_mm_and_si128(a, lowByte), _mm_srli_epi16(a, 8)),
                                              _mm_add_epi16(_mm_and_si128(b, lowByte), _mm_srli_epi16(b, 8)));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(out + x),
                             _mm_packus_epi16(_mm_srli_epi16(_mm_add_epi16(sum, two), 2), _mm_setzero_si128()));
        }
#elif defined(SCHNEIDI_H264_NEON)
        for (; x + 8 <= inside; x += 8) // pairwise sums of both rows, (sum + 2) >> 2
            vst1_u8(out + x, vrshrn_n_u16(vaddq_u16(vpaddlq_u8(vld1q_u8(r0 + 2 * x)), vpaddlq_u8(vld1q_u8(r1 + 2 * x))), 2));
#endif
        for (; x < inside; ++x)
            out[x] = uint8_t((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
        for (int x = inside; x < halfW_; ++x) {
            const int sx0 = std::min(2 * x, width_ - 1), sx1 = std::min(2 * x + 1, width_ - 1);
            out[x] = uint8_t((r0[sx0] + r0[sx1] + r1[sx0] + r1[sx1] + 2) >> 2);
        }
    }
    for (int y = 0; y < halfH_; ++y) {
        uint8_t* row = f.half.data() + size_t(y + kMargin) * stride;
        std::fill(row, row + kMargin, row[kMargin]);
        std::fill(row + kMargin + halfW_, row + stride, row[kMargin + halfW_ - 1]);
    }
    for (int y = 0; y < kMargin; ++y) {
        std::copy_n(f.half.data() + size_t(kMargin) * stride, stride, f.half.data() + size_t(y) * stride);
        std::copy_n(f.half.data() + size_t(kMargin + halfH_ - 1) * stride, stride,
                    f.half.data() + size_t(kMargin + halfH_ + y) * stride);
    }

    // Scene change: on quarter-size thumbnails, compare the best small-motion SAD of each 8x8
    // block with its SAD against its own mean (a crude intra cost). When motion no longer helps
    // for most of the picture, a keyframe is cheaper and looks better than a P frame.
    {
        const int tw = mbW_ * 4, th = mbH_ * 4;
        f.thumb.assign(size_t(tw) * th, 0);
        const int inside = std::min(tw, width_ / 4); // all four source columns inside the picture
        for (int y = 0; y < th; ++y) {
            const uint8_t* rows[4];
            for (int dy = 0; dy < 4; ++dy)
                rows[dy] = f.y.data() + size_t(std::min(y * 4 + dy, height_ - 1)) * width_;
            int x = 0;
#ifdef SCHNEIDI_H264_SSE2
            // Four thumbnail samples per step: 16-bit sums of the four rows, then of four neighbours.
            const __m128i lowByte = _mm_set1_epi16(0x00ff), ones = _mm_set1_epi16(1), eight = _mm_set1_epi32(8);
            for (; x + 4 <= inside; x += 4) {
                __m128i sum = _mm_setzero_si128();
                for (int dy = 0; dy < 4; ++dy) {
                    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rows[dy] + 4 * x));
                    sum = _mm_add_epi16(sum, _mm_add_epi16(_mm_and_si128(a, lowByte), _mm_srli_epi16(a, 8)));
                }
                const __m128i four = _mm_srli_epi32(_mm_add_epi32(_mm_madd_epi16(sum, ones), eight), 4); // pairs of pairs
                const __m128i bytes = _mm_packus_epi16(_mm_packs_epi32(four, four), four);
                const int32_t v = _mm_cvtsi128_si32(bytes);
                std::memcpy(&f.thumb[size_t(y) * tw + x], &v, 4);
            }
#elif defined(SCHNEIDI_H264_NEON)
            for (; x + 4 <= inside; x += 4) {
                uint16x8_t sum = vdupq_n_u16(0);
                for (int dy = 0; dy < 4; ++dy)
                    sum = vpadalq_u8(sum, vld1q_u8(rows[dy] + 4 * x));
                const uint16x4_t four = vrshrn_n_u32(vpaddlq_u16(sum), 4); // (sum + 8) >> 4
                const uint32_t v = vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(four, four))), 0);
                std::memcpy(&f.thumb[size_t(y) * tw + x], &v, 4);
            }
#endif
            for (; x < inside; ++x) {
                int sum = 0;
                for (int dy = 0; dy < 4; ++dy)
                    sum += rows[dy][4 * x] + rows[dy][4 * x + 1] + rows[dy][4 * x + 2] + rows[dy][4 * x + 3];
                f.thumb[size_t(y) * tw + x] = uint8_t((sum + 8) >> 4);
            }
            for (int x = inside; x < tw; ++x) {
                int sum = 0;
                for (int dy = 0; dy < 4; ++dy) {
                    const uint8_t* row = f.y.data() + size_t(std::min(y * 4 + dy, height_ - 1)) * width_;
                    for (int dx = 0; dx < 4; ++dx)
                        sum += row[std::min(x * 4 + dx, width_ - 1)];
                }
                f.thumb[size_t(y) * tw + x] = uint8_t((sum + 8) >> 4);
            }
        }
    }
}

void Lookahead::search(LookaheadFrame& f, const LookaheadFrame* prev) const
{
    sceneCut(f, prev);
    costs(f, prev);
}

void Lookahead::sceneCut(LookaheadFrame& f, const LookaheadFrame* prev) const
{
    {
        const int tw = mbW_ * 4, th = mbH_ * 4;
        f.sceneChange = false;
        constexpr int kRange = 8; // thumbnail pixels = 32 picture pixels
        if (prev && prev->thumb.size() == f.thumb.size()) {
            const std::vector<uint8_t>& thumb = f.thumb;
            const std::vector<uint8_t>& prevThumb = prev->thumb;
            int64_t interCost = 0, intraCost = 0;
            // Sums of every 8x8 block of the previous thumbnail (integral image): a block's SAD is at
            // least the difference of the two sums, so most of the 289 positions need no SAD at all
            // (same minimum as trying every one).
            const int iw = tw + 1;
            std::vector<uint32_t> integral(size_t(iw) * (th + 1), 0);
            for (int y = 0; y < th; ++y) {
                uint32_t row = 0;
                for (int x = 0; x < tw; ++x) {
                    row += prevThumb[size_t(y) * tw + x];
                    integral[size_t(y + 1) * iw + x + 1] = integral[size_t(y) * iw + x + 1] + row;
                }
            }
            auto prevSum = [&](int x, int y) {
                return int(integral[size_t(y + 8) * iw + x + 8] - integral[size_t(y) * iw + x + 8] - integral[size_t(y + 8) * iw + x] +
                           integral[size_t(y) * iw + x]);
            };
            // Only the comparison at the end is needed: every block's sum and intra cost first, then
            // the motion search stops as soon as the blocks left cannot change the answer (the inter
            // cost only grows, by at most the intra cost per block).
            const int blocksX = tw / 8, blocksY = th / 8;
            std::vector<int> sums(size_t(blocksX) * blocksY), intras(size_t(blocksX) * blocksY);
            int64_t intraLeft = 0;
            for (int by = 0; by + 8 <= th; by += 8)
                for (int bx = 0; bx + 8 <= tw; bx += 8) {
                    int sum = 0;
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x)
                            sum += thumb[size_t(by + y) * tw + bx + x];
                    const int mean = (sum + 32) >> 6;
                    int intra = 0;
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x)
                            intra += std::abs(thumb[size_t(by + y) * tw + bx + x] - mean);
                    const size_t b = size_t(by / 8) * blocksX + bx / 8;
                    sums[b] = sum;
                    intras[b] = intra;
                    intraLeft += intra;
                    intraCost += intra + 16; // a little bias so flat pictures do not count as cuts
                }
            bool decided = false;
            for (int by = 0; by + 8 <= th && !decided; by += 8)
                for (int bx = 0; bx + 8 <= tw; bx += 8) {
                    const size_t b = size_t(by / 8) * blocksX + bx / 8;
                    const int sum = sums[b], intra = intras[b];
                    // The same place first: usually close to the best, so the bound below bites early.
                    int best = sadBlock(&thumb[size_t(by) * tw + bx], tw, &prevThumb[size_t(by) * tw + bx], tw, 8, 8);
                    const uint8_t* cur = &thumb[size_t(by) * tw + bx];
                    const int mx0 = std::max(-kRange, -bx), mx1 = std::min(kRange, tw - 8 - bx); // inclusive
                    for (int i = 0; i <= 2 * kRange && best > 0; ++i) {
                        const int my = (i & 1) ? -((i + 1) / 2) : i / 2; // nearest rows first: the bound bites sooner
                        if (by + my < 0 || by + my + 8 > th)
                            continue;
                        const uint8_t* ref = &prevThumb[size_t(by + my) * tw + bx];
                        int mx = mx0;
#ifdef SCHNEIDI_H264_SSE2
                        // The sum bound four positions at a time (with the best SAD at the start of
                        // the four: maybe a SAD more than one by one, the same minimum).
                        {
                            const uint32_t* top = &integral[size_t(by + my) * iw + bx];
                            const uint32_t* bottom = &integral[size_t(by + my + 8) * iw + bx];
                            const __m128i vsum = _mm_set1_epi32(sum);
                            auto ld = [](const uint32_t* p) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p)); };
                            for (; mx + 3 <= mx1; mx += 4) {
                                const __m128i ps = _mm_add_epi32(_mm_sub_epi32(ld(bottom + mx + 8), ld(top + mx + 8)),
                                                                 _mm_sub_epi32(ld(top + mx), ld(bottom + mx)));
                                const __m128i d = _mm_sub_epi32(vsum, ps);
                                const __m128i sign = _mm_srai_epi32(d, 31);
                                const __m128i ad = _mm_sub_epi32(_mm_xor_si128(d, sign), sign);
                                int m = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmplt_epi32(ad, _mm_set1_epi32(best))));
                                for (; m; m &= m - 1) {
                                    const int k = mx + std::countr_zero(unsigned(m));
                                    best = std::min(best, sadBlock(cur, tw, ref + k, tw, 8, 8));
                                }
                            }
                        }
#elif defined(SCHNEIDI_H264_NEON)
                        {
                            const uint32_t* top = &integral[size_t(by + my) * iw + bx];
                            const uint32_t* bottom = &integral[size_t(by + my + 8) * iw + bx];
                            const int32x4_t vsum = vdupq_n_s32(sum);
                            auto ld = [](const uint32_t* p) { return vreinterpretq_s32_u32(vld1q_u32(p)); };
                            static const uint32x4_t kBits = {1, 2, 4, 8};
                            for (; mx + 3 <= mx1; mx += 4) {
                                const int32x4_t ps = vaddq_s32(vsubq_s32(ld(bottom + mx + 8), ld(top + mx + 8)),
                                                               vsubq_s32(ld(top + mx), ld(bottom + mx)));
                                const uint32x4_t lt = vcltq_s32(vabsq_s32(vsubq_s32(vsum, ps)), vdupq_n_s32(best));
                                unsigned m = vaddvq_u32(vandq_u32(lt, kBits));
                                for (; m; m &= m - 1) {
                                    const int k = mx + std::countr_zero(m);
                                    best = std::min(best, sadBlock(cur, tw, ref + k, tw, 8, 8));
                                }
                            }
                        }
#endif
                        for (; mx <= mx1; ++mx) {
                            if (std::abs(sum - prevSum(bx + mx, by + my)) >= best)
                                continue;
                            best = std::min(best, sadBlock(cur, tw, ref + mx, tw, 8, 8));
                        }
                    }
                    interCost += std::min(best, intra);
                    intraLeft -= intra;
                    if (interCost * 100 > intraCost * 85 || (interCost + intraLeft) * 100 <= intraCost * 85) {
                        decided = true;
                        break;
                    }
                }
            // Real cuts measured ~0.92-0.95, fast game motion up to ~0.72.
            f.sceneChange = interCost * 100 > intraCost * 85;
        }
    }

}

std::vector<uint8_t> Lookahead::halfPicture(const LookaheadFrame& f) const
{
    const int stride = halfW_ + 2 * kMargin;
    std::vector<uint8_t> out(size_t(halfW_) * halfH_);
    for (int y = 0; y < halfH_; ++y)
        std::memcpy(out.data() + size_t(y) * halfW_, f.half.data() + size_t(y + kMargin) * stride + kMargin, size_t(halfW_));
    return out;
}

// H264_LASUBPEL=0: the lookahead's half-pel step measures all eight neighbours.
static const bool gLaSubpel = !std::getenv("H264_LASUBPEL") || std::atoi(std::getenv("H264_LASUBPEL")) != 0;

void Lookahead::costs(LookaheadFrame& f, const LookaheadFrame* prev, int threads, const LookaheadFrame* seed,
                      int seedScale, const LookaheadFrame* src, const std::atomic<int>* prevRows, std::atomic<int>* ownRows,
                      int grid) const
{
    if (!src)
        src = &f;
    const int stride = halfW_ + 2 * kMargin;
    const size_t n = size_t(mbW_) * mbH_;
    f.intraCost.assign(n, 0);
    f.interCost.assign(n, 0);
    f.mvx.assign(n, 0);
    f.mvy.assign(n, 0);
    f.propagate.assign(n, 0.0f);
    f.treeLinks.ready = false;
    f.intraSum = f.interSum = 0;

    auto at = [&](const std::vector<uint8_t>& plane, int x, int y) -> const uint8_t* {
        return plane.data() + size_t(y + kMargin) * stride + size_t(x + kMargin);
    };
    constexpr int kLambda = 1; // SATD per bit of vector difference (the lookahead has no QP)
    constexpr int kIntraPenalty = 8;
    // One macroblock; its vector predictor reads the left, top and top-right vectors.
    auto doMb = [&](int bx, int by, double& intraSum, double& interSum) {
        if (grid > 1 && (bx % grid || by % grid))
            return; // a sample of the blocks (BPlanner): the others keep cost 0
        {
            const size_t m = size_t(by) * mbW_ + bx;
            const uint8_t* cur = at(src->half, bx * 8, by * 8);

            // Intra: DC, vertical and horizontal prediction from the source neighbours (known
            // already when another picture's half picture is searched).
            int intra;
            if (src != &f && src->intraCost.size() == n) {
                intra = src->intraCost[m];
            } else {
                uint8_t pred[64];
                int dc = 0;
                for (int i = 0; i < 8; ++i)
                    dc += cur[-stride + i] + cur[i * stride - 1];
                std::fill(pred, pred + 64, uint8_t((dc + 8) >> 4));
                intra = satd8x8U8Fast(cur, stride, pred, 8);
                for (int y = 0; y < 8; ++y)
                    std::copy_n(cur - stride, 8, pred + y * 8);
                intra = std::min(intra, satd8x8U8Fast(cur, stride, pred, 8));
                for (int y = 0; y < 8; ++y)
                    std::fill(pred + y * 8, pred + y * 8 + 8, cur[y * stride - 1]);
                intra = std::min(intra, satd8x8U8Fast(cur, stride, pred, 8)) + kIntraPenalty;
            }
            f.intraCost[m] = intra;
            intraSum += intra;
            if (!prev) {
                f.interCost[m] = intra;
                interSum += intra;
                return;
            }

            // Motion: predictor = median of the left, top and top-right vectors (half-pel units).
            auto mvOf = [&](int dx, int dy, int& vx, int& vy) {
                const int x = bx + dx * grid, y = by + dy * grid;
                if (x < 0 || y < 0 || x >= mbW_ || y >= mbH_ || (dy == 0 && dx > 0)) {
                    vx = vy = 0;
                    return false;
                }
                vx = f.mvx[size_t(y) * mbW_ + x];
                vy = f.mvy[size_t(y) * mbW_ + x];
                return true;
            };
            int ax, ay, tx, ty, rx, ry;
            const bool hasA = mvOf(-1, 0, ax, ay), hasT = mvOf(0, -1, tx, ty);
            bool hasR = mvOf(1, -1, rx, ry);
            if (!hasR)
                hasR = mvOf(-1, -1, rx, ry);
            int px = median3(ax, tx, rx), py = median3(ay, ty, ry);
            if (hasA + hasT + hasR == 1) {
                px = hasA ? ax : hasT ? tx : rx;
                py = hasA ? ay : hasT ? ty : ry;
            }
            const int limit = 2 * (kMargin - 1);
            auto clampMv = [&](int& vx, int& vy) {
                vx = std::clamp(vx, -limit, limit);
                vy = std::clamp(vy, -limit, limit);
            };
            auto mvCost = [&](int vx, int vy) { return kLambda * (seBits(vx - px) + seBits(vy - py)); };
            auto sadFull = [&](int vx, int vy) { // vx, vy in full pel
                const uint8_t* ref = at(prev->half, bx * 8 + vx, by * 8 + vy);
                return sadBlock(cur, stride, ref, stride, 8, 8) + mvCost(2 * vx, 2 * vy);
            };
            int bestX = 0, bestY = 0;
            int best = sadFull(0, 0);
            auto tryStart = [&](int vx, int vy) {
                vx = (vx + (vx >= 0 ? 1 : -1)) / 2; // nearest full pel
                vy = (vy + (vy >= 0 ? 1 : -1)) / 2;
                vx = std::clamp(vx, -(kMargin - 1), kMargin - 1);
                vy = std::clamp(vy, -(kMargin - 1), kMargin - 1);
                const int c = sadFull(vx, vy);
                if (c < best) {
                    best = c;
                    bestX = vx;
                    bestY = vy;
                }
            };
            tryStart(px, py);
            if (hasA)
                tryStart(ax, ay);
            if (hasT)
                tryStart(tx, ty);
            if (hasR)
                tryStart(rx, ry);
            if (!prev->mvx.empty())
                tryStart(prev->mvx[m], prev->mvy[m]);
            if (seed)
                tryStart(seed->mvx[m] * seedScale, seed->mvy[m] * seedScale);
            for (int step : {4, 2, 1})
                for (int iter = 0; iter < 16; ++iter) {
                    bool moved = false;
                    static constexpr int kDir[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& d : kDir) {
                        const int vx = bestX + d[0] * step, vy = bestY + d[1] * step;
                        if (std::abs(vx) >= kMargin || std::abs(vy) >= kMargin)
                            continue;
                        const int c = sadFull(vx, vy);
                        if (c < best) {
                            best = c;
                            bestX = vx;
                            bestY = vy;
                            moved = true;
                        }
                    }
                    if (!moved)
                        break;
                }
            // Half-pel refinement on SATD (bilinear).
            auto satdHalf = [&](int hx, int hy) { // half-pel units
                const int fx = hx >> 1, fy = hy >> 1;
                const uint8_t* ref = at(prev->half, bx * 8 + fx, by * 8 + fy);
                if (!(hx & 1) && !(hy & 1))
                    return satd8x8U8Fast(cur, stride, ref, stride) + mvCost(hx, hy);
                uint8_t p[64];
                const int ox = hx & 1, oy = hy & 1;
#ifdef SCHNEIDI_H264_SSE2
                const __m128i zero = _mm_setzero_si128(), two = _mm_set1_epi16(2);
                auto load = [&](const uint8_t* q) { return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(q)), zero); };
                for (int y = 0; y < 8; ++y) {
                    const uint8_t* q = ref + y * stride;
                    const __m128i sum = _mm_add_epi16(_mm_add_epi16(load(q), load(q + ox)),
                                                      _mm_add_epi16(load(q + oy * stride), load(q + oy * stride + ox)));
                    const __m128i v = _mm_srli_epi16(_mm_add_epi16(sum, two), 2);
                    _mm_storel_epi64(reinterpret_cast<__m128i*>(p + y * 8), _mm_packus_epi16(v, v));
                }
#elif defined(SCHNEIDI_H264_NEON)
                for (int y = 0; y < 8; ++y) {
                    const uint8_t* q = ref + y * stride;
                    const uint16x8_t sum = vaddq_u16(vaddl_u8(vld1_u8(q), vld1_u8(q + ox)),
                                                     vaddl_u8(vld1_u8(q + oy * stride), vld1_u8(q + oy * stride + ox)));
                    vst1_u8(p + y * 8, vrshrn_n_u16(sum, 2));
                }
#else
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) {
                        const uint8_t* q = ref + y * stride + x;
                        p[y * 8 + x] = uint8_t((q[0] + q[ox] + q[oy * stride] + q[oy * stride + ox] + 2) >> 2);
                    }
#endif
                return satd8x8U8Fast(cur, stride, p, 8) + mvCost(hx, hy);
            };
            int hx = 2 * bestX, hy = 2 * bestY;
            clampMv(hx, hy);
            int bestSatd = satdHalf(hx, hy);
            const int cx = hx, cy = hy;
            if (gLaSubpel) {
                // The four direct neighbours, then the two diagonals beside the best of them (as the
                // encoder's sub-pel step): 6 instead of 8 SATDs.
                int c4[4];
                static constexpr int kD[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (int i = 0; i < 4; ++i) {
                    const int vx = cx + kD[i][0], vy = cy + kD[i][1];
                    c4[i] = std::abs(vx) > limit || std::abs(vy) > limit ? INT_MAX : satdHalf(vx, vy);
                    if (c4[i] < bestSatd) {
                        bestSatd = c4[i];
                        hx = vx;
                        hy = vy;
                    }
                }
                const int h = c4[1] < c4[0] ? 1 : -1, v = c4[3] < c4[2] ? 1 : -1;
                const bool horizontal = std::min(c4[0], c4[1]) < std::min(c4[2], c4[3]);
                const int diag[2][2] = {{h, v}, {horizontal ? h : -h, horizontal ? -v : v}};
                for (const auto& d : diag) {
                    const int vx = cx + d[0], vy = cy + d[1];
                    if (std::abs(vx) > limit || std::abs(vy) > limit)
                        continue;
                    const int c = satdHalf(vx, vy);
                    if (c < bestSatd) {
                        bestSatd = c;
                        hx = vx;
                        hy = vy;
                    }
                }
            } else
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy)
                        continue;
                    int vx = cx + dx, vy = cy + dy;
                    if (std::abs(vx) > limit || std::abs(vy) > limit)
                        continue;
                    const int c = satdHalf(vx, vy);
                    if (c < bestSatd) {
                        bestSatd = c;
                        hx = vx;
                        hy = vy;
                    }
                }
            f.mvx[m] = int16_t(hx);
            f.mvy[m] = int16_t(hy);
            f.interCost[m] = bestSatd;
            interSum += std::min(bestSatd, intra);
        }
    };
    // Rows in a wavefront (each two macroblocks behind the row above); the sums are of integers,
    // so adding them row by row gives the same doubles in any order.
    std::vector<double> rowIntra(size_t(mbH_), 0.0), rowInter(size_t(mbH_), 0.0);
    threads = std::clamp(threads, 1, mbH_);
    if (threads == 1) {
        for (int by = 0; by < mbH_; ++by) {
            // Pictures side by side: a row reads prev's vectors of the same row.
            // (Once it has to wait it waits for a few rows more, to go on without waiting again.)
            if (prevRows && prevRows->load(std::memory_order_acquire) <= by) {
                const int need = std::min(by + 4, mbH_);
                for (int v = prevRows->load(std::memory_order_acquire); v < need; v = prevRows->load(std::memory_order_acquire))
                    prevRows->wait(v, std::memory_order_acquire);
            }
            for (int bx = 0; bx < mbW_; ++bx)
                doMb(bx, by, rowIntra[size_t(by)], rowInter[size_t(by)]);
            if (ownRows) {
                ownRows->store(by + 1, std::memory_order_release);
                ownRows->notify_all();
            }
        }
    } else {
        std::unique_ptr<std::atomic<int>[]> done(new std::atomic<int>[size_t(mbH_)]);
        for (int r = 0; r < mbH_; ++r)
            done[size_t(r)].store(0, std::memory_order_relaxed);
        std::atomic<int> nextRow{0};
        auto worker = [&] {
            for (int by; (by = nextRow.fetch_add(1)) < mbH_;)
                for (int bx = 0; bx < mbW_; ++bx) {
                    if (by > 0) {
                        const int need = std::min(bx + 2, mbW_);
                        for (int v = done[size_t(by - 1)].load(std::memory_order_acquire); v < need;
                             v = done[size_t(by - 1)].load(std::memory_order_acquire))
                            done[size_t(by - 1)].wait(v, std::memory_order_acquire);
                    }
                    doMb(bx, by, rowIntra[size_t(by)], rowInter[size_t(by)]);
                    done[size_t(by)].store(bx + 1, std::memory_order_release);
                    done[size_t(by)].notify_all();
                }
        };
        runOnThreads(threads, worker); // rows allocate nothing: only starting a thread can fail
    }
    for (int r = 0; r < mbH_; ++r) {
        f.intraSum += rowIntra[size_t(r)];
        f.interSum += rowInter[size_t(r)];
    }
}

void Lookahead::propagate(std::deque<LookaheadFrame>& frames) const
{
    for (LookaheadFrame& f : frames)
        std::fill(f.propagate.begin(), f.propagate.end(), 0.0f);
    const size_t n = size_t(mbW_) * mbH_;
    for (size_t j = frames.size(); j-- > 1;) {
        LookaheadFrame& f = frames[j];
        if (f.keyframe || f.intraCost.size() != n)
            continue;
        LookaheadFrame::TreeLinks& L = f.treeLinks;
        if (!L.ready) {
            // The parts of the propagation that do not depend on the propagated costs, once per
            // picture instead of once per coded picture it stays in the queue for.
            L.mb.clear();
            L.intra.clear();
            L.diff.clear();
            L.count.clear();
            L.target.clear();
            L.weight.clear();
            for (int by = 0; by < mbH_; ++by)
                for (int bx = 0; bx < mbW_; ++bx) {
                    const size_t m = size_t(by) * mbW_ + bx;
                    const int intra = f.intraCost[m], inter = f.interCost[m];
                    if (intra <= 0 || inter >= intra)
                        continue;
                    // Position in the reference in half-pel units; a block covers 16.
                    const int px = bx * 16 + f.mvx[m], py = by * 16 + f.mvy[m];
                    const int ix = px >> 4, iy = py >> 4; // floor
                    const int fx = px - ix * 16, fy = py - iy * 16;
                    const int w[4] = {(16 - fx) * (16 - fy), fx * (16 - fy), (16 - fx) * fy, fx * fy};
                    uint8_t count = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (!w[k])
                            continue;
                        const int x = ix + (k & 1), y = iy + (k >> 1);
                        if (x < 0 || y < 0 || x >= mbW_ || y >= mbH_)
                            continue;
                        L.target.push_back(int32_t(size_t(y) * mbW_ + x));
                        L.weight.push_back(float(w[k]));
                        ++count;
                    }
                    if (!count)
                        continue;
                    L.mb.push_back(int32_t(m));
                    L.intra.push_back(float(intra));
                    L.diff.push_back(float(intra - inter));
                    L.count.push_back(count);
                }
            L.ready = true;
        }
        // The share of each block's information that came from the reference, spread over the
        // blocks it was predicted from (the same float operations in the same order as before).
        float* refProp = frames[j - 1].propagate.data();
        const float* prop = f.propagate.data();
        const size_t links = L.mb.size();
        for (size_t i = 0, t = 0; i < links; ++i) {
            const float amount = (L.intra[i] + prop[L.mb[i]]) * L.diff[i] / L.intra[i];
            for (const size_t e = t + L.count[i]; t < e; ++t)
                refProp[L.target[t]] += amount * L.weight[t] / 256.0f;
        }
    }
}

void Lookahead::searchInto(const LookaheadFrame& cur, const LookaheadFrame& ref, LookaheadSearch& out,
                           const LookaheadFrame* seed, int seedScale, int threads, int grid) const
{
    LookaheadFrame t;
    costs(t, &ref, threads, seed, seedScale, &cur, nullptr, nullptr, grid);
    out.cost = std::move(t.interCost);
    out.mvx = std::move(t.mvx);
    out.mvy = std::move(t.mvy);
    out.sum = t.interSum;
}

void Lookahead::searchFwd(LookaheadFrame& f, const LookaheadFrame& ref, int d, int maxB, int threads, int grid) const
{
    if (f.fwd.size() != size_t(maxB) + 2)
        f.fwd.assign(size_t(maxB) + 2, LookaheadSearch{});
    // Start: the picture's own one-step vector times the distance.
    searchInto(f, ref, f.fwd[size_t(d)], &f, d, threads, grid);
}

void Lookahead::searchBack(LookaheadFrame& next, const LookaheadFrame& b, const LookaheadFrame& after, int c, int maxB,
                           int threads, int grid) const
{
    if (next.back.size() != size_t(maxB) + 1)
        next.back.assign(size_t(maxB) + 1, LookaheadSearch{});
    // Start: the one-step vector of the picture after b, reversed and times the distance.
    searchInto(b, next, next.back[size_t(c)], &after, -c, threads, grid);
}

namespace {

// 8x8 prediction at a half-pel vector (bilinear, as the lookahead's search).
void predHalf(const uint8_t* ref, int stride, int hx, int hy, uint8_t* out)
{
    const uint8_t* q0 = ref + (hy >> 1) * stride + (hx >> 1);
    const int ox = hx & 1, oy = hy & 1;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const uint8_t* q = q0 + y * stride + x;
            out[y * 8 + x] = uint8_t((q[0] + q[ox] + q[oy * stride] + q[oy * stride + ox] + 2) >> 2);
        }
}

} // namespace

void Lookahead::biCost(LookaheadFrame& next, const LookaheadFrame& b, const LookaheadFrame& r0, int a, int c, int maxB,
                       bool weighted, int grid) const
{
    const int stride = halfW_ + 2 * kMargin;
    const size_t n = size_t(mbW_) * mbH_;
    if (next.bi.size() != size_t(bComboCount(maxB))) {
        next.bi.assign(size_t(bComboCount(maxB)), {});
        next.biSum.assign(size_t(bComboCount(maxB)), -1.0);
    }
    const bool haveL0 = a == 1 || (size_t(a) < b.fwd.size() && b.fwd[size_t(a)].valid());
    if (!haveL0 || size_t(c) >= next.back.size() || !next.back[size_t(c)].valid() || b.intraCost.size() != n)
        return;
    const LookaheadSearch& l1 = next.back[size_t(c)];
    const std::vector<int32_t>& c0v = a == 1 ? b.interCost : b.fwd[size_t(a)].cost;
    const std::vector<int16_t>& mx0 = a == 1 ? b.mvx : b.fwd[size_t(a)].mvx;
    const std::vector<int16_t>& my0 = a == 1 ? b.mvy : b.fwd[size_t(a)].mvy;
    const int w1 = weighted ? 64 * a / (a + c) : 32;
    std::vector<int32_t>& out = next.bi[size_t(bComboIndex(a, c, maxB))];
    out.assign(n, 0);
    double sum = 0;
    for (int by = 0; by < mbH_; by += grid)
        for (int bx = 0; bx < mbW_; bx += grid) {
            const size_t m = size_t(by) * mbW_ + bx;
            const size_t off = size_t(by * 8 + kMargin) * stride + size_t(bx * 8 + kMargin);
            const uint8_t* cur = b.half.data() + off;
            uint8_t p0[64], p1[64], avg[64];
            predHalf(r0.half.data() + off, stride, mx0[m], my0[m], p0);
            predHalf(next.half.data() + off, stride, l1.mvx[m], l1.mvy[m], p1);
            for (int i = 0; i < 64; ++i)
                avg[i] = uint8_t(w1 == 32 ? (p0[i] + p1[i] + 1) >> 1 : (p0[i] * (64 - w1) + p1[i] * w1 + 32) >> 6);
            // The average's SATD plus what the two searches paid for their vectors.
            const int c0 = c0v[m], c1 = l1.cost[m];
            const int s0 = satd8x8U8Fast(cur, stride, p0, 8), s1 = satd8x8U8Fast(cur, stride, p1, 8);
            const int cb = satd8x8U8Fast(cur, stride, avg, 8) + std::max(0, c0 - s0) + std::max(0, c1 - s1);
            out[m] = cb;
            sum += std::min({b.intraCost[m], c0, c1, cb});
        }
    next.biSum[size_t(bComboIndex(a, c, maxB))] = sum;
}

void Lookahead::propagateB(std::deque<LookaheadFrame>& frames, int maxB, bool weighted) const
{
    const size_t n = size_t(mbW_) * mbH_;
    for (LookaheadFrame& f : frames)
        f.propagate.assign(n, 0.0f);
    // Spreads amount over the up to four macroblocks of ref that the block at (bx, by), moved by
    // the half-pel vector (mvx, mvy) of the half picture, overlaps (by area).
    auto add = [&](LookaheadFrame& ref, int bx, int by, int mvx, int mvy, float amount) {
        const int px = bx * 16 + mvx, py = by * 16 + mvy;
        const int ix = px >> 4, iy = py >> 4;
        const int fx = px - ix * 16, fy = py - iy * 16;
        const int w[4] = {(16 - fx) * (16 - fy), fx * (16 - fy), (16 - fx) * fy, fx * fy};
        for (int k = 0; k < 4; ++k) {
            if (!w[k])
                continue;
            const int x = ix + (k & 1), y = iy + (k >> 1);
            if (x < 0 || y < 0 || x >= mbW_ || y >= mbH_)
                continue;
            ref.propagate[size_t(y) * mbW_ + x] += amount * float(w[k]) / 256.0f;
        }
    };
    const int count = int(frames.size());
    // B pictures first: nothing references them, they only pass on their own cost - except the
    // reference B picture of a pyramid, which comes after the others and passes theirs on too.
    for (int pass = 0; pass < 2; ++pass)
    for (int j = 0; j < count; ++j) {
        LookaheadFrame& f = frames[size_t(j)];
        if (f.type != 2 || f.intraCost.size() != n || f.bRef != (pass == 1))
            continue;
        const int a = f.refDist, c = f.nextDist, r = j - a, s = j + c;
        if (s >= count || a < 1 || c < 1 || a + c > maxB + 1)
            continue;
        LookaheadFrame& l1f = frames[size_t(s)];
        if (size_t(c) >= l1f.back.size() || !l1f.back[size_t(c)].valid())
            continue;
        const LookaheadSearch& l1 = l1f.back[size_t(c)];
        static const std::vector<int32_t> kNoBi;
        const size_t combo = size_t(bComboIndex(a, c, maxB));
        const std::vector<int32_t>& bi = combo < l1f.bi.size() ? l1f.bi[combo] : kNoBi;
        const bool haveL0 = a == 1 || (size_t(a) < f.fwd.size() && f.fwd[size_t(a)].valid());
        const std::vector<int32_t>* c0v = !haveL0 ? nullptr : a == 1 ? &f.interCost : &f.fwd[size_t(a)].cost;
        const std::vector<int16_t>* mx0 = !haveL0 ? nullptr : a == 1 ? &f.mvx : &f.fwd[size_t(a)].mvx;
        const std::vector<int16_t>* my0 = !haveL0 ? nullptr : a == 1 ? &f.mvy : &f.fwd[size_t(a)].mvy;
        const float w0 = weighted ? 1.0f - float(64 * a / (a + c)) / 64.0f : 0.5f;
        for (int by = 0; by < mbH_; ++by)
            for (int bx = 0; bx < mbW_; ++bx) {
                const size_t m = size_t(by) * mbW_ + bx;
                const int intra = f.intraCost[m];
                const int c0 = c0v ? (*c0v)[m] : INT32_MAX, c1 = l1.cost[m];
                const int cb = c0v && bi.size() == n ? bi[m] : INT32_MAX;
                const int best = std::min({c0, c1, cb});
                if (intra <= 0 || best >= intra)
                    continue;
                const float amount = (float(intra) + f.propagate[m]) * float(intra - best) / float(intra);
                if (best == cb) {
                    if (r >= 0)
                        add(frames[size_t(r)], bx, by, (*mx0)[m], (*my0)[m], amount * w0);
                    add(l1f, bx, by, l1.mvx[m], l1.mvy[m], amount * (1.0f - w0));
                } else if (best == c0) {
                    if (r >= 0)
                        add(frames[size_t(r)], bx, by, (*mx0)[m], (*my0)[m], amount);
                } else {
                    add(l1f, bx, by, l1.mvx[m], l1.mvy[m], amount);
                }
            }
    }
    // Reference pictures, last first: each passes what it received and its own cost on.
    for (int j = count; j-- > 0;) {
        LookaheadFrame& f = frames[size_t(j)];
        if (f.type == 2 || f.keyframe || f.intraCost.size() != n)
            continue;
        const int d = f.refDist, r = j - d;
        if (r < 0 || d < 1)
            continue;
        if (d > 1 && (size_t(d) >= f.fwd.size() || !f.fwd[size_t(d)].valid()))
            continue;
        const std::vector<int32_t>& cv = d == 1 ? f.interCost : f.fwd[size_t(d)].cost;
        const std::vector<int16_t>& mx = d == 1 ? f.mvx : f.fwd[size_t(d)].mvx;
        const std::vector<int16_t>& my = d == 1 ? f.mvy : f.fwd[size_t(d)].mvy;
        LookaheadFrame& ref = frames[size_t(r)];
        for (int by = 0; by < mbH_; ++by)
            for (int bx = 0; bx < mbW_; ++bx) {
                const size_t m = size_t(by) * mbW_ + bx;
                const int intra = f.intraCost[m], inter = cv[m];
                if (intra <= 0 || inter >= intra)
                    continue;
                const float amount = (float(intra) + f.propagate[m]) * float(intra - inter) / float(intra);
                add(ref, bx, by, mx[m], my[m], amount);
            }
    }
}

// The planner's numbers (BPlanner; H264_BPLAN_* for measurements), fitted on our own material:
// fixed QP, runs of three with the pyramid against none, BD-rate PSNR / SSIM - a slow zoom -53 / -56 %
// at a ratio of 0.73, a pan in a game -1 / -6 % at 0.97, camera footage +0 / -1 % at 1.03, game
// action +8 / +6 % at 1.06.
static double planEnv(const char* name, double def)
{
    const char* e = std::getenv(name);
    return e ? std::atof(e) : def;
}
// Dead zone of a block's cost (SATD of the half picture's 8x8 block).
static const int kPlanDead = int(planEnv("H264_BPLAN_DEAD", 128));
// B pictures from a cost ratio (structure with B pictures / all P pictures) below kPlanOn on,
// until it passes kPlanOff; long runs below kPlanLong.
static const double kPlanOn = planEnv("H264_BPLAN_ON", 1.02), kPlanOff = planEnv("H264_BPLAN_OFF", 1.05);
static const double kPlanLong = planEnv("H264_BPLAN_LONG", 0.85);
static const double kPlanKeep = planEnv("H264_BPLAN_KEEP", 0.5);
// Below this cost per block and picture there is nothing to weigh: B pictures.
static const double kPlanStill = planEnv("H264_BPLAN_STILL", 12);
static const double kPlanFree = planEnv("H264_BPLAN_FREE", 0.25);
// The planner searches every kPlanGrid-th block in both directions: its sums need no more, and
// the searches are its whole cost.
static const int kPlanGrid = std::max(1, int(planEnv("H264_BPLAN_GRID", 2)));

namespace {

// A picture of the planner's window: what the searches read, without the source planes.
LookaheadFrame planCopy(const LookaheadFrame& f)
{
    LookaheadFrame c;
    c.half = f.half;
    c.intraCost = f.intraCost;
    c.interCost = f.interCost;
    c.mvx = f.mvx;
    c.mvy = f.mvy;
    c.intraSum = f.intraSum;
    c.interSum = f.interSum;
    c.sceneChange = f.sceneChange;
    c.forceKeyframe = f.forceKeyframe;
    return c;
}

} // namespace

bool BPlanner::add(const Lookahead& la, const LookaheadFrame& f, int index, int maxB, int threads)
{
    if (maxB <= 0)
        return false;
    // A cut: no picture predicts across it, the window starts again there.
    if (win_.empty() || f.sceneChange || f.forceKeyframe) {
        if (!win_.empty())
            accP_ = accB_ = 0;
        win_.clear();
        win_.push_back(planCopy(f));
        anchor_ = index;
        return false;
    }
    win_.push_back(planCopy(f));
    const int run = std::min(maxB, 3);
    if (int(win_.size()) < run + 2)
        return false;
    evaluate(la, maxB, threads);
    resultStart_ = anchor_ + 1;
    // The run's P picture is the reference before the next run.
    anchor_ = index;
    win_.erase(win_.begin(), win_.end() - 1);
    win_.front().fwd.clear();
    win_.front().back.clear();
    win_.front().bi.clear();
    win_.front().biSum.clear();
    return true;
}

void BPlanner::evaluate(const Lookahead& la, int maxB, int threads)
{
    const int run = int(win_.size()) - 2, p = run + 1; // win_[1 .. run] B pictures, win_[p] the P picture
    const int tableB = std::max(maxB, 3);
    const int mid = run >= 3 ? 1 + run / 2 : 0; // the pyramid's reference B picture
    // Every B picture's references: a pictures before it, c after it.
    struct Place {
        int k, a, c;
    };
    std::vector<Place> places;
    for (int k = 1; k <= run; ++k) {
        const int before = mid && k > mid ? mid : 0, after = mid && k < mid ? mid : p;
        places.push_back({k, k - before, after - k});
    }
    for (LookaheadFrame& f : win_) {
        f.fwd.assign(size_t(tableB) + 2, LookaheadSearch{});
        f.back.assign(size_t(tableB) + 1, LookaheadSearch{});
        f.bi.assign(size_t(bComboCount(tableB)), {});
        f.biSum.assign(size_t(bComboCount(tableB)), -1.0);
    }
    // The searches, side by side: each reads the one-step vectors only.
    struct Job {
        int kind, k, a, c;
    };
    std::vector<Job> jobs;
    jobs.push_back({0, p, p, 0});
    for (const Place& b : places) {
        if (b.a > 1)
            jobs.push_back({0, b.k, b.a, 0});
        jobs.push_back({1, b.k, b.a, b.c});
    }
    {
        std::atomic<int> next{0};
        const int count = int(jobs.size());
        runOnThreads(std::clamp(threads, 1, count), [&] {
            for (int i; (i = next.fetch_add(1)) < count;) {
                const Job& j = jobs[size_t(i)];
                if (j.kind == 0)
                    la.searchFwd(win_[size_t(j.k)], win_[size_t(j.k - j.a)], j.a, tableB, 1, kPlanGrid);
                else
                    la.searchBack(win_[size_t(j.k + j.c)], win_[size_t(j.k)], win_[size_t(j.k) + 1], j.c, tableB, 1, kPlanGrid);
            }
        });
    }
    {
        std::atomic<int> next{0};
        const int count = int(places.size());
        runOnThreads(std::clamp(threads, 1, count), [&] {
            for (int i; (i = next.fetch_add(1)) < count;) {
                const Place& b = places[size_t(i)];
                la.biCost(win_[size_t(b.k + b.c)], win_[size_t(b.k)], win_[size_t(b.k - b.a)], b.a, b.c, tableB, false, kPlanGrid);
            }
        });
    }
    // What each structure costs: per macroblock the best prediction's cost above the dead zone (a
    // block whose error stays below it is skipped or costs next to nothing in either structure).
    const int mbW = la.halfW() / 8, mbH = la.halfH() / 8;
    auto dz = [&](int c) { return double(std::max(0, c - kPlanDead)); };
    double sumP = 0, sumB = 0, sumFar = 0, sumNear = 0; // sumNear: the run's pictures as P pictures
    size_t n = 0; // blocks counted: every kPlanGrid-th in both directions
    const LookaheadFrame& pf = win_[size_t(p)];
    const std::vector<int32_t>& far = pf.fwd[size_t(p)].cost;
    for (int by = 0; by < mbH; by += kPlanGrid)
        for (int bx = 0; bx < mbW; bx += kPlanGrid) {
            const size_t m = size_t(by) * mbW + bx;
            ++n;
            for (int k = 1; k <= p; ++k) {
                const LookaheadFrame& f = win_[size_t(k)];
                const double c = dz(std::min(f.intraCost[m], f.interCost[m]));
                sumP += c;
                if (k < p)
                    sumNear += c;
            }
            const double farCost = dz(std::min(pf.intraCost[m], far[m]));
            sumB += farCost;
            sumFar += farCost;
            for (const Place& b : places) {
                const LookaheadFrame& f = win_[size_t(b.k)];
                const LookaheadFrame& nx = win_[size_t(b.k + b.c)];
                const int c0 = b.a == 1 ? f.interCost[m] : f.fwd[size_t(b.a)].cost[m];
                sumB += dz(std::min({f.intraCost[m], c0, nx.back[size_t(b.c)].cost[m], nx.bi[size_t(bComboIndex(b.a, b.c, tableB))][m]}));
            }
        }
    const double perMb = sumP / (double(n) * p);
    const bool still = perMb < kPlanStill;
    if (!still) {
        // The last windows count too (half as much each step back): single windows scatter.
        accP_ = kPlanKeep * accP_ + sumP;
        accB_ = kPlanKeep * accB_ + sumB;
    }
    const double ratio = accP_ > 0 ? accB_ / accP_ : 1.0;
    const int shortRun = std::min(maxB, 3);
    if (still) {
        // Next to nothing to code (a still picture, a desktop, the player standing): B pictures
        // are skipped there, P pictures refine with the tree picture by picture.
        // (Still where B pictures were a clear gain: no P pictures between for the warm-up.)
        gate_ = uint8_t(shortRun | (gate_ & kClear));
    } else {
        on_ = ratio < (on_ ? kPlanOff : kPlanOn);
        const bool clear = on_ && ratio < kPlanLong;
        // B pictures that cost next to nothing between their references (a clean zoom, a fade):
        // placed whatever the P picture after them seems to cost. The half picture's search sees
        // a picture several steps from its reference as far too expensive there, and it cannot
        // cost more than the steps together.
        const bool free = sumB - sumFar < kPlanFree * sumNear;
        gate_ = uint8_t(!on_ ? (free ? shortRun | kClear : 0) : clear ? (maxB | kClear) : shortRun);
    }
    static const bool trace = std::getenv("H264_B_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "bplan: picture %d P %.0f B %.0f per block (its P picture %.0f), %.3f now, %.3f held: run %d\n",
                     anchor_ + 1, sumP / (double(n) * p), sumB / (double(n) * p), sumFar / double(n), sumP > 0 ? sumB / sumP : 0.0, ratio,
                     gate_ & (kClear - 1));
}

float Lookahead::qpOffset(const LookaheadFrame& f, int mb, double strength)
{
    const int intra = f.intraCost[size_t(mb)];
    if (intra <= 0)
        return 0.0f;
    return float(-strength * std::log2((double(intra) + double(f.propagate[size_t(mb)])) / double(intra)));
}

} // namespace schneidi::h264
