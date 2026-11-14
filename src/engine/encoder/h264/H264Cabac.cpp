// schneidi's own H.264 encoder: CABAC macroblock layer (ITU-T H.264 clauses 9.3.2 and 9.3.3.1).
// Serialises the syntax elements the macroblock coders chose; context selection follows the
// neighbour rules of 9.3.3.1.1 (availability stops at slice edges like everywhere else).
#include "Cabac.h"
#include "FrameCoder.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace schneidi::h264 {
namespace {

constexpr int kBlkX[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
constexpr int kBlkY[16] = {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3};

// Table 9-31: offsets per ctxBlockCat (0 luma DC, 1 luma AC, 2 luma 4x4, 3 chroma DC, 4 chroma AC).
constexpr int kCbfOffset[5] = {0, 4, 8, 12, 16};
constexpr int kSigOffset[5] = {0, 15, 29, 44, 47};
constexpr int kAbsOffset[5] = {0, 10, 20, 30, 39};

// k-th order Exp-Golomb suffix in bypass bins (9.3.2.3).
template <class E>
void expGolombBypass(E& ce, unsigned v, int k)
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
template <class E>
int residualBlock(E& ce, int cat, const int* list, int count, int cbfInc)
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
template <class E>
void residualBlock8x8(E& ce, const int* list)
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

void FrameCoder::CabacState::resize(int mbW, int mbH)
{
    const size_t mbs = size_t(mbW) * mbH;
    kind.assign(mbs, 0);
    cbp.assign(mbs, 0);
    chromaMode.assign(mbs, 0);
    cbfDc.assign(mbs, 0);
    t8.assign(mbs, 0);
    cbfLuma.assign(mbs * 16, 0);
    mvd.assign(mbs * 16, {0, 0});
    mvd1.assign(mbs * 16, {0, 0});
    direct.assign(mbs, 0);
    cbfChroma[0].assign(mbs * 4, 0);
    cbfChroma[1].assign(mbs * 4, 0);
}

double FrameCoder::cabacLumaSideBits(const CabacEncoder& ce, int mbx, int mby, int cbpLuma, bool t8) const
{
    const CabacState& st = cabWave_;
    const size_t m = size_t(mby) * mbW_ + mbx;
    const bool hasL = mbx > 0, hasT = topMbAvailable(mby);
    const size_t mL = m - 1, mT = m - size_t(mbW_);
    uint32_t cost = 0;
    for (int b8 = 0; b8 < 4; ++b8) {
        const int x8 = b8 & 1, y8 = b8 >> 1;
        auto cond = [&](bool inside, int b8Inside, bool avail, size_t n, int b8N) {
            if (inside)
                return ((cbpLuma >> b8Inside) & 1) == 0 ? 1 : 0;
            if (!avail)
                return 0;
            if (st.kind[n] == kKindSkip)
                return 1;
            return ((st.cbp[n] >> b8N) & 1) == 0 ? 1 : 0;
        };
        const int a = cond(x8 == 1, b8 - 1, hasL, mL, b8 + 1);
        const int b = cond(y8 == 1, b8 - 2, hasT, mT, b8 + 2);
        cost += ce.binCost(73 + a + 2 * b, (cbpLuma >> b8) & 1);
    }
    if (cbpLuma && useT8())
        cost += ce.binCost(399 + (hasL && st.t8[mL]) + (hasT && st.t8[mT]), t8);
    return cost / 256.0;
}

template <class E>
void FrameCoder::cabacWriteMb(E& ce, CabacState& st, int mbx, int mby, const MbSyntax& s, bool& prevQpNonZero)
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

    // mb_skip_flag (P and B slices)
    if (pSlice_) {
        const int inc = (hasL && st.kind[mL] != kKindSkip) + (hasT && st.kind[mT] != kKindSkip);
        ce.decision((bSlice_ ? 24 : 11) + inc, s.kind == K::Skip);
    }
    // B slices, mb_type bin 0: neighbours that are neither B_Skip nor B_Direct_16x16.
    const int incB = bSlice_ ? (hasL && st.kind[mL] != kKindSkip && !st.direct[mL]) + (hasT && st.kind[mT] != kKindSkip && !st.direct[mT]) : 0;
    st.direct[m] = bSlice_ && s.kind == K::Inter && s.bType == 0;
    if (bSlice_)
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                st.mvd1[o][0] = st.mvd1[o][1] = 0;
            }
    if (s.kind == K::Skip) {
        st.kind[m] = kKindSkip;
        st.cbp[m] = 0;
        st.chromaMode[m] = 0;
        st.cbfDc[m] = 0;
        st.t8[m] = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                st.cbfLuma[o] = 0;
                st.mvd[o][0] = st.mvd[o][1] = 0;
            }
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < 4; ++b)
                st.cbfChroma[c][size_t(mby * 2 + (b >> 1)) * chw + mbx * 2 + (b & 1)] = 0;
        prevQpNonZero = false;
        return;
    }
    st.kind[m] = s.kind == K::I16 ? kKindI16 : s.kind == K::I4 ? kKindI4 : kKindInter;

    // mb_type (9.3.2.5, tables 9-36/9-37)
    if (intra) {
        int base = 3;
        if (bSlice_) {
            // prefix of an intra macroblock in a B slice: 1 1 1 1 0 1
            ce.decision(27 + incB, 1);
            ce.decision(30, 1);
            ce.decision(31, 1);
            ce.decision(32, 1);
            ce.decision(32, 0);
            ce.decision(32, 1);
            base = 32;
        } else if (pSlice_) {
            ce.decision(14, 1); // prefix: intra macroblock in a P slice
            base = 17;
        }
        const int inc0 = pSlice_ ? 0 : (hasL && st.kind[mL] != kKindI4) + (hasT && st.kind[mT] != kKindI4);
        if (s.kind == K::I4) {
            ce.decision(base + inc0, 0);
            if (useT8()) // transform_size_8x8_flag: Intra 8x8 or 4x4
                ce.decision(399 + (hasL && st.t8[mL]) + (hasT && st.t8[mT]), s.t8);
        } else {
            ce.decision(base + inc0, 1);
            ce.terminate(0); // not I_PCM
            const int ctxLuma = pSlice_ ? base + 1 : 6, ctxChroma = pSlice_ ? base + 2 : 7;
            const int ctxChroma2 = pSlice_ ? base + 2 : 8, ctxMode0 = pSlice_ ? base + 3 : 9, ctxMode1 = pSlice_ ? base + 3 : 10;
            ce.decision(ctxLuma, s.cbpLuma != 0);
            ce.decision(ctxChroma, s.cbpChroma != 0);
            if (s.cbpChroma)
                ce.decision(ctxChroma2, s.cbpChroma == 2);
            ce.decision(ctxMode0, (s.i16Mode >> 1) & 1);
            ce.decision(ctxMode1, s.i16Mode & 1);
        }
    } else if (bSlice_) {
        // mb_type in B slices (9.3.2.5, table 9-37): 0 | 1 0 x | 1 1 + four or five bins.
        const int t = s.bType;
        ce.decision(27 + incB, t != 0);
        if (t == 1 || t == 2) {
            ce.decision(30, 0);
            ce.decision(32, t - 1);
        } else if (t != 0) {
            ce.decision(30, 1);
            const int five = t >= 12 && t <= 21 ? t + 4 : -1;                           // 1xxxx: 16..25
            const int four = five >= 0 ? five >> 1 : t <= 10 ? t - 3 : t == 11 ? 14 : 15; // 22 (B_8x8) = 15
            ce.decision(31, (four >> 3) & 1);
            ce.decision(32, (four >> 2) & 1);
            ce.decision(32, (four >> 1) & 1);
            ce.decision(32, four & 1);
            if (five >= 0)
                ce.decision(32, five & 1);
        }
        if (t == 22)
            for (int p = 0; p < 4; ++p) { // sub_mb_type: direct, L0, L1 or Bi 8x8
                const int sub = s.bSub[p];
                ce.decision(36, sub != 0);
                if (sub == 1 || sub == 2) {
                    ce.decision(37, 0);
                    ce.decision(39, sub - 1);
                } else if (sub == 3) {
                    ce.decision(37, 1);
                    ce.decision(38, 0);
                    ce.decision(39, 0);
                    ce.decision(39, 0);
                }
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
            return avail && (st.kind[n] == kKindI16 || st.kind[n] == kKindI4) && st.chromaMode[n] != 0;
        };
        const int v = s.chromaMode;
        ce.decision(64 + cond(hasL, mL) + cond(hasT, mT), v > 0);
        if (v > 0)
            ce.decision(67, v > 1);
        if (v > 1)
            ce.decision(67, v > 2);
        st.chromaMode[m] = uint8_t(v);
    } else {
        st.chromaMode[m] = 0;
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
            st.mvd[o][0] = st.mvd[o][1] = 0;
        }
    if (!intra && bSlice_) {
        // One reference per list: no ref_idx. mvd_l0 of every partition that uses list 0, then mvd_l1.
        const BParts bp = bPartsOf(s.bType, s.bSub);
        for (int list = 0; list < 2; ++list) {
            std::vector<std::array<uint8_t, 2>>& mvds = list ? st.mvd1 : st.mvd;
            for (int p = 0; p < bp.count; ++p) {
                if (!(bp.use[p] & (1 << list)))
                    continue;
                const Part& P = kParts[bp.shape][p];
                const int bx = mbx * 4 + P.x4, by = mby * 4 + P.y4;
                for (int c = 0; c < 2; ++c) {
                    // Neighbours that are unavailable, skipped, intra, direct or without this list count 0.
                    int sum = 0;
                    if (mbOf4(bx - 1, by) >= 0)
                        sum += mvds[size_t(by) * lw + size_t(bx - 1)][c];
                    if (mbOf4(bx, by - 1) >= 0)
                        sum += mvds[size_t(by - 1) * lw + size_t(bx)][c];
                    const int inc = sum < 3 ? 0 : sum > 32 ? 2 : 1;
                    const int base = c ? 47 : 40;
                    const int v = list ? s.mvd1[p][c] : s.mvd[p][c], av = std::abs(v);
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
                            mvds[size_t(mby * 4 + y) * lw + mbx * 4 + x][c] = stored;
                }
            }
        }
    } else if (!intra) {
        const int count = kPartCount[s.interType];
        // Neighbour 4x4 block info for ref_idx / mvd contexts: inter macroblocks only.
        auto interNeighbour = [&](int bx, int by, size_t& o) {
            const long n = mbOf4(bx, by);
            if (n < 0)
                return false;
            if (size_t(n) != m && st.kind[size_t(n)] != kKindInter)
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
                    sum += st.mvd[o][c];
                if (interNeighbour(bx, by - 1, o))
                    sum += st.mvd[o][c];
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
                        st.mvd[size_t(mby * 4 + y) * lw + mbx * 4 + x][c] = stored;
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
                if (st.kind[n] == kKindSkip)
                    return 1;
                return ((st.cbp[n] >> b8N) & 1) == 0 ? 1 : 0;
            };
            const int a = cond(x8 == 1, b8 - 1, hasL, mL, b8 + 1);
            const int b = cond(y8 == 1, b8 - 2, hasT, mT, b8 + 2);
            ce.decision(73 + a + 2 * b, (s.cbpLuma >> b8) & 1);
        }
        auto condC = [&](bool avail, size_t n, int need) {
            if (!avail || st.kind[n] == kKindSkip)
                return 0;
            const int cc = st.cbp[n] >> 4;
            return need == 1 ? (cc != 0 ? 1 : 0) : (cc == 2 ? 1 : 0);
        };
        ce.decision(77 + condC(hasL, mL, 1) + 2 * condC(hasT, mT, 1), s.cbpChroma != 0);
        if (s.cbpChroma)
            ce.decision(81 + condC(hasL, mL, 2) + 2 * condC(hasT, mT, 2), s.cbpChroma == 2);
    }
    st.cbp[m] = uint8_t(s.cbpLuma | (s.cbpChroma << 4));
    const bool t8 = (s.kind == K::Inter && s.t8 && s.cbpLuma) || (s.kind == K::I4 && s.t8);
    if (s.kind == K::Inter && s.cbpLuma && useT8())
        ce.decision(399 + (hasL && st.t8[mL]) + (hasT && st.t8[mT]), t8);
    st.t8[m] = t8;

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
            return ((s.cbpLuma >> b8) & 1) ? int(st.cbfLuma[size_t(by) * lw + bx]) : 0;
        if (st.kind[size_t(n)] == kKindSkip || ((st.cbp[size_t(n)] >> b8) & 1) == 0)
            return 0;
        return int(st.cbfLuma[size_t(by) * lw + bx]);
    };
    uint8_t dcFlags = 0;
    if (s.kind == K::I16) {
        auto cond = [&](bool avail, size_t n) {
            if (!avail)
                return 1; // current macroblock is intra
            return st.kind[n] == kKindI16 ? (st.cbfDc[n] & 1) : 0;
        };
        dcFlags |= uint8_t(residualBlock(ce, 0, s.lumaDc, 16, cond(hasL, mL) + 2 * cond(hasT, mT)));
    }
    for (int blk = 0; blk < 16 && t8; ++blk) {
        // 8x8 blocks are coded once per 8x8; their 4x4 blocks count as coded for neighbours.
        const int bx = mbx * 4 + kBlkX[blk], by = mby * 4 + kBlkY[blk];
        const bool coded = (s.cbpLuma >> (blk >> 2)) & 1;
        if ((blk & 3) == 0 && coded)
            residualBlock8x8(ce, s.luma8[blk >> 2]);
        st.cbfLuma[size_t(by) * lw + bx] = coded;
    }
    for (int blk = 0; blk < 16 && !t8; ++blk) {
        const int bx = mbx * 4 + kBlkX[blk], by = mby * 4 + kBlkY[blk];
        uint8_t& flag = st.cbfLuma[size_t(by) * lw + bx];
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
                if (st.kind[n] == kKindSkip || (st.cbp[n] >> 4) == 0)
                    return 0;
                return (st.cbfDc[n] >> (1 + c)) & 1;
            };
            dcFlags |= uint8_t(residualBlock(ce, 3, s.chromaDc[c], 4, cond(hasL, mL) + 2 * cond(hasT, mT)) << (1 + c));
        }
    }
    st.cbfDc[m] = dcFlags;
    for (int c = 0; c < 2; ++c) {
        for (int blk = 0; blk < 4; ++blk) {
            const int bx = mbx * 2 + (blk & 1), by = mby * 2 + (blk >> 1);
            uint8_t& flag = st.cbfChroma[c][size_t(by) * chw + bx];
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
                    return int(st.cbfChroma[c][size_t(ny) * chw + nx]);
                if (st.kind[n] == kKindSkip || (st.cbp[n] >> 4) != 2)
                    return 0;
                return int(st.cbfChroma[c][size_t(ny) * chw + nx]);
            };
            flag = uint8_t(residualBlock(ce, 4, s.chromaAc[c][blk], 15, cond(bx - 1, by) + 2 * cond(bx, by - 1)));
        }
    }
}


template void FrameCoder::cabacWriteMb<CabacEncoder>(CabacEncoder&, FrameCoder::CabacState&, int, int, const FrameCoder::MbSyntax&, bool&);
template void FrameCoder::cabacWriteMb<CabacCounter>(CabacCounter&, FrameCoder::CabacState&, int, int, const FrameCoder::MbSyntax&, bool&);

// Trellis quantisation with CABAC costs (rate-distortion optimised quantisation): dynamic programming
// over the coefficients in reverse scan order. Nodes are the 8 states of the coeff_abs_level
// contexts (4 for "no level > 1 yet" by count of ones, 4 by count of levels > 1) plus "nothing
// coded yet"; each coefficient may become 0, floor(|x|) or floor(|x|) + 1. Distortion is the
// pixel-domain SSD (scale = pixel error per level unit), rate the live contexts' bin costs.
// x: real-valued levels in scan order; cat 2 (luma 4x4) or 5 (luma 8x8). Returns levels in scan order.
// A level of 1.5 or more only rounds down or up, never to zero (-1.1 % instructions, BD-rate
// -0.08 %); H264_TRELLIS3=1 keeps the zero candidate too (before 11.11.).
static const bool gTrellisTwo = std::getenv("H264_TRELLIS3") == nullptr;

// Squared pixel errors of a block at level 0, in one pass from the end (count a multiple of 4):
// end = one past the last coefficient of half a level or more, tail = sum of the squares from end
// on, energy = tail + the sum of the squares before end. Each sum runs in four lanes (k mod 4, k
// descending), combined as (l0 + l1) + (l2 + l3): SSE2 and plain C++ add in the same order.
static void trellisScan(const double* x, const double* scale, int count, int& end, double& tail, double& energy)
{
    end = 0;
    bool found = false;
#if defined(__SSE2__) || defined(_M_X64)
    const __m128d half = _mm_set1_pd(0.5);
    const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
    __m128d tl[2] = {_mm_setzero_pd(), _mm_setzero_pd()}, hd[2] = {_mm_setzero_pd(), _mm_setzero_pd()};
    for (int g = count - 4; g >= 0; g -= 4) {
        for (int h = 1; h >= 0; --h) {
            const int k = g + 2 * h;
            const __m128d v = _mm_loadu_pd(x + k);
            const __m128d e = _mm_mul_pd(v, _mm_loadu_pd(scale + k));
            const __m128d sq = _mm_mul_pd(e, e);
            if (found) {
                hd[h] = _mm_add_pd(hd[h], sq);
                continue;
            }
            const int m = _mm_movemask_pd(_mm_cmpge_pd(_mm_and_pd(v, absMask), half));
            if (!m) {
                tl[h] = _mm_add_pd(tl[h], sq);
                continue;
            }
            found = true;
            end = k + ((m & 2) ? 2 : 1);
            // Lane k + 1 is at or after end; lane k only when k + 1 is the last one.
            const __m128d before = (m & 2) ? _mm_castsi128_pd(_mm_set1_epi64x(-1))
                                           : _mm_castsi128_pd(_mm_set_epi64x(0, -1)); // lanes < end
            hd[h] = _mm_add_pd(hd[h], _mm_and_pd(before, sq));
            tl[h] = _mm_add_pd(tl[h], _mm_andnot_pd(before, sq));
        }
    }
    auto lanes = [](const __m128d* v) {
        alignas(16) double l[4];
        _mm_store_pd(l, v[0]);
        _mm_store_pd(l + 2, v[1]);
        return (l[0] + l[1]) + (l[2] + l[3]);
    };
    tail = lanes(tl);
    energy = tail + lanes(hd);
#else
    double tl[4] = {0, 0, 0, 0}, hd[4] = {0, 0, 0, 0};
    for (int k = count - 1; k >= 0; --k) {
        const double e = x[k] * scale[k];
        const double sq = e * e;
        if (!found && std::abs(x[k]) >= 0.5) {
            found = true;
            end = k + 1;
        }
        // Lanes adding +0.0 in the SIMD version change nothing, so skipping them is the same.
        (found ? hd : tl)[k & 3] += sq;
    }
    tail = (tl[0] + tl[1]) + (tl[2] + tl[3]);
    energy = tail + ((hd[0] + hd[1]) + (hd[2] + hd[3]));
#endif
}

// With energy: returns the path's cost (squared pixel error of all coefficients + lambda * bits of
// the block, coded_block_flag included for 4x4 blocks) and sets *energy to the squared error of the
// all-zero block; the mode decision compares transforms and layouts with these (no reconstruction).
// lambdaHigh >= 0: the bits of the coefficients from scan position highFrom on weigh lambdaHigh
// instead of lambda (the psy mode decision's finer quantisation of the texture frequencies).
// zeroCost (optional): the cost of the all-zero block on the same scale as the return value
// (squared error of every coefficient + lambda * the bits of coding nothing), so zeroCost minus
// the return value is what coding the chosen levels gains over dropping them (the trellis itself
// never drops a level of 1 or more, the caller may still drop the whole block).
double cabacTrellis(const CabacEncoder& ce, int cat, const double* x, const double* scale, int count, double lambda,
                    int cbfInc, int* out, double* energy, uint64_t* nonZero, double lambdaHigh, int highFrom,
                    double* zeroCost)
{
    if (nonZero)
        *nonZero = 0;
    constexpr int kEmpty = 8;
    // Coefficients after the last one that reaches half a level can only become 0: they leave
    // every path in the "nothing coded yet" node at no cost, so the search starts at that one.
    int end = count;
    double tail = 0; // squared error of the coefficients after `end` (always zero)
    if (energy) {
        trellisScan(x, scale, count, end, tail, *energy);
    } else {
#if defined(__SSE2__) || defined(_M_X64)
        const __m128d half = _mm_set1_pd(0.5);
        const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
        while (end >= 2) {
            const int m = _mm_movemask_pd(_mm_cmpge_pd(_mm_and_pd(_mm_loadu_pd(x + end - 2), absMask), half));
            if (m) {
                end -= (m & 2) ? 0 : 1;
                break;
            }
            end -= 2;
        }
#endif
        while (end > 0 && std::abs(x[end - 1]) < 0.5)
            --end;
    }
    std::fill(out, out + count, 0);
    const bool cat5 = cat == 5;
    const double l256 = lambda / 256.0;
    const double h256 = lambdaHigh >= 0 ? lambdaHigh / 256.0 : l256;
    if (lambdaHigh < 0)
        highFrom = count; // no position weighs differently
    auto emptyCost = [&] {
        double e = 0;
        if (energy) {
            e = *energy;
        } else {
            for (int k = 0; k < count; ++k)
                e += (x[k] * scale[k]) * (x[k] * scale[k]);
        }
        return e + (cat5 ? 0.0 : l256 * ce.binCost(85 + kCbfOffset[cat] + cbfInc, 0));
    };
    if (end == 0) {
        if (zeroCost)
            *zeroCost = emptyCost();
        return energy ? tail + (cat5 ? 0.0 : l256 * ce.binCost(85 + kCbfOffset[cat] + cbfInc, 0)) : 0.0;
    }
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
    uint64_t pass = 0; // positions below half a level: every path keeps its node there (level 0)
    unsigned live = 1u << kEmpty; // nodes some path has reached (the others stay at 1e300)
    // Level bits of the magnitudes 1 and 2 by node, computed on first use.
    uint32_t lbCache[16];
    uint32_t lbValid = 0;
    auto levelBitsCached = [&](int state, int a) {
        if (a >= 2)
            return levelBits(state, a);
        const int i = a * 8 + state;
        if (!((lbValid >> i) & 1)) {
            lbCache[i] = levelBits(state, a);
            lbValid |= 1u << i;
        }
        return lbCache[i];
    };
    for (int k = end - 1; k >= 0; --k) {
        const double ax = std::abs(x[k]);
        const double lk = k >= highFrom ? h256 : l256; // weight of this position's bits
        if (ax < 0.5) {
            // Only level 0: the same sums as the general case below, without the node bookkeeping.
            const double err = ax * scale[k];
            const double errSq0 = err * err;
            if (live & 0xFF) {
                const double sig = lk * ce.binCost(sigCtx(k), 0);
                for (unsigned m = live & 0xFF; m; m &= m - 1) {
                    const int n = std::countr_zero(m);
                    cost[n] = cost[n] + errSq0 + sig;
                }
            }
            if (live & (1u << kEmpty))
                cost[kEmpty] += errSq0;
            pass |= uint64_t(1) << k;
            continue;
        }
        int cand[3] = {0, 0, 0}, nc = 1;
        if (ax >= 0.5) {
            const int fl = int(ax);
            if (fl >= 1) {
                if (gTrellisTwo)
                    nc = 0; // round down or up only: a level of 1.5 or more never drops to zero
                cand[nc++] = fl;
            }
            cand[nc++] = fl + 1;
        }
        std::fill(fresh, fresh + 9, 1e300);
        // Significance and last bins of this position do not depend on the node.
        const bool notLast = k < count - 1;
        const uint32_t sig0 = ce.binCost(sigCtx(k), 0);
        const uint32_t sig1Last[2] = {notLast ? ce.binCost(sigCtx(k), 1) + ce.binCost(lastCtx(k), 0) : 0,
                                      notLast ? ce.binCost(sigCtx(k), 1) + ce.binCost(lastCtx(k), 1) : 0};
        double errSq[3];
        for (int ci = 0; ci < nc; ++ci) {
            const double err = (ax - cand[ci]) * scale[k];
            errSq[ci] = err * err;
        }
        unsigned reached = 0;
        for (unsigned m = live; m; m &= m - 1) {
            const int n = std::countr_zero(m); // ascending, as before: ties keep the first node
            const bool empty = n == kEmpty;
            for (int ci = 0; ci < nc; ++ci) {
                const int mag = cand[ci];
                double cst = cost[n] + errSq[ci];
                int to;
                if (mag == 0) {
                    if (!empty)
                        cst += lk * sig0;
                    to = n;
                } else {
                    uint32_t bits = levelBitsCached(empty ? 0 : n, mag - 1);
                    bits += sig1Last[empty ? 1 : 0];
                    cst += lk * bits;
                    to = next(empty ? 0 : n, mag);
                }
                if (cst < fresh[to]) {
                    fresh[to] = cst;
                    from[k][to] = uint8_t(n);
                    mags[k][to] = int16_t(mag);
                    reached |= 1u << to;
                }
            }
        }
        std::copy(fresh, fresh + 9, cost);
        live = reached;
    }
    int best = kEmpty;
    double bestCost = cost[kEmpty] + (cat5 ? 0.0 : l256 * ce.binCost(85 + kCbfOffset[cat] + cbfInc, 0));
    if (zeroCost)
        *zeroCost = emptyCost();
    for (int n = 0; n < 8; ++n) {
        const double c = cost[n] + (cat5 ? 0.0 : l256 * ce.binCost(85 + kCbfOffset[cat] + cbfInc, 1));
        if (c < bestCost) {
            bestCost = c;
            best = n;
        }
    }
    // Positions below half a level stay 0 (out is cleared above) and keep the node.
    uint64_t coded = ~pass & (end >= 64 ? ~uint64_t(0) : (uint64_t(1) << end) - 1);
    uint64_t nz = 0;
    for (int n = best; coded; coded &= coded - 1) {
        const int k = std::countr_zero(coded);
        const int mag = mags[k][n];
        out[k] = x[k] < 0 ? -mag : mag;
        nz |= uint64_t(mag != 0) << k;
        n = from[k][n];
    }
    if (nonZero)
        *nonZero = nz;
    return bestCost + tail;
}

} // namespace schneidi::h264
