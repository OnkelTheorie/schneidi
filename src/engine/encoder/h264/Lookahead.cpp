// Lookahead analysis at half resolution: per macroblock the SATD of the best of three intra
// predictions and of a small motion search (full-pel diamond on SAD, half-pel on SATD) against
// the previous picture, plus the macroblock tree that propagates these costs backwards.
#include "Lookahead.h"

#include "Satd.h"

#include <algorithm>
#include <cmath>

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
    const int stride = halfW_ + 2 * kMargin;
    const int rows = halfH_ + 2 * kMargin;
    f.half.assign(size_t(stride) * rows, 0);
    // 2x2 average with edge replication beyond the picture, then the margin.
    for (int y = 0; y < halfH_; ++y) {
        const int sy0 = std::min(2 * y, height_ - 1), sy1 = std::min(2 * y + 1, height_ - 1);
        const uint8_t* r0 = f.y.data() + size_t(sy0) * width_;
        const uint8_t* r1 = f.y.data() + size_t(sy1) * width_;
        uint8_t* out = f.half.data() + size_t(y + kMargin) * stride + kMargin;
        for (int x = 0; x < halfW_; ++x) {
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

    const size_t n = size_t(mbW_) * mbH_;
    f.intraCost.assign(n, 0);
    f.interCost.assign(n, 0);
    f.mvx.assign(n, 0);
    f.mvy.assign(n, 0);
    f.propagate.assign(n, 0.0f);
    f.intraSum = f.interSum = 0;

    auto at = [&](const std::vector<uint8_t>& plane, int x, int y) -> const uint8_t* {
        return plane.data() + size_t(y + kMargin) * stride + size_t(x + kMargin);
    };
    constexpr int kLambda = 1; // SATD per bit of vector difference (the lookahead has no QP)
    constexpr int kIntraPenalty = 8;
    for (int by = 0; by < mbH_; ++by)
        for (int bx = 0; bx < mbW_; ++bx) {
            const size_t m = size_t(by) * mbW_ + bx;
            const uint8_t* cur = at(f.half, bx * 8, by * 8);

            // Intra: DC, vertical and horizontal prediction from the source neighbours.
            uint8_t pred[64];
            int dc = 0;
            for (int i = 0; i < 8; ++i)
                dc += cur[-stride + i] + cur[i * stride - 1];
            std::fill(pred, pred + 64, uint8_t((dc + 8) >> 4));
            int intra = satd8x8U8(cur, stride, pred, 8);
            for (int y = 0; y < 8; ++y)
                std::copy_n(cur - stride, 8, pred + y * 8);
            intra = std::min(intra, satd8x8U8(cur, stride, pred, 8));
            for (int y = 0; y < 8; ++y)
                std::fill(pred + y * 8, pred + y * 8 + 8, cur[y * stride - 1]);
            intra = std::min(intra, satd8x8U8(cur, stride, pred, 8)) + kIntraPenalty;
            f.intraCost[m] = intra;
            f.intraSum += intra;
            if (!prev) {
                f.interCost[m] = intra;
                f.interSum += intra;
                continue;
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
                int s = 0;
                for (int y = 0; y < 8; ++y)
                    s += sadRow(cur + y * stride, ref + y * stride, 8);
                return s + mvCost(2 * vx, 2 * vy);
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
                    return satd8x8U8(cur, stride, ref, stride) + mvCost(hx, hy);
                uint8_t p[64];
                const int ox = hx & 1, oy = hy & 1;
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) {
                        const uint8_t* q = ref + y * stride + x;
                        p[y * 8 + x] = uint8_t((q[0] + q[ox] + q[oy * stride] + q[oy * stride + ox] + 2) >> 2);
                    }
                return satd8x8U8(cur, stride, p, 8) + mvCost(hx, hy);
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
            f.interSum += std::min(bestSatd, intra);
        }
}

void Lookahead::propagate(std::deque<LookaheadFrame>& frames) const
{
    for (LookaheadFrame& f : frames)
        std::fill(f.propagate.begin(), f.propagate.end(), 0.0f);
    const size_t n = size_t(mbW_) * mbH_;
    for (size_t j = frames.size(); j-- > 1;) {
        const LookaheadFrame& f = frames[j];
        if (f.keyframe || f.intraCost.size() != n)
            continue;
        LookaheadFrame& ref = frames[j - 1];
        for (int by = 0; by < mbH_; ++by)
            for (int bx = 0; bx < mbW_; ++bx) {
                const size_t m = size_t(by) * mbW_ + bx;
                const int intra = f.intraCost[m], inter = f.interCost[m];
                if (intra <= 0 || inter >= intra)
                    continue;
                // The share of this block's information that came from the reference.
                const float amount = (float(intra) + f.propagate[m]) * float(intra - inter) / float(intra);
                // Position in the reference in half-pel units; a block covers 16.
                const int px = bx * 16 + f.mvx[m], py = by * 16 + f.mvy[m];
                const int ix = px >> 4, iy = py >> 4; // floor
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
