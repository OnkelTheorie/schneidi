// schneidi's own H.264 encoder (prototype).
// Clause numbers refer to ITU-T H.264. The encoder mirrors the decoder's
// reconstruction exactly (8.3, 8.5), so later P frames can reference it.
#include "H264Encoder.h"

#include "Cabac.h"
#include "Cavlc.h"
#include "Satd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace schneidi::h264 {

// H264Cabac.cpp
void cabacTrellis(const CabacEncoder& ce, int cat, const double* x, const double* scale, int count, double lambda,
                  int cbfInc, int* out);

namespace {

// Macroblock statistics (H264_STATS=1), for comparisons with other encoders' logs.
const bool gStats = std::getenv("H264_STATS") != nullptr;
std::atomic<long> gStatInter{0}, gStatInterCbp{0}, gStatT8{0};
std::atomic<long> gStatIntraTry[4] = {}, gStatIntraWin[4] = {}; // by ratio of the intra estimate to the inter cost
// Experiments (H264_EXP bit mask), for A/B measurements only.
const int gExp = std::getenv("H264_EXP") ? std::atoi(std::getenv("H264_EXP")) : 0;
const double gIntraFactor = std::getenv("H264_INTRAF") ? std::atof(std::getenv("H264_INTRAF")) : 2.0;
// Phase timing per picture (H264_TIMING=1): where the serial parts are.
const bool gTiming = std::getenv("H264_TIMING") != nullptr;
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

inline uint8_t clip1(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// One step of a spin wait: a pause for the hyperthread sibling, then let others run.
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

void forward4x4(const int* in, int* out)
{
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
}

// 8.5.12.2: rows first, then columns, then (x + 32) >> 6.
void inverse4x4(const int* in, int* out)
{
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
}

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

Encoder::Encoder(const EncoderConfig& config)
    : cfg_(config)
{
    mbW_ = (cfg_.width + 15) / 16;
    mbH_ = (cfg_.height + 15) / 16;
    padW_ = mbW_ * 16;
    padH_ = mbH_ * 16;
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
        bw.u(40, 8);   // level_idc 4.0
        bw.ue(0);      // seq_parameter_set_id
        if (useT8()) {
            bw.ue(1);       // chroma_format_idc 4:2:0
            bw.ue(0);       // bit_depth_luma_minus8
            bw.ue(0);       // bit_depth_chroma_minus8
            bw.flag(false); // qpprime_y_zero_transform_bypass_flag
            bw.flag(false); // seq_scaling_matrix_present_flag
        }
        bw.ue(0);      // log2_max_frame_num_minus4
        bw.ue(2);      // pic_order_cnt_type: output order = decode order
        bw.ue(uint32_t(std::max(1, cfg_.refs))); // max_num_ref_frames
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
        // VUI (E.1.1): only timing, so containers and players know the frame rate.
        bw.flag(true); // vui_parameters_present_flag
        bw.flag(false); // aspect_ratio_info_present_flag
        bw.flag(false); // overscan_info_present_flag
        bw.flag(false); // video_signal_type_present_flag
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
        bw.ue(0);        // max_num_reorder_frames
        bw.ue(1);        // max_dec_frame_buffering
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
        bw.u(0, 2);     // weighted_bipred_idc
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

void Encoder::loadSource(const Picture& pic)
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

FirstPass::FirstPass(int width, int height)
    : width_(width), height_(height), la_(std::make_unique<Lookahead>(width, height, (width + 15) / 16, (height + 15) / 16)),
      stats_(std::make_shared<FirstPassStats>())
{
}

FirstPass::~FirstPass() = default;

void FirstPass::add(const Picture& pic, bool forceKeyframe)
{
    LookaheadFrame f; // the analysis reads only the luma
    f.y.resize(size_t(width_) * height_);
    for (int y = 0; y < height_; ++y)
        std::memcpy(f.y.data() + size_t(y) * width_, pic.y + size_t(y) * pic.strideY, size_t(width_));
    la_->analyse(f, havePrev_ ? &prev_ : nullptr);
    stats_->intraSum.push_back(f.intraSum);
    stats_->interSum.push_back(f.interSum);
    stats_->sceneChange.push_back(f.sceneChange ? 1 : 0);
    stats_->forceKeyframe.push_back(forceKeyframe ? 1 : 0);
    f.y = {};
    prev_ = std::move(f);
    havePrev_ = true;
}

// QP for the P pictures at which the pictures not coded yet (first-pass costs, k per picture type
// from the pictures coded so far) spend exactly the bits that are left of the clip's budget.
double Encoder::twoPassQp(double kI, double kP) const
{
    const FirstPassStats& st = *cfg_.firstPass;
    const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
    const double left = double(st.size()) - frameIndex_;
    const double budget = std::max(double(st.size()) * perFrame - rcBits_, 0.5 * left * perFrame);
    const double need = kI * tpRemI_ * std::exp2(0.5) + kP * tpRemP_;
    return 6.0 * std::log2(std::max(1.0, need) / budget);
}

int Encoder::delay() const { return useLookahead() ? std::max(0, cfg_.lookahead - 1) : 0; }

Encoder::~Encoder() { joinPending(); }

// The analysis of the newest picture runs beside the encoding of the oldest; it joins the
// queue before the next picture is handled.
void Encoder::joinPending()
{
    if (pendingThread_.joinable())
        pendingThread_.join();
    if (pendingFrame_) {
        queue_.push_back(std::move(*pendingFrame_));
        pendingFrame_.reset();
    }
}

std::vector<uint8_t> Encoder::encode(const Picture& picture, bool forceKeyframe)
{
    LookaheadFrame f = copyPicture(picture, cfg_.width, cfg_.height, forceKeyframe);
    if (!useLookahead())
        return encodeFrame(f);
    if (!la_)
        la_ = std::make_unique<Lookahead>(cfg_.width, cfg_.height, mbW_, mbH_);
    joinPending();
    const bool full = int(queue_.size()) + 1 >= cfg_.lookahead;
    // In the background only when its predecessor stays in the queue while a picture is popped.
    if (full && cfg_.threads > 1 && queue_.size() >= 2) {
        pendingFrame_ = std::make_unique<LookaheadFrame>(std::move(f));
        const LookaheadFrame* prev = &queue_.back();
        pendingThread_ = std::thread([this, prev] { la_->analyse(*pendingFrame_, prev); });
    } else {
        la_->analyse(f, queue_.empty() ? nullptr : &queue_.back());
        queue_.push_back(std::move(f));
    }
    if (!full)
        return {};
    std::vector<uint8_t> out = encodeFrame(queue_.front());
    queue_.pop_front();
    return out;
}

std::vector<uint8_t> Encoder::flush()
{
    joinPending();
    if (queue_.empty())
        return {};
    std::vector<uint8_t> out = encodeFrame(queue_.front());
    queue_.pop_front();
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

std::vector<uint8_t> Encoder::encodeFrame(LookaheadFrame& frame)
{
    const bool forceKeyframe = frame.forceKeyframe;
    const Picture picture = pictureOf(frame, cfg_.width);
    PhaseClock clock;
    loadSource(picture);
    clock.lap(0);
    const bool lookaheadFrame = useLookahead() && !queue_.empty() && &queue_.front() == &frame;
    std::fill(nzLuma_.begin(), nzLuma_.end(), 0);
    std::fill(modes4x4_.begin(), modes4x4_.end(), int8_t(-1));
    std::fill(nzChroma_[0].begin(), nzChroma_[0].end(), 0);
    std::fill(nzChroma_[1].begin(), nzChroma_[1].end(), 0);

    std::fill(mvX_.begin(), mvX_.end(), int16_t(0));
    std::fill(mvY_.begin(), mvY_.end(), int16_t(0));
    std::fill(refIdx_.begin(), refIdx_.end(), int8_t(-1));

    // Keyframe on schedule, on request (a cut the caller knows) or on a detected scene change.
    const bool sceneChange = cfg_.sceneCut && cfg_.gop > 1 &&
                             (lookaheadFrame ? frame.sceneChange : detectSceneCut()); // keeps its thumbnail current
    clock.lap(1);
    const bool cut = sceneChange && frameIndex_ > 0 && framesSinceIdr_ >= 6;
    const bool idr = cfg_.gop <= 1 || frameIndex_ == 0 || framesSinceIdr_ >= cfg_.gop || forceKeyframe || cut;
    if (idr)
        framesSinceIdr_ = 0;
    lastWasSceneCut_ = cut;
    pSlice_ = !idr;
    if (idr)
        refs_.clear(); // an IDR picture marks all references unused
    activeRefs_ = idr ? 0 : int(refs_.size());

    // Frame-level rate control (in the spirit of x264's one-pass ABR, aiming at a steady QP, which
    // is what PSNR likes best): a P picture gets the QP at which the average complexity of all P
    // pictures so far (bits * 2^(qp/6); bits roughly halve per +6 QP) meets the per-frame budget,
    // scaled by the bits over or under the target (overflow against a buffer of 5 s of bits) and at
    // most 2 away from the last one. I pictures use the P QP - 3. The first picture is probed at a
    // fixed QP so the stream starts near the right QP instead of paying for a huge first picture.
    const int baseQp = cfg_.qp;
    const bool lookahead = lookaheadFrame;
    double costUsed = 0; // lookahead cost of this picture, for the bit model afterwards
    if (lookahead) {
        markKeyframes();
        frame.keyframe = idr;
        if (cfg_.mbtree) {
            la_->propagate(queue_);
            treeOffset_.resize(size_t(mbW_) * mbH_);
            for (size_t m = 0; m < treeOffset_.size(); ++m)
                treeOffset_[m] = std::max(-8.0f, Lookahead::qpOffset(frame, int(m), cfg_.mbtreeStrength));
        }
    } else {
        treeOffset_.clear();
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
        // Predicted bits at QP 0 of the queued pictures (and the one coded now).
        auto windowNeed = [&](int& count) {
            double need = 0;
            count = 0;
            for (const LookaheadFrame& g : queue_) {
                const double c = frameCost(g, g.keyframe);
                if (&g == &frame)
                    costUsed = c;
                need += kOf(g.keyframe) * c * (g.keyframe ? std::exp2(0.5) : 1.0);
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
            tpRemI_ = tpRemP_ = 0;
            int since = 0;
            for (size_t i = 0; i < st.size(); ++i) {
                const bool key = i == 0 || since >= cfg_.gop || st.forceKeyframe[i] || (cfg_.sceneCut && st.sceneChange[i] && since >= 6);
                if (key)
                    since = 0;
                ++since;
                tpKey_[i] = key;
                (key ? tpRemI_ : tpRemP_) += key ? st.intraSum[i] : st.interSum[i];
            }
        }
        auto steadyQp = [&] {
            if (twoPass) {
                for (const LookaheadFrame& g : queue_)
                    if (&g == &frame)
                        costUsed = g.keyframe ? g.intraSum : g.interSum;
                return twoPassQp(kOf(true), kOf(false));
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
                pc.mbtree = false;
                pc.aqStrength = 0;
                pc.qp = probeQp;
                pc.gop = std::max(2, cfg_.gop);
                pc.sceneCut = false;
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
            qp = twoPassQp(kOf(true), kOf(false));
            const double step = rcFramesSeen_ < 12 ? 2.0 : 0.5;
            if (rcQp_ > 0)
                qp = std::clamp(qp, rcQp_ - step, rcQp_ + step);
            const FirstPassStats& st = *cfg_.firstPass;
            const size_t i = size_t(frameIndex_);
            (tpKey_[i] ? tpRemI_ : tpRemP_) -= tpKey_[i] ? st.intraSum[i] : st.interSum[i];
            if (std::getenv("H264_RC_TRACE"))
                std::fprintf(stderr, "rc2: frame %d kI %.3f kP %.3f qp %.2f\n", frameIndex_, kOf(true), kOf(false), qp);
        } else if (rcQp_ > 0) {
            qp = std::clamp(qp, rcQp_ - 3.0, rcQp_ + 3.0);
        }
        rcQp_ = std::clamp(qp, 10.0, 51.0);
        cfg_.qp = std::clamp(int(std::lround(idr ? qp - 3 : qp)), 10, 51);
        rcNeedSeen_ += kOf(idr) * costUsed * (idr ? std::exp2(0.5) : 1.0);
        rcFramesSeen_ += 1;
    } else if (cfg_.bitrateKbps > 0) {
        const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
        if (frameIndex_ == 0) {
            // The probe aims at an I picture of about four P budgets.
            EncoderConfig pc = cfg_;
            pc.bitrateKbps = 0;
            pc.lookahead = 0;
            pc.mbtree = false;
            pc.gop = std::max(2, cfg_.gop);
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
    }
    lastQp_ = cfg_.qp;
    mbTypeOffset_ = idr ? 0 : 5;

    clock.lap(2); // keyframe decision, macroblock tree, rate control (probe on the first picture)
    planMacroblockQp();
    clock.lap(3);
    // CABAC: one slice, its rows decided in a wavefront by the threads and entropy coded in raster
    // order afterwards. CAVLC (decisions write bits directly): one slice per thread.
    const bool wave = useCabac();
    const int sliceCount = wave ? 1 : std::clamp(cfg_.threads, 1, mbH_);
    std::vector<BitWriter> slices(static_cast<size_t>(sliceCount));
    std::vector<int> sliceTop(size_t(sliceCount) + 1);
    for (int s = 0; s <= sliceCount; ++s)
        sliceTop[size_t(s)] = s * mbH_ / sliceCount;
    for (int s = 0; s < sliceCount; ++s)
        for (int r = sliceTop[size_t(s)]; r < sliceTop[size_t(s) + 1]; ++r)
            sliceTopOfRow_[size_t(r)] = sliceTop[size_t(s)];
    for (int r = 0; r < mbH_; ++r)
        rowKey_[size_t(r)] = wave ? r : sliceTopOfRow_[size_t(r)];

    auto sliceHeader = [&](BitWriter& bw, int s) {
        // slice_header (7.3.3)
        bw.ue(uint32_t(sliceTop[size_t(s)] * mbW_)); // first_mb_in_slice
        bw.ue(idr ? 7 : 5);                         // slice_type: I or P (all slices)
        bw.ue(0);                                   // pic_parameter_set_id
        bw.u(uint32_t(framesSinceIdr_ & 15), 4); // frame_num (0 for IDR)
        if (idr) {
            bw.ue(uint32_t(idrCount_ & 0xFFFF)); // idr_pic_id (differs between neighbours)
        } else {
            bw.flag(activeRefs_ > 1); // num_ref_idx_active_override_flag
            if (activeRefs_ > 1)
                bw.ue(uint32_t(activeRefs_ - 1)); // num_ref_idx_l0_active_minus1
            bw.flag(false); // ref_pic_list_modification_flag_l0
        }
        if (idr) {
            bw.flag(false); // no_output_of_prior_pics_flag
            bw.flag(false); // long_term_reference_flag
        } else {
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
    };
    // CAVLC (and I_PCM): the macroblocks write their bits as they are decided.
    auto encodeSlice = [&](int s) {
        BitWriter& bw = slices[size_t(s)];
        sliceHeader(bw, s);
        int skipRun = 0;
        qpPredByTop_[size_t(sliceTop[size_t(s)])] = cfg_.qp;
        bool prevQpNonZero = false;
        cabacByTop_[size_t(sliceTop[size_t(s)])] = nullptr;
        prevQpNzByTop_[size_t(sliceTop[size_t(s)])] = &prevQpNonZero;
        for (int mby = sliceTop[size_t(s)]; mby < sliceTop[size_t(s) + 1]; ++mby) {
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
    };
    if (wave) {
        // Decisions: a thread takes the next row and stays two macroblocks behind the row above
        // (intra prediction and motion vector prediction read above-right). Bit estimates use a
        // counting CABAC engine per row, started from the row above after its second macroblock.
        const int rows = mbH_;
        std::vector<CabacEncoder> rowEngine(static_cast<size_t>(rows)), rowSnap(static_cast<size_t>(rows));
        std::unique_ptr<std::atomic<int>[]> progress(new std::atomic<int>[size_t(rows)]);
        std::unique_ptr<std::atomic<int>[]> deblocked(new std::atomic<int>[size_t(rows)]); // macroblocks filtered per row
        std::atomic<int> written{0};                                                        // rows the entropy writer has finished
        std::unique_ptr<bool[]> rowPrevNz(new bool[size_t(rows)]);
        for (int r = 0; r < rows; ++r) {
            progress[size_t(r)].store(0, std::memory_order_relaxed);
            deblocked[size_t(r)].store(0, std::memory_order_relaxed);
        }
        mbSyn_.resize(size_t(mbW_) * mbH_);
        std::atomic<int> nextRow{0};
        std::atomic<long> waitMs{0}; // microseconds the threads spent waiting for the row above (H264_TIMING)
        const int threads = std::clamp(cfg_.threads, 1, rows);
        // With several threads the row's thread also deblocks the row above, lagging one
        // macroblock: by then every macroblock that predicts from those unfiltered samples is
        // decided, the entropy writer has fixed the QPs (skipped macroblocks inherit it) and
        // the row above that is filtered two macroblocks further (the raster order of 8.7).
        const bool inlineDeblock = cfg_.deblock && threads > 1;
        auto worker = [&] {
            for (;;) {
                const int r = nextRow.fetch_add(1);
                if (r >= rows)
                    return;
                auto waitAbove = [&](int need) {
                    if (r == 0 || progress[size_t(r - 1)].load(std::memory_order_acquire) >= need)
                        return;
                    const auto t0 = gTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    while (progress[size_t(r - 1)].load(std::memory_order_acquire) < need)
                        spinWait();
                    if (gTiming)
                        waitMs.fetch_add(long(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count()),
                                         std::memory_order_relaxed);
                };
                int dbNext = 0; // next macroblock of row r - 1 to filter
                auto deblockBehind = [&](int upTo, bool wait) {
                    if (!inlineDeblock || r == 0)
                        return;
                    while (dbNext < upTo) {
                        const bool ready = written.load(std::memory_order_acquire) >= r &&
                                           (r == 1 || deblocked[size_t(r - 2)].load(std::memory_order_acquire) >= std::min(dbNext + 2, mbW_));
                        if (!ready) {
                            if (!wait)
                                return;
                            spinWait();
                            continue;
                        }
                        deblockMb(dbNext, r - 1);
                        deblocked[size_t(r - 1)].store(++dbNext, std::memory_order_release);
                    }
                };
                CabacEncoder& eng = rowEngine[size_t(r)];
                if (r == 0) {
                    eng.start(nullptr, cfg_.qp, pSlice_ ? 0 : -1);
                    eng.startCounting();
                } else {
                    waitAbove(std::min(2, mbW_));
                    eng = rowSnap[size_t(r - 1)];
                }
                qpPredByTop_[size_t(r)] = cfg_.qp;
                rowPrevNz[size_t(r)] = false;
                cabacByTop_[size_t(r)] = &eng;
                prevQpNzByTop_[size_t(r)] = &rowPrevNz[size_t(r)];
                for (int mbx = 0; mbx < mbW_; ++mbx) {
                    waitAbove(std::min(mbx + 2, mbW_));
                    MbOut mb;
                    if (pSlice_)
                        encodeMbP(mb, mbx, r);
                    else
                        commitQp(mbx, r, encodeMbIntra(mb, mbx, r));
                    cabacWriteMb(eng, cabWave_, mbx, r, mb.syn, rowPrevNz[size_t(r)]);
                    mbSyn_[size_t(r) * mbW_ + mbx] = mb.syn;
                    if (mbx == std::min(1, mbW_ - 1))
                        rowSnap[size_t(r)] = eng;
                    progress[size_t(r)].store(mbx + 1, std::memory_order_release);
                    deblockBehind(mbx, false);
                }
                deblockBehind(mbW_, true);
            }
        };
        // Entropy coding in raster order, mb_qp_delta against the real predecessor. It follows
        // the decisions row by row (its own neighbour state), so with several threads only the
        // last row's write remains after the wavefront.
        auto writeSlice = [&] {
            BitWriter& bw = slices[0];
            sliceHeader(bw, 0);
            while (!bw.byteAligned())
                bw.u(1, 1); // cabac_alignment_one_bit
            CabacEncoder eng;
            eng.start(&bw, cfg_.qp, pSlice_ ? 0 : -1);
            int pred = cfg_.qp;
            bool prevNz = false;
            const size_t count = size_t(mbW_) * mbH_;
            for (int r = 0; r < rows; ++r) {
                while (progress[size_t(r)].load(std::memory_order_acquire) < mbW_)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
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
                written.store(r + 1, std::memory_order_release);
            }
            bw.alignZero();
        };
        std::vector<std::thread> workers;
        for (int t = 1; t < threads; ++t)
            workers.emplace_back(worker);
        if (threads > 1) {
            std::thread writer(writeSlice);
            worker();
            for (auto& t : workers)
                t.join();
            writer.join();
            if (inlineDeblock) { // the last row has no row below to filter it
                for (int mbx = 0; mbx < mbW_; ++mbx)
                    deblockMb(mbx, rows - 1);
                deblockedInline_ = true;
            }
            if (gTiming)
                gWaitMs += waitMs.load() / 1000.0;
        } else {
            worker();
            writeSlice();
        }
    } else {
        // Slices only look at macroblocks of their own slice, so they run in parallel.
        std::vector<std::thread> workers;
        for (int s = 1; s < sliceCount; ++s)
            workers.emplace_back(encodeSlice, s);
        encodeSlice(0);
        for (auto& t : workers)
            t.join();
    }

    clock.lap(4); // decisions and entropy coding
    std::vector<uint8_t> out;
    for (const BitWriter& bw : slices) {
        if (idr)
            appendNal(out, 3, 5, bw.bytes());
        else
            appendNal(out, 2, 1, bw.bytes());
    }
    if (gStats && pSlice_) {
        std::fprintf(stderr, "stats: inter %ld with luma %ld 8x8 %ld (%.1f %% of coded, %.1f %% of inter)\n", long(gStatInter.load()),
                     long(gStatInterCbp.load()), long(gStatT8.load()), 100.0 * gStatT8.load() / std::max<long>(1, gStatInterCbp.load()),
                     100.0 * gStatT8.load() / std::max<long>(1, gStatInter.load()));
        gStatInter = gStatInterCbp = gStatT8 = 0;
        std::fprintf(stderr, "stats: intra tries/wins by estimate ratio <0.5: %ld/%ld  <1: %ld/%ld  <1.5: %ld/%ld  <2: %ld/%ld\n",
                     long(gStatIntraTry[0].load()), long(gStatIntraWin[0].load()), long(gStatIntraTry[1].load()), long(gStatIntraWin[1].load()),
                     long(gStatIntraTry[2].load()), long(gStatIntraWin[2].load()), long(gStatIntraTry[3].load()), long(gStatIntraWin[3].load()));
        for (int i = 0; i < 4; ++i)
            gStatIntraTry[i] = gStatIntraWin[i] = 0;
    }
    if (cfg_.deblock && !deblockedInline_)
        deblockPicture();
    deblockedInline_ = false;
    clock.lap(5);
    prevMvX_ = mvX_;
    prevMvY_ = mvY_;
    prevRefIdx_ = refIdx_;
    if (cfg_.gop > 1) {
        // Sliding window (8.2.5.3): the newest picture goes first, the oldest drops out.
        RefPic pic;
        if (int(refs_.size()) >= std::max(1, cfg_.refs)) {
            pic = std::move(refs_.back());
            refs_.pop_back();
        }
        pic.y = recY_;
        pic.u = recU_;
        pic.v = recV_;
        buildRefPlanes(pic);
        refs_.insert(refs_.begin(), std::move(pic));
    }
    if (cfg_.bitrateKbps <= 0)
        cfg_.qp = baseQp;
    if (cfg_.bitrateKbps > 0) {
        const double bits = double(out.size()) * 8.0;
        if (lookahead) {
            if (costUsed > 0) {
                rcKSum_[idr ? 0 : 1] += bits * std::exp2(cfg_.qp / 6.0);
                rcCostSum_[idr ? 0 : 1] += costUsed;
            }
        } else if (!idr) {
            rcCplxSum_ += bits * std::pow(2.0, cfg_.qp / 6.0);
            rcCplxCount_ += 1.0;
            rcQp_ = cfg_.qp;
        }
        rcBits_ += bits;
        rcTarget_ += cfg_.bitrateKbps * 1000.0 / cfg_.fps;
        cfg_.qp = baseQp;
    }
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
                         gPhaseMs[4] / gPhaseFrames, gWaitMs / gPhaseFrames, gPhaseMs[5] / gPhaseFrames, gPhaseMs[6] / gPhaseFrames);
    }
    return out;
}

void Encoder::encodeMbPcm(BitWriter& bw, int mbx, int mby)
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

int Encoder::lumaNc(int bx, int by) const
{
    const int w = mbW_ * 4;
    const bool hasA = bx > 0, hasB = (by & 3) ? true : topMbAvailable(by >> 2);
    const int nA = hasA ? nzLuma_[size_t(by) * w + bx - 1] : 0;
    const int nB = hasB ? nzLuma_[size_t(by - 1) * w + bx] : 0;
    if (hasA && hasB)
        return (nA + nB + 1) >> 1;
    return hasA ? nA : nB;
}

int Encoder::chromaNc(int c, int bx, int by) const
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
void predict4x4(int mode, const int* T, const int* L, bool left, bool top, uint8_t* pred)
{
    auto t = [&](int x) { return T[x + 1]; };              // x = -1 is the corner
    auto l = [&](int y) { return y < 0 ? T[0] : L[y]; }; // y = -1 is the corner
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            int v = 128;
            switch (mode) {
            case 0: v = t(x); break;
            case 1: v = l(y); break;
            case 2: {
                const int st = t(0) + t(1) + t(2) + t(3), sl = l(0) + l(1) + l(2) + l(3);
                v = (top && left) ? (st + sl + 4) >> 3 : left ? (sl + 2) >> 2 : top ? (st + 2) >> 2 : 128;
                break;
            }
            case 3:
                v = (x == 3 && y == 3) ? (t(6) + 3 * t(7) + 2) >> 2
                                       : (t(x + y) + 2 * t(x + y + 1) + t(x + y + 2) + 2) >> 2;
                break;
            case 4:
                if (x > y)
                    v = (t(x - y - 2) + 2 * t(x - y - 1) + t(x - y) + 2) >> 2;
                else if (x < y)
                    v = (l(y - x - 2) + 2 * l(y - x - 1) + l(y - x) + 2) >> 2;
                else
                    v = (t(0) + 2 * T[0] + l(0) + 2) >> 2;
                break;
            case 5: {
                const int z = 2 * x - y, k = x - (y >> 1);
                if (z >= 0 && !(z & 1))
                    v = (t(k - 1) + t(k) + 1) >> 1;
                else if (z > 0)
                    v = (t(k - 2) + 2 * t(k - 1) + t(k) + 2) >> 2;
                else if (z == -1)
                    v = (l(0) + 2 * T[0] + t(0) + 2) >> 2;
                else
                    v = (l(y - 1) + 2 * l(y - 2) + l(y - 3) + 2) >> 2;
                break;
            }
            case 6: {
                const int z = 2 * y - x, k = y - (x >> 1);
                if (z >= 0 && !(z & 1))
                    v = (l(k - 1) + l(k) + 1) >> 1;
                else if (z > 0)
                    v = (l(k - 2) + 2 * l(k - 1) + l(k) + 2) >> 2;
                else if (z == -1)
                    v = (l(0) + 2 * T[0] + t(0) + 2) >> 2;
                else
                    v = (t(x - 1) + 2 * t(x - 2) + t(x - 3) + 2) >> 2;
                break;
            }
            case 7: {
                const int k = x + (y >> 1);
                v = (y & 1) ? (t(k) + 2 * t(k + 1) + t(k + 2) + 2) >> 2 : (t(k) + t(k + 1) + 1) >> 1;
                break;
            }
            case 8: {
                const int z = x + 2 * y, k = y + (x >> 1);
                if (z > 5)
                    v = l(3);
                else if (z == 5)
                    v = (l(2) + 3 * l(3) + 2) >> 2;
                else if (z & 1)
                    v = (l(k) + 2 * l(k + 1) + l(k + 2) + 2) >> 2;
                else
                    v = (l(k) + l(k + 1) + 1) >> 1;
                break;
            }
            }
            pred[y * 4 + x] = uint8_t(v);
        }
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
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            int v = 128;
            switch (mode) {
            case 0: v = t(x); break;
            case 1: v = l(y); break;
            case 2: v = dc; break;
            case 3:
                v = (x == 7 && y == 7) ? (t(14) + 3 * t(15) + 2) >> 2
                                       : (t(x + y) + 2 * t(x + y + 1) + t(x + y + 2) + 2) >> 2;
                break;
            case 4:
                if (x > y)
                    v = (t(x - y - 2) + 2 * t(x - y - 1) + t(x - y) + 2) >> 2;
                else if (x < y)
                    v = (l(y - x - 2) + 2 * l(y - x - 1) + l(y - x) + 2) >> 2;
                else
                    v = (t(0) + 2 * T[0] + l(0) + 2) >> 2;
                break;
            case 5: {
                const int z = 2 * x - y, k = x - (y >> 1);
                if (z >= 0 && !(z & 1))
                    v = (t(k - 1) + t(k) + 1) >> 1;
                else if (z > 0)
                    v = (t(k - 2) + 2 * t(k - 1) + t(k) + 2) >> 2;
                else if (z == -1)
                    v = (l(0) + 2 * T[0] + t(0) + 2) >> 2;
                else
                    v = (l(y - 2 * x - 1) + 2 * l(y - 2 * x - 2) + l(y - 2 * x - 3) + 2) >> 2;
                break;
            }
            case 6: {
                const int z = 2 * y - x, k = y - (x >> 1);
                if (z >= 0 && !(z & 1))
                    v = (l(k - 1) + l(k) + 1) >> 1;
                else if (z > 0)
                    v = (l(k - 2) + 2 * l(k - 1) + l(k) + 2) >> 2;
                else if (z == -1)
                    v = (l(0) + 2 * T[0] + t(0) + 2) >> 2;
                else
                    v = (t(x - 2 * y - 1) + 2 * t(x - 2 * y - 2) + t(x - 2 * y - 3) + 2) >> 2;
                break;
            }
            case 7: {
                const int k = x + (y >> 1);
                v = (y & 1) ? (t(k) + 2 * t(k + 1) + t(k + 2) + 2) >> 2 : (t(k) + t(k + 1) + 1) >> 1;
                break;
            }
            case 8: {
                const int z = x + 2 * y, k = y + (x >> 1);
                if (z > 13)
                    v = l(7);
                else if (z == 13)
                    v = (l(6) + 3 * l(7) + 2) >> 2;
                else if (z & 1)
                    v = (l(k) + 2 * l(k + 1) + l(k + 2) + 2) >> 2;
                else
                    v = (l(k) + l(k + 1) + 1) >> 1;
                break;
            }
            }
            pred[y * 8 + x] = uint8_t(v);
        }
    }
}

} // namespace

struct Encoder::ChromaMb {
    int mode = 0;
    int cbp = 0;
    int dc[2][4];
    int ac[2][4][16];
};

void Encoder::analyseChroma(int mbx, int mby, ChromaMb& ch, const uint8_t (*interPred)[64])
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
            int res[16];
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    res[y * 4 + x] = int(csrcBlk[c][(oy + y) * 8 + ox + x]) - int(cpred[c][(oy + y) * 8 + ox + x]);
            forward4x4(res, ccoef[b]);
        }
        const int a = ccoef[0][0], b = ccoef[1][0], cc = ccoef[2][0], d = ccoef[3][0];
        const int f[4] = {a + b + cc + d, a - b + cc - d, a + b - cc - d, a - b - cc + d};
        for (int i = 0; i < 4; ++i) {
            ch.dc[c][i] = quantize(f[i], kQuantMf[qpcMod][0], cqbits + 1, 2 * cOffset);
            anyDc |= ch.dc[c][i] != 0;
        }
        for (int blk = 0; blk < 4; ++blk) {
            ch.ac[c][blk][0] = 0;
            for (int i = 1; i < 16; ++i) {
                ch.ac[c][blk][i] = quantize(ccoef[blk][i], kQuantMf[qpcMod][posClass(i)], cqbits, cOffset);
                anyAc |= ch.ac[c][blk][i] != 0;
            }
        }
    }
    ch.cbp = anyAc ? 2 : anyDc ? 1 : 0;

    for (int c = 0; c < 2; ++c) {
        const int* l = ch.dc[c];
        const int f[4] = {l[0] + l[1] + l[2] + l[3], l[0] - l[1] + l[2] - l[3], l[0] + l[1] - l[2] - l[3],
                          l[0] - l[1] - l[2] + l[3]};
        uint8_t* r0 = &(*crec[c])[chromaOff];
        for (int blk = 0; blk < 4; ++blk) {
            int d[16];
            d[0] = ((f[blk] * 16 * kDequantV[qpcMod][0]) << qpcDiv) >> 5;
            for (int i = 1; i < 16; ++i)
                d[i] = (ch.ac[c][blk][i] * kDequantV[qpcMod][posClass(i)]) << qpcDiv;
            int r[16];
            inverse4x4(d, r);
            const int ox = (blk & 1) * 4, oy = (blk >> 1) * 4;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    r0[size_t(oy + y) * cw + ox + x] = clip1(cpred[c][(oy + y) * 8 + ox + x] + r[y * 4 + x]);
        }
    }
}

void Encoder::writeChroma(MbOut& o, int mbx, int mby, const ChromaMb& ch)
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

bool Encoder::encodeLuma16(MbOut& o, int mbx, int mby, const ChromaMb& ch)
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
        int res[16];
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                res[y * 4 + x] = int(src[(oy + y) * 16 + ox + x]) - int(pred[(oy + y) * 16 + ox + x]);
        forward4x4(res, coef[b]);
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
        acLevel[b][0] = 0;
        for (int i = 1; i < 16; ++i) {
            acLevel[b][i] = quantize(coef[b][i], kQuantMf[qpMod][posClass(i)], qbits, intraOffset);
            anyAc |= acLevel[b][i] != 0;
        }
    }
    const int cbpLuma = anyAc ? 15 : 0;

    int dcF[16];
    hadamard4x4(dcLevel, dcF);
    const int dcScale = 16 * kDequantV[qpMod][0];
    for (int b = 0; b < 16; ++b) {
        int d[16];
        if (qp >= 36)
            d[0] = (dcF[b] * dcScale) << (qpDiv - 6);
        else
            d[0] = (dcF[b] * dcScale + (1 << (5 - qpDiv))) >> (6 - qpDiv);
        for (int i = 1; i < 16; ++i)
            d[i] = (acLevel[b][i] * kDequantV[qpMod][posClass(i)]) << qpDiv;
        int r[16];
        inverse4x4(d, r);
        const int ox = (b & 3) * 4, oy = (b >> 2) * 4;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                rec[size_t(oy + y) * padW_ + ox + x] = clip1(pred[(oy + y) * 16 + ox + x] + r[y * 4 + x]);
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

bool Encoder::encodeLuma4(MbOut& o, int mbx, int mby, const ChromaMb& ch)
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
        int bestMode = 2, bestCost = 1 << 30;
        for (int mode = 0; mode < 9; ++mode) {
            const bool needsTop = mode == 0 || mode == 3 || mode == 7;
            const bool needsLeft = mode == 1 || mode == 8;
            const bool needsBoth = mode >= 4 && mode <= 6;
            if ((needsTop && !top) || (needsLeft && !left) || (needsBoth && !(top && left)))
                continue;
            predict4x4(mode, T, L, left, top, cand);
            const int cost = satdU8(&srcY_[size_t(py) * padW_ + px], padW_, cand, 4) + lambda * (mode == predMode ? 1 : 4);
            if (cost < bestCost) {
                bestCost = cost;
                bestMode = mode;
                std::memcpy(pred, cand, 16);
            }
        }
        modes[blkIdx] = bestMode;
        modes4x4_[size_t(by) * lw + bx] = int8_t(bestMode);

        int res[16], coef[16];
        for (int i = 0; i < 16; ++i)
            res[i] = src[i] - pred[i];
        forward4x4(res, coef);
        bool any = false;
        int d[16];
        for (int i = 0; i < 16; ++i) {
            levels[blkIdx][i] = quantize(coef[i], kQuantMf[qpMod][posClass(i)], qbits, intraOffset);
            any |= levels[blkIdx][i] != 0;
            d[i] = (levels[blkIdx][i] * kDequantV[qpMod][posClass(i)]) << qpDiv;
        }
        if (any)
            cbpLuma |= 1 << (blkIdx >> 2);
        int r[16];
        inverse4x4(d, r);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                recY_[size_t(py + y) * padW_ + px + x] = clip1(pred[y * 4 + x] + r[y * 4 + x]);
    }

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

bool Encoder::encodeMbIntra(MbOut& out, int mbx, int mby)
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
    MbOut a, b;
    bool deltaA = false;
    double costA = 1e300;
    if (!intraSkip16_) {
        deltaA = encodeLuma16(a, mbx, mby, ch);
        costA = double(ssd()) + lambda * mbBits(a, mbx, mby, double(a.bw.bitCount()));
    }
    Snapshot snapA;
    save(snapA);
    const bool deltaB = encodeLuma4(b, mbx, mby, ch);
    const double costB = double(ssd()) + lambda * mbBits(b, mbx, mby, double(b.bw.bitCount()));
    if (useT8() && !intraSkip8_) {
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

// Decoder-exact inverse: rows, then columns, then (x + 32) >> 6.
void inverse8x8(const int* d, int* out)
{
    int t[64];
    for (int i = 0; i < 8; ++i)
        inverse8Line<int>(d + i * 8, t + i * 8, true);
    for (int j = 0; j < 8; ++j) {
        int col[8], res[8];
        for (int i = 0; i < 8; ++i)
            col[i] = t[i * 8 + j];
        inverse8Line<int>(col, res, true);
        for (int i = 0; i < 8; ++i)
            out[i * 8 + j] = (res[i] + 32) >> 6;
    }
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
    static const auto kM = [] { // 8 * basis, integer
        std::array<std::array<int, 8>, 8> m{};
        const Basis8& B = basis8();
        for (int k = 0; k < 8; ++k)
            for (int x = 0; x < 8; ++x)
                m[size_t(k)][size_t(x)] = int(std::lround(8.0 * B.b[k][x]));
        return m;
    }();
    static const auto kScale = [] { // 256 / (n_i n_j v8) by QP % 6 and position, the /64 of the integer transform folded in
        std::array<std::array<double, 64>, 6> t{};
        const Basis8& B = basis8();
        for (int q = 0; q < 6; ++q)
            for (int k = 0; k < 64; ++k)
                t[size_t(q)][size_t(k)] = B.norm[k >> 3] * B.norm[k & 7] * kDequantV8[q][posClass8(k)];
        return t;
    }();
    int32_t tmp[64];
    for (int y = 0; y < 8; ++y) {
        const int* r = residual + y * 8;
        for (int j = 0; j < 8; ++j) {
            const int* m = kM[size_t(j)].data();
            tmp[y * 8 + j] = m[0] * r[0] + m[1] * r[1] + m[2] * r[2] + m[3] * r[3] + m[4] * r[4] + m[5] * r[5] + m[6] * r[6] + m[7] * r[7];
        }
    }
    const double step2 = double(1 << (qp / 6));
    const double* scale = kScale[size_t(qp % 6)].data();
    for (int i = 0; i < 8; ++i) {
        const int* m = kM[size_t(i)].data();
        for (int j = 0; j < 8; ++j) {
            const int32_t w64 = m[0] * tmp[j] + m[1] * tmp[8 + j] + m[2] * tmp[16 + j] + m[3] * tmp[24 + j] + m[4] * tmp[32 + j] +
                                m[5] * tmp[40 + j] + m[6] * tmp[48 + j] + m[7] * tmp[56 + j];
            // Decoder: r = B^T (Z * v8 * 2^(qp/6 - 2)) B / 64, so Z = 256 * W / (n_i n_j v8 2^(qp/6)).
            const int k = i * 8 + j;
            const double x = 256.0 * (double(w64) / 64.0) / (scale[k] * step2);
            const int mag = std::min(int(std::abs(x) + offset), kMaxLevel);
            levels[k] = x < 0 ? -mag : mag;
            if (real)
                real[k] = x;
        }
    }
}

void dequant8x8(const int* levels, int qp, int* d)
{
    const int qpDiv = qp / 6, qpMod = qp % 6;
    for (int k = 0; k < 64; ++k) {
        const int ls = 16 * kDequantV8[qpMod][posClass8(k)];
        if (qpDiv >= 6)
            d[k] = (levels[k] * ls) << (qpDiv - 6);
        else
            d[k] = (levels[k] * ls + (1 << (5 - qpDiv))) >> (6 - qpDiv);
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
bool Encoder::encodeLuma8(MbOut& o, int mbx, int mby, const ChromaMb& ch)
{
    const int qp = mbQp_[size_t(mby) * mbW_ + mbx];
    const int lw = mbW_ * 4;
    const int lambda = std::max(1, int(0.9 * std::pow(2.0, (qp - 12) / 6.0) + 0.5));
    int cbpLuma = 0;
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
        int bestMode = 2, bestCost = 1 << 30;
        for (int mode = 0; mode < 9; ++mode) {
            const bool needsTop = mode == 0 || mode == 3 || mode == 7;
            const bool needsLeft = mode == 1 || mode == 8;
            const bool needsBoth = mode >= 4 && mode <= 6;
            if ((needsTop && !top) || (needsLeft && !left) || (needsBoth && !corner))
                continue;
            predict8x8(mode, T, L, left, top, cand);
            int cost = lambda * (mode == predMode ? 1 : 4);
            for (int k = 0; k < 2; ++k)
                cost += satd8x4U8(&srcY_[size_t(py + k * 4) * padW_ + px], padW_, cand + k * 32, 8);
            if (cost < bestCost) {
                bestCost = cost;
                bestMode = mode;
                std::memcpy(pred, cand, 64);
            }
        }

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
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                recY_[size_t(py + y) * padW_ + px + x] = clip1(pred[y * 8 + x] + r[y * 8 + x]);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) {
                modes4x4_[size_t(by + y) * lw + bx + x] = int8_t(bestMode);
                nzLuma_[size_t(by + y) * lw + bx + x] = uint8_t(count);
            }
        o.syn.i4Mode[b8] = bestMode;
        o.syn.i4Pred[b8] = predMode;
    }
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

struct Encoder::MbState {
    uint8_t y[256];
    uint8_t c[2][64];
    uint8_t nz[16];
    int8_t modes[16];
    uint8_t nzc[2][4];
    int16_t mvx[16], mvy[16];
    int8_t ref[16];
};

void Encoder::saveMb(int mbx, int mby, MbState& s) const
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
        }
    for (int c = 0; c < 2; ++c)
        for (int b = 0; b < 4; ++b)
            s.nzc[c][b] = nzChroma_[c][size_t(mby * 2 + (b >> 1)) * chw + mbx * 2 + (b & 1)];
}

void Encoder::restoreMb(int mbx, int mby, const MbState& s)
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
        }
    for (int c = 0; c < 2; ++c)
        for (int b = 0; b < 4; ++b)
            nzChroma_[c][size_t(mby * 2 + (b >> 1)) * chw + mbx * 2 + (b & 1)] = s.nzc[c][b];
}

int64_t Encoder::mbSsd(int mbx, int mby) const
{
    int64_t s = 0;
    const size_t lo = size_t(mby * 16) * padW_ + mbx * 16;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const size_t o = lo + size_t(y) * padW_ + x;
            const int d = int(srcY_[o]) - int(recY_[o]);
            s += d * d;
        }
    const int cw = padW_ / 2;
    const size_t co = size_t(mby * 8) * cw + mbx * 8;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const size_t o = co + size_t(y) * cw + x;
            const int du = int(srcU_[o]) - int(recU_[o]);
            const int dv = int(srcV_[o]) - int(recV_[o]);
            s += du * du + dv * dv;
        }
    return s;
}

// Neighbouring 4x4 block (x4, y4 relative to the macroblock, may be -1 or 4) for motion
// vector prediction (8.4.1.3.2). `decoded` marks blocks of the current macroblock that
// already have their vector.
Encoder::MvNb Encoder::mvNeighbour(int mbx, int mby, int x4, int y4, const bool* decoded) const
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
    n.ref = refIdx_[o];
    if (n.ref >= 0) {
        n.mvx = mvX_[o];
        n.mvy = mvY_[o];
    }
    return n;
}

// Motion vector prediction (8.4.1.3) for a partition at (x4, y4) of w4 x h4 blocks.
// shape: 0 = median, 1/2 = 16x8 top/bottom, 3/4 = 8x16 left/right.
void Encoder::predictMvPart(int mbx, int mby, int x4, int y4, int w4, int shape, int refIdx, const bool* decoded,
                            int& mvx, int& mvy) const
{
    MvNb a = mvNeighbour(mbx, mby, x4 - 1, y4, decoded);
    MvNb b = mvNeighbour(mbx, mby, x4, y4 - 1, decoded);
    MvNb c = mvNeighbour(mbx, mby, x4 + w4, y4 - 1, decoded);
    if (!c.avail)
        c = mvNeighbour(mbx, mby, x4 - 1, y4 - 1, decoded); // D replaces C
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
void Encoder::motionCompensate(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy,
                               uint8_t* luma, uint8_t (*chroma)[64]) const
{
    mcLuma(R, mbx * 16 + x4 * 4 + (mvx >> 2), mby * 16 + y4 * 4 + (mvy >> 2), mvx & 3, mvy & 3, w4 * 4, h4 * 4,
           luma + y4 * 4 * 16 + x4 * 4);
    // Chroma: eighth-pel bilinear (8.4.2.2.2).
    const int cw = padW_ / 2, chh = padH_ / 2;
    const int xF = mvx & 7, yF = mvy & 7;
    const std::vector<uint8_t>* planes[2] = {&R.u, &R.v};
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
void Encoder::rdoQuantize(int* levels, const int* residual, int qp, double lambda, int nC) const
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

bool Encoder::encodeMbP(MbOut& result, int mbx, int mby)
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
    motionCompensate(refs_[0], mbx, mby, 0, 0, 4, 4, skipX, skipY, lp, cp);
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
            for (int b = 0; b < 16; b += 2) {
                const int ox = (b & 3) * 4, oy = (b >> 2) * 4;
                sum += satd8x4U8(&srcY_[lumaOff + size_t(oy) * padW_ + ox], padW_, cand + oy * 16 + ox, 16);
            }
            est = std::min(est, sum);
        }
        return est;
    };
    if (skipResidualIsZero(mbx, mby, lp, cp, qp)) {
        result.syn.kind = MbSyntax::Kind::Skip;
        mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
        return false;
    }

    // ---- Candidate 2: inter with 16x16, 16x8, 8x16 or 8x8 partitions.
    const int range = 64 * 4;
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
        if (prevRefIdx_.size() == refIdx_.size() && prevRefIdx_[o] == 0)
            starts.push_back({prevMvX_[o], prevMvY_[o]});
    }
    auto search = [&](int x4, int y4, int w4, int h4, int ref, int pX, int pY, int& outX, int& outY) {
        starts.push_back({pX, pY});
        bool useSatd = false;
        const RefPic& R = refs_[size_t(ref)];
        auto cost = [&](int vx, int vy) {
            return double(sadPart(R, mbx, mby, x4, y4, w4, h4, vx, vy, useSatd)) +
                   lambdaSad * (seBits(vx - pX) + seBits(vy - pY));
        };
        int bx = 0, by = 0;
        double best = cost(0, 0);
        for (auto [cx, cy] : starts) {
            cx = std::clamp(cx & ~3, -range, range);
            cy = std::clamp(cy & ~3, -range, range);
            const double c = cost(cx, cy);
            if (c < best) {
                best = c;
                bx = cx;
                by = cy;
            }
        }
        // Experiment: a start point that already matches well (under 2 SAD per sample) skips
        // the coarse steps.
        const bool goodStart = (gExp & 8) && best < 2.0 * 16.0 * w4 * h4;
        for (int step : {32, 16, 8, 4}) {
            if (goodStart && step > 8)
                continue;
            for (int iter = 0; iter < 16; ++iter) {
                bool moved = false;
                static constexpr int kDir[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : kDir) {
                    const int cx = bx + d[0] * step, cy = by + d[1] * step;
                    if (std::abs(cx) > range || std::abs(cy) > range)
                        continue;
                    const double c = cost(cx, cy);
                    if (c < best) {
                        best = c;
                        bx = cx;
                        by = cy;
                        moved = true;
                    }
                }
                if (!moved)
                    break;
            }
        }
        useSatd = true;
        best = cost(bx, by);
        const bool diamond = gExp & 16; // experiment: 4 instead of 8 neighbours per sub-pel step
        for (int step : {2, 1}) {
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
        starts.pop_back();
        outX = bx;
        outY = by;
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
    Choice split;
    if (cfg_.effort > 0) {
        split = tryLayout(3);
        if (split.cost < whole.cost * 1.1) { // 16x8 / 8x16 only where splitting looks worth it
            for (int li : {1, 2}) {
                const Choice c = tryLayout(li);
                if (c.cost < split.cost)
                    split = c;
            }
        }
    }

    // Encodes one inter choice; leaves its state in the macroblock and returns SSD + lambda * bits.
    bool interDelta = false;
    const bool trellisLast = gExp & 32; // experiment: candidates with plain quantisation, the winner again with the trellis
    bool trellisNow = !trellisLast;
    auto encodeInter = [&](const Choice& best, MbOut& io, bool t8) {
        BitWriter& interBits = io.bw;
        const Layout& layout = kLayouts[best.layout];
        for (int p = 0; p < layout.count; ++p) {
            const Part& P = layout.parts[p];
            motionCompensate(refs_[size_t(best.ref[p])], mbx, mby, P.x4, P.y4, P.w4, P.h4, best.mv[p][0], best.mv[p][1], lp,
                             cp);
            setBlocks(P.x4, P.y4, P.w4, P.h4, best.mv[p][0], best.mv[p][1], best.ref[p]);
        }
        const int qpDiv = qp / 6, qpMod = qp % 6;
        const int qbits = 15 + qpDiv;
        const int interOffset = (1 << qbits) / 6;
        int levels[16][16];
        int cbpLuma = 0;
        int levels8[4][64] = {};
        if (t8) {
            // No decimation here: measured, it costs more quality than it saves for 8x8 blocks.
            for (int b8 = 0; b8 < 4; ++b8) {
                const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
                int res[64];
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        res[y * 8 + x] = int(srcY_[lumaOff + size_t(oy + y) * padW_ + ox + x]) - int(lp[(oy + y) * 16 + ox + x]);
                const CabacEncoder* live = cabacByTop_[size_t(rowKey_[size_t(mby)])];
                double real[64];
                quantize8x8(res, qp, 1.0 / 6.0, levels8[b8], real);
                if (live && cfg_.effort > 0 && trellisNow) {
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
                    cabacTrellis(*live, 5, xs, kScale8[size_t(qp)].data(), 64, lambda, 0, scanLv);
                    for (int s = 0; s < 64; ++s)
                        levels8[b8][kZigzag8[s]] = std::clamp(scanLv[s], -kMaxLevel, kMaxLevel);
                }
            }
            for (int b8 = 0; b8 < 4; ++b8) {
                const int ox = (b8 & 1) * 8, oy = (b8 >> 1) * 8;
                int count = 0;
                for (int k = 0; k < 64; ++k)
                    count += levels8[b8][k] != 0;
                if (count)
                    cbpLuma |= 1 << b8;
                int d[64], r[64];
                dequant8x8(levels8[b8], qp, d);
                inverse8x8(d, r);
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x)
                        recY_[lumaOff + size_t(oy + y) * padW_ + ox + x] = clip1(lp[(oy + y) * 16 + ox + x] + r[y * 8 + x]);
                for (int k = 0; k < 64; ++k)
                    io.syn.luma8[b8][k] = levels8[b8][kZigzag8[k]];
            }
            for (int b = 0; b < 16; ++b)
                std::fill(levels[b], levels[b] + 16, 0);
        }
        for (int blkIdx = 0; blkIdx < 16 && !t8; ++blkIdx) {
            const int ox = kBlkX[blkIdx] * 4, oy = kBlkY[blkIdx] * 4;
            int res[16], coef[16];
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    res[y * 4 + x] = int(srcY_[lumaOff + size_t(oy + y) * padW_ + ox + x]) - int(lp[(oy + y) * 16 + ox + x]);
            forward4x4(res, coef);
            const CabacEncoder* live = cabacByTop_[size_t(rowKey_[size_t(mby)])];
            if (cfg_.effort > 0 && live && trellisNow) {
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
                double xs[16];
                for (int s = 0; s < 16; ++s) {
                    const int r = kZigzag[s];
                    xs[s] = double(coef[r]) * kQuantMf[qpMod][posClass(r)] / double(1 << qbits);
                }
                int scanLv[16];
                cabacTrellis(*live, 2, xs, kScale4[size_t(qp)].data(), 16, lambda, 1, scanLv);
                for (int s = 0; s < 16; ++s)
                    levels[blkIdx][kZigzag[s]] = std::clamp(scanLv[s], -kMaxLevel, kMaxLevel);
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
        for (int blkIdx = 0; blkIdx < 16 && !t8; ++blkIdx) {
            scores[blkIdx] = decimateScore(levels[blkIdx]);
            mbScore += scores[blkIdx];
        }
        for (int b8 = 0; b8 < 4 && !t8; ++b8) {
            const int s = scores[b8 * 4] + scores[b8 * 4 + 1] + scores[b8 * 4 + 2] + scores[b8 * 4 + 3];
            if (s < 4 || mbScore < 6)
                for (int k = 0; k < 4; ++k)
                    std::fill(levels[b8 * 4 + k], levels[b8 * 4 + k] + 16, 0);
        }
        for (int blkIdx = 0; blkIdx < 16 && !t8; ++blkIdx) {
            const int ox = kBlkX[blkIdx] * 4, oy = kBlkY[blkIdx] * 4;
            bool any = false;
            int d[16];
            for (int i = 0; i < 16; ++i) {
                any |= levels[blkIdx][i] != 0;
                d[i] = (levels[blkIdx][i] * kDequantV[qpMod][posClass(i)]) << qpDiv;
            }
            if (any)
                cbpLuma |= 1 << (blkIdx >> 2);
            int r[16];
            inverse4x4(d, r);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    recY_[lumaOff + size_t(oy + y) * padW_ + ox + x] = clip1(lp[(oy + y) * 16 + ox + x] + r[y * 4 + x]);
        }
        ChromaMb ch;
        analyseChroma(mbx, mby, ch, cp);
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
        const int cbp = cbpLuma | (ch.cbp << 4);
        interBits.ue(uint32_t(interCbpCode(cbp)));
        io.syn.cbpLuma = cbpLuma;
        io.syn.t8 = t8 && cbpLuma != 0;
        io.syn.hasQpDelta = cbp != 0;
        io.syn.qpDelta = qp - qpPredOf(mby);
        if (cbp)
            interBits.se(qp - qpPredOf(mby)); // mb_qp_delta
        interDelta = cbp != 0;
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
        // A skip_run ends here: count its codeword to be fair against skip.
        return double(mbSsd(mbx, mby)) + lambda * mbBits(io, mbx, mby, double(interBits.bitCount() + seBits(0)));
    };
    MbOut interBits;
    double costInter = encodeInter(whole, interBits, false);
    bool chosenInterDelta = interDelta;
    MbState interState;
    saveMb(mbx, mby, interState);
    Choice best = whole;
    // Try the split for real when its estimate is close or better.
    if (split.cost < whole.cost * 1.1) {
        MbOut splitBits;
        const double costSplit = encodeInter(split, splitBits, false);
        if (costSplit < costInter) {
            costInter = costSplit;
            chosenInterDelta = interDelta;
            interBits = std::move(splitBits);
            saveMb(mbx, mby, interState);
            best = split;
        }
    }
    // 8x8 transform for the chosen layout (High profile).
    bool bestT8 = false;
    if (useT8() && cfg_.effort > 0) {
        MbOut t8Bits;
        const double costT8 = encodeInter(best, t8Bits, true);
        if (costT8 < costInter) {
            costInter = costT8;
            chosenInterDelta = interDelta;
            interBits = std::move(t8Bits);
            saveMb(mbx, mby, interState);
            bestT8 = true;
        }
    }
    if (trellisLast && cfg_.effort > 0) {
        trellisNow = true;
        MbOut finalBits;
        costInter = encodeInter(best, finalBits, bestT8);
        chosenInterDelta = interDelta;
        interBits = std::move(finalBits);
        saveMb(mbx, mby, interState);
    }

    // ---- Candidate 3: intra, only when a cheap Intra 16x16 SATD estimate beats the motion search.
    MbOut intraBits;
    double costIntra = 1e300;
    bool intraDelta = false;
    {
        const int intraEstimate = intraEstimate16();
        // recY_ of this macroblock holds the inter reconstruction here, but Intra 16x16
        // prediction only reads the neighbours above and to the left.
        if (double(intraEstimate) < best.cost * gIntraFactor) {
            // Borderline cases (estimate above the inter cost) rarely win; experiments drop
            // the dearer intra types there.
            const bool borderline = double(intraEstimate) >= best.cost;
            intraSkip8_ = borderline && (gExp & 2);
            intraSkip16_ = borderline && (gExp & 4);
            intraDelta = encodeMbIntra(intraBits, mbx, mby);
            intraSkip8_ = intraSkip16_ = false;
            setBlocks(0, 0, 4, 4, 0, 0, -1);
            costIntra = double(mbSsd(mbx, mby)) + lambda * mbBits(intraBits, mbx, mby, double(intraBits.bw.bitCount() + seBits(0)));
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

// After a macroblock is chosen: its QP is the planned one if it sent mb_qp_delta, else the predictor.
void Encoder::commitQp(int mbx, int mby, bool codedDelta)
{
    const size_t m = size_t(mby) * mbW_ + mbx;
    int& pred = qpPredByTop_[size_t(rowKey_[size_t(mby)])];
    if (codedDelta)
        pred = mbQp_[m];
    mbQpFinal_[m] = int8_t(pred);
}

void Encoder::buildRefPlanes(RefPic& R)
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
        std::vector<std::thread> workers;
        for (int p = 1; p < parts; ++p)
            workers.emplace_back([&, p] { fn(p * H / parts, (p + 1) * H / parts); });
        fn(0, H / parts);
        for (auto& t : workers)
            t.join();
    };
    auto tap6 = [](int a, int b, int c, int d, int e, int f) { return a - 5 * b + 20 * c + 20 * d - 5 * e + f; };

    parallelRows([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* src = &R.y[size_t(std::clamp(y - M, 0, padH_ - 1)) * padW_];
            uint8_t* dst = &R.F[size_t(y) * W];
            std::memset(dst, src[0], size_t(M));
            std::memcpy(dst + M, src, size_t(padW_));
            std::memset(dst + M + padW_, src[padW_ - 1], size_t(M));
        }
    });
    parallelRows([&](int y0, int y1) {
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
    });
}

// Luma prediction of a w x h block at integer position (x0, y0) plus quarter offset (fx, fy),
// table 8-12. Output stride is 16.
void Encoder::mcLuma(const RefPic& R, int x0, int y0, int fx, int fy, int w, int h, uint8_t* out) const
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
int Encoder::sadPart(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy, bool satd) const
{
    const int px = mbx * 16 + x4 * 4, py = mby * 16 + y4 * 4;
    const int w = w4 * 4, h = h4 * 4;
    const size_t lo = size_t(py) * padW_ + px;
    int s = 0;
    if (!satd && !(mvx & 3) && !(mvy & 3)) {
        const uint8_t* ref = &R.F[size_t(py + (mvy >> 2) + kMargin) * extW_ + size_t(px + (mvx >> 2) + kMargin)];
        for (int y = 0; y < h; ++y)
            s += sadRow(&srcY_[lo + size_t(y) * padW_], ref + size_t(y) * extW_, w);
        return s;
    }
    uint8_t pred[256];
    mcLuma(R, px + (mvx >> 2), py + (mvy >> 2), mvx & 3, mvy & 3, w, h, pred);
    if (satd) {
        for (int by = 0; by < h4; ++by)
            for (int bx = 0; bx < w4; bx += 2) // partitions are at least 8 wide
                s += satd8x4U8(&srcY_[lo + size_t(by * 4) * padW_ + bx * 4], padW_, pred + by * 4 * 16 + bx * 4, 16);
        return s;
    }
    for (int y = 0; y < h; ++y)
        s += sadRow(&srcY_[lo + size_t(y) * padW_], pred + y * 16, w);
    return s;
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
int Encoder::boundaryStrength(int pbx, int pby, int qbx, int qby, bool mbEdge) const
{
    const int lw = mbW_ * 4;
    const size_t mp = size_t(pby) * lw + pbx, mq = size_t(qby) * lw + qbx;
    if (refIdx_[mp] < 0 || refIdx_[mq] < 0)
        return mbEdge ? 4 : 3;
    if (nzLuma_[size_t(pby) * lw + pbx] || nzLuma_[size_t(qby) * lw + qbx])
        return 2;
    if (refIdx_[mp] != refIdx_[mq] || std::abs(mvX_[mp] - mvX_[mq]) >= 4 || std::abs(mvY_[mp] - mvY_[mq]) >= 4)
        return 1;
    return 0;
}

void Encoder::deblockPicture()
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
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t)
        pool.emplace_back(worker);
    worker();
    for (auto& t : pool)
        t.join();
}

void Encoder::deblockMb(int mbx, int mby)
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
            for (int e = 0; e < 4; ++e)
                for (int s = 0; s < 4; ++s) {
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
void Encoder::planMacroblockQp()
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
bool Encoder::detectSceneCut()
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
                        int s = 0;
                        for (int y = 0; y < 8; ++y)
                            s += sadRow(&thumb[size_t(by + y) * tw + bx], &prevThumb_[size_t(by + my + y) * tw + bx + mx], 8);
                        best = std::min(best, s);
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
bool Encoder::skipResidualIsZero(int mbx, int mby, const uint8_t* lp, const uint8_t (*cp)[64], int qp) const
{
    const int qpDiv = qp / 6, qpMod = qp % 6;
    const int qbits = 15 + qpDiv;
    const int offset = (1 << qbits) / 6;
    const size_t lumaOff = size_t(mby * 16) * padW_ + mbx * 16;
    for (int b = 0; b < 16; ++b) {
        const int ox = (b & 3) * 4, oy = (b >> 2) * 4;
        int res[16], coef[16];
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                res[y * 4 + x] = int(srcY_[lumaOff + size_t(oy + y) * padW_ + ox + x]) - int(lp[(oy + y) * 16 + ox + x]);
        forward4x4(res, coef);
        for (int i = 0; i < 16; ++i)
            if (quantize(coef[i], kQuantMf[qpMod][posClass(i)], qbits, offset) != 0)
                return false;
    }
    const int qpc = chromaQp(qp);
    const int cDiv = qpc / 6, cMod = qpc % 6, cqbits = 15 + cDiv, cOffset = (1 << cqbits) / 6;
    const int cw = padW_ / 2;
    const size_t co = size_t(mby * 8) * cw + mbx * 8;
    for (int c = 0; c < 2; ++c) {
        const std::vector<uint8_t>& src = c ? srcV_ : srcU_;
        int dc[4];
        for (int b = 0; b < 4; ++b) {
            const int ox = (b & 1) * 4, oy = (b >> 1) * 4;
            int res[16], coef[16];
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    res[y * 4 + x] = int(src[co + size_t(oy + y) * cw + ox + x]) - int(cp[c][(oy + y) * 8 + ox + x]);
            forward4x4(res, coef);
            dc[b] = coef[0];
            for (int i = 1; i < 16; ++i)
                if (quantize(coef[i], kQuantMf[cMod][posClass(i)], cqbits, cOffset) != 0)
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
double Encoder::mbBits(const MbOut& o, int mbx, int mby, double cavlcBits)
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

double Encoder::skipBits(int mbx, int mby)
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
int Encoder::residual(BitWriter& bw, const int* coeffs, int count, int nC) const
{
    if (!useCabac())
        return writeResidualBlock(bw, coeffs, count, nC);
    int total = 0;
    for (int i = 0; i < count; ++i)
        total += coeffs[i] != 0;
    return total;
}

} // namespace schneidi::h264
