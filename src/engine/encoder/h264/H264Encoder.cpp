// schneidi's own H.264 encoder (prototype).
// Clause numbers refer to ITU-T H.264. The encoder mirrors the decoder's
// reconstruction exactly (8.3, 8.5), so later P frames can reference it.
#include "H264Encoder.h"
#include "FrameCoder.h"

#include "Cabac.h"
#include "Cavlc.h"
#include "Satd.h"
#include "Transform.h"
#include "ThreadErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <climits>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <condition_variable>
#include <mutex>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace schneidi::h264 {

// H264Cabac.cpp
double cabacTrellis(const CabacEncoder& ce, int cat, const double* x, const double* scale, int count, double lambda,
                    int cbfInc, int* out, double* energy = nullptr);

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
// Experiments (H264_EXP bit mask), for A/B measurements only.
const int gExp = std::getenv("H264_EXP") ? std::atoi(std::getenv("H264_EXP")) : 0;
// Per thread (rows of a picture run on several threads): intra try in a P picture without Intra
// 8x8 / 16x16 (experiments).
thread_local bool tIntraSkip8 = false, tIntraSkip16 = false;
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
// 1.5 -> 1.2 (12.11., with the pre-selection below): see gIntraSel.
const double gIntraFactor = std::getenv("H264_INTRAF") ? std::atof(std::getenv("H264_INTRAF")) : gFullIntra ? 2.0 : 1.2;
// B pictures: 16x8 and 8x16 partitions besides 16x16 (H264_BPARTS=0 turns them off, for measurements).
const bool gBParts = !std::getenv("H264_BPARTS") || std::atoi(std::getenv("H264_BPARTS")) != 0;
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

// Chroma QP from table 8-15 (chroma_qp_index_offset = 0).
int chromaQp(int qp)
{
    static const int kTable[22] = {29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36,
                                   36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39};
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
#if defined(__SSE2__) || defined(_M_X64)
        _mm_pause();
#endif
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
#if defined(__SSE2__) || defined(_M_X64)
    _mm_pause();
#endif
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
    mbW_ = (cfg_.width + 15) / 16;
    mbH_ = (cfg_.height + 15) / 16;
    padW_ = mbW_ * 16;
    padH_ = mbH_ * 16;
    if (cfg_.mode == IntraMode::Pcm)
        cfg_.deblock = false;
    // B pictures need CABAC and P pictures; their run and its reference picture must fit the queue.
    cfg_.bframes = useB() ? std::min(cfg_.bframes, 8) : 0;
    // (the newest picture is analysed against the two before it while older ones leave the queue)
    if (useB())
        cfg_.lookahead = std::max(cfg_.lookahead, cfg_.bframes + 4);
    for (auto& c : coders_)
        c = std::make_unique<FrameCoder>(cfg_);
    idrCount_ = cfg_.idrPicIdBase;
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
    const int64_t dpb = std::max(bPictures ? 2 : 1, cfg.refs);
    for (const Level& l : kLevels) {
        const int64_t side = int64_t(std::sqrt(double(l.maxFs) * 8.0)); // A.3.1: width and height in macroblocks
        if (double(fs) * fps > double(l.maxMbps) * 1.0001 || fs > l.maxFs || mbW > side || mbH > side)
            continue;
        if (fs * dpb > l.maxDpbMbs)
            continue;
        if (cfg.bitrateKbps > 0 && cfg.bitrateKbps * 1.5 * 1000.0 > double(l.maxBr) * brFactor)
            continue;
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
        const uint32_t refFrames = uint32_t(std::max(useB() ? 2 : 1, cfg_.refs));
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
        bw.flag(false);  // nal_hrd_parameters_present_flag
        bw.flag(false);  // vcl_hrd_parameters_present_flag
        bw.flag(false);  // pic_struct_present_flag
        bw.flag(true);   // bitstream_restriction_flag: no reordering, so decoders output at once
        bw.flag(true);   // motion_vectors_over_pic_boundaries_flag
        bw.ue(0);        // max_bytes_per_pic_denom
        bw.ue(0);        // max_bits_per_mb_denom
        bw.ue(16);       // log2_max_mv_length_horizontal
        bw.ue(16);       // log2_max_mv_length_vertical
        bw.ue(useB() ? 1 : 0); // max_num_reorder_frames (B pictures: the reference picture after them comes first)
        bw.ue(refFrames);      // max_dec_frame_buffering (at least max_num_ref_frames)
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
        bw.se(0);       // chroma_qp_index_offset
        bw.flag(true);  // deblocking_filter_control_present_flag
        bw.flag(false); // constrained_intra_pred_flag
        bw.flag(false); // redundant_pic_cnt_present_flag
        if (useT8()) {
            bw.flag(true);  // transform_8x8_mode_flag
            bw.flag(false); // pic_scaling_matrix_present_flag
            bw.se(0);       // second_chroma_qp_index_offset
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

FirstPass::FirstPass(int width, int height, int threads, bool forB, size_t keepAnalysisBytes)
    : width_(width), height_(height), threads_(std::max(1, threads)),
      la_(std::make_unique<Lookahead>(width, height, (width + 15) / 16, (height + 15) / 16)),
      stats_(std::make_shared<FirstPassStats>()), forB_(forB), keepBytes_(keepAnalysisBytes)
{
    // H264_KEEPANALYSIS=0: the encoder analyses every picture again (for measurements).
    if (const char* e = std::getenv("H264_KEEPANALYSIS"))
        keepBytes_ = std::atoi(e) ? keepBytes_ : 0;
}

FirstPass::~FirstPass() = default;

void FirstPass::add(const Picture& pic, bool forceKeyframe)
{
    LookaheadFrame f; // the analysis reads only the luma
    f.forceKeyframe = forceKeyframe;
    f.y.resize(size_t(width_) * height_);
    for (int y = 0; y < height_; ++y)
        std::memcpy(f.y.data() + size_t(y) * width_, pic.y + size_t(y) * pic.strideY, size_t(width_));
    batch_.push_back(std::move(f));
    if (int(batch_.size()) >= 4 * threads_)
        analyseBatch();
}

// Half pictures, thumbnails and scene changes of the whole batch side by side, then the costs
// in order (each against its predecessor, the last one of the batch before for the first).
void FirstPass::analyseBatch() const
{
    const int n = int(batch_.size());
    if (!n)
        return;
    auto parallel = [&](auto&& fn) {
        std::atomic<int> next{0};
        runOnThreads(std::min(threads_, n), [&] {
            for (int i; (i = next.fetch_add(1)) < n;)
                fn(i);
        });
    };
    auto prevOf = [&](int i) { return i > 0 ? &batch_[size_t(i) - 1] : havePrev_ ? &prev_ : nullptr; };
    parallel([&](int i) {
        failurePoint(FailPoint::FirstPassWorker);
        la_->prepare(batch_[size_t(i)]);
    });
    parallel([&](int i) { la_->sceneCut(batch_[size_t(i)], prevOf(i)); });
    // The motion search starts from the predecessor's vectors: in order.
    for (int i = 0; i < n; ++i)
        la_->costs(batch_[size_t(i)], prevOf(i), threads_);
    // Kept from the start without gaps (the encoder's analysis of a picture needs its predecessor's).
    for (int i = 0; i < n; ++i) {
        const LookaheadFrame& f = batch_[size_t(i)];
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
    if (forB_) {
        auto prev2Of = [&](int i) { return i > 1 ? &batch_[size_t(i) - 2] : i == 1 ? (havePrev_ ? &prev_ : nullptr) : havePrev2_ ? &prev2_ : nullptr; };
        parallel([&](int i) {
            if (prev2Of(i))
                batch_[size_t(i)].bScore = la_->bScore(batch_[size_t(i)], *prevOf(i), *prev2Of(i));
        });
        for (const LookaheadFrame& f : batch_)
            stats_->bScore.push_back(f.bScore);
        if (n >= 2) {
            prev2_ = std::move(batch_[size_t(n) - 2]);
            havePrev2_ = true;
        } else {
            prev2_ = std::move(prev_);
            havePrev2_ = havePrev_;
        }
        prev2_.y = {};
    }
    for (LookaheadFrame& f : batch_) {
        stats_->intraSum.push_back(f.intraSum);
        stats_->interSum.push_back(f.interSum);
        stats_->sceneChange.push_back(f.sceneChange ? 1 : 0);
        stats_->forceKeyframe.push_back(f.forceKeyframe ? 1 : 0);
    }
    prev_ = std::move(batch_.back());
    prev_.y = {};
    havePrev_ = true;
    batch_.clear();
}

std::shared_ptr<const FirstPassStats> FirstPass::stats() const
{
    analyseBatch();
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
    if (cfg.bitrateKbps <= 0)
        return plan;
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
            if (!st.bScore.empty())
                part->bScore.push_back(st.bScore[i]);
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
    if (size_t(i) < bitrateKbps.size()) {
        c.bitrateKbps = bitrateKbps[size_t(i)];
        c.firstPass = firstPass[size_t(i)];
    }
    return c;
}

// QP for the P pictures at which the pictures not coded yet (first-pass costs, k per picture type
// from the pictures coded so far) spend exactly the bits that are left of the clip's budget.
double Encoder::twoPassQp(double kI, double kP, double kB) const
{
    const FirstPassStats& st = *cfg_.firstPass;
    const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
    const double left = double(st.size()) - frameIndex_;
    const double budget = std::max(double(st.size()) * perFrame - rcBits_, 0.5 * left * perFrame);
    double need = kI * tpRemI_ * std::exp2(0.5) + kP * tpRemP_;
    if (tpRemB_ > 0)
        need += kB * tpRemB_ * std::exp2(-cfg_.bQpOffset / 6.0);
    return 6.0 * std::log2(std::max(1.0, need) / budget);
}

int Encoder::delay() const { return (useLookahead() ? std::max(0, cfg_.lookahead - 1) : 0) + (pipelined() ? 1 : 0); }

// Workers of the frame threading. Rows are not waited in: a worker takes a row that can go on (of
// the oldest picture first, from the top), runs it until it would have to wait (for the row above,
// the entropy writer, the reference picture) and parks it there; whatever lets a parked row go on
// wakes a sleeping worker (WaveSignal). So a worker only sleeps when no row of any picture in
// flight can move.
struct Encoder::Pool {
    WaveSignal sig; // its mutex also guards active and stop
    std::vector<std::thread> threads;
    FrameCoder* active[2] = {nullptr, nullptr}; // oldest first
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
                    if (a && (r = a->claimRunnableRow()) >= 0) {
                        c = a;
                        break;
                    }
            }
            if (c) {
                c->waveRowRun(r); // catches its own failures
                continue;
            }
            // Nothing can move: a short spin, then sleep until something changed (the timeout only
            // bounds the damage of a wake-up that should ever be missed).
            for (int i = 0; i < spin && sig.epoch.load(std::memory_order_relaxed) == epoch; ++i) {
#if defined(__SSE2__) || defined(_M_X64)
                _mm_pause();
#endif
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
            (active[0] ? active[1] : active[0]) = c;
        }
        sig.wake(true);
    }
    void remove(FrameCoder* c)
    {
        std::lock_guard<std::mutex> l(sig.m);
        if (active[0] == c) {
            active[0] = active[1];
            active[1] = nullptr;
        } else if (active[1] == c) {
            active[1] = nullptr;
        }
        c->wave_.signal = nullptr;
    }
};

Encoder::~Encoder()
{
    if (pendingThread_.joinable())
        pendingThread_.join();
    if (inFlight_) {
        inFlight_->waveEnd();
        pool_->remove(inFlight_);
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
    }
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
    const bool scoreB = bAdapt() && !cfg_.firstPass; // the adaptive B placement measures here
    // Once the queue is full the newest picture joins it only at the next call (analysed in the
    // background with several threads), so the encoded picture sees the same queue with any
    // thread count. Its predecessor stays in the queue while a picture is popped.
    if (full && queue_.size() >= 2) {
        pendingFrame_ = std::make_unique<LookaheadFrame>(std::move(f));
        const LookaheadFrame* prev = &queue_.back();
        const LookaheadFrame* prev2 = scoreB ? &queue_[queue_.size() - 2] : nullptr;
        if (cfg_.threads > 1) {
            pendingThread_ = std::thread([this, prev, prev2] {
                try {
                    failurePoint(FailPoint::LookaheadThread);
                    analyseFrame(*pendingFrame_, prev);
                    if (prev2)
                        pendingFrame_->bScore = la_->bScore(*pendingFrame_, *prev, *prev2);
                } catch (...) {
                    pendingError_ = std::current_exception();
                }
            });
        } else {
            analyseFrame(*pendingFrame_, prev);
            if (prev2)
                pendingFrame_->bScore = la_->bScore(*pendingFrame_, *prev, *prev2);
        }
    } else {
        analyseFrame(f, queue_.empty() ? nullptr : &queue_.back());
        if (scoreB && queue_.size() >= 2)
            f.bScore = la_->bScore(f, queue_.back(), queue_[queue_.size() - 2]);
        queue_.push_back(std::move(f));
    }
    if (!full)
        return {};
    if (useB())
        return codeNext();
    std::vector<uint8_t> out = step(&queue_.front());
    queue_.pop_front();
    return out;
}

// Types of the queued pictures that are not decided yet, in display order: keyframes as the
// schedule, the caller and the scene changes ask for them, then runs of up to `bframes` B pictures
// each followed by a P picture. The picture before a keyframe and the last one are P pictures (a
// B picture needs a later reference of its own GOP). With `decide` the first run and its
// reference picture become final.
// Threshold of the adaptive B placement: a run of n B pictures needs every picture after its
// first one, up to the reference picture that ends it, to score below kBScoreMax[n]. Measured
// (fixed QP, BD-rate PSNR-Y against no B pictures; slow zoom with grain / fast game): 0.55 -46 % /
// 0 %, 0.57 -50 % / +2 %, 0.59 -52 % / +7 %, always three -53 % / +18 %. H264_BT1..3 override.
static const double kBScoreMax[4] = {
    0,
    std::getenv("H264_BT1") ? std::atof(std::getenv("H264_BT1")) : 0.57,
    std::getenv("H264_BT2") ? std::atof(std::getenv("H264_BT2")) : 0.57,
    std::getenv("H264_BT3") ? std::atof(std::getenv("H264_BT3")) : 0.57,
};
// Longest run of B pictures (up to maxB) that may start at a picture; score(k) is the bScore of
// the k-th picture after it (negative: unknown, the run ends before it).
template <class Score>
static int bRunLimit(int maxB, Score&& score)
{
    int limit = 0;
    double worst = 0;
    for (int k = 1; k <= maxB; ++k) {
        const double s = score(k);
        if (s < 0)
            break;
        worst = std::max(worst, s);
        if (worst < kBScoreMax[std::min(k, 3)])
            limit = k;
    }
    return limit;
}

double Encoder::bScoreOf(const LookaheadFrame& f) const
{
    if (cfg_.firstPass)
        return size_t(f.index) < cfg_.firstPass->bScore.size() ? cfg_.firstPass->bScore[size_t(f.index)] : -1.0;
    return f.bScore;
}

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
    int pos = gopPos_, run = 0, runLimit = cfg_.bframes;
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
            if (run == 0 && adapt)
                runLimit = bRunLimit(cfg_.bframes, [&](int k) { return j + size_t(k) < queue_.size() ? bScoreOf(queue_[j + size_t(k)]) : -1.0; });
            if (run < runLimit && !last && !keyAt(j + 1, pos + 1)) {
                f.type = 2;
                ++run;
            } else {
                f.type = 1;
                run = 0;
            }
        }
        f.gopPos = pos++;
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
        for (LookaheadFrame& f : queue_) {
            f.decided = true;
            if (trace)
                std::fprintf(stderr, "btype: picture %d %c score %.3f\n", f.index, "IPB"[f.type], bScoreOf(f));
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
        if (f.type != 2)
            return f.coded ? &front : &f;
    return &front; // not reached: a run of B pictures always has its reference picture
}

std::vector<uint8_t> Encoder::codeNext()
{
    LookaheadFrame* f = pickNext();
    std::vector<uint8_t> out = step(f);
    if (f) {
        f->coded = true;
        while (!queue_.empty() && queue_.front().coded)
            queue_.pop_front();
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
    std::vector<uint8_t> out;
    if (inFlight_) {
        try {
            out = finishFrame(*inFlight_);
        } catch (...) {
            inFlight_ = started; // the destructor ends it
            throw;
        }
    }
    inFlight_ = started;
    return out;
}

void Encoder::applyRc(int upTo)
{
    while (!rcDone_.empty() && rcDone_.front().index <= upTo) {
        const RcRecord r = rcDone_.front();
        rcDone_.pop_front();
        if (r.lookahead) {
            if (r.cost > 0) {
                rcKSum_[r.type] += r.bits * std::exp2(r.qp / 6.0);
                rcCostSum_[r.type] += r.cost;
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
    std::vector<uint8_t> out = std::move(fc.done_);
    fc.done_.clear();
    last_ = &fc;
    lastQp_ = fc.outQp_;
    lastWasSceneCut_ = fc.outSceneCut_;
    lastDisplayIndex_ = fc.displayIndex_;
    lastType_ = "IPB"[fc.rcType_];
    if (cfg_.bitrateKbps > 0)
        rcDone_.push_back({fc.frameNo_, fc.rcType_, fc.rcLookahead_, fc.rcCost_, fc.outQp_, double(out.size()) * 8.0});
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
double Encoder::frameCost(const LookaheadFrame& f, bool intra) const
{
    const size_t n = size_t(mbW_) * mbH_;
    if (f.intraCost.size() != n)
        return 0;
    double sum = 0;
    for (size_t m = 0; m < n; ++m) {
        const double c = intra ? f.intraCost[m] : std::min(f.intraCost[m], f.interCost[m]);
        if (cfg_.mbtree) {
            const float off = std::max(-8.0f, Lookahead::qpOffset(f, int(m), cfg_.mbtreeStrength));
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
    bw.ue(0);                                      // pic_parameter_set_id
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
        bw.flag(false); // ref_pic_list_modification_flag_l0
        if (bSlice_)
            bw.flag(false); // ref_pic_list_modification_flag_l1
    }
    if (idr_) {
        bw.flag(false); // no_output_of_prior_pics_flag
        bw.flag(false); // long_term_reference_flag
    } else {
        if (refPic_)
            bw.flag(false); // adaptive_ref_pic_marking_mode_flag
        if (useCabac())
            bw.ue(0); // cabac_init_idc
    }
    bw.se(cfg_.qp - 26);         // slice_qp_delta
    bw.ue(cfg_.deblock ? 0 : 1); // disable_deblocking_filter_idc (0 filters across slice edges too)
    if (cfg_.deblock) {
        bw.se(0); // slice_alpha_c0_offset_div2
        bw.se(0); // slice_beta_offset_div2
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
            if (bSlice_)
                encodeMbB(mb, mbx, r);
            else if (pSlice_)
                encodeMbP(mb, mbx, r);
            else
                commitQp(mbx, r, encodeMbIntra(mb, mbx, r));
            cabacWriteMb(eng, cabWave_, mbx, r, mb.syn, w.rowPrevNz[size_t(r)]);
            mbSyn_[size_t(r) * mbW_ + mbx] = mb.syn;
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
    if (!R.ref.empty()) { // the row's motion, for the B pictures that use this picture
        const size_t o = size_t(k) * 4 * (size_t(mbW_) * 4), n = 4 * (size_t(mbW_) * 4);
        std::memcpy(&R.mvx[o], &mvX_[o], n * sizeof(int16_t));
        std::memcpy(&R.mvy[o], &mvY_[o], n * sizeof(int16_t));
        std::memcpy(&R.ref[o], &refIdx_[o], n);
    }
    raiseTo(R.rowsReady, k + 1);
    if (w.signal)
        w.signal->wake(true); // rows of the next picture, and the row that publishes the next band
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

FrameCoder* Encoder::startFrame(LookaheadFrame& frame)
{
    const bool forceKeyframe = frame.forceKeyframe;
    const Picture picture = pictureOf(frame, cfg_.width);
    PhaseClock clock;
    FrameCoder& fc = *coders_[size_t(nextCoder_)];
    nextCoder_ = (nextCoder_ + 1) % 3;
    fc.loadSource(picture);
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
    if (idr)
        refList_.clear(); // an IDR picture marks all references unused
    fc.pSlice_ = !idr;
    fc.bSlice_ = type == 2;
    fc.refPic_ = type != 2;
    fc.pocLsb_ = bMode;
    fc.gopPos_ = frame.gopPos;
    if (type == 2) {
        // The reference pictures around it in display order: the one before (list 0) and the
        // one after, which was coded just before this run of B pictures (list 1).
        fc.refs_.assign(1, refList_[1]);
        fc.refs1_.assign(1, refList_[0]);
        fc.setBiWeights();
    } else {
        fc.refs_.assign(refList_.begin(), refList_.begin() + std::min<ptrdiff_t>(ptrdiff_t(refList_.size()), std::max(1, cfg_.refs)));
        fc.refs1_.clear();
    }
    fc.activeRefs_ = int(fc.refs_.size());
    // With B pictures the picture coded before may still be in flight and is no reference: the
    // start points come from the reference picture's motion instead.
    fc.prev_ = bMode ? nullptr : lastStarted_;
    lastStarted_ = &fc;

    // Frame-level rate control (in the spirit of x264's one-pass ABR, aiming at a steady QP, which
    // is what PSNR likes best): a P picture gets the QP at which the average complexity of all P
    // pictures so far (bits * 2^(qp/6); bits roughly halve per +6 QP) meets the per-frame budget,
    // scaled by the bits over or under the target (overflow against a buffer of 5 s of bits) and at
    // most 2 away from the last one. I pictures use the P QP - 3. The first picture is probed at a
    // fixed QP so the stream starts near the right QP instead of paying for a huge first picture.
    const int baseQp = cfg_.qp;
    applyRc(frameIndex_ - 2);
    const bool lookahead = lookaheadFrame;
    double costUsed = 0; // lookahead cost of this picture, for the bit model afterwards
    if (lookahead) {
        if (bMode)
            planQueue(); // also the pictures that joined since the decision
        else
            markKeyframes();
        frame.keyframe = idr;
        if (type == 2) {
            fc.treeOffset_.clear(); // nothing references a B picture
        } else if (cfg_.mbtree) {
            la_->propagate(queue_);
            fc.treeOffset_.resize(size_t(mbW_) * mbH_);
            for (size_t m = 0; m < fc.treeOffset_.size(); ++m)
                fc.treeOffset_[m] = std::max(-8.0f, Lookahead::qpOffset(frame, int(m), cfg_.mbtreeStrength));
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
            for (const LookaheadFrame& g : queue_) {
                if (g.coded && &g != &frame)
                    continue; // a reference picture coded ahead of its turn
                const int t = g.keyframe ? 0 : bMode && g.type == 2 ? 2 : 1;
                const double c = t == 2 ? g.interSum : frameCost(g, g.keyframe);
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
            // Keyframes as the encoder will place them (schedule, caller, scene cuts).
            const FirstPassStats& st = *cfg_.firstPass;
            tpKey_.assign(st.size(), 0);
            tpType_.assign(st.size(), 1);
            tpRemI_ = tpRemP_ = tpRemB_ = 0;
            int since = 0;
            for (size_t i = 0; i < st.size(); ++i) {
                const bool key = i == 0 || since >= cfg_.gop || st.forceKeyframe[i] || (cfg_.sceneCut && st.sceneChange[i] && since >= 6);
                if (key)
                    since = 0;
                ++since;
                tpKey_[i] = key;
            }
            // Picture types as planQueue will place them.
            const bool adapt = bAdapt();
            int run = 0, runLimit = cfg_.bframes;
            for (size_t i = 0; i < st.size(); ++i) {
                if (run == 0 && adapt && !tpKey_[i])
                    runLimit = bRunLimit(cfg_.bframes, [&](int k) { return i + size_t(k) < st.bScore.size() ? st.bScore[i + size_t(k)] : -1.0; });
                if (tpKey_[i]) {
                    tpType_[i] = 0;
                    run = 0;
                } else if (bMode && run < runLimit && i + 1 < st.size() && !tpKey_[i + 1]) {
                    tpType_[i] = 2;
                    ++run;
                } else {
                    run = 0;
                }
                (tpType_[i] == 0 ? tpRemI_ : tpType_[i] == 2 ? tpRemB_ : tpRemP_) += tpKey_[i] ? st.intraSum[i] : st.interSum[i];
            }
        }
        auto steadyQp = [&] {
            if (twoPass) {
                for (const LookaheadFrame& g : queue_)
                    if (&g == &frame)
                        costUsed = g.keyframe ? g.intraSum : g.interSum;
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
                pc.lookahead = 0;
                pc.bframes = 0;
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
                rcKSum_[1] = kI;
                rcCostSum_[1] = 1;
                if (queue_.size() > 1) {
                    const LookaheadFrame& second = queue_[1];
                    const double bitsP = double(probe.encode(pictureOf(second, cfg_.width)).size()) * 8.0;
                    rcKSum_[1] = bitsP * std::exp2(probeQp / 6.0) / std::max(1.0, second.interSum);
                }
                const double predicted = steadyQp();
                if (std::getenv("H264_RC_TRACE"))
                    std::fprintf(stderr, "rc: probe at %d predicts %.1f\n", probeQp, predicted);
                if (std::abs(predicted - probeQp) <= 4.0)
                    break;
                probeQp = std::clamp(int(std::lround(std::clamp(predicted, probeQp - 6.0, probeQp + 6.0))), 10, 51);
            }
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
            qp = twoPassQp(kOf(true), kOf(false), kB());
            const double step = rcFramesSeen_ < 12 ? 2.0 : 0.5;
            if (rcQp_ > 0)
                qp = std::clamp(qp, rcQp_ - step, rcQp_ + step);
            const FirstPassStats& st = *cfg_.firstPass;
            const size_t i = std::min(size_t(frame.index), st.size() - 1);
            (tpType_[i] == 0 ? tpRemI_ : tpType_[i] == 2 ? tpRemB_ : tpRemP_) -= tpKey_[i] ? st.intraSum[i] : st.interSum[i];
            if (std::getenv("H264_RC_TRACE"))
                std::fprintf(stderr, "rc2: frame %d kI %.3f kP %.3f qp %.2f\n", frameIndex_, kOf(true), kOf(false), qp);
        } else if (rcQp_ > 0) {
            qp = std::clamp(qp, rcQp_ - 3.0, rcQp_ + 3.0);
        }
        rcQp_ = std::clamp(qp, 10.0, 51.0);
        cfg_.qp = std::clamp(int(std::lround(idr ? qp - 3 : type == 2 ? qp + cfg_.bQpOffset : qp)), 10, 51);
        rcNeedSeen_ += kType(type) * costUsed * typeScale(type);
        rcFramesSeen_ += 1;
    } else if (cfg_.bitrateKbps > 0) {
        const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
        if (frameIndex_ == 0) {
            // The probe aims at an I picture of about four P budgets.
            EncoderConfig pc = cfg_;
            pc.bitrateKbps = 0;
            pc.lookahead = 0;
            pc.bframes = 0;
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
    } else if (idr && cfg_.gop > 1) {
        cfg_.qp = std::clamp(baseQp - 3, 0, 51); // keyframes are references for the whole GOP
    } else if (type == 2) {
        cfg_.qp = std::clamp(baseQp + cfg_.bQpOffset, 0, 51); // nothing depends on a B picture
    }
    fc.cfg_.qp = cfg_.qp;
    fc.mbTypeOffset_ = idr ? 0 : 5;
    fc.frameNo_ = frameIndex_;
    fc.displayIndex_ = frame.index;
    fc.outQp_ = cfg_.qp;
    fc.outSceneCut_ = cut;
    fc.rcType_ = type;
    fc.rcLookahead_ = lookahead;
    fc.rcCost_ = costUsed;

    clock.lap(2); // keyframe decision, macroblock tree, rate control (probe on the first picture)
    fc.planMacroblockQp();
    clock.lap(3);
    // Sliding window (8.2.5.3): the newest picture goes first, the oldest drops out.
    std::shared_ptr<RefPic> pic;
    if (cfg_.gop > 1 && type != 2) {
        if (int(refList_.size()) >= std::max(bMode ? 2 : 1, cfg_.refs)) {
            pic = std::move(refList_.back());
            refList_.pop_back();
        }
        if (!pic || pic.use_count() > 1)
            pic = std::make_shared<RefPic>();
        pic->gopPos = frame.gopPos;
        refList_.push_front(pic);
    }
    // frame_num: reference pictures since the IDR picture (without B pictures: all pictures).
    const int frameNum = bMode ? (idr ? 0 : refCount_) : framesSinceIdr_;
    if (bMode && type != 2)
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
                pic->mvx = fc.mvX_;
                pic->mvy = fc.mvY_;
                pic->ref = fc.refIdx_;
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
};

const IntraDirTable<4>& intraDir4()
{
    static const IntraDirTable<4> t;
    return t;
}
const IntraDirTable<8>& intraDir8()
{
    static const IntraDirTable<8> t;
    return t;
}

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

    const int qpc = chromaQp(mbQp_[size_t(mby) * mbW_ + mbx]);
    const int qpcDiv = qpc / 6, qpcMod = qpc % 6;
    const int cqbits = 15 + qpcDiv;
    const int cOffset = (1 << cqbits) / (interPred ? 6 : 3);
    bool anyDc = false, anyAc = false;
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
            anyAc |= quant4x4(ccoef[blk], kRows.mf[qpcMod], cqbits, cOffset, kMaxLevel, ch.ac[c][blk]);
        }
    }
    ch.cbp = anyAc ? 2 : anyDc ? 1 : 0;

    for (int c = 0; c < 2; ++c) {
        const int* l = ch.dc[c];
        const int f[4] = {l[0] + l[1] + l[2] + l[3], l[0] - l[1] + l[2] - l[3], l[0] + l[1] - l[2] - l[3],
                          l[0] - l[1] - l[2] + l[3]};
        uint8_t* r0 = &(*crec[c])[chromaOff];
        for (int blk = 0; blk < 4; ++blk) {
            int d[16], r[16];
            dequant4x4(ch.ac[c][blk], kRows.v[qpcMod], qpcDiv, d); // ac[0] is 0
            d[0] = ((f[blk] * 16 * kDequantV[qpcMod][0]) << qpcDiv) >> 5;
            inverse4x4(d, r);
            const int ox = (blk & 1) * 4, oy = (blk >> 1) * 4;
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
    const int intraOffset = (1 << qbits) / 3;
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
    const int intraOffset = (1 << qbits) / 3;
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
        uint8_t edge[IntraDirTable<4>::kG];
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
        intraModeSearch(tryMode, predMode);
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
        return s;
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
            costC = double(ssd()) + lambda * mbBits(c, mbx, mby, 0.0);
        }
        if (satd4 >= 0 && satd4 <= lim) {
            restore(snapB);
            costB = double(ssd()) + lambda * mbBits(b, mbx, mby, double(b.bw.bitCount()));
        }
        MbOut a;
        bool deltaA = false;
        if (est16 != INT_MAX && est16 <= lim) {
            deltaA = encodeLuma16(a, mbx, mby, ch);
            costA = double(ssd()) + lambda * mbBits(a, mbx, mby, double(a.bw.bitCount()));
        }
        if (costA >= 1e300 && costB >= 1e300 && costC >= 1e300) {
            tIntraNone = true; // all given up: the macroblock state is left half written
            return false;
        }
        // Same order of preference as below.
        if (costC < costA && costC < costB) {
            restore(snapC);
            out = std::move(c);
            return deltaC;
        }
        if (costB <= costA) {
            restore(snapB);
            out = std::move(b);
            return deltaB;
        }
        out = std::move(a); // state is the Intra 16x16 one (coded last)
        return deltaA;
    }
    MbOut a, b;
    bool deltaA = false;
    double costA = 1e300;
    if (!tIntraSkip16) {
        deltaA = encodeLuma16(a, mbx, mby, ch);
        costA = double(ssd()) + lambda * mbBits(a, mbx, mby, double(a.bw.bitCount()));
    }
    Snapshot snapA;
    save(snapA);
    const bool deltaB = encodeLuma4(b, mbx, mby, ch);
    const double costB = double(ssd()) + lambda * mbBits(b, mbx, mby, double(b.bw.bitCount()));
    if (useT8() && !tIntraSkip8) {
        Snapshot snapB;
        save(snapB);
        MbOut c;
        const bool deltaC = encodeLuma8(c, mbx, mby, ch);
        const double costC = double(ssd()) + lambda * mbBits(c, mbx, mby, 0.0);
        if (costC < costA && costC < costB) {
            out = std::move(c);
            return deltaC;
        }
        if (costB <= costA) {
            restore(snapB);
            out = std::move(b);
            return deltaB;
        }
    }
    if (costA <= costB) {
        restore(snapA);
        out = std::move(a);
        return deltaA;
    }
    out = std::move(b);
    return deltaB;
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
inline int seBits(int v)
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

// x264-style decimation score of a 4x4 block (raster levels, scanned in zig-zag order).
int decimateScore(const int* levels)
{
    static constexpr int kTable[16] = {3, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int scan[16];
    for (int k = 0; k < 16; ++k)
        scan[k] = levels[kZigzag[k]];
    int last = 15;
    while (last >= 0 && scan[last] == 0)
        --last;
    int score = 0;
    while (last >= 0) {
        if (std::abs(scan[last--]) > 1)
            return 9;
        int run = 0;
        while (last >= 0 && scan[last] == 0) {
            --last;
            ++run;
        }
        score += kTable[run];
    }
    return score;
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
        uint8_t edge[IntraDirTable<8>::kG];
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
        intraModeSearch(tryMode, predMode);
        satdSum += bestCost;
        if (satdSum > bound)
            return false; // clearly worse than another intra type: the caller drops it

        int res[64], levels[64];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                res[y * 8 + x] = int(srcY_[size_t(py + y) * padW_ + px + x]) - int(pred[y * 8 + x]);
        quantize8x8(res, qp, 1.0 / 3.0, levels);
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
#if defined(__SSE2__) || defined(_M_X64)
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
    for (int y = 0; y < 8; ++y) {
        const size_t o = co + size_t(y) * cw;
        add8(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&srcU_[o])), zero),
             _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&recU_[o])), zero));
        add8(_mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&srcV_[o])), zero),
             _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&recV_[o])), zero));
    }
    acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(1, 0, 3, 2)));
    acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, _MM_SHUFFLE(2, 3, 0, 1)));
    return int64_t(uint32_t(_mm_cvtsi128_si32(acc)));
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
            s += du * du + dv * dv;
        }
    return s;
#endif
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
    auto cost = [&](int vx, int vy) {
        return double(sadPart(R, mbx, mby, x4, y4, w4, h4, vx, vy, useSatd)) +
               lambdaSad * (seBits(vx - pX) + seBits(vy - pY));
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
    if (fullPelCost)
        *fullPelCost = best;
    useSatd = true;
    best = cost(bx, by);
    const bool diamond = gExp & 16; // experiment: 4 instead of 8 neighbours per sub-pel step
    for (int step : {2, 1}) {
        if (subpel < (step == 2 ? 1 : 2))
            break;
        const int cx0 = bx, cy0 = by;
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
                               InterResidual& ir)
{
    planInterLuma(mbx, mby, lp, qp, lambda, t8, trellis, ir, nullptr);
    reconInterLuma(mbx, mby, lp, qp, t8, io, ir);
}

// First half of coding an inter macroblock's luma residual: transform and quantisation (trellis,
// decimation) into ir. With est (trellis path only, else left at -1): the trellis' own estimate of
// squared luma error + lambda * coefficient bits, for comparing candidates without reconstructing them.
void FrameCoder::planInterLuma(int mbx, int mby, const uint8_t* lp, int qp, double lambda, bool t8, bool trellis,
                               InterResidual& ir, double* est)
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
        // No decimation here: measured, it costs more quality than it saves for 8x8 blocks.
        double sum = 0;
        for (int b8 = 0; b8 < 4; ++b8) {
            const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
            int res[64];
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    res[y * 8 + x] = int(srcY_[lumaOff + size_t(oy + y) * padW_ + ox + x]) - int(lp[(oy + y) * 16 + ox + x]);
            double real[64];
            const bool trellisPath = live && cfg_.effort > 0 && trellis;
            quantize8x8(res, qp, 1.0 / 6.0, trellisPath ? nullptr : levels8[b8], real);
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
                double xs[64];
                for (int s = 0; s < 64; ++s)
                    xs[s] = real[kZigzag8[s]];
                int scanLv[64];
                double energy = 0;
                const double j = cabacTrellis(*live, 5, xs, kScale8[size_t(qp)].data(), 64, lambda, 0, scanLv, est ? &energy : nullptr);
                bool any = false;
                for (int s = 0; s < 64; ++s) {
                    levels8[b8][kZigzag8[s]] = std::clamp(scanLv[s], -kMaxLevel, kMaxLevel);
                    any |= scanLv[s] != 0;
                }
                sum += any ? j : energy;
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
    double jBlk[16], eBlk[16];
    bool zeroBlk[16] = {}; // known to be all zero (no score to compute)
    const bool trellisPath = cfg_.effort > 0 && live && trellis;
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
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
        if (trellisPath) {
            // Trellis with CABAC costs on real-valued levels.
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
            double xs[16];
            const double invStep = 1.0 / double(1 << qbits); // a power of two: same as dividing
            const double* mf = kMfScan[size_t(qpMod)].data();
            for (int s = 0; s < 16; ++s)
                xs[s] = double(coef[kZigzag[s]]) * mf[s] * invStep;
            int scanLv[16];
            jBlk[blkIdx] = cabacTrellis(*live, 2, xs, kScale4[size_t(qp)].data(), 16, lambda, 1, scanLv, est ? &eBlk[blkIdx] : nullptr);
            int any = 0;
            for (int s = 0; s < 16; ++s) {
                levels[blkIdx][kZigzag[s]] = std::clamp(scanLv[s], -kMaxLevel, kMaxLevel);
                any |= scanLv[s];
            }
            zeroBlk[blkIdx] = any == 0;
        } else if (cfg_.effort > 0 && !live) {
            // Round to nearest, then let the rate-distortion pass decide which levels to lower.
            for (int i = 0; i < 16; ++i)
                levels[blkIdx][i] = quantize(coef[i], kQuantMf[qpMod][posClass(i)], qbits, (1 << qbits) / 2);
            rdoQuantize(levels[blkIdx], res, qp, lambda, lumaNc(mbx * 4 + kBlkX[blkIdx], mby * 4 + kBlkY[blkIdx]));
        } else {
            for (int i = 0; i < 16; ++i)
                levels[blkIdx][i] = quantize(coef[i], kQuantMf[qpMod][posClass(i)], qbits, interOffset);
        }
    }
    // Decimation: a few lone +-1 levels cost more bits than they bring back.
    int scores[16], mbScore = 0;
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        scores[blkIdx] = zeroBlk[blkIdx] ? 0 : decimateScore(levels[blkIdx]);
        mbScore += scores[blkIdx];
    }
    double sum = 0;
    for (int b8 = 0; b8 < 4; ++b8) {
        const int s = scores[b8 * 4] + scores[b8 * 4 + 1] + scores[b8 * 4 + 2] + scores[b8 * 4 + 3];
        if (s < 4 || mbScore < 6)
            for (int k = 0; k < 4; ++k)
                std::fill(levels[b8 * 4 + k], levels[b8 * 4 + k] + 16, 0);
        else
            cbpLuma |= 1 << b8; // a kept group has a level (its score is not 0)
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
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        const int bx = mbx * 4 + kBlkX[blkIdx], by = mby * 4 + kBlkY[blkIdx];
        int total = 0;
        int scan[16];
        for (int k = 0; k < 16; ++k)
            scan[k] = levels[blkIdx][kZigzag[k]];
        std::memcpy(io.syn.luma[blkIdx], scan, sizeof(scan));
        if (t8) {
            // 8x8 transform (CABAC only): the four 4x4 blocks carry the 8x8 block's count (bS, nC).
            for (int k = 0; k < 64; ++k)
                total += levels8[blkIdx >> 2][k] != 0;
        } else if (cbpLuma & (1 << (blkIdx >> 2))) {
            total = residual(interBits, scan, 16, lumaNc(bx, by));
        }
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
    const double costSkip = double(mbSsd(mbx, mby)) + lambda * skipBits(mbx, mby);
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
    if (skipResidualIsZero(mbx, mby, lp, cp, qp)) {
        result.syn.kind = MbSyntax::Kind::Skip;
        mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
        return false;
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
    if (cfg_.effort > 0 && !(whole.cost < kSplitSkip * lambdaSad)) {
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

    // Encodes one inter choice; leaves its state in the macroblock and returns SSD + lambda * bits.
    bool interDelta = false;
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
        return double(mbSsd(mbx, mby)) + lambda * mbBits(io, mbx, mby, double(interBits.bitCount() + seBits(0)));
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
    if (useEst) {
        struct Plan {
            InterResidual r4, r8;
            double j4, j8;
        };
        auto plan = [&](const Choice& c, Plan& p) {
            const PredCache& pc = predictLayout(c);
            planInterLuma(mbx, mby, pc.lp, qp, lambda, false, true, p.r4, &p.j4);
            planInterLuma(mbx, mby, pc.lp, qp, lambda, true, true, p.r8, &p.j8);
            p.j4 += lambda * cabacLumaSideBits(*liveCe, mbx, mby, p.r4.cbpLuma, false);
            p.j8 += lambda * cabacLumaSideBits(*liveCe, mbx, mby, p.r8.cbpLuma, true);
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
            }
            return cost;
        };
        auto codeLayout = [&](const Choice& c, const Plan& p) {
            const bool t8 = p.j8 < p.j4;
            const double dist = std::abs(p.j8 - p.j4) / lambda;
            const double first = consider(c, t8, p);
            double other = 1e300;
            if (dist < kEstBoth || gStats)
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
            chosenInterDelta = interDelta;
            interBits = std::move(splitBits);
            saveMb(mbx, mby, interState);
            best = split;
            bestPick = pickSplit;
        }
    }
    // 8x8 transform for the chosen layout (High profile). Experiment (H264_EXP & 128): not when
    // the 4x4 transform left no luma coefficients.
    bool bestT8 = bestPick == 1;
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
        chosenInterDelta = interDelta;
        interBits = std::move(finalBits);
        saveMb(mbx, mby, interState);
    }
    } // !useEst

    // ---- Candidate 3: intra, only when a cheap Intra 16x16 SATD estimate beats the motion search.
    MbOut intraBits;
    double costIntra = 1e300;
    bool intraDelta = false;
    {
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
                costIntra = double(mbSsd(mbx, mby)) +
                            lambda * mbBits(intraBits, mbx, mby, double(intraBits.bw.bitCount() + seBits(0)));
            if (gStats) {
                const int bucket = std::min(3, int(2.0 * intraEstimate / std::max(1.0, best.cost)));
                gStatIntraTry[bucket].fetch_add(1, std::memory_order_relaxed);
                if (costIntra < costInter && costIntra < costSkip)
                    gStatIntraWin[bucket].fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

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
    const double costSkip = double(mbSsd(mbx, mby)) + lambda * skipBits(mbx, mby);
    MbState skipState;
    saveMb(mbx, mby, skipState);
    if (skipResidualIsZero(mbx, mby, dlp, dcp, qp)) {
        result.syn.kind = MbSyntax::Kind::Skip;
        mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
        return false;
    }
    const double estDirect = double(satd16(dlp)) + lambdaSad;

    // ---- Candidate 2: motion search in both lists. Start points: the direct vector, the
    // neighbours' vectors and the co-located block's vector scaled to this picture's distance.
    struct Part {
        int x4, y4, w4, h4, shape;
    };
    static constexpr Part kPart[3][2] = {
        {{0, 0, 4, 4, 0}, {0, 0, 4, 4, 0}},
        {{0, 0, 4, 2, 1}, {0, 2, 4, 2, 2}}, // 16x8
        {{0, 0, 2, 4, 3}, {2, 0, 2, 4, 4}}, // 8x16
    };
    struct Cand {
        double cost = 1e300;
        int bType = 0, shape = 0, count = 0;
        int use[2] = {};
        int mv[2][2][2] = {};  // [partition][list][x, y]
        int mvp[2][2][2] = {}; // the predictors the vector differences are coded against
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
    }
    {
        const RefPic& col = *refs1_[0];
        const size_t o = size_t(mby * 4 + 1) * lw + size_t(mbx * 4 + 1);
        const int td = col.gopPos - refs_[0]->gopPos, tb = gopPos_ - refs_[0]->gopPos;
        if (col.ref[o] == 0 && td > 0) {
            const int fx = col.mvx[o] * tb / td, fy = col.mvy[o] * tb / td;
            starts[0][startCount[0]++] = {fx, fy};
            starts[1][startCount[1]++] = {fx - col.mvx[o], fy - col.mvy[o]};
        }
    }
    // Bits of an mb_type by its number of bins (the first three have adaptive contexts).
    auto typeBits = [](int bType) { return bType == 0 ? 1 : bType <= 2 ? 3 : bType <= 11 ? 6 : 7; };
    auto mvBits = [&](const Cand& c, int p, int list) {
        return seBits(c.mv[p][list][0] - c.mvp[p][list][0]) + seBits(c.mv[p][list][1] - c.mvp[p][list][1]);
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
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    a[y * 16 + x] = clip1((a[y * 16 + x] * biW0_ + b[y * 16 + x] * biW1_ + 32) >> 6);
            int satdBi = 0;
            const size_t so = lumaOff + size_t(P.y4 * 4) * padW_ + P.x4 * 4;
            for (int oy = 0; oy < h; oy += 4)
                for (int ox = 0; ox < w; ox += 8)
                    satdBi += satd8x4U8(&srcY_[so + size_t(oy) * padW_ + ox], padW_, a + oy * 16 + ox, 16);
            const double costBi = satdBi + lambdaSad * (mvBits(c, p, 0) + mvBits(c, p, 1));
            c.use[p] = costBi < cost[0] && costBi < cost[1] ? 3 : cost[0] <= cost[1] ? 1 : 2;
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
        return c;
    };
    // The 16x16 candidate and, where its estimate is better, the best split one (both are coded for real).
    Cand cands[2];
    cands[0] = tryShape(0);
    int candCount = 1;
    if (cfg_.effort > 0 && gBParts) {
        for (int shape : {1, 2}) {
            const Cand c = tryShape(shape);
            if (c.cost < cands[1].cost)
                cands[1] = c;
        }
        if (cands[1].cost < cands[0].cost)
            candCount = 2;
    }
    const double estSearch = candCount == 2 ? cands[1].cost : cands[0].cost;

    // Codes the direct prediction (which = 0) or a searched candidate (1, 2) with its residual;
    // leaves its state in the macroblock and returns SSD + lambda * bits.
    uint8_t clp[2][256], ccp[2][2][64];
    for (int k = 0; k < candCount; ++k)
        for (int p = 0; p < cands[k].count; ++p) {
            const Part& P = kPart[cands[k].shape][p];
            predictBi(mbx, mby, P.x4, P.y4, P.w4, P.h4, cands[k].use[p], cands[k].mv[p][0], cands[k].mv[p][1], clp[k], ccp[k]);
        }
    struct ChromaCache {
        bool valid = false;
        uint8_t recU[64], recV[64];
        ChromaMb ch;
    };
    ChromaCache chromaCache[3];
    bool sentDelta = false;
    auto encodeB = [&](int which, MbOut& io, bool t8) {
        const Cand& best = cands[which ? which - 1 : 0];
        const uint8_t* lp = which ? clp[which - 1] : dlp;
        const uint8_t (*cp)[64] = which ? ccp[which - 1] : dcp;
        if (which) {
            for (int p = 0; p < best.count; ++p) {
                const Part& P = kPart[best.shape][p];
                setBlocks(P.x4, P.y4, P.w4, P.h4, best.use[p], best.mv[p][0], best.mv[p][1]);
            }
        } else {
            setDirect();
        }
        InterResidual res;
        codeInterLuma(mbx, mby, lp, qp, lambda, t8, true, io, res);
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
        if (which)
            for (int p = 0; p < best.count; ++p)
                for (int c = 0; c < 2; ++c) {
                    io.syn.mvd[p][c] = best.mv[p][0][c] - best.mvp[p][0][c];
                    io.syn.mvd1[p][c] = best.mv[p][1][c] - best.mvp[p][1][c];
                }
        sentDelta = writeInterResidual(mbx, mby, qp, t8, res, ch, io);
        return double(mbSsd(mbx, mby)) + lambda * mbBits(io, mbx, mby, 0.0);
    };
    MbOut interBits;
    int bestWhich = 0;
    double costInter = encodeB(0, interBits, false);
    bool chosenDelta = sentDelta;
    MbState interState;
    saveMb(mbx, mby, interState);
    for (int which = 1; which <= candCount; ++which) {
        MbOut candBits;
        const double costCand = encodeB(which, candBits, false);
        if (costCand < costInter) {
            costInter = costCand;
            chosenDelta = sentDelta;
            interBits = std::move(candBits);
            saveMb(mbx, mby, interState);
            bestWhich = which;
        }
    }
    // 8x8 transform for the best of them (High profile).
    if (useT8() && cfg_.effort > 0) {
        MbOut t8Bits;
        const double costT8 = encodeB(bestWhich, t8Bits, true);
        if (costT8 < costInter) {
            costInter = costT8;
            chosenDelta = sentDelta;
            interBits = std::move(t8Bits);
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
                costIntra = double(mbSsd(mbx, mby)) + lambda * mbBits(intraBits, mbx, mby, 0.0);
        }
    }

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
        coded = chosenDelta;
    } else {
        result = std::move(intraBits); // state is already the intra one
        coded = intraDelta;
    }
    commitQp(mbx, mby, coded);
    return true;
}

// After a macroblock is chosen: its QP is the planned one if it sent mb_qp_delta, else the predictor.
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
#if defined(__SSE2__) || defined(_M_X64)
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
#if defined(__SSE2__) || defined(_M_X64)
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
void FrameCoder::mcLuma(const RefPic& R, int x0, int y0, int fx, int fy, int w, int h, uint8_t* out) const
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
#if defined(__SSE2__) || defined(_M_X64)
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
    if (!satd && !(mvx & 3) && !(mvy & 3)) {
        const uint8_t* ref = &R.F[size_t(py + (mvy >> 2) + kMargin) * extW_ + size_t(px + (mvx >> 2) + kMargin)];
        return sadBlock(&srcY_[lo], padW_, ref, extW_, w, h);
    }
    uint8_t buf[256];
    const uint8_t* pred = buf;
    int ps = 16;
    // Full and half positions are one plane: read it in place instead of copying the block.
    const int fx = mvx & 3, fy = mvy & 3;
    if (!(fx & 1) && !(fy & 1)) {
        const RefPic& P = R;
        const std::vector<uint8_t>& plane = fy ? (fx ? P.J : P.V) : (fx ? P.B : P.F);
        pred = plane.data() + size_t(py + (mvy >> 2) + kMargin) * extW_ + size_t(px + (mvx >> 2) + kMargin);
        ps = extW_;
    } else {
        mcLuma(R, px + (mvx >> 2), py + (mvy >> 2), fx, fy, w, h, buf);
    }
    if (satd) {
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

// Filters one line across an edge. q points at q0, step is the distance between samples across the edge.
inline void filterLine(uint8_t* q, int step, int bS, int alpha, int beta, int indexA, bool chroma)
{
    const int p0 = q[-step], p1 = q[-2 * step], q0 = q[0], q1 = q[step];
    if (!(std::abs(p0 - q0) < alpha && std::abs(p1 - p0) < beta && std::abs(q1 - q0) < beta))
        return;
    if (chroma) {
        if (bS < 4) {
            const int tc = kTc0[indexA][bS - 1] + 1;
            const int delta = std::clamp((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tc, tc);
            q[-step] = clip1(p0 + delta);
            q[0] = clip1(q0 - delta);
        } else {
            q[-step] = uint8_t((2 * p1 + p0 + q1 + 2) >> 2);
            q[0] = uint8_t((2 * q1 + q0 + p1 + 2) >> 2);
        }
        return;
    }
    const int p2 = q[-3 * step], q2 = q[2 * step];
    const int ap = std::abs(p2 - p0), aq = std::abs(q2 - q0);
    if (bS < 4) {
        const int tc0 = kTc0[indexA][bS - 1];
        const int tc = tc0 + (ap < beta) + (aq < beta);
        const int delta = std::clamp((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3, -tc, tc);
        q[-step] = clip1(p0 + delta);
        q[0] = clip1(q0 - delta);
        if (ap < beta)
            q[-2 * step] = uint8_t(p1 + std::clamp((p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1, -tc0, tc0));
        if (aq < beta)
            q[step] = uint8_t(q1 + std::clamp((q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1, -tc0, tc0));
    } else {
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
}

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
                const int y = (qpP + qpQ + 1) >> 1, ch = (chromaQp(qpP) + chromaQp(qpQ) + 1) >> 1;
                return EdgeQp{y, kAlpha[y], kBeta[y], ch, kAlpha[ch], kBeta[ch]};
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
            // (with the 8x8 transform only edges 0 and 2).
            const bool t8 = cabWave_.t8[size_t(mby) * mbW_ + mbx] != 0;
            for (int e = 0; e < 4; ++e)
                for (int s = 0; s < 4; ++s) {
                    if (!bsV[e][s] || (t8 && (e & 1)))
                        continue;
                    for (int k = 0; k < 4; ++k) {
                        uint8_t* q = &recY_[size_t(mby * 16 + s * 4 + k) * padW_ + mbx * 16 + e * 4];
                        const EdgeQp& eq = e == 0 ? leftEdge : inner;
                        filterLine(q, 1, bsV[e][s], eq.alphaY, eq.betaY, eq.indexY, false);
                    }
                }
            for (int e = 0; e < 4; ++e)
                for (int s = 0; s < 4; ++s) {
                    if (!bsH[e][s] || (t8 && (e & 1)))
                        continue;
                    for (int k = 0; k < 4; ++k) {
                        uint8_t* q = &recY_[size_t(mby * 16 + e * 4) * padW_ + mbx * 16 + s * 4 + k];
                        const EdgeQp& eq = e == 0 ? topEdge : inner;
                        filterLine(q, padW_, bsH[e][s], eq.alphaY, eq.betaY, eq.indexY, false);
                    }
                }
            // Chroma: edges 0 and 4 use the strength of luma edges 0 and 2.
            for (std::vector<uint8_t>* plane : {&recU_, &recV_}) {
                for (int ce = 0; ce < 2; ++ce)
                    for (int k = 0; k < 8; ++k) {
                        const int bS = bsV[ce * 2][k >> 1];
                        const EdgeQp& eq = ce == 0 ? leftEdge : inner;
                        if (bS)
                            filterLine(&(*plane)[size_t(mby * 8 + k) * cw + mbx * 8 + ce * 4], 1, bS, eq.alphaC, eq.betaC,
                                       eq.indexC, true);
                    }
                for (int ce = 0; ce < 2; ++ce)
                    for (int k = 0; k < 8; ++k) {
                        const int bS = bsH[ce * 2][k >> 1];
                        const EdgeQp& eq = ce == 0 ? topEdge : inner;
                        if (bS)
                            filterLine(&(*plane)[size_t(mby * 8 + ce * 4) * cw + mbx * 8 + k], cw, bS, eq.alphaC, eq.betaC,
                                       eq.indexC, true);
                    }
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
        if (!tree) {
            std::fill(mbQp_.begin(), mbQp_.end(), int8_t(cfg_.qp));
            return;
        }
        for (int m = 0; m < n; ++m)
            mbQp_[size_t(m)] = int8_t(std::clamp(int(std::lround(cfg_.qp + treeOffset_[size_t(m)])), 0, 51));
        return;
    }
    double mean = 0;
    for (int mby = 0; mby < mbH_; ++mby)
        for (int mbx = 0; mbx < mbW_; ++mbx) {
            int64_t sum = 0, sq = 0;
            for (int y = 0; y < 16; ++y) {
                const uint8_t* row = &srcY_[size_t(mby * 16 + y) * padW_ + mbx * 16];
                for (int x = 0; x < 16; ++x) {
                    sum += row[x];
                    sq += row[x] * row[x];
                }
            }
            const double variance = double(sq - sum * sum / 256);
            const double e = std::log2(variance + 1.0);
            mbEnergy_[size_t(mby) * mbW_ + mbx] = float(e);
            mean += e;
        }
    mean /= n;
    for (int m = 0; m < n; ++m) {
        double offset = std::clamp(cfg_.aqStrength * (mbEnergy_[size_t(m)] - mean) * 0.5, -8.0, 8.0);
        if (tree)
            offset += treeOffset_[size_t(m)];
        mbQp_[size_t(m)] = int8_t(std::clamp(int(std::lround(cfg_.qp + offset)), 0, 51));
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
    const int qpc = chromaQp(qp);
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
    CabacEncoder probe = *live;
    probe.startCounting();
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
    CabacEncoder probe = *live;
    probe.startCounting();
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
