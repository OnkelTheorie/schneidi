// Lookahead analysis at half resolution: per macroblock the SATD of the best of three intra
// predictions and of a small motion search (full-pel diamond on SAD, half-pel on SATD) against
// the previous picture, plus the macroblock tree that propagates these costs backwards.
#include "Lookahead.h"

#include "Satd.h"
#include "ThreadErrors.h"

#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
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
#if defined(__SSE2__) || defined(_M_X64)
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
#if defined(__SSE2__) || defined(_M_X64)
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
                    // The same place first: usually close to the best, so the bound below bites early.
                    int best = sadBlock(&thumb[size_t(by) * tw + bx], tw, &prevThumb[size_t(by) * tw + bx], tw, 8, 8);
                    const uint8_t* cur = &thumb[size_t(by) * tw + bx];
                    const int mx0 = std::max(-kRange, -bx), mx1 = std::min(kRange, tw - 8 - bx); // inclusive
                    for (int my = -kRange; my <= kRange && best > 0; ++my) {
                        if (by + my < 0 || by + my + 8 > th)
                            continue;
                        const uint8_t* ref = &prevThumb[size_t(by + my) * tw + bx];
                        int mx = mx0;
#if defined(__SSE2__) || defined(_M_X64)
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
#endif
                        for (; mx <= mx1; ++mx) {
                            if (std::abs(sum - prevSum(bx + mx, by + my)) >= best)
                                continue;
                            best = std::min(best, sadBlock(cur, tw, ref + mx, tw, 8, 8));
                        }
                    }
                    interCost += std::min(best, intra);
                    intraCost += intra + 16; // a little bias so flat pictures do not count as cuts
                }
            // Real cuts measured ~0.92-0.95, fast game motion up to ~0.72.
            f.sceneChange = interCost * 100 > intraCost * 85;
        }
    }

}

double Lookahead::bScore(const LookaheadFrame& f, const LookaheadFrame& prev, const LookaheadFrame& prev2) const
{
    LookaheadFrame far;
    far.half = f.half;
    costs(far, &prev2, 1, &f, 2); // about twice the vector of one step
    const double steps = f.interSum + prev.interSum;
    return steps > 0 ? far.interSum / steps : 0.5;
}

void Lookahead::costs(LookaheadFrame& f, const LookaheadFrame* prev, int threads, const LookaheadFrame* seed,
                      int seedScale) const
{
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
        {
            const size_t m = size_t(by) * mbW_ + bx;
            const uint8_t* cur = at(f.half, bx * 8, by * 8);

            // Intra: DC, vertical and horizontal prediction from the source neighbours.
            uint8_t pred[64];
            int dc = 0;
            for (int i = 0; i < 8; ++i)
                dc += cur[-stride + i] + cur[i * stride - 1];
            std::fill(pred, pred + 64, uint8_t((dc + 8) >> 4));
            int intra = satd8x8U8Fast(cur, stride, pred, 8);
            for (int y = 0; y < 8; ++y)
                std::copy_n(cur - stride, 8, pred + y * 8);
            intra = std::min(intra, satd8x8U8Fast(cur, stride, pred, 8));
            for (int y = 0; y < 8; ++y)
                std::fill(pred + y * 8, pred + y * 8 + 8, cur[y * stride - 1]);
            intra = std::min(intra, satd8x8U8Fast(cur, stride, pred, 8)) + kIntraPenalty;
            f.intraCost[m] = intra;
            intraSum += intra;
            if (!prev) {
                f.interCost[m] = intra;
                interSum += intra;
                return;
            }

            // Motion: predictor = median of the left, top and top-right vectors (half-pel units).
            auto mvOf = [&](int dx, int dy, int& vx, int& vy) {
                const int x = bx + dx, y = by + dy;
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
#if defined(__SSE2__) || defined(_M_X64)
                const __m128i zero = _mm_setzero_si128(), two = _mm_set1_epi16(2);
                auto load = [&](const uint8_t* q) { return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(q)), zero); };
                for (int y = 0; y < 8; ++y) {
                    const uint8_t* q = ref + y * stride;
                    const __m128i sum = _mm_add_epi16(_mm_add_epi16(load(q), load(q + ox)),
                                                      _mm_add_epi16(load(q + oy * stride), load(q + oy * stride + ox)));
                    const __m128i v = _mm_srli_epi16(_mm_add_epi16(sum, two), 2);
                    _mm_storel_epi64(reinterpret_cast<__m128i*>(p + y * 8), _mm_packus_epi16(v, v));
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
        for (int by = 0; by < mbH_; ++by)
            for (int bx = 0; bx < mbW_; ++bx)
                doMb(bx, by, rowIntra[size_t(by)], rowInter[size_t(by)]);
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

float Lookahead::qpOffset(const LookaheadFrame& f, int mb, double strength)
{
    const int intra = f.intraCost[size_t(mb)];
    if (intra <= 0)
        return 0.0f;
    return float(-strength * std::log2((double(intra) + double(f.propagate[size_t(mb)])) / double(intra)));
}

} // namespace schneidi::h264
