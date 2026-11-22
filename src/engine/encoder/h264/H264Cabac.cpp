// schneidi's own H.264 encoder: CABAC macroblock layer (ITU-T H.264 clauses 9.3.2 and 9.3.3.1).
// Serialises the syntax elements the macroblock coders chose; context selection follows the
// neighbour rules of 9.3.3.1.1 (availability stops at slice edges like everywhere else).
#include "Simd.h" // first: no floating-point contraction (see there)
#include "DevEnv.h"
#include "Cabac.h"
#include "FrameCoder.h"

#include <algorithm>
#include <bit>
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
S264_DEV(bool, gTrellisTwo, devEnv("H264_TRELLIS3") == nullptr);

// Squared pixel errors of a block at level 0, in one pass from the end (count a multiple of 4):
// end = one past the last coefficient of half a level or more, tail = sum of the squares from end
// on, energy = tail + the sum of the squares before end. Each sum runs in four lanes (k mod 4, k
// descending), combined as (l0 + l1) + (l2 + l3): SSE2 and plain C++ add in the same order.
static void trellisScan(const double* x, const double* scale, int count, int& end, double& tail, double& energy)
{
    end = 0;
    bool found = false;
#if defined(SCHNEIDI_H264_SSE2) && defined(__AVX2__)
    // The x86-64-v3 build: the four lanes in one register.
    const __m256d half = _mm256_set1_pd(0.5);
    const __m256d absMask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7fffffffffffffffLL));
    __m256d tl = _mm256_setzero_pd(), hd = _mm256_setzero_pd();
    for (int g = count - 4; g >= 0; g -= 4) {
        const __m256d v = _mm256_loadu_pd(x + g);
        const __m256d e = _mm256_mul_pd(v, _mm256_loadu_pd(scale + g));
        const __m256d sq = _mm256_mul_pd(e, e);
        if (found) {
            hd = _mm256_add_pd(hd, sq);
            continue;
        }
        const int m = _mm256_movemask_pd(_mm256_cmp_pd(_mm256_and_pd(v, absMask), half, _CMP_GE_OQ));
        if (!m) {
            tl = _mm256_add_pd(tl, sq);
            continue;
        }
        found = true;
        const int last = 31 - std::countl_zero(unsigned(m)); // the last lane of half a level or more
        end = g + last + 1;
        static const int64_t kBefore[4][4] = {{-1, 0, 0, 0}, {-1, -1, 0, 0}, {-1, -1, -1, 0}, {-1, -1, -1, -1}};
        const __m256d before = _mm256_castsi256_pd(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(kBefore[last]))); // lanes < end
        hd = _mm256_add_pd(hd, _mm256_and_pd(before, sq));
        tl = _mm256_add_pd(tl, _mm256_andnot_pd(before, sq));
    }
    auto lanes = [](__m256d v) {
        alignas(32) double l[4];
        _mm256_store_pd(l, v);
        return (l[0] + l[1]) + (l[2] + l[3]);
    };
    tail = lanes(tl);
    energy = tail + lanes(hd);
#elif defined(SCHNEIDI_H264_SSE2)
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
#elif defined(SCHNEIDI_H264_NEON)
    // The SSE2 steps (multiplies and adds kept apart: no fused multiply-add).
    const float64x2_t half = vdupq_n_f64(0.5);
    float64x2_t tl[2] = {vdupq_n_f64(0.0), vdupq_n_f64(0.0)}, hd[2] = {vdupq_n_f64(0.0), vdupq_n_f64(0.0)};
    for (int g = count - 4; g >= 0; g -= 4) {
        for (int h = 1; h >= 0; --h) {
            const int k = g + 2 * h;
            const float64x2_t v = vld1q_f64(x + k);
            const float64x2_t e = vmulq_f64(v, vld1q_f64(scale + k));
            const float64x2_t sq = vmulq_f64(e, e);
            if (found) {
                hd[h] = vaddq_f64(hd[h], sq);
                continue;
            }
            const uint64x2_t ge = vcgeq_f64(vabsq_f64(v), half);
            const bool g0 = vgetq_lane_u64(ge, 0) != 0, g1 = vgetq_lane_u64(ge, 1) != 0;
            if (!g0 && !g1) {
                tl[h] = vaddq_f64(tl[h], sq);
                continue;
            }
            found = true;
            end = k + (g1 ? 2 : 1);
            // Lane k + 1 is at or after end; lane k only when k + 1 is the last one.
            const uint64x2_t before = g1 ? vdupq_n_u64(~0ull) : vcombine_u64(vcreate_u64(~0ull), vcreate_u64(0));
            const float64x2_t zero = vdupq_n_f64(0.0);
            hd[h] = vaddq_f64(hd[h], vbslq_f64(before, sq, zero));
            tl[h] = vaddq_f64(tl[h], vbslq_f64(before, zero, sq));
        }
    }
    auto lanes = [](const float64x2_t* v) {
        return (vgetq_lane_f64(v[0], 0) + vgetq_lane_f64(v[0], 1)) + (vgetq_lane_f64(v[1], 0) + vgetq_lane_f64(v[1], 1));
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
                    double* zeroCost, TrellisLevelBits* levelTable)
{
    if (nonZero)
        *nonZero = 0;
    constexpr int kEmpty = 8;
    // Coefficients after the last one that reaches half a level can only become 0: they leave
    // every path in the "nothing coded yet" node at no cost, so the search starts at that one.
    int end = count;
    double tail = 0; // squared error of the coefficients after `end` (always zero)
    double zero = 0; // and of the whole block at level 0
    trellisScan(x, scale, count, end, tail, zero);
    if (energy)
        *energy = zero;
    std::fill(out, out + count, 0);
    const bool cat5 = cat == 5;
    const double l256 = lambda / 256.0;
    const double h256 = lambdaHigh >= 0 ? lambdaHigh / 256.0 : l256;
    if (lambdaHigh < 0)
        highFrom = count; // no position weighs differently
    const double cbf0 = cat5 ? 0.0 : l256 * ce.binCost(85 + kCbfOffset[cat] + cbfInc, 0);
    if (zeroCost)
        *zeroCost = zero + cbf0;
    if (end == 0)
        return energy ? zero + cbf0 : 0.0;
    const int absBase = cat5 ? 426 : 227 + kAbsOffset[cat];
    auto sigCtx = [&](int k) { return cat5 ? 402 + kSig8[k] : 105 + kSigOffset[cat] + k; };
    auto lastCtx = [&](int k) { return cat5 ? 417 + kLast8[k] : 166 + kSigOffset[cat] + k; };
    // Bits of a level by node (8 = nothing coded yet: the contexts of state 0), in 1/256 bits: |level| 1 and
    // 2 with the sign, and for larger ones the first bin (as 1, with the sign) and the two costs of the later
    // bins. Kept as doubles: every sum of them below is a whole number, so exact. Node 0 is never reached (the
    // first coded level leads from "nothing coded yet" to node 1 or 4). levelTable (optional) keeps them for
    // the caller's next blocks of the same category and contexts.
    TrellisLevelBits ownTable;
    TrellisLevelBits& table = levelTable ? *levelTable : ownTable;
    if (!table.ready) {
        table.ready = true;
        for (int n = 1; n < 9; ++n) {
            const int state = n == kEmpty ? 0 : n;
            const int ctx0 = absBase + (state < 4 ? 1 + state : 0);
            const int ctxN = absBase + 5 + (state < 4 ? 0 : state - 3);
            table.one[n] = double(ce.binCost(ctx0, 0) + 256);
            table.first1[n] = double(ce.binCost(ctx0, 1) + 256);
            table.more0[n] = double(ce.binCost(ctxN, 0));
            table.more1[n] = double(ce.binCost(ctxN, 1));
            table.two[n] = table.first1[n] + table.more0[n];
        }
        // (Index 0 repeats "nothing coded yet": the lanes of the low group below are tables[0..3].)
        table.one[0] = table.one[kEmpty], table.two[0] = table.two[kEmpty], table.first1[0] = table.first1[kEmpty];
        table.more0[0] = table.more0[kEmpty], table.more1[0] = table.more1[kEmpty];
    }
    const double *one = table.one, *two = table.two, *first1 = table.first1, *more0 = table.more0, *more1 = table.more1;
    // The part of a level's bits that does not depend on the node: a = |level| - 1 >= 1 costs
    // first1 + ones * more1 + zero * more0 + escape.
    struct LevelBits {
        double ones, zero, escape;
    };
    auto levelBits = [](int a) {
        const int prefix = std::min(a, 14);
        int escape = 0;
        if (a >= 14) {
            unsigned v = unsigned(a - 14);
            int k = 0;
            while (v >= (1u << k)) {
                v -= 1u << k;
                ++k;
            }
            escape = 256 * (2 * k + 1);
        }
        return LevelBits{double(prefix - 1), prefix < 14 ? 1.0 : 0.0, double(escape)};
    };
    // The costs are kept relative to the all-zero block (`zero`, summed by the scan): a level adds what it takes
    // off the squared error (a negative number) and its bits, a zero only its bits where a level was coded before -
    // and those wait as whole numbers (zerosLow, zerosHigh by the weight of their position) until the next
    // coefficient of half a level or more, so a run of small coefficients costs no floating-point additions.
    // Costs per node, in two groups: "low" = nothing coded yet (cE) and one to three levels of 1 so far (c1 to
    // c3), "high" = one to four levels above 1 so far (c4 to c7). A node no path has reached costs 1e300; adding
    // to it leaves exactly 1e300 and it never wins a strict comparison, so a group is computed as a whole without
    // a branch per node, and skipped as a whole while none of its nodes is reached: high before the first level
    // that may be above 1, low after the first one that must be (lowLive, highLive: false only when every node of
    // the group is at 1e300). Arrivals at a node are compared in the order source node ascending, then level
    // ascending; ties keep the first. The path is kept per position and node as step = source node | candidate
    // index << 4 and traced back.
    constexpr double kDead = 1e300;
    double cE = 0, c1 = kDead, c2 = kDead, c3 = kDead, c4 = kDead, c5 = kDead, c6 = kDead, c7 = kDead;
    bool lowLive = true, highLive = false;
    uint8_t step[64][9];
    int16_t mags[64][3]; // the candidates' magnitudes
    uint64_t pass = 0; // positions below half a level: every path keeps its node there (level 0)
    uint32_t zerosLow = 0, zerosHigh = 0; // bits (1/256) of the zeros since the last flush, by weight (lambda, lambdaHigh)
    auto flushZeros = [&] {
        if (!(zerosLow | zerosHigh))
            return;
        const double z = h256 * double(zerosHigh) + l256 * double(zerosLow);
        zerosLow = zerosHigh = 0;
        c1 += z, c2 += z, c3 += z, c4 += z, c5 += z, c6 += z, c7 += z; // (an unreached node stays at 1e300)
    };
    constexpr int kHi = 16; // step: the second candidate
    auto take = [](double& best, int& bestStep, double c, int s) {
        const bool less = c < best;
        best = less ? c : best;
        bestStep = less ? s : bestStep;
    };
#if defined(SCHNEIDI_H264_SSE2) && defined(__AVX2__)
    // The x86-64-v3 build keeps each group in one register (low: cE, c1, c2, c3; high: c4 to c7) and computes the
    // arrivals of a group at once: the same sums and the same order of comparisons as the loop below, so the same
    // path. The steps are kept as 64-bit lanes (laneStep[k][node & 7]).
    const bool lanes = gTrellisTwo;
    alignas(32) int64_t laneStep[64][8];
    if (lanes) {
        auto steps = [](int a, int b, int c, int d) { return _mm256_castsi256_pd(_mm256_setr_epi64x(a, b, c, d)); };
        const __m256d dead = _mm256_set1_pd(kDead);
        const __m256d lane3 = _mm256_castsi256_pd(_mm256_setr_epi64x(0, 0, 0, -1));
        const __m256d oneL = _mm256_loadu_pd(one), oneH = _mm256_loadu_pd(one + 4);
        const __m256d twoL = _mm256_loadu_pd(two), twoH = _mm256_loadu_pd(two + 4);
        __m256d L = _mm256_setr_pd(0.0, kDead, kDead, kDead), H = dead;
        auto flushLanes = [&] {
            if (!(zerosLow | zerosHigh))
                return;
            const __m256d z = _mm256_set1_pd(h256 * double(zerosHigh) + l256 * double(zerosLow));
            zerosLow = zerosHigh = 0;
            L = _mm256_add_pd(L, _mm256_blend_pd(z, _mm256_setzero_pd(), 1)); // (not "nothing coded yet")
            H = _mm256_add_pd(H, z);
        };
        // cost + error + weight * bits, as the loop below sums it
        auto arrive = [](__m256d c, __m256d err, __m256d lkv, __m256d bits) {
            return _mm256_add_pd(_mm256_add_pd(c, err), _mm256_mul_pd(lkv, bits));
        };
        auto less = [](__m256d a, __m256d b) { return _mm256_cmp_pd(a, b, _CMP_LT_OQ); };
        // lanes 0 0 1 2: the arrivals from the node below
        auto fromBelow = [](__m256d v) { return _mm256_permute4x64_pd(v, _MM_SHUFFLE(2, 1, 0, 0)); };
        auto takeScalar = [](double& best, int& bestStep, double c, int st) {
            const bool lt = c < best;
            best = lt ? c : best;
            bestStep = lt ? st : bestStep;
        };
        auto lane0 = [](__m256d v, double c) { return _mm256_blend_pd(v, _mm256_castpd128_pd256(_mm_set_sd(c)), 1); };
        auto stepLane0 = [](__m256d v, int st) {
            return _mm256_blend_pd(v, _mm256_castsi256_pd(_mm256_castsi128_si256(_mm_cvtsi32_si128(st))), 1);
        };
        for (int k = end - 1; k >= 0; --k) {
            const double ax = std::abs(x[k]);
            if (ax < 0.5) {
                (k >= highFrom ? zerosHigh : zerosLow) += ce.binCost(sigCtx(k), 0);
                pass |= uint64_t(1) << k;
                continue;
            }
            flushLanes();
            const double lk = k >= highFrom ? h256 : l256;
            const double err0 = ax * scale[k];
            const double errSq0 = err0 * err0;
            const bool notLast = k < count - 1;
            const uint32_t sig1 = notLast ? ce.binCost(sigCtx(k), 1) : 0;
            const double sigMore = notLast ? double(sig1 + ce.binCost(lastCtx(k), 0)) : 0.0;
            const double sigLast = notLast ? double(sig1 + ce.binCost(lastCtx(k), 1)) : 0.0;
            const __m256d lkv = _mm256_set1_pd(lk);
            const __m256d sigH = _mm256_set1_pd(sigMore);
            const __m256d sigL = lane0(sigH, sigLast);
            const int fl = int(ax);
            __m256d sL = _mm256_setzero_pd(), sH = _mm256_setzero_pd();
            if (fl == 0) {
                // 0 (the node stays) or 1.
                mags[k][0] = 0;
                mags[k][1] = 1;
                const double err1 = (ax - 1) * scale[k];
                const __m256d gain1 = _mm256_set1_pd(err1 * err1 - errSq0);
                const __m256d stayBits = _mm256_set1_pd(notLast ? lk * ce.binCost(sigCtx(k), 0) : 0.0);
                if (lowLive) {
                    const __m256d stay = _mm256_add_pd(L, _mm256_blend_pd(stayBits, _mm256_setzero_pd(), 1));
                    const __m256d up = arrive(L, gain1, lkv, _mm256_add_pd(oneL, sigL)); // upE, up1, up2, up3
                    const __m256d upBelow = fromBelow(up);
                    // cE | stay1, upE | up1, stay2 | up2, stay3, up3
                    const __m256d first = _mm256_blend_pd(stay, upBelow, 0b1100);
                    const __m256d second = _mm256_blend_pd(_mm256_blend_pd(upBelow, stay, 0b1100), dead, 1);
                    const __m256d m2 = less(second, first);
                    const __m256d best = _mm256_blendv_pd(first, second, m2);
                    const __m256d st = _mm256_blendv_pd(steps(kEmpty, 1, 1 | kHi, 2 | kHi), steps(0, kEmpty | kHi, 2, 3), m2);
                    const __m256d m3 = _mm256_and_pd(less(up, best), lane3);
                    L = _mm256_blendv_pd(best, up, m3);
                    sL = _mm256_blendv_pd(st, steps(0, 0, 0, 3 | kHi), m3);
                }
                if (highLive) {
                    const __m256d stay = _mm256_add_pd(H, stayBits);
                    const __m256d up = arrive(H, gain1, lkv, _mm256_add_pd(oneH, sigH));
                    const __m256d m = less(up, stay);
                    H = _mm256_blendv_pd(stay, up, m);
                    sH = _mm256_blendv_pd(steps(4, 5, 6, 7), steps(4 | kHi, 5 | kHi, 6 | kHi, 7 | kHi), m);
                }
            } else if (fl == 1) {
                // 1 or 2: a level of 1.5 or more never drops to zero.
                mags[k][0] = 1;
                mags[k][1] = 2;
                const double errLo = (ax - 1) * scale[k], errHi = (ax - 2) * scale[k];
                const __m256d errSqLo = _mm256_set1_pd(errLo * errLo - errSq0), errSqHi = _mm256_set1_pd(errHi * errHi - errSq0);
                __m256d upL = dead, downH = dead, newL = dead, newH = dead;
                if (lowLive) {
                    const __m256d down = arrive(L, errSqLo, lkv, _mm256_add_pd(oneL, sigL)); // downE, down1, down2, down3
                    upL = arrive(L, errSqHi, lkv, _mm256_add_pd(twoL, sigL));
                    // - | downE | down1 | down2, down3
                    const __m256d below = fromBelow(down);
                    const __m256d m = _mm256_and_pd(less(down, below), lane3);
                    newL = _mm256_blend_pd(_mm256_blendv_pd(below, down, m), dead, 1);
                    sL = _mm256_blendv_pd(steps(0, kEmpty, 1, 2), steps(0, 0, 0, 3), m);
                }
                if (highLive) {
                    downH = arrive(H, errSqLo, lkv, _mm256_add_pd(oneH, sigH));
                    const __m256d up = arrive(H, errSqHi, lkv, _mm256_add_pd(twoH, sigH));
                    // (c4 below) | up4, down5 | up5, down6 | up6, down7, up7
                    const __m256d first = fromBelow(up);
                    const __m256d m2 = less(downH, first);
                    const __m256d best = _mm256_blendv_pd(first, downH, m2);
                    const __m256d st = _mm256_blendv_pd(steps(0, 4 | kHi, 5 | kHi, 6 | kHi), steps(0, 5, 6, 7), m2);
                    const __m256d m3 = _mm256_and_pd(less(up, best), lane3);
                    newH = _mm256_blendv_pd(best, up, m3);
                    sH = _mm256_blendv_pd(st, steps(0, 0, 0, 7 | kHi), m3);
                }
                // c4: up1, up2, up3, down4, then 2 from "nothing coded yet"
                alignas(32) double u[4];
                _mm256_store_pd(u, upL);
                double f4 = u[1];
                int s4 = 1 | kHi;
                takeScalar(f4, s4, u[2], 2 | kHi);
                takeScalar(f4, s4, u[3], 3 | kHi);
                takeScalar(f4, s4, _mm256_cvtsd_f64(downH), 4);
                takeScalar(f4, s4, u[0], kEmpty | kHi);
                L = newL;
                H = lane0(newH, f4);
                sH = stepLane0(sH, s4);
                highLive = true;
            } else {
                // Round down or up, both above 1: every path ends in the high group.
                mags[k][0] = int16_t(fl);
                mags[k][1] = int16_t(fl + 1);
                const double errLo = (ax - fl) * scale[k], errHi = (ax - (fl + 1)) * scale[k];
                const __m256d errSqLo = _mm256_set1_pd(errLo * errLo - errSq0), errSqHi = _mm256_set1_pd(errHi * errHi - errSq0);
                const LevelBits lo = levelBits(fl - 1), hi = levelBits(fl);
                // first1 + ones * more1 + zero * more0 + escape + sig (whole numbers: exact in any order)
                auto bits = [&](const LevelBits& lb, int from, __m256d sig) {
                    const __m256d a = _mm256_add_pd(_mm256_loadu_pd(first1 + from),
                                                    _mm256_mul_pd(_mm256_set1_pd(lb.ones), _mm256_loadu_pd(more1 + from)));
                    const __m256d b = _mm256_add_pd(a, _mm256_mul_pd(_mm256_set1_pd(lb.zero), _mm256_loadu_pd(more0 + from)));
                    return _mm256_add_pd(_mm256_add_pd(b, _mm256_set1_pd(lb.escape)), sig);
                };
                __m256d newH = dead;
                if (highLive) {
                    const __m256d down = arrive(H, errSqLo, lkv, bits(lo, 4, sigH));
                    const __m256d up = arrive(H, errSqHi, lkv, bits(hi, 4, sigH));
                    // (c4 below) | down4, up4 | down5, up5 | down6, up6, down7, up7
                    const __m256d first = fromBelow(down), second = fromBelow(up);
                    const __m256d m2 = less(second, first);
                    __m256d best = _mm256_blendv_pd(first, second, m2);
                    __m256d st = _mm256_blendv_pd(steps(0, 4, 5, 6), steps(0, 4 | kHi, 5 | kHi, 6 | kHi), m2);
                    const __m256d m3 = _mm256_and_pd(less(down, best), lane3);
                    best = _mm256_blendv_pd(best, down, m3);
                    st = _mm256_blendv_pd(st, steps(0, 0, 0, 7), m3);
                    const __m256d m4 = _mm256_and_pd(less(up, best), lane3);
                    newH = _mm256_blendv_pd(best, up, m4);
                    sH = _mm256_blendv_pd(st, steps(0, 0, 0, 7 | kHi), m4);
                }
                double f4 = kDead;
                int s4 = 0;
                if (lowLive) {
                    alignas(32) double d[4], u[4];
                    _mm256_store_pd(d, arrive(L, errSqLo, lkv, bits(lo, 0, sigL)));
                    _mm256_store_pd(u, arrive(L, errSqHi, lkv, bits(hi, 0, sigL)));
                    f4 = d[1], s4 = 1;
                    takeScalar(f4, s4, u[1], 1 | kHi);
                    takeScalar(f4, s4, d[2], 2);
                    takeScalar(f4, s4, u[2], 2 | kHi);
                    takeScalar(f4, s4, d[3], 3);
                    takeScalar(f4, s4, u[3], 3 | kHi);
                    takeScalar(f4, s4, d[0], kEmpty);
                    takeScalar(f4, s4, u[0], kEmpty | kHi);
                }
                L = dead;
                H = lane0(newH, f4);
                sH = stepLane0(sH, s4);
                lowLive = false;
                highLive = true;
            }
            _mm256_store_pd(reinterpret_cast<double*>(laneStep[k]), sL);
            _mm256_store_pd(reinterpret_cast<double*>(laneStep[k]) + 4, sH);
        }
        flushLanes();
        alignas(32) double c[8];
        _mm256_store_pd(c, L);
        _mm256_store_pd(c + 4, H);
        cE = c[0], c1 = c[1], c2 = c[2], c3 = c[3], c4 = c[4], c5 = c[5], c6 = c[6], c7 = c[7];
    }
    auto stepAt = [&](int k, int n) { return lanes ? int(laneStep[k][n & 7]) : int(step[k][n]); };
    for (int k = lanes ? -1 : end - 1; k >= 0; --k) {
#else
    auto stepAt = [&](int k, int n) { return int(step[k][n]); };
    for (int k = end - 1; k >= 0; --k) {
#endif
        const double ax = std::abs(x[k]);
        if (ax < 0.5) {
            // Only level 0: every path keeps its node, the ones with a level pay the bits of the zero.
            (k >= highFrom ? zerosHigh : zerosLow) += ce.binCost(sigCtx(k), 0);
            pass |= uint64_t(1) << k;
            continue;
        }
        flushZeros();
        const double lk = k >= highFrom ? h256 : l256; // weight of this position's bits
        const double err0 = ax * scale[k];
        const double errSq0 = err0 * err0;
        // Significance and last bins of this position: a first coded coefficient (from "nothing coded yet") is
        // the last one of the block.
        const bool notLast = k < count - 1;
        const uint32_t sig1 = notLast ? ce.binCost(sigCtx(k), 1) : 0;
        const double sigMore = notLast ? double(sig1 + ce.binCost(lastCtx(k), 0)) : 0.0;
        const double sigLast = notLast ? double(sig1 + ce.binCost(lastCtx(k), 1)) : 0.0;
        const int fl = int(ax);
        int s1 = 0, s2 = 0, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0, sE = 0;
        if (fl == 0) {
            // 0 (the node stays) or 1.
            mags[k][0] = 0;
            mags[k][1] = 1;
            const double err1 = (ax - 1) * scale[k];
            const double gain1 = err1 * err1 - errSq0;
            const double stayBits = notLast ? lk * ce.binCost(sigCtx(k), 0) : 0.0; // (the last position: no path has a level)
            if (lowLive) {
                const double stay1 = c1 + stayBits;
                const double stay2 = c2 + stayBits;
                const double stay3 = c3 + stayBits;
                const double upE = (cE + gain1) + lk * (one[kEmpty] + sigLast);
                const double up1 = (c1 + gain1) + lk * (one[1] + sigMore);
                const double up2 = (c2 + gain1) + lk * (one[2] + sigMore);
                const double up3 = (c3 + gain1) + lk * (one[3] + sigMore);
                sE = kEmpty;
                c1 = stay1, s1 = 1;
                take(c1, s1, upE, kEmpty | kHi);
                c2 = up1, s2 = 1 | kHi;
                take(c2, s2, stay2, 2);
                c3 = up2, s3 = 2 | kHi;
                take(c3, s3, stay3, 3);
                take(c3, s3, up3, 3 | kHi);
            }
            if (highLive) {
                auto stayOrUp = [&](double& c, int& s, int n) {
                    const double stay = c + stayBits;
                    const double up = (c + gain1) + lk * (one[n] + sigMore);
                    c = stay, s = n;
                    take(c, s, up, n | kHi);
                };
                stayOrUp(c4, s4, 4);
                stayOrUp(c5, s5, 5);
                stayOrUp(c6, s6, 6);
                stayOrUp(c7, s7, 7);
            }
        } else if (!gTrellisTwo) {
            // H264_TRELLIS3: 0, round down or round up, node by node.
            mags[k][0] = 0;
            mags[k][1] = int16_t(fl);
            mags[k][2] = int16_t(fl + 1);
            const double errLo = (ax - fl) * scale[k], errHi = (ax - (fl + 1)) * scale[k];
            const double gainLo = errLo * errLo - errSq0, gainHi = errHi * errHi - errSq0;
            const double stayBits = notLast ? lk * ce.binCost(sigCtx(k), 0) : 0.0;
            const LevelBits lo = levelBits(fl - 1), hi = levelBits(fl);
            const double cost[9] = {kDead, c1, c2, c3, c4, c5, c6, c7, cE};
            double fresh[9];
            int st[9] = {};
            std::fill(fresh, fresh + 9, kDead);
            for (int n = 1; n < 9; ++n) {
                const int state = n == kEmpty ? 0 : n;
                const double sig = n == kEmpty ? sigLast : sigMore;
                const double bitsLo = fl == 1 ? one[n] : first1[n] + lo.ones * more1[n] + lo.zero * more0[n] + lo.escape;
                const double bitsHi = first1[n] + hi.ones * more1[n] + hi.zero * more0[n] + hi.escape;
                take(fresh[n], st[n], n == kEmpty ? cost[n] : cost[n] + stayBits, n);
                const int toLo = fl == 1 ? (state < 4 ? std::min(state + 1, 3) : state) : state < 4 ? 4 : std::min(state + 1, 7);
                const int toHi = state < 4 ? 4 : std::min(state + 1, 7);
                take(fresh[toLo], st[toLo], (cost[n] + gainLo) + lk * (bitsLo + sig), n | kHi);
                take(fresh[toHi], st[toHi], (cost[n] + gainHi) + lk * (bitsHi + sig), n | 2 * kHi);
            }
            c1 = fresh[1], c2 = fresh[2], c3 = fresh[3], c4 = fresh[4], c5 = fresh[5], c6 = fresh[6], c7 = fresh[7];
            cE = fresh[kEmpty];
            s1 = st[1], s2 = st[2], s3 = st[3], s4 = st[4], s5 = st[5], s6 = st[6], s7 = st[7], sE = st[kEmpty];
            highLive = true;
        } else if (fl == 1) {
            // 1 or 2: a level of 1.5 or more never drops to zero.
            mags[k][0] = 1;
            mags[k][1] = 2;
            const double errLo = (ax - 1) * scale[k], errHi = (ax - 2) * scale[k];
            const double errSqLo = errLo * errLo - errSq0, errSqHi = errHi * errHi - errSq0; // (against level 0)
            double f1 = kDead, f2 = kDead, f3 = kDead, f4 = kDead;
            if (lowLive) {
                const double downE = (cE + errSqLo) + lk * (one[kEmpty] + sigLast);
                const double down1 = (c1 + errSqLo) + lk * (one[1] + sigMore);
                const double down2 = (c2 + errSqLo) + lk * (one[2] + sigMore);
                const double down3 = (c3 + errSqLo) + lk * (one[3] + sigMore);
                const double up1 = (c1 + errSqHi) + lk * (two[1] + sigMore);
                const double up2 = (c2 + errSqHi) + lk * (two[2] + sigMore);
                const double up3 = (c3 + errSqHi) + lk * (two[3] + sigMore);
                f1 = downE, s1 = kEmpty;
                f2 = down1, s2 = 1;
                f3 = down2, s3 = 2;
                take(f3, s3, down3, 3);
                f4 = up1, s4 = 1 | kHi;
                take(f4, s4, up2, 2 | kHi);
                take(f4, s4, up3, 3 | kHi);
            }
            if (highLive) {
                const double down4 = (c4 + errSqLo) + lk * (one[4] + sigMore);
                const double down5 = (c5 + errSqLo) + lk * (one[5] + sigMore);
                const double down6 = (c6 + errSqLo) + lk * (one[6] + sigMore);
                const double down7 = (c7 + errSqLo) + lk * (one[7] + sigMore);
                const double up4 = (c4 + errSqHi) + lk * (two[4] + sigMore);
                const double up5 = (c5 + errSqHi) + lk * (two[5] + sigMore);
                const double up6 = (c6 + errSqHi) + lk * (two[6] + sigMore);
                const double up7 = (c7 + errSqHi) + lk * (two[7] + sigMore);
                take(f4, s4, down4, 4);
                c5 = up4, s5 = 4 | kHi;
                take(c5, s5, down5, 5);
                c6 = up5, s6 = 5 | kHi;
                take(c6, s6, down6, 6);
                c7 = up6, s7 = 6 | kHi;
                take(c7, s7, down7, 7);
                take(c7, s7, up7, 7 | kHi);
            }
            if (lowLive)
                take(f4, s4, (cE + errSqHi) + lk * (two[kEmpty] + sigLast), kEmpty | kHi);
            c1 = f1, c2 = f2, c3 = f3, c4 = f4;
            cE = kDead;
            highLive = true;
        } else {
            // Round down or up, both above 1: every path ends in the high group.
            mags[k][0] = int16_t(fl);
            mags[k][1] = int16_t(fl + 1);
            const double errLo = (ax - fl) * scale[k], errHi = (ax - (fl + 1)) * scale[k];
            const double errSqLo = errLo * errLo - errSq0, errSqHi = errHi * errHi - errSq0; // (against level 0)
            const LevelBits lo = levelBits(fl - 1), hi = levelBits(fl);
            auto down = [&](double c, int n, double sig) {
                return (c + errSqLo) + lk * (first1[n] + lo.ones * more1[n] + lo.zero * more0[n] + lo.escape + sig);
            };
            auto up = [&](double c, int n, double sig) {
                return (c + errSqHi) + lk * (first1[n] + hi.ones * more1[n] + hi.zero * more0[n] + hi.escape + sig);
            };
            double f4 = kDead;
            if (highLive) {
                const double down4 = down(c4, 4, sigMore), up4 = up(c4, 4, sigMore);
                const double down5 = down(c5, 5, sigMore), up5 = up(c5, 5, sigMore);
                const double down6 = down(c6, 6, sigMore), up6 = up(c6, 6, sigMore);
                const double down7 = down(c7, 7, sigMore), up7 = up(c7, 7, sigMore);
                c5 = down4, s5 = 4;
                take(c5, s5, up4, 4 | kHi);
                c6 = down5, s6 = 5;
                take(c6, s6, up5, 5 | kHi);
                c7 = down6, s7 = 6;
                take(c7, s7, up6, 6 | kHi);
                take(c7, s7, down7, 7);
                take(c7, s7, up7, 7 | kHi);
            }
            if (lowLive) {
                f4 = down(c1, 1, sigMore), s4 = 1;
                take(f4, s4, up(c1, 1, sigMore), 1 | kHi);
                take(f4, s4, down(c2, 2, sigMore), 2);
                take(f4, s4, up(c2, 2, sigMore), 2 | kHi);
                take(f4, s4, down(c3, 3, sigMore), 3);
                take(f4, s4, up(c3, 3, sigMore), 3 | kHi);
                take(f4, s4, down(cE, kEmpty, sigLast), kEmpty);
                take(f4, s4, up(cE, kEmpty, sigLast), kEmpty | kHi);
            }
            c4 = f4;
            c1 = c2 = c3 = cE = kDead;
            lowLive = false;
            highLive = true;
        }
        uint8_t* st = step[k];
        st[1] = uint8_t(s1), st[2] = uint8_t(s2), st[3] = uint8_t(s3), st[4] = uint8_t(s4);
        st[5] = uint8_t(s5), st[6] = uint8_t(s6), st[7] = uint8_t(s7), st[kEmpty] = uint8_t(sE);
    }
    flushZeros();
    const double cost[9] = {kDead, c1, c2, c3, c4, c5, c6, c7, cE};
    int best = kEmpty;
    double bestCost = cost[kEmpty] + cbf0;
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
        const int st = stepAt(k, n);
        const int mag = mags[k][st >> 4];
        out[k] = x[k] < 0 ? -mag : mag;
        nz |= uint64_t(mag != 0) << k;
        n = st & 15;
    }
    if (nonZero)
        *nonZero = nz;
    // (Without energy the squared error after the last coefficient of half a level is left out, as it always was.)
    return (energy ? zero : zero - tail) + bestCost;
}


// The values of this file's S264_DEV switches in the x86-64-v3 build (DevEnv.h).
S264_DEV_FILL_ALL(devFillCabac);

} // namespace schneidi::h264
