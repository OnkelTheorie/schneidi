// schneidi's own H.264 encoder: CABAC macroblock layer (ITU-T H.264 clauses 9.3.2 and 9.3.3.1).
// Serialises the syntax elements the macroblock coders chose; context selection follows the
// neighbour rules of 9.3.3.1.1 (availability stops at slice edges like everywhere else).
#include "Cabac.h"
#include "H264Encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace schneidi::h264 {
namespace {

constexpr int kBlkX[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
constexpr int kBlkY[16] = {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3};

// Table 9-31: offsets per ctxBlockCat (0 luma DC, 1 luma AC, 2 luma 4x4, 3 chroma DC, 4 chroma AC).
constexpr int kCbfOffset[5] = {0, 4, 8, 12, 16};
constexpr int kSigOffset[5] = {0, 15, 29, 44, 47};
constexpr int kAbsOffset[5] = {0, 10, 20, 30, 39};

// k-th order Exp-Golomb suffix in bypass bins (9.3.2.3).
void expGolombBypass(CabacEncoder& ce, unsigned v, int k)
{
    for (;;) {
        if (v >= (1u << k)) {
            ce.bypass(1);
            v -= 1u << k;
            ++k;
        } else {
            ce.bypass(0);
            while (k--)
                ce.bypass(int((v >> k) & 1));
            return;
        }
    }
}

// residual_block_cabac (7.3.5.3.3) for one coefficient list; returns coded_block_flag.
int residualBlock(CabacEncoder& ce, int cat, const int* list, int count, int cbfInc)
{
    int last = -1;
    for (int i = 0; i < count; ++i)
        if (list[i])
            last = i;
    ce.decision(85 + kCbfOffset[cat] + cbfInc, last >= 0);
    if (last < 0)
        return 0;
    const int sigBase = 105 + kSigOffset[cat], lastBase = 166 + kSigOffset[cat], absBase = 227 + kAbsOffset[cat];
    for (int i = 0; i < count - 1; ++i) {
        const int inc = cat == 3 ? std::min(i, 2) : i;
        const int sig = list[i] != 0;
        ce.decision(sigBase + inc, sig);
        if (sig) {
            ce.decision(lastBase + inc, i == last);
            if (i == last)
                break;
        }
    }
    int eq1 = 0, gt1 = 0;
    for (int i = last; i >= 0; --i) {
        if (!list[i])
            continue;
        const int a = std::abs(list[i]) - 1; // coeff_abs_level_minus1: UEG0, uCoff 14
        const int ctx0 = absBase + (gt1 ? 0 : std::min(4, 1 + eq1));
        const int ctxN = absBase + 5 + std::min(4 - (cat == 3 ? 1 : 0), gt1);
        const int prefix = std::min(a, 14);
        ce.decision(ctx0, prefix > 0);
        for (int b = 1; b < prefix; ++b)
            ce.decision(ctxN, 1);
        if (prefix > 0 && prefix < 14)
            ce.decision(ctxN, 0);
        if (a >= 14)
            expGolombBypass(ce, unsigned(a - 14), 0);
        ce.bypass(list[i] < 0); // coeff_sign_flag
        if (a == 0)
            ++eq1;
        else
            ++gt1;
    }
    return 1;
}

enum : uint8_t { kKindSkip = 0, kKindI16 = 1, kKindI4 = 2, kKindInter = 3 };

// Table 9-43: ctxIdxInc of significant/last_significant_coeff_flag for 8x8 blocks (frame).
constexpr uint8_t kSig8[63] = {0,  1,  2,  3,  4,  5,  5,  4,  4,  3,  3,  4,  4,  4,  5,  5,  4,  4,  4,  4,  3,
                               3,  6,  7,  7,  7,  8,  9,  10, 9,  8,  7,  7,  6,  11, 12, 13, 11, 6,  7,  8,  9,
                               14, 10, 9,  8,  6,  11, 12, 13, 11, 6,  9,  14, 10, 9,  11, 12, 13, 11, 14, 10, 12};
constexpr uint8_t kLast8[63] = {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6,
                                6, 6, 7, 7, 7, 7, 8, 8, 8};

// residual_block_cabac for an 8x8 block (ctxBlockCat 5): no coded_block_flag (inferred 1).
void residualBlock8x8(CabacEncoder& ce, const int* list)
{
    int last = -1;
    for (int i = 0; i < 64; ++i)
        if (list[i])
            last = i;
    for (int i = 0; i < 63; ++i) {
        const int sig = list[i] != 0;
        ce.decision(402 + kSig8[i], sig);
        if (sig) {
            ce.decision(417 + kLast8[i], i == last);
            if (i == last)
                break;
        }
    }
    int eq1 = 0, gt1 = 0;
    for (int i = last; i >= 0; --i) {
        if (!list[i])
            continue;
        const int a = std::abs(list[i]) - 1;
        const int ctx0 = 426 + (gt1 ? 0 : std::min(4, 1 + eq1));
        const int ctxN = 426 + 5 + std::min(4, gt1);
        const int prefix = std::min(a, 14);
        ce.decision(ctx0, prefix > 0);
        for (int b = 1; b < prefix; ++b)
            ce.decision(ctxN, 1);
        if (prefix > 0 && prefix < 14)
            ce.decision(ctxN, 0);
        if (a >= 14)
            expGolombBypass(ce, unsigned(a - 14), 0);
        ce.bypass(list[i] < 0);
        if (a == 0)
            ++eq1;
        else
            ++gt1;
    }
}

} // namespace

void Encoder::cabacWriteMb(CabacEncoder& ce, int mbx, int mby, const MbSyntax& s, bool& prevQpNonZero)
{
    using K = MbSyntax::Kind;
    const size_t m = size_t(mby) * mbW_ + mbx;
    const bool hasL = mbx > 0, hasT = topMbAvailable(mby);
    const size_t mL = m - 1, mT = m - size_t(mbW_);
    const int lw = mbW_ * 4, chw = mbW_ * 2;
    const bool intra = s.kind == K::I16 || s.kind == K::I4;
    // Neighbour macroblock of a luma 4x4 position (picture coordinates in 4x4 units); -1 = unavailable.
    auto mbOf4 = [&](int bx, int by) -> long {
        if (bx < 0 || by < 0)
            return -1;
        const int nx = bx >> 2, ny = by >> 2;
        if (ny != mby && !topMbAvailable(mby))
            return -1;
        return long(ny) * mbW_ + nx;
    };

    // mb_skip_flag (P slices)
    if (pSlice_) {
        const int inc = (hasL && cabKind_[mL] != kKindSkip) + (hasT && cabKind_[mT] != kKindSkip);
        ce.decision(11 + inc, s.kind == K::Skip);
    }
    if (s.kind == K::Skip) {
        cabKind_[m] = kKindSkip;
        cabCbp_[m] = 0;
        cabChromaMode_[m] = 0;
        cabCbfDc_[m] = 0;
        cabT8_[m] = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                cabCbfLuma_[o] = 0;
                cabMvd_[o][0] = cabMvd_[o][1] = 0;
            }
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < 4; ++b)
                cabCbfChroma_[c][size_t(mby * 2 + (b >> 1)) * chw + mbx * 2 + (b & 1)] = 0;
        prevQpNonZero = false;
        return;
    }
    cabKind_[m] = s.kind == K::I16 ? kKindI16 : s.kind == K::I4 ? kKindI4 : kKindInter;

    // mb_type (9.3.2.5, tables 9-36/9-37)
    if (intra) {
        int base = 3;
        if (pSlice_) {
            ce.decision(14, 1); // prefix: intra macroblock in a P slice
            base = 17;
        }
        const int inc0 = pSlice_ ? 0 : (hasL && cabKind_[mL] != kKindI4) + (hasT && cabKind_[mT] != kKindI4);
        if (s.kind == K::I4) {
            ce.decision(base + inc0, 0);
            if (useT8()) // transform_size_8x8_flag: Intra 8x8 or 4x4
                ce.decision(399 + (hasL && cabT8_[mL]) + (hasT && cabT8_[mT]), s.t8);
        } else {
            ce.decision(base + inc0, 1);
            ce.terminate(0); // not I_PCM
            const int ctxLuma = pSlice_ ? 18 : 6, ctxChroma = pSlice_ ? 19 : 7;
            const int ctxChroma2 = pSlice_ ? 19 : 8, ctxMode0 = pSlice_ ? 20 : 9, ctxMode1 = pSlice_ ? 20 : 10;
            ce.decision(ctxLuma, s.cbpLuma != 0);
            ce.decision(ctxChroma, s.cbpChroma != 0);
            if (s.cbpChroma)
                ce.decision(ctxChroma2, s.cbpChroma == 2);
            ce.decision(ctxMode0, (s.i16Mode >> 1) & 1);
            ce.decision(ctxMode1, s.i16Mode & 1);
        }
    } else {
        static constexpr int kBins[4][3] = {{0, 0, 0}, {0, 1, 1}, {0, 1, 0}, {0, 0, 1}};
        const int* b = kBins[s.interType];
        ce.decision(14, b[0]);
        ce.decision(15, b[1]);
        ce.decision(b[1] != 1 ? 16 : 17, b[2]);
        if (s.interType == 3)
            for (int p = 0; p < 4; ++p)
                ce.decision(21, 1); // sub_mb_type P_L0_8x8
    }

    // mb_pred: intra modes
    if (s.kind == K::I4) {
        for (int blk = 0; blk < (s.t8 ? 4 : 16); ++blk) { // prev_intra{4x4,8x8}_pred_mode share contexts
            const bool same = s.i4Mode[blk] == s.i4Pred[blk];
            ce.decision(68, same);
            if (!same) {
                const int rem = s.i4Mode[blk] < s.i4Pred[blk] ? s.i4Mode[blk] : s.i4Mode[blk] - 1;
                for (int bit = 0; bit < 3; ++bit)
                    ce.decision(69, (rem >> bit) & 1);
            }
        }
    }
    if (intra) {
        auto cond = [&](bool avail, size_t n) {
            return avail && (cabKind_[n] == kKindI16 || cabKind_[n] == kKindI4) && cabChromaMode_[n] != 0;
        };
        const int v = s.chromaMode;
        ce.decision(64 + cond(hasL, mL) + cond(hasT, mT), v > 0);
        if (v > 0)
            ce.decision(67, v > 1);
        if (v > 1)
            ce.decision(67, v > 2);
        cabChromaMode_[m] = uint8_t(v);
    } else {
        cabChromaMode_[m] = 0;
    }

    // mb_pred / sub_mb_pred: ref_idx_l0 and mvd_l0
    struct Part {
        int x4, y4, w4, h4;
    };
    static constexpr Part kParts[4][4] = {
        {{0, 0, 4, 4}},
        {{0, 0, 4, 2}, {0, 2, 4, 2}},
        {{0, 0, 2, 4}, {2, 0, 2, 4}},
        {{0, 0, 2, 2}, {2, 0, 2, 2}, {0, 2, 2, 2}, {2, 2, 2, 2}},
    };
    static constexpr int kPartCount[4] = {1, 2, 2, 4};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
            cabMvd_[o][0] = cabMvd_[o][1] = 0;
        }
    if (!intra) {
        const int count = kPartCount[s.interType];
        // Neighbour 4x4 block info for ref_idx / mvd contexts: inter macroblocks only.
        auto interNeighbour = [&](int bx, int by, size_t& o) {
            const long n = mbOf4(bx, by);
            if (n < 0)
                return false;
            if (size_t(n) != m && cabKind_[size_t(n)] != kKindInter)
                return false;
            o = size_t(by) * lw + size_t(bx);
            return true;
        };
        if (activeRefs_ > 1) {
            for (int p = 0; p < count; ++p) {
                const Part& P = kParts[s.interType][p];
                const int bx = mbx * 4 + P.x4, by = mby * 4 + P.y4;
                size_t o;
                const int a = interNeighbour(bx - 1, by, o) && refIdx_[o] > 0;
                const int b = interNeighbour(bx, by - 1, o) && refIdx_[o] > 0;
                const int r = s.ref[p];
                for (int i = 0; i <= r; ++i) {
                    const int ctx = i == 0 ? 54 + a + 2 * b : i == 1 ? 58 : 59;
                    ce.decision(ctx, i < r);
                }
            }
        }
        for (int p = 0; p < count; ++p) {
            const Part& P = kParts[s.interType][p];
            const int bx = mbx * 4 + P.x4, by = mby * 4 + P.y4;
            for (int c = 0; c < 2; ++c) {
                size_t o;
                int sum = 0;
                if (interNeighbour(bx - 1, by, o))
                    sum += cabMvd_[o][c];
                if (interNeighbour(bx, by - 1, o))
                    sum += cabMvd_[o][c];
                const int inc = sum < 3 ? 0 : sum > 32 ? 2 : 1;
                const int base = c ? 47 : 40;
                const int v = s.mvd[p][c], av = std::abs(v);
                const int prefix = std::min(av, 9); // UEG3, uCoff 9
                for (int i = 0; i < prefix; ++i)
                    ce.decision(base + (i == 0 ? inc : std::min(i + 2, 6)), 1);
                if (av < 9)
                    ce.decision(base + (prefix == 0 ? inc : std::min(prefix + 2, 6)), 0);
                else
                    expGolombBypass(ce, unsigned(av - 9), 3);
                if (av)
                    ce.bypass(v < 0);
                const uint8_t stored = uint8_t(std::min(av, 255));
                for (int y = P.y4; y < P.y4 + P.h4; ++y)
                    for (int x = P.x4; x < P.x4 + P.w4; ++x)
                        cabMvd_[size_t(mby * 4 + y) * lw + mbx * 4 + x][c] = stored;
            }
        }
    }

    // coded_block_pattern (not for Intra 16x16)
    if (s.kind != K::I16) {
        for (int b8 = 0; b8 < 4; ++b8) {
            const int x8 = b8 & 1, y8 = b8 >> 1;
            auto cond = [&](bool inside, int b8Inside, bool avail, size_t n, int b8N) {
                if (inside)
                    return ((s.cbpLuma >> b8Inside) & 1) == 0 ? 1 : 0;
                if (!avail)
                    return 0;
                if (cabKind_[n] == kKindSkip)
                    return 1;
                return ((cabCbp_[n] >> b8N) & 1) == 0 ? 1 : 0;
            };
            const int a = cond(x8 == 1, b8 - 1, hasL, mL, b8 + 1);
            const int b = cond(y8 == 1, b8 - 2, hasT, mT, b8 + 2);
            ce.decision(73 + a + 2 * b, (s.cbpLuma >> b8) & 1);
        }
        auto condC = [&](bool avail, size_t n, int need) {
            if (!avail || cabKind_[n] == kKindSkip)
                return 0;
            const int cc = cabCbp_[n] >> 4;
            return need == 1 ? (cc != 0 ? 1 : 0) : (cc == 2 ? 1 : 0);
        };
        ce.decision(77 + condC(hasL, mL, 1) + 2 * condC(hasT, mT, 1), s.cbpChroma != 0);
        if (s.cbpChroma)
            ce.decision(81 + condC(hasL, mL, 2) + 2 * condC(hasT, mT, 2), s.cbpChroma == 2);
    }
    cabCbp_[m] = uint8_t(s.cbpLuma | (s.cbpChroma << 4));
    const bool t8 = (s.kind == K::Inter && s.t8 && s.cbpLuma) || (s.kind == K::I4 && s.t8);
    if (s.kind == K::Inter && s.cbpLuma && useT8())
        ce.decision(399 + (hasL && cabT8_[mL]) + (hasT && cabT8_[mT]), t8);
    cabT8_[m] = t8;

    // mb_qp_delta
    if (s.kind == K::I16 || s.cbpLuma || s.cbpChroma) {
        const int v = s.qpDelta;
        const int k = v > 0 ? 2 * v - 1 : -2 * v;
        for (int i = 0; i <= k; ++i)
            ce.decision(i == 0 ? 60 + (prevQpNonZero ? 1 : 0) : i == 1 ? 62 : 63, i < k);
        prevQpNonZero = v != 0;
    } else {
        prevQpNonZero = false;
    }

    // residual
    // coded_block_flag condition of a neighbouring luma 4x4 block (9.3.3.1.1.9).
    auto lumaCbfCond = [&](int bx, int by) {
        const long n = mbOf4(bx, by);
        if (n < 0)
            return intra ? 1 : 0;
        const int b8 = ((by & 3) >> 1) * 2 + ((bx & 3) >> 1);
        if (size_t(n) == m)
            return ((s.cbpLuma >> b8) & 1) ? int(cabCbfLuma_[size_t(by) * lw + bx]) : 0;
        if (cabKind_[size_t(n)] == kKindSkip || ((cabCbp_[size_t(n)] >> b8) & 1) == 0)
            return 0;
        return int(cabCbfLuma_[size_t(by) * lw + bx]);
    };
    uint8_t dcFlags = 0;
    if (s.kind == K::I16) {
        auto cond = [&](bool avail, size_t n) {
            if (!avail)
                return 1; // current macroblock is intra
            return cabKind_[n] == kKindI16 ? (cabCbfDc_[n] & 1) : 0;
        };
        dcFlags |= uint8_t(residualBlock(ce, 0, s.lumaDc, 16, cond(hasL, mL) + 2 * cond(hasT, mT)));
    }
    for (int blk = 0; blk < 16 && t8; ++blk) {
        // 8x8 blocks are coded once per 8x8; their 4x4 blocks count as coded for neighbours.
        const int bx = mbx * 4 + kBlkX[blk], by = mby * 4 + kBlkY[blk];
        const bool coded = (s.cbpLuma >> (blk >> 2)) & 1;
        if ((blk & 3) == 0 && coded)
            residualBlock8x8(ce, s.luma8[blk >> 2]);
        cabCbfLuma_[size_t(by) * lw + bx] = coded;
    }
    for (int blk = 0; blk < 16 && !t8; ++blk) {
        const int bx = mbx * 4 + kBlkX[blk], by = mby * 4 + kBlkY[blk];
        uint8_t& flag = cabCbfLuma_[size_t(by) * lw + bx];
        flag = 0;
        if (!((s.cbpLuma >> (blk >> 2)) & 1))
            continue;
        const int inc = lumaCbfCond(bx - 1, by) + 2 * lumaCbfCond(bx, by - 1);
        if (s.kind == K::I16)
            flag = uint8_t(residualBlock(ce, 1, s.luma[blk], 15, inc));
        else
            flag = uint8_t(residualBlock(ce, 2, s.luma[blk], 16, inc));
    }
    if (s.cbpChroma) {
        for (int c = 0; c < 2; ++c) {
            auto cond = [&](bool avail, size_t n) {
                if (!avail)
                    return intra ? 1 : 0;
                if (cabKind_[n] == kKindSkip || (cabCbp_[n] >> 4) == 0)
                    return 0;
                return (cabCbfDc_[n] >> (1 + c)) & 1;
            };
            dcFlags |= uint8_t(residualBlock(ce, 3, s.chromaDc[c], 4, cond(hasL, mL) + 2 * cond(hasT, mT)) << (1 + c));
        }
    }
    cabCbfDc_[m] = dcFlags;
    for (int c = 0; c < 2; ++c) {
        for (int blk = 0; blk < 4; ++blk) {
            const int bx = mbx * 2 + (blk & 1), by = mby * 2 + (blk >> 1);
            uint8_t& flag = cabCbfChroma_[c][size_t(by) * chw + bx];
            flag = 0;
            if (s.cbpChroma != 2)
                continue;
            auto cond = [&](int nx, int ny) {
                if (nx < 0 || ny < 0)
                    return intra ? 1 : 0;
                const int mx = nx >> 1, my = ny >> 1;
                if (my != mby && !topMbAvailable(mby))
                    return intra ? 1 : 0;
                const size_t n = size_t(my) * mbW_ + mx;
                if (n == m)
                    return int(cabCbfChroma_[c][size_t(ny) * chw + nx]);
                if (cabKind_[n] == kKindSkip || (cabCbp_[n] >> 4) != 2)
                    return 0;
                return int(cabCbfChroma_[c][size_t(ny) * chw + nx]);
            };
            flag = uint8_t(residualBlock(ce, 4, s.chromaAc[c][blk], 15, cond(bx - 1, by) + 2 * cond(bx, by - 1)));
        }
    }
}


// Trellis quantisation with CABAC costs (in the spirit of x264's trellis): dynamic programming
// over the coefficients in reverse scan order. Nodes are the 8 states of the coeff_abs_level
// contexts (4 for "no level > 1 yet" by count of ones, 4 by count of levels > 1) plus "nothing
// coded yet"; each coefficient may become 0, floor(|x|) or floor(|x|) + 1. Distortion is the
// pixel-domain SSD (scale = pixel error per level unit), rate the live contexts' bin costs.
// x: real-valued levels in scan order; cat 2 (luma 4x4) or 5 (luma 8x8). Returns levels in scan order.
void cabacTrellis(const CabacEncoder& ce, int cat, const double* x, const double* scale, int count, double lambda,
                  int cbfInc, int* out)
{
    constexpr int kEmpty = 8;
    // Coefficients after the last one that reaches half a level can only become 0: they leave
    // every path in the "nothing coded yet" node at no cost, so the search starts at that one.
    int end = count;
    while (end > 0 && std::abs(x[end - 1]) < 0.5)
        --end;
    std::fill(out + end, out + count, 0);
    if (end == 0)
        return;
    const bool cat5 = cat == 5;
    const int absBase = cat5 ? 426 : 227 + kAbsOffset[cat];
    auto sigCtx = [&](int k) { return cat5 ? 402 + kSig8[k] : 105 + kSigOffset[cat] + k; };
    auto lastCtx = [&](int k) { return cat5 ? 417 + kLast8[k] : 166 + kSigOffset[cat] + k; };
    auto levelBits = [&](int state, int a) { // a = |level| - 1, in 1/256 bits
        const int ctx0 = absBase + (state < 4 ? std::min(4, 1 + state) : 0);
        const int ctxN = absBase + 5 + std::min(4, state < 4 ? 0 : state - 3);
        const int prefix = std::min(a, 14);
        uint32_t bits = ce.binCost(ctx0, prefix > 0) + 256; // + sign
        if (prefix > 1)
            bits += uint32_t(prefix - 1) * ce.binCost(ctxN, 1);
        if (prefix > 0 && prefix < 14)
            bits += ce.binCost(ctxN, 0);
        if (a >= 14) {
            unsigned v = unsigned(a - 14);
            int k = 0;
            while (v >= (1u << k)) {
                v -= 1u << k;
                ++k;
            }
            bits += 256u * unsigned(2 * k + 1);
        }
        return bits;
    };
    auto next = [](int state, int mag) {
        if (mag == 1)
            return state < 4 ? std::min(state + 1, 3) : state;
        return state < 4 ? 4 : std::min(state + 1, 7);
    };
    // Costs per node; the path is kept as (previous node, level) per position and traced back.
    double cost[9], fresh[9];
    uint8_t from[64][9];
    int16_t mags[64][9];
    std::fill(cost, cost + 9, 1e300);
    cost[kEmpty] = 0;
    const double l256 = lambda / 256.0;
    for (int k = end - 1; k >= 0; --k) {
        const double ax = std::abs(x[k]);
        int cand[3] = {0, 0, 0}, nc = 1;
        if (ax >= 0.5) {
            const int fl = int(ax);
            if (fl >= 1)
                cand[nc++] = fl;
            cand[nc++] = fl + 1;
        }
        std::fill(fresh, fresh + 9, 1e300);
        for (int n = 0; n < 9; ++n) {
            if (cost[n] >= 1e299)
                continue;
            const bool empty = n == kEmpty;
            for (int ci = 0; ci < nc; ++ci) {
                const int mag = cand[ci];
                const double err = (ax - mag) * scale[k];
                double cst = cost[n] + err * err;
                int to;
                if (mag == 0) {
                    if (!empty)
                        cst += l256 * ce.binCost(sigCtx(k), 0);
                    to = n;
                } else {
                    uint32_t bits = levelBits(empty ? 0 : n, mag - 1);
                    if (k < count - 1)
                        bits += ce.binCost(sigCtx(k), 1) + ce.binCost(lastCtx(k), empty ? 1 : 0);
                    cst += l256 * bits;
                    to = next(empty ? 0 : n, mag);
                }
                if (cst < fresh[to]) {
                    fresh[to] = cst;
                    from[k][to] = uint8_t(n);
                    mags[k][to] = int16_t(mag);
                }
            }
        }
        std::copy(fresh, fresh + 9, cost);
    }
    int best = kEmpty;
    double bestCost = cost[kEmpty] + (cat5 ? 0.0 : l256 * ce.binCost(85 + kCbfOffset[cat] + cbfInc, 0));
    for (int n = 0; n < 8; ++n) {
        const double c = cost[n] + (cat5 ? 0.0 : l256 * ce.binCost(85 + kCbfOffset[cat] + cbfInc, 1));
        if (c < bestCost) {
            bestCost = c;
            best = n;
        }
    }
    for (int k = 0, n = best; k < end; ++k) {
        const int mag = mags[k][n];
        out[k] = x[k] < 0 ? -mag : mag;
        n = from[k][n];
    }
}

} // namespace schneidi::h264
