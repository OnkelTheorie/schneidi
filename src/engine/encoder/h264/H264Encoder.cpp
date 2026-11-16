// schneidi's own H.264 encoder (prototype).
// Clause numbers refer to ITU-T H.264. The encoder mirrors the decoder's
// reconstruction exactly (8.3, 8.5), so later P frames can reference it.
#include "Simd.h" // first: no floating-point contraction (see there)
#include "H264Encoder.h"
#include "FrameCoder.h"

#include "Cabac.h"
#include "Cavlc.h"
#include "Satd.h"
#include "Transform.h"
#include "Deblock.h"
#include "ThreadErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <system_error>
#include <utility>
#include <condition_variable>
#include <mutex>


namespace schneidi::h264 {

// H264Cabac.cpp
// nonZero (optional): bit k set where out[k] != 0.
double cabacTrellis(const CabacEncoder& ce, int cat, const double* x, const double* scale, int count, double lambda,
                    int cbfInc, int* out, double* energy = nullptr, uint64_t* nonZero = nullptr, double lambdaHigh = -1.0,
                    int highFrom = 0, double* zeroCost = nullptr);

// Phase timing per picture (H264_TIMING=1): where the serial parts are.
static const bool gTiming = std::getenv("H264_TIMING") != nullptr;
struct PhaseClock {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    double ms[8] = {};
    void lap(int phase)
    {
        if (!gTiming)
            return;
        const auto now = std::chrono::steady_clock::now();
        ms[phase] += std::chrono::duration<double, std::milli>(now - t).count();
        t = now;
    }
};

namespace {

// Macroblock statistics (H264_STATS=1), for comparisons with other encoders' logs.
const bool gStats = std::getenv("H264_STATS") != nullptr;
std::atomic<long> gStatInter{0}, gStatInterCbp{0}, gStatT8{0};
std::mutex gStatMutex;
double gStatT8LossIf4[20] = {}, gStatT8LossIf8[20] = {}; // RD cost lost if only 4x4 / only 8x8 were coded
double gStatT8Total = 0;
double gStatSplitN[20] = {}, gStatSplitWin[20] = {}, gStatSplitLoss[20] = {}, gStatInterTotal = 0; // by whole.cost / sqrt(lambda)
std::atomic<long> gStatT8Try[20] = {}, gStatT8Win[20] = {}; // by sa8d / satd of the chosen layout, 0.05 steps from 0.6
std::atomic<long> gStatIntraTry[4] = {}, gStatIntraWin[4] = {}; // by ratio of the intra estimate to the inter cost
// Mode decision by estimate (see encodeMbP): per bucket of the estimates' distance the cases, wrong
// picks and the RD cost lost by trusting the estimate.
double gStatEstT8[8][3] = {}, gStatEstSplit[8][3] = {}, gStatEstTotal = 0;
// B macroblocks: skip, direct, 16x16 L0 / L1 / Bi, 16x8 or 8x16, 8x8, intra, and of the coded inter ones the 8x8 transform.
std::atomic<long> gStatB[9] = {};
// Experiments (H264_EXP bit mask), for A/B measurements only.
const int gExp = std::getenv("H264_EXP") ? std::atoi(std::getenv("H264_EXP")) : 0;
// Deblocking filter offsets (slice_alpha_c0_offset_div2 / slice_beta_offset_div2, -6..6), for
// measurements only: H264_DBA / H264_DBB.
const int gDeblockA = std::clamp(std::getenv("H264_DBA") ? std::atoi(std::getenv("H264_DBA")) : 0, -6, 6);
const int gDeblockB = std::clamp(std::getenv("H264_DBB") ? std::atoi(std::getenv("H264_DBB")) : 0, -6, 6);
// AQ texture measure (experiment, H264_AQEDGE): 0 = variance of the macroblock, 1 = mean of its
// 8x8 quadrants' variances, 2 = mean of the two flattest quadrants.
const int gAqEdge = std::getenv("H264_AQEDGE") ? std::atoi(std::getenv("H264_AQEDGE")) : 0;
// Per thread (rows of a picture run on several threads): intra try in a P picture without Intra
// 8x8 / 16x16 (experiments).
thread_local bool tIntraSkip8 = false, tIntraSkip16 = false;
// H264_MB_TRACE: the costs of the last P macroblock's full decision (see FrameCoder::traceMb).
thread_local char tDecision[160] = {};
// Intra try in an inter picture: the caller's Intra 16x16 SATD estimate (-1 = none). With it the
// three intra types are first compared by SATD (+ mode bits) and only those within kIntraSel of the
// best are coded in full (H264_INTRASEL = factor, 0 = off: all of them as before). Measured with
// the intra try threshold 1.2 and the limit against inter 1.0 (two-pass, AQ + tree, 600 pictures CS2,
// BD-rate PSNR-Y / SSIM-Y / instructions): threshold 1.2 alone +0.17 / +0.11 % / -5.9 %, all three
// +0.30 / +0.22 % / -7.8 %; pre-selection 1.1 with limit 0.9 +0.31 / +0.53 % / -6.4 % (no).
thread_local int tIntraEst16 = -1;
const double gIntraSel = std::getenv("H264_INTRASEL") ? std::atof(std::getenv("H264_INTRASEL")) : 1.2;
// With the pre-selection: an intra type is also given up once its SATD exceeds this many times the
// inter cost (the motion search's SATD + mode bits; H264_INTRAINTER, 0 = no such limit).
thread_local double tInterCost = -1;
const double gIntraInter = std::getenv("H264_INTRAINTER") ? std::atof(std::getenv("H264_INTRAINTER")) : 1.0;
// Set by encodeMbIntra when every intra type was given up (nothing coded).
thread_local bool tIntraNone = false;
// Intra try in P pictures only when its estimate is below gIntraFactor x the inter cost (2.0 before 11.11.:
// -10 % instructions for +0.17 % BD-rate), and in the borderline case (estimate above the inter
// cost) without Intra 8x8 / 16x16 (-8 % for +0.2 %). H264_FULLINTRA=1 restores the full search.
const bool gFullIntra = std::getenv("H264_FULLINTRA") != nullptr;
// 1.5 -> 1.2 (12.11., with the pre-selection below): see gIntraSel. 1.2 -> 1.1 (14.11.; two-pass,
// 4 rates, BD-rate PSNR-Y / SSIM-Y on CS2, camera, calm, animation; instructions two-pass 8 Mbit/s
// CS2): +0.14 / +0.09, +0.14 / +0.02, -0.03 / -0.08, +0.18 / +0.77 % for -1.6 %; 1.0: +0.28 / +0.20,
// +0.44 / +0.14, +0.06 / -0.12, -0.11 / -0.84 % for -3.7 %.
const double gIntraFactor = std::getenv("H264_INTRAF") ? std::atof(std::getenv("H264_INTRAF")) : gFullIntra ? 2.0 : 1.1;
// B pictures: 16x8 and 8x16 partitions besides 16x16 (H264_BPARTS=0 turns them off, for measurements).
const bool gBParts = !std::getenv("H264_BPARTS") || std::atoi(std::getenv("H264_BPARTS")) != 0;
// Psycho-visual RD of the macroblock a thread is deciding (FrameCoder::psyBegin): the weight of a
// unit of distance in its cost (0 = off) and the Hadamard coefficient magnitudes of the source's
// four 8x8 luma blocks (hadamardMags8x8U8).
struct PsyMb {
    double weight = 0;
    alignas(16) int16_t src[4][128];
};
thread_local PsyMb tPsy;
// With psy, the inter candidate that won is coded once more with a finer trellis quantisation
// (lambda times gPsyFine for its coefficient bits): fewer small coefficients dropped, so more of
// the source's detail at more bits, and the psy cost decides between the two. The mode decision
// alone has little to choose from: its candidates are all quantised for squared error.
// Measured at fixed QP 30-38, 60 pictures each of CS2, a slow zoom with grain and camera footage,
// at equal bitrate against psy off (block AC energy kept / high-frequency energy kept / PSNR-Y),
// psyRd 1.0: mode decision alone -0.2..+0.2 points / -0.5..-0.1 / -0.04..-0.18 dB; with this
// candidate +0.2..0.9 / +2.5..3.2 / -0.08..-0.14; with the intra one below as well +0.5..1.1 /
// +2.9..4.0 / -0.08..-0.11; psyRd 2.0 +1.2..1.8 / +5.7..7.2 / -0.20..-0.41. H264_PSYQ overrides
// (0 = no such candidate).
const double gPsyFine = std::getenv("H264_PSYQ") ? std::atof(std::getenv("H264_PSYQ")) : 0.25;
// The finer quantisation leaves the lowest frequencies as they are (scan positions below these, 4x4
// and 8x8 transform): a DC or first AC level more in one block and not in its neighbour is a
// visible step at high QP, not texture. H264_PSYQ4 / H264_PSYQ8 override.
const int gPsyFineFrom4 = std::getenv("H264_PSYQ4") ? std::atoi(std::getenv("H264_PSYQ4")) : 3;
const int gPsyFineFrom8 = std::getenv("H264_PSYQ8") ? std::atoi(std::getenv("H264_PSYQ8")) : 6;
// The same for intra macroblocks, whose quantisation rounds down below 2/3 of a step: the chosen
// type once more rounding to nearest (tIntraFine: offset gPsyIntraRound instead of 1/3), and the
// psy cost decides. H264_PSYIQ overrides (0 = no such candidate).
const double gPsyIntraRound = std::getenv("H264_PSYIQ") ? std::atof(std::getenv("H264_PSYIQ")) : 0.5;
thread_local bool tIntraFine = false;
// B pictures: B_8x8 with direct 8x8 blocks (H264_B8X8=0 turns it off, for measurements).
const bool gB8x8 = !std::getenv("H264_B8X8") || std::atoi(std::getenv("H264_B8X8")) != 0;
// B pictures, prediction from both lists: rounds of the joint refinement of the two vectors (0 =
// the average of the two vectors as found), tried where the average is within this factor of the
// better single list. In long runs only (a slow zoom: +0.06 dB at 2 Mbit/s, and most macroblocks
// are skipped before it); in short runs it costs +16 % instructions for 0.6 % of the B pictures'
// bits (a pan with B pictures). H264_BIREFINE sets the rounds for all runs.
const int gBiRefineEnv = std::getenv("H264_BIREFINE") ? std::atoi(std::getenv("H264_BIREFINE")) : -1;
constexpr int kBiRefineLong = 2;
// B pictures: the splits (16x8, 8x16, 8x8) are not searched where the 16x16 estimate is below this
// many lambda (as for P pictures, H264_SPLITSKIP). 0 = always.
const double gBSplitSkip = std::getenv("H264_BSPLITSKIP") ? std::atof(std::getenv("H264_BSPLITSKIP")) : 200.0;
const double gBiRefineNear = std::getenv("H264_BIREFINE_NEAR") ? std::atof(std::getenv("H264_BIREFINE_NEAR")) : 1.15;
// B pictures: candidates coded for real (direct aside): the best 16x16, the best split and, with
// H264_BRD, the other choices of lists of the 16x16 layout.
constexpr int kMaxBCands = 4;
const double gBRd = std::getenv("H264_BRD") ? std::atof(std::getenv("H264_BRD")) : 0.0;
// B pictures keep their reference pictures' texture (EncoderConfig::bSharp, FrameCoder::encodeMbB).
// H264_BSHARP overrides the weight (0 = off).
// - In the search's SATD estimates (list 0, list 1 or both per partition, direct or searched per
//   8x8 block) the lost detail counts gBSharpEst times its SATD-like size (the magnitudes summed,
//   halved). H264_BSHARPEST overrides.
// - The lost detail counts in full where the average is no closer to the source than the better
//   single prediction (SATD ratio 1) and not at all where its SATD is below gBSharpKeep of the
//   single one's, linear between. Two pictures with noise of their own (camera footage, grain)
//   lose "detail" in their average too, but there the average is the better picture - that is
//   what B pictures gain. Measured (wall = sharpness step of the drifting wall of the A/B clip,
//   x264 0.027; BD-rate PSNR-Y against x264 of camera footage / a slow zoom, without: -13.3 /
//   -21.9 %): weight 1 without this 0.036, -6.0 / -15.5 %; weight 1.5 with 0.85: 0.038, -9.2 /
//   -15.8 %. H264_BSHARPKEEP overrides (0 = always in full).
// - Not in long runs (more than four B pictures, as placed on still or slowly zooming material):
//   their B pictures do not pulse more than x264's (sharpness step 0.018 against 0.020) and the
//   cost would take 6 points of their gain. H264_BSHARPLONG=1 applies it there too.
const double gBSharpEnv = std::getenv("H264_BSHARP") ? std::atof(std::getenv("H264_BSHARP")) : -1.0;
const double gBSharpEst = std::getenv("H264_BSHARPEST") ? std::atof(std::getenv("H264_BSHARPEST")) : 1.0;
const double gBSharpKeep = std::getenv("H264_BSHARPKEEP") ? std::atof(std::getenv("H264_BSHARPKEEP")) : 0.85;
const bool gBSharpLong = std::getenv("H264_BSHARPLONG") && std::atoi(std::getenv("H264_BSHARPLONG")) != 0;
// Texture keepers (psy-RD, bSharp) by the source macroblock's texture (the AQ's energy, log2 of 256 x variance + 1):
// none below gFlatLo, in full from gFlatHi. A flat or smoothly shaded macroblock has no texture to keep - there
// both only cost: psy codes quantisation noise as "detail", bSharp refuses the average of two pictures, which on
// a gradient is exactly the picture in between. H264_FLATLO / H264_FLATHI override; H264_FLATHI=0: off (the
// stream before 16.11.2026).
const double gFlatLo = std::getenv("H264_FLATLO") ? std::atof(std::getenv("H264_FLATLO")) : 6.0;
const double gFlatHi = std::getenv("H264_FLATHI") ? std::atof(std::getenv("H264_FLATHI")) : 9.0;
inline double textureShare(float energy)
{
    if (!(gFlatHi > 0))
        return 1.0;
    return std::clamp((double(energy) - gFlatLo) / std::max(1e-6, gFlatHi - gFlatLo), 0.0, 1.0);
}
// B pictures: transform by estimate (see encodeMbB; EncoderConfig::modeEstimate). H264_BEST=0/1 overrides.
const int gBEstEnv = std::getenv("H264_BEST") ? std::atoi(std::getenv("H264_BEST")) : -1;
// The lookahead's vectors along the planned references (Encoder::takeLookaheadVectors) as start
// points of the motion search: 1 = B pictures, 2 = also P pictures after a run of B pictures
// (default), 0 = none (H264_LASTART). The vectors exist where the tree or the rate control asked
// for the searches; H264_LASTART set makes them also without (fixed QP without the tree, for
// measurements).
const int gLaStart = std::getenv("H264_LASTART") ? std::atoi(std::getenv("H264_LASTART")) : 2;
const bool gLaStartForce = std::getenv("H264_LASTART") && std::atoi(std::getenv("H264_LASTART")) > 0;
double gPhaseMs[8] = {};
double gWaitMs = 0; // summed over threads
int gPhaseFrames = 0;

// 4x4 frame zig-zag scan: scan index -> raster index (y * 4 + x).
constexpr int kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

// Quantisation multipliers and dequantisation scales (flat matrices), by
// QP % 6 and position class (0: both even, 1: both odd, 2: mixed).
constexpr int kQuantMf[6][3] = {
    {13107, 5243, 8066}, {11916, 4660, 7490}, {10082, 4194, 6554},
    {9362, 3647, 5825},  {8192, 3355, 5243},  {7282, 2893, 4559},
};
constexpr int kDequantV[6][3] = {
    {10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23},
};

// The two row patterns of a 4x4 block's position classes (rows 0/2: 0 2 0 2, rows 1/3: 2 1 2 1) for
// the SIMD kernels of Transform.h: quantisation multipliers and dequantisation scales by QP % 6.
struct RowPatterns {
    int mf[6][8], v[6][8];
    constexpr RowPatterns() : mf(), v()
    {
        constexpr int kClass[8] = {0, 2, 0, 2, 2, 1, 2, 1};
        for (int q = 0; q < 6; ++q)
            for (int i = 0; i < 8; ++i) {
                mf[q][i] = kQuantMf[q][kClass[i]];
                v[q][i] = kDequantV[q][kClass[i]];
            }
    }
};
constexpr RowPatterns kRows;

constexpr int posClass(int raster)
{
    const int y = raster >> 2, x = raster & 3;
    if (((x | y) & 1) == 0)
        return 0;
    if ((x & 1) && (y & 1))
        return 1;
    return 2;
}

// Chroma QP from table 8-15; qp = the luma QP plus chroma_qp_index_offset.
int chromaQp(int qp)
{
    static const int kTable[22] = {29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36,
                                   36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39};
    qp = std::clamp(qp, 0, 51);
    return qp < 30 ? qp : kTable[qp - 30];
}

// Intra 4x4 / 8x8 direction search: all nine modes in order, or (experiment H264_EXP & 64) DC,
// vertical, horizontal and the predicted mode first, then only the directions next to the best
// one (angular order HU - H - HD - DDR - VR - V - VL - DDL). tryMode(mode) returns its cost.
template <class F>
void intraModeSearch(F&& tryMode, int predMode)
{
    if (!(gExp & 64)) {
        for (int mode = 0; mode < 9; ++mode)
            tryMode(mode);
        return;
    }
    int cost[9];
    std::fill(cost, cost + 9, 1 << 30);
    for (int mode : {2, 0, 1})
        cost[mode] = tryMode(mode);
    if (predMode > 2)
        cost[predMode] = tryMode(predMode);
    auto test = [&](int mode) {
        if (cost[mode] == (1 << 30) && mode != predMode)
            cost[mode] = tryMode(mode);
    };
    if (cost[0] <= cost[1] && cost[0] <= cost[2]) { // vertical: its neighbours, then one further
        test(5);
        test(7);
        test(cost[5] < cost[7] ? 4 : 3);
    } else if (cost[1] <= cost[2]) { // horizontal
        test(6);
        test(8);
        if (cost[6] < cost[1])
            test(4);
    } else { // DC: the two diagonals
        test(3);
        test(4);
    }
}

inline uint8_t clip1(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// One step of a spin wait: a pause for the hyperthread sibling, then let others run.
// Waits until counter >= need: a short spin (the wavefront usually catches up within
// microseconds), then sleeps in the kernel (atomic wait) instead of burning the hyperthread
// sibling's time. Writers call notify_all after every store that someone may wait for.
// Spin length before sleeping: long while the threads have a core each (sleeping costs latency;
// 4 threads on 6 cores: +6 % with 8192 instead of 128), short once they share cores with SMT
// siblings (10 threads on 6 cores: 128 is 4 % faster than 8192). H264_SPIN overrides.
int spinFor(int threads)
{
    if (const char* e = std::getenv("H264_SPIN"))
        return std::atoi(e);
    const int hw = int(std::thread::hardware_concurrency());
    return hw > 0 && 2 * threads <= hw ? 8192 : 128;
}

inline void waitAtLeast(const std::atomic<int>& counter, int need, int spin = 128)
{
    for (int i = 0; i < spin; ++i) {
        if (counter.load(std::memory_order_acquire) >= need)
            return;
        cpuRelax();
    }
    for (int v = counter.load(std::memory_order_acquire); v < need; v = counter.load(std::memory_order_acquire))
        counter.wait(v, std::memory_order_acquire);
}

// Raises a progress counter (never lowers it: after a failure every counter is pushed to its end
// so no thread waits forever, and a late store of a running row must not undo that).
inline void raiseTo(std::atomic<int>& counter, int v)
{
    int cur = counter.load(std::memory_order_relaxed);
    while (cur < v && !counter.compare_exchange_weak(cur, v, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
    counter.notify_all();
}

// Phases of a wavefront row (FrameCoder::WaveState::rowPhase).
enum : uint8_t { kRowNew = 0, kRowMbs = 1, kRowTail = 2, kRowPublish = 3, kRowDone = 4 };

inline void spinWait()
{
    cpuRelax();
    std::this_thread::yield();
}

// Blocks in luma4x4BlkIdx order -> (x, y) in 4x4 units inside the macroblock.
constexpr int kBlkX[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
constexpr int kBlkY[16] = {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3};

// 4x4 Hadamard (used for the luma DC both ways).
inline int quantize(int coef, int mf, int qbits, int offset)
{
    const int mag = int((int64_t(std::abs(coef)) * mf + offset) >> qbits);
    const int level = std::min(mag, kMaxLevel);
    return coef < 0 ? -level : level;
}

// Residual group threshold of inter luma (4x4 transform), in bits at the macroblock's lambda: an
// 8x8 group of four blocks is coded only when they together gain more than kGroupBits (see
// planInterLuma). Fitted by fixed-QP BD-rate on our test material (0, 3, 6, 10, 15 bits tried; a
// further macroblock-level threshold over all kept groups gained nothing). H264_DECG overrides it.
const double kGroupBits = std::getenv("H264_DECG") ? std::atof(std::getenv("H264_DECG")) : 3.0;
// Bit model of a sparse 4x4 block (every coefficient below 1.5 levels) for the paths without the
// CABAC trellis: block overhead + bits per non-zero level, fitted by least squares to the trellis'
// own bit counts on our test material (H264_DECLOG=1 prints the fit).
const double kBlkBits = std::getenv("H264_DECA") ? std::atof(std::getenv("H264_DECA")) : 0.0;
const double kNzBits = std::getenv("H264_DECB") ? std::atof(std::getenv("H264_DECB")) : 3.7;

// Fit data of the bit model (H264_DECLOG=1): normal equations of bits ~ 1 + nonzero.
const bool gDecLog = std::getenv("H264_DECLOG") != nullptr;
std::mutex gDecLogMutex;
struct DecFit {
    double m[2][2] = {}, v[2] = {}, sq = 0;
    long n = 0;
    ~DecFit()
    {
        if (!gDecLog || n == 0)
            return;
        const double d = m[0][0] * m[1][1] - m[0][1] * m[1][0];
        const double a = (v[0] * m[1][1] - m[0][1] * v[1]) / d, b = (m[0][0] * v[1] - m[1][0] * v[0]) / d;
        const double mean = v[0] / double(n);
        std::fprintf(stderr, "declog blocks=%ld bits = %.3f + %.3f*nz (mean %.2f, rms %.2f)\n", n, a, b, mean,
                     std::sqrt(std::max(0.0, sq / double(n) - mean * mean)));
    }
} gDecFit;

// Squared pixel error the levels of a 4x4 block remove (transform domain; x = real-valued levels
// in scan order, scale = pixel error per level unit). Sets sparse when every coefficient is below
// 1.5 levels (only then can the block drop to zero without a large error) and nz to the count of
// non-zero levels.
double levelGain(const int* raster, const double* x, const double* scale, bool& sparse, int& nz)
{
    double dd = 0;
    nz = 0;
    sparse = true;
    for (int s = 0; s < 16; ++s) {
        if (std::abs(x[s]) >= 1.5)
            sparse = false;
        const int l = raster[kZigzag[s]];
        if (l == 0)
            continue;
        const double e0 = x[s] * scale[s], e1 = (x[s] - l) * scale[s];
        dd += e0 * e0 - e1 * e1;
        ++nz;
    }
    return dd;
}

// What coding a 4x4 block's levels gains over dropping them (squared error + lambda * bits), for
// the paths without the trellis: the bits come from the fitted model; a block that is not sparse
// is always worth coding (a huge gain, like the trellis, which never drops such a block).
double sparseLevelGain(const int* raster, const double* x, const double* scale, double lambda)
{
    bool sparse;
    int nz;
    const double dd = levelGain(raster, x, scale, sparse, nz);
    if (nz == 0)
        return 0.0;
    if (!sparse)
        return 1e300;
    return dd - lambda * (kBlkBits + kNzBits * nz);
}

// Records one trellis-coded sparse block for the bit-model fit (H264_DECLOG=1).
void decLogSample(const int* raster, const double* x, const double* scale, double lambda, double gainJ)
{
    bool sparse;
    int nz;
    const double dd = levelGain(raster, x, scale, sparse, nz);
    if (!sparse || nz == 0 || lambda <= 0)
        return;
    const double measured = (dd - gainJ) / lambda; // bits of the levels over the empty block
    std::lock_guard<std::mutex> lk(gDecLogMutex);
    gDecFit.m[0][0] += 1;
    gDecFit.m[0][1] += nz;
    gDecFit.m[1][0] += nz;
    gDecFit.m[1][1] += double(nz) * nz;
    gDecFit.v[0] += measured;
    gDecFit.v[1] += nz * measured;
    gDecFit.sq += measured * measured;
    ++gDecFit.n;
}

int sad(const uint8_t* a, const uint8_t* b, int n)
{
    int s = 0;
    for (int i = 0; i < n; ++i)
        s += std::abs(int(a[i]) - int(b[i]));
    return s;
}

// Intra 16x16 luma prediction (8.3.3). Modes: 0 vertical, 1 horizontal, 2 DC, 3 plane.
void predictLuma16(int mode, const uint8_t* rec, int stride, bool left, bool top, uint8_t* pred)
{
    const uint8_t* above = rec - stride;
    switch (mode) {
    case 0:
        for (int y = 0; y < 16; ++y)
            std::memcpy(pred + y * 16, above, 16);
        break;
    case 1:
        for (int y = 0; y < 16; ++y)
            std::memset(pred + y * 16, rec[y * stride - 1], 16);
        break;
    case 2: {
        int sum = 0, dc = 128;
        if (left && top) {
            for (int i = 0; i < 16; ++i)
                sum += above[i] + rec[i * stride - 1];
            dc = (sum + 16) >> 5;
        } else if (left) {
            for (int i = 0; i < 16; ++i)
                sum += rec[i * stride - 1];
            dc = (sum + 8) >> 4;
        } else if (top) {
            for (int i = 0; i < 16; ++i)
                sum += above[i];
            dc = (sum + 8) >> 4;
        }
        std::memset(pred, dc, 256);
        break;
    }
    case 3: {
        auto p = [&](int x, int y) { return int(rec[y * stride + x]); }; // x, y may be -1
        int h = 0, v = 0;
        for (int i = 0; i < 8; ++i) {
            h += (i + 1) * (p(8 + i, -1) - p(6 - i, -1));
            v += (i + 1) * (p(-1, 8 + i) - p(-1, 6 - i));
        }
        const int a = 16 * (p(-1, 15) + p(15, -1));
        const int b = (5 * h + 32) >> 6;
        const int c = (5 * v + 32) >> 6;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                pred[y * 16 + x] = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
        break;
    }
    }
}

// Intra chroma prediction 8x8 (8.3.4). Modes: 0 DC, 1 horizontal, 2 vertical, 3 plane.
void predictChroma8(int mode, const uint8_t* rec, int stride, bool left, bool top, uint8_t* pred)
{
    const uint8_t* above = rec - stride;
    switch (mode) {
    case 0:
        for (int by = 0; by < 2; ++by) {
            for (int bx = 0; bx < 2; ++bx) {
                int sumTop = 0, sumLeft = 0;
                for (int i = 0; i < 4; ++i) {
                    if (top)
                        sumTop += above[bx * 4 + i];
                    if (left)
                        sumLeft += rec[(by * 4 + i) * stride - 1];
                }
                int dc = 128;
                const bool diagonal = (bx == by);
                if (diagonal) {
                    if (top && left)
                        dc = (sumTop + sumLeft + 4) >> 3;
                    else if (left)
                        dc = (sumLeft + 2) >> 2;
                    else if (top)
                        dc = (sumTop + 2) >> 2;
                } else if (bx == 1) { // top-right block prefers the top samples
                    if (top)
                        dc = (sumTop + 2) >> 2;
                    else if (left)
                        dc = (sumLeft + 2) >> 2;
                } else { // bottom-left block prefers the left samples
                    if (left)
                        dc = (sumLeft + 2) >> 2;
                    else if (top)
                        dc = (sumTop + 2) >> 2;
                }
                for (int y = 0; y < 4; ++y)
                    std::memset(pred + (by * 4 + y) * 8 + bx * 4, dc, 4);
            }
        }
        break;
    case 1:
        for (int y = 0; y < 8; ++y)
            std::memset(pred + y * 8, rec[y * stride - 1], 8);
        break;
    case 2:
        for (int y = 0; y < 8; ++y)
            std::memcpy(pred + y * 8, above, 8);
        break;
    case 3: {
        auto p = [&](int x, int y) { return int(rec[y * stride + x]); };
        int h = 0, v = 0;
        for (int i = 0; i < 4; ++i) {
            h += (i + 1) * (p(4 + i, -1) - p(2 - i, -1));
            v += (i + 1) * (p(-1, 4 + i) - p(-1, 2 - i));
        }
        const int a = 16 * (p(-1, 7) + p(7, -1));
        const int b = (34 * h + 32) >> 6;
        const int c = (34 * v + 32) >> 6;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                pred[y * 8 + x] = clip1((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
        break;
    }
    }
}

} // namespace

FrameCoder::FrameCoder(const EncoderConfig& config)
    : cfg_(config)
{
    cfg_.chromaRdWeight = std::clamp(cfg_.chromaRdWeight, 1, 16);
    cfg_.chromaQpOffset = std::clamp(cfg_.chromaQpOffset, -12, 12);
    mbW_ = (cfg_.width + 15) / 16;
    mbH_ = (cfg_.height + 15) / 16;
    padW_ = mbW_ * 16;
    padH_ = mbH_ * 16;
    extW_ = padW_ + 2 * kMargin;
    extH_ = padH_ + 2 * kMargin;
    srcY_.resize(size_t(padW_) * padH_);
    srcU_.resize(size_t(padW_ / 2) * (padH_ / 2));
    srcV_.resize(srcU_.size());
    recY_.resize(srcY_.size());
    recU_.resize(srcU_.size());
    recV_.resize(srcU_.size());
    nzLuma_.resize(size_t(mbW_ * 4) * (mbH_ * 4));
    modes4x4_.resize(nzLuma_.size());
    mvX_.resize(nzLuma_.size());
    mvY_.resize(nzLuma_.size());
    refIdx_.resize(mvX_.size());
    fullPelCost_.assign(size_t(mbW_) * mbH_, -1.0f);
    if (cfg_.bframes > 0) {
        mvX1_.resize(mvX_.size());
        mvY1_.resize(mvX_.size());
        refIdx1_.resize(mvX_.size());
    }
    sliceTopOfRow_.resize(size_t(mbH_));
    rowKey_.resize(size_t(mbH_));
    cabWave_.resize(mbW_, mbH_);
    cabWrite_.resize(mbW_, mbH_);
    cabacByTop_.resize(size_t(mbH_));
    prevQpNzByTop_.resize(size_t(mbH_));
    qpPredByTop_.resize(size_t(mbH_));
    mbQp_.resize(size_t(mbW_) * mbH_);
    mbQpFinal_.resize(mbQp_.size());
    mbEnergy_.resize(mbQp_.size());
    if (cfg_.mode == IntraMode::Pcm)
        cfg_.deblock = false; // the filter below assumes every macroblock uses the slice QP
    nzChroma_[0].resize(size_t(mbW_ * 2) * (mbH_ * 2));
    nzChroma_[1].resize(nzChroma_[0].size());
}

Encoder::Encoder(const EncoderConfig& config)
    : cfg_(config)
{
    cfg_.chromaQpOffset = std::clamp(cfg_.chromaQpOffset, -12, 12);
    mbW_ = (cfg_.width + 15) / 16;
    mbH_ = (cfg_.height + 15) / 16;
    padW_ = mbW_ * 16;
    padH_ = mbH_ * 16;
    if (cfg_.mode == IntraMode::Pcm)
        cfg_.deblock = false;
    // B pictures need CABAC and P pictures; their run and its reference picture must fit the queue.
    cfg_.bframes = useB() ? std::min(cfg_.bframes, 8) : 0;
    // (and the pictures of the run coded now be settled: the probe's answer, a keyframe behind them)
    if (useB())
        cfg_.lookahead = std::max(cfg_.lookahead, 2 * cfg_.bframes + 4);
    for (auto& c : coders_)
        c = std::make_unique<FrameCoder>(cfg_);
    idrCount_ = cfg_.idrPicIdBase;
    if (vbvOn()) {
        // The first removal waits for initial_cpb_removal_delay (90 kHz) - the fill as the decoder sees it.
        const double init = std::floor(90000.0 * std::clamp(cfg_.vbvInit, 0.05, 1.0) * vbvSize() / vbvRate());
        vbvFill_ = vbvOutFill_ = init * vbvRate() / 90000.0;
        vbvMinFill_ = vbvSize();
    }
}

const std::vector<uint8_t>& Encoder::reconY() const { return (last_ ? last_ : coders_[0].get())->recY_; }
const std::vector<uint8_t>& Encoder::reconU() const { return (last_ ? last_ : coders_[0].get())->recU_; }
const std::vector<uint8_t>& Encoder::reconV() const { return (last_ ? last_ : coders_[0].get())->recV_; }

int levelIdcFor(const EncoderConfig& cfg)
{
    struct Level {
        int idc;
        int64_t maxMbps, maxFs, maxDpbMbs, maxBr; // MaxBR in 1000 (1250 for High) bit/s
    };
    static constexpr Level kLevels[] = {
        {10, 1485, 99, 396, 64},          {11, 3000, 396, 900, 192},        {12, 6000, 396, 2376, 384},
        {13, 11880, 396, 2376, 768},      {20, 11880, 396, 2376, 2000},     {21, 19800, 792, 4752, 4000},
        {22, 20250, 1620, 8100, 4000},    {30, 40500, 1620, 8100, 10000},   {31, 108000, 3600, 18000, 14000},
        {32, 216000, 5120, 20480, 20000}, {40, 245760, 8192, 32768, 20000}, {41, 245760, 8192, 32768, 50000},
        {42, 522240, 8704, 34816, 50000}, {50, 589824, 22080, 110400, 135000}, {51, 983040, 36864, 184320, 240000},
        {52, 2073600, 36864, 184320, 240000}, {60, 4177920, 139264, 696320, 240000}, {61, 8355840, 139264, 696320, 480000},
        {62, 16711680, 139264, 696320, 800000},
    };
    const int64_t mbW = (cfg.width + 15) / 16, mbH = (cfg.height + 15) / 16, fs = mbW * mbH;
    const double fps = cfg.fps > 0 ? cfg.fps : 60.0;
    const bool high = cfg.cabac && cfg.transform8x8 && cfg.mode != IntraMode::Pcm;
    const double brFactor = high ? 1250.0 : 1000.0;
    const bool bPictures = cfg.bframes > 0 && cfg.cabac && cfg.mode != IntraMode::Pcm && cfg.gop > 1;
    const bool pyramid = bPictures && cfg.bPyramid && std::min(cfg.bframes, 8) >= 3 && cfg.refs < 16;
    const int64_t dpb = std::max(bPictures ? 2 : 1, cfg.refs) + (pyramid ? 2 : 0); // max_dec_frame_buffering
    for (const Level& l : kLevels) {
        const int64_t side = int64_t(std::sqrt(double(l.maxFs) * 8.0)); // A.3.1: width and height in macroblocks
        if (double(fs) * fps > double(l.maxMbps) * 1.0001 || fs > l.maxFs || mbW > side || mbH > side)
            continue;
        if (fs * dpb > l.maxDpbMbs)
            continue;
        if (cfg.bitrateKbps > 0 && cfg.bitrateKbps * 1.5 * 1000.0 > double(l.maxBr) * brFactor)
            continue;
        if (cfg.vbvMaxrateKbps > 0 && cfg.vbvBufsizeKbits > 0) {
            // NAL HRD (Table A-1, cpbBrNalFactor of Table A-2): MaxCPB per level, in the order of kLevels.
            static constexpr int64_t kMaxCpb[] = {175,   500,   1000,  2000,   2000,   4000,   4000,   10000,  14000, 20000,
                                                  25000, 62500, 62500, 135000, 240000, 240000, 240000, 480000, 800000};
            const double nal = high ? 1500.0 : 1200.0;
            if (cfg.vbvMaxrateKbps * 1000.0 > double(l.maxBr) * nal || cfg.vbvBufsizeKbits * 1000.0 > double(kMaxCpb[&l - kLevels]) * nal)
                continue;
        }
        return l.idc;
    }
    return 62;
}

std::vector<uint8_t> Encoder::headers() const
{
    std::vector<uint8_t> out;
    {
        BitWriter bw; // seq_parameter_set_rbsp (7.3.2.1.1)
        if (useT8()) {
            bw.u(100, 8); // profile_idc: High
            bw.u(0, 8);
        } else if (useCabac()) {
            bw.u(77, 8);   // profile_idc: Main
            bw.u(0x40, 8); // constraint_set1: decodable by Main decoders
        } else {
            bw.u(66, 8);   // profile_idc: Baseline
            bw.u(0xC0, 8); // constraint_set0 + constraint_set1 (constrained baseline)
        }
        bw.u(uint32_t(levelIdcFor(cfg_)), 8); // level_idc
        bw.ue(0);      // seq_parameter_set_id
        if (useT8()) {
            bw.ue(1);       // chroma_format_idc 4:2:0
            bw.ue(0);       // bit_depth_luma_minus8
            bw.ue(0);       // bit_depth_chroma_minus8
            bw.flag(false); // qpprime_y_zero_transform_bypass_flag
            bw.flag(false); // seq_scaling_matrix_present_flag
        }
        bw.ue(0);      // log2_max_frame_num_minus4
        // B pictures keep the reference pictures on both sides: two frames at least.
        // B pyramid: one more for the reference B picture.
        const uint32_t refFrames = uint32_t(std::max(useB() ? 2 : 1, cfg_.refs) + (usePyramid() ? 1 : 0));
        if (useB()) {
            bw.ue(0); // pic_order_cnt_type: pic_order_cnt_lsb in every slice header
            bw.ue(4); // log2_max_pic_order_cnt_lsb_minus4 (8 bits)
        } else {
            bw.ue(2); // pic_order_cnt_type: output order = decode order
        }
        bw.ue(refFrames); // max_num_ref_frames
        bw.flag(false); // gaps_in_frame_num_value_allowed_flag
        bw.ue(uint32_t(mbW_ - 1));
        bw.ue(uint32_t(mbH_ - 1));
        bw.flag(true); // frame_mbs_only_flag
        bw.flag(true); // direct_8x8_inference_flag
        const bool crop = padW_ != cfg_.width || padH_ != cfg_.height;
        bw.flag(crop);
        if (crop) {
            bw.ue(0);
            bw.ue(uint32_t((padW_ - cfg_.width) / 2));
            bw.ue(0);
            bw.ue(uint32_t((padH_ - cfg_.height) / 2));
        }
        // VUI (E.1.1): timing, so containers and players know the frame rate, and the video signal
        // (range, colours) when the caller knows it.
        bw.flag(true); // vui_parameters_present_flag
        bw.flag(false); // aspect_ratio_info_present_flag
        bw.flag(false); // overscan_info_present_flag
        const bool colours = cfg_.colourPrimaries != 2 || cfg_.transferCharacteristics != 2 || cfg_.matrixCoefficients != 2;
        bw.flag(colours || cfg_.fullRange); // video_signal_type_present_flag
        if (colours || cfg_.fullRange) {
            bw.u(5, 3);               // video_format: unspecified
            bw.flag(cfg_.fullRange);  // video_full_range_flag
            bw.flag(colours);         // colour_description_present_flag
            if (colours) {
                bw.u(uint32_t(cfg_.colourPrimaries & 255), 8);
                bw.u(uint32_t(cfg_.transferCharacteristics & 255), 8);
                bw.u(uint32_t(cfg_.matrixCoefficients & 255), 8);
            }
        }
        bw.flag(false); // chroma_loc_info_present_flag
        bw.flag(true);  // timing_info_present_flag
        // Frame rate = time_scale / (2 * num_units_in_tick); 1001-based rates stay exact.
        const double fps = cfg_.fps > 0 ? cfg_.fps : 60.0;
        const bool ntsc = std::abs(fps * 1.001 - std::round(fps * 1.001)) < 1e-3 && std::abs(fps - std::round(fps)) > 1e-3;
        const uint32_t units = ntsc ? 1001u : 1000u;
        const uint32_t scale = uint32_t(std::lround(fps * units * 2.0));
        bw.u(units, 32); // num_units_in_tick
        bw.u(scale, 32); // time_scale
        bw.flag(true);   // fixed_frame_rate_flag
        bw.flag(vbvOn()); // nal_hrd_parameters_present_flag
        if (vbvOn()) {
            // hrd_parameters (E.1.2): one CPB, VBR, the model's rate and size (vbvRate/vbvSize).
            bw.ue(0);    // cpb_cnt_minus1
            bw.u(0, 4);  // bit_rate_scale: 64 bit/s
            bw.u(0, 4);  // cpb_size_scale: 16 bits
            bw.ue(uint32_t(vbvRate() / 64.0) - 1); // bit_rate_value_minus1
            bw.ue(uint32_t(vbvSize() / 16.0) - 1); // cpb_size_value_minus1
            bw.flag(false); // cbr_flag
            bw.u(23, 5);    // initial_cpb_removal_delay_length_minus1 (24 bits)
            bw.u(23, 5);    // cpb_removal_delay_length_minus1
            bw.u(23, 5);    // dpb_output_delay_length_minus1
            bw.u(0, 5);     // time_offset_length
        }
        bw.flag(false);  // vcl_hrd_parameters_present_flag
        if (vbvOn())
            bw.flag(false); // low_delay_hrd_flag
        bw.flag(false);  // pic_struct_present_flag
        bw.flag(true);   // bitstream_restriction_flag: no reordering, so decoders output at once
        bw.flag(true);   // motion_vectors_over_pic_boundaries_flag
        bw.ue(0);        // max_bytes_per_pic_denom
        bw.ue(0);        // max_bits_per_mb_denom
        bw.ue(16);       // log2_max_mv_length_horizontal
        bw.ue(16);       // log2_max_mv_length_vertical
        bw.ue(usePyramid() ? 2 : useB() ? 1 : 0); // max_num_reorder_frames (B pictures: the reference picture after them
                                                  // comes first; B pyramid: and the reference B picture)
        // max_dec_frame_buffering (at least max_num_ref_frames). B pyramid: one more. The B pictures
        // after the reference B picture are decoded while it still waits for its output, with every
        // frame buffer holding a reference picture; a decoder that stores them first needs a free
        // one (C.4.5.2; NVDEC showed later pictures in their place without it).
        bw.ue(refFrames + (usePyramid() ? 1 : 0));
        bw.rbspTrailing();
        appendNal(out, 3, 7, bw.bytes());
    }
    {
        BitWriter bw; // pic_parameter_set_rbsp (7.3.2.2)
        bw.ue(0);       // pic_parameter_set_id
        bw.ue(0);       // seq_parameter_set_id
        bw.flag(useCabac()); // entropy_coding_mode_flag
        bw.flag(false); // bottom_field_pic_order_in_frame_present_flag
        bw.ue(0);       // num_slice_groups_minus1
        bw.ue(0);       // num_ref_idx_l0_default_active_minus1
        bw.ue(0);       // num_ref_idx_l1_default_active_minus1
        bw.flag(false); // weighted_pred_flag
        bw.u(useB() && cfg_.weightedBipred ? 2 : 0, 2); // weighted_bipred_idc: implicit (by picture distance) or none
        bw.se(0);       // pic_init_qp_minus26
        bw.se(0);       // pic_init_qs_minus26
        bw.se(chromaOffset()); // chroma_qp_index_offset
        bw.flag(true);  // deblocking_filter_control_present_flag
        bw.flag(false); // constrained_intra_pred_flag
        bw.flag(false); // redundant_pic_cnt_present_flag
        if (useT8()) {
            bw.flag(true);  // transform_8x8_mode_flag
            bw.flag(false); // pic_scaling_matrix_present_flag
            bw.se(chromaOffset()); // second_chroma_qp_index_offset
        }
        bw.rbspTrailing();
        appendNal(out, 3, 8, bw.bytes());
    }
    if (useWeightedPred()) {
        // A second picture parameter set, the same but with weighted_pred_flag: P pictures across
        // a fade refer to it and carry a pred_weight_table; all others stay as they were.
        BitWriter bw;
        bw.ue(1);       // pic_parameter_set_id
        bw.ue(0);       // seq_parameter_set_id
        bw.flag(useCabac()); // entropy_coding_mode_flag
        bw.flag(false); // bottom_field_pic_order_in_frame_present_flag
        bw.ue(0);       // num_slice_groups_minus1
        bw.ue(0);       // num_ref_idx_l0_default_active_minus1
        bw.ue(0);       // num_ref_idx_l1_default_active_minus1
        bw.flag(true);  // weighted_pred_flag
        bw.u(useB() && cfg_.weightedBipred ? 2 : 0, 2); // weighted_bipred_idc
        bw.se(0);       // pic_init_qp_minus26
        bw.se(0);       // pic_init_qs_minus26
        bw.se(chromaOffset()); // chroma_qp_index_offset
        bw.flag(true);  // deblocking_filter_control_present_flag
        bw.flag(false); // constrained_intra_pred_flag
        bw.flag(false); // redundant_pic_cnt_present_flag
        if (useT8()) {
            bw.flag(true);  // transform_8x8_mode_flag
            bw.flag(false); // pic_scaling_matrix_present_flag
            bw.se(chromaOffset()); // second_chroma_qp_index_offset
        }
        bw.rbspTrailing();
        appendNal(out, 3, 8, bw.bytes());
    }
    return out;
}

void FrameCoder::loadSource(const Picture& pic)
{
    // Copy with edge replication into the macroblock-aligned buffers.
    auto copyPlane = [](const uint8_t* src, int srcStride, int w, int h, std::vector<uint8_t>& dst, int dw, int dh) {
        for (int y = 0; y < dh; ++y) {
            const uint8_t* row = src + size_t(std::min(y, h - 1)) * srcStride;
            uint8_t* out = dst.data() + size_t(y) * dw;
            std::memcpy(out, row, size_t(w));
            std::memset(out + w, row[w - 1], size_t(dw - w));
        }
    };
    copyPlane(pic.y, pic.strideY, cfg_.width, cfg_.height, srcY_, padW_, padH_);
    copyPlane(pic.u, pic.strideC, cfg_.width / 2, cfg_.height / 2, srcU_, padW_ / 2, padH_ / 2);
    copyPlane(pic.v, pic.strideC, cfg_.width / 2, cfg_.height / 2, srcV_, padW_ / 2, padH_ / 2);
}

// B probe (see BProbe): B pictures are placed where runs of two need at least this much fewer bits
// (H264_BPROBE_GAIN; measured: slow zoom -0.3 .. -0.5, game, camera and noise -0.05 .. +0.6).
// H264_BPROBE=0: no probe, runs of B pictures everywhere (for measurements).
static const bool kBProbe = !std::getenv("H264_BPROBE") || std::atoi(std::getenv("H264_BPROBE")) != 0;
// Where B pictures pay is decided from the lookahead's costs (BPlanner) since 15.11.2026;
// H264_BPLAN=0: by the probe's small encodes as before (the fallback, and for comparisons).
static const bool kBPlan = !std::getenv("H264_BPLAN") || std::atoi(std::getenv("H264_BPLAN")) != 0;
static const bool kBGate = kBPlan || kBProbe; // false: runs of B pictures everywhere
// The probe's first question: runs of this many B pictures against none (3 = with the pyramid).
static const int kBProbeShort = std::getenv("H264_BPROBE_SHORT") ? std::atoi(std::getenv("H264_BPROBE_SHORT")) : 3;
// A new measurement before the interval is over when the pictures' activity (lookahead cost with
// motion against intra) is this factor away from that of the last measurement's pictures, for two
// pictures in a row and not sooner than the gap: a game clip goes from standing to running and back
// within a second. 0 = only by the interval.
static const double kBProbeChange = std::getenv("H264_BPROBE_CHANGE") ? std::atof(std::getenv("H264_BPROBE_CHANGE")) : 1.35;
static const int kBProbeGap = std::getenv("H264_BPROBE_GAP") ? std::atoi(std::getenv("H264_BPROBE_GAP")) : 30;
// Nothing to measure (next to nothing is coded: a still picture, a desktop, the player standing):
// runs of the short length (1, default) or no B pictures (0, as before 15.11.2026).
// Adaptive placement in one pass: the first pictures after the clip's first one are P pictures. The rate
// control's model starts from a probe that misses by up to 2x and settles on the first P pictures;
// with B pictures from the start the plain P pictures of the rest of the clip keep that seed for as
// long as the B pictures last (measured on a game clip: QP 34 instead of 37.5 for 70 pictures, then
// 39 for the rest). 0 = none. Not where the probe found B pictures a clear gain (below kBWarmGain)
// or chose long runs: such material keeps its B pictures throughout, and twelve P pictures cost it
// a lot (slow zoom, 300 pictures at 2 Mbit/s: 0.34 dB).
static const int kBWarmup = std::getenv("H264_BWARMUP") ? std::atoi(std::getenv("H264_BWARMUP")) : 12;
// With a first pass (two-pass plan) the same count: tried without them on 16.11.2026 - detailed camera
// footage and calm game scenes gain a little (crowd_run, 150 pictures at 10 Mbit/s: +0.04 dB PSNR-Y,
// +0.002 SSIM-Y, +0.2 VMAF; a calm game scene +0.05 dB at 1.5 Mbit/s), but game action loses much more
// (CS2, 600 pictures at 3 Mbit/s: -0.27 dB, VMAF -2.2 at 1.4 % more bits). H264_BWARMUP2P sets the
// count of the two-pass plan alone (measuring).
static const int kBWarmupPlanned = std::getenv("H264_BWARMUP2P") ? std::atoi(std::getenv("H264_BWARMUP2P")) : kBWarmup;
static const double kBWarmGain = std::getenv("H264_BWARMGAIN") ? std::atof(std::getenv("H264_BWARMGAIN")) : -0.2;
static const int kBRefQpEnv = std::getenv("H264_BREFQP") ? std::atoi(std::getenv("H264_BREFQP")) : -1;
static const int kPyramidMaxRun = std::getenv("H264_PYRAMID_MAXRUN") ? std::atoi(std::getenv("H264_PYRAMID_MAXRUN")) : 4;
static const double kBProbeFlat = std::getenv("H264_BPROBE_FLAT") ? std::atof(std::getenv("H264_BPROBE_FLAT")) : 0.75;
static const bool kBProbeOneWay = std::getenv("H264_BPROBE_ONEWAY") && std::atoi(std::getenv("H264_BPROBE_ONEWAY")) != 0;
static const bool kBProbeLongClear = !std::getenv("H264_BPROBE_LONGCLEAR") || std::atoi(std::getenv("H264_BPROBE_LONGCLEAR")) != 0;
static const bool kBProbeStill = !std::getenv("H264_BPROBE_STILL") || std::atoi(std::getenv("H264_BPROBE_STILL")) != 0;
static const int kBProbeInterval = std::getenv("H264_BPROBE_INTERVAL") ? std::atoi(std::getenv("H264_BPROBE_INTERVAL")) : BProbe::kInterval;
// B pictures from a gain of 4.5 % on (15 % until 15.11.2026: the B pictures did not pay on moving
// material then). Measured gains (probe with B QP +2): slow zoom -0.3 .. -0.6, moving phases of a
// calm game scene -0.09 .. -0.3, a pan -0.055 .. -0.25, camera footage -0.05 .. -0.06, action
// -0.04 .. +0.5. The pan with the crosshair trail (-0.055) and the calmest start of an action clip
// (-0.037) lie close together: the threshold sits between them.
static const double kBProbeGain = std::getenv("H264_BPROBE_GAIN") ? std::atof(std::getenv("H264_BPROBE_GAIN")) : -0.045;

namespace {

LookaheadFrame copyPicture(const Picture& pic, int w, int h, bool forceKeyframe)
{
    LookaheadFrame f;
    f.forceKeyframe = forceKeyframe;
    f.y.resize(size_t(w) * h);
    f.u.resize(size_t(w / 2) * (h / 2));
    f.v.resize(f.u.size());
    for (int y = 0; y < h; ++y)
        std::memcpy(f.y.data() + size_t(y) * w, pic.y + size_t(y) * pic.strideY, size_t(w));
    for (int y = 0; y < h / 2; ++y) {
        std::memcpy(f.u.data() + size_t(y) * (w / 2), pic.u + size_t(y) * pic.strideC, size_t(w / 2));
        std::memcpy(f.v.data() + size_t(y) * (w / 2), pic.v + size_t(y) * pic.strideC, size_t(w / 2));
    }
    return f;
}

Picture pictureOf(const LookaheadFrame& f, int w)
{
    Picture p;
    p.y = f.y.data();
    p.u = f.u.data();
    p.v = f.v.data();
    p.strideY = w;
    p.strideC = w / 2;
    return p;
}

} // namespace

namespace {

// Class of a cost in FirstPassStats' histograms: quarter octaves from 8 on, in whole numbers (the
// same class on every platform).
int costHistBin(int32_t c)
{
    if (c < 8)
        return 0;
    const int oct = std::bit_width(uint32_t(c)) - 1; // 2^oct <= c
    const int64_t m = (int64_t(c) << 10) >> oct;     // c / 2^oct in 1/1024: 1024 .. 2047
    const int quarter = m >= 1722 ? 3 : m >= 1448 ? 2 : m >= 1218 ? 1 : 0;
    return std::min(FirstPassStats::kHistBins - 1, (oct - 3) * 4 + quarter);
}

void addCostHist(FirstPassStats& st, const LookaheadFrame& f)
{
    const size_t base = st.histCount.size();
    st.histCount.resize(base + 2 * FirstPassStats::kHistBins, 0);
    st.histSum.resize(base + 2 * FirstPassStats::kHistBins, 0.0f);
    std::array<int64_t, 2 * FirstPassStats::kHistBins> sum{};
    for (size_t m = 0; m < f.intraCost.size(); ++m) {
        const int32_t costs[2] = {f.intraCost[m], std::min(f.intraCost[m], f.interCost[m])};
        for (int w = 0; w < 2; ++w) {
            const size_t b = size_t(w * FirstPassStats::kHistBins + costHistBin(costs[w]));
            ++st.histCount[base + b];
            sum[b] += costs[w];
        }
    }
    for (size_t b = 0; b < sum.size(); ++b)
        st.histSum[base + b] = float(sum[b]);
}

} // namespace

// H264_FPASYNC=0: the first pass analyses each batch on the caller's thread (as before 14.11.).
static const bool gFirstPassAsync = !std::getenv("H264_FPASYNC") || std::atoi(std::getenv("H264_FPASYNC")) != 0;

// A new measurement every kInterval pictures and from each scene change or keyframe the caller asks
// for on (also one inside the pictures being collected: they start again there).
bool BProbe::startsAt(const LookaheadFrame& f, int index) const
{
    return lastStart_ < 0 || index - lastStart_ >= kBProbeInterval || ((f.sceneChange || f.forceKeyframe) && index > lastStart_);
}

BProbe::~BProbe()
{
    if (firstThread_.joinable())
        firstThread_.join();
}

// The first answer is there (waits for it when it is measured in the background): run() and
// gate() hold from resultStart() on. False when none was under way.
bool BProbe::collectFirst()
{
    if (!firstPending_)
        return false;
    if (firstThread_.joinable())
        firstThread_.join();
    firstPending_ = false;
    if (firstError_)
        std::rethrow_exception(std::exchange(firstError_, nullptr));
    const int result = firstResult_;
    const double gain = firstGain_;
    run_ = result == 0 ? (gain < kBProbeGain ? firstShort_ : 0) : result == 1 && kBProbeStill ? firstShort_ : 0;
    const bool still = result != 0;
    resultStart_ = firstStart_;
    clear_ = result == 0 && gain < kBWarmGain;
    static const bool trace = std::getenv("H264_B_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "bprobe: picture %d gain %+.3f run %d\n", resultStart_, gain, run_);
    // The long run is only asked for where B pictures are a clear gain (a slow zoom: -0.3 ..
    // -0.6): on a marginal gain it rarely wins, and its measurement costs twice the first one.
    if ((run_ == 0 || firstBframes_ <= firstShort_ || still || (kBProbeLongClear && !clear_)) && start_ == firstStart_) {
        start_ = -1;
        pics_.clear();
    }
    return true;
}

bool BProbe::add(const Lookahead& la, const LookaheadFrame& f, int index, int bframes, int threads, bool background)
{
    if (!kBProbe || bframes <= 0)
        return false;
    const double activity = f.intraSum > 0 ? f.interSum / f.intraSum : 0.0;
    // H264_BPROBE_ONEWAY=1 (experiment, off): only with B pictures placed on more activity, without
    // them on less. Saves probes (each costs about four pictures' worth of coding) but misses pans:
    // their activity is high and B pictures pay there all the same (the crosshair trail came back).
    auto far = [&](double a) {
        const bool more = a > sampleActivity_ * kBProbeChange, less = a < sampleActivity_ / kBProbeChange;
        return kBProbeOneWay ? (run_ > 0 ? more : less) : more || less;
    };
    // (While the first answer is still being measured the pictures for the long run are collected
    // on; a new measurement may start all the same - it is at least the gap away.)
    const bool changed = kBProbeChange > 0 && (start_ < 0 || firstPending_) && sampleActivity_ > 0 && index - lastStart_ >= kBProbeGap &&
                         far(activity) && far(lastActivity_);
    lastActivity_ = activity;
    bool answered = false;
    if (startsAt(f, index) || changed) {
        answered = collectFirst(); // the answer before this one, in order
        pics_.clear();
        start_ = lastStart_ = index;
        activitySum_ = 0;
    }
    if (start_ < 0)
        return answered;
    pics_.push_back(la.halfPicture(f));
    if (index > start_)
        activitySum_ += activity;
    static const bool trace = std::getenv("H264_B_TRACE") != nullptr;
    const int shortRun = std::min(bframes, kBProbeShort);
    const int count = int(pics_.size());
    if (count == shortRun + 2) {
        // First answer, from the pictures of one short run: do B pictures pay at all? In the
        // background (first pass) it is measured beside the analysis of the next pictures - the
        // probe was half of that pass's time on game footage - and taken when the next
        // measurement starts, the long run's pictures are complete or the pass ends.
        sampleActivity_ = activitySum_ / double(count - 1);
        firstStart_ = start_;
        firstShort_ = shortRun;
        firstBframes_ = bframes;
        firstPending_ = true;
        const int w = la.halfW(), h = la.halfH();
        bool started = false;
        if (background && !kBProbeOneWay) {
            try {
                firstThread_ = std::thread([this, pics = pics_, w, h, threads, shortRun] {
                    try {
                        firstResult_ = measure(pics, w, h, threads, &shortRun, 1, &firstGain_);
                    } catch (...) {
                        firstError_ = std::current_exception();
                    }
                });
                started = true;
            } catch (const std::system_error&) { // no thread: measured here
            }
        }
        if (!started) {
            firstResult_ = measure(pics_, w, h, threads, &shortRun, 1, &firstGain_);
            return collectFirst() || answered;
        }
        if (bframes <= shortRun) { // nothing more to collect
            start_ = -1;
            pics_.clear();
        }
        return answered;
    }
    if (count < bframes + 2)
        return answered;
    answered = collectFirst() || answered;
    if (start_ < 0)
        return answered; // no long run asked for
    // Second answer, where they do and longer runs are allowed: the long run against the short
    // one over the pictures of one long run.
    const int runs[2] = {shortRun, bframes};
    double gains[2] = {};
    measure(pics_, la.halfW(), la.halfH(), threads, runs, 2, gains);
    run_ = gains[1] < gains[0] ? bframes : shortRun;
    clear_ = clear_ || run_ > kPyramidMaxRun;
    if (trace)
        std::fprintf(stderr, "bprobe: picture %d gain %+.3f with %d, %+.3f with %d: run %d\n", resultStart_, gains[0], shortRun, gains[1],
                     bframes, run_);
    start_ = -1;
    pics_.clear();
    return true;
}

int BProbe::measure(const std::vector<std::vector<uint8_t>>& pics, int w, int h, int threads, const int* runs, int runCount,
                    double* gains)
{
    std::fill(gains, gains + runCount, 0.0);
    const int count = int(pics.size());
    if (count < 3 || w < 16 || h < 16)
        return 2;
    const std::vector<uint8_t> grey(size_t(w / 2) * (h / 2), 128);
    // Bits and luma error of the pictures after the first one.
    auto code = [&](int qp, int bframes, double& bits, double& ssd, int encThreads) {
        EncoderConfig c;
        c.width = w;
        c.height = h;
        c.qp = qp;
        c.gop = 1 << 20;
        c.sceneCut = false;
        c.lookahead = 0;
        c.mbtree = false;
        c.threads = encThreads;
        c.bframes = bframes;
        c.bAdaptive = false;
        c.weightedPred = false;
        c.aqStrength = 1.0; // the probe's threshold was measured with these
        c.psyRd = 0.0;
        c.bQpOffsetShort = 2; // B pictures as cheap as they can be: at the P pictures' QP their gain hardly shows
        c.bSharp = 0.0;
        Encoder e(c);
        bits = ssd = 0;
        auto take = [&](const std::vector<uint8_t>& au) {
            const int i = e.lastDisplayIndex();
            if (au.empty() || i <= 0 || i >= count)
                return;
            bits += double(au.size()) * 8.0;
            const uint8_t* rec = e.reconY().data();
            const int stride = e.paddedWidth();
            uint64_t sum = 0;
            for (int y = 0; y < h; ++y) {
                const uint8_t* a = pics[size_t(i)].data() + size_t(y) * w;
                const uint8_t* b = rec + size_t(y) * stride;
                uint32_t row = 0;
                for (int x = 0; x < w; ++x) {
                    const int d = int(a[x]) - int(b[x]);
                    row += uint32_t(d * d);
                }
                sum += row;
            }
            ssd += double(sum);
        };
        for (const std::vector<uint8_t>& p : pics) {
            Picture pic;
            pic.y = p.data();
            pic.u = pic.v = grey.data();
            pic.strideY = w;
            pic.strideC = w / 2;
            take(e.encode(pic));
        }
        for (;;) {
            const std::vector<uint8_t> au = e.flush();
            if (au.empty())
                break;
            take(au);
        }
    };
    constexpr int kQp = 28, kQpStep = 3;
    // All runs side by side (each with a share of the threads; the streams do not depend on the
    // thread count): the probe sits in the first pass's one analysis thread, and its small encodes
    // one after another were half of that pass's time on game footage. (On a still picture the
    // runs beside the first are wasted; they cost next to nothing there.)
    double r0 = 0, d0 = 0, r1 = 0, d1 = 0;
    double rb[2] = {0, 0}, db[2] = {0, 0};
    {
        const int jobs = 2 + std::min(runCount, 2);
        const int share = std::max(1, (threads + jobs - 1) / jobs);
        std::atomic<int> next{0};
        runOnThreads(std::min(jobs, std::max(1, threads)), [&] {
            for (int j; (j = next.fetch_add(1)) < jobs;) {
                if (j == 0)
                    code(kQp, 0, r0, d0, share);
                else if (j == 1)
                    code(kQp + kQpStep, 0, r1, d1, share);
                else
                    code(kQp, runs[j - 2], rb[j - 2], db[j - 2], share);
            }
        });
    }
    const double floorBits = (0.002 * w * h + 256.0) * (count - 1);
    static const bool trace = std::getenv("H264_B_TRACE") != nullptr;
    if (r0 < floorBits) {
        if (trace)
            std::fprintf(stderr, "bprobe: P runs %.0f bits at QP %d, under the floor of %.0f\n", r0, kQp, floorBits);
        return 1;
    }
    // The two P runs give no slope: no measurement, no B pictures. (Next to nothing coded - a still
    // picture, a desktop: the slice headers alone are some 100 bits - was answered above; with the
    // B pyramid runs of three gain 0 .. 3 % on screen recordings.)
    if (trace)
        std::fprintf(stderr, "bprobe: P runs %.0f bits / ssd %.0f at QP %d, %.0f / %.0f at %d (floor %.0f bits)\n", r0, d0, kQp, r1, d1,
                     kQp + kQpStep, floorBits);
    // Three QP steps coarser and still nearly the bits: there is next to no residual to save, the
    // bits are vectors and headers (a pan over smooth content) and the comparison at equal
    // distortion below has no slope to stand on. Treated like a still picture.
    if (r1 > kBProbeFlat * r0 && r1 < r0)
        return 1;
    if (r1 >= r0 || d1 <= d0 * 1.02 || d0 <= 0)
        return 2;
    const double slope = std::log(r1 / r0) / std::log(d1 / d0); // < 0
    for (int k = 0; k < runCount && k < 2; ++k)
        if (db[k] > 0)
            gains[k] = rb[k] / (r0 * std::exp(slope * std::log(db[k] / d0))) - 1.0; // against the P runs' bits at this error
    return 0;
}

FirstPass::FirstPass(int width, int height, int threads, int bframes, size_t keepAnalysisBytes)
    : width_(width), height_(height), threads_(std::max(1, threads)),
      la_(std::make_unique<Lookahead>(width, height, (width + 15) / 16, (height + 15) / 16)),
      bframes_(std::clamp(bframes, 0, 8)), keepBytes_(keepAnalysisBytes), stats_(std::make_shared<FirstPassStats>())
{
    stats_->bMax = bframes_;
    // H264_KEEPANALYSIS=0: the encoder analyses every picture again (for measurements).
    if (const char* e = std::getenv("H264_KEEPANALYSIS"))
        keepBytes_ = std::atoi(e) ? keepBytes_ : 0;
}

FirstPass::~FirstPass()
{
    if (worker_.joinable())
        worker_.join();
}

void FirstPass::add(const Picture& pic, bool forceKeyframe)
{
    LookaheadFrame f; // the analysis reads only the luma
    f.forceKeyframe = forceKeyframe;
    f.y.resize(size_t(width_) * height_);
    for (int y = 0; y < height_; ++y)
        std::memcpy(f.y.data() + size_t(y) * width_, pic.y + size_t(y) * pic.strideY, size_t(width_));
    batch_.push_back(std::move(f));
    // With threads the batch is analysed on a thread of its own while the caller brings the next
    // pictures (half the batch size, so as many pictures wait as without it).
    if (int(batch_.size()) >= (threads_ > 1 && gFirstPassAsync ? 2 : 4) * threads_)
        startBatch();
}

// Waits for the batch under analysis; what its thread threw comes out here.
void FirstPass::joinWorker() const
{
    if (worker_.joinable())
        worker_.join();
    if (workerError_) {
        const std::exception_ptr e = workerError_;
        workerError_ = nullptr;
        job_.clear();
        std::rethrow_exception(e);
    }
}

// Hands the waiting pictures to the analysis: on a thread (threads > 1) or right here.
void FirstPass::startBatch() const
{
    joinWorker();
    job_ = std::move(batch_);
    batch_.clear();
    if (threads_ > 1 && gFirstPassAsync) {
        try {
            worker_ = std::thread([this] {
                try {
                    analyseBatch();
                } catch (...) {
                    workerError_ = std::current_exception();
                }
            });
            return;
        } catch (const std::system_error&) { // no thread: analysed here
        }
    }
    analyseBatch();
}

// Half pictures, thumbnails and scene changes of the whole batch side by side, then the costs
// in order (each against its predecessor, the last one of the batch before for the first).
void FirstPass::analyseBatch() const
{
    const int n = int(job_.size());
    if (!n)
        return;
    auto parallel = [&](auto&& fn) {
        std::atomic<int> next{0};
        runOnThreads(std::min(threads_, n), [&] {
            for (int i; (i = next.fetch_add(1)) < n;)
                fn(i);
        });
    };
    auto prevOf = [&](int i) { return i > 0 ? &job_[size_t(i) - 1] : havePrev_ ? &prev_ : nullptr; };
    PhaseClock clock;
    parallel([&](int i) {
        failurePoint(FailPoint::FirstPassWorker);
        la_->prepare(job_[size_t(i)]);
    });
    clock.lap(0);
    parallel([&](int i) { la_->sceneCut(job_[size_t(i)], prevOf(i)); });
    clock.lap(1);
    // The motion search starts from the predecessor's vectors: in order.
    if (threads_ > 1 && n > 1 && gFirstPassAsync) {
        // Pictures side by side, one thread each: a row needs the predecessor's vectors of the same
        // row only, so each picture runs one row behind the one before it (the same numbers as
        // one picture after the other).
        std::unique_ptr<std::atomic<int>[]> rows(new std::atomic<int>[size_t(n)]);
        for (int i = 0; i < n; ++i)
            rows[size_t(i)].store(0, std::memory_order_relaxed);
        std::atomic<int> next{0};
        runOnThreads(std::min(threads_, n), [&] {
            for (int i; (i = next.fetch_add(1)) < n;) {
                try {
                    la_->costs(job_[size_t(i)], prevOf(i), 1, nullptr, 1, nullptr, i > 0 ? &rows[size_t(i) - 1] : nullptr, &rows[size_t(i)]);
                } catch (...) { // the picture after this one must not wait for ever
                    rows[size_t(i)].store(INT_MAX, std::memory_order_release);
                    rows[size_t(i)].notify_all();
                    throw;
                }
            }
        });
    } else {
        for (int i = 0; i < n; ++i)
            la_->costs(job_[size_t(i)], prevOf(i), threads_);
    }
    clock.lap(2);
    if (gTiming)
        std::fprintf(stderr, "first pass batch of %d, ms: prepare %.1f scenecut %.1f costs %.1f\n", n, clock.ms[0], clock.ms[1],
                     clock.ms[2]);
    // Kept from the start without gaps (the encoder's analysis of a picture needs its predecessor's).
    for (int i = 0; i < n; ++i) {
        const LookaheadFrame& f = job_[size_t(i)];
        const size_t bytes = f.intraCost.size() * 12;
        if (stats_->analysis.size() != stats_->intraSum.size() + size_t(i) || keptBytes_ + bytes > keepBytes_)
            break;
        auto a = std::make_shared<FirstPassStats::Analysis>();
        a->intraCost = f.intraCost;
        a->interCost = f.interCost;
        a->mvx = f.mvx;
        a->mvy = f.mvy;
        a->intraSum = f.intraSum;
        a->interSum = f.interSum;
        a->sceneChange = f.sceneChange;
        stats_->analysis.push_back(std::move(a));
        keptBytes_ += bytes;
    }
    // B pictures: where they pay (the probe needs the half pictures and the scene changes).
    if (bframes_ > 0 && kBPlan) {
        const int base = int(stats_->intraSum.size());
        for (int i = 0; i < n; ++i) {
            stats_->bGate.push_back(planner_.gate());
            if (planner_.add(*la_, job_[size_t(i)], base + i, bframes_, threads_))
                std::fill(stats_->bGate.begin() + planner_.resultStart(), stats_->bGate.end(), planner_.gate());
        }
    } else if (bframes_ > 0 && kBProbe) {
        const int base = int(stats_->intraSum.size());
        for (int i = 0; i < n; ++i) {
            stats_->bGate.push_back(probe_.gate());
            if (probe_.add(*la_, job_[size_t(i)], base + i, bframes_, threads_, true))
                std::fill(stats_->bGate.begin() + probe_.resultStart(), stats_->bGate.end(), probe_.gate());
        }
    }
    for (const LookaheadFrame& f : job_)
        addCostHist(*stats_, f);
    for (LookaheadFrame& f : job_) {
        stats_->intraSum.push_back(f.intraSum);
        stats_->interSum.push_back(f.interSum);
        stats_->sceneChange.push_back(f.sceneChange ? 1 : 0);
        stats_->forceKeyframe.push_back(f.forceKeyframe ? 1 : 0);
    }
    prev_ = std::move(job_.back());
    prev_.y = {};
    havePrev_ = true;
    job_.clear();
}

std::shared_ptr<const FirstPassStats> FirstPass::stats() const
{
    joinWorker();
    if (!batch_.empty()) {
        job_ = std::move(batch_);
        batch_.clear();
        analyseBatch();
    }
    if (!kBPlan && probe_.collectFirst()) // an answer still being measured
        std::fill(stats_->bGate.begin() + probe_.resultStart(), stats_->bGate.end(), probe_.gate());
    return stats_;
}

namespace {

// Keyframes as the encoder places them from `from` on (schedule, caller, scene cuts), `from` itself
// being one.
std::vector<uint8_t> keyPlan(const FirstPassStats& st, const EncoderConfig& cfg, size_t from, size_t to)
{
    std::vector<uint8_t> key(to - from, 0);
    int since = 0;
    for (size_t i = from; i < to; ++i) {
        const bool k = i == from || since >= cfg.gop || st.forceKeyframe[i] || (cfg.sceneCut && st.sceneChange[i] && since >= 6);
        if (k)
            since = 0;
        ++since;
        key[i - from] = k;
    }
    return key;
}

} // namespace

// Constant quality: log2 of the mean first-pass cost per macroblock (+ 1) over the pictures that do not repeat the
// one before - the same measure crfQp takes around a picture.
// A picture whose lookahead cost per macroblock is below this repeats the picture before (an exact copy costs 2).
static const double gCrfRepeat = std::getenv("H264_CRFREPEAT") ? std::atof(std::getenv("H264_CRFREPEAT")) : 4.0;
double crfClipComplexity(const FirstPassStats& st, const EncoderConfig& cfg)
{
    const double mbs = double((cfg.width + 15) / 16) * ((cfg.height + 15) / 16);
    double sum = 0, all = 0;
    size_t count = 0;
    for (size_t i = 0; i < st.size(); ++i) {
        all += st.interSum[i];
        if (st.interSum[i] >= gCrfRepeat * mbs) {
            sum += st.interSum[i];
            ++count;
        }
    }
    if (st.size() == 0)
        return -1;
    return std::log2((count ? sum / double(count) : all / double(st.size())) / mbs + 1.0);
}

ChunkPlan planChunks(const FirstPassStats& st, const EncoderConfig& cfg, int chunks, int minFrames)
{
    ChunkPlan plan;
    const int n = int(st.size());
    if (minFrames <= 0)
        minFrames = int(std::lround(15.0 * (cfg.fps > 0 ? cfg.fps : 60.0)));
    chunks = std::clamp(chunks, 1, std::max(1, n / std::max(1, minFrames)));
    const std::vector<uint8_t> key = keyPlan(st, cfg, 0, size_t(n));
    plan.start.push_back(0);
    for (int c = 1; c < chunks; ++c) {
        // The keyframe nearest to the even split within a quarter chunk, else the split itself
        // (H264_CHUNK_EVEN: always the split, to measure what a seam costs).
        const int target = int(int64_t(c) * n / chunks);
        const int window = std::getenv("H264_CHUNK_EVEN") ? -1 : n / chunks / 4;
        int best = target;
        for (int d = 0; d <= window; ++d) {
            if (target - d > plan.start.back() + minFrames / 2 && key[size_t(target - d)]) {
                best = target - d;
                break;
            }
            if (target + d < n && key[size_t(target + d)]) {
                best = target + d;
                break;
            }
        }
        if (best - plan.start.back() >= minFrames / 2 && n - best >= minFrames / 2)
            plan.start.push_back(best);
    }
    if (cfg.bitrateKbps <= 0) {
        if (cfg.crf >= 0) // constant quality: the clip's mean complexity, as each chunk's reference (crfQp)
            plan.crfComplexity = crfClipComplexity(st, cfg);
        return plan;
    }
    // Bits each chunk needs at one QP: I pictures cost about 2.5x their lookahead cost compared
    // with P pictures (measured k ratio) and sit 3 QP lower (x sqrt 2).
    constexpr double kIntraWeight = 2.5 * 1.41421356;
    const size_t chunkCount = plan.start.size();
    std::vector<double> need(chunkCount, 0.0);
    double total = 0;
    for (size_t c = 0; c < chunkCount; ++c) {
        const size_t a = size_t(plan.start[c]), b = c + 1 < chunkCount ? size_t(plan.start[c + 1]) : size_t(n);
        const std::vector<uint8_t> k = keyPlan(st, cfg, a, b);
        auto part = std::make_shared<FirstPassStats>();
        for (size_t i = a; i < b; ++i) {
            need[c] += k[i - a] ? kIntraWeight * st.intraSum[i] : st.interSum[i];
            part->intraSum.push_back(st.intraSum[i]);
            part->interSum.push_back(st.interSum[i]);
            part->sceneChange.push_back(st.sceneChange[i]);
            part->forceKeyframe.push_back(st.forceKeyframe[i]);
            part->bMax = st.bMax;
            if (!st.bGate.empty())
                part->bGate.push_back(st.bGate[i]);
            if (st.histCount.size() == st.size() * 2 * FirstPassStats::kHistBins) {
                const size_t h = i * 2 * FirstPassStats::kHistBins, e = h + 2 * FirstPassStats::kHistBins;
                part->histCount.insert(part->histCount.end(), st.histCount.begin() + ptrdiff_t(h), st.histCount.begin() + ptrdiff_t(e));
                part->histSum.insert(part->histSum.end(), st.histSum.begin() + ptrdiff_t(h), st.histSum.begin() + ptrdiff_t(e));
            }
            // Only the first chunk analyses its pictures like the first pass did (the others
            // start without a predecessor).
            if (c == 0 && i < st.analysis.size())
                part->analysis.push_back(st.analysis[i]);
        }
        total += need[c];
        plan.firstPass.push_back(std::move(part));
    }
    const double budget = double(cfg.bitrateKbps) * n; // kbit * fps
    for (size_t c = 0; c < chunkCount; ++c) {
        const double frames = double(plan.firstPass[c]->size());
        plan.bitrateKbps.push_back(std::max(1, int(std::lround(budget * need[c] / std::max(1.0, total) / frames))));
    }
    return plan;
}

EncoderConfig ChunkPlan::configFor(const EncoderConfig& cfg, int i, int threads) const
{
    EncoderConfig c = cfg;
    c.threads = threads;
    c.idrPicIdBase = (i * 4096) & 0xFFFF; // far from the ids of the chunk before
    if (i > 0)
        c.startBoostQp = 0; // only the clip starts
    if (crfComplexity >= 0)
        c.crfClipComplexity = crfComplexity;
    if (size_t(i) < bitrateKbps.size()) {
        c.bitrateKbps = bitrateKbps[size_t(i)];
        c.firstPass = firstPass[size_t(i)];
    }
    if (c.vbvMaxrateKbps > 0 && c.vbvBufsizeKbits > 0) {
        // VBV across the seams: a chunk after the first assumes kChunkVbvFill at its start (where the buffer usually
        // is: the plan keeps it half full at the end of the lookahead), and every chunk but the last ends with at
        // least kChunkVbvEnd, so the real buffer at the seam is never lower than the next chunk assumes.
        if (i > 0)
            c.vbvInit = std::min(c.vbvInit, kChunkVbvFill);
        if (size_t(i) + 1 < start.size())
            c.vbvEndFill = kChunkVbvEnd;
    }
    return c;
}

std::vector<uint8_t> vbvJoinChunk(const std::vector<uint8_t>& firstAu, int framesSinceBp)
{
    // The access unit starts with our SEI NAL unit (Encoder::vbvSei): buffering period, then picture timing whose
    // first 24 bits are cpb_removal_delay - two ticks per picture since the buffering period before.
    if (firstAu.size() < 6 || firstAu[0] != 0 || firstAu[1] != 0 || firstAu[2] != 0 || firstAu[3] != 1 || (firstAu[4] & 31) != 6)
        return firstAu;
    size_t end = 5;
    for (int zeros = 0; end < firstAu.size(); ++end) { // up to the next start code
        if (zeros >= 2 && firstAu[end] == 1)
            break;
        zeros = firstAu[end] == 0 ? zeros + 1 : 0;
    }
    if (end >= firstAu.size())
        return firstAu;
    size_t naluEnd = end - 2; // the start code's zeros
    while (naluEnd > 5 && firstAu[naluEnd - 1] == 0)
        --naluEnd;
    std::vector<uint8_t> rbsp;
    for (size_t i = 5, zeros = 0; i < naluEnd; ++i) {
        if (zeros >= 2 && firstAu[i] == 3) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(firstAu[i]);
        zeros = firstAu[i] == 0 ? zeros + 1 : 0;
    }
    for (size_t k = 0; k + 1 < rbsp.size() && rbsp[k] != 0x80;) {
        const int type = rbsp[k], size = rbsp[k + 1]; // (ours are short: one byte each)
        if (type == 1 && size >= 3 && k + 2 + 3 <= rbsp.size()) {
            const uint32_t d = uint32_t(2 * std::max(0, framesSinceBp)) & 0xFFFFFF;
            rbsp[k + 2] = uint8_t(d >> 16);
            rbsp[k + 3] = uint8_t(d >> 8);
            rbsp[k + 4] = uint8_t(d);
            std::vector<uint8_t> out;
            appendNal(out, 0, 6, rbsp);
            out.insert(out.end(), firstAu.begin() + ptrdiff_t(naluEnd), firstAu.end());
            return out;
        }
        k += 2 + size_t(size);
    }
    return firstAu;
}

// Classes of P pictures for the first-pass rate control (H264_TPBIN=0: one k for all).
static const bool gTpBin = !std::getenv("H264_TPBIN") || std::atoi(std::getenv("H264_TPBIN")) != 0;
// Two-pass: P pictures after a run of B pictures are their own kind in the bit model; until one is
// coded their k is this factor times the other P pictures'. 0 = one kind (as before 15.11.2026).
static const double gTpAfterB = std::getenv("H264_TPAFTERB") ? std::atof(std::getenv("H264_TPAFTERB")) : 1.6;
// Two-pass: B pictures in cost classes like the P pictures (H264_TPBINB=0: one k for all B pictures, as before
// 16.11.2026). The B pictures of a moving stretch cost several times the bits per cost of those between
// standing pictures; with one k the plan misjudged a clip that has both.
static const bool gTpBinB = !std::getenv("H264_TPBINB") || std::atoi(std::getenv("H264_TPBINB")) != 0;
// How far (in QP) the two-pass QP may leave the QP that fits the whole clip's budget with the model
// as it stands; past that the clip ends over or under its size instead of starving (or flooding)
// its last seconds (H264_TPDEV, <= 0 = no limit).
static const double gTpDev = std::getenv("H264_TPDEV") ? std::atof(std::getenv("H264_TPDEV")) : 2.0;
// How much of the way from that plan QP to the QP that spends exactly what is left the picture's QP
// goes (H264_TPFOLLOW; 1 = all the way, as before 15.11.2026; default a quarter). The plan QP moves only as the model
// learns; the other also swings with every stretch the model misjudged, and near the clip's end with
// little left to spread an error over.
static const bool gTpSeedTree = !std::getenv("H264_TPSEEDTREE") || std::atoi(std::getenv("H264_TPSEEDTREE")) != 0;
static const double gTpFollow = std::getenv("H264_TPFOLLOW") ? std::atof(std::getenv("H264_TPFOLLOW")) : 0.25;
// The rest of the way is gone slowly: the gap between the two QPs, averaged over about this many
// seconds of pictures, is added in full less the quarter above (H264_TPLAG, default 2; 0 = not at
// all: a model that stays wrong in one direction then misses the clip's size by what it is wrong,
// measured up to 7 %). A stretch the model misjudges for a moment moves the QP little, a lasting
// debt is paid. Measured on seven clips of 4 to 30 s: every target within 2 % at 2 s (within 2.5 %
// at 4 s), for 0.2 to 0.5 % of BD-rate; H264_TPFOLLOW=0 with it is 0.5 to 1 % better and misses by up to 3.3 %.
static const double gTpLag = std::getenv("H264_TPLAG") ? std::atof(std::getenv("H264_TPLAG")) : 2.0;
// The least budget the pictures left are planned with: this share of what they cost at the plan
// QP (H264_TPRESTFLOOR; 0 = half the clip's mean rate per picture, as before - on a clip whose
// I pictures take a third of the bits that is more than its other pictures ever need, so an
// overdrawn clip coded its last pictures finer than planned).
static const double gTpRestFloor = std::getenv("H264_TPRESTFLOOR") ? std::atof(std::getenv("H264_TPRESTFLOOR")) : 0.5;

// Two-pass complexity compression (EncoderConfig::qcomp): smoothing window in seconds and the
// largest QP offset (H264_QCBLUR, H264_QCMAX: experiment knobs).
static const double gQcBlur = std::getenv("H264_QCBLUR") ? std::atof(std::getenv("H264_QCBLUR")) : 1.5;
static const double gQcMax = std::getenv("H264_QCMAX") ? std::atof(std::getenv("H264_QCMAX")) : 4.0;

// The bit model's dead zone: a block costs bits in proportion to its lookahead cost over the
// quantiser step (cost * 2^(-qp/6)) above gTpDead; below it nothing (H264_TPDEAD; 0 = the model
// before 15.11.2026, bits = k * cost * 2^(-qp/6)). Measured on a game clip with standing and moving
// phases, fixed QP 30 and 36: without it the same pictures' bits per cost differ by 2.6x between
// the two QPs and by 3-4x between standing and moving pictures, with a dead zone of 2 by 1.2x and
// 1.5-2x. gTpFloor: what a picture costs per macroblock whatever its content (headers, skips).
static const double gTpDead = std::getenv("H264_TPDEAD") ? std::atof(std::getenv("H264_TPDEAD")) : 2.0;
static const double gTpFloor = std::getenv("H264_TPFLOOR") ? std::atof(std::getenv("H264_TPFLOOR")) : 0.03;

// Two-pass: the plan's costs follow the pictures' real references (H264_TPRUN=0: every picture against the
// one before, as the first pass measured it and as before 16.11.2026). A recording with duplicated pictures (a
// game at 12 fps captured at 60) has its changes on single pictures and nothing between: a P picture after a
// run of B pictures that happens to be a duplicate of the picture before looked free and cost fourteen times
// the plan, the B picture on the change looked expensive and was a copy of the P picture after it.
static const bool gTpRun = !std::getenv("H264_TPRUN") || std::atoi(std::getenv("H264_TPRUN")) != 0;

// What picture j needs at a quantiser scale against the picture before it (key: as an I picture), in bits per
// k: its blocks' first-pass costs over the quantiser step, each above the dead zone (from the picture's cost
// histogram; without one, the plain sum).
double Encoder::tpRawNeed(size_t j, bool key, double scale) const
{
    const FirstPassStats& st = *cfg_.firstPass;
    if (st.histCount.size() != st.size() * 2 * FirstPassStats::kHistBins)
        return (key ? st.intraSum[j] : st.interSum[j]) * scale;
    const size_t base = (j * 2 + (key ? 0 : 1)) * FirstPassStats::kHistBins;
    double need = 0;
    for (int b = 0; b < FirstPassStats::kHistBins; ++b) {
        const uint32_t count = st.histCount[base + size_t(b)];
        if (!count)
            continue;
        const double over = double(st.histSum[base + size_t(b)]) * scale - gTpDead * count; // the class's mean cost against the dead zone
        if (over > 0)
            need += over;
    }
    return need;
}

// What picture i needs at QP qp, in bits per k. A picture whose reference is not the picture before it
// (tpRef0_/tpRef1_, from the plan's picture types) needs what the largest step on the way from the reference
// needs - a change is paid once, duplicates between are free; a B picture the smaller of its two ways. The
// first pass knows no more than each picture against the one before; steady motion comes out as before (the
// classes' k carry the rest). plain: the picture against the one before, whatever the plan makes of it.
double Encoder::tpNeed(size_t i, double qp, bool plain) const
{
    const double scale = std::exp2(-qp / 6.0);
    const bool key = i < tpClass_.size() && tpClass_[i] == 0;
    const double floorNeed = gTpFloor * double(mbW_) * mbH_;
    if (key || plain || i >= tpRef0_.size() || tpRef0_[i] < 0)
        return tpRawNeed(i, key, scale) + floorNeed;
    auto way = [&](size_t from, size_t to) { // the steps from picture `from` to picture `to` (from < to)
        double most = 0;
        for (size_t j = from + 1; j <= to; ++j)
            most = std::max(most, tpRawNeed(j, false, scale));
        return most;
    };
    double need = way(size_t(tpRef0_[i]), i);
    if (tpRef1_[i] >= 0 && need > 0)
        need = std::min(need, way(i, size_t(tpRef1_[i])));
    return need + floorNeed;
}

// The same for the plain cost sums (a picture's cost class).
double Encoder::tpRunCost(size_t i) const
{
    const FirstPassStats& st = *cfg_.firstPass;
    if (i >= tpRef0_.size() || tpRef0_[i] < 0)
        return st.interSum[i];
    auto way = [&](size_t from, size_t to) {
        double most = 0;
        for (size_t j = from + 1; j <= to; ++j)
            most = std::max(most, st.interSum[j]);
        return most;
    };
    double cost = way(size_t(tpRef0_[i]), i);
    if (tpRef1_[i] >= 0)
        cost = std::min(cost, way(i, size_t(tpRef1_[i])));
    return cost;
}

// Picture i's need at every step of the P QP (the picture's own QP offset applied).
void Encoder::tpRow(size_t i, double* row) const
{
    for (int k = 0; k < kTpQSteps; ++k)
        row[k] = tpNeed(i, kTpQMin + 0.5 * k + tpQOff_[i]);
}

// Bits of the pictures in tab (by class) at P QP qp with the model's k as it stands.
double Encoder::tpBits(const std::vector<double>& tab, double qp, double kI, double kP, double kB) const
{
    const double pos = std::clamp((qp - kTpQMin) * 2.0, 0.0, double(kTpQSteps - 1));
    const int k0 = std::min(int(pos), kTpQSteps - 2);
    const double t = pos - k0;
    auto at = [&](int cls) {
        const double* row = tab.data() + size_t(cls) * kTpQSteps;
        return row[k0] + (row[k0 + 1] - row[k0]) * t;
    };
    double bits = kI * at(0) + kB * at(1);
    for (int kind = 0; kind < 3; ++kind)
        for (int b = 0; b < kRcBins; ++b) {
            const double need = at(2 + kind * kRcBins + b);
            if (need > 0)
                bits += (kind == 2 ? kPOfBin(kind, b, kB) : gTpBin ? kPOfBin(kind, b, kP) : kP) * need;
        }
    return bits;
}

// The P QP at which the pictures in tab cost budget bits (bits fall with the QP: bisection).
double Encoder::tpSolve(const std::vector<double>& tab, double budget, double kI, double kP, double kB) const
{
    double lo = 0.0, hi = 60.0;
    for (int it = 0; it < 24; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (tpBits(tab, mid, kI, kP, kB) > budget)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

int Encoder::rcBin(double cost) const
{
    const double perMb = cost / std::max(1, mbW_ * mbH_);
    return std::clamp(int(2.0 * std::log2(perMb + 1.0)), 0, kRcBins - 1);
}

// k of a class of P pictures: its own and its neighbours' pictures (weight halving per half
// octave), drawn towards the k of all P pictures while a class has seen little.
double Encoder::kPOfBin(int kind, int bin, double kP) const
{
    // P pictures after B runs: their own k once some are coded, a guess from the others before.
    if (kind == 1)
        kP = rcCostSumPb_ > 0 ? rcKSumPb_ / rcCostSumPb_ : kP * gTpAfterB;
    if (kind == 2 ? !gTpBinB : !gTpBin)
        return kP;
    const double prior = 4.0 * (mbW_ * mbH_) * std::exp2(bin * 0.5); // about four pictures of this class
    double num = kP * prior, den = prior;
    for (int j = 0; j < kRcBins; ++j) {
        if (rcCostBin_[kind][j] <= 0)
            continue;
        const double w = std::exp2(-std::abs(j - bin));
        num += w * rcKBin_[kind][j];
        den += w * rcCostBin_[kind][j];
    }
    return num / den;
}

// The need tables (all pictures of the plan, by class) from the pictures' QP offsets as they stand.
void Encoder::tpBuildTables()
{
    const FirstPassStats& st = *cfg_.firstPass;
    tpRemTab_.assign(size_t(kTpClasses) * kTpQSteps, 0.0);
    std::vector<double> row(kTpQSteps);
    for (size_t i = 0; i < st.size(); ++i) {
        tpRow(i, row.data());
        double* into = tpRemTab_.data() + size_t(tpClass_[i]) * kTpQSteps;
        for (int k = 0; k < kTpQSteps; ++k)
            into[k] += row[size_t(k)];
    }
    tpAllTab_ = tpRemTab_;
}

// The most the clip's start may take from the rest of the clip, as a share of the clip's bits (H264_STARTSHARE).
static const double gStartShare = std::getenv("H264_STARTSHARE") ? std::atof(std::getenv("H264_STARTSHARE")) : 0.025;

// The clip's start (EncoderConfig::startBoostQp): the first startBoostSeconds get a finer QP, fading out over as
// long again, as far as the bits for it stay within gStartShare of the clip's (the model as the probe left it:
// kI, kP; B pictures guessed as elsewhere). The rest of the clip pays through the plan.
void Encoder::tpStartBoost(double kI, double kP)
{
    const FirstPassStats& st = *cfg_.firstPass;
    const size_t n = st.size();
    if (cfg_.startBoostQp <= 0 || cfg_.startBoostSeconds <= 0 || n == 0)
        return;
    const double hold = cfg_.startBoostSeconds * cfg_.fps, fade = std::max(1.0, hold);
    const size_t end = std::min(n, size_t(std::ceil(hold + fade)));
    auto weight = [&](size_t i) { return std::clamp(1.0 - (double(i) - hold) / fade, 0.0, 1.0); }; // 1 while held, 0 after the fade
    auto kOfPicture = [&](size_t i) {
        const int cls = tpClass_[i];
        return cls == 0 ? kI : cls == 1 || cls >= 2 + 2 * kRcBins ? 0.4 * kP : cls >= 2 + kRcBins && gTpAfterB > 0 ? kP * gTpAfterB : kP;
    };
    const double planQp = twoPassPlanQp(kI, kP, 0.4 * kP);
    double total = 0;
    for (size_t i = 0; i < n; ++i)
        total += kOfPicture(i) * tpNeed(i, planQp + tpQOff_[i]);
    auto extra = [&](double boost) {
        double bits = 0;
        for (size_t i = 0; i < end; ++i)
            bits += kOfPicture(i) * (tpNeed(i, planQp + tpQOff_[i] - boost * weight(i)) - tpNeed(i, planQp + tpQOff_[i]));
        return bits;
    };
    double boost = cfg_.startBoostQp;
    if (gStartShare > 0 && extra(boost) > gStartShare * total) {
        double lo = 0;
        for (int it = 0; it < 20; ++it) {
            const double mid = 0.5 * (lo + boost);
            (extra(mid) > gStartShare * total ? boost : lo) = mid;
        }
        boost = lo;
    }
    if (std::getenv("H264_RC_TRACE"))
        std::fprintf(stderr, "rc: start boost %.2f QP (%.1f %% of the clip's bits)\n", boost, 100.0 * extra(boost) / std::max(1.0, total));
    if (boost < 0.05)
        return;
    tpOff_.resize(n, 0.0f);
    for (size_t i = 0; i < end; ++i) {
        const float off = float(boost * weight(i));
        tpOff_[i] -= off;
        tpQOff_[i] -= off;
    }
    tpBuildTables();
}

// QP for the P pictures at which the pictures not coded yet (first-pass costs, k per picture type
// from the pictures coded so far) spend exactly the bits that are left of the clip's budget.
double Encoder::twoPassQp(double kI, double kP, double kB, double planQp) const
{
    const FirstPassStats& st = *cfg_.firstPass;
    const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
    const double left = double(st.size()) - frameIndex_;
    const double least = planQp >= 0 && gTpRestFloor > 0 ? gTpRestFloor * tpBits(tpRemTab_, planQp, kI, kP, kB)
                                                         : 0.5 * left * perFrame;
    const double budget = std::max(double(st.size()) * perFrame - rcBits_, least);
    const double qp = tpSolve(tpRemTab_, budget, kI, kP, kB);
    static const bool trace = std::getenv("H264_RC_TRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "tp: frame %d left %.0f budget %.0f qp %.2f\n", frameIndex_, left, budget, qp);
    return qp;
}

// The one QP at which the whole clip (coded pictures included) meets its budget with the model as
// it stands now.
double Encoder::twoPassPlanQp(double kI, double kP, double kB) const
{
    const FirstPassStats& st = *cfg_.firstPass;
    const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
    return tpSolve(tpAllTab_, double(st.size()) * perFrame, kI, kP, kB);
}

// Constant quality (EncoderConfig::crf): the scale's zero point against the P pictures' QP at 30 pictures per
// second and what a doubling of the frame rate adds. Fitted (16.11.2026, with the macroblock tree's level and the
// AQ reference below) so that a crf value gives about the size and PSNR of x264's --crf with the same value
// (medium preset) on game, camera, animation and screen footage; see the encoder notes of that day.
static const double gCrfBase = std::getenv("H264_CRFBASE") ? std::atof(std::getenv("H264_CRFBASE")) : 5.6;
// QP steps per crf step around 23: our sizes fall faster with the QP than x264's (psy fades out above QP 26);
// with 1, crf 18 came out 15-28 % larger than x264's and crf 33 21-46 % smaller.
static const double gCrfSlope = std::getenv("H264_CRFSLOPE") ? std::atof(std::getenv("H264_CRFSLOPE")) : 0.87;
static const double gCrfFps = std::getenv("H264_CRFFPS") ? std::atof(std::getenv("H264_CRFFPS")) : 2.4;
// H264_CRFREF: a fixed reference complexity instead of the clip's mean (the scale before 16.11.2026 used 7; measuring only).
static const double gCrfRef = std::getenv("H264_CRFREF") ? std::atof(std::getenv("H264_CRFREF")) : -1.0;

// The lookahead cost of each picture as it joins the queue (constant quality: the complexity around a picture).
void Encoder::noteCost(const LookaheadFrame& f)
{
    if (!crfOn())
        return;
    if (crfCost_.size() <= size_t(f.index))
        crfCost_.resize(size_t(f.index) + 1, 0.0f);
    crfCost_[size_t(f.index)] = float(f.interSum);
    // Running sums over the pictures that do not repeat the one before (crfQp: the clip's mean so far).
    if (crfSum_.size() < crfCost_.size() + 1) {
        crfSum_.resize(crfCost_.size() + 1, 0.0);
        crfCount_.resize(crfCost_.size() + 1, 0);
    }
    const double mbs = double(mbW_) * mbH_;
    for (size_t j = size_t(f.index); j < crfCost_.size(); ++j) {
        const bool counts = crfCost_[j] >= gCrfRepeat * mbs;
        crfSum_[j + 1] = crfSum_[j] + (counts ? crfCost_[j] : 0.0);
        crfCount_[j + 1] = crfCount_[j] + (counts ? 1 : 0);
    }
    if (std::getenv("H264_RC_TRACE"))
        std::fprintf(stderr, "crfcost: picture %d cost per macroblock %.2f\n", f.index, f.interSum / (double(mbW_) * mbH_));
}

// Constant quality: the QP of a P picture at this place of the clip (I and B pictures have their offsets from
// it). The crf value moved to the QP scale, the frame rate (a picture shown half as long may be coarser), and
// qcomp times the complexity around the picture against the reference: the mean lookahead cost of the pictures
// up to gQcBlur seconds before and as far after it as the queue reaches.
double Encoder::crfQp(const LookaheadFrame& frame, double* complexity) const
{
    double qp = 23.0 + gCrfSlope * (cfg_.crf - 23.0) + gCrfBase + gCrfFps * std::log2(std::clamp(cfg_.fps, 1.0, 1000.0) / 30.0);
    if (complexity)
        *complexity = 0;
    if (cfg_.qcomp > 0 && !crfCost_.empty()) {
        const size_t w = size_t(std::max(0.0, gQcBlur * cfg_.fps));
        const size_t i = std::min(size_t(std::max(0, frame.index)), crfCost_.size() - 1);
        const size_t a = i > w ? i - w : 0, b = std::min(crfCost_.size(), i + w + 1);
        // Pictures that repeat the one before (a lookahead cost of next to nothing per macroblock) do not
        // count: how long a picture stays is the macroblock tree's business - it gives a picture that is
        // shown five times the finer QP. Counted here as well, a game recorded at a multiple of its frame
        // rate was coded 4 QP finer than the same pictures without the repeats.
        const double mbs = double(mbW_) * mbH_;
        double sum = 0, all = 0;
        size_t count = 0;
        for (size_t j = a; j < b; ++j) {
            all += crfCost_[j];
            if (crfCost_[j] >= gCrfRepeat * mbs) {
                sum += crfCost_[j];
                ++count;
            }
        }
        const double local = std::log2((count ? sum / double(count) : all / double(b - a)) / mbs + 1.0);
        if (complexity)
            *complexity = local;
        // Against the clip's mean (like the two-pass complexity compression): the term moves bits between
        // stretches of the clip and leaves its level to the crf value. Chunks get the whole clip's mean from
        // the first pass (planChunks), so neighbouring chunks agree at their seam; a single encoder takes the
        // mean of the pictures it has seen so far (and those in the queue) - the clip's start counts 0.
        double ref = gCrfRef;
        if (ref < 0)
            ref = cfg_.crfClipComplexity >= 0 ? cfg_.crfClipComplexity
                  : crfCount_[b] > 0           ? std::log2(crfSum_[b] / double(crfCount_[b]) / mbs + 1.0)
                                               : local;
        qp += std::clamp(cfg_.qcomp * 6.0 * (local - ref), -gQcMax, gQcMax);
    }
    return qp;
}

int Encoder::delay() const { return (useLookahead() ? std::max(0, cfg_.lookahead - 1) : 0) + (pipelined() ? kInFlight - 1 : 0); }

// Workers of the frame threading. Rows are not waited in: a worker takes a row that can go on (of
// the oldest picture first, from the top), runs it until it would have to wait (for the row above,
// the entropy writer, the reference picture) and parks it there; whatever lets a parked row go on
// wakes a sleeping worker (WaveSignal). So a worker only sleeps when no row of any picture in
// flight can move.
struct Encoder::Pool {
    WaveSignal sig; // its mutex also guards active and stop
    std::vector<std::thread> threads;
    std::vector<FrameCoder*> active; // oldest first
    bool stop = false;
    int spin = 128;

    explicit Pool(int count)
    {
        spin = spinFor(count);
        try {
            for (int t = 0; t < count; ++t)
                threads.emplace_back([this] { run(); });
        } catch (...) { // no thread: stop the ones started (a joinable std::thread must not be destroyed)
            shutdown();
            throw;
        }
    }
    ~Pool() { shutdown(); }
    void shutdown()
    {
        {
            std::lock_guard<std::mutex> l(sig.m);
            stop = true;
        }
        sig.wake(true);
        for (auto& t : threads)
            t.join();
        threads.clear();
    }
    void run()
    {
        for (;;) {
            const uint32_t epoch = sig.epoch.load(std::memory_order_seq_cst);
            FrameCoder* c = nullptr;
            int r = -1;
            {
                std::lock_guard<std::mutex> l(sig.m);
                if (stop)
                    return;
                for (FrameCoder* a : active)
                    if ((r = a->claimRunnableRow()) >= 0) {
                        c = a;
                        c->wave_.running.fetch_add(1, std::memory_order_acq_rel);
                        break;
                    }
            }
            if (c) {
                c->waveRowRun(r); // catches its own failures
                c->wave_.running.fetch_sub(1, std::memory_order_acq_rel);
                continue;
            }
            // Nothing can move: a short spin, then sleep until something changed (the timeout only
            // bounds the damage of a wake-up that should ever be missed).
            for (int i = 0; i < spin && sig.epoch.load(std::memory_order_relaxed) == epoch; ++i) {
                cpuRelax();
            }
            if (sig.epoch.load(std::memory_order_seq_cst) != epoch)
                continue;
            const auto t0 = gTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            {
                std::unique_lock<std::mutex> l(sig.m);
                sig.sleepers.fetch_add(1, std::memory_order_seq_cst);
                if (!stop && sig.epoch.load(std::memory_order_seq_cst) == epoch)
                    sig.cv.wait_for(l, std::chrono::milliseconds(2));
                sig.sleepers.fetch_sub(1, std::memory_order_seq_cst);
            }
            if (gTiming)
                idleUs.fetch_add(long(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()),
                                 std::memory_order_relaxed);
        }
    }
    std::atomic<long> idleUs{0}; // time the workers slept (H264_TIMING)
    void add(FrameCoder* c)
    {
        {
            std::lock_guard<std::mutex> l(sig.m);
            active.push_back(c);
        }
        sig.wake(true);
    }
    void remove(FrameCoder* c)
    {
        {
            std::lock_guard<std::mutex> l(sig.m);
            active.erase(std::remove(active.begin(), active.end(), c), active.end());
        }
        // No worker can claim a row of c any more; the ones still in waveRowRun (their rows are
        // done, they are on their way out) must be gone before the picture's state is reused:
        // one of them read wave_.signal while it was cleared here (a crash seen once in ~300 runs).
        while (c->wave_.running.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();
        c->wave_.signal = nullptr;
    }
};

// Pictures in flight when picture `index` (coding order) starts, itself included: two while the
// rate control settles (it learns from every finished picture, and one more in flight is one more
// picture coded on the first guess: +0.18 % BD-rate on a 10 s clip), then kInFlight. H264_WARM
// overrides the length of that start.
int Encoder::inFlightAt(int index) const
{
    static const int kWarm = std::getenv("H264_WARM") ? std::atoi(std::getenv("H264_WARM")) : 16;
    return index < kWarm ? 2 : kInFlight;
}

Encoder::~Encoder()
{
    if (bSearchThread_.joinable())
        bSearchThread_.join();
    if (pendingThread_.joinable())
        pendingThread_.join();
    for (FrameCoder* fc : inFlight_) {
        fc->waveEnd();
        pool_->remove(fc);
    }
    pool_.reset();
}

// The analysis of the newest picture runs beside the encoding of the oldest; it joins the
// queue before the next picture is handled.
void Encoder::joinPending()
{
    if (pendingThread_.joinable())
        pendingThread_.join();
    if (pendingError_) {
        pendingFrame_.reset();
        std::rethrow_exception(std::exchange(pendingError_, nullptr));
    }
    if (pendingFrame_) {
        queue_.push_back(std::move(*pendingFrame_));
        pendingFrame_.reset();
        noteGate(queue_.back());
        noteCost(queue_.back());
    }
}

void Encoder::noteGate(const LookaheadFrame& f)
{
    if (!bAdapt() || cfg_.firstPass || !kBGate)
        return;
    if (kBPlan) {
        if (planner_.add(*la_, f, f.index, cfg_.bframes, cfg_.threads))
            gate_.emplace_back(planner_.resultStart(), planner_.gate());
        return;
    }
    if (!probe_.add(*la_, f, f.index, cfg_.bframes, cfg_.threads))
        return;
    if (!gate_.empty() && gate_.back().first == probe_.resultStart())
        gate_.back().second = probe_.gate(); // the second answer for the same pictures
    else
        gate_.emplace_back(probe_.resultStart(), probe_.gate());
}

// The lookahead's searches along the planned references of B pictures (macroblock tree, rate
// control): made once per picture, when its run is settled - the last pictures of the queue may
// still change their type with the next one that joins (a keyframe, the probe's answer), unless
// nothing more comes in.
void Encoder::ensureBSearches()
{
    if (!useB() || (!cfg_.mbtree && cfg_.bitrateKbps <= 0 && !gLaStartForce))
        return;
    const int maxB = cfg_.bframes;
    const size_t count = queue_.size();
    const size_t settled = flushing_ ? count : count - std::min(count, size_t(maxB) + 4);
    for (size_t j = 0; j < count; ++j) {
        LookaheadFrame& f = queue_[j];
        if ((j >= settled && !f.decided) || f.keyframe || f.type == 0)
            continue;
        const int a = f.refDist;
        if (a < 1 || a > maxB + 1 || size_t(a) > j)
            continue; // its reference has left the queue: searched while it was there
        const bool haveFwd = a == 1 || (size_t(a) < f.fwd.size() && f.fwd[size_t(a)].valid());
        if (!haveFwd)
            bSearchJobs_.push_back({0, &f, &queue_[j - size_t(a)], nullptr, a, 0});
        if (f.type != 2)
            continue;
        const int c = f.nextDist;
        const size_t s = j + size_t(c);
        if (c < 1 || a + c > maxB + 1 || s >= count)
            continue;
        LookaheadFrame& next = queue_[s];
        if (size_t(c) >= next.back.size() || !next.back[size_t(c)].valid())
            bSearchJobs_.push_back({1, &f, &next, &queue_[j + 1], a, c});
        const size_t combo = size_t(bComboIndex(a, c, maxB));
        if (combo >= next.biSum.size() || next.biSum[combo] < 0)
            bSearchJobs_.push_back({2, &f, &next, &queue_[j - size_t(a)], a, c});
    }
    if (bSearchJobs_.empty())
        return;
    // With frame threading they run beside the pictures in flight (the searches were a third of
    // the time between two pictures on footage with B pictures); the next picture joins them.
    if (pipelined()) {
        try {
            bSearchThread_ = std::thread([this] {
                try {
                    runBSearches(std::max(1, cfg_.threads / 2));
                } catch (...) {
                    bSearchError_ = std::current_exception();
                }
            });
        } catch (const std::system_error&) { // no thread: they run in joinBSearches
        }
    }
}

// The lookahead's vectors of a picture into its planned references (complete here: the searches
// are joined) for the motion search's start points; half-pel of the half picture = full-pel.
void Encoder::takeLookaheadVectors(const LookaheadFrame& frame, FrameCoder& fc, int type) const
{
    if (gLaStart <= 0 || type == 0 || (type == 1 && (gLaStart < 2 || frame.refDist < 2)))
        return;
    auto take = [&](int list, const std::vector<int16_t>& mx, const std::vector<int16_t>& my) {
        if (mx.empty() || mx.size() != my.size())
            return;
        fc.laMv_[list][0].resize(mx.size());
        fc.laMv_[list][1].resize(my.size());
        for (size_t i = 0; i < mx.size(); ++i) {
            fc.laMv_[list][0][i] = int16_t(mx[i] * 4);
            fc.laMv_[list][1][i] = int16_t(my[i] * 4);
        }
    };
    const int a = frame.refDist;
    if (a == 1)
        take(0, frame.mvx, frame.mvy);
    else if (size_t(a) < frame.fwd.size() && frame.fwd[size_t(a)].valid())
        take(0, frame.fwd[size_t(a)].mvx, frame.fwd[size_t(a)].mvy);
    if (type != 2)
        return;
    const int c = frame.nextDist;
    for (const LookaheadFrame& n : queue_)
        if (n.index == frame.index + c) {
            if (size_t(c) < n.back.size() && n.back[size_t(c)].valid())
                take(1, n.back[size_t(c)].mvx, n.back[size_t(c)].mvy);
            break;
        }
}

void Encoder::runBSearches(int threads)
{
    const int maxB = cfg_.bframes;
    for (const BSearchJob& j : bSearchJobs_) {
        if (j.kind == 0)
            la_->searchFwd(*j.f, *j.other, j.a, maxB, threads);
        else if (j.kind == 1)
            la_->searchBack(*j.other, *j.f, *j.third, j.c, maxB, threads);
        else
            la_->biCost(*j.other, *j.f, *j.third, j.a, j.c, maxB, cfg_.weightedBipred);
    }
}

// The searches collected by the last ensureBSearches are done when this returns. Every picture's
// tree and rate control see the searches up to the picture before (with any thread count), and
// no search is under way while the queue changes.
void Encoder::joinBSearches()
{
    if (bSearchThread_.joinable()) {
        bSearchThread_.join();
        bSearchJobs_.clear();
        if (bSearchError_)
            std::rethrow_exception(std::exchange(bSearchError_, nullptr));
        return;
    }
    if (!bSearchJobs_.empty()) {
        runBSearches(cfg_.threads);
        bSearchJobs_.clear();
    }
}

// Longest run of B pictures a picture may be part of, by the probe.
int Encoder::gateAt(int index) const
{
    if (!kBGate)
        return cfg_.bframes;
    for (size_t i = gate_.size(); i-- > 0;)
        if (gate_[i].first <= index)
            return index <= kBWarmup && !(gate_[i].second & BProbe::kClear) ? 0 : gate_[i].second & (BProbe::kClear - 1);
    return 0;
}

std::vector<uint8_t> Encoder::encode(const Picture& picture, bool forceKeyframe)
{
    if (broken_)
        std::rethrow_exception(broken_);
    try {
        return encodeImpl(picture, forceKeyframe);
    } catch (...) {
        broken_ = std::current_exception();
        throw;
    }
}

void Encoder::analyseFrame(LookaheadFrame& f, const LookaheadFrame* prev) const
{
    const FirstPassStats* st = cfg_.firstPass.get();
    if (!st || size_t(f.index) >= st->analysis.size() || !st->analysis[size_t(f.index)]) {
        la_->analyse(f, prev);
        return;
    }
    // The same picture with the same predecessor as in the first pass: same numbers. The half
    // picture and thumbnail are still made (a later picture past the kept ones searches in them).
    const FirstPassStats::Analysis& a = *st->analysis[size_t(f.index)];
    la_->prepare(f);
    f.intraCost = a.intraCost;
    f.interCost = a.interCost;
    f.mvx = a.mvx;
    f.mvy = a.mvy;
    f.propagate.assign(a.intraCost.size(), 0.0f);
    f.treeLinks.ready = false;
    f.intraSum = a.intraSum;
    f.interSum = a.interSum;
    f.sceneChange = a.sceneChange;
}

std::vector<uint8_t> Encoder::encodeImpl(const Picture& picture, bool forceKeyframe)
{
    LookaheadFrame f = copyPicture(picture, cfg_.width, cfg_.height, forceKeyframe);
    f.index = inCount_++;
    if (!useLookahead())
        return step(&f);
    if (!la_)
        la_ = std::make_unique<Lookahead>(cfg_.width, cfg_.height, mbW_, mbH_);
    joinPending();
    const bool full = int(queue_.size()) + 1 >= cfg_.lookahead;
    // Once the queue is full the newest picture joins it only at the next call (analysed in the
    // background with several threads), so the encoded picture sees the same queue with any
    // thread count. Its predecessor stays in the queue while a picture is popped.
    if (full && queue_.size() >= 2) {
        pendingFrame_ = std::make_unique<LookaheadFrame>(std::move(f));
        const LookaheadFrame* prev = &queue_.back();
        if (cfg_.threads > 1) {
            pendingThread_ = std::thread([this, prev] {
                try {
                    failurePoint(FailPoint::LookaheadThread);
                    analyseFrame(*pendingFrame_, prev);
                } catch (...) {
                    pendingError_ = std::current_exception();
                }
            });
        } else {
            analyseFrame(*pendingFrame_, prev);
        }
    } else {
        analyseFrame(f, queue_.empty() ? nullptr : &queue_.back());
        queue_.push_back(std::move(f));
        noteGate(queue_.back());
        noteCost(queue_.back());
    }
    if (!full)
        return {};
    if (useB())
        return codeNext();
    std::vector<uint8_t> out = step(&queue_.front());
    queue_.pop_front();
    return out;
}

// Picture types of the whole clip from the first pass: keyframes as the encoder places them,
// between them runs of B pictures where the first pass's probe found them worth it; and each
// picture's lookahead cost as planned.
void Encoder::planFromStats()
{
    const FirstPassStats& st = *cfg_.firstPass;
    const size_t n = st.size();
    const std::vector<uint8_t> key = keyPlan(st, cfg_, 0, n);
    statType_.assign(n, 1);
    statCost_.assign(n, 0);
    const bool adapt = bAdapt();
    for (size_t i = 0; i < n;) {
        if (key[i]) {
            statType_[i] = 0;
            ++i;
            continue;
        }
        size_t e = i;
        while (e < n && !key[e])
            ++e;
        const int len = int(e - i);
        // Runs of B pictures, each ended by a P picture; the last picture before a keyframe is P.
        // Adaptive: only where the probe found them worth it.
        auto gate = [&](size_t k) {
            if (!adapt || !kBGate || k >= st.bGate.size())
                return cfg_.bframes;
            if (int(k) <= kBWarmupPlanned && !(st.bGate[k] & BProbe::kClear))
                return 0;
            return std::min<int>(cfg_.bframes, st.bGate[k] & (BProbe::kClear - 1));
        };
        std::vector<uint8_t> types(size_t(len) + 1, 1);
        for (int t = 1, run = 0; t < len; ++t)
            if (run < gate(i + size_t(t) - 1)) {
                types[size_t(t)] = 2;
                ++run;
            } else {
                run = 0;
            }
        for (int t = 1; t <= len; ++t)
            statType_[i + size_t(t) - 1] = types[size_t(t)];
        i = e;
    }
    // The rate control's costs: the first pass knows each picture against the one before only; the
    // B and the distant P pictures have their own bits per cost there.
    for (size_t i = 0; i < n; ++i)
        statCost_[i] = statType_[i] == 0 ? st.intraSum[i] : st.interSum[i];
}

// Lookahead cost of queued picture j as planned: P from the reference before, B between its two.
double Encoder::queueCost(size_t j) const
{
    const LookaheadFrame& g = queue_[j];
    if (g.keyframe)
        return g.intraSum;
    if (g.type == 2) {
        const size_t s = j + size_t(g.nextDist);
        if (s < queue_.size() && g.refDist + g.nextDist <= cfg_.bframes + 1) {
            const size_t combo = size_t(bComboIndex(g.refDist, g.nextDist, cfg_.bframes));
            if (combo < queue_[s].biSum.size() && queue_[s].biSum[combo] >= 0)
                return queue_[s].biSum[combo];
        }
        return g.interSum;
    }
    if (g.refDist > 1 && size_t(g.refDist) < g.fwd.size() && g.fwd[size_t(g.refDist)].valid())
        return g.fwd[size_t(g.refDist)].sum;
    return g.interSum;
}

// Types of the queued pictures that are not decided yet, in display order: keyframes as the
// schedule, the caller and the scene changes ask for them, then runs of up to `bframes` B pictures
// each followed by a P picture. The picture before a keyframe and the last one are P pictures (a
// B picture needs a later reference of its own GOP). Adaptive: B pictures only where the probe
// found them worth it (the queue's own, or the first pass's plan of the whole clip).
void Encoder::planQueue()
{
    size_t u = 0;
    while (u < queue_.size() && queue_[u].decided)
        ++u;
    auto keyAt = [&](size_t j, int pos) {
        const LookaheadFrame& f = queue_[j];
        return f.index == 0 || pos >= cfg_.gop || f.forceKeyframe || (cfg_.sceneCut && f.sceneChange && pos >= 6);
    };
    const bool adapt = bAdapt();
    if (adapt && cfg_.firstPass && statType_.empty())
        planFromStats();
    int pos = gopPos_, run = 0;
    for (size_t j = u; j < queue_.size(); ++j) {
        LookaheadFrame& f = queue_[j];
        const bool key = keyAt(j, pos);
        f.keyframe = key;
        f.cutKey = cfg_.sceneCut && f.sceneChange && f.index > 0 && pos >= 6;
        if (key) {
            f.type = 0;
            pos = 0;
            run = 0;
        } else {
            const bool last = j + 1 == queue_.size();
            const bool canB = !last && !keyAt(j + 1, pos + 1);
            bool b;
            if (adapt && cfg_.firstPass)
                b = size_t(f.index) < statType_.size() && statType_[size_t(f.index)] == 2;
            else
                b = !adapt || run < gateAt(f.index); // adaptive: runs as long as the probe found worth it
            if (b && canB && run < cfg_.bframes) {
                f.type = 2;
                ++run;
            } else {
                f.type = 1;
                run = 0;
            }
        }
        f.gopPos = pos++;
    }
    if (usePyramid()) {
        // B pyramid: the middle B picture of a run of three or more is a reference for the others.
        // The run may start before the queue (its first pictures and the reference picture before
        // it are coded and gone): the place of that reference picture is known from its index.
        const int n = int(queue_.size());
        const int none = INT32_MIN;
        int last = lastRefIndex_ >= 0 && n > 0 ? lastRefIndex_ - queue_[0].index : none;
        for (int j = 0; j < n;) {
            LookaheadFrame& f = queue_[size_t(j)];
            if (f.type != 2) {
                if (last != none)
                    f.refDist = j - last;
                f.bRef = false;
                last = j++;
                continue;
            }
            int s = j;
            while (s < n && queue_[size_t(s)].type == 2)
                ++s;
            const int first = last == none ? j : last + 1, count = s - first;
            // Runs of up to kPyramidMaxRun only: longer ones are chosen where B pictures cost next to
            // nothing (a slow zoom: mostly skipped), and there a reference B picture with its
            // residual is a loss (two-pass, 2 Mbit/s: -0.14 dB with runs of 8, 5 and 3 alike).
            const int mid = last != none && count >= 3 && count <= kPyramidMaxRun ? first + count / 2 : none;
            for (int k = j; k < s; ++k) {
                LookaheadFrame& b = queue_[size_t(k)];
                b.nearRun = last != none ? count : 99; // its own run's length
                b.bRef = k == mid;
                if (last != none)
                    b.refDist = mid != none && k > mid ? k - mid : k - last;
                b.nextDist = mid != none && k < mid ? mid - k : s - k;
            }
            j = s;
        }
    }
    // Distances to the reference pictures (pictures whose reference left the queue keep theirs).
    int lastRef = -1;
    for (size_t j = 0; !usePyramid() && j < queue_.size(); ++j) {
        LookaheadFrame& f = queue_[j];
        if (f.type == 2) {
            if (lastRef >= 0)
                f.refDist = int(j) - lastRef;
            size_t s = j + 1;
            while (s < queue_.size() && queue_[s].type == 2)
                ++s;
            f.nextDist = int(s - j);
            f.nearRun = f.refDist + f.nextDist - 1; // its own run's length
        } else {
            if (lastRef >= 0)
                f.refDist = int(j) - lastRef;
            lastRef = int(j);
        }
    }
    // Reference pictures with B pictures before or after them (treeStrength). A picture coded
    // ahead of its B pictures keeps what it was coded with.
    for (size_t j = 0; j < queue_.size(); ++j) {
        LookaheadFrame& f = queue_[j];
        if (f.type != 2 && !f.coded) {
            f.nearB = f.refDist > 1 || (j + 1 < queue_.size() && queue_[j + 1].type == 2);
            size_t e = j + 1;
            while (e < queue_.size() && queue_[e].type == 2)
                ++e;
            f.nearRun = std::max(f.refDist - 1, int(e - j) - 1);
        }
    }
}

// The next picture in coding order: the reference picture after a run of B pictures first (it
// stays in the queue, marked as coded), then the B pictures.
LookaheadFrame* Encoder::pickNext()
{
    if (queue_.empty())
        return nullptr;
    if (!queue_.front().decided) {
        planQueue();
        static const bool trace = std::getenv("H264_B_TRACE") != nullptr;
        for (size_t j = 0; j < queue_.size(); ++j) {
            LookaheadFrame& f = queue_[j];
            f.decided = true;
            if (trace)
                std::fprintf(stderr, "btype: picture %d %c %d %d cost %.0f intra %.0f p1 %.0f\n", f.index, "IPB"[f.type], f.refDist,
                             f.type == 2 ? f.nextDist : 0, queueCost(j), f.intraSum, f.interSum);
            if (f.type != 2) {
                gopPos_ = f.gopPos + 1;
                break;
            }
        }
    }
    LookaheadFrame& front = queue_.front();
    if (front.type != 2)
        return &front;
    for (LookaheadFrame& f : queue_)
        if (f.type != 2) {
            if (!f.coded)
                return &f;
            // B pyramid: the reference B picture next, then the others in display order.
            for (LookaheadFrame& b : queue_) {
                if (&b == &f)
                    break;
                if (b.bRef && !b.coded)
                    return &b;
            }
            for (LookaheadFrame& b : queue_)
                if (!b.coded)
                    return &b;
            return &front;
        }
    return &front; // not reached: a run of B pictures always has its reference picture
}

std::vector<uint8_t> Encoder::codeNext()
{
    LookaheadFrame* f = pickNext();
    std::vector<uint8_t> out = step(f);
    joinBSearches(); // they hold pictures of the queue
    if (f) {
        f->coded = true;
        while (!queue_.empty() && queue_.front().coded) {
            if (queue_.front().type != 2)
                lastRefIndex_ = queue_.front().index;
            queue_.pop_front();
        }
    }
    return out;
}

std::vector<uint8_t> Encoder::flush()
{
    if (broken_)
        std::rethrow_exception(broken_);
    try {
        return flushImpl();
    } catch (...) {
        broken_ = std::current_exception();
        throw;
    }
}

std::vector<uint8_t> Encoder::flushImpl()
{
    joinPending();
    flushing_ = true;
    while (!queue_.empty()) {
        std::vector<uint8_t> out;
        if (useB()) {
            out = codeNext();
        } else {
            out = step(&queue_.front());
            queue_.pop_front();
        }
        if (!out.empty())
            return out;
    }
    return step(nullptr);
}

// Starts the next picture (if any) and hands out the one before: with frame threading the
// picture just started runs on while the caller fetches the next one.
std::vector<uint8_t> Encoder::step(LookaheadFrame* next)
{
    FrameCoder* started = next ? startFrame(*next) : nullptr;
    if (!pipelined())
        return started ? finishFrame(*started) : std::vector<uint8_t>{};
    if (started)
        inFlight_.push_back(started);
    // The oldest picture comes out once more are in flight than the next picture may find (or
    // nothing more comes in): the caller prepares the next picture while the others are coded.
    // Where the depth grows (after the warm-up) one call hands out nothing.
    std::vector<uint8_t> out;
    const int keep = started ? inFlightAt(frameIndex_) - 1 : 0;
    if (int(inFlight_.size()) > keep) {
        FrameCoder* oldest = inFlight_.front();
        inFlight_.pop_front();
        out = finishFrame(*oldest); // throws: the destructor ends the others
    }
    return out;
}

void Encoder::applyRc(int upTo)
{
    while (!rcDone_.empty() && rcDone_.front().index <= upTo) {
        const RcRecord r = rcDone_.front();
        rcDone_.pop_front();
        if (r.lookahead) {
            if (r.cost > 0) {
                const bool pb = r.type == 1 && r.afterB && gTpAfterB > 0;
                if (pb) {
                    rcKSumPb_ += r.bits * std::exp2(r.qp / 6.0);
                    rcCostSumPb_ += r.cost;
                } else {
                    rcKSum_[r.type] += r.bits * std::exp2(r.qp / 6.0);
                    rcCostSum_[r.type] += r.cost;
                }
                if (r.type == 1) {
                    const int b = rcBin(r.cost);
                    rcKBin_[pb][b] += r.bits * std::exp2(r.qp / 6.0);
                    rcCostBin_[pb][b] += r.cost;
                } else if (r.type == 2 && r.bin >= 0) {
                    rcKBin_[2][r.bin] += r.bits * std::exp2(r.qp / 6.0);
                    rcCostBin_[2][r.bin] += r.cost;
                }
            }
        } else if (r.type != 0) {
            rcCplxSum_ += r.bits * std::pow(2.0, r.qp / 6.0);
            rcCplxCount_ += 1.0;
            rcQp_ = r.qp;
        }
        rcBits_ += r.bits;
        rcTarget_ += cfg_.bitrateKbps * 1000.0 / cfg_.fps;
    }
}

std::vector<uint8_t> Encoder::finishFrame(FrameCoder& fc)
{
    PhaseClock clock;
    if (pipelined()) {
        fc.waveEnd();
        pool_->remove(&fc);
        fc.rethrowWaveError();
        fc.done_ = fc.assemble(clock);
        fc.outPic_.reset();
    }
    fc.refs_.clear(); // its reference pictures can be reused once nothing else holds them
    fc.refs1_.clear();
    std::vector<uint8_t> out = std::move(fc.done_);
    fc.done_.clear();
    last_ = &fc;
    lastQp_ = fc.outQp_;
    lastWasSceneCut_ = fc.outSceneCut_;
    lastDisplayIndex_ = fc.displayIndex_;
    lastType_ = "IPB"[fc.rcType_];
    if (vbvOn()) {
        // SEI first (they count in the access unit's bits), then the buffer as the decoder has it.
        std::vector<uint8_t> sei = vbvSei(fc, fc.idr_);
        out.insert(out.begin(), sei.begin(), sei.end());
        if (fc.frameNo_ == 0)
            vbvHeaderBytes_ = headers().size();
        const double bits = double(out.size() + (fc.frameNo_ == 0 ? vbvHeaderBytes_ : 0)) * 8.0;
        vbvOutFill_ -= bits;
        if (vbvOutFill_ < 0)
            ++vbvUnderflows_;
        vbvMinFill_ = std::min(vbvMinFill_, vbvOutFill_);
        vbvOutFill_ = std::min(vbvOutFill_ + vbvTick(), vbvSize());
        vbvDone_.push_back({fc.frameNo_, fc.vbvClass_, fc.vbvQp_ + fc.vbvShift_, fc.vbvCost_, bits});
        if (std::getenv("H264_VBV_TRACE"))
            std::fprintf(stderr, "vbv out: frame %d %c qp %.2f bits %.0f predicted %.0f fill %.1f %%\n", fc.frameNo_, "IPB"[fc.rcType_],
                         fc.vbvQp_, bits, fc.vbvPred_, 100.0 * vbvOutFill_ / vbvSize());
    }
    if (cfg_.bitrateKbps > 0)
        rcDone_.push_back({fc.frameNo_, fc.rcType_, fc.rcAfterB_, fc.rcLookahead_, fc.rcCost_, fc.rcBin_, fc.outQp_,
                           double(out.size()) * 8.0});
    if (gTiming) {
        for (int i = 4; i < 8; ++i)
            gPhaseMs[i] += clock.ms[i];
    }
    return out;
}

// Which queued pictures the schedule or the caller makes keyframes (scene cuts are only
// detected when a picture is encoded).
void Encoder::markKeyframes()
{
    int since = framesSinceIdr_;
    bool first = frameIndex_ == 0;
    for (LookaheadFrame& f : queue_) {
        f.keyframe = first || since >= cfg_.gop || f.forceKeyframe;
        first = false;
        if (f.keyframe)
            since = 0;
        ++since;
    }
}

// Lookahead cost of a picture for the bit model, with the macroblock tree's QP offsets folded in
// (a block at offset -6 costs about twice the bits).
// Strength of the macroblock tree for a picture. A reference picture with B pictures before or
// after it collects the costs of the whole run; the full strength then gives it more bits than the
// B pictures get back (measured on a slow zoom: strength 1 -50.5 %, 1.5 -49.5 %, 2 -47.2 %).
double Encoder::treeStrength(const LookaheadFrame& f) const
{
    // Next to short runs (as placed on moving material, with the pyramid) the full strength: there
    // the P pictures carry the B pictures as they carry each other without them (game footage,
    // two-pass, runs of 3: 10 % fewer bits than with the reduced strength).
    static const bool kShortFull = !std::getenv("H264_BTREE_SHORT") || std::atoi(std::getenv("H264_BTREE_SHORT")) != 0;
    return useB() && f.nearB && !(kShortFull && f.nearRun <= kPyramidMaxRun) ? cfg_.mbtreeStrengthB : cfg_.mbtreeStrength;
}

double Encoder::frameCost(const LookaheadFrame& f, bool intra, const std::vector<int32_t>* inter) const
{
    if (!inter)
        inter = &f.interCost;
    const size_t n = size_t(mbW_) * mbH_;
    if (f.intraCost.size() != n)
        return 0;
    double sum = 0;
    for (size_t m = 0; m < n; ++m) {
        const double c = intra ? f.intraCost[m] : std::min(f.intraCost[m], (*inter)[m]);
        if (cfg_.mbtree) {
            const float off = std::max(-8.0f, Lookahead::qpOffset(f, int(m), treeStrength(f)));
            sum += c * std::exp2(-off / 6.0);
        } else {
            sum += c;
        }
    }
    return sum;
}

// Per-picture setup: neighbour state, slices (CABAC: one slice in a wavefront; CAVLC, whose
// decisions write bits directly: one slice per thread).
void FrameCoder::reset(bool idr, int frameNum, int idrCount)
{
    idr_ = idr;
    framesSinceIdr_ = frameNum;
    idrCount_ = idrCount;
    std::fill(nzLuma_.begin(), nzLuma_.end(), 0);
    std::fill(modes4x4_.begin(), modes4x4_.end(), int8_t(-1));
    std::fill(nzChroma_[0].begin(), nzChroma_[0].end(), 0);
    std::fill(nzChroma_[1].begin(), nzChroma_[1].end(), 0);
    std::fill(mvX_.begin(), mvX_.end(), int16_t(0));
    std::fill(mvY_.begin(), mvY_.end(), int16_t(0));
    std::fill(refIdx_.begin(), refIdx_.end(), int8_t(-1));
    if (bSlice_) {
        std::fill(mvX1_.begin(), mvX1_.end(), int16_t(0));
        std::fill(mvY1_.begin(), mvY1_.end(), int16_t(0));
        std::fill(refIdx1_.begin(), refIdx1_.end(), int8_t(-1));
    }
    const bool wave = useCabac();
    const int sliceCount = wave ? 1 : std::clamp(cfg_.threads, 1, mbH_);
    sliceTop_.assign(size_t(sliceCount) + 1, 0);
    for (int s = 0; s <= sliceCount; ++s)
        sliceTop_[size_t(s)] = s * mbH_ / sliceCount;
    for (int s = 0; s < sliceCount; ++s)
        for (int r = sliceTop_[size_t(s)]; r < sliceTop_[size_t(s) + 1]; ++r)
            sliceTopOfRow_[size_t(r)] = sliceTop_[size_t(s)];
    for (int r = 0; r < mbH_; ++r)
        rowKey_[size_t(r)] = wave ? r : sliceTopOfRow_[size_t(r)];
    slices_.assign(size_t(sliceCount), BitWriter());
}

// slice_header (7.3.3)
void FrameCoder::sliceHeader(BitWriter& bw, int s) const
{
    bw.ue(uint32_t(sliceTop_[size_t(s)] * mbW_)); // first_mb_in_slice
    bw.ue(idr_ ? 7 : bSlice_ ? 6 : 5);             // slice_type: I, B or P (all slices)
    bw.ue(wpAny_ ? 1 : 0);                         // pic_parameter_set_id (1: with weighted prediction)
    bw.u(uint32_t(framesSinceIdr_ & 15), 4);       // frame_num (0 for IDR)
    if (idr_)
        bw.ue(uint32_t(idrCount_ & 0xFFFF)); // idr_pic_id (differs between neighbours)
    if (pocLsb_)
        bw.u(uint32_t((2 * gopPos_) & 255), 8); // pic_order_cnt_lsb
    if (bSlice_)
        bw.flag(true); // direct_spatial_mv_pred_flag
    if (!idr_) {
        // B slices use one reference per list, the default of the picture parameter set.
        bw.flag(activeRefs_ > 1); // num_ref_idx_active_override_flag
        if (activeRefs_ > 1)
            bw.ue(uint32_t(activeRefs_ - 1)); // num_ref_idx_l0_active_minus1
        bw.flag(!listMod_.empty()); // ref_pic_list_modification_flag_l0
        for (int v : listMod_) {
            bw.ue(0);           // modification_of_pic_nums_idc: subtract
            bw.ue(uint32_t(v)); // abs_diff_pic_num_minus1
        }
        if (!listMod_.empty())
            bw.ue(3);
        if (bSlice_)
            bw.flag(false); // ref_pic_list_modification_flag_l1
    }
    if (wpAny_) {
        // pred_weight_table (7.3.3.2)
        bw.ue(uint32_t(wpLumaLog_));   // luma_log2_weight_denom
        bw.ue(uint32_t(wpChromaLog_)); // chroma_log2_weight_denom
        for (int i = 0; i < activeRefs_; ++i) {
            const WeightParams& w = wp_[size_t(i)];
            bw.flag(w.luma); // luma_weight_l0_flag
            if (w.luma) {
                bw.se(w.lumaWeight);
                bw.se(w.lumaOffset);
            }
            bw.flag(w.chroma); // chroma_weight_l0_flag
            if (w.chroma)
                for (int c = 0; c < 2; ++c) {
                    bw.se(w.chromaWeight[c]);
                    bw.se(w.chromaOffset[c]);
                }
        }
    }
    if (idr_) {
        bw.flag(false); // no_output_of_prior_pics_flag
        bw.flag(false); // long_term_reference_flag
    } else {
        if (refPic_) {
            bw.flag(mmcoDiff_ >= 0); // adaptive_ref_pic_marking_mode_flag
            if (mmcoDiff_ >= 0) {
                bw.ue(1);                   // memory_management_control_operation: a short-term picture unused
                bw.ue(uint32_t(mmcoDiff_)); // difference_of_pic_nums_minus1
                bw.ue(0);
            }
        }
        if (useCabac())
            bw.ue(0); // cabac_init_idc
    }
    bw.se(cfg_.qp - 26);         // slice_qp_delta
    bw.ue(cfg_.deblock ? 0 : 1); // disable_deblocking_filter_idc (0 filters across slice edges too)
    if (cfg_.deblock) {
        bw.se(gDeblockA); // slice_alpha_c0_offset_div2
        bw.se(gDeblockB); // slice_beta_offset_div2
    }
}

// CAVLC (and I_PCM): the macroblocks write their bits as they are decided.
void FrameCoder::encodeSlice(int s)
{
    BitWriter& bw = slices_[size_t(s)];
    sliceHeader(bw, s);
    int skipRun = 0;
    qpPredByTop_[size_t(sliceTop_[size_t(s)])] = cfg_.qp;
    bool prevQpNonZero = false;
    cabacByTop_[size_t(sliceTop_[size_t(s)])] = nullptr;
    prevQpNzByTop_[size_t(sliceTop_[size_t(s)])] = &prevQpNonZero;
    for (int mby = sliceTop_[size_t(s)]; mby < sliceTop_[size_t(s) + 1]; ++mby) {
        for (int mbx = 0; mbx < mbW_; ++mbx) {
            if (cfg_.mode == IntraMode::Pcm) {
                encodeMbPcm(bw, mbx, mby);
                continue;
            }
            MbOut mb;
            bool coded = true;
            psyBegin(mbx, mby);
            if (pSlice_)
                coded = encodeMbP(mb, mbx, mby);
            else
                commitQp(mbx, mby, encodeMbIntra(mb, mbx, mby));
            if (!coded) {
                ++skipRun;
                continue;
            }
            if (pSlice_) {
                bw.ue(uint32_t(skipRun));
                skipRun = 0;
            }
            bw.append(mb.bw);
        }
    }
    if (skipRun)
        bw.ue(uint32_t(skipRun));
    bw.rbspTrailing();
}

// Wavefront (CABAC): a thread takes the next row and stays two macroblocks behind the row above
// (intra prediction and motion vector prediction read above-right). Bit estimates use a counting
// CABAC engine per row, started from the row above after its second macroblock. With several
// threads the row's thread also deblocks the row above, lagging one macroblock: by then every
// macroblock that predicts from those unfiltered samples is decided, the entropy writer has fixed
// the QPs (skipped macroblocks inherit it) and the row above that is filtered two macroblocks
// further (the raster order of 8.7). With `out`, finished rows go into that reference picture
// band by band (rowsReady), so the next picture can start before this one is done.
void FrameCoder::waveBegin(int threads, RefPic* out, bool writerThread)
{
    WaveState& w = wave_;
    const int rows = mbH_;
    w.rows = rows;
    w.spin = spinFor(threads);
    w.inlineDeblock = cfg_.deblock && threads > 1;
    w.rowEngine.assign(size_t(rows), CabacEncoder());
    w.rowSnap.assign(size_t(rows), CabacEncoder());
    if (w.allocated != rows) {
        w.progress.reset(new std::atomic<int>[size_t(rows)]);
        w.deblocked.reset(new std::atomic<int>[size_t(rows)]);
        w.rowPrevNz.reset(new bool[size_t(rows)]);
        w.rowMb.reset(new std::atomic<int>[size_t(rows)]);
        w.rowDb.reset(new std::atomic<int>[size_t(rows)]);
        w.rowPhase.reset(new std::atomic<uint8_t>[size_t(rows)]);
        w.rowBusy.reset(new std::atomic<uint8_t>[size_t(rows)]);
        w.allocated = rows;
    }
    for (int r = 0; r < rows; ++r) {
        w.progress[size_t(r)].store(0, std::memory_order_relaxed);
        w.deblocked[size_t(r)].store(0, std::memory_order_relaxed);
        w.rowMb[size_t(r)].store(0, std::memory_order_relaxed);
        w.rowDb[size_t(r)].store(0, std::memory_order_relaxed);
        w.rowPhase[size_t(r)].store(kRowNew, std::memory_order_relaxed);
        w.rowBusy[size_t(r)].store(0, std::memory_order_relaxed);
    }
    w.firstOpen.store(0, std::memory_order_relaxed);
    // A parked row goes on once it can do a batch (not one macroblock at a time behind the row
    // above); small enough that every thread still finds a row across the picture's width.
    static const int kBatchEnv = std::getenv("H264_BATCH") ? std::atoi(std::getenv("H264_BATCH")) : -1;
    w.batch = kBatchEnv >= 0 ? kBatchEnv : std::clamp(mbW_ / (2 * std::max(1, threads)), 0, 8);
    w.written.store(0, std::memory_order_relaxed);
    w.rowsDone.store(0, std::memory_order_relaxed);
    waveErrors_.clear();
    w.waitUs.store(0, std::memory_order_relaxed);
    w.out = out;
    w.yCopied = w.cCopied = w.fBuilt = w.hBuilt = 0;
    if (out) {
        const size_t n = size_t(extW_) * extH_;
        out->y.resize(recY_.size());
        out->u.resize(recU_.size());
        out->v.resize(recV_.size());
        out->F.resize(n);
        out->B.resize(n);
        out->V.resize(n);
        out->J.resize(n);
        if (cfg_.bframes > 0) {
            out->mvx.resize(mvX_.size());
            out->mvy.resize(mvX_.size());
            out->ref.resize(mvX_.size());
        }
        out->rowsReady.store(0, std::memory_order_relaxed);
    }
    mbSyn_.resize(size_t(mbW_) * mbH_);
    if (writerThread)
        w.writer = std::thread([this] { waveWriteGuarded(); });
    w.nextRow.store(0, std::memory_order_release); // rows may be taken from here on
}

// A failure (exception) in any row or the writer: it is kept for the caller, and every counter
// anyone may wait on goes to its end, so the remaining rows stop early and the picture ends
// (its content is then garbage; the encoder reports the failure and is not used again).
void FrameCoder::waveFail()
{
    WaveState& w = wave_;
    waveErrors_.capture();
    for (int r = 0; r < w.rows; ++r) {
        raiseTo(w.progress[size_t(r)], mbW_);
        raiseTo(w.deblocked[size_t(r)], mbW_);
    }
    raiseTo(w.written, w.rows);
    if (w.signal)
        w.signal->wake(true); // parked rows are given up by the workers
    // The reference rows are released once no row writes them any more (waveRow, waveEnd).
}

void FrameCoder::rethrowWaveError() { waveErrors_.rethrow(); }

void FrameCoder::waveWriteGuarded()
{
    try {
        failurePoint(FailPoint::Writer);
        waveWrite();
    } catch (...) {
        waveFail();
    }
}

// The failed picture's row: whoever waits for it is released, the row counts as done.
void FrameCoder::abandonRow(int r)
{
    WaveState& w = wave_;
    w.rowPhase[size_t(r)].store(kRowDone, std::memory_order_release);
    raiseTo(w.progress[size_t(r)], mbW_);
    if (r > 0)
        raiseTo(w.deblocked[size_t(r - 1)], mbW_);
    w.rowsDone.fetch_add(1, std::memory_order_acq_rel);
}

void FrameCoder::waveRow(int r)
{
    bool done = false;
    try {
        done = !waveErrors_.failed() && waveRowBody(r, true) == 1;
    } catch (...) {
        waveFail();
    }
    if (!done)
        abandonRow(r); // failure here or elsewhere
    // The last row of a failed picture: nothing writes the reference picture any more, so the
    // next picture's rows (waiting for its rows) may go on.
    if (waveErrors_.failed() && wave_.rowsDone.load(std::memory_order_acquire) >= wave_.rows && wave_.out)
        raiseTo(wave_.out->rowsReady, mbH_);
}

// Reference rows row r can reach: vectors up to 64 pixels (4 rows) plus the 6-tap filter.
bool FrameCoder::refRowsReady(int r) const
{
    if (!pSlice_)
        return true;
    const int need = std::min(mbH_, r + 6);
    for (const auto& ref : refs_)
        if (ref->rowsReady.load(std::memory_order_acquire) < need)
            return false;
    if (bSlice_)
        for (const auto& ref : refs1_)
            if (ref->rowsReady.load(std::memory_order_acquire) < need)
                return false;
    return true;
}

// Whether a parked row can go on by a batch (or, after a failure, must be given up).
bool FrameCoder::rowRunnable(int r) const
{
    const WaveState& w = wave_;
    if (waveErrors_.failed())
        return w.rowPhase[size_t(r)].load(std::memory_order_acquire) != kRowDone;
    const int k = w.batch;
    switch (w.rowPhase[size_t(r)].load(std::memory_order_acquire)) {
    case kRowNew:
        if (r > 0 && w.progress[size_t(r - 1)].load(std::memory_order_acquire) < std::min(2 + k, mbW_))
            return false;
        return refRowsReady(r);
    case kRowMbs:
        return r == 0 || w.progress[size_t(r - 1)].load(std::memory_order_acquire) >=
                             std::min(w.rowMb[size_t(r)].load(std::memory_order_acquire) + 2 + k, mbW_);
    case kRowTail: {
        if (!w.inlineDeblock || r == 0)
            return true;
        const int db = w.rowDb[size_t(r)].load(std::memory_order_acquire);
        if (db >= mbW_)
            return true;
        return w.written.load(std::memory_order_acquire) >= r &&
               (r == 1 || w.deblocked[size_t(r - 2)].load(std::memory_order_acquire) >= std::min(db + 2 + k, mbW_));
    }
    case kRowPublish:
        return !(w.out && w.inlineDeblock && r >= 2) || w.out->rowsReady.load(std::memory_order_acquire) >= r - 2;
    default:
        return false;
    }
}

// After a change that may let row r go on: wakes a sleeping worker if nobody is on that row.
void FrameCoder::wakeRow(int r)
{
    WaveState& w = wave_;
    if (w.signal && r < w.rows && !w.rowBusy[size_t(r)].load(std::memory_order_acquire) && rowRunnable(r))
        w.signal->wake();
}

// Called with the pool's lock held: the first row (from the top) that can go on, marked busy.
int FrameCoder::claimRunnableRow()
{
    WaveState& w = wave_;
    int first = w.firstOpen.load(std::memory_order_relaxed);
    for (int r = first; r < w.rows; ++r) {
        const uint8_t phase = w.rowPhase[size_t(r)].load(std::memory_order_acquire);
        if (phase == kRowDone) {
            if (r == first)
                w.firstOpen.store(++first, std::memory_order_relaxed);
            continue;
        }
        if (!w.rowBusy[size_t(r)].load(std::memory_order_acquire) && rowRunnable(r)) {
            w.rowBusy[size_t(r)].store(1, std::memory_order_release);
            return r;
        }
        if (phase == kRowNew)
            break; // the rows below it cannot start before it
    }
    return -1;
}

void FrameCoder::waveRowRun(int r)
{
    WaveState& w = wave_;
    int res = -1;
    try {
        if (!waveErrors_.failed())
            res = waveRowBody(r, false);
    } catch (...) {
        waveFail();
    }
    if (res < 0 && w.rowPhase[size_t(r)].load(std::memory_order_acquire) != kRowDone)
        abandonRow(r);
    w.rowBusy[size_t(r)].store(0, std::memory_order_release);
    if (res == 0)
        wakeRow(r); // what it waited for may have come between its last look and here
    if (waveErrors_.failed() && w.rowsDone.load(std::memory_order_acquire) >= w.rows && w.out)
        raiseTo(w.out->rowsReady, mbH_);
    if (waveErrors_.failed() && w.signal)
        w.signal->wake(true);
}

// One row of the wavefront, in phases: start (reference rows, the row above two macroblocks ahead),
// the macroblocks, the rest of the deblocking of the row above, the band that became final. With
// block it waits wherever it cannot go on (a thread per row); without, it returns 0 there and
// goes on from the same place when called again (the pool parks the row). -1 when the picture failed.
// Debug trace (H264_MB_TRACE="x0,x1,y0,y1", macroblock columns and rows): one line per macroblock of
// that window with its type, vectors (ref:x,y of the four 8x8 corners), QP and the mean squared
// error of its reconstruction (before deblocking) against the source in Y, U and V; P macroblocks
// that went through the full decision add lambda and the costs of skip, inter and intra.
void FrameCoder::traceMb(int mbx, int mby, const MbSyntax& syn) const
{
    static const std::array<int, 4> win = [] {
        std::array<int, 4> w = {-1, -1, -1, -1};
        if (const char* e = std::getenv("H264_MB_TRACE"))
            std::sscanf(e, "%d,%d,%d,%d", &w[0], &w[1], &w[2], &w[3]);
        return w;
    }();
    if (win[0] < 0 || mbx < win[0] || mbx > win[1] || mby < win[2] || mby > win[3])
        return;
    const size_t lo = size_t(mby * 16) * padW_ + mbx * 16;
    const int cw = padW_ / 2;
    const size_t co = size_t(mby * 8) * cw + mbx * 8;
    int64_t sy = 0, su = 0, sv = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const int d = int(srcY_[lo + size_t(y) * padW_ + x]) - int(recY_[lo + size_t(y) * padW_ + x]);
            sy += d * d;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const size_t o = co + size_t(y) * cw + x;
            const int du = int(srcU_[o]) - int(recU_[o]), dv = int(srcV_[o]) - int(recV_[o]);
            su += du * du;
            sv += dv * dv;
        }
    static const char* const kKinds[] = {"skip", "i16", "i4", "inter"};
    const size_t lw = size_t(mbW_) * 4, b0 = size_t(mby * 4) * lw + mbx * 4;
    std::string mvs;
    for (int k : {0, 2, 8, 10}) {
        const size_t o = b0 + size_t(k >> 2) * lw + (k & 3);
        char buf[48];
        std::snprintf(buf, sizeof buf, " %d:%d,%d", int(refIdx_[o]), int(mvX_[o]), int(mvY_[o]));
        mvs += buf;
    }
    std::fprintf(stderr, "MB d%d %c %d,%d %s t%d b%d qp%d cbp%d/%d mv%s mse %.1f %.1f %.1f%s\n", displayIndex_,
                 bSlice_ ? 'B' : pSlice_ ? 'P' : 'I', mbx, mby, kKinds[int(syn.kind)], syn.interType, int(syn.bType),
                 int(mbQpFinal_[size_t(mby) * mbW_ + mbx]), syn.cbpLuma, syn.cbpChroma, mvs.c_str(), double(sy) / 256.0,
                 double(su) / 64.0, double(sv) / 64.0, pSlice_ && !bSlice_ ? tDecision : "");
}

int FrameCoder::waveRowBody(int r, bool block)
{
    WaveState& w = wave_;
    auto waitRef = [&](const RefPic& ref) {
        const int need = std::min(mbH_, r + 6);
        if (ref.rowsReady.load(std::memory_order_acquire) >= need)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        waitAtLeast(ref.rowsReady, need, w.spin);
        w.refWaitUs.fetch_add(long(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()),
                              std::memory_order_relaxed);
    };
    auto aboveAt = [&](int need) { return r == 0 || w.progress[size_t(r - 1)].load(std::memory_order_acquire) >= need; };
    auto waitAbove = [&](int need) {
        if (aboveAt(need))
            return;
        const auto t0 = gTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        waitAtLeast(w.progress[size_t(r - 1)], need, w.spin);
        if (gTiming)
            w.waitUs.fetch_add(long(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()),
                               std::memory_order_relaxed);
    };
    int dbNext = w.rowDb[size_t(r)].load(std::memory_order_relaxed); // next macroblock of row r - 1 to filter
    auto deblockBehind = [&](int upTo, bool wait) {
        if (!w.inlineDeblock || r == 0)
            return;
        const int before = dbNext;
        while (dbNext < upTo) {
            const bool ready = w.written.load(std::memory_order_acquire) >= r &&
                               (r == 1 || w.deblocked[size_t(r - 2)].load(std::memory_order_acquire) >= std::min(dbNext + 2, mbW_));
            if (!ready) {
                if (!wait)
                    break;
                waitAtLeast(w.written, r, w.spin);
                if (r > 1)
                    waitAtLeast(w.deblocked[size_t(r - 2)], std::min(dbNext + 2, mbW_), w.spin);
                continue;
            }
            if (waveErrors_.failed()) // the counters may have been pushed to their end
                break;
            deblockMb(dbNext, r - 1);
            raiseTo(w.deblocked[size_t(r - 1)], ++dbNext);
        }
        if (dbNext != before) {
            w.rowDb[size_t(r)].store(dbNext, std::memory_order_release);
            wakeRow(r + 1); // its own deblocking follows this one
        }
    };
    CabacEncoder& eng = w.rowEngine[size_t(r)];
    uint8_t phase = w.rowPhase[size_t(r)].load(std::memory_order_relaxed);
    if (phase == kRowNew) {
        failurePoint(FailPoint::WaveRow);
        if (pSlice_ && !refRowsReady(r)) {
            if (!block)
                return 0;
            for (const auto& ref : refs_)
                waitRef(*ref);
            if (bSlice_)
                for (const auto& ref : refs1_)
                    waitRef(*ref);
        }
        if (r == 0) {
            eng.start(nullptr, cfg_.qp, pSlice_ ? 0 : -1);
            eng.startCounting();
        } else {
            if (!aboveAt(std::min(2, mbW_))) {
                if (!block)
                    return 0;
                waitAbove(std::min(2, mbW_));
            }
            if (waveErrors_.failed())
                return -1;
            eng = w.rowSnap[size_t(r - 1)];
        }
        qpPredByTop_[size_t(r)] = cfg_.qp;
        w.rowPrevNz[size_t(r)] = false;
        cabacByTop_[size_t(r)] = &eng;
        prevQpNzByTop_[size_t(r)] = &w.rowPrevNz[size_t(r)];
        w.rowPhase[size_t(r)].store(phase = kRowMbs, std::memory_order_release);
    }
    if (phase == kRowMbs) {
        for (int mbx = w.rowMb[size_t(r)].load(std::memory_order_relaxed); mbx < mbW_; ++mbx) {
            if (!aboveAt(std::min(mbx + 2, mbW_))) {
                if (!block)
                    return 0;
                waitAbove(std::min(mbx + 2, mbW_));
            }
            if (waveErrors_.failed())
                return -1;
            MbOut mb;
            const int vbvAdd = vbvRowCap_ > 0 ? vbvRowAdjust(mbx, r) : 0;
            const double vbvBits0 = vbvRowCap_ > 0 ? eng.countedBits() : 0.0;
            psyBegin(mbx, r);
            if (bSlice_)
                encodeMbB(mb, mbx, r);
            else if (pSlice_)
                encodeMbP(mb, mbx, r);
            else
                commitQp(mbx, r, encodeMbIntra(mb, mbx, r));
            traceMb(mbx, r, mb.syn);
            cabacWriteMb(eng, cabWave_, mbx, r, mb.syn, w.rowPrevNz[size_t(r)]);
            if (vbvRowCap_ > 0) {
                const size_t k = size_t(r) * size_t(mbW_ + 1) + size_t(mbx);
                vbvPrefB_[k + 1] = vbvPrefB_[k] + (eng.countedBits() - vbvBits0);
                vbvPrefE_[k + 1] = vbvPrefE_[k] + vbvW_[size_t(r) * mbW_ + mbx] * std::exp2(-vbvAdd / 6.0);
            }
            mbSyn_[size_t(r) * mbW_ + mbx].assignUsed(mb.syn);
            if (mbx == std::min(1, mbW_ - 1))
                w.rowSnap[size_t(r)] = eng;
            w.rowMb[size_t(r)].store(mbx + 1, std::memory_order_release);
            raiseTo(w.progress[size_t(r)], mbx + 1);
            wakeRow(r + 1);
            deblockBehind(mbx, false);
        }
        w.rowPhase[size_t(r)].store(phase = kRowTail, std::memory_order_release);
    }
    if (phase == kRowTail) {
        deblockBehind(mbW_, block);
        if (waveErrors_.failed())
            return -1;
        if (w.inlineDeblock && r > 0 && dbNext < mbW_)
            return 0; // only without block
        w.rowPhase[size_t(r)].store(phase = kRowPublish, std::memory_order_release);
    }
    // Row r - 1 is filtered now, so row r - 2 is final (filtering row r - 1 changed its last lines).
    if (w.out && w.inlineDeblock && r >= 2) {
        if (!block && w.out->rowsReady.load(std::memory_order_acquire) < r - 2)
            return 0;
        publishBand(r - 2);
        if (waveErrors_.failed())
            return -1;
    }
    w.rowPhase[size_t(r)].store(kRowDone, std::memory_order_release);
    w.rowsDone.fetch_add(1, std::memory_order_acq_rel);
    return 1;
}

// Copies the final rows of band k (macroblock row) into the reference picture and builds its
// planes; bands go out in order (rowsReady), each needs the 3 luma lines below it.
void FrameCoder::publishBand(int k)
{
    WaveState& w = wave_;
    RefPic& R = *w.out;
    // Band k - 1 comes from another row; after a failure it may never come (polling, not
    // waitAtLeast: a failure does not change rowsReady until no row writes the picture any more).
    for (int i = 0; R.rowsReady.load(std::memory_order_acquire) < k; ++i) {
        if (waveErrors_.failed())
            return;
        if (i < 1024)
            spinWait();
        else
            std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    if (waveErrors_.failed())
        return;
    const bool last = k == mbH_ - 1;
    const int M = kMargin;
    const int yLim = last ? padH_ : std::min(padH_, 16 * k + 19);
    std::memcpy(&R.y[size_t(w.yCopied) * padW_], &recY_[size_t(w.yCopied) * padW_], size_t(yLim - w.yCopied) * padW_);
    w.yCopied = yLim;
    const int cw = padW_ / 2, cLim = last ? padH_ / 2 : 8 * k + 8;
    std::memcpy(&R.u[size_t(w.cCopied) * cw], &recU_[size_t(w.cCopied) * cw], size_t(cLim - w.cCopied) * cw);
    std::memcpy(&R.v[size_t(w.cCopied) * cw], &recV_[size_t(w.cCopied) * cw], size_t(cLim - w.cCopied) * cw);
    w.cCopied = cLim;
    const int fLim = last ? extH_ : M + yLim;
    refFullRows(R, w.fBuilt, fLim);
    w.fBuilt = fLim;
    const int hLim = last ? extH_ : M + 16 * k + 16;
    refHalfRows(R, w.hBuilt, hLim);
    w.hBuilt = hLim;
    if (!R.ref.empty()) // the row's motion, for the B pictures that use this picture
        publishMotion(R, k, k + 1);
    raiseTo(R.rowsReady, k + 1);
    if (w.signal)
        w.signal->wake(true); // rows of the next picture, and the row that publishes the next band
}

// The motion of macroblock rows [row0, row1) as the co-located picture of a later B picture shows
// it (8.4.1.2.1): list 0 where a block uses it, else list 1 (a reference B picture).
void FrameCoder::publishMotion(RefPic& R, int row0, int row1) const
{
    const size_t lw = size_t(mbW_) * 4, o = size_t(row0) * 4 * lw, n = size_t(row1 - row0) * 4 * lw;
    std::memcpy(&R.mvx[o], &mvX_[o], n * sizeof(int16_t));
    std::memcpy(&R.mvy[o], &mvY_[o], n * sizeof(int16_t));
    std::memcpy(&R.ref[o], &refIdx_[o], n);
    if (!bSlice_)
        return;
    for (size_t i = o; i < o + n; ++i)
        if (refIdx_[i] < 0 && refIdx1_[i] >= 0) {
            R.mvx[i] = mvX1_[i];
            R.mvy[i] = mvY1_[i];
            R.ref[i] = refIdx1_[i];
        }
}

// Entropy coding in raster order, mb_qp_delta against the real predecessor. It follows the
// decisions row by row (its own neighbour state), so with several threads only the last row's
// write remains after the wavefront.
void FrameCoder::waveWrite()
{
    WaveState& w = wave_;
    BitWriter& bw = slices_[0];
    sliceHeader(bw, 0);
    while (!bw.byteAligned())
        bw.u(1, 1); // cabac_alignment_one_bit
    CabacEncoder eng;
    eng.start(&bw, cfg_.qp, pSlice_ ? 0 : -1);
    int pred = cfg_.qp;
    bool prevNz = false;
    const size_t count = size_t(mbW_) * mbH_;
    for (int r = 0; r < w.rows; ++r) {
        while (w.progress[size_t(r)].load(std::memory_order_acquire) < mbW_)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        if (waveErrors_.failed())
            return; // the rows hold no finished decisions
        for (int mbx = 0; mbx < mbW_; ++mbx) {
            const size_t m = size_t(r) * mbW_ + mbx;
            MbSyntax& syn = mbSyn_[m];
            using K = MbSyntax::Kind;
            const bool sends = syn.kind == K::I16 || (syn.kind != K::Skip && (syn.cbpLuma || syn.cbpChroma));
            if (sends) {
                syn.qpDelta = mbQp_[m] - pred;
                pred = mbQp_[m];
            }
            mbQpFinal_[m] = int8_t(pred);
            cabacWriteMb(eng, cabWrite_, mbx, r, syn, prevNz);
            eng.terminate(m + 1 == count); // end_of_slice_flag
        }
        raiseTo(w.written, r + 1);
        wakeRow(r + 1); // it may filter row r now
    }
    bw.alignZero();
}

// After every row is decided: the writer's last row, the last row's deblocking and the rest of
// the reference picture.
void FrameCoder::waveEnd()
{
    WaveState& w = wave_;
    while (w.rowsDone.load(std::memory_order_acquire) < w.rows)
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    if (w.writer.joinable())
        w.writer.join();
    if (waveErrors_.failed()) {
        if (w.out)
            raiseTo(w.out->rowsReady, mbH_);
        w.nextRow.store(INT_MAX / 2, std::memory_order_release);
        return;
    }
    if (w.inlineDeblock) { // the last row has no row below to filter it
        for (int mbx = 0; mbx < mbW_; ++mbx)
            deblockMb(mbx, w.rows - 1);
        deblockedInline_ = true;
    }
    if (w.out) {
        for (int k = std::max(0, w.inlineDeblock ? w.rows - 2 : 0); k < w.rows; ++k)
            publishBand(k);
    }
    w.nextRow.store(INT_MAX / 2, std::memory_order_release);
    if (gTiming)
        gWaitMs += (w.waitUs.load() + w.refWaitUs.exchange(0)) / 1000.0;
}

std::vector<uint8_t> FrameCoder::assemble(PhaseClock& clock)
{
    clock.lap(4); // decisions and entropy coding
    std::vector<uint8_t> out;
    for (const BitWriter& bw : slices_) {
        if (idr_)
            appendNal(out, 3, 5, bw.bytes());
        else
            appendNal(out, refPic_ ? 2 : 0, 1, bw.bytes());
    }
    if (gStats && bSlice_)
        std::fprintf(stderr, "statsB: skip %ld direct %ld L0 %ld L1 %ld Bi %ld split %ld 8x8 %ld intra %ld t8 %ld\n", gStatB[0].load(),
                     gStatB[1].load(), gStatB[2].load(), gStatB[3].load(), gStatB[4].load(), gStatB[5].load(), gStatB[6].load(),
                     gStatB[7].load(), gStatB[8].load());
    if (gStats && pSlice_ && !bSlice_) {
        std::fprintf(stderr, "stats: inter %ld with luma %ld 8x8 %ld (%.1f %% of coded, %.1f %% of inter)\n", long(gStatInter.load()),
                     long(gStatInterCbp.load()), long(gStatT8.load()), 100.0 * gStatT8.load() / std::max<long>(1, gStatInterCbp.load()),
                     100.0 * gStatT8.load() / std::max<long>(1, gStatInter.load()));
        gStatInter = gStatInterCbp = gStatT8 = 0;
        std::fprintf(stderr, "stats: intra tries/wins by estimate ratio <0.5: %ld/%ld  <1: %ld/%ld  <1.5: %ld/%ld  <2: %ld/%ld\n",
                     long(gStatIntraTry[0].load()), long(gStatIntraWin[0].load()), long(gStatIntraTry[1].load()), long(gStatIntraWin[1].load()),
                     long(gStatIntraTry[2].load()), long(gStatIntraWin[2].load()), long(gStatIntraTry[3].load()), long(gStatIntraWin[3].load()));
        for (int i = 0; i < 4; ++i)
            gStatIntraTry[i] = gStatIntraWin[i] = 0;
        std::fprintf(stderr, "stats: t8 tries/wins by sa8d/satd (from 0.6, 0.05 steps):");
        for (int i = 0; i < 20; ++i)
            std::fprintf(stderr, " %ld/%ld", long(gStatT8Try[i].load()), long(gStatT8Win[i].load()));
        std::fprintf(stderr, "\nstats: split by 2*log2(whole/sqrt(lambda)+1): n/wins/loss permille:");
        for (int i = 0; i < 20; ++i)
            std::fprintf(stderr, " [%d] %.0f/%.0f/%.2f", i, gStatSplitN[i], gStatSplitWin[i], 1000.0 * gStatSplitLoss[i] / std::max(1.0, gStatInterTotal));
        std::fprintf(stderr, "\nstats: estimate, transform by |j4-j8|/lambda (<0.5 <1 <2 <4 <8 <16 <32 more) n/wrong/loss permille:");
        for (int i = 0; i < 8; ++i)
            std::fprintf(stderr, " %.0f/%.0f/%.3f", gStatEstT8[i][0], gStatEstT8[i][1], 1000.0 * gStatEstT8[i][2] / std::max(1.0, gStatEstTotal));
        std::fprintf(stderr, "\nstats: estimate, split by jSplit/jWhole (<0.8 <0.9 <0.95 <1 <1.05 <1.1 <1.2 more) n/split wins/gain permille:");
        for (int i = 0; i < 8; ++i)
            std::fprintf(stderr, " %.0f/%.0f/%.3f", gStatEstSplit[i][0], gStatEstSplit[i][1], 1000.0 * gStatEstSplit[i][2] / std::max(1.0, gStatEstTotal));
        std::fprintf(stderr, "\nstats: t8 loss if only 4x4 / only 8x8 (permille of the inter cost):");
        for (int i = 0; i < 20; ++i)
            std::fprintf(stderr, " %.2f/%.2f", 1000.0 * gStatT8LossIf4[i] / std::max(1.0, gStatT8Total),
                         1000.0 * gStatT8LossIf8[i] / std::max(1.0, gStatT8Total));
        std::fprintf(stderr, "\n");
    }
    if (cfg_.deblock && !deblockedInline_)
        deblockPicture();
    deblockedInline_ = false;
    clock.lap(5);
    return out;
}

// Codes the whole picture on cfg_.threads threads and returns it when done.
std::vector<uint8_t> FrameCoder::code(bool idr, int frameNum, int idrCount, PhaseClock& clock)
{
    reset(idr, frameNum, idrCount);
    if (useCabac()) {
        const int threads = std::clamp(cfg_.threads, 1, mbH_);
        waveBegin(threads, nullptr, threads > 1);
        auto worker = [&] {
            for (;;) {
                const int r = claimRow();
                if (r < 0)
                    return;
                waveRow(r);
            }
        };
        try {
            runOnThreads(threads, worker); // rows catch their own failures
        } catch (...) {
            waveFail(); // the writer thread must still end (a joinable std::thread terminates)
        }
        if (threads == 1)
            waveWriteGuarded();
        waveEnd(); // joins the writer
        rethrowWaveError();
    } else {
        // Slices only look at macroblocks of their own slice, so they run in parallel.
        const int sliceCount = int(slices_.size());
        std::atomic<int> next{0};
        runOnThreads(sliceCount, [&] {
            for (int s; (s = next.fetch_add(1)) < sliceCount;)
                encodeSlice(s);
        });
    }
    return assemble(clock);
}

// ---- VBV (EncoderConfig::vbvMaxrateKbps / vbvBufsizeKbits) ----

static const double gVbvSafe = std::getenv("H264_VBV_SAFE") ? std::atof(std::getenv("H264_VBV_SAFE")) : 2.0;
static const double gVbvDecay = std::getenv("H264_VBV_DECAY") ? std::atof(std::getenv("H264_VBV_DECAY")) : 0.6;
static const double gVbvTarget = std::getenv("H264_VBV_TARGET") ? std::atof(std::getenv("H264_VBV_TARGET")) : 0.5;
static const bool gVbvTrace = std::getenv("H264_VBV_TRACE") != nullptr;
// VBV inside the picture (FrameCoder::vbvRowAdjust); H264_VBV_ROWS=0: frame level only (measuring).
static const bool gVbvRows = !std::getenv("H264_VBV_ROWS") || std::atoi(std::getenv("H264_VBV_ROWS")) != 0;
static const double gVbvRowMargin = std::getenv("H264_VBV_ROWMARGIN") ? std::atof(std::getenv("H264_VBV_ROWMARGIN")) : 0.02;

// The real sizes of the pictures up to upTo (coding order) go into the decision side's buffer and bit model.
void Encoder::vbvLearn(int upTo)
{
    while (!vbvDone_.empty() && vbvDone_.front().frameNo <= upTo) {
        const VbvRecord r = vbvDone_.front();
        vbvDone_.pop_front();
        while (!vbvPred_.empty() && vbvPred_.front().first <= r.frameNo)
            vbvPred_.pop_front();
        vbvFill_ = std::min(vbvFill_ - r.bits + vbvTick(), vbvSize());
        if (r.cost > 0) {
            vbvKNum_[r.type] = vbvKNum_[r.type] * gVbvDecay + r.bits * std::exp2(r.qp / 6.0);
            vbvKDen_[r.type] = vbvKDen_[r.type] * gVbvDecay + r.cost;
            vbvQNum_[r.type] = vbvQNum_[r.type] * gVbvDecay + r.qp;
            vbvQDen_[r.type] = vbvQDen_[r.type] * gVbvDecay + 1.0;
        }
    }
}

double Encoder::vbvK(int type) const
{
    if (vbvKDen_[type] > 0)
        return vbvKNum_[type] / vbvKDen_[type];
    if (type == 3)
        return vbvK(1);
    // Not seen yet: from the other types (a B picture about half a P picture's bits per cost).
    if (type == 2 && vbvKDen_[1] > 0)
        return 0.5 * vbvKNum_[1] / vbvKDen_[1];
    if (type == 0 && vbvKDen_[1] > 0)
        return vbvKNum_[1] / vbvKDen_[1];
    if (type != 0 && vbvKDen_[0] > 0)
        return vbvKNum_[0] / vbvKDen_[0];
    return 1.0;
}

// Bits of a picture of this class and cost at qp: k * cost * 2^(-qp/6) - but below the QPs k was learned at, bits
// grow faster than that (noise and fine texture that a coarser QP quantised away come back: measured up to 4x for
// 4 QP), so there they double every gVbvSteep QP. It decides only where the buffer gets tight, so erring high
// costs little.
static const double gVbvSteep = std::getenv("H264_VBV_STEEP") ? std::atof(std::getenv("H264_VBV_STEEP")) : 3.0;

double Encoder::vbvPredict(int type, double cost, double qp) const
{
    const double k = vbvK(type) * cost;
    const int c = vbvQDen_[type] > 0 ? type : type == 3 ? 1 : type;
    if (vbvQDen_[c] <= 0)
        return k * std::exp2(-qp / 6.0);
    const double ref = vbvQNum_[c] / vbvQDen_[c];
    return qp >= ref ? k * std::exp2(-qp / 6.0) : k * std::exp2(-ref / 6.0 + (ref - qp) / gVbvSteep);
}

// A P or B picture whose motion cost is near its intra cost (a cut, a flash the scene detection let through)
// codes like an I picture: as many bits per cost as one, far more than a P picture's (measured 2.8x on a cut
// inside a run of B pictures), so it goes by the I pictures' k and teaches them.
static const double gVbvCut = std::getenv("H264_VBV_CUT") ? std::atof(std::getenv("H264_VBV_CUT")) : 0.6;

int Encoder::vbvClass(const LookaheadFrame& f, int type) const
{
    if (type == 0)
        return 0;
    const bool afterB = type == 1 && useB() && f.refDist > 1;
    const double motion = afterB && size_t(f.refDist) < f.fwd.size() && f.fwd[size_t(f.refDist)].valid() ? f.fwd[size_t(f.refDist)].sum : f.interSum;
    if (gVbvCut > 0 && f.intraSum > 0 && motion >= gVbvCut * f.intraSum)
        return 0;
    return afterB ? 3 : type;
}

// The bit model's cost of a queued picture (lookahead cost, as the rate control's) and in *shift the macroblock
// tree's finer QPs as one offset of the whole picture (its cost with the tree's weights is cost * 2^(-shift/6)):
// the picture's bits are predicted at its QP + shift, where the curve below the learned QPs is steeper. With f
// null (no lookahead) 1, so k is a picture's bits at QP 0 of its type.
double Encoder::vbvCost(const LookaheadFrame* f, int type, double* shift) const
{
    *shift = 0;
    if (!f)
        return 1.0;
    for (size_t j = 0; j < queue_.size(); ++j) {
        const LookaheadFrame& g = queue_[j];
        if (&g != f)
            continue;
        if (type == 2)
            return std::max(1.0, queueCost(j));
        const std::vector<int32_t>* inter =
            type == 1 && useB() && g.refDist > 1 && size_t(g.refDist) < g.fwd.size() && g.fwd[size_t(g.refDist)].valid()
                ? &g.fwd[size_t(g.refDist)].cost
                : nullptr;
        if (!inter)
            inter = &g.interCost;
        const size_t n = size_t(mbW_) * mbH_;
        if (g.intraCost.size() != n)
            return 1.0;
        double raw = 0, tree = 0;
        const double strength = treeStrength(g);
        for (size_t m = 0; m < n; ++m) {
            const double c = type == 0 ? g.intraCost[m] : std::min(g.intraCost[m], (*inter)[m]);
            raw += c;
            tree += cfg_.mbtree ? c * std::exp2(-std::max(-8.0f, Lookahead::qpOffset(g, int(m), strength)) / 6.0) : c;
        }
        if (raw < 1.0)
            return 1.0;
        *shift = -6.0 * std::log2(std::max(tree, 1e-9) / raw);
        return raw;
    }
    return 1.0;
}

// The first picture as I and the next as P, coded once at the stream's first QP without the tree or AQ: the
// bit model's start (the lookahead costs carry the tree's share).
void Encoder::vbvProbe(const LookaheadFrame& frame, double qpI)
{
    EncoderConfig pc = cfg_;
    pc.bitrateKbps = 0;
    pc.crf = -1;
    pc.vbvMaxrateKbps = 0;
    pc.lookahead = 0;
    pc.bframes = 0;
    pc.weightedPred = false;
    pc.mbtree = false;
    pc.aqStrength = 0;
    pc.firstPass.reset();
    pc.qp = std::clamp(int(std::lround(qpI)) + 3, 13, 51); // the probe's I picture is coded 3 below
    pc.gop = std::max(2, cfg_.gop);
    pc.sceneCut = false;
    pc.frameThreads = false;
    Encoder probe(pc);
    const bool costs = useLookahead() && !queue_.empty();
    const double bitsI = double(probe.encode(pictureOf(frame, cfg_.width)).size()) * 8.0;
    vbvKNum_[0] = bitsI * std::exp2((pc.qp - 3) / 6.0);
    vbvQNum_[0] = vbvQNum_[1] = pc.qp - 3; // (P: the probe's I and P together)
    vbvQDen_[0] = vbvQDen_[1] = 1.0;
    vbvKDen_[0] = costs ? std::max(1.0, frame.intraSum) : 1.0;
    if (costs && queue_.size() > 1 && &queue_[0] == &frame) {
        const LookaheadFrame& second = queue_[1];
        const int qpP = pc.qp;
        const double bitsP = double(probe.encode(pictureOf(second, cfg_.width)).size()) * 8.0;
        vbvKNum_[1] = bitsP * std::exp2(qpP / 6.0);
        vbvKDen_[1] = std::max(1.0, second.interSum);
    } else {
        vbvKNum_[1] = 0.3 * vbvKNum_[0]; // without costs: a P picture about a third of the I picture
        vbvKDen_[1] = vbvKDen_[0];
    }
}

// How far the picture's QP must rise (0 = not at all) so that neither it nor the planned pictures after it
// (the queue, in coding order, at the same QP plus their type's offset) run the buffer empty, and the
// buffer is no lower than half full (or than it is now plus half of what comes in, whichever is less) at
// the end of the lookahead. The picture itself (and those in flight) must fit gVbvSafe times its prediction: it is
// coded with a frame-level QP only, nothing corrects it once it runs over (a correction per macroblock row would
// depend on which rows of the wavefront are done, so the stream on the thread count). 2 against 1.3: on 0.5 s
// buffers far fewer and smaller underflows for no measurable quality.
double Encoder::vbvRaise(const LookaheadFrame* frame, int type, double qp, double cost, double shift, int bQp)
{
    const int cls = frame ? vbvClass(*frame, type) : type;
    const double size = vbvSize(), tick = vbvTick();
    double fill = vbvFill_, low = vbvFill_; // after the pictures in flight: as predicted, and with gVbvSafe times that
    for (const auto& p : vbvPred_) {
        fill = std::min(fill - p.second + tick, size);
        low = std::min(low - gVbvSafe * p.second + tick, size);
    }
    vbvLow_ = low;
    // The planned pictures in coding order (a run of B pictures after the reference picture that ends it).
    struct Planned {
        int type, cls;
        double cost, shift;
    };
    std::vector<Planned> plan;
    if (frame) {
        std::vector<Planned> run;
        for (size_t j = 0; j < queue_.size(); ++j) {
            const LookaheadFrame& g = queue_[j];
            if (g.coded || &g == frame)
                continue;
            const int t = g.keyframe ? 0 : useB() && g.type == 2 ? 2 : 1;
            double sh = 0;
            const double c = vbvCost(&g, t, &sh);
            if (t == 2) {
                run.push_back({t, vbvClass(g, t), c, sh});
            } else {
                plan.push_back({t, vbvClass(g, t), c, sh});
                plan.insert(plan.end(), run.begin(), run.end());
                run.clear();
            }
        }
        plan.insert(plan.end(), run.begin(), run.end());
    }
    auto offset = [&](int t) { return t == 0 ? (cfg_.gop > 1 ? -3.0 : 0.0) : t == 2 ? double(bQp) : 0.0; }; // (3 = P too)
    const double base = qp - offset(type); // the P QP the plan follows
    auto fits = [&](double d) {
        const double cur = vbvPredict(cls, cost, qp + shift + d);
        if (low - gVbvSafe * cur < 0)
            return false;
        double f = std::min(fill - cur + tick, size), seconds = 0;
        for (const Planned& p : plan) {
            f -= vbvPredict(p.cls, p.cost, std::min(51.0, base + offset(p.type) + d) + p.shift);
            if (f < 0)
                return false;
            f = std::min(f + tick, size);
            seconds += tick / vbvRate();
        }
        // A chunk followed by another (vbvEndFill): once the queue holds the rest, it ends at least that full.
        if (flushing_ && cfg_.vbvEndFill > 0 && f < cfg_.vbvEndFill * size)
            return false;
        return f >= std::min(fill + gVbvTarget * seconds * vbvRate(), gVbvTarget * size);
    };
    double d = 0;
    while (qp + d < 51.0 && !fits(d))
        d += 0.25;
    if (gVbvTrace)
        std::fprintf(stderr, "vbv: frame %d %c qp %.2f +%.2f fill %.1f %% predicted %.0f plan %zu class %d display %d intra %.0f inter %.0f dist %d\n",
                     frameIndex_, "IPB"[type], qp, d, 100.0 * fill / size, vbvPredict(cls, cost, std::min(51.0, qp + d) + shift), plan.size(), cls,
                     frame ? frame->index : -1, frame ? frame->intraSum : 0.0, frame ? frame->interSum : 0.0, frame ? frame->refDist : 0);
    return std::min(d, std::max(0.0, 51.0 - qp));
}

// SEI of an access unit with VBV (D.1.2, D.1.3): a buffering period at every IDR picture (the buffer level as
// the decoder has it before this picture's removal, in 90 kHz) and the picture timing (removal two ticks per
// picture after the last buffering period, output at its display turn).
std::vector<uint8_t> Encoder::vbvSei(const FrameCoder& fc, bool idr)
{
    auto message = [](BitWriter& sei, int type, BitWriter& payload) {
        if (!payload.byteAligned()) {
            payload.flag(true); // bit_equal_to_one
            payload.alignZero();
        }
        sei.u(uint32_t(type), 8);
        size_t n = payload.bytes().size();
        for (; n >= 255; n -= 255)
            sei.u(255, 8);
        sei.u(uint32_t(n), 8);
        sei.append(payload);
    };
    BitWriter sei;
    if (idr) {
        BitWriter bp;
        bp.ue(0); // seq_parameter_set_id
        const double fill = std::clamp(vbvOutFill_, 0.0, vbvSize());
        const uint32_t delay = uint32_t(std::max(1.0, std::floor(90000.0 * fill / vbvRate())));
        const uint32_t whole = uint32_t(std::floor(90000.0 * vbvSize() / vbvRate()));
        bp.u(delay, 24);                                // initial_cpb_removal_delay
        bp.u(whole > delay ? whole - delay : 0, 24);    // initial_cpb_removal_delay_offset
        message(sei, 0, bp);
    }
    BitWriter pt;
    pt.u(uint32_t(2 * (fc.frameNo_ - vbvBpFrame_)) & 0xFFFFFF, 24);                               // cpb_removal_delay
    pt.u(uint32_t(std::max(0, 2 * (fc.displayIndex_ + reorderDelay() - fc.frameNo_))) & 0xFFFFFF, 24); // dpb_output_delay
    message(sei, 1, pt);
    if (idr)
        vbvBpFrame_ = fc.frameNo_;
    sei.rbspTrailing();
    std::vector<uint8_t> out;
    appendNal(out, 0, 6, sei.bytes());
    return out;
}

FrameCoder* Encoder::startFrame(LookaheadFrame& frame)
{
    const bool forceKeyframe = frame.forceKeyframe;
    const Picture picture = pictureOf(frame, cfg_.width);
    PhaseClock clock;
    FrameCoder& fc = *coders_[size_t(nextCoder_)];
    nextCoder_ = (nextCoder_ + 1) % kCoders;
    fc.loadSource(picture);
    for (auto& l : fc.laMv_) // set again by takeLookaheadVectors where the lookahead has them
        for (auto& v : l)
            v.clear();
    clock.lap(0);
    const bool bMode = useB(); // the picture's type is decided already (planQueue)
    const bool lookaheadFrame = useLookahead() && !queue_.empty() && (bMode || &queue_.front() == &frame);

    // Keyframe on schedule, on request (a cut the caller knows) or on a detected scene change.
    const bool sceneChange = cfg_.sceneCut && cfg_.gop > 1 &&
                             (lookaheadFrame ? frame.sceneChange : fc.detectSceneCut(prevThumb_)); // keeps its thumbnail current
    clock.lap(1);
    const bool cut = bMode ? frame.cutKey && frame.type == 0 : sceneChange && frameIndex_ > 0 && framesSinceIdr_ >= 6;
    const bool idr = bMode ? frame.type == 0
                           : cfg_.gop <= 1 || frameIndex_ == 0 || framesSinceIdr_ >= cfg_.gop || forceKeyframe || cut;
    const int type = idr ? 0 : bMode && frame.type == 2 ? 2 : 1; // I, P, B
    if (idr)
        framesSinceIdr_ = 0;
    lastWasSceneCut_ = cut;
    const bool bRef = type == 2 && frame.bRef && usePyramid();
    // QP of a reference B picture above the P QP: between the P pictures' and the other B pictures'.
    // B pictures of short runs (up to kPyramidMaxRun, as placed on moving material): bQpOffsetShort.
    // Their P pictures have the tree's full strength, and with the long runs' offset on top the B
    // pictures fall up to 2 dB below them (a flicker at a quarter of the frame rate).
    const int bQp = type == 2 && frame.nearRun <= kPyramidMaxRun ? std::min(cfg_.bQpOffset, cfg_.bQpOffsetShort) : cfg_.bQpOffset;
    const int bRefQp = kBRefQpEnv >= 0 ? kBRefQpEnv : (bQp + 1) / 2;
    // frame_num: reference pictures since the IDR picture (without B pictures: all pictures).
    const int frameNum = bMode ? (idr ? 0 : refCount_) : framesSinceIdr_;
    if (idr)
        refList_.clear(); // an IDR picture marks all references unused
    fc.pSlice_ = !idr;
    fc.bSlice_ = type == 2;
    fc.refPic_ = type != 2 || bRef;
    fc.listMod_.clear();
    fc.mmcoDiff_ = -1;
    fc.pocLsb_ = bMode;
    fc.gopPos_ = frame.gopPos;
    if (type == 2) {
        // The reference pictures around it in display order: the one before (list 0) and the
        // one after, which was coded just before this run of B pictures (list 1).
        // B pyramid: the B pictures before the run's reference B picture lie between the picture
        // before and it, those after between it and the picture after.
        const bool withRef = bRefPic_ && !bRef;
        fc.refs_.assign(1, withRef && frame.index > bRefIndex_ ? bRefPic_ : refList_[1]);
        fc.refs1_.assign(1, withRef && frame.index < bRefIndex_ ? bRefPic_ : refList_[0]);
        fc.setBiWeights();
    } else {
        fc.refs_.assign(refList_.begin(), refList_.begin() + std::min<ptrdiff_t>(ptrdiff_t(refList_.size()), std::max(1, cfg_.refs)));
        fc.refs1_.clear();
        if (bRefPic_) {
            // The run before had a reference B picture: it is the newest picture in the decoder's
            // buffer and would lead list 0. The list is put in the order of refs_ and the B
            // picture marked unused, so the buffer holds the I and P pictures again.
            if (!idr) {
                int pred = frameNum;
                for (const auto& r : fc.refs_) {
                    fc.listMod_.push_back(pred - r->frameNum - 1);
                    pred = r->frameNum;
                }
                fc.mmcoDiff_ = frameNum - bRefFrameNum_ - 1;
            }
            refSpare_.push_back(std::move(bRefPic_));
            bRefPic_.reset();
            if (refSpare_.size() > size_t(kInFlight + 3))
                refSpare_.erase(refSpare_.begin());
        }
    }
    fc.wpAny_ = false;
    if (useWeightedPred() && type != 2) {
        fc.computeBlockMeans();
        if (type == 1)
            fc.planWeights();
        if (fc.wpAny_)
            ++weightedPictures_;
    }
    fc.activeRefs_ = int(fc.refs_.size());
    // With B pictures the picture coded before may still be in flight and is no reference: the
    // start points come from the reference picture's motion instead.
    fc.prev_ = bMode ? nullptr : lastStarted_;
    lastStarted_ = &fc;

    // Frame-level rate control (one-pass average-bitrate control aiming at a steady QP, which
    // is what PSNR likes best): a P picture gets the QP at which the average complexity of all P
    // pictures so far (bits * 2^(qp/6); bits roughly halve per +6 QP) meets the per-frame budget,
    // scaled by the bits over or under the target (overflow against a buffer of 5 s of bits) and at
    // most 2 away from the last one. I pictures use the P QP - 3. The first picture is probed at a
    // fixed QP so the stream starts near the right QP instead of paying for a huge first picture.
    const int baseQp = cfg_.qp;
    float qpFrac = 0; // constant quality: the picture's QP is cfg_.qp + qpFrac
    applyRc(frameIndex_ - inFlightAt(frameIndex_));
    const bool lookahead = lookaheadFrame;
    double costUsed = 0; // lookahead cost of this picture, for the bit model afterwards
    if (lookahead) {
        if (bMode) {
            planQueue(); // also the pictures that joined since the decision
            joinBSearches();
            takeLookaheadVectors(frame, fc, type);
        } else
            markKeyframes();
        frame.keyframe = idr;
        static const bool kBRefTree = !std::getenv("H264_BREFTREE") || std::atoi(std::getenv("H264_BREFTREE")) != 0;
        if (type == 2 && (!bRef || !kBRefTree)) {
            fc.treeOffset_.clear(); // nothing references a B picture
        } else if (cfg_.mbtree) {
            if (bMode)
                la_->propagateB(queue_, cfg_.bframes, cfg_.weightedBipred);
            else
                la_->propagate(queue_);
            fc.treeOffset_.resize(size_t(mbW_) * mbH_);
            for (size_t m = 0; m < fc.treeOffset_.size(); ++m)
                fc.treeOffset_[m] = std::max(-8.0f, Lookahead::qpOffset(frame, int(m), treeStrength(frame)));
        }
    } else {
        fc.treeOffset_.clear();
    }
    if (cfg_.bitrateKbps > 0 && lookahead) {
        // Rate control with lookahead: bits = k * cost * 2^(-qp/6) with k measured on the pictures
        // coded so far (per picture type, I pictures 3 below the P QP). The P QP is the one at
        // which a picture of average cost (all pictures seen so far, the queued ones included)
        // meets the per-frame budget - a steady QP is what PSNR likes best - nudged by the bits
        // over or under the target so far, and at most 3 away from the last one.
        const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
        auto kOf = [&](bool intra) {
            const int t = intra ? 0 : 1;
            return rcCostSum_[t] > 0 ? rcKSum_[t] / rcCostSum_[t] : rcKSum_[1 - t] / rcCostSum_[1 - t];
        };
        // B pictures: their own k once one is coded (before that a guess from the P pictures),
        // and their bits at the P QP shrink by the QP offset.
        auto kB = [&] { return rcCostSum_[2] > 0 ? rcKSum_[2] / rcCostSum_[2] : 0.4 * kOf(false); };
        auto kType = [&](int t) { return t == 2 ? kB() : kOf(t == 0); };
        auto typeScale = [&](int t) { return t == 0 ? std::exp2(0.5) : t == 2 ? std::exp2(-cfg_.bQpOffset / 6.0) : 1.0; };
        // Predicted bits at QP 0 of the queued pictures (and the one coded now).
        auto windowNeed = [&](int& count) {
            double need = 0;
            count = 0;
            for (size_t j = 0; j < queue_.size(); ++j) {
                const LookaheadFrame& g = queue_[j];
                if (g.coded && &g != &frame)
                    continue; // a reference picture coded ahead of its turn
                const int t = g.keyframe ? 0 : bMode && g.type == 2 ? 2 : 1;
                // With B pictures: a B picture between its references, a P picture from its reference.
                const std::vector<int32_t>* inter = bMode && t == 1 && g.refDist > 1 && size_t(g.refDist) < g.fwd.size() &&
                                                            g.fwd[size_t(g.refDist)].valid()
                                                        ? &g.fwd[size_t(g.refDist)].cost
                                                        : nullptr;
                const double c = t == 2 ? queueCost(j) : frameCost(g, g.keyframe, inter);
                if (&g == &frame)
                    costUsed = c;
                need += kType(t) * c * typeScale(t);
                ++count;
            }
            return need;
        };
        const bool windowOnly = std::getenv("H264_RC_WINDOW") != nullptr;
        // First pass: the whole clip's costs are known, one QP for all of it.
        const bool twoPass = cfg_.firstPass && size_t(frameIndex_) < cfg_.firstPass->size();
        if (twoPass && frameIndex_ == 0) {
            // Keyframes and picture types as the encoder will place them, with their costs.
            if (statType_.empty())
                planFromStats();
            const FirstPassStats& st = *cfg_.firstPass;
            tpKey_.assign(st.size(), 0);
            tpType_.assign(st.size(), 1);
            tpAfterB_.assign(st.size(), 0);
            for (size_t i = 0; i < st.size() && i < statType_.size(); ++i) {
                tpType_[i] = statType_[i];
                tpKey_[i] = statType_[i] == 0;
                tpAfterB_[i] = gTpAfterB > 0 && statType_[i] == 1 && i > 0 && statType_[i - 1] == 2;
            }
            tpOff_.clear();
            if (cfg_.qcomp > 0 && st.size() > 0) {
                const size_t n = st.size();
                const double mbs = double(mbW_) * mbH_;
                std::vector<double> lg(n + 1, 0.0); // prefix sums of log2(cost per macroblock + 1)
                for (size_t i = 0; i < n; ++i)
                    lg[i + 1] = lg[i] + std::log2(st.interSum[i] / mbs + 1.0);
                const double mean = lg[n] / double(n);
                const size_t w = size_t(std::max(0.0, gQcBlur * cfg_.fps));
                tpOff_.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    const size_t a = i > w ? i - w : 0, b = std::min(n, i + w + 1);
                    const double local = (lg[b] - lg[a]) / double(b - a);
                    tpOff_[i] = float(std::clamp(cfg_.qcomp * 6.0 * (local - mean), -gQcMax, gQcMax));
                }
            }
            // Each picture's class and its QP against the P QP: I pictures 3 below, B pictures by their
            // run's length (planQueue), all by the complexity compression. The references the pictures
            // will have (tpNeed): a P picture after a run of B pictures the I or P picture before the run,
            // a B picture the two around it (pyramid: the run's middle picture for those on either side).
            tpClass_.assign(st.size(), 0);
            tpQOff_.assign(st.size(), 0.0f);
            tpRef0_.assign(gTpRun ? st.size() : 0, -1);
            tpRef1_.assign(gTpRun ? st.size() : 0, -1);
            for (size_t i = 0; i < st.size();) {
                if (tpType_[i] != 2) {
                    tpClass_[i] = tpType_[i] == 0 ? 0 : uint8_t(2 + tpAfterB_[i] * kRcBins + rcBin(st.interSum[i]));
                    tpQOff_[i] = tpType_[i] == 0 ? -3.0f : 0.0f;
                    ++i;
                    continue;
                }
                size_t e = i;
                while (e < st.size() && tpType_[e] == 2)
                    ++e;
                const int count = int(e - i);
                const int bQpRun = count <= kPyramidMaxRun ? std::min(cfg_.bQpOffset, cfg_.bQpOffsetShort) : cfg_.bQpOffset;
                const size_t mid = usePyramid() && count >= 3 && count <= kPyramidMaxRun ? i + size_t(count / 2) : st.size();
                if (gTpRun && i > 0 && e < st.size()) {
                    for (size_t k = i; k < e; ++k) {
                        tpRef0_[k] = int32_t(mid < st.size() && k > mid ? mid : i - 1);
                        tpRef1_[k] = int32_t(mid < st.size() && k < mid ? mid : e);
                    }
                    if (tpType_[e] == 1) {
                        tpRef0_[e] = int32_t(i - 1);
                        tpClass_[e] = uint8_t(2 + tpAfterB_[e] * kRcBins + rcBin(tpRunCost(e)));
                        tpQOff_[e] = 0.0f;
                    }
                }
                for (size_t k = i; k < e; ++k) {
                    tpClass_[k] = gTpBinB ? uint8_t(2 + 2 * kRcBins + rcBin(tpRunCost(k))) : uint8_t(1);
                    tpQOff_[k] = float(k == mid ? (kBRefQpEnv >= 0 ? kBRefQpEnv : (bQpRun + 1) / 2) : bQpRun);
                }
                i = gTpRun && i > 0 && e < st.size() && tpType_[e] == 1 ? e + 1 : e;
            }
            for (size_t i = 0; i < st.size() && i < tpOff_.size(); ++i)
                tpQOff_[i] += tpOff_[i];
            tpBuildTables();
        }
        auto steadyQp = [&] {
            if (twoPass) {
                for (const LookaheadFrame& g : queue_)
                    if (&g == &frame)
                        costUsed = bMode && size_t(g.index) < statCost_.size() ? statCost_[size_t(g.index)]
                                                                               : g.keyframe ? g.intraSum : g.interSum;
                return twoPassQp(kOf(true), kOf(false), kB());
            }
            int count;
            const double need = windowNeed(count);
            const double all = windowOnly ? need : need + rcNeedSeen_;
            const int n = windowOnly ? count : count + rcFramesSeen_;
            return 6.0 * std::log2(std::max(1.0, all) / (n * perFrame));
        };
        if (frameIndex_ == 0) {
            // Probe: the first picture as I and the second as P calibrate the model. The bits
            // fall off faster than 2^(-qp/6) once the QP passes the noise, so the probe moves
            // towards the predicted QP until it is within reach.
            int probeQp = baseQp;
            for (int round = 0; round < 3; ++round) {
                EncoderConfig pc = cfg_;
                pc.bitrateKbps = 0;
                pc.crf = -1;
                pc.lookahead = 0;
                pc.bframes = 0;
                pc.weightedPred = false;
                pc.mbtree = false;
                pc.aqStrength = 0;
                pc.qp = probeQp;
                pc.gop = std::max(2, cfg_.gop);
                pc.sceneCut = false;
                pc.frameThreads = false;
                Encoder probe(pc);
                const double bitsI = double(probe.encode(picture).size()) * 8.0;
                const double kI = bitsI * std::exp2((probeQp - 3) / 6.0) / std::max(1.0, frame.intraSum);
                rcKSum_[0] = kI;
                rcCostSum_[0] = 1; // seeds only: the first real pictures outweigh them
                if (twoPass) // the two-pass model counts need above the dead zone (tpNeed)
                    rcKSum_[0] = bitsI / std::max(1.0, tpNeed(0, probeQp - 3));
                rcKSum_[1] = rcKSum_[0];
                rcCostSum_[1] = 1;
                if (queue_.size() > 1) {
                    const LookaheadFrame& second = queue_[1];
                    const double bitsP = double(probe.encode(pictureOf(second, cfg_.width)).size()) * 8.0;
                    rcKSum_[1] = bitsP * std::exp2(probeQp / 6.0) / std::max(1.0, second.interSum);
                    if (twoPass && cfg_.firstPass->size() > 1) {
                        // (The second picture as a P picture, whatever the plan makes of it.)
                        const uint8_t cls = std::exchange(tpClass_[1], uint8_t(2));
                        rcKSum_[1] = bitsP / std::max(1.0, tpNeed(1, probeQp, true));
                        tpClass_[1] = cls;
                    }
                }
                if (twoPass && !fc.treeOffset_.empty() && gTpSeedTree) {
                    // The probe codes without the macroblock tree; the real pictures have its finer QPs
                    // (one-pass: folded into each picture's cost instead).
                    double mean = 0;
                    for (float o : fc.treeOffset_)
                        mean += o;
                    const double more = std::exp2(-mean / double(fc.treeOffset_.size()) / 6.0);
                    rcKSum_[0] *= more;
                    rcKSum_[1] *= more;
                }
                const double predicted = steadyQp();
                if (std::getenv("H264_RC_TRACE"))
                    std::fprintf(stderr, "rc: probe at %d predicts %.1f\n", probeQp, predicted);
                if (std::abs(predicted - probeQp) <= 4.0)
                    break;
                probeQp = std::clamp(int(std::lround(std::clamp(predicted, probeQp - 6.0, probeQp + 6.0))), 10, 51);
            }
            if (twoPass)
                tpStartBoost(kOf(true), kOf(false));
            rcQp_ = 0;
        }
        // The debt only nudges the QP (at most +-3, paid back over 10 s): a quiet passage must not
        // pull the QP down just to spend its budget, that costs PSNR where it is already high.
        const double debt = rcBits_ - rcTarget_;
        const double overflow = std::clamp(1.0 + debt / (10.0 * perFrame * cfg_.fps), 0.7, 1.4);
        double qp = steadyQp() + 6.0 * std::log2(overflow);
        if (std::getenv("H264_RC_TRACE"))
            std::fprintf(stderr, "rc: frame %d overflow %.2f kI %.3f kP %.3f cost %.0f (intra %.0f inter %.0f) qp %.2f\n",
                         frameIndex_, overflow, kOf(true), kOf(false), costUsed, frame.intraSum, frame.interSum, qp);
        if (twoPass) {
            // The QP stays put; the model's k settles over the first pictures (the probe misses
            // by up to 2x, so up to 2 QP per picture then) and the leftover budget corrects
            // slowly (at most half a QP per picture).
            const double plan = twoPassPlanQp(kOf(true), kOf(false), kB());
            qp = twoPassQp(kOf(true), kOf(false), kB(), plan);
            const double rest = qp;
            if (rcFramesSeen_ >= 12) {
                const double gap = gTpDev > 0 ? std::clamp(rest - plan, -gTpDev, gTpDev) : rest - plan;
                if (gTpLag > 0)
                    tpGap_ += (gap - tpGap_) / std::max(1.0, gTpLag * cfg_.fps);
                qp = plan + gTpFollow * (qp - plan) + (1.0 - gTpFollow) * tpGap_;
            }
            if (gTpDev > 0 && rcFramesSeen_ >= 12)
                qp = std::clamp(qp, plan - gTpDev, plan + gTpDev);
            if (std::getenv("H264_RC_TRACE"))
                std::fprintf(stderr, "rc3: frame %d rest %.2f plan %.2f\n", frameIndex_, rest, plan);
            const double step = rcFramesSeen_ < 12 ? 2.0 : 0.5;
            if (rcQp_ > 0)
                qp = std::clamp(qp, rcQp_ - step, rcQp_ + step);
            const FirstPassStats& st = *cfg_.firstPass;
            const size_t i = std::min(size_t(frame.index), st.size() - 1);
            {
                std::vector<double> row(kTpQSteps);
                tpRow(i, row.data());
                double* from = tpRemTab_.data() + size_t(tpClass_[i]) * kTpQSteps;
                for (int k = 0; k < kTpQSteps; ++k)
                    from[k] = std::max(0.0, from[k] - row[size_t(k)]);
            }
            if (std::getenv("H264_RC_TRACE"))
                std::fprintf(stderr, "rc2: frame %d kI %.3f kP %.3f qp %.2f\n", frameIndex_, kOf(true), kOf(false), qp);
        } else if (rcQp_ > 0) {
            qp = std::clamp(qp, rcQp_ - 3.0, rcQp_ + 3.0);
        }
        rcQp_ = std::clamp(qp, 10.0, 51.0);
        if (twoPass && size_t(frame.index) < tpOff_.size())
            qp += tpOff_[size_t(frame.index)];
        cfg_.qp = std::clamp(int(std::lround(idr ? qp - 3 : type == 2 ? qp + (bRef ? bRefQp : bQp) : qp)), 10, 51);
        if (twoPass) // the model's cost of this picture: its need at the QP it is coded with, as cost * 2^(-qp/6)
            costUsed = tpNeed(std::min(size_t(frame.index), cfg_.firstPass->size() - 1), cfg_.qp) * std::exp2(cfg_.qp / 6.0);
        rcNeedSeen_ += kType(type) * costUsed * typeScale(type);
        rcFramesSeen_ += 1;
    } else if (cfg_.bitrateKbps > 0) {
        const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
        if (frameIndex_ == 0) {
            // The probe aims at an I picture of about four P budgets.
            EncoderConfig pc = cfg_;
            pc.bitrateKbps = 0;
            pc.crf = -1;
            pc.lookahead = 0;
            pc.bframes = 0;
            pc.weightedPred = false;
            pc.mbtree = false;
            pc.gop = std::max(2, cfg_.gop);
            pc.frameThreads = false;
            Encoder probe(pc);
            const double bits = double(probe.encode(picture).size()) * 8.0;
            const double qI = std::clamp((baseQp - 3) + 6.0 * std::log2(bits / (4.0 * perFrame)), 10.0, 48.0);
            rcQp_ = qI + 3;
            rcCplxSum_ = perFrame * std::pow(2.0, rcQp_ / 6.0);
            rcCplxCount_ = 1;
        }
        double qp = rcQp_;
        if (idr) {
            qp = rcQp_ - 3;
        } else if (rcCplxCount_ > 0) {
            const double debt = rcBits_ - rcTarget_;
            const double overflow = std::clamp(1.0 + debt / (5.0 * perFrame * cfg_.fps), 0.5, 2.0);
            qp = 6.0 * std::log2(rcCplxSum_ / rcCplxCount_ / perFrame) + 6.0 * std::log2(overflow);
            qp = std::clamp(qp, rcQp_ - 2.0, rcQp_ + 2.0);
        }
        cfg_.qp = std::clamp(int(std::lround(qp)), 10, 51);
    } else if (crfOn()) {
        // Constant quality: no budget and no model, the QP follows the content (crfQp); the fraction of a QP
        // goes into the macroblocks' offsets, so neighbouring pictures do not step by a whole QP.
        double complexity = 0;
        double p = crfQp(frame, &complexity);
        // The macroblock tree's level: next to long runs of B pictures the tree runs at a reduced strength
        // (treeStrength), which keeps its split between the reference and the B pictures but leaves the
        // whole picture coarser than the full strength would - still and calm footage (where those runs
        // are) came out 3 QP coarser against x264 than moving footage. So the picture's QP moves by the
        // mean offset of the full strength (unclamped) against the one applied; the split stays as it is.
        // B pictures (no tree of their own) take their reference's. H264_CRFTREEX: weight (measuring).
        static const double kCrfTreeX = std::getenv("H264_CRFTREEX") ? std::atof(std::getenv("H264_CRFTREEX")) : 1.0;
        if (!fc.treeOffset_.empty()) {
            double t = 0, x = 0;
            for (size_t m = 0; m < fc.treeOffset_.size(); ++m) {
                t += fc.treeOffset_[m];
                x += Lookahead::qpOffset(frame, int(m), cfg_.mbtreeStrength);
            }
            crfTreeShift_ = (x - t) / double(fc.treeOffset_.size());
        }
        p += kCrfTreeX * crfTreeShift_;
        const double q = std::clamp(idr ? (cfg_.gop > 1 ? p - 3 : p) : type == 2 ? p + (bRef ? bRefQp : bQp) : p, 10.0, 51.0);
        cfg_.qp = int(std::lround(q));
        qpFrac = float(q - cfg_.qp);
        if (std::getenv("H264_RC_TRACE"))
            std::fprintf(stderr, "crf: picture %d complexity %.2f P qp %.2f intra %.1f inter %.1f tree shift %.2f\n", frame.index, complexity, p,
                         frame.intraSum / (double(mbW_) * mbH_), frame.interSum / (double(mbW_) * mbH_), crfTreeShift_);
    } else if (idr && cfg_.gop > 1) {
        cfg_.qp = std::clamp(baseQp - 3, 0, 51); // keyframes are references for the whole GOP
    } else if (type == 2) {
        cfg_.qp = std::clamp(baseQp + (bRef ? bRefQp : bQp), 0, 51); // nothing (little) depends on a B picture
    }
    if (vbvOn()) {
        // VBV: the QP the mode chose, raised where the buffer needs it (vbvRaise).
        vbvLearn(frameIndex_ - inFlightAt(frameIndex_));
        const double q = cfg_.qp + qpFrac;
        if (frameIndex_ == 0)
            vbvProbe(frame, q);
        double shift = 0;
        const double cost = vbvCost(lookahead ? &frame : nullptr, type, &shift);
        const double d = vbvRaise(lookahead ? &frame : nullptr, type, q, cost, shift, bQp);
        if (d > 0) {
            const double nq = std::min(51.0, q + d);
            cfg_.qp = int(std::lround(nq));
            qpFrac = float(nq - cfg_.qp);
            if (cfg_.bitrateKbps > 0 && lookahead && cfg_.firstPass && size_t(frameIndex_) < cfg_.firstPass->size())
                costUsed = tpNeed(std::min(size_t(frame.index), cfg_.firstPass->size() - 1), cfg_.qp) * std::exp2(cfg_.qp / 6.0);
        }
        fc.vbvQp_ = cfg_.qp + qpFrac;
        fc.vbvCost_ = cost;
        fc.vbvClass_ = lookahead ? vbvClass(frame, type) : type;
        fc.vbvShift_ = shift;
        fc.vbvPred_ = vbvPredict(fc.vbvClass_, cost, fc.vbvQp_ + shift);
        vbvPred_.push_back({frameIndex_, fc.vbvPred_});
    }
    fc.cfg_.qp = cfg_.qp;
    fc.qpFrac_ = qpFrac;
    static const bool kQpTrace = std::getenv("H264_QP_TRACE") != nullptr;
    if (kQpTrace && lookahead) {
        double tm = 0;
        for (float o : fc.treeOffset_)
            tm += o;
        std::fprintf(stderr, "qp: picture %d %c qp %d tree %.2f cost %.0f\n", frame.index, "IPB"[type], cfg_.qp,
                     fc.treeOffset_.empty() ? 0.0 : tm / double(fc.treeOffset_.size()), costUsed);
    }
    // Psy strength follows the picture's QP (EncoderConfig::psyQpFull / psyQpOff): full below, none
    // above, linear between - texture is kept where there are bits for it and not invented where
    // the picture is starved.
    fc.cfg_.psyRd = cfg_.psyRd;
    if (cfg_.psyRd > 0 && cfg_.psyQpOff > cfg_.psyQpFull)
        fc.cfg_.psyRd = cfg_.psyRd * std::clamp(double(cfg_.psyQpOff - cfg_.qp) / double(cfg_.psyQpOff - cfg_.psyQpFull), 0.0, 1.0);
    fc.mbTypeOffset_ = idr ? 0 : 5;
    fc.frameNo_ = frameIndex_;
    fc.displayIndex_ = frame.index;
    fc.outQp_ = cfg_.qp;
    fc.outSceneCut_ = cut;
    if (lookahead && bMode)
        ensureBSearches(); // for the pictures after this one (nothing below reads their results)
    fc.bLongRun_ = type == 2 && frame.nearRun > kPyramidMaxRun;
    fc.biRefine_ = type != 2 ? 0 : gBiRefineEnv >= 0 ? gBiRefineEnv : frame.nearRun > kPyramidMaxRun ? kBiRefineLong : 0;
    fc.rcType_ = type;
    // Two-pass only: the one-pass model takes a P picture's cost over its real distance.
    fc.rcAfterB_ = type == 1 && bMode && frame.refDist > 1 && cfg_.firstPass && size_t(frame.index) < cfg_.firstPass->size();
    fc.rcLookahead_ = lookahead;
    fc.rcCost_ = costUsed;
    // A B picture's cost class in the two-pass bit model: the one it was planned in.
    fc.rcBin_ = type == 2 && cfg_.firstPass && size_t(frame.index) < tpClass_.size() && tpClass_[size_t(frame.index)] >= 2 + 2 * kRcBins
                    ? tpClass_[size_t(frame.index)] - (2 + 2 * kRcBins)
                    : -1;

    clock.lap(2); // keyframe decision, macroblock tree, rate control (probe on the first picture)
    fc.planMacroblockQp();
    fc.vbvRowCap_ = 0;
    if (vbvOn() && useCabac() && gVbvRows) {
        // The picture's slice data may take what the buffer holds at worst before it (the pictures in flight at
        // gVbvSafe times their prediction), less a margin for the SEI and headers and the counting CABAC's error.
        fc.vbvRowCap_ = std::max(1.0, vbvLow_ - gVbvRowMargin * vbvSize() - 2000.0);
        const size_t n = size_t(mbW_) * mbH_;
        const bool costs = lookahead && frame.intraCost.size() == n;
        const std::vector<int32_t>* inter = costs ? &frame.interCost : nullptr;
        if (costs && type == 1 && bMode && frame.refDist > 1 && size_t(frame.refDist) < frame.fwd.size() && frame.fwd[size_t(frame.refDist)].valid())
            inter = &frame.fwd[size_t(frame.refDist)].cost;
        fc.vbvW_.resize(n);
        fc.vbvPrefB_.assign(size_t(mbW_ + 1) * mbH_, 0.0);
        fc.vbvPrefE_.assign(size_t(mbW_ + 1) * mbH_, 0.0);
        fc.vbvPrefW_.assign(size_t(mbW_ + 1) * mbH_, 0.0);
        fc.vbvWTotal_ = 0;
        for (int r = 0; r < mbH_; ++r)
            for (int x = 0; x < mbW_; ++x) {
                const size_t m = size_t(r) * mbW_ + size_t(x);
                const double c = costs ? 1.0 + (type == 0 ? frame.intraCost[m] : std::min(frame.intraCost[m], (*inter)[m])) : 1.0;
                const double wgt = c * std::exp2(-(fc.mbQp_[m] - cfg_.qp) / 6.0);
                fc.vbvW_[m] = wgt;
                fc.vbvWTotal_ += wgt;
                const size_t k = size_t(r) * size_t(mbW_ + 1) + size_t(x);
                fc.vbvPrefW_[k + 1] = fc.vbvPrefW_[k] + wgt;
            }
    }
    clock.lap(3);
    // Sliding window (8.2.5.3): the newest picture goes first, the oldest drops out.
    std::shared_ptr<RefPic> pic;
    if (cfg_.gop > 1 && (type != 2 || bRef)) {
        if (!bRef && int(refList_.size()) >= std::max(bMode ? 2 : 1, cfg_.refs)) {
            pic = std::move(refList_.back());
            refList_.pop_back();
        }
        // A picture still referenced by one in flight waits in the spares; a spare nobody holds
        // any more is used again (no allocation and page faults of ~14 MB per 1080p picture).
        if (pic && pic.use_count() > 1) {
            refSpare_.push_back(std::move(pic));
            pic.reset();
            if (refSpare_.size() > size_t(kInFlight + 2))
                refSpare_.erase(refSpare_.begin()); // freed once the picture using it is done
        }
        for (size_t i = 0; !pic && i < refSpare_.size(); ++i)
            if (refSpare_[i].use_count() == 1) {
                pic = std::move(refSpare_[i]);
                refSpare_.erase(refSpare_.begin() + ptrdiff_t(i));
            }
        if (!pic)
            pic = std::make_shared<RefPic>();
        pic->gopPos = frame.gopPos;
        pic->refGopPos = fc.refs_.empty() ? frame.gopPos : fc.refs_[0]->gopPos;
        pic->frameNum = frameNum;
        if (useWeightedPred() && !bRef) {
            pic->dcY = fc.dcY_;
            pic->dcU = fc.dcU_;
            pic->dcV = fc.dcV_;
        }
        if (bRef) {
            bRefPic_ = pic;
            bRefIndex_ = frame.index;
            bRefFrameNum_ = frameNum;
        } else {
            refList_.push_front(pic);
        }
    }
    if (bMode && fc.refPic_)
        refCount_ = idr ? 1 : refCount_ + 1;
    if (pipelined()) {
        // Rows go to the workers; finished rows go into the reference picture band by band.
        if (!pool_)
            pool_ = std::make_unique<Pool>(cfg_.threads);
        fc.reset(idr, frameNum, idrCount_);
        fc.outPic_ = pic;
        fc.wave_.signal = &pool_->sig;
        fc.waveBegin(cfg_.threads, pic.get(), true);
        pool_->add(&fc);
    } else {
        fc.done_ = fc.code(idr, frameNum, idrCount_, clock);
        if (pic) {
            pic->y = fc.recY_;
            pic->u = fc.recU_;
            pic->v = fc.recV_;
            if (cfg_.bframes > 0) {
                pic->mvx.resize(fc.mvX_.size());
                pic->mvy.resize(fc.mvX_.size());
                pic->ref.resize(fc.mvX_.size());
                fc.publishMotion(*pic, 0, mbH_);
            }
            fc.buildRefPlanes(*pic);
            pic->rowsReady.store(mbH_, std::memory_order_release);
            pic->rowsReady.notify_all();
        }
    }
    cfg_.qp = baseQp;
    if (idr)
        ++idrCount_;
    ++frameIndex_;
    ++framesSinceIdr_;
    clock.lap(6); // reference planes
    if (gTiming) {
        for (int i = 0; i < 8; ++i)
            gPhaseMs[i] += clock.ms[i];
        if (++gPhaseFrames % 20 == 0)
            std::fprintf(stderr, "timing ms/frame: load %.1f scenecut %.1f tree+rc %.1f aq %.1f encode %.1f (threads waited %.1f) deblock %.1f refplanes %.1f\n",
                         gPhaseMs[0] / gPhaseFrames, gPhaseMs[1] / gPhaseFrames, gPhaseMs[2] / gPhaseFrames, gPhaseMs[3] / gPhaseFrames,
                         gPhaseMs[4] / gPhaseFrames, (gWaitMs + (pool_ ? pool_->idleUs.load() / 1000.0 : 0.0)) / gPhaseFrames, gPhaseMs[5] / gPhaseFrames, gPhaseMs[6] / gPhaseFrames);
    }
    return &fc;
}

void FrameCoder::encodeMbPcm(BitWriter& bw, int mbx, int mby)
{
    bw.ue(25); // mb_type I_PCM
    bw.alignZero();
    for (int y = 0; y < 16; ++y) {
        const size_t off = size_t(mby * 16 + y) * padW_ + mbx * 16;
        for (int x = 0; x < 16; ++x)
            bw.u(srcY_[off + x], 8);
        std::memcpy(&recY_[off], &srcY_[off], 16);
    }
    const int cw = padW_ / 2;
    for (auto [src, rec] : {std::pair{&srcU_, &recU_}, std::pair{&srcV_, &recV_}}) {
        for (int y = 0; y < 8; ++y) {
            const size_t off = size_t(mby * 8 + y) * cw + mbx * 8;
            for (int x = 0; x < 8; ++x)
                bw.u((*src)[off + x], 8);
            std::memcpy(&(*rec)[off], &(*src)[off], 8);
        }
    }
    // I_PCM counts as 16 coefficients in every block for nC prediction.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            nzLuma_[size_t(mby * 4 + y) * (mbW_ * 4) + mbx * 4 + x] = 16;
    for (int c = 0; c < 2; ++c)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                nzChroma_[c][size_t(mby * 2 + y) * (mbW_ * 2) + mbx * 2 + x] = 16;
}

int FrameCoder::lumaNc(int bx, int by) const
{
    const int w = mbW_ * 4;
    const bool hasA = bx > 0, hasB = (by & 3) ? true : topMbAvailable(by >> 2);
    const int nA = hasA ? nzLuma_[size_t(by) * w + bx - 1] : 0;
    const int nB = hasB ? nzLuma_[size_t(by - 1) * w + bx] : 0;
    if (hasA && hasB)
        return (nA + nB + 1) >> 1;
    return hasA ? nA : nB;
}

int FrameCoder::chromaNc(int c, int bx, int by) const
{
    const int w = mbW_ * 2;
    const bool hasA = bx > 0, hasB = (by & 1) ? true : topMbAvailable(by >> 1);
    const int nA = hasA ? nzChroma_[c][size_t(by) * w + bx - 1] : 0;
    const int nB = hasB ? nzChroma_[c][size_t(by - 1) * w + bx] : 0;
    if (hasA && hasB)
        return (nA + nB + 1) >> 1;
    return hasA ? nA : nB;
}


namespace {

// coded_block_pattern codeNum -> cbp for intra macroblocks (table 9-4, 4:2:0).
constexpr int kIntraCbpFromCode[48] = {47, 31, 15, 0,  23, 27, 29, 30, 7,  11, 13, 14, 39, 43, 45, 46,
                                       16, 3,  5,  10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1,  2,  4,
                                       8,  17, 18, 20, 24, 6,  9,  22, 25, 32, 33, 34, 36, 40, 38, 41};

int intraCbpCode(int cbp)
{
    static const auto table = [] {
        std::array<int, 48> t{};
        for (int code = 0; code < 48; ++code)
            t[size_t(kIntraCbpFromCode[code])] = code;
        return t;
    }();
    return table[size_t(cbp)];
}

constexpr int blkIdxAt(int x, int y) { return (y >> 1) * 8 + (x >> 1) * 4 + (y & 1) * 2 + (x & 1); }

// Intra 4x4 prediction (8.3.1.2). T[0] = p[-1,-1], T[1 + x] = p[x,-1] (x = 0..7), L[y] = p[-1,y].
template <int Mode>
inline void predict4x4Mode(const int* T, const int* L, bool left, bool top, int dc, uint8_t* pred)
{
    (void)left;
    (void)top;
    (void)dc;
    auto t = [&](int x) { return T[x + 1]; };              // x = -1 is the corner
    auto l = [&](int y) { return y < 0 ? T[0] : L[y]; }; // y = -1 is the corner
    (void)t;
    (void)l;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int v = 128;
            if constexpr (Mode == 0) {
                v = t(x);
            }
            if constexpr (Mode == 1) {
                v = l(y);
            }
            if constexpr (Mode == 2) {
                const int st = t(0) + t(1) + t(2) + t(3), sl = l(0) + l(1) + l(2) + l(3);
                v = (top && left) ? (st + sl + 4) >> 3 : left ? (sl + 2) >> 2 : top ? (st + 2) >> 2 : 128;
            }
            if constexpr (Mode == 3) {
                v = (x == 3 && y == 3) ? (t(6) + 3 * t(7) + 2) >> 2
                                       : (t(x + y) + 2 * t(x + y + 1) + t(x + y + 2) + 2) >> 2;
            }
            if constexpr (Mode == 4) {
                if (x > y)
                    v = (t(x - y - 2) + 2 * t(x - y - 1) + t(x - y) + 2) >> 2;
                else if (x < y)
                    v = (l(y - x - 2) + 2 * l(y - x - 1) + l(y - x) + 2) >> 2;
                else
                    v = (t(0) + 2 * T[0] + l(0) + 2) >> 2;
            }
            if constexpr (Mode == 5) {
                const int z = 2 * x - y, k = x - (y >> 1);
                if (z >= 0 && !(z & 1))
                    v = (t(k - 1) + t(k) + 1) >> 1;
                else if (z > 0)
                    v = (t(k - 2) + 2 * t(k - 1) + t(k) + 2) >> 2;
                else if (z == -1)
                    v = (l(0) + 2 * T[0] + t(0) + 2) >> 2;
                else
                    v = (l(y - 1) + 2 * l(y - 2) + l(y - 3) + 2) >> 2;
            }
            if constexpr (Mode == 6) {
                const int z = 2 * y - x, k = y - (x >> 1);
                if (z >= 0 && !(z & 1))
                    v = (l(k - 1) + l(k) + 1) >> 1;
                else if (z > 0)
                    v = (l(k - 2) + 2 * l(k - 1) + l(k) + 2) >> 2;
                else if (z == -1)
                    v = (l(0) + 2 * T[0] + t(0) + 2) >> 2;
                else
                    v = (t(x - 1) + 2 * t(x - 2) + t(x - 3) + 2) >> 2;
            }
            if constexpr (Mode == 7) {
                const int k = x + (y >> 1);
                v = (y & 1) ? (t(k) + 2 * t(k + 1) + t(k + 2) + 2) >> 2 : (t(k) + t(k + 1) + 1) >> 1;
            }
            if constexpr (Mode == 8) {
                const int z = x + 2 * y, k = y + (x >> 1);
                if (z > 5)
                    v = l(3);
                else if (z == 5)
                    v = (l(2) + 3 * l(3) + 2) >> 2;
                else if (z & 1)
                    v = (l(k) + 2 * l(k + 1) + l(k + 2) + 2) >> 2;
                else
                    v = (l(k) + l(k + 1) + 1) >> 1;
            }
            pred[y * 4 + x] = uint8_t(v);
        }
}

// One loop per mode (the mode switch outside the sample loops).
void predict4x4(int mode, const int* T, const int* L, bool left, bool top, uint8_t* pred)
{
    const int dc = 0; // Mode 2 sums its own neighbours
    switch (mode) {
    case 0: predict4x4Mode<0>(T, L, left, top, dc, pred); break;
    case 1: predict4x4Mode<1>(T, L, left, top, dc, pred); break;
    case 2: predict4x4Mode<2>(T, L, left, top, dc, pred); break;
    case 3: predict4x4Mode<3>(T, L, left, top, dc, pred); break;
    case 4: predict4x4Mode<4>(T, L, left, top, dc, pred); break;
    case 5: predict4x4Mode<5>(T, L, left, top, dc, pred); break;
    case 6: predict4x4Mode<6>(T, L, left, top, dc, pred); break;
    case 7: predict4x4Mode<7>(T, L, left, top, dc, pred); break;
    case 8: predict4x4Mode<8>(T, L, left, top, dc, pred); break;
    }
}

// Reference sample filtering for Intra 8x8 (8.3.2.2.1). T[0] = p[-1,-1], T[1 + x] = p[x,-1]
// (x = 0..15, the right half already substituted when missing), L[y] = p[-1,y] (y = 0..7).
void filterIntra8Refs(int* T, int* L, bool left, bool top, bool corner)
{
    int t[17], l[8];
    std::copy(T, T + 17, t);
    std::copy(L, L + 8, l);
    if (top) {
        T[1] = corner ? (t[0] + 2 * t[1] + t[2] + 2) >> 2 : (3 * t[1] + t[2] + 2) >> 2;
        for (int x = 1; x < 15; ++x)
            T[1 + x] = (t[x] + 2 * t[1 + x] + t[2 + x] + 2) >> 2;
        T[16] = (t[15] + 3 * t[16] + 2) >> 2;
    }
    if (corner) {
        if (top && left)
            T[0] = (t[1] + 2 * t[0] + l[0] + 2) >> 2;
        else if (top)
            T[0] = (3 * t[0] + t[1] + 2) >> 2;
        else if (left)
            T[0] = (3 * t[0] + l[0] + 2) >> 2;
    }
    if (left) {
        L[0] = corner ? (t[0] + 2 * l[0] + l[1] + 2) >> 2 : (3 * l[0] + l[1] + 2) >> 2;
        for (int y = 1; y < 7; ++y)
            L[y] = (l[y - 1] + 2 * l[y] + l[y + 1] + 2) >> 2;
        L[7] = (l[6] + 3 * l[7] + 2) >> 2;
    }
}

// Intra 8x8 prediction (8.3.2.2.2..10) from filtered references (layout as filterIntra8Refs). Output stride 8.
template <int Mode>
inline void predict8x8Mode(const int* T, const int* L, bool left, bool top, int dc, uint8_t* pred)
{
    (void)left;
    (void)top;
    (void)dc;
    auto t = [&](int x) { return T[x + 1]; };              // x = -1 is the corner
    auto l = [&](int y) { return y < 0 ? T[0] : L[y]; }; // y = -1 is the corner
    (void)t;
    (void)l;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            int v = 128;
            if constexpr (Mode == 0) {
                v = t(x);
            }
            if constexpr (Mode == 1) {
                v = l(y);
            }
            if constexpr (Mode == 2) {
                v = dc;
            }
            if constexpr (Mode == 3) {
                v = (x == 7 && y == 7) ? (t(14) + 3 * t(15) + 2) >> 2
                                       : (t(x + y) + 2 * t(x + y + 1) + t(x + y + 2) + 2) >> 2;
            }
            if constexpr (Mode == 4) {
                if (x > y)
                    v = (t(x - y - 2) + 2 * t(x - y - 1) + t(x - y) + 2) >> 2;
                else if (x < y)
                    v = (l(y - x - 2) + 2 * l(y - x - 1) + l(y - x) + 2) >> 2;
                else
                    v = (t(0) + 2 * T[0] + l(0) + 2) >> 2;
            }
            if constexpr (Mode == 5) {
                const int z = 2 * x - y, k = x - (y >> 1);
                if (z >= 0 && !(z & 1))
                    v = (t(k - 1) + t(k) + 1) >> 1;
                else if (z > 0)
                    v = (t(k - 2) + 2 * t(k - 1) + t(k) + 2) >> 2;
                else if (z == -1)
                    v = (l(0) + 2 * T[0] + t(0) + 2) >> 2;
                else
                    v = (l(y - 2 * x - 1) + 2 * l(y - 2 * x - 2) + l(y - 2 * x - 3) + 2) >> 2;
            }
            if constexpr (Mode == 6) {
                const int z = 2 * y - x, k = y - (x >> 1);
                if (z >= 0 && !(z & 1))
                    v = (l(k - 1) + l(k) + 1) >> 1;
                else if (z > 0)
                    v = (l(k - 2) + 2 * l(k - 1) + l(k) + 2) >> 2;
                else if (z == -1)
                    v = (l(0) + 2 * T[0] + t(0) + 2) >> 2;
                else
                    v = (t(x - 2 * y - 1) + 2 * t(x - 2 * y - 2) + t(x - 2 * y - 3) + 2) >> 2;
            }
            if constexpr (Mode == 7) {
                const int k = x + (y >> 1);
                v = (y & 1) ? (t(k) + 2 * t(k + 1) + t(k + 2) + 2) >> 2 : (t(k) + t(k + 1) + 1) >> 1;
            }
            if constexpr (Mode == 8) {
                const int z = x + 2 * y, k = y + (x >> 1);
                if (z > 13)
                    v = l(7);
                else if (z == 13)
                    v = (l(6) + 3 * l(7) + 2) >> 2;
                else if (z & 1)
                    v = (l(k) + 2 * l(k + 1) + l(k + 2) + 2) >> 2;
                else
                    v = (l(k) + l(k + 1) + 1) >> 1;
            }
            pred[y * 8 + x] = uint8_t(v);
        }
}

void predict8x8(int mode, const int* T, const int* L, bool left, bool top, uint8_t* pred)
{
    auto t = [&](int x) { return T[x + 1]; };              // x = -1 is the corner
    auto l = [&](int y) { return y < 0 ? T[0] : L[y]; }; // y = -1 is the corner
    int dc = 128;
    if (mode == 2) {
        int st = 0, sl = 0;
        for (int i = 0; i < 8; ++i) {
            st += t(i);
            sl += l(i);
        }
        dc = (top && left) ? (st + sl + 8) >> 4 : left ? (sl + 4) >> 3 : top ? (st + 4) >> 3 : 128;
    }
    switch (mode) {
    case 0: predict8x8Mode<0>(T, L, left, top, dc, pred); break;
    case 1: predict8x8Mode<1>(T, L, left, top, dc, pred); break;
    case 2: predict8x8Mode<2>(T, L, left, top, dc, pred); break;
    case 3: predict8x8Mode<3>(T, L, left, top, dc, pred); break;
    case 4: predict8x8Mode<4>(T, L, left, top, dc, pred); break;
    case 5: predict8x8Mode<5>(T, L, left, top, dc, pred); break;
    case 6: predict8x8Mode<6>(T, L, left, top, dc, pred); break;
    case 7: predict8x8Mode<7>(T, L, left, top, dc, pred); break;
    case 8: predict8x8Mode<8>(T, L, left, top, dc, pred); break;
    }
}

// Directional intra prediction by table. With the edge E = p[-1, N-1..0], the corner, p[0..2N-1, -1]
// (3N + 1 samples), every sample of the directional modes is (sum of w_i E_i + 2) >> 2 with quarter
// weights: a copy, a two-tap (a + b + 1) >> 1, a three-tap (a + 2b + c + 2) >> 2 or (a + 3b + 2) >> 2 at
// the far end. intraEdges() computes all of them once per block, the table (made by feeding the
// formulas above unit edges) says which one each sample of a mode is: same numbers, no per-sample
// branches. DC (mode 2) keeps its own path.
template <int N>
struct IntraDirTable {
    static constexpr int kE = 3 * N + 1;           // edge samples
    static constexpr int kG = kE + (kE - 1) + (kE - 2) + 2; // + two-tap + three-tap + two (1, 3) ends
    uint8_t idx[9][N * N] = {};
    bool ok = true;
    IntraDirTable()
    {
        // Weight vectors (quarters) of the candidate values.
        std::vector<std::array<int, kE>> cand(kG);
        for (auto& c : cand)
            c.fill(0);
        for (int j = 0; j < kE; ++j)
            cand[size_t(j)][size_t(j)] = 4;
        for (int j = 0; j < kE - 1; ++j)
            cand[size_t(kE + j)][size_t(j)] = cand[size_t(kE + j)][size_t(j + 1)] = 2;
        for (int j = 0; j < kE - 2; ++j) {
            auto& c = cand[size_t(2 * kE - 1 + j)];
            c[size_t(j)] = 1;
            c[size_t(j + 1)] = 2;
            c[size_t(j + 2)] = 1;
        }
        // (l[N-2] + 3 l[N-1] + 2) >> 2 and (t[2N-2] + 3 t[2N-1] + 2) >> 2.
        cand[size_t(kG - 2)][1] = 1;
        cand[size_t(kG - 2)][0] = 3;
        cand[size_t(kG - 1)][size_t(kE - 2)] = 1;
        cand[size_t(kG - 1)][size_t(kE - 1)] = 3;
        int w[9][N * N][kE] = {};
        for (int j = 0; j < kE; ++j) {
            int e[kE] = {};
            e[j] = 64;
            int T[2 * N + 1], L[N];
            T[0] = e[N];
            for (int x = 0; x < 2 * N; ++x)
                T[1 + x] = e[N + 1 + x];
            for (int y = 0; y < N; ++y)
                L[y] = e[N - 1 - y];
            for (int m = 0; m < 9; ++m) {
                if (m == 2)
                    continue;
                uint8_t out[N * N];
                if constexpr (N == 4)
                    predict4x4(m, T, L, true, true, out);
                else
                    predict8x8(m, T, L, true, true, out);
                for (int p = 0; p < N * N; ++p) {
                    if (out[p] % 16)
                        ok = false;
                    w[m][p][j] = out[p] / 16;
                }
            }
        }
        for (int m = 0; m < 9; ++m) {
            if (m == 2)
                continue;
            for (int p = 0; p < N * N; ++p) {
                int found = -1;
                for (int c = 0; c < kG && found < 0; ++c)
                    if (std::equal(cand[size_t(c)].begin(), cand[size_t(c)].end(), w[m][p]))
                        found = c;
                if (found < 0)
                    ok = false;
                else
                    idx[m][p] = uint8_t(found);
            }
        }
    }
    // All candidate values of one block's edge (T and L as predict4x4 / predict8x8 take them).
    static void edges(const int* T, const int* L, uint8_t* g)
    {
        int e[kE];
        for (int y = 0; y < N; ++y)
            e[N - 1 - y] = L[y];
        e[N] = T[0];
        for (int x = 0; x < 2 * N; ++x)
            e[N + 1 + x] = T[1 + x];
        for (int j = 0; j < kE; ++j)
            g[j] = uint8_t(e[j]);
        for (int j = 0; j < kE - 1; ++j)
            g[kE + j] = uint8_t((e[j] + e[j + 1] + 1) >> 1);
        for (int j = 0; j < kE - 2; ++j)
            g[2 * kE - 1 + j] = uint8_t((e[j] + 2 * e[j + 1] + e[j + 2] + 2) >> 2);
        g[kG - 2] = uint8_t((e[1] + 3 * e[0] + 2) >> 2);
        g[kG - 1] = uint8_t((e[kE - 2] + 3 * e[kE - 1] + 2) >> 2);
    }
    void predict(int mode, const uint8_t* g, uint8_t* pred) const
    {
        const uint8_t* t = idx[mode];
        for (int p = 0; p < N * N; ++p)
            pred[p] = g[t[p]];
    }
    void predict(int mode, const uint8_t* g, uint8_t* pred, int stride) const
    {
        const uint8_t* t = idx[mode];
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
                pred[y * stride + x] = g[t[y * N + x]];
    }
};

const IntraDirTable<4>& intraDir4()
{
    static const IntraDirTable<4> t;
    return t;
}
#ifdef SCHNEIDI_H264_AVX2
// All nine 4x4 intra predictions at once when top and left are available: g = the candidate edge
// values (IntraDirTable<4>::edges) with the DC value at 38, 48 bytes readable. Output: three
// 16 x 4 groups side by side (modes 0-3, 4-7, 8; the rest of the last group repeats mode 8), each
// row of a group gathered by byte shuffles from the three 16-byte parts of g.
struct Intra4Shuffles {
    alignas(32) uint8_t s[3][2][3][32]; // [group][row pair][part of g] -> two rows
    Intra4Shuffles()
    {
        const IntraDirTable<4>& dir = intraDir4();
        for (int k = 0; k < 3; ++k)
            for (int y = 0; y < 4; ++y)
                for (int j = 0; j < 4; ++j) {
                    const int mode = std::min(4 * k + j, 8);
                    for (int x = 0; x < 4; ++x) {
                        const int idx = mode == 2 ? 38 : dir.idx[mode][y * 4 + x];
                        for (int part = 0; part < 3; ++part)
                            s[k][y >> 1][part][(y & 1) * 16 + j * 4 + x] =
                                idx >= part * 16 && idx < part * 16 + 16 ? uint8_t(idx - part * 16) : uint8_t(0x80);
                    }
                }
    }
};
const Intra4Shuffles& intra4Shuffles()
{
    static const Intra4Shuffles t;
    return t;
}
__attribute__((target("avx2"))) void intra4PredictAll(const uint8_t* g, uint8_t (*group)[64])
{
    const Intra4Shuffles& t = intra4Shuffles();
    const __m256i g0 = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(g)));
    const __m256i g1 = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(g + 16)));
    const __m256i g2 = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(g + 32)));
    for (int k = 0; k < 3; ++k)
        for (int r = 0; r < 2; ++r) {
            const uint8_t(*sh)[32] = t.s[k][r];
            const __m256i v = _mm256_or_si256(
                _mm256_or_si256(_mm256_shuffle_epi8(g0, _mm256_load_si256(reinterpret_cast<const __m256i*>(sh[0]))),
                                _mm256_shuffle_epi8(g1, _mm256_load_si256(reinterpret_cast<const __m256i*>(sh[1])))),
                _mm256_shuffle_epi8(g2, _mm256_load_si256(reinterpret_cast<const __m256i*>(sh[2]))));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(group[k] + r * 32), v);
        }
}
inline bool haveIntraAll() { return haveAvx2(); }
#elif defined(SCHNEIDI_H264_NEON)
// The same with NEON table lookups (g = 48 bytes, the DC value at 38): one lookup per group row.
void intra4PredictAll(const uint8_t* g, uint8_t (*group)[64])
{
    static const auto kIdx = [] {
        std::array<std::array<uint8_t, 16>, 12> t{}; // [group * 4 + row]
        const IntraDirTable<4>& dir = intraDir4();
        for (int k = 0; k < 3; ++k)
            for (int y = 0; y < 4; ++y)
                for (int j = 0; j < 4; ++j) {
                    const int mode = std::min(4 * k + j, 8);
                    for (int x = 0; x < 4; ++x)
                        t[size_t(k * 4 + y)][size_t(j * 4 + x)] = uint8_t(mode == 2 ? 38 : dir.idx[mode][y * 4 + x]);
                }
        return t;
    }();
    const uint8x16x3_t tbl = {{vld1q_u8(g), vld1q_u8(g + 16), vld1q_u8(g + 32)}};
    for (int k = 0; k < 3; ++k)
        for (int y = 0; y < 4; ++y)
            vst1q_u8(group[k] + y * 16, vqtbl3q_u8(tbl, vld1q_u8(kIdx[size_t(k * 4 + y)].data())));
}
inline bool haveIntraAll() { return true; }
#endif
const IntraDirTable<8>& intraDir8()
{
    static const IntraDirTable<8> t;
    return t;
}

#ifdef SCHNEIDI_H264_AVX2
// The same for 8x8 blocks: g = IntraDirTable<8>::edges values with the DC value at 74, 80 bytes
// readable; out = the nine 8x8 predictions, 64 bytes each (stride 8).
struct Intra8Shuffles {
    alignas(32) uint8_t s[9][2][5][32]; // [mode][rows 0-3 / 4-7][part of g]
    Intra8Shuffles()
    {
        const IntraDirTable<8>& dir = intraDir8();
        for (int mode = 0; mode < 9; ++mode)
            for (int p = 0; p < 64; ++p) {
                const int idx = mode == 2 ? 74 : dir.idx[mode][p];
                for (int part = 0; part < 5; ++part)
                    s[mode][p >> 5][part][p & 31] =
                        idx >= part * 16 && idx < part * 16 + 16 ? uint8_t(idx - part * 16) : uint8_t(0x80);
            }
    }
};
__attribute__((target("avx2"))) void intra8PredictAll(const uint8_t* g, uint8_t (*out)[64])
{
    static const Intra8Shuffles t;
    __m256i gp[5];
    for (int part = 0; part < 5; ++part)
        gp[part] = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(g + part * 16)));
    for (int mode = 0; mode < 9; ++mode)
        for (int h = 0; h < 2; ++h) {
            __m256i v = _mm256_shuffle_epi8(gp[0], _mm256_load_si256(reinterpret_cast<const __m256i*>(t.s[mode][h][0])));
            for (int part = 1; part < 5; ++part)
                v = _mm256_or_si256(v, _mm256_shuffle_epi8(gp[part], _mm256_load_si256(reinterpret_cast<const __m256i*>(t.s[mode][h][part]))));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out[mode] + h * 32), v);
        }
}
#elif defined(SCHNEIDI_H264_NEON)
// The same with NEON table lookups (g = 80 bytes, the DC value at 74): 64-byte table, then the rest.
void intra8PredictAll(const uint8_t* g, uint8_t (*out)[64])
{
    static const auto kIdx = [] {
        std::array<std::array<uint8_t, 64>, 9> t{};
        const IntraDirTable<8>& dir = intraDir8();
        for (int mode = 0; mode < 9; ++mode)
            for (int p = 0; p < 64; ++p)
                t[size_t(mode)][size_t(p)] = uint8_t(mode == 2 ? 74 : dir.idx[mode][p]);
        return t;
    }();
    const uint8x16x4_t lo = {{vld1q_u8(g), vld1q_u8(g + 16), vld1q_u8(g + 32), vld1q_u8(g + 48)}};
    const uint8x16_t hi = vld1q_u8(g + 64);
    const uint8x16_t k64 = vdupq_n_u8(64);
    for (int mode = 0; mode < 9; ++mode)
        for (int p = 0; p < 64; p += 16) {
            const uint8x16_t i = vld1q_u8(kIdx[size_t(mode)].data() + p);
            // Indices 64..79 from the fifth part; below 64 the subtraction wraps out of range.
            vst1q_u8(out[mode] + p, vqtbx1q_u8(vqtbl4q_u8(lo, i), hi, vsubq_u8(i, k64)));
        }
}
#endif

} // namespace

struct FrameCoder::ChromaMb {
    int mode = 0;
    int cbp = 0;
    int dc[2][4];
    int ac[2][4][16];
};

void FrameCoder::analyseChroma(int mbx, int mby, ChromaMb& ch, const uint8_t (*interPred)[64])
{
    const bool left = mbx > 0, top = topMbAvailable(mby);
    const int cw = padW_ / 2;
    const size_t chromaOff = size_t(mby * 8) * cw + mbx * 8;
    const std::vector<uint8_t>* csrc[2] = {&srcU_, &srcV_};
    std::vector<uint8_t>* crec[2] = {&recU_, &recV_};
    uint8_t csrcBlk[2][64];
    for (int c = 0; c < 2; ++c)
        for (int y = 0; y < 8; ++y)
            std::memcpy(csrcBlk[c] + y * 8, &(*csrc[c])[chromaOff + size_t(y) * cw], 8);

    uint8_t cpred[2][64], ccand[2][64];
    int bestCost = 1 << 30;
    if (interPred) {
        ch.mode = 0;
        std::memcpy(cpred, interPred, sizeof(cpred));
    }
    for (int mode = 0; mode < 4 && !interPred; ++mode) {
        if ((mode == 1 && !left) || (mode == 2 && !top) || (mode == 3 && !(top && left)))
            continue;
        int cost = 0;
        for (int c = 0; c < 2; ++c) {
            predictChroma8(mode, &(*crec[c])[chromaOff], cw, left, top, ccand[c]);
            cost += sad(csrcBlk[c], ccand[c], 64);
        }
        if (cost < bestCost) {
            bestCost = cost;
            ch.mode = mode;
            std::memcpy(cpred, ccand, sizeof(cpred));
        }
    }

    const int qpc = chromaQp(mbQp_[size_t(mby) * mbW_ + mbx] + cfg_.chromaQpOffset);
    const int qpcDiv = qpc / 6, qpcMod = qpc % 6;
    const int cqbits = 15 + qpcDiv;
    const int cOffset = (1 << cqbits) / (interPred ? 6 : 3);
    bool anyDc = false, anyAc = false;
    bool acNz[2][4];
    for (int c = 0; c < 2; ++c) {
        int ccoef[4][16];
        for (int b = 0; b < 4; ++b) {
            const int ox = (b & 1) * 4, oy = (b >> 1) * 4;
            forward4x4Diff(csrcBlk[c] + oy * 8 + ox, 8, cpred[c] + oy * 8 + ox, 8, ccoef[b]);
        }
        const int a = ccoef[0][0], b = ccoef[1][0], cc = ccoef[2][0], d = ccoef[3][0];
        const int f[4] = {a + b + cc + d, a - b + cc - d, a + b - cc - d, a - b - cc + d};
        for (int i = 0; i < 4; ++i) {
            ch.dc[c][i] = quantize(f[i], kQuantMf[qpcMod][0], cqbits + 1, 2 * cOffset);
            anyDc |= ch.dc[c][i] != 0;
        }
        for (int blk = 0; blk < 4; ++blk) {
            ccoef[blk][0] = 0; // the DC went its own way above
            acNz[c][blk] = quant4x4(ccoef[blk], kRows.mf[qpcMod], cqbits, cOffset, kMaxLevel, ch.ac[c][blk]);
            anyAc |= acNz[c][blk];
        }
    }
    ch.cbp = anyAc ? 2 : anyDc ? 1 : 0;

    for (int c = 0; c < 2; ++c) {
        const int* l = ch.dc[c];
        const int f[4] = {l[0] + l[1] + l[2] + l[3], l[0] - l[1] + l[2] - l[3], l[0] + l[1] - l[2] - l[3],
                          l[0] - l[1] - l[2] + l[3]};
        uint8_t* r0 = &(*crec[c])[chromaOff];
        for (int blk = 0; blk < 4; ++blk) {
            const int ox = (blk & 1) * 4, oy = (blk >> 1) * 4;
            if (!acNz[c][blk] && f[blk] == 0) { // no residual: the prediction is the reconstruction
                for (int y = 0; y < 4; ++y)
                    std::memcpy(r0 + size_t(oy + y) * cw + ox, cpred[c] + (oy + y) * 8 + ox, 4);
                continue;
            }
            int d[16], r[16];
            dequant4x4(ch.ac[c][blk], kRows.v[qpcMod], qpcDiv, d); // ac[0] is 0
            d[0] = ((f[blk] * 16 * kDequantV[qpcMod][0]) << qpcDiv) >> 5;
            inverse4x4(d, r);
            addClip4x4(cpred[c] + oy * 8 + ox, 8, r, r0 + size_t(oy) * cw + ox, cw);
        }
    }
}

void FrameCoder::writeChroma(MbOut& o, int mbx, int mby, const ChromaMb& ch)
{
    BitWriter& bw = o.bw;
    o.syn.cbpChroma = ch.cbp;
    std::memcpy(o.syn.chromaDc, ch.dc, sizeof(o.syn.chromaDc));
    if (ch.cbp) {
        for (int c = 0; c < 2; ++c)
            residual(bw, ch.dc[c], 4, -1);
    }
    const int chw = mbW_ * 2;
    for (int c = 0; c < 2; ++c) {
        for (int blk = 0; blk < 4; ++blk) {
            const int bx = mbx * 2 + (blk & 1), by = mby * 2 + (blk >> 1);
            int total = 0;
            if (ch.cbp == 2) {
                int scan[15];
                for (int k = 1; k < 16; ++k)
                    scan[k - 1] = ch.ac[c][blk][kZigzag[k]];
                std::memcpy(o.syn.chromaAc[c][blk], scan, sizeof(scan));
                total = residual(bw, scan, 15, chromaNc(c, bx, by));
            } else {
                std::memset(o.syn.chromaAc[c][blk], 0, sizeof(o.syn.chromaAc[c][blk]));
            }
            nzChroma_[c][size_t(by) * chw + bx] = uint8_t(total);
        }
    }
}

bool FrameCoder::encodeLuma16(MbOut& o, int mbx, int mby, const ChromaMb& ch)
{
    BitWriter& bw = o.bw;
    const bool left = mbx > 0, top = topMbAvailable(mby);
    const int qp = mbQp_[size_t(mby) * mbW_ + mbx];
    const int qpDiv = qp / 6, qpMod = qp % 6;

    const size_t lumaOff = size_t(mby * 16) * padW_ + mbx * 16;
    uint8_t src[256];
    for (int y = 0; y < 16; ++y)
        std::memcpy(src + y * 16, &srcY_[lumaOff + size_t(y) * padW_], 16);
    uint8_t* rec = &recY_[lumaOff];

    uint8_t pred[256], candidate[256];
    int bestMode = 2, bestCost = 1 << 30;
    for (int mode = 0; mode < 4; ++mode) {
        if ((mode == 0 && !top) || (mode == 1 && !left) || (mode == 3 && !(top && left)))
            continue;
        predictLuma16(mode, rec, padW_, left, top, candidate);
        const int cost = sad(src, candidate, 256);
        if (cost < bestCost) {
            bestCost = cost;
            bestMode = mode;
            std::memcpy(pred, candidate, 256);
        }
    }

    int coef[16][16]; // spatial block index b = y * 4 + x
    for (int b = 0; b < 16; ++b) {
        const int ox = (b & 3) * 4, oy = (b >> 2) * 4;
        forward4x4Diff(src + oy * 16 + ox, 16, pred + oy * 16 + ox, 16, coef[b]);
    }

    const int qbits = 15 + qpDiv;
    const int intraOffset = tIntraFine ? int((1 << qbits) * gPsyIntraRound) : (1 << qbits) / 3;
    int dcIn[16], dcT[16], dcLevel[16];
    for (int b = 0; b < 16; ++b)
        dcIn[b] = coef[b][0];
    hadamard4x4(dcIn, dcT);
    for (int i = 0; i < 16; ++i)
        dcLevel[i] = quantize(dcT[i] / 2, kQuantMf[qpMod][0], qbits + 1, 2 * intraOffset);

    int acLevel[16][16];
    bool anyAc = false;
    for (int b = 0; b < 16; ++b) {
        coef[b][0] = 0; // the DC is in dcIn
        anyAc |= quant4x4(coef[b], kRows.mf[qpMod], qbits, intraOffset, kMaxLevel, acLevel[b]);
    }
    const int cbpLuma = anyAc ? 15 : 0;

    int dcF[16];
    hadamard4x4(dcLevel, dcF);
    const int dcScale = 16 * kDequantV[qpMod][0];
    for (int b = 0; b < 16; ++b) {
        int d[16];
        dequant4x4(acLevel[b], kRows.v[qpMod], qpDiv, d); // acLevel[b][0] is 0
        if (qp >= 36)
            d[0] = (dcF[b] * dcScale) << (qpDiv - 6);
        else
            d[0] = (dcF[b] * dcScale + (1 << (5 - qpDiv))) >> (6 - qpDiv);
        int r[16];
        inverse4x4(d, r);
        const int ox = (b & 3) * 4, oy = (b >> 2) * 4;
        addClip4x4(pred + oy * 16 + ox, 16, r, rec + size_t(oy) * padW_ + ox, padW_);
    }

    // Syntax (7.3.5).
    o.syn.kind = MbSyntax::Kind::I16;
    o.syn.i16Mode = bestMode;
    o.syn.chromaMode = ch.mode;
    o.syn.cbpLuma = cbpLuma;
    o.syn.hasQpDelta = true;
    o.syn.qpDelta = qp - qpPredOf(mby);
    bw.ue(uint32_t(mbTypeOffset_ + 1 + bestMode + 4 * ch.cbp + (cbpLuma ? 12 : 0))); // mb_type I_16x16_*
    bw.ue(uint32_t(ch.mode)); // intra_chroma_pred_mode
    bw.se(qp - qpPredOf(mby)); // mb_qp_delta (always present for Intra 16x16)

    const int mbBx = mbx * 4, mbBy = mby * 4, lw = mbW_ * 4;
    {
        int scan[16];
        for (int k = 0; k < 16; ++k)
            scan[k] = dcLevel[kZigzag[k]];
        std::memcpy(o.syn.lumaDc, scan, sizeof(scan));
        residual(bw, scan, 16, lumaNc(mbBx, mbBy));
    }
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        const int bx = mbBx + kBlkX[blkIdx], by = mbBy + kBlkY[blkIdx];
        int total = 0;
        if (cbpLuma) {
            const int b = kBlkY[blkIdx] * 4 + kBlkX[blkIdx];
            int scan[15];
            for (int k = 1; k < 16; ++k)
                scan[k - 1] = acLevel[b][kZigzag[k]];
            std::memcpy(o.syn.luma[blkIdx], scan, sizeof(scan));
            total = residual(bw, scan, 15, lumaNc(bx, by));
        } else {
            std::memset(o.syn.luma[blkIdx], 0, sizeof(o.syn.luma[blkIdx]));
        }
        nzLuma_[size_t(by) * lw + bx] = uint8_t(total);
        modes4x4_[size_t(by) * lw + bx] = -1;
    }
    writeChroma(o, mbx, mby, ch);
    return true;
}

bool FrameCoder::encodeLuma4(MbOut& o, int mbx, int mby, const ChromaMb& ch, int* satdOut, int bound)
{
    BitWriter& bw = o.bw;
    const int qp = mbQp_[size_t(mby) * mbW_ + mbx];
    const int qpDiv = qp / 6, qpMod = qp % 6;
    const int qbits = 15 + qpDiv;
    const int intraOffset = tIntraFine ? int((1 << qbits) * gPsyIntraRound) : (1 << qbits) / 3;
    const int lw = mbW_ * 4;
    // SATD-domain lambda for the mode decision.
    const int lambda = std::max(1, int(0.9 * std::pow(2.0, (qp - 12) / 6.0) + 0.5));

    int modes[16], predModes[16];
    int levels[16][16]; // per blkIdx, raster order
    int cbpLuma = 0;
    int satdSum = 0; // chosen modes: SATD + lambda * mode bits

    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        const int bxIn = kBlkX[blkIdx], byIn = kBlkY[blkIdx];
        const int bx = mbx * 4 + bxIn, by = mby * 4 + byIn;
        const int px = bx * 4, py = by * 4;
        const bool left = px > 0, top = byIn > 0 || topMbAvailable(mby);
        bool topRight;
        if (byIn == 0)
            topRight = top && (bxIn < 3 || mbx + 1 < mbW_);
        else
            topRight = bxIn < 3 && blkIdxAt(bxIn + 1, byIn - 1) < blkIdx;

        int T[9] = {128, 128, 128, 128, 128, 128, 128, 128, 128};
        int L[4] = {128, 128, 128, 128};
        if (top) {
            const uint8_t* above = &recY_[size_t(py - 1) * padW_ + px];
            for (int i = 0; i < 4; ++i)
                T[1 + i] = above[i];
            for (int i = 4; i < 8; ++i)
                T[1 + i] = topRight ? above[i] : above[3];
        }
        if (left)
            for (int i = 0; i < 4; ++i)
                L[i] = recY_[size_t(py + i) * padW_ + px - 1];
        if (top && left)
            T[0] = recY_[size_t(py - 1) * padW_ + px - 1];

        int predMode = 2;
        if (left && top) {
            const int a = modes4x4_[size_t(by) * lw + bx - 1];
            const int b = modes4x4_[size_t(by - 1) * lw + bx];
            predMode = std::min(a < 0 ? 2 : a, b < 0 ? 2 : b);
        }
        predModes[blkIdx] = predMode;

        int src[16];
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                src[y * 4 + x] = srcY_[size_t(py + y) * padW_ + px + x];

        uint8_t pred[16], cand[16];
        const IntraDirTable<4>& dir = intraDir4();
        uint8_t edge[48] = {}; // IntraDirTable<4>::kG values, [38] = DC for the shuffle path
        static_assert(IntraDirTable<4>::kG <= 38);
        if (dir.ok)
            IntraDirTable<4>::edges(T, L, edge);
        int bestMode = 2, bestCost = 1 << 30;
        auto tryMode = [&](int mode) {
            const bool needsTop = mode == 0 || mode == 3 || mode == 7;
            const bool needsLeft = mode == 1 || mode == 8;
            const bool needsBoth = mode >= 4 && mode <= 6;
            if ((needsTop && !top) || (needsLeft && !left) || (needsBoth && !(top && left)))
                return 1 << 30;
            if (mode != 2 && dir.ok)
                dir.predict(mode, edge, cand);
            else
                predict4x4(mode, T, L, left, top, cand);
            const int cost = satdU8(&srcY_[size_t(py) * padW_ + px], padW_, cand, 4) + lambda * (mode == predMode ? 1 : 4);
            if (cost < bestCost) {
                bestCost = cost;
                bestMode = mode;
                std::memcpy(pred, cand, 16);
            }
            return cost;
        };
        if (gExp & 64) {
            intraModeSearch(tryMode, predMode);
        } else {
            // All nine directions: predictions four side by side, one SATD call per four (same
            // costs and the same first-lowest pick as trying them one by one).
            alignas(16) uint8_t group[3][64];
            int modeOf[9], n = 0;
#if defined(SCHNEIDI_H264_AVX2) || defined(SCHNEIDI_H264_NEON)
            if (dir.ok && top && left && haveIntraAll()) {
                edge[38] = uint8_t((T[1] + T[2] + T[3] + T[4] + L[0] + L[1] + L[2] + L[3] + 4) >> 3);
                intra4PredictAll(edge, group);
                for (; n < 9; ++n)
                    modeOf[n] = n;
            }
#endif
            for (int mode = n ? 9 : 0; mode < 9; ++mode) {
                const bool needsTop = mode == 0 || mode == 3 || mode == 7;
                const bool needsLeft = mode == 1 || mode == 8;
                const bool needsBoth = mode >= 4 && mode <= 6;
                if ((needsTop && !top) || (needsLeft && !left) || (needsBoth && !(top && left)))
                    continue;
                uint8_t* g = group[n >> 2] + (n & 3) * 4;
                if (mode != 2 && dir.ok) {
                    dir.predict(mode, edge, g, 16);
                } else {
                    predict4x4(mode, T, L, left, top, cand);
                    for (int y = 0; y < 4; ++y)
                        std::memcpy(g + y * 16, cand + y * 4, 4);
                }
                modeOf[n++] = mode;
            }
            int satd[12];
            const uint8_t* srcBlk = &srcY_[size_t(py) * padW_ + px];
            int k = 0;
            for (; k + 4 <= n || (n - k >= 2 && k < n); k += 4) {
                for (int j = n - k; j < 4; ++j) // unused slots of the last group
                    for (int y = 0; y < 4; ++y)
                        std::memset(group[k >> 2] + y * 16 + j * 4, 0, 4);
                satd4x4x4U8(srcBlk, padW_, group[k >> 2], satd + k);
            }
            for (; k < n; ++k)
                satd[k] = satdU8(srcBlk, padW_, group[k >> 2] + (k & 3) * 4, 16);
            int bestI = 0;
            for (int i = 0; i < n; ++i) {
                const int cost = satd[i] + lambda * (modeOf[i] == predMode ? 1 : 4);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestMode = modeOf[i];
                    bestI = i;
                }
            }
            for (int y = 0; y < 4; ++y)
                std::memcpy(pred + y * 4, group[bestI >> 2] + y * 16 + (bestI & 3) * 4, 4);
        }
        satdSum += bestCost;
        if (satdSum > bound) {
            if (satdOut)
                *satdOut = -1;
            return false; // clearly worse than something else: the caller drops it
        }
        modes[blkIdx] = bestMode;
        modes4x4_[size_t(by) * lw + bx] = int8_t(bestMode);

        int coef[16];
        forward4x4Diff(&srcY_[size_t(py) * padW_ + px], padW_, pred, 4, coef);
        if (quant4x4(coef, kRows.mf[qpMod], qbits, intraOffset, kMaxLevel, levels[blkIdx])) {
            cbpLuma |= 1 << (blkIdx >> 2);
            int d[16], r[16];
            dequant4x4(levels[blkIdx], kRows.v[qpMod], qpDiv, d);
            inverse4x4(d, r);
            addClip4x4(pred, 4, r, &recY_[size_t(py) * padW_ + px], padW_);
        } else {
            for (int y = 0; y < 4; ++y)
                std::memcpy(&recY_[size_t(py + y) * padW_ + px], pred + y * 4, 4);
        }
    }

    if (satdOut)
        *satdOut = satdSum;
    // Syntax (7.3.5): I_NxN.
    o.syn.kind = MbSyntax::Kind::I4;
    std::memcpy(o.syn.i4Mode, modes, sizeof(modes));
    std::memcpy(o.syn.i4Pred, predModes, sizeof(predModes));
    o.syn.chromaMode = ch.mode;
    o.syn.cbpLuma = cbpLuma;
    bw.ue(uint32_t(mbTypeOffset_));
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        if (modes[blkIdx] == predModes[blkIdx]) {
            bw.flag(true);
        } else {
            bw.flag(false);
            bw.u(uint32_t(modes[blkIdx] < predModes[blkIdx] ? modes[blkIdx] : modes[blkIdx] - 1), 3);
        }
    }
    bw.ue(uint32_t(ch.mode));
    const int cbp = cbpLuma | (ch.cbp << 4);
    bw.ue(uint32_t(intraCbpCode(cbp)));
    o.syn.hasQpDelta = cbp != 0;
    o.syn.qpDelta = qp - qpPredOf(mby);
    if (cbp)
        bw.se(qp - qpPredOf(mby)); // mb_qp_delta
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        const int bx = mbx * 4 + kBlkX[blkIdx], by = mby * 4 + kBlkY[blkIdx];
        int total = 0;
        int scan[16];
        for (int k = 0; k < 16; ++k)
            scan[k] = levels[blkIdx][kZigzag[k]];
        std::memcpy(o.syn.luma[blkIdx], scan, sizeof(scan));
        if (cbpLuma & (1 << (blkIdx >> 2)))
            total = residual(bw, scan, 16, lumaNc(bx, by));
        nzLuma_[size_t(by) * lw + bx] = uint8_t(total);
    }
    writeChroma(o, mbx, mby, ch);
    return cbp != 0;
}

bool FrameCoder::encodeMbIntra(MbOut& out, int mbx, int mby)
{
    ChromaMb ch;
    analyseChroma(mbx, mby, ch);
    if (cfg_.mode == IntraMode::Intra16)
        return encodeLuma16(out, mbx, mby, ch);
    if (cfg_.mode == IntraMode::Intra4)
        return encodeLuma4(out, mbx, mby, ch);

    // Try both, keep the one with the lower SSD + lambda * bits.
    const size_t lumaOff = size_t(mby * 16) * padW_ + mbx * 16;
    const int lw = mbW_ * 4;
    auto ssd = [&] {
        int64_t s = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const size_t o = lumaOff + size_t(y) * padW_ + x;
                const int d = int(srcY_[o]) - int(recY_[o]);
                s += d * d;
            }
        return double(s) + psyCost(mbx, mby);
    };
    struct Snapshot {
        uint8_t rec[256];
        uint8_t nz[16];
        int8_t modes[16];
    };
    auto save = [&](Snapshot& s) {
        for (int y = 0; y < 16; ++y)
            std::memcpy(s.rec + y * 16, &recY_[lumaOff + size_t(y) * padW_], 16);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                s.nz[y * 4 + x] = nzLuma_[o];
                s.modes[y * 4 + x] = modes4x4_[o];
            }
    };
    auto restore = [&](const Snapshot& s) {
        for (int y = 0; y < 16; ++y)
            std::memcpy(&recY_[lumaOff + size_t(y) * padW_], s.rec + y * 16, 16);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                nzLuma_[o] = s.nz[y * 4 + x];
                modes4x4_[o] = s.modes[y * 4 + x];
            }
    };

    const double lambda = 0.85 * std::pow(2.0, (mbQp_[size_t(mby) * mbW_ + mbx] - 12) / 3.0);
    int picked = -1; // 0 Intra 16x16, 1 Intra 4x4, 2 Intra 8x8
    double pickedCost = 0;
    const auto pick = [&]() -> bool {
    if (tIntraEst16 >= 0 && gIntraSel > 0) {
        // SATD pre-selection: Intra 4x4 and 8x8 are analysed block by block (and given up once
        // clearly worse than the best so far or than inter); full RD (and the Intra 16x16 coding)
        // only for the types near the best SATD.
        const int est16 = tIntraSkip16 ? INT_MAX : tIntraEst16;
        const double interLim = gIntraInter > 0 && tInterCost >= 0 ? gIntraInter * tInterCost : 1e18;
        auto boundOf = [&](double best) {
            const double b = std::min(gIntraSel * best, interLim);
            return b >= double(INT_MAX) ? INT_MAX : int(b);
        };
        MbOut b, c;
        int satd4 = -1, satd8 = -1;
        const bool deltaB = encodeLuma4(b, mbx, mby, ch, &satd4, boundOf(double(est16)));
        Snapshot snapB, snapC;
        if (satd4 >= 0)
            save(snapB);
        bool deltaC = false;
        if (useT8() && !tIntraSkip8) {
            deltaC = encodeLuma8(c, mbx, mby, ch, boundOf(double(std::min(satd4 >= 0 ? satd4 : INT_MAX, est16))), &satd8);
            if (satd8 >= 0)
                save(snapC);
        }
        int minSat = est16;
        if (satd4 >= 0)
            minSat = std::min(minSat, satd4);
        if (satd8 >= 0)
            minSat = std::min(minSat, satd8);
        const double lim = std::min(gIntraSel * minSat, interLim);
        double costA = 1e300, costB = 1e300, costC = 1e300;
        if (satd8 >= 0 && satd8 <= lim) {
            restore(snapC);
            costC = ssd() + lambda * mbBits(c, mbx, mby, 0.0);
        }
        if (satd4 >= 0 && satd4 <= lim) {
            restore(snapB);
            costB = ssd() + lambda * mbBits(b, mbx, mby, double(b.bw.bitCount()));
        }
        MbOut a;
        bool deltaA = false;
        if (est16 != INT_MAX && est16 <= lim) {
            deltaA = encodeLuma16(a, mbx, mby, ch);
            costA = ssd() + lambda * mbBits(a, mbx, mby, double(a.bw.bitCount()));
        }
        if (costA >= 1e300 && costB >= 1e300 && costC >= 1e300) {
            tIntraNone = true; // all given up: the macroblock state is left half written
            return false;
        }
        // Same order of preference as below.
        if (costC < costA && costC < costB) {
            restore(snapC);
            out = std::move(c);
            picked = 2;
            pickedCost = costC;
            return deltaC;
        }
        if (costB <= costA) {
            restore(snapB);
            out = std::move(b);
            picked = 1;
            pickedCost = costB;
            return deltaB;
        }
        out = std::move(a); // state is the Intra 16x16 one (coded last)
        picked = 0;
        pickedCost = costA;
        return deltaA;
    }
    MbOut a, b;
    bool deltaA = false;
    double costA = 1e300;
    if (!tIntraSkip16) {
        deltaA = encodeLuma16(a, mbx, mby, ch);
        costA = ssd() + lambda * mbBits(a, mbx, mby, double(a.bw.bitCount()));
    }
    Snapshot snapA;
    save(snapA);
    const bool deltaB = encodeLuma4(b, mbx, mby, ch);
    const double costB = ssd() + lambda * mbBits(b, mbx, mby, double(b.bw.bitCount()));
    if (useT8() && !tIntraSkip8) {
        Snapshot snapB;
        save(snapB);
        MbOut c;
        const bool deltaC = encodeLuma8(c, mbx, mby, ch);
        const double costC = ssd() + lambda * mbBits(c, mbx, mby, 0.0);
        if (costC < costA && costC < costB) {
            out = std::move(c);
            picked = 2;
            pickedCost = costC;
            return deltaC;
        }
        if (costB <= costA) {
            restore(snapB);
            out = std::move(b);
            picked = 1;
            pickedCost = costB;
            return deltaB;
        }
    }
    if (costA <= costB) {
        restore(snapA);
        out = std::move(a);
        picked = 0;
        pickedCost = costA;
        return deltaA;
    }
    out = std::move(b);
    picked = 1;
    pickedCost = costB;
    return deltaB;
    };
    bool delta = pick();
    // Psy: the chosen type once more with the quantisation rounding to nearest (see gPsyIntraRound).
    if (tPsy.weight > 0 && gPsyIntraRound > 0 && picked >= 0) {
        Snapshot keep;
        save(keep);
        MbOut fine;
        struct FineScope { // not left set for the thread's next macroblock if the coding throws
            FineScope() { tIntraFine = true; }
            ~FineScope() { tIntraFine = false; }
        };
        bool fineDelta;
        {
            const FineScope scope;
            fineDelta = picked == 0 ? encodeLuma16(fine, mbx, mby, ch) : picked == 1 ? encodeLuma4(fine, mbx, mby, ch)
                                                                                  : encodeLuma8(fine, mbx, mby, ch);
        }
        const double costFine = ssd() + lambda * mbBits(fine, mbx, mby, picked == 2 ? 0.0 : double(fine.bw.bitCount()));
        if (costFine < pickedCost) {
            out = std::move(fine);
            delta = fineDelta;
        } else {
            restore(keep);
        }
    }
    return delta;
}


namespace {

// coded_block_pattern codeNum -> cbp for inter macroblocks (table 9-4, 4:2:0).
constexpr int kInterCbpFromCode[48] = {0,  16, 1,  2,  4,  8,  32, 3,  5,  10, 12, 15, 47, 7,  11, 13,
                                       14, 6,  9,  31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
                                       17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41};

int interCbpCode(int cbp)
{
    static const auto table = [] {
        std::array<int, 48> t{};
        for (int code = 0; code < 48; ++code)
            t[size_t(kInterCbpFromCode[code])] = code;
        return t;
    }();
    return table[size_t(cbp)];
}

// Bits of se(v): 2 * floor(log2(codeNum + 1)) + 1.
inline int seBitsCalc(int v)
{
    const uint32_t k = v > 0 ? uint32_t(2 * v - 1) : uint32_t(-2 * v);
#if defined(__GNUC__) || defined(__clang__)
    return 2 * (31 - __builtin_clz(k + 1)) + 1;
#else
    int len = 0;
    while (((k + 1) >> len) > 1)
        ++len;
    return 2 * len + 1;
#endif
}

// The same from a table for the vector differences the searches see (|v| < 1024).
struct SeBitsTable {
    uint8_t bits[2048];
    SeBitsTable()
    {
        for (int v = -1024; v < 1024; ++v)
            bits[v + 1024] = uint8_t(seBitsCalc(v));
    }
};
const SeBitsTable kSeBits;
inline int seBits(int v)
{
    return unsigned(v + 1024) < 2048u ? kSeBits.bits[v + 1024] : seBitsCalc(v);
}

// ---- 8x8 transform (High profile), 8.5.11.
// 8x8 frame zig-zag (table 8-14): scan index -> raster index (row * 8 + column).
constexpr int kZigzag8[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                              41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                              30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
// normAdjust8x8 (8-317) by QP % 6 and position class.
constexpr int kDequantV8[6][6] = {{20, 18, 32, 19, 25, 24}, {22, 19, 35, 21, 28, 26}, {26, 23, 42, 24, 33, 31},
                                  {28, 25, 45, 26, 35, 33}, {32, 28, 51, 30, 40, 38}, {36, 32, 58, 34, 46, 43}};
constexpr int posClass8(int raster)
{
    const int i = raster >> 3, j = raster & 7;
    if (i % 4 == 0 && j % 4 == 0)
        return 0;
    if (i % 2 == 1 && j % 2 == 1)
        return 1;
    if (i % 4 == 2 && j % 4 == 2)
        return 2;
    if ((i % 4 == 0 && j % 2 == 1) || (i % 2 == 1 && j % 4 == 0))
        return 3;
    if ((i % 4 == 0 && j % 4 == 2) || (i % 4 == 2 && j % 4 == 0))
        return 4;
    return 5;
}

// One-dimensional inverse 8x8 transform (8-361..8-384), integer exact or real-valued.
template <typename T>
void inverse8Line(const T* d, T* g, bool exact)
{
    auto half = [&](T v) { return exact ? T(int64_t(v) >> 1) : v / 2; };
    auto quarter = [&](T v) { return exact ? T(int64_t(v) >> 2) : v / 4; };
    const T e0 = d[0] + d[4], e1 = -d[3] + d[5] - d[7] - half(d[7]), e2 = d[0] - d[4];
    const T e3 = d[1] + d[7] - d[3] - half(d[3]), e4 = half(d[2]) - d[6], e5 = -d[1] + d[7] + d[5] + half(d[5]);
    const T e6 = d[2] + half(d[6]), e7 = d[3] + d[5] + d[1] + half(d[1]);
    const T f0 = e0 + e6, f1 = e1 + quarter(e7), f2 = e2 + e4, f3 = e3 + quarter(e5);
    const T f4 = e2 - e4, f5 = quarter(e3) - e5, f6 = e0 - e6, f7 = e7 - quarter(e1);
    g[0] = f0 + f7;
    g[1] = f2 + f5;
    g[2] = f4 + f3;
    g[3] = f6 + f1;
    g[4] = f6 - f1;
    g[5] = f4 - f3;
    g[6] = f2 - f5;
    g[7] = f0 - f7;
}

// Real-valued basis of the inverse (basis[k][x] = output sample x for a unit coefficient k)
// and its squared norms; the encoder's forward transform is its transpose.
struct Basis8 {
    double b[8][8];
    double norm[8];
    Basis8()
    {
        for (int k = 0; k < 8; ++k) {
            double d[8] = {}, g[8];
            d[k] = 1;
            inverse8Line<double>(d, g, false);
            norm[k] = 0;
            for (int x = 0; x < 8; ++x) {
                b[k][x] = g[x];
                norm[k] += g[x] * g[x];
            }
        }
    }
};
const Basis8& basis8()
{
    static const Basis8 b;
    return b;
}

// Quantises an 8x8 residual (raster) for QP; deadzone offset as a fraction of a step.
// The forward transform is the transpose of the inverse's real-valued basis; its entries are
// multiples of 1/8, so 8 * basis is integer and the transform runs exactly in integers.
void quantize8x8(const int* residual, int qp, double offset, int* levels, double* real = nullptr)
{
    // Decoder: r = B^T (Z * v8 * 2^(qp/6 - 2)) B / 64, so Z = 256 * W / (n_i n_j v8 2^(qp/6)) with W = w64 / 64,
    // i.e. Z = 4 * w64 / den: both multiplications by powers of two are exact, so this is the same double.
    static const auto kScale = [] { // n_i n_j v8 by QP % 6 and position
        std::array<std::array<double, 64>, 6> t{};
        const Basis8& B = basis8();
        for (int q = 0; q < 6; ++q)
            for (int k = 0; k < 64; ++k)
                t[size_t(q)][size_t(k)] = B.norm[k >> 3] * B.norm[k & 7] * kDequantV8[q][posClass8(k)];
        return t;
    }();
    int w64[64];
    forward8x8(residual, w64);
    const double step2 = double(1 << (qp / 6));
    const double* scale = kScale[size_t(qp % 6)].data();
    if (!levels) { // the trellis takes the real-valued levels only
        for (int k = 0; k < 64; ++k)
            real[k] = 4.0 * double(w64[k]) / (scale[k] * step2);
        return;
    }
    for (int k = 0; k < 64; ++k) {
        const double x = 4.0 * double(w64[k]) / (scale[k] * step2);
        const int mag = std::min(int(std::abs(x) + offset), kMaxLevel);
        levels[k] = x < 0 ? -mag : mag;
        if (real)
            real[k] = x;
    }
}

// The real-valued levels of quantize8x8 (trellis path) straight in zigzag scan order: the same
// doubles (same operations per coefficient), without the copy into scan order afterwards.
void quantize8x8Scan(const int* residual, int qp, double* xs)
{
    static const auto kScaleScan = [] { // n_i n_j v8 by QP % 6 and scan position
        std::array<std::array<double, 64>, 6> t{};
        const Basis8& B = basis8();
        for (int q = 0; q < 6; ++q)
            for (int s = 0; s < 64; ++s) {
                const int k = kZigzag8[s];
                t[size_t(q)][size_t(s)] = B.norm[k >> 3] * B.norm[k & 7] * kDequantV8[q][posClass8(k)];
            }
        return t;
    }();
    int w64[64];
    forward8x8(residual, w64);
    const double step2 = double(1 << (qp / 6));
    const double* scale = kScaleScan[size_t(qp % 6)].data();
    int s = 0;
#ifdef SCHNEIDI_H264_SSE2
    const __m128d four = _mm_set1_pd(4.0), st = _mm_set1_pd(step2);
    for (; s < 64; s += 2) {
        const __m128d w = _mm_cvtepi32_pd(_mm_setr_epi32(w64[kZigzag8[s]], w64[kZigzag8[s + 1]], 0, 0));
        _mm_storeu_pd(xs + s, _mm_div_pd(_mm_mul_pd(four, w), _mm_mul_pd(_mm_loadu_pd(scale + s), st)));
    }
#elif defined(SCHNEIDI_H264_NEON)
    const float64x2_t st = vdupq_n_f64(step2);
    for (; s < 64; s += 2) {
        const int32x2_t wi = {w64[kZigzag8[s]], w64[kZigzag8[s + 1]]};
        const float64x2_t w = vcvtq_f64_s64(vmovl_s32(wi));
        vst1q_f64(xs + s, vdivq_f64(vmulq_n_f64(w, 4.0), vmulq_f64(vld1q_f64(scale + s), st)));
    }
#endif
    for (; s < 64; ++s)
        xs[s] = 4.0 * double(w64[kZigzag8[s]]) / (scale[s] * step2);
}

void dequant8x8(const int* levels, int qp, int* d)
{
    static const auto kLs = [] { // 16 * normAdjust8x8 by QP % 6 and position
        std::array<std::array<int, 64>, 6> t{};
        for (int q = 0; q < 6; ++q)
            for (int k = 0; k < 64; ++k)
                t[size_t(q)][size_t(k)] = 16 * kDequantV8[q][posClass8(k)];
        return t;
    }();
    const int qpDiv = qp / 6;
    const int* ls = kLs[size_t(qp % 6)].data();
    if (qpDiv >= 6) {
        for (int k = 0; k < 64; ++k)
            d[k] = (levels[k] * ls[k]) << (qpDiv - 6);
    } else {
        const int sh = 6 - qpDiv, rnd = 1 << (5 - qpDiv);
        for (int k = 0; k < 64; ++k)
            d[k] = (levels[k] * ls[k] + rnd) >> sh;
    }
}

int median3(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

} // namespace

// Intra 8x8 (High profile, CABAC only): four 8x8 blocks, each with its own prediction direction
// from filtered neighbour samples and an 8x8 transform.
bool FrameCoder::encodeLuma8(MbOut& o, int mbx, int mby, const ChromaMb& ch, int bound, int* satdOut)
{
    const int qp = mbQp_[size_t(mby) * mbW_ + mbx];
    const int lw = mbW_ * 4;
    const int lambda = std::max(1, int(0.9 * std::pow(2.0, (qp - 12) / 6.0) + 0.5));
    int cbpLuma = 0;
    int satdSum = 0; // chosen modes: SATD + lambda * mode bits
    if (satdOut)
        *satdOut = -1; // aborted until the end
    for (int b8 = 0; b8 < 4; ++b8) {
        const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
        const int px = mbx * 16 + ox, py = mby * 16 + oy;
        const int bx = px / 4, by = py / 4;
        const bool left = px > 0, top = oy > 0 || topMbAvailable(mby);
        const bool corner = left && top;
        const bool topRight = b8 == 0 ? top : b8 == 1 ? top && mbx + 1 < mbW_ : b8 == 2;

        int T[17], L[8];
        std::fill(T, T + 17, 128);
        std::fill(L, L + 8, 128);
        if (top) {
            const uint8_t* above = &recY_[size_t(py - 1) * padW_ + px];
            for (int i = 0; i < 8; ++i)
                T[1 + i] = above[i];
            for (int i = 8; i < 16; ++i)
                T[1 + i] = topRight ? above[i] : above[7];
        }
        if (left)
            for (int i = 0; i < 8; ++i)
                L[i] = recY_[size_t(py + i) * padW_ + px - 1];
        if (corner)
            T[0] = recY_[size_t(py - 1) * padW_ + px - 1];
        filterIntra8Refs(T, L, left, top, corner);

        int predMode = 2;
        if (left && top) {
            const int a = modes4x4_[size_t(by) * lw + bx - 1];
            const int b = modes4x4_[size_t(by - 1) * lw + bx];
            predMode = std::min(a < 0 ? 2 : a, b < 0 ? 2 : b);
        }

        uint8_t pred[64], cand[64];
        const IntraDirTable<8>& dir = intraDir8();
        uint8_t edge[80] = {}; // IntraDirTable<8>::kG values, [74] = DC for the shuffle path
        static_assert(IntraDirTable<8>::kG <= 74);
        if (dir.ok)
            IntraDirTable<8>::edges(T, L, edge);
        int bestMode = 2, bestCost = 1 << 30;
        auto tryMode = [&](int mode) {
            const bool needsTop = mode == 0 || mode == 3 || mode == 7;
            const bool needsLeft = mode == 1 || mode == 8;
            const bool needsBoth = mode >= 4 && mode <= 6;
            if ((needsTop && !top) || (needsLeft && !left) || (needsBoth && !corner))
                return 1 << 30;
            if (mode != 2 && dir.ok)
                dir.predict(mode, edge, cand);
            else
                predict8x8(mode, T, L, left, top, cand);
            const int cost = lambda * (mode == predMode ? 1 : 4) + satd8x8U8Fast(&srcY_[size_t(py) * padW_ + px], padW_, cand, 8);
            if (cost < bestCost) {
                bestCost = cost;
                bestMode = mode;
                std::memcpy(pred, cand, 64);
            }
            return cost;
        };
#if defined(SCHNEIDI_H264_AVX2) || defined(SCHNEIDI_H264_NEON)
        if (!(gExp & 64) && dir.ok && corner && haveIntraAll()) {
            // All nine directions at once (same costs, same first-lowest pick as below).
            int st = 0, sl = 0;
            for (int i = 0; i < 8; ++i) {
                st += T[1 + i];
                sl += L[i];
            }
            edge[74] = uint8_t((st + sl + 8) >> 4);
            alignas(32) uint8_t all[9][64];
            intra8PredictAll(edge, all);
            const uint8_t* src8 = &srcY_[size_t(py) * padW_ + px];
            for (int mode = 0; mode < 9; ++mode) {
                const int cost = lambda * (mode == predMode ? 1 : 4) + satd8x8U8Fast(src8, padW_, all[mode], 8);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestMode = mode;
                }
            }
            std::memcpy(pred, all[bestMode], 64);
        } else
#endif
            intraModeSearch(tryMode, predMode);
        satdSum += bestCost;
        if (satdSum > bound)
            return false; // clearly worse than another intra type: the caller drops it

        int res[64], levels[64];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                res[y * 8 + x] = int(srcY_[size_t(py + y) * padW_ + px + x]) - int(pred[y * 8 + x]);
        quantize8x8(res, qp, tIntraFine ? gPsyIntraRound : 1.0 / 3.0, levels);
        int count = 0;
        for (int k = 0; k < 64; ++k) {
            count += levels[k] != 0;
            o.syn.luma8[b8][k] = levels[kZigzag8[k]];
        }
        if (count)
            cbpLuma |= 1 << b8;
        int d[64], r[64];
        dequant8x8(levels, qp, d);
        inverse8x8(d, r);
        addClip8x8(pred, 8, r, &recY_[size_t(py) * padW_ + px], padW_);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) {
                modes4x4_[size_t(by + y) * lw + bx + x] = int8_t(bestMode);
                nzLuma_[size_t(by + y) * lw + bx + x] = uint8_t(count);
            }
        o.syn.i4Mode[b8] = bestMode;
        o.syn.i4Pred[b8] = predMode;
    }
    if (satdOut)
        *satdOut = satdSum;
    o.syn.kind = MbSyntax::Kind::I4;
    o.syn.t8 = true;
    o.syn.chromaMode = ch.mode;
    o.syn.cbpLuma = cbpLuma;
    const int cbp = cbpLuma | (ch.cbp << 4);
    o.syn.hasQpDelta = cbp != 0;
    o.syn.qpDelta = qp - qpPredOf(mby);
    writeChroma(o, mbx, mby, ch);
    return cbp != 0;
}

struct FrameCoder::MbState {
    uint8_t y[256];
    uint8_t c[2][64];
    uint8_t nz[16];
    int8_t modes[16];
    uint8_t nzc[2][4];
    int16_t mvx[16], mvy[16];
    int8_t ref[16];
    int16_t mvx1[16], mvy1[16]; // list 1 (B pictures only)
    int8_t ref1[16];
};

void FrameCoder::saveMb(int mbx, int mby, MbState& s) const
{
    const size_t lo = size_t(mby * 16) * padW_ + mbx * 16;
    for (int y = 0; y < 16; ++y)
        std::memcpy(s.y + y * 16, &recY_[lo + size_t(y) * padW_], 16);
    const int cw = padW_ / 2;
    const size_t co = size_t(mby * 8) * cw + mbx * 8;
    for (int y = 0; y < 8; ++y) {
        std::memcpy(s.c[0] + y * 8, &recU_[co + size_t(y) * cw], 8);
        std::memcpy(s.c[1] + y * 8, &recV_[co + size_t(y) * cw], 8);
    }
    const int lw = mbW_ * 4, chw = mbW_ * 2;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
            s.nz[y * 4 + x] = nzLuma_[o];
            s.modes[y * 4 + x] = modes4x4_[o];
            s.mvx[y * 4 + x] = mvX_[o];
            s.mvy[y * 4 + x] = mvY_[o];
            s.ref[y * 4 + x] = refIdx_[o];
            if (bSlice_) {
                s.mvx1[y * 4 + x] = mvX1_[o];
                s.mvy1[y * 4 + x] = mvY1_[o];
                s.ref1[y * 4 + x] = refIdx1_[o];
            }
        }
    for (int c = 0; c < 2; ++c)
        for (int b = 0; b < 4; ++b)
            s.nzc[c][b] = nzChroma_[c][size_t(mby * 2 + (b >> 1)) * chw + mbx * 2 + (b & 1)];
}

void FrameCoder::restoreMb(int mbx, int mby, const MbState& s)
{
    const size_t lo = size_t(mby * 16) * padW_ + mbx * 16;
    for (int y = 0; y < 16; ++y)
        std::memcpy(&recY_[lo + size_t(y) * padW_], s.y + y * 16, 16);
    const int cw = padW_ / 2;
    const size_t co = size_t(mby * 8) * cw + mbx * 8;
    for (int y = 0; y < 8; ++y) {
        std::memcpy(&recU_[co + size_t(y) * cw], s.c[0] + y * 8, 8);
        std::memcpy(&recV_[co + size_t(y) * cw], s.c[1] + y * 8, 8);
    }
    const int lw = mbW_ * 4, chw = mbW_ * 2;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
            nzLuma_[o] = s.nz[y * 4 + x];
            modes4x4_[o] = s.modes[y * 4 + x];
            mvX_[o] = s.mvx[y * 4 + x];
            mvY_[o] = s.mvy[y * 4 + x];
            refIdx_[o] = s.ref[y * 4 + x];
            if (bSlice_) {
                mvX1_[o] = s.mvx1[y * 4 + x];
                mvY1_[o] = s.mvy1[y * 4 + x];
                refIdx1_[o] = s.ref1[y * 4 + x];
            }
        }
    for (int c = 0; c < 2; ++c)
        for (int b = 0; b < 4; ++b)
            nzChroma_[c][size_t(mby * 2 + (b >> 1)) * chw + mbx * 2 + (b & 1)] = s.nzc[c][b];
}

int64_t FrameCoder::mbSsd(int mbx, int mby) const
{
    const size_t lo = size_t(mby * 16) * padW_ + mbx * 16;
    const int cw = padW_ / 2;
    const size_t co = size_t(mby * 8) * cw + mbx * 8;
#ifdef SCHNEIDI_H264_SSE2
    // At most 256 * 255^2 + 128 * 255^2: fits 32 bits.
    const __m128i zero = _mm_setzero_si128();
    __m128i acc = zero;
    auto add8 = [&](__m128i a, __m128i b) { // 8 samples, zero-extended to 16 bits
        const __m128i d = _mm_sub_epi16(a, b);
        acc = _mm_add_epi32(acc, _mm_madd_epi16(d, d));
    };
    for (int y = 0; y < 16; ++y) {
        const size_t o = lo + size_t(y) * padW_;
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&srcY_[o]));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&recY_[o]));
        add8(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero));
        add8(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero));
    }
    const __m128i lumaAcc = acc;
    acc = zero;
    for (int y = 0; y < 8; ++y) {
        const size_t o = co + size_t(y) * cw;
        add8(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&srcU_[o])), zero),
             _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&recU_[o])), zero));
        add8(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&srcV_[o])), zero),
             _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&recV_[o])), zero));
    }
    auto total = [](__m128i v) {
        v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
        v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
        return int64_t(uint32_t(_mm_cvtsi128_si32(v)));
    };
    return total(lumaAcc) + int64_t(cfg_.chromaRdWeight) * total(acc);
#elif defined(SCHNEIDI_H264_NEON)
    uint32x4_t acc = vdupq_n_u32(0);
    auto add8 = [&](uint8x8_t a, uint8x8_t b) {
        const uint8x8_t d = vabd_u8(a, b);
        acc = vpadalq_u16(acc, vmull_u8(d, d));
    };
    for (int y = 0; y < 16; ++y) {
        const size_t o = lo + size_t(y) * padW_;
        const uint8x16_t a = vld1q_u8(&srcY_[o]), b = vld1q_u8(&recY_[o]);
        add8(vget_low_u8(a), vget_low_u8(b));
        add8(vget_high_u8(a), vget_high_u8(b));
    }
    const int64_t lumaSum = int64_t(vaddvq_u32(acc));
    acc = vdupq_n_u32(0);
    for (int y = 0; y < 8; ++y) {
        const size_t o = co + size_t(y) * cw;
        add8(vld1_u8(&srcU_[o]), vld1_u8(&recU_[o]));
        add8(vld1_u8(&srcV_[o]), vld1_u8(&recV_[o]));
    }
    return lumaSum + int64_t(cfg_.chromaRdWeight) * int64_t(vaddvq_u32(acc));
#else
    int64_t s = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const size_t o = lo + size_t(y) * padW_ + x;
            const int d = int(srcY_[o]) - int(recY_[o]);
            s += d * d;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const size_t o = co + size_t(y) * cw + x;
            const int du = int(srcU_[o]) - int(recU_[o]);
            const int dv = int(srcV_[o]) - int(recV_[o]);
            s += int64_t(cfg_.chromaRdWeight) * (du * du + dv * dv);
        }
    return s;
#endif
}

// Psycho-visual RD. The distance (hadamardMagsDistance) is the mean of the two scales, summed over
// the four 8x8 blocks, and weighs psyRd * sqrt(lambda) per unit: like a SATD against the mode
// decision's SSD. Comparing the totals of the magnitudes instead (the blocks' AC energy) keeps
// more energy for the same strength, but at high QP it is met by any pattern: one strong
// coefficient per block in place of fine grain shows as a regular mesh on dark, flat areas.
void FrameCoder::psyBegin(int mbx, int mby) const
{
    if (!(cfg_.psyRd > 0)) {
        tPsy.weight = 0;
        return;
    }
    static const auto kSqrtLambda = [] {
        std::array<double, 52> t{};
        for (int q = 0; q < 52; ++q)
            t[size_t(q)] = std::sqrt(0.85 * std::pow(2.0, (q - 12) / 3.0));
        return t;
    }();
    tPsy.weight = cfg_.psyRd * 0.5 * kSqrtLambda[size_t(mbQp_[size_t(mby) * mbW_ + mbx])] *
                  textureShare(mbEnergy_[size_t(mby) * mbW_ + mbx]);
    if (!(tPsy.weight > 0))
        return;
    const uint8_t* src = &srcY_[size_t(mby * 16) * padW_ + mbx * 16];
    for (int b = 0; b < 4; ++b)
        hadamardMags8x8U8(src + size_t((b >> 1) * 8) * padW_ + (b & 1) * 8, padW_, tPsy.src[b]);
}

double FrameCoder::psyCost(int mbx, int mby) const
{
    if (!(tPsy.weight > 0))
        return 0.0;
    const uint8_t* rec = &recY_[size_t(mby * 16) * padW_ + mbx * 16];
    int distance = 0;
    for (int b = 0; b < 4; ++b) {
        alignas(16) int16_t mags[128];
        hadamardMags8x8U8(rec + size_t((b >> 1) * 8) * padW_ + (b & 1) * 8, padW_, mags);
        distance += hadamardMagsDistance(tPsy.src[b], mags);
    }
    return tPsy.weight * distance;
}

// Neighbouring 4x4 block (x4, y4 relative to the macroblock, may be -1 or 4) for motion
// vector prediction (8.4.1.3.2). `decoded` marks blocks of the current macroblock that
// already have their vector.
FrameCoder::MvNb FrameCoder::mvNeighbour(int mbx, int mby, int x4, int y4, const bool* decoded, int list) const
{
    MvNb n;
    bool avail;
    if (y4 < 0) {
        const bool top = topMbAvailable(mby);
        avail = top && (x4 < 0 ? mbx > 0 : x4 >= 4 ? mbx + 1 < mbW_ : true);
    } else if (x4 < 0) {
        avail = mbx > 0;
    } else if (x4 >= 4) {
        avail = false; // the macroblock to the right is not coded yet
    } else {
        avail = decoded[y4 * 4 + x4];
    }
    n.avail = avail;
    if (!avail)
        return n;
    const size_t o = size_t(mby * 4 + y4) * (mbW_ * 4) + size_t(mbx * 4 + x4);
    if (list) {
        n.ref = refIdx1_[o];
        if (n.ref >= 0) {
            n.mvx = mvX1_[o];
            n.mvy = mvY1_[o];
        }
        return n;
    }
    n.ref = refIdx_[o];
    if (n.ref >= 0) {
        n.mvx = mvX_[o];
        n.mvy = mvY_[o];
    }
    return n;
}

// Motion vector prediction (8.4.1.3) for a partition at (x4, y4) of w4 x h4 blocks.
// shape: 0 = median, 1/2 = 16x8 top/bottom, 3/4 = 8x16 left/right.
void FrameCoder::predictMvPart(int mbx, int mby, int x4, int y4, int w4, int shape, int refIdx, const bool* decoded,
                            int& mvx, int& mvy, int list) const
{
    MvNb a = mvNeighbour(mbx, mby, x4 - 1, y4, decoded, list);
    MvNb b = mvNeighbour(mbx, mby, x4, y4 - 1, decoded, list);
    MvNb c = mvNeighbour(mbx, mby, x4 + w4, y4 - 1, decoded, list);
    if (!c.avail)
        c = mvNeighbour(mbx, mby, x4 - 1, y4 - 1, decoded, list); // D replaces C
    const MvNb* directional = shape == 1 ? &b : shape == 2 ? &a : shape == 3 ? &a : shape == 4 ? &c : nullptr;
    if (directional && directional->ref == refIdx) {
        mvx = directional->mvx;
        mvy = directional->mvy;
        return;
    }
    if (!b.avail && !c.avail && a.avail)
        b = c = a;
    const int matches = (a.ref == refIdx) + (b.ref == refIdx) + (c.ref == refIdx);
    if (matches == 1) {
        const MvNb& only = a.ref == refIdx ? a : b.ref == refIdx ? b : c;
        mvx = only.mvx;
        mvy = only.mvy;
        return;
    }
    mvx = median3(a.mvx, b.mvx, c.mvx);
    mvy = median3(a.mvy, b.mvy, c.mvy);
}

// Motion compensation of one partition into macroblock-sized prediction buffers.
void FrameCoder::motionCompensate(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy,
                                  uint8_t* luma, uint8_t (*chroma)[64]) const
{
    motionCompensatePlain(R, mbx, mby, x4, y4, w4, h4, mvx, mvy, luma, chroma); // luma: weighted by mcLuma
    const WeightParams* wp = weightsOf(R);
    if (!wp || !wp->chroma)
        return;
    const int round = wpChromaLog_ >= 1 ? 1 << (wpChromaLog_ - 1) : 0;
    for (int c = 0; c < 2; ++c) {
        const int w = wp->chromaWeight[c], o = wp->chromaOffset[c];
        for (int y = y4 * 2; y < (y4 + h4) * 2; ++y) {
            uint8_t* row = chroma[c] + y * 8;
            for (int x = x4 * 2; x < (x4 + w4) * 2; ++x)
                row[x] = uint8_t(std::clamp(((row[x] * w + round) >> wpChromaLog_) + o, 0, 255));
        }
    }
}

void FrameCoder::motionCompensatePlain(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy,
                                       uint8_t* luma, uint8_t (*chroma)[64]) const
{
    mcLuma(R, mbx * 16 + x4 * 4 + (mvx >> 2), mby * 16 + y4 * 4 + (mvy >> 2), mvx & 3, mvy & 3, w4 * 4, h4 * 4,
           luma + y4 * 4 * 16 + x4 * 4);
    // Chroma: eighth-pel bilinear (8.4.2.2.2).
    const int cw = padW_ / 2, chh = padH_ / 2;
    const int xF = mvx & 7, yF = mvy & 7;
    const std::vector<uint8_t>* planes[2] = {&R.u, &R.v};
    const int wA = (8 - xF) * (8 - yF), wB = xF * (8 - yF), wC = (8 - xF) * yF, wD = xF * yF;
    const int bx0 = mbx * 8 + x4 * 2 + (mvx >> 3), by0 = mby * 8 + y4 * 2 + (mvy >> 3);
    const int bw = w4 * 2, bh = h4 * 2;
    if (bx0 >= 0 && by0 >= 0 && bx0 + bw < cw && by0 + bh < chh) { // inside: no edge clamping
        for (int c = 0; c < 2; ++c) {
            const uint8_t* p = planes[c]->data();
            for (int y = 0; y < bh; ++y) {
                const uint8_t* r0 = p + size_t(by0 + y) * cw + bx0;
                const uint8_t* r1 = r0 + cw;
                uint8_t* o = chroma[c] + (y4 * 2 + y) * 8 + x4 * 2;
#ifdef SCHNEIDI_H264_SSE2
                // Same sums in 16-bit lanes (at most 64 * 255 + 32); r0[bw] is inside the plane.
                if (bw == 8 || bw == 4) {
                    const __m128i zero = _mm_setzero_si128();
                    auto ld = [&](const uint8_t* q) {
                        if (bw == 8)
                            return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(q)), zero);
                        int32_t v;
                        std::memcpy(&v, q, 4);
                        return _mm_unpacklo_epi8(_mm_cvtsi32_si128(v), zero);
                    };
                    const __m128i sum = _mm_add_epi16(
                        _mm_add_epi16(_mm_mullo_epi16(ld(r0), _mm_set1_epi16(int16_t(wA))),
                                      _mm_mullo_epi16(ld(r0 + 1), _mm_set1_epi16(int16_t(wB)))),
                        _mm_add_epi16(_mm_mullo_epi16(ld(r1), _mm_set1_epi16(int16_t(wC))),
                                      _mm_add_epi16(_mm_mullo_epi16(ld(r1 + 1), _mm_set1_epi16(int16_t(wD))), _mm_set1_epi16(32))));
                    const __m128i px = _mm_packus_epi16(_mm_srli_epi16(sum, 6), zero);
                    if (bw == 8) {
                        _mm_storel_epi64(reinterpret_cast<__m128i*>(o), px);
                    } else {
                        const int32_t v = _mm_cvtsi128_si32(px);
                        std::memcpy(o, &v, 4);
                    }
                    continue;
                }
#elif defined(SCHNEIDI_H264_NEON)
                // At most 64 * 255: 16 bits; r0[bw] is inside the plane (bw = 4: plain loop).
                if (bw == 8) {
                    uint16x8_t sum = vmull_u8(vld1_u8(r0), vdup_n_u8(uint8_t(wA)));
                    sum = vmlal_u8(sum, vld1_u8(r0 + 1), vdup_n_u8(uint8_t(wB)));
                    sum = vmlal_u8(sum, vld1_u8(r1), vdup_n_u8(uint8_t(wC)));
                    sum = vmlal_u8(sum, vld1_u8(r1 + 1), vdup_n_u8(uint8_t(wD)));
                    vst1_u8(o, vrshrn_n_u16(sum, 6));
                    continue;
                }
#endif
                for (int x = 0; x < bw; ++x)
                    o[x] = uint8_t((wA * r0[x] + wB * r0[x + 1] + wC * r1[x] + wD * r1[x + 1] + 32) >> 6);
            }
        }
        return;
    }
    for (int c = 0; c < 2; ++c) {
        const std::vector<uint8_t>& p = *planes[c];
        for (int y = y4 * 2; y < (y4 + h4) * 2; ++y) {
            const int yi = mby * 8 + y + (mvy >> 3);
            const int y0 = std::clamp(yi, 0, chh - 1), y1 = std::clamp(yi + 1, 0, chh - 1);
            for (int x = x4 * 2; x < (x4 + w4) * 2; ++x) {
                const int xi = mbx * 8 + x + (mvx >> 3);
                const int x0 = std::clamp(xi, 0, cw - 1), x1 = std::clamp(xi + 1, 0, cw - 1);
                const int A = p[size_t(y0) * cw + x0], B = p[size_t(y0) * cw + x1];
                const int C = p[size_t(y1) * cw + x0], D = p[size_t(y1) * cw + x1];
                chroma[c][y * 8 + x] = uint8_t(((8 - xF) * (8 - yF) * A + xF * (8 - yF) * B + (8 - xF) * yF * C +
                                                xF * yF * D + 32) >> 6);
            }
        }
    }
}

// Greedy rate-distortion quantisation of one 4x4 block (raster levels, 16 coefficients):
// from high to low frequency, lower each level by one step while SSD + lambda * bits drops.
void FrameCoder::rdoQuantize(int* levels, const int* residual, int qp, double lambda, int nC) const
{
    const int qpDiv = qp / 6, qpMod = qp % 6;
    auto evaluate = [&](const int* lv) {
        int d[16], r[16];
        for (int i = 0; i < 16; ++i)
            d[i] = (lv[i] * kDequantV[qpMod][posClass(i)]) << qpDiv;
        inverse4x4(d, r);
        int64_t ssd = 0;
        for (int i = 0; i < 16; ++i)
            ssd += int64_t(residual[i] - r[i]) * (residual[i] - r[i]);
        int scan[16];
        for (int k = 0; k < 16; ++k)
            scan[k] = lv[kZigzag[k]];
        BitWriter bits;
        writeResidualBlock(bits, scan, 16, nC);
        return double(ssd) + lambda * double(bits.bitCount());
    };
    double best = evaluate(levels);
    for (int k = 15; k >= 0; --k) {
        const int i = kZigzag[k];
        while (levels[i] != 0) {
            const int old = levels[i];
            levels[i] = old > 0 ? old - 1 : old + 1;
            const double cost = evaluate(levels);
            if (cost < best) {
                best = cost;
            } else {
                levels[i] = old;
                break;
            }
        }
    }
}

// One sub-pel step around (cx0, cy0) without measuring all eight neighbours: the four direct ones
// first, then of the diagonals only those next to them that can still win. Pattern 2: both
// diagonals beside the best direct neighbour (6 instead of 8 SATDs); pattern 1: only the diagonal
// between the better horizontal and the better vertical neighbour (5); 0: all eight (before 14.11.).
// H264_SUBPEL = two digits, half-pel and quarter-pel step. Measured (two-pass, 4 rates, BD-rate
// PSNR-Y / SSIM-Y on CS2, camera, calm, animation; instructions two-pass 8 Mbit/s CS2): 22 +0.18 /
// +0.14, +0.12 / +0.12, +0.10 / -0.04, +0.19 / +0.56 % for -3.2 %; 11 +0.62 / +0.47, +0.57 / +0.43,
// +0.05 / -0.20, +0.74 / +1.45 % for -4.9 % (too dear); 21 against 22 +0.31, +0.42, +0.08, +0.05 %
// for -0.8 %.
static const int gSubpelBoth = std::getenv("H264_SUBPEL") ? std::atoi(std::getenv("H264_SUBPEL")) : 22;
static const int gSubpelHalf = gSubpelBoth / 10, gSubpelQuarter = gSubpelBoth % 10;

template <typename Cost>
static inline void subpelStep(Cost&& cost, int cx0, int cy0, int step, int range, double& best, int& bx, int& by)
{
    const int pattern = step == 2 ? gSubpelHalf : gSubpelQuarter;
    double c[4]; // left, right, up, down
    static constexpr int kD[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        const int cx = cx0 + kD[i][0] * step, cy = cy0 + kD[i][1] * step;
        c[i] = std::abs(cx) > range || std::abs(cy) > range ? 1e300 : cost(cx, cy);
        if (c[i] < best) {
            best = c[i];
            bx = cx;
            by = cy;
        }
    }
    const int h = c[1] < c[0] ? 1 : -1, v = c[3] < c[2] ? 1 : -1;
    auto diag = [&](int dx, int dy) {
        const int cx = cx0 + dx * step, cy = cy0 + dy * step;
        if (std::abs(cx) > range || std::abs(cy) > range)
            return;
        const double d = cost(cx, cy);
        if (d < best) {
            best = d;
            bx = cx;
            by = cy;
        }
    };
    diag(h, v);
    if (pattern == 2) {
        // The second diagonal beside the better of the two chosen direct neighbours.
        if (std::min(c[0], c[1]) < std::min(c[2], c[3]))
            diag(h, -v);
        else
            diag(-h, v);
    }
}

// Motion search for one partition: diamond full-pel, then half/quarter-pel with SATD.
// Start points: neighbours' vectors and the same macroblock in the previous P frame.
// Points already measured (repeated start points, the centre a step just left) are not measured
// again: their cost never beats the best, so the result is the same with fewer SADs. predCost >= 0: the full-pel cost the neighbours ended with; a start
// point at least that good skips the coarse steps (32, 16). subpel: 0 full-pel only, 1 up to
// half-pel, 2 up to quarter-pel. fullPelCost (optional) gets the full-pel cost of the result.
double FrameCoder::motionSearch(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int pX, int pY,
                                double lambdaSad, const std::pair<int, int>* starts, int startCount, int subpel, int& outX,
                                int& outY, double predCost, double* fullPelCost) const
{
    const int range = 64 * 4;
    bool useSatd = false;
    // Full-pel positions (all of the first stage) read the plane in place: the SAD of sadPart
    // without its per-call set-up.
    const int px0 = mbx * 16 + x4 * 4, py0 = mby * 16 + y4 * 4;
    const uint8_t* const srcBlk = &srcY_[size_t(py0) * padW_ + px0];
    const uint8_t* const refBase = &R.F[size_t(py0 + kMargin) * extW_ + size_t(px0 + kMargin)];
    const int bw = w4 * 4, bh = h4 * 4;
    const WeightParams* const wpR = weightsOf(R);
    const bool weighted = wpR && wpR->luma;
    auto cost = [&](int vx, int vy) {
        const int sad = !useSatd && !(vx & 3) && !(vy & 3) && !weighted
                            ? sadBlock(srcBlk, padW_, refBase + ptrdiff_t(vy >> 2) * extW_ + (vx >> 2), extW_, bw, bh)
                            : sadPart(R, mbx, mby, x4, y4, w4, h4, vx, vy, useSatd);
        return double(sad) + lambdaSad * (seBits(vx - pX) + seBits(vy - pY));
    };
    // Start points measured so far (duplicates are common: the skip vector, the neighbours, the prediction).
    int seenX[16], seenY[16], seenCount = 0;
    int bx = 0, by = 0;
    double best = cost(0, 0);
    seenX[0] = seenY[0] = 0;
    seenCount = 1;
    for (int i = 0; i < startCount; ++i) {
        const int cx = std::clamp(starts[i].first & ~3, -range, range);
        const int cy = std::clamp(starts[i].second & ~3, -range, range);
        bool dup = false;
        for (int j = 0; j < seenCount && !dup; ++j)
            dup = seenX[j] == cx && seenY[j] == cy;
        if (dup)
            continue;
        if (seenCount < 16) {
            seenX[seenCount] = cx;
            seenY[seenCount++] = cy;
        }
        const double c = cost(cx, cy);
        if (c < best) {
            best = c;
            bx = cx;
            by = cy;
        }
    }
    // Experiment: a start point that already matches well (under 2 SAD per sample) skips
    // the coarse steps.
    const bool goodStart = ((gExp & 8) && best < 2.0 * 16.0 * w4 * h4) || (predCost >= 0 && best <= predCost);
    int fromX = INT_MIN, fromY = INT_MIN; // the centre the last move left (measured, never better)
    for (int step : {32, 16, 8, 4}) {
        if (goodStart && step > 8)
            continue;
        for (int iter = 0; iter < 16; ++iter) {
            bool moved = false;
            static constexpr int kDir[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& d : kDir) {
                const int cx = bx + d[0] * step, cy = by + d[1] * step;
                if (std::abs(cx) > range || std::abs(cy) > range || (cx == fromX && cy == fromY))
                    continue;
                const double c = cost(cx, cy);
                if (c < best) {
                    best = c;
                    fromX = bx;
                    fromY = by;
                    bx = cx;
                    by = cy;
                    moved = true;
                }
            }
            if (!moved)
                break;
        }
    }
    // Experiments (measured 15.11., B pictures with the lookahead's start points, fixed QP, CS2):
    // H264_MESQ=1 the four diagonal neighbours at one sample after the diamond (-0.3 % of the B
    // pictures' bits for +1.1 % instructions), H264_MEFULL = r every full-pel position within r
    // samples (an upper bound: r = 16 -3.6 % of all bits without the start points); H264_MEONLYB=1
    // only in B pictures.
    static const int kMeSq = std::getenv("H264_MESQ") ? std::atoi(std::getenv("H264_MESQ")) : 0;
    static const bool kMeOnlyB = std::getenv("H264_MEONLYB") && std::atoi(std::getenv("H264_MEONLYB")) != 0;
    const bool meExtra = !kMeOnlyB || bSlice_;
    if (kMeSq && meExtra) { // the four diagonal neighbours at one sample, repeated while one is better
        for (int iter = 0; iter < 8; ++iter) {
            const int cx0 = bx, cy0 = by;
            for (const auto& d : {std::pair{1, 1}, std::pair{1, -1}, std::pair{-1, 1}, std::pair{-1, -1}}) {
                const int cx = cx0 + d.first * 4, cy = cy0 + d.second * 4;
                if (std::abs(cx) > range || std::abs(cy) > range)
                    continue;
                const double c = cost(cx, cy);
                if (c < best) {
                    best = c;
                    bx = cx;
                    by = cy;
                }
            }
            if (bx == cx0 && by == cy0)
                break;
        }
    }
    static const int kMeFull = std::getenv("H264_MEFULL") ? std::atoi(std::getenv("H264_MEFULL")) : 0;
    if (kMeFull > 0 && meExtra) {
        const int cx0 = bx, cy0 = by;
        for (int dy = -kMeFull; dy <= kMeFull; ++dy)
            for (int dx = -kMeFull; dx <= kMeFull; ++dx) {
                const int cx = cx0 + dx * 4, cy = cy0 + dy * 4;
                if (std::abs(cx) > range || std::abs(cy) > range)
                    continue;
                const double c = cost(cx, cy);
                if (c < best) {
                    best = c;
                    bx = cx;
                    by = cy;
                }
            }
    }
    if (fullPelCost)
        *fullPelCost = best;
    useSatd = true;
    best = cost(bx, by);
    const bool diamond = gExp & 16; // experiment: 4 instead of 8 neighbours per sub-pel step
    for (int step : {2, 1}) {
        if (subpel < (step == 2 ? 1 : 2))
            break;
        const int cx0 = bx, cy0 = by;
        if ((step == 2 ? gSubpelHalf : gSubpelQuarter) > 0) {
            subpelStep(cost, cx0, cy0, step, range, best, bx, by);
            continue;
        }
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if ((!dx && !dy) || (diamond && dx && dy))
                    continue;
                const int cx = cx0 + dx * step, cy = cy0 + dy * step;
                if (std::abs(cx) > range || std::abs(cy) > range)
                    continue;
                const double c = cost(cx, cy);
                if (c < best) {
                    best = c;
                    bx = cx;
                    by = cy;
                }
            }
    }
    outX = bx;
    outY = by;
    return best;
}

// Quarter-pel step around a vector (a search that stopped at half-pel): SATD of the eight
// neighbours one quarter away, the centre's cost given.
double FrameCoder::refineQuarter(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int pX, int pY,
                                 double lambdaSad, int& vx, int& vy) const
{
    const int range = 64 * 4;
    auto cost = [&](int cx, int cy) {
        return double(sadPart(R, mbx, mby, x4, y4, w4, h4, cx, cy, true)) + lambdaSad * (seBits(cx - pX) + seBits(cy - pY));
    };
    double best = cost(vx, vy);
    const int cx0 = vx, cy0 = vy;
    if (gSubpelQuarter > 0) {
        subpelStep(cost, cx0, cy0, 1, range, best, vx, vy);
        return best;
    }
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = cx0 + dx, cy = cy0 + dy;
            if ((!dx && !dy) || std::abs(cx) > range || std::abs(cy) > range)
                continue;
            const double c = cost(cx, cy);
            if (c < best) {
                best = c;
                vx = cx;
                vy = cy;
            }
        }
    return best;
}

// Luma residual of an inter macroblock against the prediction lp: transform, quantisation
// (trellis), decimation and reconstruction.
void FrameCoder::codeInterLuma(int mbx, int mby, const uint8_t* lp, int qp, double lambda, bool t8, bool trellis, MbOut& io,
                               InterResidual& ir, double fineLambda)
{
    planInterLuma(mbx, mby, lp, qp, lambda, t8, trellis, ir, nullptr, fineLambda);
    reconInterLuma(mbx, mby, lp, qp, t8, io, ir);
}

// First half of coding an inter macroblock's luma residual: transform and quantisation (trellis,
// decimation) into ir. With est (trellis path only, else left at -1): the trellis' own estimate of
// squared luma error + lambda * coefficient bits, for comparing candidates without reconstructing them.
// fineLambda >= 0 (trellis path only): the psy candidate's finer quantisation - the coefficients
// above the lowest frequencies are decided with fineLambda instead of lambda.
void FrameCoder::planInterLuma(int mbx, int mby, const uint8_t* lp, int qp, double lambda, bool t8, bool trellis,
                               InterResidual& ir, double* est, double fineLambda, const InterResidual* emptyOf8)
{
    const size_t lumaOff = size_t(mby * 16) * padW_ + mbx * 16;
    const int qpDiv = qp / 6, qpMod = qp % 6;
    const int qbits = 15 + qpDiv;
    const int interOffset = (1 << qbits) / 6;
    int (*levels)[16] = ir.levels;
    int (*levels8)[64] = ir.levels8;
    int cbpLuma = 0; // levels8 is only read with t8, which writes all of it
    const CabacEncoder* live = cabacByTop_[size_t(rowKey_[size_t(mby)])];
    if (est)
        *est = -1;
    if (t8) {
        // No residual-group test here: measured with the same rule (RD gain against the block's
        // coded_block_pattern bit), it gains nothing for 8x8 blocks.
        double sum = 0;
        for (int b8 = 0; b8 < 4; ++b8) {
            const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
            int res[64];
            {
                const uint8_t* sp = &srcY_[lumaOff + size_t(oy) * padW_ + ox];
                const uint8_t* pp = lp + oy * 16 + ox;
                int y = 0;
#ifdef SCHNEIDI_H264_SSE2
                const __m128i zero = _mm_setzero_si128();
                for (; y < 8; ++y) {
                    const __m128i d = _mm_sub_epi16(
                        _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(sp + size_t(y) * padW_)), zero),
                        _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(pp + y * 16)), zero));
                    const __m128i sign = _mm_srai_epi16(d, 15);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + y * 8), _mm_unpacklo_epi16(d, sign));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(res + y * 8 + 4), _mm_unpackhi_epi16(d, sign));
                }
#elif defined(SCHNEIDI_H264_NEON)
                for (; y < 8; ++y) {
                    const int16x8_t d = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(sp + size_t(y) * padW_), vld1_u8(pp + y * 16)));
                    vst1q_s32(res + y * 8, vmovl_s16(vget_low_s16(d)));
                    vst1q_s32(res + y * 8 + 4, vmovl_high_s16(d));
                }
#endif
                for (; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        res[y * 8 + x] = int(sp[size_t(y) * padW_ + x]) - int(pp[y * 16 + x]);
            }
            const bool trellisPath = live && cfg_.effort > 0 && trellis;
            double xs[64];
            if (!trellisPath)
                quantize8x8(res, qp, 1.0 / 6.0, levels8[b8]);
            else
                quantize8x8Scan(res, qp, xs);
            if (trellisPath) {
                // Pixel error per level unit, by QP and scan position (computed once).
                static const auto kScale8 = [] {
                    std::array<std::array<double, 64>, 52> t{};
                    const Basis8& B = basis8();
                    for (int q = 0; q < 52; ++q)
                        for (int s = 0; s < 64; ++s) {
                            const int r = kZigzag8[s];
                            t[size_t(q)][size_t(s)] = kDequantV8[q % 6][posClass8(r)] * std::pow(2.0, q / 6 - 2) *
                                                      std::sqrt(B.norm[r >> 3] * B.norm[r & 7]) / 64.0;
                        }
                    return t;
                }();
                int scanLv[64];
                double energy = 0;
                uint64_t nz = 0;
                const double j = cabacTrellis(*live, 5, xs, kScale8[size_t(qp)].data(), 64, lambda, 0, scanLv,
                                              est ? &energy : nullptr, &nz, fineLambda, gPsyFineFrom8);
                const bool any = nz != 0;
                std::fill(levels8[b8], levels8[b8] + 64, 0);
                for (uint64_t m = nz; m; m &= m - 1) {
                    const int s = std::countr_zero(m);
                    levels8[b8][kZigzag8[s]] = std::clamp(scanLv[s], -kMaxLevel, kMaxLevel);
                }
                sum += any ? j : energy;
                ir.energy8[b8] = energy;
                if (any)
                    cbpLuma |= 1 << b8;
            } else {
                for (int k = 0; k < 64; ++k)
                    if (levels8[b8][k] != 0) {
                        cbpLuma |= 1 << b8;
                        break;
                    }
            }
        }
        for (int b = 0; b < 16; ++b)
            std::fill(levels[b], levels[b] + 16, 0);
        if (est && live && cfg_.effort > 0 && trellis)
            *est = sum;
        ir.cbpLuma = cbpLuma;
        return;
    }
    // Pixel error per level unit of a 4x4 coefficient, by QP and scan position.
    static const auto kScale4 = [] {
        std::array<std::array<double, 16>, 52> t{};
        static constexpr double kNorm4[4] = {4.0, 2.5, 4.0, 2.5};
        for (int q = 0; q < 52; ++q)
            for (int s = 0; s < 16; ++s) {
                const int r = kZigzag[s];
                t[size_t(q)][size_t(s)] = kDequantV[q % 6][posClass(r)] * double(1 << (q / 6)) *
                                          std::sqrt(kNorm4[r >> 2] * kNorm4[r & 3]) / 64.0;
            }
        return t;
    }();
    static const auto kMfScan = [] { // the quantisation multiplier by QP % 6 and scan position
        std::array<std::array<double, 16>, 6> t{};
        for (int q = 0; q < 6; ++q)
            for (int s = 0; s < 16; ++s)
                t[size_t(q)][size_t(s)] = kQuantMf[q][posClass(kZigzag[s])];
        return t;
    }();
    const double* scale4 = kScale4[size_t(qp)].data();
    const double invStep = 1.0 / double(1 << qbits); // a power of two: same as dividing
    const double* mf = kMfScan[size_t(qpMod)].data();
    double jBlk[16], eBlk[16];
    // What coding each block's levels gains over dropping them: squared error + lambda * bits of
    // the empty block minus the same for the chosen levels (0 for an empty block).
    double gain[16];
    const bool trellisPath = cfg_.effort > 0 && live && trellis;
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        if (emptyOf8 && est && trellisPath && !((emptyOf8->cbpLuma >> (blkIdx >> 2)) & 1)) {
            // The 8x8 plan left this quadrant without a level: taken as empty here too, at the
            // same squared error (the quadrant's, in equal shares).
            std::fill(levels[blkIdx], levels[blkIdx] + 16, 0);
            gain[blkIdx] = 0.0;
            jBlk[blkIdx] = eBlk[blkIdx] = 0.25 * emptyOf8->energy8[blkIdx >> 2];
            continue;
        }
        const int ox = kBlkX[blkIdx] * 4, oy = kBlkY[blkIdx] * 4;
        int res[16], coef[16];
        if (trellisPath) {
            forward4x4Diff(&srcY_[lumaOff + size_t(oy) * padW_ + ox], padW_, lp + oy * 16 + ox, 16, coef);
        } else {
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    res[y * 4 + x] = int(srcY_[lumaOff + size_t(oy + y) * padW_ + ox + x]) - int(lp[(oy + y) * 16 + ox + x]);
            forward4x4(res, coef);
        }
        double xs[16];
        for (int s = 0; s < 16; ++s)
            xs[s] = double(coef[kZigzag[s]]) * mf[s] * invStep;
        if (trellisPath) {
            // Trellis with CABAC costs on real-valued levels.
            int scanLv[16];
            uint64_t nz = 0;
            double zeroJ = 0;
            jBlk[blkIdx] = cabacTrellis(*live, 2, xs, scale4, 16, lambda, 1, scanLv, est ? &eBlk[blkIdx] : nullptr, &nz,
                                        fineLambda, gPsyFineFrom4, &zeroJ);
            std::fill(levels[blkIdx], levels[blkIdx] + 16, 0);
            for (uint64_t m = nz; m; m &= m - 1) {
                const int s = std::countr_zero(m);
                levels[blkIdx][kZigzag[s]] = std::clamp(scanLv[s], -kMaxLevel, kMaxLevel);
            }
            gain[blkIdx] = nz ? zeroJ - jBlk[blkIdx] : 0.0;
            if (gDecLog && nz && fineLambda < 0)
                decLogSample(levels[blkIdx], xs, scale4, lambda, gain[blkIdx]);
            continue;
        }
        if (cfg_.effort > 0 && !live) {
            // Round to nearest, then let the rate-distortion pass decide which levels to lower.
            for (int i = 0; i < 16; ++i)
                levels[blkIdx][i] = quantize(coef[i], kQuantMf[qpMod][posClass(i)], qbits, (1 << qbits) / 2);
            rdoQuantize(levels[blkIdx], res, qp, lambda, lumaNc(mbx * 4 + kBlkX[blkIdx], mby * 4 + kBlkY[blkIdx]));
        } else {
            for (int i = 0; i < 16; ++i)
                levels[blkIdx][i] = quantize(coef[i], kQuantMf[qpMod][posClass(i)], qbits, interOffset);
        }
        gain[blkIdx] = sparseLevelGain(levels[blkIdx], xs, scale4, lambda);
    }
    // Residual groups that do not pay for themselves: an 8x8 group is coded only when its blocks
    // together gain more than its own signalling and the context cost of a non-empty group
    // (kGroupBits, fitted by measurement: the coded_block_pattern bit alone would be about one).
    double sum = 0;
    for (int b8 = 0; b8 < 4; ++b8) {
        const double groupGain = gain[b8 * 4] + gain[b8 * 4 + 1] + gain[b8 * 4 + 2] + gain[b8 * 4 + 3];
        if (groupGain > lambda * kGroupBits)
            cbpLuma |= 1 << b8; // a group with a positive gain has a level
        else
            for (int k = 0; k < 4; ++k)
                std::fill(levels[b8 * 4 + k], levels[b8 * 4 + k] + 16, 0);
        if (est && trellisPath)
            for (int k = 0; k < 4; ++k)
                sum += (cbpLuma >> b8) & 1 ? jBlk[b8 * 4 + k] : eBlk[b8 * 4 + k];
    }
    if (est && trellisPath)
        *est = sum;
    ir.cbpLuma = cbpLuma;
}

// Second half: reconstruction of the planned levels over the prediction lp (and the 8x8 levels
// into the syntax).
void FrameCoder::reconInterLuma(int mbx, int mby, const uint8_t* lp, int qp, bool t8, MbOut& io, const InterResidual& ir)
{
    const size_t lumaOff = size_t(mby * 16) * padW_ + mbx * 16;
    const int qpDiv = qp / 6, qpMod = qp % 6;
    const int (*levels)[16] = ir.levels;
    const int (*levels8)[64] = ir.levels8;
    if (t8) {
        for (int b8 = 0; b8 < 4; ++b8) {
            const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
            if (!((ir.cbpLuma >> b8) & 1)) {
                for (int y = 0; y < 8; ++y)
                    std::memcpy(&recY_[lumaOff + size_t(oy + y) * padW_ + ox], lp + (oy + y) * 16 + ox, 8);
                std::memset(io.syn.luma8[b8], 0, sizeof(io.syn.luma8[b8]));
                continue;
            }
            int d[64], r[64];
            dequant8x8(levels8[b8], qp, d);
            inverse8x8(d, r);
            addClip8x8(lp + oy * 16 + ox, 16, r, &recY_[lumaOff + size_t(oy) * padW_ + ox], padW_);
            for (int k = 0; k < 64; ++k)
                io.syn.luma8[b8][k] = levels8[b8][kZigzag8[k]];
        }
        return;
    }
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        const int ox = kBlkX[blkIdx] * 4, oy = kBlkY[blkIdx] * 4;
        if (!((ir.cbpLuma >> (blkIdx >> 2)) & 1)) {
            for (int y = 0; y < 4; ++y)
                std::memcpy(&recY_[lumaOff + size_t(oy + y) * padW_ + ox], lp + (oy + y) * 16 + ox, 4);
            continue;
        }
        int d[16], r[16];
        dequant4x4(levels[blkIdx], kRows.v[qpMod], qpDiv, d);
        inverse4x4(d, r);
        addClip4x4(lp + oy * 16 + ox, 16, r, &recY_[lumaOff + size_t(oy) * padW_ + ox], padW_);
    }
}

// The part of an inter macroblock after its header: coded block pattern, mb_qp_delta and the
// coefficients (syntax, CAVLC bits, coefficient counts). Returns whether mb_qp_delta was sent.
bool FrameCoder::writeInterResidual(int mbx, int mby, int qp, bool t8, const InterResidual& res, const ChromaMb& ch, MbOut& io)
{
    const int lw = mbW_ * 4;
    BitWriter& interBits = io.bw;
    const int (*levels)[16] = res.levels;
    const int (*levels8)[64] = res.levels8;
    const int cbpLuma = res.cbpLuma;
    const int cbp = cbpLuma | (ch.cbp << 4);
    interBits.ue(uint32_t(interCbpCode(cbp)));
    io.syn.cbpLuma = cbpLuma;
    io.syn.t8 = t8 && cbpLuma != 0;
    io.syn.hasQpDelta = cbp != 0;
    io.syn.qpDelta = qp - qpPredOf(mby);
    if (cbp)
        interBits.se(qp - qpPredOf(mby)); // mb_qp_delta
    if (t8) {
        // 8x8 transform (CABAC only): the four 4x4 blocks carry the 8x8 block's count (bS, nC). The
        // 4x4 syntax levels are not read with the 8x8 transform, so they are not copied.
        for (int b8 = 0; b8 < 4; ++b8) {
            int total = 0;
            for (int k = 0; k < 64; ++k)
                total += levels8[b8][k] != 0;
            for (int i = 0; i < 4; ++i) {
                const int blkIdx = b8 * 4 + i;
                const int bx = mbx * 4 + kBlkX[blkIdx], by = mby * 4 + kBlkY[blkIdx];
                nzLuma_[size_t(by) * lw + bx] = uint8_t(std::min(total, 255));
            }
        }
    }
    for (int blkIdx = 0; blkIdx < 16 && !t8; ++blkIdx) {
        const int bx = mbx * 4 + kBlkX[blkIdx], by = mby * 4 + kBlkY[blkIdx];
        int total = 0;
        int scan[16];
        for (int k = 0; k < 16; ++k)
            scan[k] = levels[blkIdx][kZigzag[k]];
        std::memcpy(io.syn.luma[blkIdx], scan, sizeof(scan));
        if (cbpLuma & (1 << (blkIdx >> 2)))
            total = residual(interBits, scan, 16, lumaNc(bx, by));
        nzLuma_[size_t(by) * lw + bx] = uint8_t(std::min(total, 255));
    }
    writeChroma(io, mbx, mby, ch);
    return cbp != 0;
}

bool FrameCoder::encodeMbP(MbOut& result, int mbx, int mby)
{
    const int lw = mbW_ * 4, chw = mbW_ * 2;
    const size_t mbIndex = size_t(mby) * mbW_ + mbx;
    const int qp = mbQp_[mbIndex];
    const double lambda = 0.85 * std::pow(2.0, (qp - 12) / 3.0);
    const double lambdaSad = std::sqrt(lambda);
    const size_t lumaOff = size_t(mby * 16) * padW_ + mbx * 16;

    auto setBlocks = [&](int x4, int y4, int w4, int h4, int vx, int vy, int ref) {
        for (int y = y4; y < y4 + h4; ++y)
            for (int x = x4; x < x4 + w4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                mvX_[o] = int16_t(vx);
                mvY_[o] = int16_t(vy);
                refIdx_[o] = int8_t(ref);
            }
    };
    auto clearNz = [&] {
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                nzLuma_[o] = 0;
                modes4x4_[o] = -1;
            }
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < 4; ++b)
                nzChroma_[c][size_t(mby * 2 + (b >> 1)) * chw + mbx * 2 + (b & 1)] = 0;
    };

    const bool none[16] = {};
    int mvpX, mvpY;
    predictMvPart(mbx, mby, 0, 0, 4, 0, 0, none, mvpX, mvpY);

    // P_Skip vector (8.4.1.1).
    int skipX = mvpX, skipY = mvpY;
    {
        const MvNb a = mvNeighbour(mbx, mby, -1, 0, none), b = mvNeighbour(mbx, mby, 0, -1, none);
        if (!a.avail || !b.avail || (a.ref == 0 && a.mvx == 0 && a.mvy == 0) || (b.ref == 0 && b.mvx == 0 && b.mvy == 0))
            skipX = skipY = 0;
    }

    // ---- Candidate 1: P_Skip.
    uint8_t lp[256], cp[2][64];
    motionCompensate(*refs_[0], mbx, mby, 0, 0, 4, 4, skipX, skipY, lp, cp);
    {
        for (int y = 0; y < 16; ++y)
            std::memcpy(&recY_[lumaOff + size_t(y) * padW_], lp + y * 16, 16);
        const int cw = padW_ / 2;
        const size_t co = size_t(mby * 8) * cw + mbx * 8;
        for (int y = 0; y < 8; ++y) {
            std::memcpy(&recU_[co + size_t(y) * cw], cp[0] + y * 8, 8);
            std::memcpy(&recV_[co + size_t(y) * cw], cp[1] + y * 8, 8);
        }
    }
    clearNz();
    setBlocks(0, 0, 4, 4, skipX, skipY, 0);
    const double ssdSkip = double(mbSsd(mbx, mby));
    const double costSkip = ssdSkip + psyCost(mbx, mby) + lambda * skipBits(mbx, mby);
    tDecision[0] = 0;
    MbState skipState;
    saveMb(mbx, mby, skipState);

    // Early skip: if the skip residual quantises to nothing anyway, skip is all but certain
    // to win; spare the motion search.
    // Intra 16x16 SATD estimate (only the neighbours above and to the left are read).
    auto intraEstimate16 = [&] {
        const bool left = mbx > 0, top = topMbAvailable(mby);
        uint8_t cand[256];
        int est = 1 << 30;
        for (int mode = 0; mode < 4; ++mode) {
            if ((mode == 0 && !top) || (mode == 1 && !left) || (mode == 3 && !(top && left)))
                continue;
            predictLuma16(mode, &recY_[lumaOff], padW_, left, top, cand);
            int sum = 0;
            for (int oy = 0; oy < 16; oy += 4)
                sum += satd16x4U8(&srcY_[lumaOff + size_t(oy) * padW_], padW_, cand + oy * 16, 16);
            est = std::min(est, sum);
        }
        return est;
    };
    fullPelCost_[mbIndex] = -1.0f;
    // H264_EARLYSKIP=0: no early skip (measurements; skip then goes through the full decision).
    static const bool kEarlySkip = !std::getenv("H264_EARLYSKIP") || std::atoi(std::getenv("H264_EARLYSKIP")) != 0;
    // Experiment (16.11.2026, off): early skip only where the skip is also close to the source - squared
    // error (luma + chroma) up to H264_EARLYSKIPSSD lambda (0 = no limit); H264_EARLYSKIPSSDB the same for
    // the B pictures' direct skip. At a high QP a residual quantises to nothing although the prediction is
    // well off (a fast pan over foliage or water, where the predicted vector is not the motion); such a
    // macroblock then goes through the search and the full decision. Limit 20, measured: park_joy, P
    // pictures only at QP 36, +0.14 dB PSNR-Y at the same bits (40: +0.07, 80: nothing; no early skip at
    // all +0.14 dB and -0.6 % bits); two-pass BD-rate PSNR-Y / SSIM-Y / VMAF with both limits at 20: CS2
    // -2.7 / -2.5 / -1.2 %, park_joy -0.7 / -1.5 / -1.0 %, a pan in a game -0.6 / -0.8 / -0.6 %, a calm game
    // scene +0.3 / -0.2 / +1.4 %. Not the default: it costs +5 / +7 % instructions in P pictures alone (CS2 /
    // the calm scene, two-pass) and +12 / +50 % with the B pictures' limit, and the chroma planes lose 0.1
    // to 0.2 dB on the game clips while luma gains - to be tuned (limit by QP, chroma, speed) first.
    static const double kEarlySkipSsd = std::getenv("H264_EARLYSKIPSSD") ? std::atof(std::getenv("H264_EARLYSKIPSSD")) : 0.0;
    static const double kEarlySkipFloor = std::getenv("H264_EARLYSKIPFLOOR") ? std::atof(std::getenv("H264_EARLYSKIPFLOOR")) : 0.0;
    // H264_EARLYSKIPLIGHT: what a refused skip does without (bit 0: the split search, bit 1: the intra try).
    static const int kEarlySkipLight = std::getenv("H264_EARLYSKIPLIGHT") ? std::atoi(std::getenv("H264_EARLYSKIPLIGHT")) : 3;
    int offSkip = 0; // the skip's residual quantises to nothing, but the skip is far from the source
    if (kEarlySkip && skipResidualIsZero(mbx, mby, lp, cp, qp)) {
        if (kEarlySkipSsd <= 0 || ssdSkip <= std::max(kEarlySkipSsd * lambda, kEarlySkipFloor)) {
            result.syn.kind = MbSyntax::Kind::Skip;
            mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
            return false;
        }
        offSkip = kEarlySkipLight;
    }

    // ---- Candidate 2: inter with 16x16, 16x8, 8x16 or 8x8 partitions.
    // Motion search for one partition: diamond full-pel, then half/quarter-pel with SATD.
    // Start points: neighbours' vectors and the same macroblock in the previous P frame.
    std::vector<std::pair<int, int>> starts = {{skipX, skipY}};
    for (auto [nx, ny] : {std::pair{-1, 0}, std::pair{0, -1}, std::pair{4, -1}}) {
        const MvNb n = mvNeighbour(mbx, mby, nx, ny, none);
        if (n.ref == 0)
            starts.push_back({n.mvx, n.mvy});
    }
    {
        const size_t o = size_t(mby * 4) * lw + mbx * 4;
        if (prev_ && prev_->refIdx_[o] == 0)
            starts.push_back({prev_->mvX_[o], prev_->mvY_[o]});
        // With B pictures: the same block of the reference picture (its motion is kept there).
        const RefPic& R0 = *refs_[0];
        if (!prev_ && !R0.ref.empty() && R0.ref[o] == 0)
            starts.push_back({R0.mvx[o], R0.mvy[o]});
        if (!laMv_[0][0].empty())
            starts.push_back({laMv_[0][0][mbIndex], laMv_[0][1][mbIndex]});
    }
    int subpelNow = 2; // sub-pel refinement in search(): 2 quarter, 1 half, 0 none
    // Experiment H264_COARSE = factor (0 = off): a start point as good as the full-pel cost the
    // neighbours' 16x16 searches ended with (times factor) skips the coarse search steps. 1.5:
    // +0.20 % BD-rate for -1.2 % instructions, with the split rule above +0.55 % -> not used.
    static const double kCoarse = std::getenv("H264_COARSE") ? std::atof(std::getenv("H264_COARSE")) : 0.0;
    double predFull = -1;
    if (kCoarse > 0 && pSlice_ && !bSlice_) {
        const float l = mbx > 0 ? fullPelCost_[mbIndex - 1] : -1.0f;
        const float t = topMbAvailable(mby) ? fullPelCost_[mbIndex - size_t(mbW_)] : -1.0f;
        const float tr = topMbAvailable(mby) && mbx + 1 < mbW_ ? fullPelCost_[mbIndex - size_t(mbW_) + 1] : -1.0f;
        float m = -1.0f;
        for (float c : {l, t, tr})
            if (c >= 0 && (m < 0 || c < m))
                m = c;
        if (m >= 0)
            predFull = kCoarse * m;
    }
    double wholeFull = -1;
    auto search = [&](int x4, int y4, int w4, int h4, int ref, int pX, int pY, int& outX, int& outY) {
        starts.push_back({pX, pY});
        const double area = double(w4 * h4) / 16.0;
        const bool whole16 = w4 == 4 && h4 == 4 && ref == 0;
        const double best = motionSearch(*refs_[size_t(ref)], mbx, mby, x4, y4, w4, h4, pX, pY, lambdaSad, starts.data(),
                                         int(starts.size()), subpelNow, outX, outY, predFull >= 0 ? predFull * area : -1.0,
                                         whole16 ? &wholeFull : nullptr);
        starts.pop_back();
        return best;
    };

    struct Part {
        int x4, y4, w4, h4, shape;
    };
    struct Layout {
        int mbType;
        int typeBits;
        int count;
        Part parts[4];
    };
    static constexpr Layout kLayouts[4] = {
        {0, 1, 1, {{0, 0, 4, 4, 0}}},                                              // P_L0_16x16
        {1, 3, 2, {{0, 0, 4, 2, 1}, {0, 2, 4, 2, 2}}},                             // P_L0_L0_16x8
        {2, 3, 2, {{0, 0, 2, 4, 3}, {2, 0, 2, 4, 4}}},                             // P_L0_L0_8x16
        {3, 9, 4, {{0, 0, 2, 2, 0}, {2, 0, 2, 2, 0}, {0, 2, 2, 2, 0}, {2, 2, 2, 2, 0}}}, // P_8x8, all P_L0_8x8
    };
    struct Choice {
        double cost = 1e300;
        int layout = 0;
        int mv[4][2] = {};
        int mvp[4][2] = {};
        int ref[4] = {};
    };
    // ref_idx_l0 is te(v) (9.1.2): one inverted bit for two references, ue(v) for more.
    auto refBits = [&](int r) { return activeRefs_ <= 1 ? 0 : activeRefs_ == 2 ? 1 : seBits(0) + 2 * int(std::log2(r + 1)); };
    auto tryLayout = [&](int li) {
        const Layout& L = kLayouts[li];
        Choice ch;
        ch.layout = li;
        ch.cost = lambdaSad * L.typeBits;
        bool decoded[16] = {};
        for (int p = 0; p < L.count; ++p) {
            const Part& P = L.parts[p];
            double bestPart = 1e300;
            for (int r = 0; r < activeRefs_; ++r) {
                int pX, pY;
                predictMvPart(mbx, mby, P.x4, P.y4, P.w4, P.shape, r, decoded, pX, pY);
                int vx, vy;
                const double cost = search(P.x4, P.y4, P.w4, P.h4, r, pX, pY, vx, vy) + lambdaSad * refBits(r);
                if (cost < bestPart) {
                    bestPart = cost;
                    ch.mv[p][0] = vx;
                    ch.mv[p][1] = vy;
                    ch.mvp[p][0] = pX;
                    ch.mvp[p][1] = pY;
                    ch.ref[p] = r;
                }
            }
            ch.cost += bestPart;
            setBlocks(P.x4, P.y4, P.w4, P.h4, ch.mv[p][0], ch.mv[p][1], ch.ref[p]);
            for (int y = P.y4; y < P.y4 + P.h4; ++y)
                for (int x = P.x4; x < P.x4 + P.w4; ++x)
                    decoded[y * 4 + x] = true;
        }
        return ch;
    };
    const Choice whole = tryLayout(0);
    fullPelCost_[mbIndex] = float(wholeFull);
    Choice split;
    // No split layouts when the 16x16 cost is below c * sqrt(lambda): a well-predicted macroblock
    // rarely gains from splitting. Measured (two-pass, AQ + tree, 600 pictures CS2, BD-rate PSNR-Y /
    // instructions): 120 +0.03 % / -2.6 %, 160 +0.15 % / -7.4 %, 200 +0.53 % / -12.7 %. With the mode
    // decision by estimate (which gained 0.40 %): 180 +0.23 % / -3.0 %, 200 +0.43 % / -5.5 % against 160
    // -> 200 there, 160 without it. H264_SPLITSKIP overrides (0 = always search the splits).
    static const double kSplitSkipEnv = std::getenv("H264_SPLITSKIP") ? std::atof(std::getenv("H264_SPLITSKIP")) : -1.0;
    static const int kRdEstEnv = std::getenv("H264_RDEST") ? std::atoi(std::getenv("H264_RDEST")) : -1;
    const bool kRdEst = (kRdEstEnv < 0 ? cfg_.modeEstimate : kRdEstEnv != 0) && useCabac() && useT8() && cfg_.effort > 0;
    const double kSplitSkip = kSplitSkipEnv >= 0 ? kSplitSkipEnv : kRdEst ? 200.0 : 160.0;
    // Exception (13.11.): the 16x16 cost may be low overall but sit in one part of the macroblock
    // (a thin line, a dot or an object edge moving over a still background - drawn/synthetic
    // material, screen recordings). The splits are still searched when the dearest 8x8 quarter of
    // the 16x16 prediction (SATD) holds at least kSplitConc of the four quarters' sum and costs at
    // least kSplitQuad times its share of the threshold. Natural texture spreads the cost (the
    // dearest quarter ~0.25-0.35 of the sum). Without it the threshold cost +8.3 % BD-rate (PSNR-Y,
    // two-pass) on the animation test material for -3.4 % instructions there; SPLITSKIP was only
    // measured on CS2. With 0.5 / 0.4 (two-pass, 4 rates, BD-rate PSNR-Y / SSIM-Y against no
    // exception; instructions two-pass 8 Mbit/s, 120 pictures, 8 threads): animation -5.1 / -3.9 %
    // for +2.0 %, CS2 -0.2 / -0.2 % for +4.0 % (+0.6 % of it the four quarter SATDs), camera
    // -0.1 / -0.1 % for +1.8 %, calm -0.1 / -0.2 % for +0.2 %. Concentration 0.45: animation
    // fixed QP -3.6 instead of -4.3 %, CS2 +2.1 %; 0.35: CS2 +7.7 %. H264_SPLITQ / H264_SPLITR
    // override, H264_SPLITQ=0 = off (the stream before).
    static const double kSplitQuad = std::getenv("H264_SPLITQ") ? std::atof(std::getenv("H264_SPLITQ")) : 0.5;
    static const double kSplitConc = std::getenv("H264_SPLITR") ? std::atof(std::getenv("H264_SPLITR")) : 0.4;
    // The 8x8 plan of the 16x16 layout right after its search (mode decision by estimate; kept
    // for the decision below), for two shortcuts:
    // - Late skip: the search ended on the skip vector and the trellis leaves no luma level of the
    //   residual (nor does chroma) -> that candidate is the skip macroblock with more bits; skip
    //   is taken without the split search and the intra try. H264_LATESKIP=0: off.
    // - No split search when the plan leaves no luma level (what the trellis drops whole is
    //   predicted well enough). H264_ZERONOSPLIT=0: off.
    // Measured 14.11. (two-pass, 4 rates, BD-rate PSNR-Y on CS2 / camera / calm / animation;
    // instructions two-pass 8 Mbit/s CS2): late skip -0.03 / -0.03 / +0.04 / +0.34 % for -1.2 %,
    // no split search +0.03 / +0.01 / -0.04 / -0.18 % for -1.1 %.
    static const bool kLateSkip = !std::getenv("H264_LATESKIP") || std::atoi(std::getenv("H264_LATESKIP")) != 0;
    static const bool kZeroNoSplit = !std::getenv("H264_ZERONOSPLIT") || std::atoi(std::getenv("H264_ZERONOSPLIT")) != 0;
    InterResidual lateR8;
    double lateJ8 = 0;
    bool lateValid = false, wholeZero = false;
    const bool atSkip = whole.ref[0] == 0 && whole.mv[0][0] == skipX && whole.mv[0][1] == skipY;
    if (((kLateSkip && atSkip) || kZeroNoSplit) && kRdEst && !bSlice_ && tPsy.weight <= 0 &&
        cabacByTop_[size_t(rowKey_[size_t(mby)])]) {
        uint8_t lpWhole[256];
        if (!atSkip)
            mcLuma(*refs_[size_t(whole.ref[0])], mbx * 16 + (whole.mv[0][0] >> 2), mby * 16 + (whole.mv[0][1] >> 2),
                   whole.mv[0][0] & 3, whole.mv[0][1] & 3, 16, 16, lpWhole);
        planInterLuma(mbx, mby, atSkip ? lp : lpWhole, qp, lambda, true, true, lateR8, &lateJ8);
        lateValid = true;
        wholeZero = lateR8.cbpLuma == 0;
        if (kLateSkip && atSkip && wholeZero) {
            ChromaMb chLate;
            analyseChroma(mbx, mby, chLate, cp);
            if (chLate.cbp == 0) {
                restoreMb(mbx, mby, skipState);
                result.syn.kind = MbSyntax::Kind::Skip;
                mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
                return false;
            }
        }
    }
    bool skipSplits = whole.cost < kSplitSkip * lambdaSad;
    if (skipSplits && kSplitQuad > 0 && cfg_.effort > 0) {
        const double quadLim = kSplitQuad * 0.25 * kSplitSkip * lambdaSad;
        const RefPic& R = *refs_[size_t(whole.ref[0])];
        int qMax = 0, qSum = 0;
        for (int q = 0; q < 4; ++q) {
            const int s = sadPart(R, mbx, mby, (q & 1) * 2, (q >> 1) * 2, 2, 2, whole.mv[0][0], whole.mv[0][1], true);
            qMax = std::max(qMax, s);
            qSum += s;
        }
        skipSplits = !(qMax >= quadLim && qMax >= kSplitConc * qSum);
    }
    if ((wholeZero && kZeroNoSplit) || (offSkip & 1))
        skipSplits = true;
    if (cfg_.effort > 0 && !skipSplits) {
        // Experiment (H264_EXP & 256): the split layouts compare at full-pel (SATD); only the best
        // one gets the sub-pel refinement, and only when it is close.
        // The split layouts compare at half-pel; the best one gets the quarter-pel step only when its
        // cost is within 1.1 times the 16x16 cost. Measured (two-pass, AQ + tree, 600 pictures CS2,
        // BD-rate PSNR-Y / instructions): +0.29 % / -4.8 %; always refining the best split +0.37 % /
        // -3.8 %. H264_QPELSPLIT = c overrides, 0 = every split searched to quarter-pel.
        static const double kQpelSplit = std::getenv("H264_QPELSPLIT") ? std::atof(std::getenv("H264_QPELSPLIT")) : 1.1;
        const bool lateSubpel = gExp & 256;
        subpelNow = lateSubpel ? 0 : kQpelSplit > 0 ? 1 : 2;
        split = tryLayout(3);
        if (split.cost < whole.cost * 1.1) { // 16x8 / 8x16 only where splitting looks worth it
            for (int li : {1, 2}) {
                const Choice c = tryLayout(li);
                if (c.cost < split.cost)
                    split = c;
            }
        }
        subpelNow = 2;
        if (lateSubpel && split.cost < whole.cost * 1.1)
            split = tryLayout(split.layout);
        if (!lateSubpel && kQpelSplit > 0 && split.cost < whole.cost * kQpelSplit) {
            // Parts in order: a part's vector prediction follows the refined vectors before it.
            const Layout& L = kLayouts[split.layout];
            bool decoded[16] = {};
            split.cost = lambdaSad * L.typeBits;
            for (int p = 0; p < L.count; ++p) {
                const Part& P = L.parts[p];
                int pX, pY;
                predictMvPart(mbx, mby, P.x4, P.y4, P.w4, P.shape, split.ref[p], decoded, pX, pY);
                split.mvp[p][0] = pX;
                split.mvp[p][1] = pY;
                split.cost += refineQuarter(*refs_[size_t(split.ref[p])], mbx, mby, P.x4, P.y4, P.w4, P.h4, pX, pY, lambdaSad,
                                            split.mv[p][0], split.mv[p][1]) +
                              lambdaSad * refBits(split.ref[p]);
                setBlocks(P.x4, P.y4, P.w4, P.h4, split.mv[p][0], split.mv[p][1], split.ref[p]);
                for (int y = P.y4; y < P.y4 + P.h4; ++y)
                    for (int x = P.x4; x < P.x4 + P.w4; ++x)
                        decoded[y * 4 + x] = true;
            }
        }
    }

    // Encodes one inter choice; leaves its state in the macroblock and returns SSD + lambda * bits
    // (+ the psy cost, also left in interPsy).
    bool interDelta = false;
    double interPsy = 0;
    const bool trellisLast = gExp & 32; // experiment: candidates with plain quantisation, the winner again with the trellis
    bool trellisNow = !trellisLast;
    // Prediction and chroma coding of a layout do not depend on the transform size: kept for
    // the second coding of the same layout (8x8 transform).
    struct PredCache {
        bool valid = false, chroma = false;
        uint8_t lp[256], cp[2][64], recU[64], recV[64];
        ChromaMb ch;
    };
    PredCache predCache[2]; // whole, split
    const int cw = padW_ / 2;
    const size_t chromaOff = size_t(mby * 8) * cw + mbx * 8;
    auto predictLayout = [&](const Choice& c) -> PredCache& {
        PredCache& pc = predCache[c.layout == 0 ? 0 : 1];
        if (!pc.valid) {
            const Layout& layout = kLayouts[c.layout];
            for (int p = 0; p < layout.count; ++p) {
                const Part& P = layout.parts[p];
                motionCompensate(*refs_[size_t(c.ref[p])], mbx, mby, P.x4, P.y4, P.w4, P.h4, c.mv[p][0], c.mv[p][1], pc.lp, pc.cp);
            }
            pc.valid = true;
        }
        return pc;
    };
    // planned: the luma levels of this layout and transform from planInterLuma (else coded here).
    auto encodeInter = [&](const Choice& best, MbOut& io, bool t8, const InterResidual* planned = nullptr) {
        BitWriter& interBits = io.bw;
        const Layout& layout = kLayouts[best.layout];
        PredCache& pc = predictLayout(best);
        for (int p = 0; p < layout.count; ++p) {
            const Part& P = layout.parts[p];
            setBlocks(P.x4, P.y4, P.w4, P.h4, best.mv[p][0], best.mv[p][1], best.ref[p]);
        }
        InterResidual own;
        if (planned)
            reconInterLuma(mbx, mby, pc.lp, qp, t8, io, *planned);
        else
            codeInterLuma(mbx, mby, pc.lp, qp, lambda, t8, trellisNow, io, own);
        const InterResidual& res = planned ? *planned : own;
        if (pc.chroma) {
            for (int y = 0; y < 8; ++y) {
                std::memcpy(&recU_[chromaOff + size_t(y) * cw], pc.recU + y * 8, 8);
                std::memcpy(&recV_[chromaOff + size_t(y) * cw], pc.recV + y * 8, 8);
            }
        } else {
            analyseChroma(mbx, mby, pc.ch, pc.cp);
            for (int y = 0; y < 8; ++y) {
                std::memcpy(pc.recU + y * 8, &recU_[chromaOff + size_t(y) * cw], 8);
                std::memcpy(pc.recV + y * 8, &recV_[chromaOff + size_t(y) * cw], 8);
            }
            pc.chroma = true;
        }
        const ChromaMb& ch = pc.ch;
        clearNz();

        io.syn.kind = MbSyntax::Kind::Inter;
        io.syn.interType = layout.mbType;
        for (int p = 0; p < layout.count; ++p) {
            io.syn.ref[p] = best.ref[p];
            io.syn.mvd[p][0] = best.mv[p][0] - best.mvp[p][0];
            io.syn.mvd[p][1] = best.mv[p][1] - best.mvp[p][1];
        }
        interBits.ue(uint32_t(layout.mbType));
        if (layout.mbType == 3)
            for (int p = 0; p < 4; ++p)
                interBits.ue(0); // sub_mb_type P_L0_8x8
        if (activeRefs_ > 1)
            for (int p = 0; p < layout.count; ++p) {
                if (activeRefs_ == 2)
                    interBits.flag(best.ref[p] == 0); // te(v) with cMax 1: inverted bit
                else
                    interBits.ue(uint32_t(best.ref[p]));
            }
        for (int p = 0; p < layout.count; ++p) {
            interBits.se(best.mv[p][0] - best.mvp[p][0]);
            interBits.se(best.mv[p][1] - best.mvp[p][1]);
        }
        interDelta = writeInterResidual(mbx, mby, qp, t8, res, ch, io);
        // A skip_run ends here: count its codeword to be fair against skip.
        interPsy = psyCost(mbx, mby);
        return double(mbSsd(mbx, mby)) + interPsy + lambda * mbBits(io, mbx, mby, double(interBits.bitCount() + seBits(0)));
    };
    // Transform size by SATD (experiment H264_EXP & 512): the 8x8 Hadamard of the residual against
    // four 4x4 ones; clearly one way -> only that transform is coded, else both (as without).
    static const double kT8Lo = std::getenv("H264_T8LO") ? std::atof(std::getenv("H264_T8LO")) : 1.0;
    static const double kT8Hi = std::getenv("H264_T8HI") ? std::atof(std::getenv("H264_T8HI")) : 1.0;
    const bool t8Pre = useT8() && cfg_.effort > 0 && (gExp & 512);
    auto satd48 = [&](const Choice& c, int& s4, int& s8) {
        const Layout& layout = kLayouts[c.layout];
        uint8_t pred[256];
        for (int p = 0; p < layout.count; ++p) {
            const Part& P = layout.parts[p];
            const int mvx = c.mv[p][0], mvy = c.mv[p][1];
            mcLuma(*refs_[size_t(c.ref[p])], mbx * 16 + P.x4 * 4 + (mvx >> 2), mby * 16 + P.y4 * 4 + (mvy >> 2), mvx & 3,
                   mvy & 3, P.w4 * 4, P.h4 * 4, pred + P.y4 * 4 * 16 + P.x4 * 4);
        }
        s4 = s8 = 0;
        for (int oy = 0; oy < 16; oy += 4)
            s4 += satd16x4U8(&srcY_[lumaOff + size_t(oy) * padW_], padW_, pred + oy * 16, 16);
        for (int b8 = 0; b8 < 4; ++b8) {
            const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
            s8 += sa8dU8(&srcY_[lumaOff + size_t(oy) * padW_ + ox], padW_, pred + oy * 16 + ox, 16);
        }
    };
    auto transformPick = [&](const Choice& c) { // 0 = try both, 1 = 8x8 only, 2 = 4x4 only
        if (!t8Pre)
            return 0;
        int s4, s8;
        satd48(c, s4, s8);
        if (double(s8) < kT8Lo * s4)
            return 1;
        static const double kT8Slope = std::getenv("H264_T8SLOPE") ? std::atof(std::getenv("H264_T8SLOPE")) : 0.0;
        if (double(s8) > (kT8Hi + kT8Slope * std::max(0, qp - 26)) * s4)
            return 2;
        return 0;
    };
    // Try the split for real when its estimate is close or better.
    // Measured: 1.0 is as fast as 1.1 and -0.04 % BD-rate (0.95: +0.44 %). H264_SPLITF overrides.
    static const double kSplitTry = std::getenv("H264_SPLITF") ? std::atof(std::getenv("H264_SPLITF")) : 1.0;
    MbOut interBits;
    double costInter = 1e300;
    bool chosenInterDelta = false;
    MbState interState;
    Choice best = whole;
    bool bestT8 = false;   // transform of the chosen inter coding
    double bestPsy = 0;    // and its psy cost
    // Mode decision by estimate (CABAC, High profile): each candidate (16x16 or the best split, 4x4 or
    // 8x8 transform) is transformed and quantised with the trellis, whose own cost (squared error +
    // lambda * coefficient bits) ranks them; only the best transform of a layout is reconstructed and
    // counted for real, the other one only when the estimates are within kEstBoth * lambda, and the
    // split only when its estimate is below kEstSplit times the 16x16 one. Measured (two-pass, AQ +
    // tree, 600 pictures CS2, BD-rate PSNR-Y / SSIM-Y / instructions against coding every candidate):
    // -0.40 / -0.33 % / -1.7 % (the split now competes with its best transform, not only with 4x4;
    // the estimate picks the wrong transform for 0.12 % of the inter cost). kEstSplit 1.0: +0.10 %
    // for another -0.5 %, not worth it. EncoderConfig::modeEstimate; H264_RDEST=0/1 overrides.
    static const double kEstBoth = std::getenv("H264_ESTBOTH") ? std::atof(std::getenv("H264_ESTBOTH")) : 0.0;
    static const double kEstSplit = std::getenv("H264_ESTSPLIT") ? std::atof(std::getenv("H264_ESTSPLIT")) : 1e9;
    const CabacEncoder* liveCe = cabacByTop_[size_t(rowKey_[size_t(mby)])];
    const bool useEst = kRdEst && liveCe && cfg_.effort > 0 && useT8() && !trellisLast && !t8Pre;
    // A layout whose 8x8 plan leaves no luma level is not planned with the 4x4 transform as well
    // (without levels both transforms are the same macroblock; H264_ZERO8=0: plan both), and the
    // quadrants the 8x8 plan left empty stay empty in the 4x4 plan (H264_ZEROQ=0: all sixteen
    // blocks through the trellis). Measured 14.11. (two-pass, 4 rates, BD-rate PSNR-Y / SSIM-Y on
    // CS2, camera, calm, animation; instructions two-pass 8 Mbit/s CS2): the first -0.02 / 0.00,
    // +0.05 / +0.05, +0.01 / -0.04, -0.22 / -0.29 % for -2.2 %; the second +0.05 / -0.06, +0.35 /
    // +0.13, +0.18 / +0.09, +0.54 / -0.66 % for -2.3 % (only quadrants with every coefficient below
    // half a level: -0.8 %).
    static const bool kZeroQuad = !std::getenv("H264_ZEROQ") || std::atoi(std::getenv("H264_ZEROQ")) != 0;
    static const bool kZero8 = !std::getenv("H264_ZERO8") || std::atoi(std::getenv("H264_ZERO8")) != 0;
    if (useEst) {
        struct Plan {
            InterResidual r4, r8;
            double j4, j8;
        };
        auto plan = [&](const Choice& c, Plan& p) {
            const PredCache& pc = predictLayout(c);
            if (lateValid && c.layout == 0) {
                p.r8 = lateR8;
                p.j8 = lateJ8;
            } else {
                planInterLuma(mbx, mby, pc.lp, qp, lambda, true, true, p.r8, &p.j8);
            }
            p.j8 += lambda * cabacLumaSideBits(*liveCe, mbx, mby, p.r8.cbpLuma, true);
            if (kZero8 && p.r8.cbpLuma == 0) {
                p.j4 = 1e300;
                return;
            }
            planInterLuma(mbx, mby, pc.lp, qp, lambda, false, true, p.r4, &p.j4, -1.0, kZeroQuad ? &p.r8 : nullptr);
            p.j4 += lambda * cabacLumaSideBits(*liveCe, mbx, mby, p.r4.cbpLuma, false);
        };
        auto consider = [&](const Choice& c, bool t8, const Plan& p) {
            MbOut tmp;
            const double cost = encodeInter(c, tmp, t8, t8 ? &p.r8 : &p.r4);
            if (cost < costInter) {
                costInter = cost;
                chosenInterDelta = interDelta;
                interBits = std::move(tmp);
                saveMb(mbx, mby, interState);
                best = c;
                bestT8 = t8;
                bestPsy = interPsy;
            }
            return cost;
        };
        auto codeLayout = [&](const Choice& c, const Plan& p) {
            const bool t8 = p.j8 < p.j4;
            const double dist = std::abs(p.j8 - p.j4) / lambda;
            const double first = consider(c, t8, p);
            double other = 1e300;
            if ((dist < kEstBoth || gStats) && std::max(p.j4, p.j8) < 1e299)
                other = consider(c, !t8, p);
            if (gStats) {
                const int bucket = dist < 0.5 ? 0 : dist < 1 ? 1 : dist < 2 ? 2 : dist < 4 ? 3 : dist < 8 ? 4 : dist < 16 ? 5 : dist < 32 ? 6 : 7;
                std::lock_guard<std::mutex> l(gStatMutex);
                gStatEstT8[bucket][0] += 1;
                if (other < first) {
                    gStatEstT8[bucket][1] += 1;
                    gStatEstT8[bucket][2] += first - other;
                }
            }
            return std::min(first, other);
        };
        Plan pw;
        plan(whole, pw);
        const double costWhole = codeLayout(whole, pw);
        if (split.cost < whole.cost * kSplitTry) {
            Plan ps;
            plan(split, ps);
            // Header bits of both layouts as the CAVLC count (type, references, vector differences).
            auto headBits = [&](const Choice& c) {
                const Layout& L = kLayouts[c.layout];
                int bits = L.typeBits;
                for (int p = 0; p < L.count; ++p)
                    bits += refBits(c.ref[p]) + seBits(c.mv[p][0] - c.mvp[p][0]) + seBits(c.mv[p][1] - c.mvp[p][1]);
                return bits;
            };
            const double jw = std::min(pw.j4, pw.j8) + lambda * headBits(whole);
            const double js = std::min(ps.j4, ps.j8) + lambda * headBits(split);
            if (js < jw * kEstSplit || gStats) {
                const double costSplit = codeLayout(split, ps);
                if (gStats) {
                    const double r = js / std::max(1.0, jw);
                    const int bucket = r < 0.8 ? 0 : r < 0.9 ? 1 : r < 0.95 ? 2 : r < 1 ? 3 : r < 1.05 ? 4 : r < 1.1 ? 5 : r < 1.2 ? 6 : 7;
                    std::lock_guard<std::mutex> l(gStatMutex);
                    gStatEstSplit[bucket][0] += 1;
                    if (costSplit < costWhole) {
                        gStatEstSplit[bucket][1] += 1;
                        gStatEstSplit[bucket][2] += costWhole - costSplit;
                    }
                }
            }
        }
        if (gStats) {
            std::lock_guard<std::mutex> l(gStatMutex);
            gStatEstTotal += costInter;
        }
    } else {
    const int pickWhole = transformPick(whole);
    costInter = encodeInter(whole, interBits, pickWhole == 1);
    bestPsy = interPsy;
    int bestPick = pickWhole;
    chosenInterDelta = interDelta;
    saveMb(mbx, mby, interState);
    const int splitBucket = std::clamp(int(2.0 * std::log2(whole.cost / lambdaSad + 1.0)), 0, 19);
    const double costWhole4 = costInter;
    if (split.cost < whole.cost * kSplitTry) {
        MbOut splitBits;
        const int pickSplit = transformPick(split);
        const double costSplit = encodeInter(split, splitBits, pickSplit == 1);
        if (gStats && costSplit < costInter) {
            std::lock_guard<std::mutex> l(gStatMutex);
            gStatSplitWin[splitBucket] += 1;
            gStatSplitLoss[splitBucket] += costInter - costSplit;
        }
        if (costSplit < costInter) {
            costInter = costSplit;
            bestPsy = interPsy;
            chosenInterDelta = interDelta;
            interBits = std::move(splitBits);
            saveMb(mbx, mby, interState);
            best = split;
            bestPick = pickSplit;
        }
    }
    // 8x8 transform for the chosen layout (High profile). Experiment (H264_EXP & 128): not when
    // the 4x4 transform left no luma coefficients.
    bestT8 = bestPick == 1;
    if (useT8() && cfg_.effort > 0 && bestPick == 0 && !((gExp & 128) && interBits.syn.cbpLuma == 0)) {
        MbOut t8Bits;
        const double costT8 = encodeInter(best, t8Bits, true);
        if (gStats && (t8Bits.syn.cbpLuma || interBits.syn.cbpLuma)) {
            int s4, s8;
            satd48(best, s4, s8);
            const int bucket = std::clamp(int((double(s8) / std::max(1, s4) - 0.6) / 0.05), 0, 19);
            gStatT8Try[bucket].fetch_add(1, std::memory_order_relaxed);
            if (costT8 < costInter)
                gStatT8Win[bucket].fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> l(gStatMutex);
            gStatT8LossIf4[bucket] += std::max(0.0, costInter - costT8);
            gStatT8LossIf8[bucket] += std::max(0.0, costT8 - costInter);
            gStatT8Total += std::min(costT8, costInter);
        }
        if (costT8 < costInter) {
            costInter = costT8;
            bestPsy = interPsy;
            chosenInterDelta = interDelta;
            interBits = std::move(t8Bits);
            saveMb(mbx, mby, interState);
            bestT8 = true;
        }
    }
    if (gStats) {
        std::lock_guard<std::mutex> l(gStatMutex);
        gStatSplitN[splitBucket] += 1;
        gStatInterTotal += std::min(costInter, costWhole4);
    }
    if (trellisLast && cfg_.effort > 0) {
        trellisNow = true;
        MbOut finalBits;
        costInter = encodeInter(best, finalBits, bestT8);
        bestPsy = interPsy;
        chosenInterDelta = interDelta;
        interBits = std::move(finalBits);
        saveMb(mbx, mby, interState);
    }
    } // !useEst
    // Psy: the winner once more with the finer quantisation (see gPsyFine).
    if (tPsy.weight > 0 && gPsyFine > 0 && cfg_.effort > 0 && bestPsy > 0 && liveCe) {
        InterResidual fine;
        planInterLuma(mbx, mby, predictLayout(best).lp, qp, lambda, bestT8, true, fine, nullptr, lambda * gPsyFine);
        if (fine.cbpLuma || interBits.syn.cbpLuma) { // else the same macroblock again
            MbOut fineBits;
            const double costFine = encodeInter(best, fineBits, bestT8, &fine);
            if (costFine < costInter) {
                costInter = costFine;
                chosenInterDelta = interDelta;
                interBits = std::move(fineBits);
                saveMb(mbx, mby, interState);
            }
        }
    }

    // ---- Candidate 3: intra, only when a cheap Intra 16x16 SATD estimate beats the motion search.
    MbOut intraBits;
    double costIntra = 1e300;
    bool intraDelta = false;
    if (!(offSkip & 2)) {
        const int intraEstimate = intraEstimate16();
        // recY_ of this macroblock holds the inter reconstruction here, but Intra 16x16
        // prediction only reads the neighbours above and to the left.
        if (double(intraEstimate) < best.cost * gIntraFactor) {
            // Borderline cases (estimate above the inter cost) rarely win: only the cheap
            // intra types there.
            const bool borderline = double(intraEstimate) >= best.cost;
            tIntraSkip8 = borderline && (!gFullIntra || (gExp & 2));
            tIntraSkip16 = borderline && (!gFullIntra || (gExp & 4));
            tIntraEst16 = intraEstimate;
            tInterCost = best.cost;
            tIntraNone = false;
            intraDelta = encodeMbIntra(intraBits, mbx, mby);
            tIntraSkip8 = tIntraSkip16 = false;
            tIntraEst16 = -1;
            setBlocks(0, 0, 4, 4, 0, 0, -1);
            if (!tIntraNone)
                costIntra = double(mbSsd(mbx, mby)) + psyCost(mbx, mby) +
                            lambda * mbBits(intraBits, mbx, mby, double(intraBits.bw.bitCount() + seBits(0)));
            if (gStats) {
                const int bucket = std::min(3, int(2.0 * intraEstimate / std::max(1.0, best.cost)));
                gStatIntraTry[bucket].fetch_add(1, std::memory_order_relaxed);
                if (costIntra < costInter && costIntra < costSkip)
                    gStatIntraWin[bucket].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    static const bool kTrace = std::getenv("H264_MB_TRACE") != nullptr;
    if (kTrace)
        std::snprintf(tDecision, sizeof tDecision, " | lam %.0f skip %.0f inter %.0f (layout %d mv %d,%d) intra %.0f", lambda,
                      costSkip, costInter, best.layout, best.mv[0][0], best.mv[0][1], costIntra < 1e299 ? costIntra : -1.0);
    if (costSkip <= costInter && costSkip <= costIntra) {
        restoreMb(mbx, mby, skipState);
        result.syn.kind = MbSyntax::Kind::Skip;
        mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
        return false;
    }
    bool coded;
    if (costInter <= costIntra) {
        restoreMb(mbx, mby, interState);
        result = std::move(interBits);
        coded = chosenInterDelta;
        if (gStats) {
            gStatInter.fetch_add(1, std::memory_order_relaxed);
            if (result.syn.cbpLuma)
                gStatInterCbp.fetch_add(1, std::memory_order_relaxed);
            if (result.syn.t8)
                gStatT8.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        result = std::move(intraBits); // state is already the intra one
        coded = intraDelta;
    }
    commitQp(mbx, mby, coded);
    return true;
}

// Spatial direct prediction (8.4.1.2.2, direct_8x8_inference_flag = 1): per list the smallest
// reference index among the neighbours A, B and C (none in both lists: index 0 in both with zero
// vectors) and the median vector prediction for it; an 8x8 quadrant whose co-located block in the
// first list 1 picture (its corner 4x4 block) stands still takes the zero vector for index 0.
void FrameCoder::directPredict(int mbx, int mby, DirectPred& d) const
{
    const bool none[16] = {};
    int mvp[2][2] = {};
    for (int list = 0; list < 2; ++list) {
        const MvNb a = mvNeighbour(mbx, mby, -1, 0, none, list), b = mvNeighbour(mbx, mby, 0, -1, none, list);
        MvNb c = mvNeighbour(mbx, mby, 4, -1, none, list);
        if (!c.avail)
            c = mvNeighbour(mbx, mby, -1, -1, none, list);
        auto minPositive = [](int x, int y) { return x >= 0 && y >= 0 ? std::min(x, y) : std::max(x, y); };
        d.ref[list] = minPositive(a.ref, minPositive(b.ref, c.ref));
        if (d.ref[list] >= 0)
            predictMvPart(mbx, mby, 0, 0, 4, 0, d.ref[list], none, mvp[list][0], mvp[list][1], list);
    }
    const bool zero = d.ref[0] < 0 && d.ref[1] < 0;
    if (zero)
        d.ref[0] = d.ref[1] = 0;
    const RefPic& col = *refs1_[0];
    const int lw = mbW_ * 4;
    for (int q = 0; q < 4; ++q) {
        bool colZero = false;
        if (!zero) {
            const size_t o = size_t(mby * 4 + (q >> 1) * 3) * lw + size_t(mbx * 4 + (q & 1) * 3);
            colZero = col.ref[o] == 0 && std::abs(col.mvx[o]) <= 1 && std::abs(col.mvy[o]) <= 1;
        }
        for (int list = 0; list < 2; ++list) {
            const bool still = d.ref[list] < 0 || zero || (d.ref[list] == 0 && colZero);
            d.mv[list][q][0] = still ? 0 : mvp[list][0];
            d.mv[list][q][1] = still ? 0 : mvp[list][1];
        }
    }
}

// Implicit weights of a prediction from both lists (8.4.2.3.2): by the picture order distances,
// the nearer reference picture counts more. Equal weights are the plain average.
void FrameCoder::setBiWeights()
{
    biW0_ = biW1_ = 32;
    if (!cfg_.weightedBipred)
        return;
    const int tb = std::clamp(2 * (gopPos_ - refs_[0]->gopPos), -128, 127);
    const int td = std::clamp(2 * (refs1_[0]->gopPos - refs_[0]->gopPos), -128, 127);
    if (td == 0)
        return;
    const int tx = (16384 + std::abs(td / 2)) / td;
    const int scale = std::clamp((tb * tx + 32) >> 6, -1024, 1023) >> 2;
    if (scale < -64 || scale > 128)
        return;
    biW0_ = 64 - scale;
    biW1_ = scale;
}

// Prediction of a block of a B macroblock into macroblock-sized buffers: from list 0 (use 1),
// list 1 (2) or both (3): their rounded average or, with implicit weights, their weighted sum.
void FrameCoder::predictBi(int mbx, int mby, int x4, int y4, int w4, int h4, int use, const int* mv0, const int* mv1,
                           uint8_t* luma, uint8_t (*chroma)[64]) const
{
    if (use != 3) {
        const int* mv = use == 1 ? mv0 : mv1;
        motionCompensate(use == 1 ? *refs_[0] : *refs1_[0], mbx, mby, x4, y4, w4, h4, mv[0], mv[1], luma, chroma);
        return;
    }
    uint8_t l1[256], c1[2][64];
    motionCompensate(*refs_[0], mbx, mby, x4, y4, w4, h4, mv0[0], mv0[1], luma, chroma);
    motionCompensate(*refs1_[0], mbx, mby, x4, y4, w4, h4, mv1[0], mv1[1], l1, c1);
    const int w0 = biW0_, w1 = biW1_;
    if (w0 != w1) {
        for (int y = y4 * 4; y < (y4 + h4) * 4; ++y)
            for (int x = x4 * 4; x < (x4 + w4) * 4; ++x)
                luma[y * 16 + x] = clip1((luma[y * 16 + x] * w0 + l1[y * 16 + x] * w1 + 32) >> 6);
        for (int c = 0; c < 2; ++c)
            for (int y = y4 * 2; y < (y4 + h4) * 2; ++y)
                for (int x = x4 * 2; x < (x4 + w4) * 2; ++x)
                    chroma[c][y * 8 + x] = clip1((chroma[c][y * 8 + x] * w0 + c1[c][y * 8 + x] * w1 + 32) >> 6);
        return;
    }
    for (int y = y4 * 4; y < (y4 + h4) * 4; ++y)
        for (int x = x4 * 4; x < (x4 + w4) * 4; ++x)
            luma[y * 16 + x] = uint8_t((luma[y * 16 + x] + l1[y * 16 + x] + 1) >> 1);
    for (int c = 0; c < 2; ++c)
        for (int y = y4 * 2; y < (y4 + h4) * 2; ++y)
            for (int x = x4 * 2; x < (x4 + w4) * 2; ++x)
                chroma[c][y * 8 + x] = uint8_t((chroma[c][y * 8 + x] + c1[c][y * 8 + x] + 1) >> 1);
}

// A macroblock of a B picture: B_Skip (spatial direct prediction, nothing coded), B_Direct_16x16
// (the same prediction with a residual), one 16x16 or two 16x8 / 8x16 partitions from list 0,
// list 1 or both, or intra. Returns false for a skipped macroblock.
bool FrameCoder::encodeMbB(MbOut& result, int mbx, int mby)
{
    const int lw = mbW_ * 4, chw = mbW_ * 2;
    const size_t mbIndex = size_t(mby) * mbW_ + mbx;
    const int qp = mbQp_[mbIndex];
    const double lambda = 0.85 * std::pow(2.0, (qp - 12) / 3.0);
    const double lambdaSad = std::sqrt(lambda);
    const size_t lumaOff = size_t(mby * 16) * padW_ + mbx * 16;
    const int cw = padW_ / 2;
    const size_t chromaOff = size_t(mby * 8) * cw + mbx * 8;

    // Motion of a block: the lists in `use` take their vector, the others are unused (-1).
    auto setBlocks = [&](int x4, int y4, int w4, int h4, int use, const int* mv0, const int* mv1) {
        for (int y = y4; y < y4 + h4; ++y)
            for (int x = x4; x < x4 + w4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                mvX_[o] = int16_t(use & 1 ? mv0[0] : 0);
                mvY_[o] = int16_t(use & 1 ? mv0[1] : 0);
                refIdx_[o] = int8_t(use & 1 ? 0 : -1);
                mvX1_[o] = int16_t(use & 2 ? mv1[0] : 0);
                mvY1_[o] = int16_t(use & 2 ? mv1[1] : 0);
                refIdx1_[o] = int8_t(use & 2 ? 0 : -1);
            }
    };
    auto clearNz = [&] {
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const size_t o = size_t(mby * 4 + y) * lw + mbx * 4 + x;
                nzLuma_[o] = 0;
                modes4x4_[o] = -1;
            }
        for (int c = 0; c < 2; ++c)
            for (int b = 0; b < 4; ++b)
                nzChroma_[c][size_t(mby * 2 + (b >> 1)) * chw + mbx * 2 + (b & 1)] = 0;
    };
    // SATD of a macroblock-sized luma prediction.
    auto satd16 = [&](const uint8_t* pred) {
        int sum = 0;
        for (int oy = 0; oy < 16; oy += 4)
            sum += satd16x4U8(&srcY_[lumaOff + size_t(oy) * padW_], padW_, pred + oy * 16, 16);
        return sum;
    };

    // ---- Candidate 1: B_Skip, the direct prediction as it is.
    DirectPred d;
    directPredict(mbx, mby, d);
    const int dUse = (d.ref[0] >= 0 ? 1 : 0) | (d.ref[1] >= 0 ? 2 : 0);
    bool uniform = true;
    for (int q = 1; q < 4; ++q)
        for (int list = 0; list < 2; ++list)
            uniform = uniform && d.mv[list][q][0] == d.mv[list][0][0] && d.mv[list][q][1] == d.mv[list][0][1];
    uint8_t dlp[256], dcp[2][64];
    if (uniform)
        predictBi(mbx, mby, 0, 0, 4, 4, dUse, d.mv[0][0], d.mv[1][0], dlp, dcp);
    else
        for (int q = 0; q < 4; ++q)
            predictBi(mbx, mby, (q & 1) * 2, (q >> 1) * 2, 2, 2, dUse, d.mv[0][q], d.mv[1][q], dlp, dcp);
    auto setDirect = [&] {
        for (int q = 0; q < 4; ++q)
            setBlocks((q & 1) * 2, (q >> 1) * 2, 2, 2, dUse, d.mv[0][q], d.mv[1][q]);
    };
    for (int y = 0; y < 16; ++y)
        std::memcpy(&recY_[lumaOff + size_t(y) * padW_], dlp + y * 16, 16);
    for (int y = 0; y < 8; ++y) {
        std::memcpy(&recU_[chromaOff + size_t(y) * cw], dcp[0] + y * 8, 8);
        std::memcpy(&recV_[chromaOff + size_t(y) * cw], dcp[1] + y * 8, 8);
    }
    clearNz();
    setDirect();
    // Texture kept (EncoderConfig::bSharp). A prediction from both lists is the average of two
    // pictures. On a slowly drifting texture they show it at different sub-sample positions and
    // the average is softer than either; skipped, it costs nothing, so the B pictures were softer
    // than the P pictures around them and the texture pulsed at the P pictures' rate. The detail
    // an average has lost against its two single predictions (sum of the 4x4 Hadamard AC
    // magnitudes of the 8x8 blocks, hadamardMags8x8U8) is a cost of every candidate that uses it:
    // sharp * sqrt(lambda) per unit. Where the average is clearly the better prediction it still
    // wins; where it only saves the few bits of a vector, the macroblock copies from one picture.
    const double sharp = (bLongRun_ && !gBSharpLong ? 0.0 : gBSharpEnv >= 0 ? gBSharpEnv : cfg_.bSharp) *
                         textureShare(mbEnergy_[size_t(mby) * mbW_ + mbx]);
    // Lost detail: single = mean detail of the two single predictions, both = of their average;
    // satdSingle / satdBoth: SATD (or search cost) of the better single prediction and of the
    // average - the loss counts by how little closer to the source the average is (gBSharpKeep).
    auto lostOf = [](int single, int both, double satdSingle, double satdBoth) {
        const int lost = std::max(0, single - both);
        if (!(gBSharpKeep > 0) || lost == 0)
            return lost;
        const double ratio = satdBoth / std::max(1.0, satdSingle);
        return int(lost * std::clamp((ratio - gBSharpKeep) / (1.0 - gBSharpKeep), 0.0, 1.0));
    };
    auto detail = [](const uint8_t* p, int w, int h) { // p: stride 16
        int sum = 0;
        for (int y = 0; y < h; y += 8)
            for (int x = 0; x < w; x += 8) {
                alignas(16) int16_t m[128];
                hadamardMags8x8U8(p + y * 16 + x, 16, m);
                for (int i = 0; i < 64; ++i)
                    sum += m[i];
            }
        return sum;
    };
    int lostDirect = 0; // detail the direct prediction has lost, whole macroblock
    if (sharp > 0 && dUse == 3) {
        uint8_t t[256];
        int e = 0, satd[2] = {0, 0};
        const int px = mbx * 16, py = mby * 16;
        for (int list = 0; list < 2; ++list) {
            const RefPic& R = list ? *refs1_[0] : *refs_[0];
            if (uniform) {
                const int* mv = d.mv[list][0];
                mcLuma(R, px + (mv[0] >> 2), py + (mv[1] >> 2), mv[0] & 3, mv[1] & 3, 16, 16, t);
                e += detail(t, 16, 16);
                if (gBSharpKeep > 0)
                    satd[list] = satd16(t);
            } else
                for (int q = 0; q < 4; ++q) {
                    const int* mv = d.mv[list][q];
                    const int ox = (q & 1) * 8, oy = (q >> 1) * 8;
                    mcLuma(R, px + ox + (mv[0] >> 2), py + oy + (mv[1] >> 2), mv[0] & 3, mv[1] & 3, 8, 8, t);
                    e += detail(t, 8, 8);
                    if (gBSharpKeep > 0)
                        satd[list] += satd8x8U8(&srcY_[lumaOff + size_t(oy) * padW_ + ox], padW_, t, 16);
                }
        }
        lostDirect = lostOf(e / 2, detail(dlp, 16, 16), std::min(satd[0], satd[1]), gBSharpKeep > 0 ? satd16(dlp) : 0);
    }
    const double sharpDirect = sharp * lambdaSad * lostDirect;
    const double costSkip = double(mbSsd(mbx, mby)) + psyCost(mbx, mby) + lambda * skipBits(mbx, mby) + sharpDirect;
    MbState skipState;
    saveMb(mbx, mby, skipState);
    // Skipped at once where nothing would be coded - unless the lost detail weighs more than the
    // type and vector of a copy from one picture would cost (about six bits).
    static const double kEarlySkipFloorB = std::getenv("H264_EARLYSKIPFLOOR") ? std::atof(std::getenv("H264_EARLYSKIPFLOOR")) : 0.0;
    static const double kEarlySkipSsdB = std::getenv("H264_EARLYSKIPSSDB") ? std::atof(std::getenv("H264_EARLYSKIPSSDB")) : 0.0;
    if (sharpDirect <= 6 * lambda && (kEarlySkipSsdB <= 0 || double(mbSsd(mbx, mby)) <= std::max(kEarlySkipSsdB * lambda, kEarlySkipFloorB)) &&
        skipResidualIsZero(mbx, mby, dlp, dcp, qp)) {
        result.syn.kind = MbSyntax::Kind::Skip;
        mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
        if (gStats)
            ++gStatB[0];
        return false;
    }
    const double estDirect = double(satd16(dlp)) + lambdaSad + sharp * gBSharpEst * 0.5 * lostDirect;

    // ---- Candidate 2: motion search in both lists. Start points: the direct vector, the
    // neighbours' vectors and the co-located block's vector scaled to this picture's distance.
    struct Part {
        int x4, y4, w4, h4, shape;
    };
    static constexpr Part kPart[4][4] = {
        {{0, 0, 4, 4, 0}},
        {{0, 0, 4, 2, 1}, {0, 2, 4, 2, 2}},                                 // 16x8
        {{0, 0, 2, 4, 3}, {2, 0, 2, 4, 4}},                                 // 8x16
        {{0, 0, 2, 2, 0}, {2, 0, 2, 2, 0}, {0, 2, 2, 2, 0}, {2, 2, 2, 2, 0}}, // 8x8 (B_8x8)
    };
    struct Cand {
        double cost = 1e300;
        int bType = 0, shape = 0, count = 0;
        int use[4] = {};       // per partition: 1 list 0, 2 list 1, 3 both; 0 = direct (B_8x8 only)
        int mv[4][2][2] = {};  // [partition][list][x, y]
        int mvp[4][2][2] = {}; // the predictors the vector differences are coded against
        // 16x16 only: the cost of each choice of lists (1 list 0, 2 list 1, 3 both) and its vectors.
        double alt[4] = {1e300, 1e300, 1e300, 1e300};
        int altMv[4][2][2] = {};
        int lost = 0;     // detail lost by its partitions predicted from both lists (bSharp)
        int lostBi16 = 0; // 16x16 only: the same for the choice of both lists, whichever is chosen
    };
    const bool none[16] = {};
    std::pair<int, int> starts[2][8];
    int startCount[2] = {0, 0};
    for (int list = 0; list < 2; ++list) {
        auto add = [&](int x, int y) { starts[list][startCount[list]++] = {x, y}; };
        if (d.ref[list] >= 0)
            add(d.mv[list][0][0], d.mv[list][0][1]);
        for (auto [nx, ny] : {std::pair{-1, 0}, std::pair{0, -1}, std::pair{4, -1}}) {
            const MvNb n = mvNeighbour(mbx, mby, nx, ny, none, list);
            if (n.ref == 0)
                add(n.mvx, n.mvy);
        }
        if (!laMv_[list][0].empty())
            add(laMv_[list][0][mbIndex], laMv_[list][1][mbIndex]);
    }
    {
        const RefPic& col = *refs1_[0];
        const size_t o = size_t(mby * 4 + 1) * lw + size_t(mbx * 4 + 1);
        const int td = col.gopPos - col.refGopPos, tb = gopPos_ - refs_[0]->gopPos;
        if (col.ref[o] == 0 && td > 0 && col.refGopPos == refs_[0]->gopPos) {
            const int fx = col.mvx[o] * tb / td, fy = col.mvy[o] * tb / td;
            starts[0][startCount[0]++] = {fx, fy};
            starts[1][startCount[1]++] = {fx - col.mvx[o], fy - col.mvy[o]};
        } else if (col.ref[o] == 0 && td != 0) {
            // B pyramid: the co-located picture's vectors point to another picture than list 0.
            const int tc = gopPos_ - col.gopPos;
            starts[0][startCount[0]++] = {col.mvx[o] * tb / td, col.mvy[o] * tb / td};
            starts[1][startCount[1]++] = {col.mvx[o] * tc / td, col.mvy[o] * tc / td};
        }
    }
    // Bits of an mb_type by its number of bins (the first three have adaptive contexts).
    auto typeBits = [](int bType) { return bType == 0 ? 1 : bType <= 2 ? 3 : bType <= 11 ? 6 : 7; };
    auto mvBits = [&](const Cand& c, int p, int list) {
        return seBits(c.mv[p][list][0] - c.mvp[p][list][0]) + seBits(c.mv[p][list][1] - c.mvp[p][list][1]);
    };
    // Both lists together: the two vectors were found each on its own; their average is refined by
    // moving one vector at a time by a quarter sample (the other's prediction stays), as long as
    // SATD + vector bits of the average get smaller. Returns the cost; mv0 / mv1 are updated.
    auto refineBi = [&](const Part& P, int* mv0, int* mv1, const int* mvp0, const int* mvp1, uint8_t* a, uint8_t* b, double cost) {
        const int px = mbx * 16 + P.x4 * 4, py = mby * 16 + P.y4 * 4, w = P.w4 * 4, h = P.h4 * 4;
        const size_t so = lumaOff + size_t(P.y4 * 4) * padW_ + P.x4 * 4;
        uint8_t t[256], avg[256];
        for (int round = 0; round < biRefine_; ++round) {
            bool moved = false;
            for (int list = 0; list < 2; ++list) {
                int* mv = list ? mv1 : mv0;
                const int* mvp = list ? mvp1 : mvp0;
                const int* other = list ? mv0 : mv1;
                const int* otherP = list ? mvp0 : mvp1;
                uint8_t* own = list ? b : a;
                const uint8_t* fixed = list ? a : b;
                const int wOwn = list ? biW1_ : biW0_, wFixed = list ? biW0_ : biW1_;
                const double otherBits = lambdaSad * (seBits(other[0] - otherP[0]) + seBits(other[1] - otherP[1]));
                int bestDx = 0, bestDy = 0;
                for (const auto& [dx, dy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1}, std::pair{0, 1}}) {
                    const int vx = mv[0] + dx, vy = mv[1] + dy;
                    mcLuma(list ? *refs1_[0] : *refs_[0], px + (vx >> 2), py + (vy >> 2), vx & 3, vy & 3, w, h, t);
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                            avg[y * 16 + x] = clip1((t[y * 16 + x] * wOwn + fixed[y * 16 + x] * wFixed + 32) >> 6);
                    int satd = 0;
                    for (int oy = 0; oy < h; oy += 4)
                        for (int ox = 0; ox < w; ox += 8)
                            satd += satd8x4U8(&srcY_[so + size_t(oy) * padW_ + ox], padW_, avg + oy * 16 + ox, 16);
                    const double c = satd + lambdaSad * (seBits(vx - mvp[0]) + seBits(vy - mvp[1])) + otherBits;
                    if (c < cost) {
                        cost = c;
                        bestDx = dx;
                        bestDy = dy;
                    }
                }
                if (bestDx || bestDy) {
                    mv[0] += bestDx;
                    mv[1] += bestDy;
                    mcLuma(list ? *refs1_[0] : *refs_[0], px + (mv[0] >> 2), py + (mv[1] >> 2), mv[0] & 3, mv[1] & 3, w, h, own);
                    moved = true;
                }
            }
            if (!moved)
                break;
        }
        return cost;
    };
    // One layout (16x16, 16x8 or 8x16): per partition the search in both lists, then the cheapest
    // of list 0, list 1 and both by SATD + vector and type bits.
    auto tryShape = [&](int shape) {
        Cand c;
        c.shape = shape;
        c.count = shape == 0 ? 1 : 2;
        double total = 0;
        bool decoded[16] = {};
        for (int p = 0; p < c.count; ++p) {
            const Part& P = kPart[shape][p];
            double cost[2];
            for (int list = 0; list < 2; ++list) {
                int pX, pY;
                predictMvPart(mbx, mby, P.x4, P.y4, P.w4, P.shape, 0, decoded, pX, pY, list);
                c.mvp[p][list][0] = pX;
                c.mvp[p][list][1] = pY;
                starts[list][startCount[list]] = {pX, pY};
                cost[list] = motionSearch(list ? *refs1_[0] : *refs_[0], mbx, mby, P.x4, P.y4, P.w4, P.h4, pX, pY, lambdaSad,
                                          starts[list], startCount[list] + 1, 2, c.mv[p][list][0], c.mv[p][list][1]);
            }
            // Both lists: the average of the two predictions found.
            uint8_t a[256], b[256];
            const int px = mbx * 16 + P.x4 * 4, py = mby * 16 + P.y4 * 4, w = P.w4 * 4, h = P.h4 * 4;
            mcLuma(*refs_[0], px + (c.mv[p][0][0] >> 2), py + (c.mv[p][0][1] >> 2), c.mv[p][0][0] & 3, c.mv[p][0][1] & 3, w, h, a);
            mcLuma(*refs1_[0], px + (c.mv[p][1][0] >> 2), py + (c.mv[p][1][1] >> 2), c.mv[p][1][0] & 3, c.mv[p][1][1] & 3, w, h, b);
            uint8_t av[256];
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    av[y * 16 + x] = clip1((a[y * 16 + x] * biW0_ + b[y * 16 + x] * biW1_ + 32) >> 6);
            int satdBi = 0;
            const size_t so = lumaOff + size_t(P.y4 * 4) * padW_ + P.x4 * 4;
            for (int oy = 0; oy < h; oy += 4)
                for (int ox = 0; ox < w; ox += 8)
                    satdBi += satd8x4U8(&srcY_[so + size_t(oy) * padW_ + ox], padW_, av + oy * 16 + ox, 16);
            double costBi = satdBi + lambdaSad * (mvBits(c, p, 0) + mvBits(c, p, 1));
            const int lostBi = sharp > 0 ? lostOf((detail(a, w, h) + detail(b, w, h)) / 2, detail(av, w, h), std::min(cost[0], cost[1]), costBi) : 0;
            costBi += sharp * gBSharpEst * 0.5 * lostBi;
            if (shape == 0) {
                c.lostBi16 = lostBi;
                for (int u = 1; u <= 3; ++u)
                    for (int list = 0; list < 2; ++list)
                        for (int k = 0; k < 2; ++k)
                            c.altMv[u][list][k] = c.mv[0][list][k];
                c.alt[1] = cost[0];
                c.alt[2] = cost[1];
            }
            if (biRefine_ > 0 && costBi < gBiRefineNear * std::min(cost[0], cost[1])) {
                int m0[2] = {c.mv[p][0][0], c.mv[p][0][1]}, m1[2] = {c.mv[p][1][0], c.mv[p][1][1]};
                const double sharpBi = sharp * gBSharpEst * 0.5 * lostBi; // as before the refinement: it moves by quarter samples
                costBi = refineBi(P, m0, m1, c.mvp[p][0], c.mvp[p][1], a, b, costBi - sharpBi) + sharpBi;
                if (shape == 0) {
                    c.altMv[3][0][0] = m0[0];
                    c.altMv[3][0][1] = m0[1];
                    c.altMv[3][1][0] = m1[0];
                    c.altMv[3][1][1] = m1[1];
                }
                if (costBi < cost[0] && costBi < cost[1]) {
                    c.mv[p][0][0] = m0[0];
                    c.mv[p][0][1] = m0[1];
                    c.mv[p][1][0] = m1[0];
                    c.mv[p][1][1] = m1[1];
                }
            }
            if (shape == 0)
                c.alt[3] = costBi;
            c.use[p] = costBi < cost[0] && costBi < cost[1] ? 3 : cost[0] <= cost[1] ? 1 : 2;
            if (c.use[p] == 3)
                c.lost += lostBi;
            total += c.use[p] == 3 ? costBi : cost[c.use[p] - 1];
            // Later partitions predict their vectors from this one.
            setBlocks(P.x4, P.y4, P.w4, P.h4, c.use[p], c.mv[p][0], c.mv[p][1]);
            for (int y = P.y4; y < P.y4 + P.h4; ++y)
                for (int x = P.x4; x < P.x4 + P.w4; ++x)
                    decoded[y * 4 + x] = true;
        }
        if (shape == 0) {
            c.bType = c.use[0];
        } else {
            static constexpr int kPair[4][4] = {{0, 0, 0, 0}, {0, 0, 2, 4}, {0, 3, 1, 5}, {0, 6, 7, 8}}; // [use 0][use 1]
            c.bType = 4 + 2 * kPair[c.use[0]][c.use[1]] + (shape - 1);
        }
        c.cost = total + lambdaSad * typeBits(c.bType);
        if (shape == 0)
            for (int u = 1; u <= 3; ++u)
                c.alt[u] += lambdaSad * typeBits(u);
        return c;
    };
    // B_8x8: per 8x8 block the direct prediction (sub_mb_type B_Direct_8x8, no vector coded) or
    // its own search in list 0, list 1 or both.
    auto tryB8x8 = [&](const Cand& whole) {
        Cand c;
        c.shape = 3;
        c.count = 4;
        c.bType = 22;
        double total = lambdaSad * 7;
        bool decoded[16] = {};
        for (int p = 0; p < 4; ++p) {
            const Part& P = kPart[3][p];
            const size_t so = lumaOff + size_t(P.y4 * 4) * padW_ + P.x4 * 4;
            const int satdDirect = satd8x8U8(&srcY_[so], padW_, dlp + P.y4 * 64 + P.x4 * 4, 16);
            double cost[2];
            for (int list = 0; list < 2; ++list) {
                int pX, pY;
                predictMvPart(mbx, mby, P.x4, P.y4, P.w4, 0, 0, decoded, pX, pY, list);
                c.mvp[p][list][0] = pX;
                c.mvp[p][list][1] = pY;
                std::pair<int, int> st[10];
                int n = 0;
                for (int i = 0; i < startCount[list] && n < 8; ++i)
                    st[n++] = starts[list][i];
                st[n++] = {whole.mv[0][list][0], whole.mv[0][list][1]};
                st[n++] = {pX, pY};
                cost[list] = motionSearch(list ? *refs1_[0] : *refs_[0], mbx, mby, P.x4, P.y4, P.w4, P.h4, pX, pY, lambdaSad, st, n,
                                          true, c.mv[p][list][0], c.mv[p][list][1]);
            }
            uint8_t a[256], b[256];
            const int px = mbx * 16 + P.x4 * 4, py = mby * 16 + P.y4 * 4;
            mcLuma(*refs_[0], px + (c.mv[p][0][0] >> 2), py + (c.mv[p][0][1] >> 2), c.mv[p][0][0] & 3, c.mv[p][0][1] & 3, 8, 8, a);
            mcLuma(*refs1_[0], px + (c.mv[p][1][0] >> 2), py + (c.mv[p][1][1] >> 2), c.mv[p][1][0] & 3, c.mv[p][1][1] & 3, 8, 8, b);
            const int eSingle = sharp > 0 ? (detail(a, 8, 8) + detail(b, 8, 8)) / 2 : 0; // before a becomes the average
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    a[y * 16 + x] = clip1((a[y * 16 + x] * biW0_ + b[y * 16 + x] * biW1_ + 32) >> 6);
            double costBi = satd8x8U8(&srcY_[so], padW_, a, 16) + lambdaSad * (mvBits(c, p, 0) + mvBits(c, p, 1) + 5);
            const int lostBi = sharp > 0 ? lostOf(eSingle, detail(a, 8, 8), std::min(cost[0], cost[1]), costBi) : 0;
            costBi += sharp * gBSharpEst * 0.5 * lostBi;
            const double costDirect = satdDirect + lambdaSad + sharp * gBSharpEst * 0.5 * (lostDirect / 4);
            cost[0] += lambdaSad * 3;
            cost[1] += lambdaSad * 3;
            const double best = std::min({costDirect, costBi, cost[0], cost[1]});
            c.use[p] = best == costDirect ? 0 : best == cost[0] ? 1 : best == cost[1] ? 2 : 3;
            c.lost += c.use[p] == 0 ? lostDirect / 4 : c.use[p] == 3 ? lostBi : 0;
            total += best;
            if (c.use[p] == 0)
                setBlocks(P.x4, P.y4, 2, 2, dUse, d.mv[0][p], d.mv[1][p]);
            else
                setBlocks(P.x4, P.y4, 2, 2, c.use[p], c.mv[p][0], c.mv[p][1]);
            for (int y = P.y4; y < P.y4 + 2; ++y)
                for (int x = P.x4; x < P.x4 + 2; ++x)
                    decoded[y * 4 + x] = true;
        }
        c.cost = total;
        return c;
    };
    // The 16x16 candidate and, where its estimate is better, the best split one (both are coded for real).
    Cand cands[kMaxBCands];
    cands[0] = tryShape(0);
    int candCount = 1;
    if (cfg_.effort > 0 && gBParts && !(gBSplitSkip > 0 && std::min(cands[0].cost, estDirect) < gBSplitSkip * lambdaSad)) {
        for (int shape : {1, 2}) {
            const Cand c = tryShape(shape);
            if (c.cost < cands[1].cost)
                cands[1] = c;
        }
        if (gB8x8) {
            const Cand c = tryB8x8(cands[0]);
            if (c.cost < cands[1].cost)
                cands[1] = c;
        }
        if (cands[1].cost < cands[0].cost)
            candCount = 2;
    }
    double estSearch = candCount == 2 ? cands[1].cost : cands[0].cost;
    // Rate-distortion choice among the estimates (H264_BRD = factor, 0 = off): every candidate
    // whose SATD estimate is within the factor of the best one is coded for real - the other
    // choices of lists of the 16x16 layout and the best split even where it estimates worse.
    if (gBRd > 0) {
        const double lim = gBRd * estSearch;
        if (candCount == 1 && cands[1].count > 0 && cands[1].cost < lim)
            candCount = 2;
        for (int u = 1; u <= 3; ++u) {
            if (u == cands[0].use[0] || cands[0].alt[u] >= lim || candCount >= kMaxBCands)
                continue;
            Cand v = cands[0];
            v.use[0] = u;
            v.bType = u;
            v.lost = u == 3 ? cands[0].lostBi16 : 0;
            v.cost = cands[0].alt[u];
            std::memcpy(v.mv[0], cands[0].altMv[u], sizeof(v.mv[0]));
            cands[candCount++] = v;
        }
    }

    // Codes the direct prediction (which = 0) or a searched candidate (1, 2) with its residual;
    // leaves its state in the macroblock and returns SSD + lambda * bits.
    uint8_t clp[kMaxBCands][256], ccp[kMaxBCands][2][64];
    for (int k = 0; k < candCount; ++k)
        for (int p = 0; p < cands[k].count; ++p) {
            const Part& P = kPart[cands[k].shape][p];
            if (cands[k].use[p] == 0) { // a direct 8x8 block: the direct prediction's quarter
                for (int y = 0; y < 8; ++y)
                    std::memcpy(clp[k] + (P.y4 * 4 + y) * 16 + P.x4 * 4, dlp + (P.y4 * 4 + y) * 16 + P.x4 * 4, 8);
                for (int c = 0; c < 2; ++c)
                    for (int y = 0; y < 4; ++y)
                        std::memcpy(ccp[k][c] + (P.y4 * 2 + y) * 8 + P.x4 * 2, dcp[c] + (P.y4 * 2 + y) * 8 + P.x4 * 2, 4);
                continue;
            }
            predictBi(mbx, mby, P.x4, P.y4, P.w4, P.h4, cands[k].use[p], cands[k].mv[p][0], cands[k].mv[p][1], clp[k], ccp[k]);
        }
    struct ChromaCache {
        bool valid = false;
        uint8_t recU[64], recV[64];
        ChromaMb ch;
    };
    ChromaCache chromaCache[kMaxBCands + 1];
    bool sentDelta = false;
    double lastPsy = 0; // psy cost of the coding encodeB did last
    auto encodeB = [&](int which, MbOut& io, bool t8, double fineLambda, const InterResidual* planned = nullptr) {
        const Cand& best = cands[which ? which - 1 : 0];
        const uint8_t* lp = which ? clp[which - 1] : dlp;
        const uint8_t (*cp)[64] = which ? ccp[which - 1] : dcp;
        if (which) {
            for (int p = 0; p < best.count; ++p) {
                const Part& P = kPart[best.shape][p];
                if (best.use[p] == 0)
                    setBlocks(P.x4, P.y4, P.w4, P.h4, dUse, d.mv[0][p], d.mv[1][p]);
                else
                    setBlocks(P.x4, P.y4, P.w4, P.h4, best.use[p], best.mv[p][0], best.mv[p][1]);
            }
        } else {
            setDirect();
        }
        InterResidual own;
        if (planned)
            reconInterLuma(mbx, mby, lp, qp, t8, io, *planned);
        else
            codeInterLuma(mbx, mby, lp, qp, lambda, t8, true, io, own, fineLambda);
        const InterResidual& res = planned ? *planned : own;
        ChromaCache& cc = chromaCache[which];
        ChromaMb ch;
        if (cc.valid) {
            ch = cc.ch;
            for (int y = 0; y < 8; ++y) {
                std::memcpy(&recU_[chromaOff + size_t(y) * cw], cc.recU + y * 8, 8);
                std::memcpy(&recV_[chromaOff + size_t(y) * cw], cc.recV + y * 8, 8);
            }
        } else {
            analyseChroma(mbx, mby, ch, cp);
            cc.ch = ch;
            for (int y = 0; y < 8; ++y) {
                std::memcpy(cc.recU + y * 8, &recU_[chromaOff + size_t(y) * cw], 8);
                std::memcpy(cc.recV + y * 8, &recV_[chromaOff + size_t(y) * cw], 8);
            }
            cc.valid = true;
        }
        clearNz();
        io.syn.kind = MbSyntax::Kind::Inter;
        io.syn.bType = uint8_t(which ? best.bType : 0);
        if (which && best.shape == 3)
            for (int p = 0; p < 4; ++p)
                io.syn.bSub[p] = uint8_t(best.use[p]);
        if (which)
            for (int p = 0; p < best.count; ++p)
                for (int c = 0; c < 2; ++c) {
                    io.syn.mvd[p][c] = best.mv[p][0][c] - best.mvp[p][0][c];
                    io.syn.mvd1[p][c] = best.mv[p][1][c] - best.mvp[p][1][c];
                }
        sentDelta = writeInterResidual(mbx, mby, qp, t8, res, ch, io);
        lastPsy = psyCost(mbx, mby);
        return double(mbSsd(mbx, mby)) + lastPsy + lambda * mbBits(io, mbx, mby, 0.0) + (which ? sharp * lambdaSad * best.lost : sharpDirect);
    };
    MbOut interBits;
    int bestWhich = 0;
    bool bestT8 = false;
    double costInter = 1e300;
    double bestPsy = 0;
    bool chosenDelta = false;
    MbState interState;
    const CabacEncoder* liveCe = cabacByTop_[size_t(rowKey_[size_t(mby)])];
    // Transform by estimate (H264_BEST): per candidate both transforms are quantised with the
    // trellis, whose own cost (squared error + lambda * coefficient and side bits) picks the one
    // that is coded for real - as in P pictures (Encoder modeEstimate), instead of 4x4 for all and
    // 8x8 only for the winner.
    const bool useEst = (gBEstEnv < 0 ? cfg_.modeEstimate : gBEstEnv != 0) && liveCe && useT8() && cfg_.effort > 0;
    if (useEst) {
        static const bool kZeroQuadB = !std::getenv("H264_ZEROQ") || std::atoi(std::getenv("H264_ZEROQ")) != 0;
        static const bool kZero8B = !std::getenv("H264_ZERO8") || std::atoi(std::getenv("H264_ZERO8")) != 0;
        // A candidate whose estimate (with its vector and type bits as the CAVLC count) is above
        // kBPrune times the best one's is not coded for real. Measured (fixed QP, CS2, 3 B forced):
        // 1.1 +0.1 % of the B pictures' bits for -2.1 % instructions. H264_BPRUNE overrides, 0 = off.
        static const double kBPrune = std::getenv("H264_BPRUNE") ? std::atof(std::getenv("H264_BPRUNE")) : 1.1;
        InterResidual r8[kMaxBCands + 1], r4[kMaxBCands + 1];
        double jBest[kMaxBCands + 1];
        bool use8[kMaxBCands + 1];
        double jMin = 1e300;
        for (int which = 0; which <= candCount; ++which) {
            const uint8_t* lp = which ? clp[which - 1] : dlp;
            double j8 = 0, j4 = 1e300;
            planInterLuma(mbx, mby, lp, qp, lambda, true, true, r8[which], &j8);
            j8 += lambda * cabacLumaSideBits(*liveCe, mbx, mby, r8[which].cbpLuma, true);
            if (!(kZero8B && r8[which].cbpLuma == 0)) {
                planInterLuma(mbx, mby, lp, qp, lambda, false, true, r4[which], &j4, -1.0, kZeroQuadB ? &r8[which] : nullptr);
                j4 += lambda * cabacLumaSideBits(*liveCe, mbx, mby, r4[which].cbpLuma, false);
            }
            use8[which] = j8 < j4;
            jBest[which] = std::min(j8, j4);
            if (kBPrune > 0) {
                int head = 1;
                if (which) {
                    const Cand& c = cands[which - 1];
                    head = typeBits(c.bType);
                    for (int p = 0; p < c.count; ++p)
                        for (int list = 0; list < 2; ++list)
                            if (c.use[p] & (1 << list))
                                head += mvBits(c, p, list);
                }
                jBest[which] += lambda * head + (which ? sharp * lambdaSad * cands[which - 1].lost : sharpDirect);
                jMin = std::min(jMin, jBest[which]);
            }
        }
        for (int which = 0; which <= candCount; ++which) {
            if (kBPrune > 0 && jBest[which] > kBPrune * jMin)
                continue;
            const bool t8 = use8[which];
            MbOut candBits;
            const double costCand = encodeB(which, candBits, t8, -1.0, t8 ? &r8[which] : &r4[which]);
            if (costCand < costInter) {
                costInter = costCand;
                bestPsy = lastPsy;
                chosenDelta = sentDelta;
                interBits = std::move(candBits);
                saveMb(mbx, mby, interState);
                bestWhich = which;
                bestT8 = t8;
            }
        }
    } else {
        costInter = encodeB(0, interBits, false, -1.0);
        bestPsy = lastPsy;
        chosenDelta = sentDelta;
        saveMb(mbx, mby, interState);
    }
    for (int which = 1; which <= (useEst ? 0 : candCount); ++which) {
        MbOut candBits;
        const double costCand = encodeB(which, candBits, false, -1.0);
        if (costCand < costInter) {
            costInter = costCand;
            bestPsy = lastPsy;
            chosenDelta = sentDelta;
            interBits = std::move(candBits);
            saveMb(mbx, mby, interState);
            bestWhich = which;
        }
    }
    // 8x8 transform for the best of them (High profile).
    if (useT8() && cfg_.effort > 0 && !useEst) {
        MbOut t8Bits;
        const double costT8 = encodeB(bestWhich, t8Bits, true, -1.0);
        if (costT8 < costInter) {
            costInter = costT8;
            bestPsy = lastPsy;
            chosenDelta = sentDelta;
            interBits = std::move(t8Bits);
            saveMb(mbx, mby, interState);
            bestT8 = true;
        }
    }
    // Psy: the winner once more with the finer quantisation (see gPsyFine).
    if (tPsy.weight > 0 && gPsyFine > 0 && cfg_.effort > 0 && bestPsy > 0) {
        MbOut fineBits;
        const double costFine = encodeB(bestWhich, fineBits, bestT8, lambda * gPsyFine);
        if (costFine < costInter) {
            costInter = costFine;
            chosenDelta = sentDelta;
            interBits = std::move(fineBits);
            saveMb(mbx, mby, interState);
        }
    }

    // ---- Candidate 3: intra, only when a cheap Intra 16x16 SATD estimate beats the predictions.
    MbOut intraBits;
    double costIntra = 1e300;
    bool intraDelta = false;
    {
        const bool left = mbx > 0, top = topMbAvailable(mby);
        uint8_t cand[256];
        int est = 1 << 30;
        for (int mode = 0; mode < 4; ++mode) {
            if ((mode == 0 && !top) || (mode == 1 && !left) || (mode == 3 && !(top && left)))
                continue;
            predictLuma16(mode, &recY_[lumaOff], padW_, left, top, cand);
            est = std::min(est, satd16(cand));
        }
        const double estInter = std::min(estSearch, estDirect);
        if (double(est) < estInter * gIntraFactor) {
            const bool borderline = double(est) >= estInter;
            tIntraSkip8 = borderline && (!gFullIntra || (gExp & 2));
            tIntraSkip16 = borderline && (!gFullIntra || (gExp & 4));
            tIntraEst16 = est;
            tInterCost = estInter;
            tIntraNone = false;
            intraDelta = encodeMbIntra(intraBits, mbx, mby);
            tIntraSkip8 = tIntraSkip16 = false;
            tIntraEst16 = -1;
            const int zero[2] = {0, 0};
            setBlocks(0, 0, 4, 4, 0, zero, zero);
            if (!tIntraNone)
                costIntra = double(mbSsd(mbx, mby)) + psyCost(mbx, mby) + lambda * mbBits(intraBits, mbx, mby, 0.0);
        }
    }

    if (costSkip <= costInter && costSkip <= costIntra) {
        restoreMb(mbx, mby, skipState);
        result.syn.kind = MbSyntax::Kind::Skip;
        mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
        if (gStats)
            ++gStatB[0];
        return false;
    }
    bool coded;
    if (costInter <= costIntra) {
        restoreMb(mbx, mby, interState);
        result = std::move(interBits);
        coded = chosenDelta;
        if (gStats) {
            const int t = result.syn.bType;
            ++gStatB[t == 0 ? 1 : t <= 3 ? 1 + t : t < 22 ? 5 : 6];
            if (result.syn.t8)
                ++gStatB[8];
        }
    } else {
        if (gStats)
            ++gStatB[7];
        result = std::move(intraBits); // state is already the intra one
        coded = intraDelta;
    }
    commitQp(mbx, mby, coded);
    return true;
}

// After a macroblock is chosen: its QP is the planned one if it sent mb_qp_delta, else the predictor.
// VBV inside the picture (vbvRowCap_): the QP rise for macroblock (mbx, r), applied to its mbQp_. Seen so far: this
// row up to mbx and every row r - j above up to column mbx + 2j - what the wavefront has finished for certain
// before this macroblock starts, on any number of threads. Projection: their bits, plus the planned share of the
// rest scaled by what the seen part took against its plan (at the QPs it got) and by 2^(-rise/6).
static const double gVbvRowStart = std::getenv("H264_VBV_ROWSTART") ? std::atof(std::getenv("H264_VBV_ROWSTART")) : 0.05;

int FrameCoder::vbvRowAdjust(int mbx, int r)
{
    const size_t stride = size_t(mbW_ + 1);
    double bits = vbvPrefB_[size_t(r) * stride + size_t(mbx)], eff = vbvPrefE_[size_t(r) * stride + size_t(mbx)],
           plan = vbvPrefW_[size_t(r) * stride + size_t(mbx)];
    for (int j = 1; j <= r; ++j) {
        const size_t k = size_t(r - j) * stride + size_t(std::min(mbW_, mbx + 2 * j));
        bits += vbvPrefB_[k];
        eff += vbvPrefE_[k];
        plan += vbvPrefW_[k];
    }
    if (plan < gVbvRowStart * vbvWTotal_ || eff <= 0)
        return 0;
    const size_t m = size_t(r) * mbW_ + size_t(mbx);
    const double room = vbvRowCap_ - bits, need = bits / eff * std::max(0.0, vbvWTotal_ - plan);
    int add = 0;
    if (room <= 0)
        add = 51;
    else if (need > room)
        add = int(std::ceil(6.0 * std::log2(need / room)));
    add = std::clamp(add, 0, 51 - mbQp_[m]);
    mbQp_[m] = int8_t(mbQp_[m] + add);
    return add;
}

void FrameCoder::commitQp(int mbx, int mby, bool codedDelta)
{
    const size_t m = size_t(mby) * mbW_ + mbx;
    int& pred = qpPredByTop_[size_t(rowKey_[size_t(mby)])];
    if (codedDelta)
        pred = mbQp_[m];
    mbQpFinal_[m] = int8_t(pred);
}

void FrameCoder::buildRefPlanes(RefPic& R)
{
    const int M = kMargin;
    extW_ = padW_ + 2 * M;
    extH_ = padH_ + 2 * M;
    const size_t n = size_t(extW_) * extH_;
    R.F.resize(n);
    R.B.resize(n);
    R.V.resize(n);
    R.J.resize(n);
    const int W = extW_, H = extH_;

    // Runs fn(y0, y1) over row ranges on cfg_.threads threads.
    auto parallelRows = [&](auto&& fn) {
        const int parts = std::clamp(cfg_.threads, 1, H);
        std::atomic<int> next{0};
        runOnThreads(parts, [&] {
            for (int p; (p = next.fetch_add(1)) < parts;)
                fn(p * H / parts, (p + 1) * H / parts);
        });
    };
    parallelRows([&](int y0, int y1) { refFullRows(R, y0, y1); });
    parallelRows([&](int y0, int y1) { refHalfRows(R, y0, y1); });
}

// Full-pel luma with the margin (edge samples repeated) for the rows [y0, y1) of the extended plane.
void FrameCoder::refFullRows(RefPic& R, int y0, int y1) const
{
    const int M = kMargin;
    const int W = extW_, H = extH_;
    (void)M;
    (void)H;
    for (int y = y0; y < y1; ++y) {
        const uint8_t* src = &R.y[size_t(std::clamp(y - M, 0, padH_ - 1)) * padW_];
        uint8_t* dst = &R.F[size_t(y) * W];
        std::memset(dst, src[0], size_t(M));
        std::memcpy(dst + M, src, size_t(padW_));
        std::memset(dst + M + padW_, src[padW_ - 1], size_t(M));
    }
}

// Half-pel planes B, V, J for the rows [y0, y1); needs the full-pel rows y0 - 2 .. y1 + 2.
#ifdef SCHNEIDI_H264_AVX2
// AVX2 parts of refHalfRows (the SSE2 steps 16 samples at a time; same numbers): the vertical
// 6-tap (32-bit intermediate + rounded plane) and the horizontal 6-tap of one row, continuing at
// x / hEnd; the centre 6-tap over the intermediate, continuing at jEnd.
// (Helpers take and return __m256i: only ever inlined into AVX2 functions, so GCC's ABI note is moot.)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpsabi"
#endif
#define S264_AVX2 __attribute__((target("avx2"))) static inline
S264_AVX2 __m256i halfLd16(const uint8_t* p) { return _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))); }
S264_AVX2 __m256i halfTap16(__m256i a, __m256i b, __m256i c, __m256i d, __m256i e, __m256i f)
{
    const __m256i outer = _mm256_add_epi16(a, f), mid = _mm256_add_epi16(c, d), side = _mm256_add_epi16(b, e);
    return _mm256_sub_epi16(_mm256_add_epi16(outer, _mm256_mullo_epi16(mid, _mm256_set1_epi16(20))),
                            _mm256_mullo_epi16(side, _mm256_set1_epi16(5)));
}
S264_AVX2 void halfStore8(uint8_t* p, __m256i v16) // 16 x int16, (v + 16) >> 5 -> 16 x uint8 (saturated)
{
    const __m256i v = _mm256_srai_epi16(_mm256_add_epi16(v16, _mm256_set1_epi16(16)), 5);
    const __m256i packed = _mm256_permute4x64_epi64(_mm256_packus_epi16(v, v), 0xD8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm256_castsi256_si128(packed));
}
S264_AVX2 __m256i halfTap32(const int* v, int x)
{
#define S264_LD(i) _mm256_loadu_si256(reinterpret_cast<const __m256i*>(v + (i)))
    const __m256i outer = _mm256_add_epi32(S264_LD(x - 2), S264_LD(x + 3));
    const __m256i mid = _mm256_add_epi32(S264_LD(x), S264_LD(x + 1)), side = _mm256_add_epi32(S264_LD(x - 1), S264_LD(x + 2));
#undef S264_LD
    const __m256i mid20 = _mm256_add_epi32(_mm256_slli_epi32(mid, 4), _mm256_slli_epi32(mid, 2));
    const __m256i side5 = _mm256_add_epi32(_mm256_slli_epi32(side, 2), side);
    const __m256i jj = _mm256_sub_epi32(_mm256_add_epi32(outer, mid20), side5);
    return _mm256_srai_epi32(_mm256_add_epi32(jj, _mm256_set1_epi32(512)), 10);
}

S264_AVX2 void halfPelRowAvx2(const uint8_t* const* r, int W, int* vi, uint8_t* vb, uint8_t* hb, int& x, int& hEnd)
{
    for (; x + 16 <= W; x += 16) {
        const __m256i vv = halfTap16(halfLd16(r[0] + x), halfLd16(r[1] + x), halfLd16(r[2] + x), halfLd16(r[3] + x),
                                     halfLd16(r[4] + x), halfLd16(r[5] + x));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(vi + x), _mm256_cvtepi16_epi32(_mm256_castsi256_si128(vv)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(vi + x + 8), _mm256_cvtepi16_epi32(_mm256_extracti128_si256(vv, 1)));
        halfStore8(vb + x, vv);
    }
    const uint8_t* row = r[2];
    for (; hEnd + 19 <= W; hEnd += 16) {
        const uint8_t* q = row + hEnd;
        halfStore8(hb + hEnd, halfTap16(halfLd16(q - 2), halfLd16(q - 1), halfLd16(q), halfLd16(q + 1), halfLd16(q + 2),
                                        halfLd16(q + 3)));
    }
}

S264_AVX2 void halfPelCentreAvx2(const int* v, int W, uint8_t* j, int& jEnd)
{
    for (; jEnd + 19 <= W; jEnd += 16) {
        const __m256i w = _mm256_permute4x64_epi64(_mm256_packs_epi32(halfTap32(v, jEnd), halfTap32(v, jEnd + 8)), 0xD8);
        const __m256i b = _mm256_permute4x64_epi64(_mm256_packus_epi16(w, w), 0xD8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(j + jEnd), _mm256_castsi256_si128(b));
    }
}
#undef S264_AVX2
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

#ifdef SCHNEIDI_H264_NEON
// NEON parts of refHalfRows (16 samples per step, the same numbers): the vertical 6-tap (32-bit
// intermediate + rounded plane) and the horizontal 6-tap of one row, continuing at x / hEnd; the
// centre 6-tap over the intermediate, continuing at jEnd. 16-bit lanes hold -2550..10710.
static inline int16x8_t halfTapN(uint8x8_t a, uint8x8_t b, uint8x8_t c, uint8x8_t d, uint8x8_t e, uint8x8_t f)
{
    const int16x8_t outer = vreinterpretq_s16_u16(vaddl_u8(a, f));
    const int16x8_t mid = vreinterpretq_s16_u16(vaddl_u8(c, d)), side = vreinterpretq_s16_u16(vaddl_u8(b, e));
    return vmlsq_n_s16(vmlaq_n_s16(outer, mid, 20), side, 5);
}
static void halfPelRowNeon(const uint8_t* const* r, int W, int* vi, uint8_t* vb, uint8_t* hb, int& x, int& hEnd)
{
    for (; x + 8 <= W; x += 8) {
        const int16x8_t vv = halfTapN(vld1_u8(r[0] + x), vld1_u8(r[1] + x), vld1_u8(r[2] + x), vld1_u8(r[3] + x),
                                      vld1_u8(r[4] + x), vld1_u8(r[5] + x));
        vst1q_s32(vi + x, vmovl_s16(vget_low_s16(vv)));
        vst1q_s32(vi + x + 4, vmovl_high_s16(vv));
        vst1_u8(vb + x, vqrshrun_n_s16(vv, 5)); // clip1((v + 16) >> 5)
    }
    const uint8_t* row = r[2];
    for (; hEnd + 19 <= W; hEnd += 16) {
        const uint8_t* q = row + hEnd;
        const uint8x16_t m2 = vld1q_u8(q - 2), m1 = vld1q_u8(q - 1), p0 = vld1q_u8(q), p1 = vld1q_u8(q + 1);
        const uint8x16_t p2 = vld1q_u8(q + 2), p3 = vld1q_u8(q + 3);
        const int16x8_t lo = halfTapN(vget_low_u8(m2), vget_low_u8(m1), vget_low_u8(p0), vget_low_u8(p1), vget_low_u8(p2),
                                      vget_low_u8(p3));
        const int16x8_t hi = halfTapN(vget_high_u8(m2), vget_high_u8(m1), vget_high_u8(p0), vget_high_u8(p1),
                                      vget_high_u8(p2), vget_high_u8(p3));
        vst1q_u8(hb + hEnd, vcombine_u8(vqrshrun_n_s16(lo, 5), vqrshrun_n_s16(hi, 5)));
    }
}
static void halfPelCentreNeon(const int* v, int W, uint8_t* j, int& jEnd)
{
    auto tap = [&](int x) { // 32-bit lanes: up to about 450000
        const int32x4_t outer = vaddq_s32(vld1q_s32(v + x - 2), vld1q_s32(v + x + 3));
        const int32x4_t mid = vaddq_s32(vld1q_s32(v + x), vld1q_s32(v + x + 1));
        const int32x4_t side = vaddq_s32(vld1q_s32(v + x - 1), vld1q_s32(v + x + 2));
        return vqmovn_s32(vrshrq_n_s32(vmlsq_n_s32(vmlaq_n_s32(outer, mid, 20), side, 5), 10)); // (j + 512) >> 10
    };
    for (; jEnd + 11 <= W; jEnd += 8)
        vst1_u8(j + jEnd, vqmovun_s16(vcombine_s16(tap(jEnd), tap(jEnd + 4))));
}
#endif

void FrameCoder::refHalfRows(RefPic& R, int y0, int y1) const
{
    const int M = kMargin;
    const int W = extW_, H = extH_;
    (void)M;
    (void)H;
    auto tap6 = [](int a, int b, int c, int d, int e, int f) { return a - 5 * b + 20 * c + 20 * d - 5 * e + f; };
    std::vector<int> vRow(size_t(W) + 8);
    for (int y = y0; y < y1; ++y) {
        const uint8_t* r[6];
        for (int k = 0; k < 6; ++k)
            r[k] = &R.F[size_t(std::clamp(y - 2 + k, 0, H - 1)) * W];
        const uint8_t* row = r[2];
        int* vi = vRow.data(); // vertical 6-tap intermediate of this row (unclipped)
        uint8_t* vb = &R.V[size_t(y) * W];
        uint8_t* hb = &R.B[size_t(y) * W];
        int x = 0, hEnd = 2;
#ifdef SCHNEIDI_H264_AVX2
        if (haveAvx2())
            halfPelRowAvx2(r, W, vi, vb, hb, x, hEnd);
#endif
#ifdef SCHNEIDI_H264_SSE2
        // Same arithmetic eight samples at a time (16-bit lanes hold -2550..10710).
        {
            const __m128i zero = _mm_setzero_si128(), c5 = _mm_set1_epi16(5), c20 = _mm_set1_epi16(20);
            const __m128i r16 = _mm_set1_epi16(16);
            auto ld = [&](const uint8_t* p) {
                return _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p)), zero);
            };
            auto tap = [&](__m128i a, __m128i b, __m128i c, __m128i d, __m128i e, __m128i f) {
                const __m128i outer = _mm_add_epi16(a, f), mid = _mm_add_epi16(c, d), side = _mm_add_epi16(b, e);
                return _mm_sub_epi16(_mm_add_epi16(outer, _mm_mullo_epi16(mid, c20)), _mm_mullo_epi16(side, c5));
            };
            for (; x + 8 <= W; x += 8) {
                const __m128i vv = tap(ld(r[0] + x), ld(r[1] + x), ld(r[2] + x), ld(r[3] + x), ld(r[4] + x), ld(r[5] + x));
                const __m128i sign = _mm_srai_epi16(vv, 15);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(vi + x), _mm_unpacklo_epi16(vv, sign));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(vi + x + 4), _mm_unpackhi_epi16(vv, sign));
                const __m128i v8 = _mm_srai_epi16(_mm_add_epi16(vv, r16), 5);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(vb + x), _mm_packus_epi16(v8, v8));
            }
            for (; hEnd + 11 <= W; hEnd += 8) {
                const uint8_t* q = row + hEnd;
                const __m128i bb = tap(ld(q - 2), ld(q - 1), ld(q), ld(q + 1), ld(q + 2), ld(q + 3));
                const __m128i b8 = _mm_srai_epi16(_mm_add_epi16(bb, r16), 5);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(hb + hEnd), _mm_packus_epi16(b8, b8));
            }
        }
#elif defined(SCHNEIDI_H264_NEON)
        halfPelRowNeon(r, W, vi, vb, hb, x, hEnd);
#endif
        for (; x < W; ++x) {
            const int vv = tap6(r[0][x], r[1][x], r[2][x], r[3][x], r[4][x], r[5][x]);
            vi[x] = vv;
            vb[x] = clip1((vv + 16) >> 5);
        }
        for (x = 0; x < W; ++x) {
            if (x == 2)
                x = hEnd; // [2, hEnd) done above
            if (x >= W)
                break;
            int bb;
            if (x >= 2 && x + 3 < W)
                bb = tap6(row[x - 2], row[x - 1], row[x], row[x + 1], row[x + 2], row[x + 3]);
            else
                bb = tap6(row[std::clamp(x - 2, 0, W - 1)], row[std::clamp(x - 1, 0, W - 1)], row[x],
                          row[std::min(x + 1, W - 1)], row[std::min(x + 2, W - 1)], row[std::min(x + 3, W - 1)]);
            hb[x] = clip1((bb + 16) >> 5);
        }
        // Centre half-pel: horizontal 6-tap over the vertical intermediate, same row.
        const int* v = vRow.data();
        uint8_t* j = &R.J[size_t(y) * W];
        int jEnd = 2;
#ifdef SCHNEIDI_H264_AVX2
        if (haveAvx2())
            halfPelCentreAvx2(v, W, j, jEnd);
#endif
#ifdef SCHNEIDI_H264_SSE2
        {
            auto ld = [&](int x) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(v + x)); };
            auto tap = [&](int x) { // 32-bit lanes: up to about 450000
                const __m128i outer = _mm_add_epi32(ld(x - 2), ld(x + 3));
                const __m128i mid = _mm_add_epi32(ld(x), ld(x + 1)), side = _mm_add_epi32(ld(x - 1), ld(x + 2));
                const __m128i mid20 = _mm_add_epi32(_mm_slli_epi32(mid, 4), _mm_slli_epi32(mid, 2));
                const __m128i side5 = _mm_add_epi32(_mm_slli_epi32(side, 2), side);
                const __m128i jj = _mm_sub_epi32(_mm_add_epi32(outer, mid20), side5);
                return _mm_srai_epi32(_mm_add_epi32(jj, _mm_set1_epi32(512)), 10);
            };
            for (; jEnd + 11 <= W; jEnd += 8) {
                const __m128i w = _mm_packs_epi32(tap(jEnd), tap(jEnd + 4));
                _mm_storel_epi64(reinterpret_cast<__m128i*>(j + jEnd), _mm_packus_epi16(w, w));
            }
        }
#elif defined(SCHNEIDI_H264_NEON)
        halfPelCentreNeon(v, W, j, jEnd);
#endif
        for (int x = 0; x < W; ++x) {
            if (x == 2)
                x = jEnd; // [2, jEnd) done above
            if (x >= W)
                break;
            int jj;
            if (x >= 2 && x + 3 < W)
                jj = tap6(v[x - 2], v[x - 1], v[x], v[x + 1], v[x + 2], v[x + 3]);
            else
                jj = tap6(v[std::clamp(x - 2, 0, W - 1)], v[std::clamp(x - 1, 0, W - 1)], v[x],
                          v[std::min(x + 1, W - 1)], v[std::min(x + 2, W - 1)], v[std::min(x + 3, W - 1)]);
            j[x] = clip1((jj + 512) >> 10);
        }
    }
}

// Luma prediction of a w x h block at integer position (x0, y0) plus quarter offset (fx, fy),
// table 8-12. Output stride is 16.
// Weighted prediction, deciding (experiment knobs): the weighted block means must leave at most
// H264_WPTHR of the unweighted difference to the reference and gain at least H264_WPMIN grey levels
// per block on average.
static const double gWpThr = std::getenv("H264_WPTHR") ? std::atof(std::getenv("H264_WPTHR")) : 0.8;
static const double gWpMin = std::getenv("H264_WPMIN") ? std::atof(std::getenv("H264_WPMIN")) : 0.25;
static const bool gWpTrace = std::getenv("H264_WP_TRACE") != nullptr;

void FrameCoder::computeBlockMeans()
{
    const int cw = padW_ / 2;
    dcY_.resize(size_t(mbW_) * mbH_);
    dcU_.resize(dcY_.size());
    dcV_.resize(dcY_.size());
    for (int mby = 0; mby < mbH_; ++mby)
        for (int mbx = 0; mbx < mbW_; ++mbx) {
            int sum = 0;
            for (int y = 0; y < 16; ++y) {
                const uint8_t* row = &srcY_[size_t(mby * 16 + y) * padW_ + mbx * 16];
                int rs = 0;
                for (int x = 0; x < 16; ++x)
                    rs += row[x];
                sum += rs;
            }
            int su = 0, sv = 0;
            for (int y = 0; y < 8; ++y) {
                const uint8_t* ru = &srcU_[size_t(mby * 8 + y) * cw + mbx * 8];
                const uint8_t* rv = &srcV_[size_t(mby * 8 + y) * cw + mbx * 8];
                for (int x = 0; x < 8; ++x) {
                    su += ru[x];
                    sv += rv[x];
                }
            }
            const size_t m = size_t(mby) * mbW_ + mbx;
            dcY_[m] = float(sum) / 256.0f;
            dcU_[m] = float(su) / 64.0f;
            dcV_[m] = float(sv) / 64.0f;
        }
}

namespace {

// Mean and mean absolute deviation of block means.
void meanAndSpread(const std::vector<float>& v, double& mean, double& spread)
{
    double s = 0;
    for (float x : v)
        s += x;
    mean = s / double(std::max<size_t>(1, v.size()));
    double d = 0;
    for (float x : v)
        d += std::abs(x - mean);
    spread = d / double(std::max<size_t>(1, v.size()));
}

// Sum of |cur - clip(ref * w + o)| over the block means.
double weightedDiff(const std::vector<float>& cur, const std::vector<float>& ref, double w, double o)
{
    double s = 0;
    for (size_t i = 0; i < cur.size(); ++i)
        s += std::abs(double(cur[i]) - std::clamp(double(ref[i]) * w + o, 0.0, 255.0));
    return s;
}

struct WeightFit {
    bool use = false;
    double w = 1.0, o = 0.0; // as fitted, before the denominator is known
    double mc = 0, mr = 0;
};

// Gain and offset that map the reference's block means on this picture's: gain from the spreads,
// offset from the means; or an offset alone. The better one, if it explains the difference.
WeightFit fitWeights(const std::vector<float>& cur, const std::vector<float>& ref, double thr, double minGain)
{
    WeightFit f;
    if (cur.empty() || cur.size() != ref.size())
        return f;
    double sc, sr;
    meanAndSpread(cur, f.mc, sc);
    meanAndSpread(ref, f.mr, sr);
    const double plain = weightedDiff(cur, ref, 1.0, 0.0);
    double best = plain;
    if (sr > 0.25 && sc > 0.25) {
        const double w = std::clamp(sc / sr, 1.0 / 16.0, 16.0);
        const double d = weightedDiff(cur, ref, w, f.mc - w * f.mr);
        if (d < best) {
            best = d;
            f.w = w;
            f.o = f.mc - w * f.mr;
        }
    }
    const double d1 = weightedDiff(cur, ref, 1.0, f.mc - f.mr);
    if (d1 < best * 0.98) { // an offset alone, when it does about as well: no rounding of the gain
        best = d1;
        f.w = 1.0;
        f.o = f.mc - f.mr;
    }
    f.use = best <= thr * plain && plain - best >= minGain * double(cur.size());
    return f;
}

} // namespace

// Explicit weighted prediction for this P picture: per reference a gain and an offset for luma
// and for the two chroma planes, where the block means of the two source pictures differ by such a
// law (a fade, a flash) rather than by motion. One denominator per picture for luma, one for
// chroma: the finest that still holds the largest gain (weights are 8 bit signed).
void FrameCoder::planWeights()
{
    const size_t n = refs_.size();
    wp_.assign(n, WeightParams());
    wpAny_ = false;
    std::vector<WeightFit> fy(n), fu(n), fv(n);
    double maxLuma = 0, maxChroma = 0;
    for (size_t i = 0; i < n; ++i) {
        const RefPic& R = *refs_[i];
        fy[i] = fitWeights(dcY_, R.dcY, gWpThr, gWpMin);
        if (fy[i].use)
            maxLuma = std::max(maxLuma, fy[i].w);
        // Chroma only together with luma (a fade moves all three), both planes with their own fit.
        if (!fy[i].use)
            continue;
        fu[i] = fitWeights(dcU_, R.dcU, 1.0, 0.0);
        fv[i] = fitWeights(dcV_, R.dcV, 1.0, 0.0);
        const double plain = weightedDiff(dcU_, R.dcU, 1.0, 0.0) + weightedDiff(dcV_, R.dcV, 1.0, 0.0);
        const double fitted = weightedDiff(dcU_, R.dcU, fu[i].w, fu[i].o) + weightedDiff(dcV_, R.dcV, fv[i].w, fv[i].o);
        fu[i].use = fv[i].use = fitted <= gWpThr * plain && plain - fitted >= 0.5 * gWpMin * double(dcU_.size());
        if (fu[i].use)
            maxChroma = std::max({maxChroma, fu[i].w, fv[i].w});
    }
    if (maxLuma <= 0)
        return;
    auto denomFor = [](double maxW) {
        int d = 7;
        while (d > 0 && std::lround(maxW * double(1 << d)) > 127)
            --d;
        return d;
    };
    wpLumaLog_ = denomFor(maxLuma);
    wpChromaLog_ = maxChroma > 0 ? denomFor(maxChroma) : 0;
    auto quantise = [](const WeightFit& f, int log, int& weight, int& offset) {
        weight = int(std::clamp<long>(std::lround(f.w * double(1 << log)), -128, 127));
        offset = int(std::clamp<long>(std::lround(f.mc - f.mr * double(weight) / double(1 << log)), -128, 127));
    };
    for (size_t i = 0; i < n; ++i) {
        const RefPic& R = *refs_[i];
        WeightParams& w = wp_[i];
        if (!fy[i].use)
            continue;
        quantise(fy[i], wpLumaLog_, w.lumaWeight, w.lumaOffset);
        // The rounded numbers must still pass (a large offset is cut at 8 bits).
        const double plain = weightedDiff(dcY_, R.dcY, 1.0, 0.0);
        const double got = weightedDiff(dcY_, R.dcY, double(w.lumaWeight) / double(1 << wpLumaLog_), w.lumaOffset);
        w.luma = got <= gWpThr * plain && plain - got >= gWpMin * double(dcY_.size()) &&
                 (w.lumaWeight != (1 << wpLumaLog_) || w.lumaOffset != 0);
        if (!w.luma)
            continue;
        wpAny_ = true;
        if (fu[i].use) {
            quantise(fu[i], wpChromaLog_, w.chromaWeight[0], w.chromaOffset[0]);
            quantise(fv[i], wpChromaLog_, w.chromaWeight[1], w.chromaOffset[1]);
            w.chroma = true;
        }
        if (gWpTrace)
            std::fprintf(stderr, "wp: frame %d ref %zu luma %d/%d %+d (means %.1f <- %.1f, diff %.2f -> %.2f)%s\n", displayIndex_, i,
                         w.lumaWeight, 1 << wpLumaLog_, w.lumaOffset, fy[i].mc, fy[i].mr, plain / double(dcY_.size()),
                         got / double(dcY_.size()), w.chroma ? " +chroma" : "");
    }
}

// Luma prediction of a block (rows 16 apart in out), with the picture's weight for this reference
// applied when it has one (8.4.2.3: after the interpolation).
void FrameCoder::mcLuma(const RefPic& R, int x0, int y0, int fx, int fy, int w, int h, uint8_t* out) const
{
    mcLumaPlain(R, x0, y0, fx, fy, w, h, out);
    const WeightParams* wp = weightsOf(R);
    if (!wp || !wp->luma)
        return;
    const int round = wpLumaLog_ >= 1 ? 1 << (wpLumaLog_ - 1) : 0;
    const int lw = wp->lumaWeight, lo = wp->lumaOffset, sh = wpLumaLog_;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = out + y * 16;
        for (int x = 0; x < w; ++x)
            row[x] = uint8_t(std::clamp(((row[x] * lw + round) >> sh) + lo, 0, 255));
    }
}

void FrameCoder::mcLumaPlain(const RefPic& R, int x0, int y0, int fx, int fy, int w, int h, uint8_t* out) const
{
    const int M = kMargin;
    const int W = extW_;
    const size_t base = size_t(y0 + M) * W + size_t(x0 + M);
    // Each quarter position is one plane or the rounded average of two (table 8-12):
    // pick both sources once per block so the row loops stay simple.
    const uint8_t* a = nullptr;
    const uint8_t* b = nullptr;
    const uint8_t* F = R.F.data() + base;
    const uint8_t* B = R.B.data() + base;
    const uint8_t* V = R.V.data() + base;
    const uint8_t* J = R.J.data() + base;
    switch (fy * 4 + fx) {
    case 0: a = F; break;
    case 1: a = F; b = B; break;
    case 2: a = B; break;
    case 3: a = F + 1; b = B; break;
    case 4: a = F; b = V; break;
    case 5: a = B; b = V; break;
    case 6: a = B; b = J; break;
    case 7: a = B; b = V + 1; break;
    case 8: a = V; break;
    case 9: a = V; b = J; break;
    case 10: a = J; break;
    case 11: a = J; b = V + 1; break;
    case 12: a = F + W; b = V; break;
    case 13: a = V; b = B + W; break;
    case 14: a = J; b = B + W; break;
    default: a = V + 1; b = B + W; break;
    }
#ifdef SCHNEIDI_H264_SSE2
    if (b && (w == 16 || w == 8)) {
        for (int y = 0; y < h; ++y) {
            const uint8_t* ra = a + size_t(y) * W;
            const uint8_t* rb = b + size_t(y) * W;
            if (w == 16)
                _mm_storeu_si128(reinterpret_cast<__m128i*>(out + y * 16),
                                 _mm_avg_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ra)),
                                              _mm_loadu_si128(reinterpret_cast<const __m128i*>(rb))));
            else
                _mm_storel_epi64(reinterpret_cast<__m128i*>(out + y * 16),
                                 _mm_avg_epu8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(ra)),
                                              _mm_loadl_epi64(reinterpret_cast<const __m128i*>(rb))));
        }
        return;
    }
#elif defined(SCHNEIDI_H264_NEON)
    if (w == 16 || w == 8) { // averages, or plain copies without a library call per row
        for (int y = 0; y < h; ++y) {
            const uint8_t* ra = a + size_t(y) * W;
            const uint8_t* rb = b ? b + size_t(y) * W : ra;
            if (w == 16)
                vst1q_u8(out + y * 16, vrhaddq_u8(vld1q_u8(ra), vld1q_u8(rb)));
            else
                vst1_u8(out + y * 16, vrhadd_u8(vld1_u8(ra), vld1_u8(rb)));
        }
        return;
    }
#endif
    for (int y = 0; y < h; ++y) {
        const uint8_t* ra = a + size_t(y) * W;
        uint8_t* o = out + y * 16;
        if (b) {
            const uint8_t* rb = b + size_t(y) * W;
            for (int x = 0; x < w; ++x)
                o[x] = uint8_t((ra[x] + rb[x] + 1) >> 1);
        } else {
            std::memcpy(o, ra, size_t(w));
        }
    }
}

// SAD (full-pel fast path) or SATD of a partition against the reference.
int FrameCoder::sadPart(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy, bool satd) const
{
    const int px = mbx * 16 + x4 * 4, py = mby * 16 + y4 * 4;
    const int w = w4 * 4, h = h4 * 4;
    const size_t lo = size_t(py) * padW_ + px;
    int s = 0;
    const WeightParams* wp = weightsOf(R);
    const bool weighted = wp && wp->luma; // then every position goes through the weighted block
    if (!satd && !(mvx & 3) && !(mvy & 3) && !weighted) {
        const uint8_t* ref = &R.F[size_t(py + (mvy >> 2) + kMargin) * extW_ + size_t(px + (mvx >> 2) + kMargin)];
        return sadBlock(&srcY_[lo], padW_, ref, extW_, w, h);
    }
    uint8_t buf[256];
    const uint8_t* pred = buf;
    int ps = 16;
    // Full and half positions are one plane: read it in place instead of copying the block.
    const int fx = mvx & 3, fy = mvy & 3;
    if (!(fx & 1) && !(fy & 1) && !weighted) {
        const RefPic& P = R;
        const std::vector<uint8_t>& plane = fy ? (fx ? P.J : P.V) : (fx ? P.B : P.F);
        pred = plane.data() + size_t(py + (mvy >> 2) + kMargin) * extW_ + size_t(px + (mvx >> 2) + kMargin);
        ps = extW_;
    } else {
        mcLuma(R, px + (mvx >> 2), py + (mvy >> 2), fx, fy, w, h, buf);
    }
    if (satd) {
#ifdef SCHNEIDI_H264_AVX2
        if ((w4 == 4 || !(h4 & 1)) && haveAvx2())
            return satdBlockAvx2(&srcY_[lo], padW_, pred, ps, w, h);
#endif
#ifdef SCHNEIDI_H264_NEON
        return neon_detail::satdBlock(&srcY_[lo], padW_, pred, ps, w, h);
#endif
        for (int by = 0; by < h4; ++by) {
            if (w4 == 4) {
                s += satd16x4U8(&srcY_[lo + size_t(by * 4) * padW_], padW_, pred + by * 4 * ps, ps);
                continue;
            }
            if (!(by & 1) && by + 1 < h4) { // 8 wide (partitions are at least 8x8): two strips at once
                s += satd8x8U8Fast(&srcY_[lo + size_t(by * 4) * padW_], padW_, pred + by * 4 * ps, ps);
                ++by;
                continue;
            }
            for (int bx = 0; bx < w4; bx += 2)
                s += satd8x4U8(&srcY_[lo + size_t(by * 4) * padW_ + bx * 4], padW_, pred + by * 4 * ps + bx * 4, ps);
        }
        return s;
    }
    return sadBlock(&srcY_[lo], padW_, pred, ps, w, h);
}

namespace {

// Table 8-16 (alpha', beta') and 8-17 (tC0 by bS 1..3), by indexA / indexB.
constexpr uint8_t kAlpha[52] = {0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   0,   0,   0,   4,   4,
                                5,  6,  7,  8,  9,  10, 12, 13, 15, 17, 20, 22, 25,  28,  32,  36,  40,  45,
                                50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255};
constexpr uint8_t kBeta[52] = {0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  2,  2,
                               2, 3, 3, 3, 3, 4, 4, 4, 6,  6,  7,  7,  8,  8,  9,  9,  10, 10,
                               11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18};
constexpr uint8_t kTc0[52][3] = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 1},
    {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1},
    {1, 1, 2}, {1, 1, 2}, {1, 1, 2}, {1, 1, 2}, {1, 2, 3}, {1, 2, 3}, {2, 2, 3}, {2, 2, 4}, {2, 3, 4},
    {2, 3, 4}, {3, 3, 5}, {3, 4, 6}, {3, 4, 6}, {4, 5, 7}, {4, 5, 8}, {4, 6, 9}, {5, 7, 10}, {6, 8, 11},
    {6, 8, 13}, {7, 10, 14}, {8, 11, 16}, {9, 12, 18}, {10, 13, 20}, {11, 15, 23}, {13, 17, 25},
};

} // namespace

// Boundary strength between two 4x4 luma blocks (8.7.2.1), positions in 4x4 units.
int FrameCoder::boundaryStrength(int pbx, int pby, int qbx, int qby, bool mbEdge) const
{
    const int lw = mbW_ * 4;
    const size_t mp = size_t(pby) * lw + pbx, mq = size_t(qby) * lw + qbx;
    if (bSlice_) {
        // One reference picture per list, two different pictures: the blocks must use the same
        // lists and their vectors per list must be closer than one sample.
        const bool p0 = refIdx_[mp] >= 0, p1 = refIdx1_[mp] >= 0, q0 = refIdx_[mq] >= 0, q1 = refIdx1_[mq] >= 0;
        if (!(p0 || p1) || !(q0 || q1))
            return mbEdge ? 4 : 3;
        if (nzLuma_[mp] || nzLuma_[mq])
            return 2;
        if (p0 != q0 || p1 != q1)
            return 1;
        if (p0 && (std::abs(mvX_[mp] - mvX_[mq]) >= 4 || std::abs(mvY_[mp] - mvY_[mq]) >= 4))
            return 1;
        if (p1 && (std::abs(mvX1_[mp] - mvX1_[mq]) >= 4 || std::abs(mvY1_[mp] - mvY1_[mq]) >= 4))
            return 1;
        return 0;
    }
    if (refIdx_[mp] < 0 || refIdx_[mq] < 0)
        return mbEdge ? 4 : 3;
    if (nzLuma_[size_t(pby) * lw + pbx] || nzLuma_[size_t(qby) * lw + qbx])
        return 2;
    if (refIdx_[mp] != refIdx_[mq] || std::abs(mvX_[mp] - mvX_[mq]) >= 4 || std::abs(mvY_[mp] - mvY_[mq]) >= 4)
        return 1;
    return 0;
}

void FrameCoder::deblockPicture()
{
    // Wavefront: macroblock x of a row may be filtered once the row above has finished
    // macroblocks up to x + 1 (whose edges touch the samples shared with it). That keeps
    // the raster-order result of 8.7 exactly.
    const int threads = std::clamp(cfg_.threads, 1, mbH_);
    std::vector<std::atomic<int>> done(static_cast<size_t>(mbH_));
    for (auto& d : done)
        d.store(0, std::memory_order_relaxed);
    std::atomic<int> nextRow{0};
    auto worker = [&] {
        for (;;) {
            const int mby = nextRow.fetch_add(1);
            if (mby >= mbH_)
                return;
            for (int mbx = 0; mbx < mbW_; ++mbx) {
                if (mby > 0) {
                    const int need = std::min(mbx + 2, mbW_);
                    while (done[size_t(mby - 1)].load(std::memory_order_acquire) < need)
                        spinWait();
                }
                deblockMb(mbx, mby);
                done[size_t(mby)].store(mbx + 1, std::memory_order_release);
            }
        }
    };
    runOnThreads(threads, worker);
}

void FrameCoder::deblockMb(int mbx, int mby)
{
    const int cw = padW_ / 2;
    {
        {
            // Filter strength per edge from the average QP of both sides (8.7.2.2).
            const int qpQ = mbQpFinal_[size_t(mby) * mbW_ + mbx];
            const int qpLeft = mbx > 0 ? mbQpFinal_[size_t(mby) * mbW_ + mbx - 1] : qpQ;
            const int qpTop = mby > 0 ? mbQpFinal_[size_t(mby - 1) * mbW_ + mbx] : qpQ;
            struct EdgeQp {
                int indexY, alphaY, betaY, indexC, alphaC, betaC;
            };
            auto edgeQp = [&](int qpP) {
                const int y = (qpP + qpQ + 1) >> 1, ch = (chromaQp(qpP + cfg_.chromaQpOffset) + chromaQp(qpQ + cfg_.chromaQpOffset) + 1) >> 1;
                if (gDeblockA == 0 && gDeblockB == 0)
                    return EdgeQp{y, kAlpha[y], kBeta[y], ch, kAlpha[ch], kBeta[ch]};
                // Slice offsets (8.7.2.2): indexA (alpha, tC0) and indexB (beta) move separately.
                const int ya = std::clamp(y + 2 * gDeblockA, 0, 51), yb = std::clamp(y + 2 * gDeblockB, 0, 51);
                const int ca = std::clamp(ch + 2 * gDeblockA, 0, 51), cb = std::clamp(ch + 2 * gDeblockB, 0, 51);
                return EdgeQp{ya, kAlpha[ya], kBeta[yb], ca, kAlpha[ca], kBeta[cb]};
            };
            const EdgeQp inner = edgeQp(qpQ), leftEdge = edgeQp(qpLeft), topEdge = edgeQp(qpTop);
            int bsV[4][4], bsH[4][4]; // [edge][segment]
            // A macroblock predicted in one piece without luma coefficients (most skipped ones) has
            // no inner edge to filter: boundaryStrength would say 0 for each of them.
            bool uniform = !bSlice_;
            if (uniform) {
                const int lw = mbW_ * 4;
                const size_t o0 = size_t(mby * 4) * lw + mbx * 4;
                const int ref0 = refIdx_[o0], vx0 = mvX_[o0], vy0 = mvY_[o0];
                uniform = ref0 >= 0;
                for (int y = 0; y < 4 && uniform; ++y)
                    for (int x = 0; x < 4; ++x) {
                        const size_t o = o0 + size_t(y) * lw + x;
                        if (nzLuma_[o] || refIdx_[o] != ref0 || mvX_[o] != vx0 || mvY_[o] != vy0) {
                            uniform = false;
                            break;
                        }
                    }
            }
            for (int e = 0; e < 4; ++e)
                for (int s = 0; s < 4; ++s) {
                    if (uniform && e > 0) {
                        bsV[e][s] = bsH[e][s] = 0;
                        continue;
                    }
                    const int bx = mbx * 4 + e, by = mby * 4 + s;
                    bsV[e][s] = bx == 0 ? 0 : boundaryStrength(bx - 1, by, bx, by, e == 0);
                    const int hx = mbx * 4 + s, hy = mby * 4 + e;
                    bsH[e][s] = hy == 0 ? 0 : boundaryStrength(hx, hy - 1, hx, hy, e == 0);
                }
            // Luma: vertical edges left to right, then horizontal edges top to bottom
            // (with the 8x8 transform only edges 0 and 2). Every line of an edge is independent of the
            // others, so a kernel filters a whole edge at once; the edges keep their order.
            const bool t8 = cabWave_.t8[size_t(mby) * mbW_ + mbx] != 0;
            // bS per segment -> kernel edge; false = nothing to filter. An intra side on a macroblock
            // edge gives bS 4 to every segment (macroblocks are intra or inter as a whole).
            auto makeEdge = [](const int* bs, int index, int alpha, int beta, DeblockEdge& out) {
                if (!(bs[0] | bs[1] | bs[2] | bs[3]) || alpha == 0 || beta == 0)
                    return false;
                out.alpha = alpha;
                out.beta = beta;
                out.bs4 = bs[0] == 4;
                for (int s = 0; s < 4; ++s)
                    out.tc0[s] = bs[s] == 0 ? int8_t(-1) : bs[s] == 4 ? int8_t(0) : int8_t(kTc0[index][bs[s] - 1]);
                return true;
            };
            DeblockEdge ed;
            uint8_t* const y0 = &recY_[size_t(mby * 16) * padW_ + mbx * 16];
            for (int e = 0; e < 4; ++e) {
                if (t8 && (e & 1))
                    continue;
                const EdgeQp& eq = e == 0 ? leftEdge : inner;
                if (makeEdge(bsV[e], eq.indexY, eq.alphaY, eq.betaY, ed))
                    deblockLumaEdgeV(y0 + e * 4, padW_, ed);
            }
            for (int e = 0; e < 4; ++e) {
                if (t8 && (e & 1))
                    continue;
                const EdgeQp& eq = e == 0 ? topEdge : inner;
                if (makeEdge(bsH[e], eq.indexY, eq.alphaY, eq.betaY, ed))
                    deblockLumaEdgeH(y0 + size_t(e * 4) * padW_, padW_, ed);
            }
            // Chroma: edges 0 and 4 use the strength of luma edges 0 and 2; Cb and Cr share the QP.
            uint8_t* const u0 = &recU_[size_t(mby * 8) * cw + mbx * 8];
            uint8_t* const v0 = &recV_[size_t(mby * 8) * cw + mbx * 8];
            for (int ce = 0; ce < 2; ++ce) {
                const EdgeQp& eq = ce == 0 ? leftEdge : inner;
                if (makeEdge(bsV[ce * 2], eq.indexC, eq.alphaC, eq.betaC, ed))
                    deblockChromaEdgeV(u0 + ce * 4, v0 + ce * 4, cw, ed);
            }
            for (int ce = 0; ce < 2; ++ce) {
                const EdgeQp& eq = ce == 0 ? topEdge : inner;
                if (makeEdge(bsH[ce * 2], eq.indexC, eq.alphaC, eq.betaC, ed))
                    deblockChromaEdgeH(u0 + size_t(ce * 4) * cw, v0 + size_t(ce * 4) * cw, cw, ed);
            }
        }
    }
}


// Adaptive quantisation: flat macroblocks (where errors show) get a finer QP, busy ones a
// coarser QP; the offsets average to zero so the frame QP keeps its meaning.
void FrameCoder::planMacroblockQp()
{
    const int n = mbW_ * mbH_;
    const bool tree = treeOffset_.size() == size_t(n);
    if (cfg_.aqStrength <= 0 || cfg_.mode == IntraMode::Pcm) {
        std::fill(mbEnergy_.begin(), mbEnergy_.end(), 99.0f); // not measured: every macroblock counts as textured
        if (!tree) {
            std::fill(mbQp_.begin(), mbQp_.end(), int8_t(cfg_.qp));
            return;
        }
        for (int m = 0; m < n; ++m)
            mbQp_[size_t(m)] = int8_t(std::clamp(int(std::lround(cfg_.qp + qpFrac_ + treeOffset_[size_t(m)])), 0, 51));
        return;
    }
    double mean = 0;
    for (int mby = 0; mby < mbH_; ++mby)
        for (int mbx = 0; mbx < mbW_; ++mbx) {
            int64_t sum = 0, sq = 0;
#ifdef SCHNEIDI_H264_SSE2
            {
                const __m128i zero = _mm_setzero_si128();
                __m128i s = zero, q = zero; // 64-bit sums / 32-bit sums of squares (at most 256 * 255^2)
                for (int y = 0; y < 16; ++y) {
                    const __m128i v =
                        _mm_loadu_si128(reinterpret_cast<const __m128i*>(&srcY_[size_t(mby * 16 + y) * padW_ + mbx * 16]));
                    s = _mm_add_epi64(s, _mm_sad_epu8(v, zero));
                    const __m128i lo = _mm_unpacklo_epi8(v, zero), hi = _mm_unpackhi_epi8(v, zero);
                    q = _mm_add_epi32(q, _mm_add_epi32(_mm_madd_epi16(lo, lo), _mm_madd_epi16(hi, hi)));
                }
                q = _mm_add_epi32(q, _mm_shuffle_epi32(q, _MM_SHUFFLE(1, 0, 3, 2)));
                q = _mm_add_epi32(q, _mm_shuffle_epi32(q, _MM_SHUFFLE(2, 3, 0, 1)));
                sum = _mm_cvtsi128_si32(s) + _mm_cvtsi128_si32(_mm_unpackhi_epi64(s, s));
                sq = _mm_cvtsi128_si32(q);
            }
#else
            for (int y = 0; y < 16; ++y) {
                const uint8_t* row = &srcY_[size_t(mby * 16 + y) * padW_ + mbx * 16];
                for (int x = 0; x < 16; ++x) {
                    sum += row[x];
                    sq += row[x] * row[x];
                }
            }
#endif
            double variance = double(sq - sum * sum / 256);
            if (gAqEdge) {
                // Experiment: texture measured inside the four 8x8 quadrants, so a step between two
                // flat areas (a bright edge against dark water) does not count as busy texture.
                double v[4];
                for (int q = 0; q < 4; ++q) {
                    int64_t qs = 0, qq = 0;
                    for (int y = 0; y < 8; ++y) {
                        const uint8_t* row = &srcY_[size_t(mby * 16 + (q >> 1) * 8 + y) * padW_ + mbx * 16 + (q & 1) * 8];
                        for (int x = 0; x < 8; ++x) {
                            qs += row[x];
                            qq += row[x] * row[x];
                        }
                    }
                    v[q] = 4.0 * double(qq - qs * qs / 64); // on the 256-pixel scale
                }
                std::sort(v, v + 4);
                variance = gAqEdge == 1 ? (v[0] + v[1] + v[2] + v[3]) / 4 : (v[0] + v[1]) / 2;
            }
            // Below kAqFloor all macroblocks count as equally flat for the offsets: a variance of 0.004 against
            // 0.03 (a band edge of a smooth gradient running through the macroblock or not) is no difference
            // in what shows, but 3 energy steps = 2 QP of bits moved between them. Measured two-pass, a smooth
            // gradient at 0.5 Mbit/s / a cartoon at 2 Mbit/s (PSNR-Y, the gradient's bits): off 62.24 dB at 595
            // kbit/s / 38.71 dB; 2: 62.44 at 560 / -0.01 dB; 3: 62.65 at 560 / -0.01; 4: 62.86 at 548 / -0.04;
            // 6: 63.07 at 540 / -0.30 (the cartoon's flat areas lose what its busy ones gain).
            // H264_AQFLOOR overrides (0 = off, the stream before 16.11.2026).
            static const double kAqFloor = std::getenv("H264_AQFLOOR") ? std::atof(std::getenv("H264_AQFLOOR")) : 3.0;
            const double e = std::log2(variance + 1.0);
            mbEnergy_[size_t(mby) * mbW_ + mbx] = float(e);
            mean += std::max(e, kAqFloor);
        }
    mean /= n;
    // Constant quality: the offsets centre on a fixed energy instead of the picture's mean, so a picture's level
    // follows its texture as well. Centred on the mean, a cartoon (mostly flat, mean energy ~2) had its few
    // busy macroblocks - where its bits go - pushed up to 8 QP coarser and the cartoon came out
    // ~6 QP coarser against x264 than game footage; calm grain (~15) and game footage (~13) hardly move. H264_CRFAQB: weight (0 = the mean as in the other
    // modes), H264_CRFAQREF: the energy (measuring).
    static const double kCrfAqB = std::getenv("H264_CRFAQB") ? std::atof(std::getenv("H264_CRFAQB")) : 1.0;
    static const double kCrfAqRef = std::getenv("H264_CRFAQREF") ? std::atof(std::getenv("H264_CRFAQREF")) : 13.0;
    if (cfg_.crf >= 0 && cfg_.bitrateKbps <= 0)
        mean += kCrfAqB * (kCrfAqRef - mean);
    static const bool aqTrace = std::getenv("H264_AQ_TRACE") != nullptr;
    if (aqTrace) {
        double tm = 0;
        if (treeOffset_.size() == size_t(n))
            for (int m = 0; m < n; ++m)
                tm += treeOffset_[size_t(m)];
        std::fprintf(stderr, "aq: frame %d qp %d mean %.2f tree %.2f\n", displayIndex_, cfg_.qp, mean, tm / n);
    }
    for (int m = 0; m < n; ++m) {
        static const double kAqFloor2 = std::getenv("H264_AQFLOOR") ? std::atof(std::getenv("H264_AQFLOOR")) : 3.0;
        double offset = std::clamp(cfg_.aqStrength * (std::max(double(mbEnergy_[size_t(m)]), kAqFloor2) - mean) * 0.5, -8.0, 8.0);
        if (tree)
            offset += treeOffset_[size_t(m)];
        mbQp_[size_t(m)] = int8_t(std::clamp(int(std::lround(cfg_.qp + qpFrac_ + offset)), 0, 51));
    }
}


// Scene change: on quarter-size thumbnails, compare the best small-motion SAD of each 8x8
// block with its SAD against its own mean (a crude intra cost). When motion no longer helps
// for most of the picture, a keyframe is cheaper and looks better than a P frame.
bool FrameCoder::detectSceneCut(std::vector<uint8_t>& prevThumb_)
{
    const int tw = padW_ / 4, th = padH_ / 4;
    std::vector<uint8_t> thumb(size_t(tw) * th);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            int s = 0;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 4; ++dx)
                    s += srcY_[size_t(y * 4 + dy) * padW_ + x * 4 + dx];
            thumb[size_t(y) * tw + x] = uint8_t((s + 8) >> 4);
        }
    bool cut = false;
    constexpr int kRange = 8; // thumbnail pixels = 32 picture pixels
    if (prevThumb_.size() == thumb.size()) {
        int64_t interCost = 0, intraCost = 0;
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
                int best = 1 << 30;
                for (int my = -kRange; my <= kRange; ++my)
                    for (int mx = -kRange; mx <= kRange; ++mx) {
                        if (by + my < 0 || bx + mx < 0 || by + my + 8 > th || bx + mx + 8 > tw)
                            continue;
                        best = std::min(best, sadBlock(&thumb[size_t(by) * tw + bx], tw, &prevThumb_[size_t(by + my) * tw + bx + mx], tw, 8, 8));
                    }
                interCost += std::min(best, intra);
                intraCost += intra + 16; // a little bias so flat pictures do not count as cuts
            }
        // Real cuts measured ~0.92-0.95, fast game motion up to ~0.72.
        cut = interCost * 100 > intraCost * 85;
    }
    prevThumb_ = std::move(thumb);
    return cut;
}


// True when the residual against this prediction would quantise to zero everywhere (inter
// rounding), luma 4x4 blocks and chroma DC/AC alike.
bool FrameCoder::skipResidualIsZero(int mbx, int mby, const uint8_t* lp, const uint8_t (*cp)[64], int qp) const
{
    // A coefficient quantises to zero while |c| * mf + offset < 2^qbits: the largest such |c| per
    // position class, laid out as the two row patterns of a 4x4 block (classes 0 2 0 2 / 2 1 2 1).
    auto limits = [](int q, int* lim) {
        const int qbits = 15 + q / 6, offset = (1 << qbits) / 6;
        int byClass[3];
        for (int c = 0; c < 3; ++c) {
            const int mf = kQuantMf[q % 6][c];
            byClass[c] = ((1 << qbits) - offset + mf - 1) / mf - 1;
        }
        static constexpr int kClass[8] = {0, 2, 0, 2, 2, 1, 2, 1};
        for (int i = 0; i < 8; ++i)
            lim[i] = byClass[kClass[i]];
    };
    int lim[8];
    limits(qp, lim);
    const size_t lumaOff = size_t(mby * 16) * padW_ + mbx * 16;
    for (int b = 0; b < 16; ++b) {
        const int ox = (b & 3) * 4, oy = (b >> 2) * 4;
        int coef[16];
        forward4x4Diff(&srcY_[lumaOff + size_t(oy) * padW_ + ox], padW_, lp + oy * 16 + ox, 16, coef);
        if (anyAbove4x4(coef, lim))
            return false;
    }
    const int qpc = chromaQp(qp + cfg_.chromaQpOffset);
    const int cDiv = qpc / 6, cMod = qpc % 6, cqbits = 15 + cDiv, cOffset = (1 << cqbits) / 6;
    int limC[8];
    limits(qpc, limC);
    const int cw = padW_ / 2;
    const size_t co = size_t(mby * 8) * cw + mbx * 8;
    for (int c = 0; c < 2; ++c) {
        const std::vector<uint8_t>& src = c ? srcV_ : srcU_;
        int dc[4];
        for (int b = 0; b < 4; ++b) {
            const int ox = (b & 1) * 4, oy = (b >> 1) * 4;
            int coef[16];
            forward4x4Diff(&src[co + size_t(oy) * cw + ox], cw, cp[c] + oy * 8 + ox, 8, coef);
            dc[b] = coef[0];
            coef[0] = 0; // the DC goes through its own transform below
            if (anyAbove4x4(coef, limC))
                return false;
        }
        const int f[4] = {dc[0] + dc[1] + dc[2] + dc[3], dc[0] - dc[1] + dc[2] - dc[3], dc[0] + dc[1] - dc[2] - dc[3],
                          dc[0] - dc[1] - dc[2] + dc[3]};
        for (int v : f)
            if (quantize(v, kQuantMf[cMod][0], cqbits + 1, 2 * cOffset) != 0)
                return false;
    }
    return true;
}


// Bits of a candidate macroblock: the CAVLC count, or with CABAC a dry run on a copy of the
// slice's current context states.
double FrameCoder::mbBits(const MbOut& o, int mbx, int mby, double cavlcBits)
{
    const size_t top = size_t(rowKey_[size_t(mby)]);
    const CabacEncoder* live = cabacByTop_[top];
    if (!live)
        return cavlcBits;
    CabacCounter probe(*live);
    bool prev = *prevQpNzByTop_[top];
    cabacWriteMb(probe, cabWave_, mbx, mby, o.syn, prev);
    return probe.countedBits();
}

double FrameCoder::skipBits(int mbx, int mby)
{
    const size_t top = size_t(rowKey_[size_t(mby)]);
    const CabacEncoder* live = cabacByTop_[top];
    if (!live)
        return 1.0;
    CabacCounter probe(*live);
    MbSyntax skip;
    bool prev = false;
    cabacWriteMb(probe, cabWave_, mbx, mby, skip, prev);
    return probe.countedBits();
}


// A CAVLC residual block, or with CABAC only its coefficient count (the CAVLC bits are not used then).
int FrameCoder::residual(BitWriter& bw, const int* coeffs, int count, int nC) const
{
    if (!useCabac())
        return writeResidualBlock(bw, coeffs, count, nC);
    int total = 0;
    for (int i = 0; i < count; ++i)
        total += coeffs[i] != 0;
    return total;
}

} // namespace schneidi::h264
