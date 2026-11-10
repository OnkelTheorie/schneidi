// schneidi's own H.264 encoder (prototype).
// Clause numbers refer to ITU-T H.264. The encoder mirrors the decoder's
// reconstruction exactly (8.3, 8.5), so later P frames can reference it.
#include "H264Encoder.h"

#include "Cavlc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace schneidi::h264 {
namespace {

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
void hadamard4x4(const int* in, int* out)
{
    int t[16];
    for (int y = 0; y < 4; ++y) {
        const int* r = in + y * 4;
        const int a = r[0] + r[1], b = r[0] - r[1], c = r[2] + r[3], d = r[2] - r[3];
        t[y * 4 + 0] = a + c;
        t[y * 4 + 1] = a - c;
        t[y * 4 + 2] = b - d;
        t[y * 4 + 3] = b + d;
    }
    for (int x = 0; x < 4; ++x) {
        const int a = t[x] + t[4 + x], b = t[x] - t[4 + x], c = t[8 + x] + t[12 + x], d = t[8 + x] - t[12 + x];
        out[x] = a + c;
        out[4 + x] = a - c;
        out[8 + x] = b - d;
        out[12 + x] = b + d;
    }
}

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
    refY_.resize(recY_.size());
    refU_.resize(recU_.size());
    refV_.resize(recV_.size());
    mvX_.resize(nzLuma_.size());
    mvY_.resize(nzLuma_.size());
    refIdx_.resize(mvX_.size());
    sliceTopOfRow_.resize(size_t(mbH_));
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
        bw.u(66, 8);   // profile_idc: Baseline
        bw.u(0xC0, 8); // constraint_set0 + constraint_set1 (constrained baseline)
        bw.u(40, 8);   // level_idc 4.0
        bw.ue(0);      // seq_parameter_set_id
        bw.ue(0);      // log2_max_frame_num_minus4
        bw.ue(2);      // pic_order_cnt_type: output order = decode order
        bw.ue(1);      // max_num_ref_frames
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
        bw.flag(false); // entropy_coding_mode_flag: CAVLC
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

std::vector<uint8_t> Encoder::encode(const Picture& picture)
{
    loadSource(picture);
    std::fill(nzLuma_.begin(), nzLuma_.end(), 0);
    std::fill(modes4x4_.begin(), modes4x4_.end(), int8_t(-1));
    std::fill(nzChroma_[0].begin(), nzChroma_[0].end(), 0);
    std::fill(nzChroma_[1].begin(), nzChroma_[1].end(), 0);

    std::fill(mvX_.begin(), mvX_.end(), int16_t(0));
    std::fill(mvY_.begin(), mvY_.end(), int16_t(0));
    std::fill(refIdx_.begin(), refIdx_.end(), int8_t(-1));

    const bool idr = cfg_.gop <= 1 || frameIndex_ % cfg_.gop == 0;
    pSlice_ = !idr;

    // Frame-level rate control: P frames predict their bits from the last P frame (bits roughly
    // halve per +6 QP) and pay back over- or undershoot over about one second; I frames
    // follow the P QP (3 lower) and their cost is paid back by the P frames.
    const int baseQp = cfg_.qp;
    if (cfg_.bitrateKbps > 0) {
        if (frameIndex_ == 0)
            rcQp_ = baseQp;
        double qp = rcQp_;
        if (idr) {
            qp = rcQp_ - 3;
        } else if (rcComplexity_[1] > 0) {
            const double perFrame = cfg_.bitrateKbps * 1000.0 / cfg_.fps;
            const double debt = rcBits_ - rcTarget_;
            const double budget = std::clamp(perFrame - debt / std::max(1.0, cfg_.fps), perFrame * 0.5, perFrame * 2.0);
            qp = std::clamp(6.0 * std::log2(rcComplexity_[1] / budget), rcQp_ - 2.0, rcQp_ + 2.0);
        }
        cfg_.qp = std::clamp(int(std::lround(qp)), 10, 51);
    } else if (idr && cfg_.gop > 1) {
        cfg_.qp = std::clamp(baseQp - 3, 0, 51); // keyframes are references for the whole GOP
    }
    lastQp_ = cfg_.qp;
    mbTypeOffset_ = idr ? 0 : 5;

    planMacroblockQp();
    const int sliceCount = std::clamp(cfg_.threads, 1, mbH_);
    std::vector<BitWriter> slices(static_cast<size_t>(sliceCount));
    std::vector<int> sliceTop(size_t(sliceCount) + 1);
    for (int s = 0; s <= sliceCount; ++s)
        sliceTop[size_t(s)] = s * mbH_ / sliceCount;
    for (int s = 0; s < sliceCount; ++s)
        for (int r = sliceTop[size_t(s)]; r < sliceTop[size_t(s) + 1]; ++r)
            sliceTopOfRow_[size_t(r)] = sliceTop[size_t(s)];

    auto encodeSlice = [&](int s) {
        BitWriter& bw = slices[size_t(s)];
        // slice_header (7.3.3)
        bw.ue(uint32_t(sliceTop[size_t(s)] * mbW_)); // first_mb_in_slice
        bw.ue(idr ? 7 : 5);                         // slice_type: I or P (all slices)
        bw.ue(0);                                   // pic_parameter_set_id
        bw.u(idr ? 0 : uint32_t((frameIndex_ % cfg_.gop) & 15), 4); // frame_num
        if (idr) {
            bw.ue(uint32_t(idrCount_ & 0xFFFF)); // idr_pic_id (differs between neighbours)
        } else {
            bw.flag(false); // num_ref_idx_active_override_flag
            bw.flag(false); // ref_pic_list_modification_flag_l0
        }
        if (idr) {
            bw.flag(false); // no_output_of_prior_pics_flag
            bw.flag(false); // long_term_reference_flag
        } else {
            bw.flag(false); // adaptive_ref_pic_marking_mode_flag
        }
        bw.se(cfg_.qp - 26);         // slice_qp_delta
        bw.ue(cfg_.deblock ? 0 : 1); // disable_deblocking_filter_idc (0 filters across slice edges too)
        if (cfg_.deblock) {
            bw.se(0); // slice_alpha_c0_offset_div2
            bw.se(0); // slice_beta_offset_div2
        }
        int skipRun = 0;
        qpPredByTop_[size_t(sliceTop[size_t(s)])] = cfg_.qp;
        for (int mby = sliceTop[size_t(s)]; mby < sliceTop[size_t(s) + 1]; ++mby) {
            for (int mbx = 0; mbx < mbW_; ++mbx) {
                if (cfg_.mode == IntraMode::Pcm)
                    encodeMbPcm(bw, mbx, mby);
                else if (pSlice_)
                    encodeMbP(bw, mbx, mby, skipRun);
                else
                    commitQp(mbx, mby, encodeMbIntra(bw, mbx, mby));
            }
        }
        if (skipRun)
            bw.ue(uint32_t(skipRun));
        bw.rbspTrailing();
    };
    // Slices only look at macroblocks of their own slice, so they run in parallel.
    std::vector<std::thread> workers;
    for (int s = 1; s < sliceCount; ++s)
        workers.emplace_back(encodeSlice, s);
    encodeSlice(0);
    for (auto& t : workers)
        t.join();

    std::vector<uint8_t> out;
    for (const BitWriter& bw : slices) {
        if (idr)
            appendNal(out, 3, 5, bw.bytes());
        else
            appendNal(out, 2, 1, bw.bytes());
    }
    if (cfg_.deblock)
        deblockPicture();
    prevMvX_ = mvX_;
    prevMvY_ = mvY_;
    prevRefIdx_ = refIdx_;
    if (cfg_.gop > 1) {
        refY_ = recY_;
        refU_ = recU_;
        refV_ = recV_;
        buildRefPlanes();
    }
    if (cfg_.bitrateKbps <= 0)
        cfg_.qp = baseQp;
    if (cfg_.bitrateKbps > 0) {
        const double bits = double(out.size()) * 8.0;
        if (!idr) {
            rcComplexity_[1] = bits * std::pow(2.0, cfg_.qp / 6.0);
            rcQp_ = cfg_.qp;
        }
        rcBits_ += bits;
        rcTarget_ += cfg_.bitrateKbps * 1000.0 / cfg_.fps;
        cfg_.qp = baseQp;
    }
    if (idr)
        ++idrCount_;
    ++frameIndex_;
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

[[maybe_unused]] int satd4x4(const int* diff);

// SATD of a 4x4 block between two 8-bit images; same result as satd4x4 on their difference.
inline int satdU8(const uint8_t* a, int strideA, const uint8_t* b, int strideB)
{
#if defined(__SSE2__) || defined(_M_X64)
    const __m128i zero = _mm_setzero_si128();
    auto row = [&](int i) {
        int32_t wa, wb;
        std::memcpy(&wa, a + i * strideA, 4);
        std::memcpy(&wb, b + i * strideB, 4);
        return _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(wa), zero), _mm_unpacklo_epi8(_mm_cvtsi32_si128(wb), zero));
    };
    const __m128i r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    // Vertical 4-point Hadamard (lanes = columns).
    const __m128i s01 = _mm_add_epi16(r0, r1), d01 = _mm_sub_epi16(r0, r1);
    const __m128i s23 = _mm_add_epi16(r2, r3), d23 = _mm_sub_epi16(r2, r3);
    const __m128i t0 = _mm_add_epi16(s01, s23), t1 = _mm_sub_epi16(s01, s23);
    const __m128i t2 = _mm_sub_epi16(d01, d23), t3 = _mm_add_epi16(d01, d23);
    // Transpose: v0 = columns 0|1, v1 = columns 2|3.
    const __m128i u0 = _mm_unpacklo_epi16(t0, t1), u1 = _mm_unpacklo_epi16(t2, t3);
    const __m128i v0 = _mm_unpacklo_epi32(u0, u1), v1 = _mm_unpackhi_epi32(u0, u1);
    const __m128i c1 = _mm_unpackhi_epi64(v0, v0), c3 = _mm_unpackhi_epi64(v1, v1);
    // Horizontal 4-point Hadamard on the columns.
    const __m128i ha = _mm_add_epi16(v0, c1), hb = _mm_sub_epi16(v0, c1);
    const __m128i hc = _mm_add_epi16(v1, c3), hd = _mm_sub_epi16(v1, c3);
    const __m128i o01 = _mm_unpacklo_epi64(_mm_add_epi16(ha, hc), _mm_sub_epi16(ha, hc));
    const __m128i o23 = _mm_unpacklo_epi64(_mm_sub_epi16(hb, hd), _mm_add_epi16(hb, hd));
    auto absv = [&](__m128i x) { return _mm_max_epi16(x, _mm_sub_epi16(zero, x)); };
    const __m128i sum = _mm_madd_epi16(_mm_add_epi16(absv(o01), absv(o23)), _mm_set1_epi16(1));
    const __m128i s2 = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2)));
    const __m128i s1 = _mm_add_epi32(s2, _mm_shuffle_epi32(s2, _MM_SHUFFLE(2, 3, 0, 1)));
    return (_mm_cvtsi128_si32(s1) + 1) >> 1;
#else
    int diff[16];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            diff[y * 4 + x] = int(a[y * strideA + x]) - int(b[y * strideB + x]);
    return satd4x4(diff);
#endif
}

[[maybe_unused]] int satd4x4(const int* diff)
{
    int t[16];
    hadamard4x4(diff, t);
    int s = 0;
    for (int v : t)
        s += std::abs(v);
    return (s + 1) >> 1;
}

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

void Encoder::writeChroma(BitWriter& bw, int mbx, int mby, const ChromaMb& ch)
{
    if (ch.cbp) {
        for (int c = 0; c < 2; ++c)
            writeResidualBlock(bw, ch.dc[c], 4, -1);
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
                total = writeResidualBlock(bw, scan, 15, chromaNc(c, bx, by));
            }
            nzChroma_[c][size_t(by) * chw + bx] = uint8_t(total);
        }
    }
}

bool Encoder::encodeLuma16(BitWriter& bw, int mbx, int mby, const ChromaMb& ch)
{
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
    bw.ue(uint32_t(mbTypeOffset_ + 1 + bestMode + 4 * ch.cbp + (cbpLuma ? 12 : 0))); // mb_type I_16x16_*
    bw.ue(uint32_t(ch.mode)); // intra_chroma_pred_mode
    bw.se(qp - qpPredOf(mby)); // mb_qp_delta (always present for Intra 16x16)

    const int mbBx = mbx * 4, mbBy = mby * 4, lw = mbW_ * 4;
    {
        int scan[16];
        for (int k = 0; k < 16; ++k)
            scan[k] = dcLevel[kZigzag[k]];
        writeResidualBlock(bw, scan, 16, lumaNc(mbBx, mbBy));
    }
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        const int bx = mbBx + kBlkX[blkIdx], by = mbBy + kBlkY[blkIdx];
        int total = 0;
        if (cbpLuma) {
            const int b = kBlkY[blkIdx] * 4 + kBlkX[blkIdx];
            int scan[15];
            for (int k = 1; k < 16; ++k)
                scan[k - 1] = acLevel[b][kZigzag[k]];
            total = writeResidualBlock(bw, scan, 15, lumaNc(bx, by));
        }
        nzLuma_[size_t(by) * lw + bx] = uint8_t(total);
        modes4x4_[size_t(by) * lw + bx] = -1;
    }
    writeChroma(bw, mbx, mby, ch);
    return true;
}

bool Encoder::encodeLuma4(BitWriter& bw, int mbx, int mby, const ChromaMb& ch)
{
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
    if (cbp)
        bw.se(qp - qpPredOf(mby)); // mb_qp_delta
    for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
        const int bx = mbx * 4 + kBlkX[blkIdx], by = mby * 4 + kBlkY[blkIdx];
        int total = 0;
        if (cbpLuma & (1 << (blkIdx >> 2))) {
            int scan[16];
            for (int k = 0; k < 16; ++k)
                scan[k] = levels[blkIdx][kZigzag[k]];
            total = writeResidualBlock(bw, scan, 16, lumaNc(bx, by));
        }
        nzLuma_[size_t(by) * lw + bx] = uint8_t(total);
    }
    writeChroma(bw, mbx, mby, ch);
    return cbp != 0;
}

bool Encoder::encodeMbIntra(BitWriter& bw, int mbx, int mby)
{
    ChromaMb ch;
    analyseChroma(mbx, mby, ch);
    if (cfg_.mode == IntraMode::Intra16)
        return encodeLuma16(bw, mbx, mby, ch);
    if (cfg_.mode == IntraMode::Intra4)
        return encodeLuma4(bw, mbx, mby, ch);

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
    BitWriter a, b;
    const bool deltaA = encodeLuma16(a, mbx, mby, ch);
    const double costA = double(ssd()) + lambda * double(a.bitCount());
    Snapshot snapA;
    save(snapA);
    const bool deltaB = encodeLuma4(b, mbx, mby, ch);
    const double costB = double(ssd()) + lambda * double(b.bitCount());
    if (costA <= costB) {
        restore(snapA);
        bw.append(a);
        return deltaA;
    }
    bw.append(b);
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

int seBits(int v)
{
    const uint32_t k = v > 0 ? uint32_t(2 * v - 1) : uint32_t(-2 * v);
    int len = 0;
    while (((k + 1) >> len) > 1)
        ++len;
    return 2 * len + 1;
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

// SAD of one row of 8 or 16 samples (SSE2 where available, plain C elsewhere).
inline int sadRow(const uint8_t* a, const uint8_t* b, int w)
{
#if defined(__SSE2__) || defined(_M_X64)
    if (w == 16) {
        const __m128i s = _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)),
                                       _mm_loadu_si128(reinterpret_cast<const __m128i*>(b)));
        return _mm_cvtsi128_si32(s) + _mm_extract_epi16(s, 4);
    }
    if (w == 8) {
        const __m128i s = _mm_sad_epu8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(a)),
                                       _mm_loadl_epi64(reinterpret_cast<const __m128i*>(b)));
        return _mm_cvtsi128_si32(s);
    }
#endif
    int s = 0;
    for (int x = 0; x < w; ++x)
        s += std::abs(int(a[x]) - int(b[x]));
    return s;
}

int median3(int a, int b, int c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

} // namespace

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
void Encoder::predictMvPart(int mbx, int mby, int x4, int y4, int w4, int shape, const bool* decoded, int& mvx,
                            int& mvy) const
{
    MvNb a = mvNeighbour(mbx, mby, x4 - 1, y4, decoded);
    MvNb b = mvNeighbour(mbx, mby, x4, y4 - 1, decoded);
    MvNb c = mvNeighbour(mbx, mby, x4 + w4, y4 - 1, decoded);
    if (!c.avail)
        c = mvNeighbour(mbx, mby, x4 - 1, y4 - 1, decoded); // D replaces C
    const MvNb* directional = shape == 1 ? &b : shape == 2 ? &a : shape == 3 ? &a : shape == 4 ? &c : nullptr;
    if (directional && directional->ref == 0) {
        mvx = directional->mvx;
        mvy = directional->mvy;
        return;
    }
    if (!b.avail && !c.avail && a.avail)
        b = c = a;
    const int matches = (a.ref == 0) + (b.ref == 0) + (c.ref == 0);
    if (matches == 1) {
        const MvNb& only = a.ref == 0 ? a : b.ref == 0 ? b : c;
        mvx = only.mvx;
        mvy = only.mvy;
        return;
    }
    mvx = median3(a.mvx, b.mvx, c.mvx);
    mvy = median3(a.mvy, b.mvy, c.mvy);
}

// Motion compensation of one partition into macroblock-sized prediction buffers.
void Encoder::motionCompensate(int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy, uint8_t* luma,
                               uint8_t (*chroma)[64]) const
{
    mcLuma(mbx * 16 + x4 * 4 + (mvx >> 2), mby * 16 + y4 * 4 + (mvy >> 2), mvx & 3, mvy & 3, w4 * 4, h4 * 4,
           luma + y4 * 4 * 16 + x4 * 4);
    // Chroma: eighth-pel bilinear (8.4.2.2.2).
    const int cw = padW_ / 2, chh = padH_ / 2;
    const int xF = mvx & 7, yF = mvy & 7;
    const std::vector<uint8_t>* planes[2] = {&refU_, &refV_};
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

void Encoder::encodeMbP(BitWriter& bw, int mbx, int mby, int& skipRun)
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
    predictMvPart(mbx, mby, 0, 0, 4, 0, none, mvpX, mvpY);

    // P_Skip vector (8.4.1.1).
    int skipX = mvpX, skipY = mvpY;
    {
        const MvNb a = mvNeighbour(mbx, mby, -1, 0, none), b = mvNeighbour(mbx, mby, 0, -1, none);
        if (!a.avail || !b.avail || (a.ref == 0 && a.mvx == 0 && a.mvy == 0) || (b.ref == 0 && b.mvx == 0 && b.mvy == 0))
            skipX = skipY = 0;
    }

    // ---- Candidate 1: P_Skip.
    uint8_t lp[256], cp[2][64];
    motionCompensate(mbx, mby, 0, 0, 4, 4, skipX, skipY, lp, cp);
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
    const double costSkip = double(mbSsd(mbx, mby)) + lambda * 1.0;
    MbState skipState;
    saveMb(mbx, mby, skipState);

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
    auto search = [&](int x4, int y4, int w4, int h4, int pX, int pY, int& outX, int& outY) {
        starts.push_back({pX, pY});
        bool useSatd = false;
        auto cost = [&](int vx, int vy) {
            return double(sadPart(mbx, mby, x4, y4, w4, h4, vx, vy, useSatd)) +
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
        for (int step : {32, 16, 8, 4}) {
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
        for (int step : {2, 1}) {
            const int cx0 = bx, cy0 = by;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy)
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
    };
    auto tryLayout = [&](int li) {
        const Layout& L = kLayouts[li];
        Choice ch;
        ch.layout = li;
        ch.cost = lambdaSad * L.typeBits;
        bool decoded[16] = {};
        for (int p = 0; p < L.count; ++p) {
            const Part& P = L.parts[p];
            int pX, pY;
            predictMvPart(mbx, mby, P.x4, P.y4, P.w4, P.shape, decoded, pX, pY);
            int vx, vy;
            ch.cost += search(P.x4, P.y4, P.w4, P.h4, pX, pY, vx, vy);
            ch.mv[p][0] = vx;
            ch.mv[p][1] = vy;
            ch.mvp[p][0] = pX;
            ch.mvp[p][1] = pY;
            setBlocks(P.x4, P.y4, P.w4, P.h4, vx, vy, 0);
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
    auto encodeInter = [&](const Choice& best, BitWriter& interBits) {
        const Layout& layout = kLayouts[best.layout];
        for (int p = 0; p < layout.count; ++p) {
            const Part& P = layout.parts[p];
            motionCompensate(mbx, mby, P.x4, P.y4, P.w4, P.h4, best.mv[p][0], best.mv[p][1], lp, cp);
            setBlocks(P.x4, P.y4, P.w4, P.h4, best.mv[p][0], best.mv[p][1], 0);
        }
        const int qpDiv = qp / 6, qpMod = qp % 6;
        const int qbits = 15 + qpDiv;
        const int interOffset = (1 << qbits) / 6;
        int levels[16][16];
        int cbpLuma = 0;
        for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
            const int ox = kBlkX[blkIdx] * 4, oy = kBlkY[blkIdx] * 4;
            int res[16], coef[16];
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    res[y * 4 + x] = int(srcY_[lumaOff + size_t(oy + y) * padW_ + ox + x]) - int(lp[(oy + y) * 16 + ox + x]);
            forward4x4(res, coef);
            if (cfg_.effort > 0) {
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
            scores[blkIdx] = decimateScore(levels[blkIdx]);
            mbScore += scores[blkIdx];
        }
        for (int b8 = 0; b8 < 4; ++b8) {
            const int s = scores[b8 * 4] + scores[b8 * 4 + 1] + scores[b8 * 4 + 2] + scores[b8 * 4 + 3];
            if (s < 4 || mbScore < 6)
                for (int k = 0; k < 4; ++k)
                    std::fill(levels[b8 * 4 + k], levels[b8 * 4 + k] + 16, 0);
        }
        for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
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

        interBits.ue(uint32_t(layout.mbType));
        if (layout.mbType == 3)
            for (int p = 0; p < 4; ++p)
                interBits.ue(0); // sub_mb_type P_L0_8x8
        for (int p = 0; p < layout.count; ++p) {
            interBits.se(best.mv[p][0] - best.mvp[p][0]);
            interBits.se(best.mv[p][1] - best.mvp[p][1]);
        }
        const int cbp = cbpLuma | (ch.cbp << 4);
        interBits.ue(uint32_t(interCbpCode(cbp)));
        if (cbp)
            interBits.se(qp - qpPredOf(mby)); // mb_qp_delta
        interDelta = cbp != 0;
        for (int blkIdx = 0; blkIdx < 16; ++blkIdx) {
            const int bx = mbx * 4 + kBlkX[blkIdx], by = mby * 4 + kBlkY[blkIdx];
            int total = 0;
            if (cbpLuma & (1 << (blkIdx >> 2))) {
                int scan[16];
                for (int k = 0; k < 16; ++k)
                    scan[k] = levels[blkIdx][kZigzag[k]];
                total = writeResidualBlock(interBits, scan, 16, lumaNc(bx, by));
            }
            nzLuma_[size_t(by) * lw + bx] = uint8_t(total);
        }
        writeChroma(interBits, mbx, mby, ch);
        // A skip_run ends here: count its codeword to be fair against skip.
        return double(mbSsd(mbx, mby)) + lambda * double(interBits.bitCount() + seBits(0));
    };
    BitWriter interBits;
    double costInter = encodeInter(whole, interBits);
    bool chosenInterDelta = interDelta;
    MbState interState;
    saveMb(mbx, mby, interState);
    Choice best = whole;
    // Try the split for real when its estimate is close or better.
    if (split.cost < whole.cost * 1.1) {
        BitWriter splitBits;
        const double costSplit = encodeInter(split, splitBits);
        if (costSplit < costInter) {
            costInter = costSplit;
            chosenInterDelta = interDelta;
            interBits = std::move(splitBits);
            saveMb(mbx, mby, interState);
            best = split;
        }
    }

    // ---- Candidate 3: intra, only when a cheap Intra 16x16 SATD estimate beats the motion search.
    BitWriter intraBits;
    double costIntra = 1e300;
    bool intraDelta = false;
    {
        const bool left = mbx > 0, top = topMbAvailable(mby);
        uint8_t src[256], cand[256];
        for (int y = 0; y < 16; ++y)
            std::memcpy(src + y * 16, &srcY_[lumaOff + size_t(y) * padW_], 16);
        int intraEstimate = 1 << 30;
        for (int mode = 0; mode < 4; ++mode) {
            if ((mode == 0 && !top) || (mode == 1 && !left) || (mode == 3 && !(top && left)))
                continue;
            predictLuma16(mode, &recY_[lumaOff], padW_, left, top, cand);
            int sum = 0;
            for (int b = 0; b < 16; ++b) {
                const int ox = (b & 3) * 4, oy = (b >> 2) * 4;
                sum += satdU8(src + oy * 16 + ox, 16, cand + oy * 16 + ox, 16);
            }
            intraEstimate = std::min(intraEstimate, sum);
        }
        // recY_ of this macroblock holds the inter reconstruction here, but Intra 16x16
        // prediction only reads the neighbours above and to the left.
        if (double(intraEstimate) < best.cost * 2.0) {
            intraDelta = encodeMbIntra(intraBits, mbx, mby);
            setBlocks(0, 0, 4, 4, 0, 0, -1);
            costIntra = double(mbSsd(mbx, mby)) + lambda * double(intraBits.bitCount() + seBits(0));
        }
    }

    if (costSkip <= costInter && costSkip <= costIntra) {
        restoreMb(mbx, mby, skipState);
        ++skipRun;
        mbQpFinal_[mbIndex] = int8_t(qpPredOf(mby));
        return;
    }
    bw.ue(uint32_t(skipRun));
    skipRun = 0;
    bool coded;
    if (costInter <= costIntra) {
        restoreMb(mbx, mby, interState);
        bw.append(interBits);
        coded = chosenInterDelta;
    } else {
        bw.append(intraBits); // state is already the intra one
        coded = intraDelta;
    }
    commitQp(mbx, mby, coded);
}

// After a macroblock is chosen: its QP is the planned one if it sent mb_qp_delta, else the predictor.
void Encoder::commitQp(int mbx, int mby, bool codedDelta)
{
    const size_t m = size_t(mby) * mbW_ + mbx;
    int& pred = qpPredByTop_[size_t(sliceTopOfRow_[size_t(mby)])];
    if (codedDelta)
        pred = mbQp_[m];
    mbQpFinal_[m] = int8_t(pred);
}

void Encoder::buildRefPlanes()
{
    const int M = kMargin;
    extW_ = padW_ + 2 * M;
    extH_ = padH_ + 2 * M;
    const size_t n = size_t(extW_) * extH_;
    extF_.resize(n);
    extB_.resize(n);
    extV_.resize(n);
    extJ_.resize(n);
    vInter_.resize(n);
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
            const uint8_t* src = &refY_[size_t(std::clamp(y - M, 0, padH_ - 1)) * padW_];
            uint8_t* dst = &extF_[size_t(y) * W];
            std::memset(dst, src[0], size_t(M));
            std::memcpy(dst + M, src, size_t(padW_));
            std::memset(dst + M + padW_, src[padW_ - 1], size_t(M));
        }
    });
    parallelRows([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* r[6];
            for (int k = 0; k < 6; ++k)
                r[k] = &extF_[size_t(std::clamp(y - 2 + k, 0, H - 1)) * W];
            const uint8_t* row = r[2];
            int* vi = &vInter_[size_t(y) * W];
            uint8_t* vb = &extV_[size_t(y) * W];
            uint8_t* hb = &extB_[size_t(y) * W];
            for (int x = 0; x < W; ++x) {
                const int vv = tap6(r[0][x], r[1][x], r[2][x], r[3][x], r[4][x], r[5][x]);
                vi[x] = vv;
                vb[x] = clip1((vv + 16) >> 5);
                int bb;
                if (x >= 2 && x + 3 < W)
                    bb = tap6(row[x - 2], row[x - 1], row[x], row[x + 1], row[x + 2], row[x + 3]);
                else
                    bb = tap6(row[std::clamp(x - 2, 0, W - 1)], row[std::clamp(x - 1, 0, W - 1)], row[x],
                              row[std::min(x + 1, W - 1)], row[std::min(x + 2, W - 1)], row[std::min(x + 3, W - 1)]);
                hb[x] = clip1((bb + 16) >> 5);
            }
        }
    });
    parallelRows([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int* v = &vInter_[size_t(y) * W];
            uint8_t* j = &extJ_[size_t(y) * W];
            for (int x = 0; x < W; ++x) {
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
void Encoder::mcLuma(int x0, int y0, int fx, int fy, int w, int h, uint8_t* out) const
{
    const int M = kMargin;
    const int W = extW_;
    const size_t base = size_t(y0 + M) * W + size_t(x0 + M);
    // Each quarter position is one plane or the rounded average of two (table 8-12):
    // pick both sources once per block so the row loops stay simple.
    const uint8_t* a = nullptr;
    const uint8_t* b = nullptr;
    const uint8_t* F = extF_.data() + base;
    const uint8_t* B = extB_.data() + base;
    const uint8_t* V = extV_.data() + base;
    const uint8_t* J = extJ_.data() + base;
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
int Encoder::sadPart(int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy, bool satd) const
{
    const int px = mbx * 16 + x4 * 4, py = mby * 16 + y4 * 4;
    const int w = w4 * 4, h = h4 * 4;
    const size_t lo = size_t(py) * padW_ + px;
    int s = 0;
    if (!satd && !(mvx & 3) && !(mvy & 3)) {
        const uint8_t* ref = &extF_[size_t(py + (mvy >> 2) + kMargin) * extW_ + size_t(px + (mvx >> 2) + kMargin)];
        for (int y = 0; y < h; ++y)
            s += sadRow(&srcY_[lo + size_t(y) * padW_], ref + size_t(y) * extW_, w);
        return s;
    }
    uint8_t pred[256];
    mcLuma(px + (mvx >> 2), py + (mvy >> 2), mvx & 3, mvy & 3, w, h, pred);
    if (satd) {
        for (int by = 0; by < h4; ++by)
            for (int bx = 0; bx < w4; ++bx) {
                s += satdU8(&srcY_[lo + size_t(by * 4) * padW_ + bx * 4], padW_, pred + by * 4 * 16 + bx * 4, 16);
            }
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
    if (std::abs(mvX_[mp] - mvX_[mq]) >= 4 || std::abs(mvY_[mp] - mvY_[mq]) >= 4)
        return 1;
    return 0;
}

void Encoder::deblockPicture()
{
    const int cw = padW_ / 2;
    for (int mby = 0; mby < mbH_; ++mby) {
        for (int mbx = 0; mbx < mbW_; ++mbx) {
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
            // Luma: vertical edges left to right, then horizontal edges top to bottom.
            for (int e = 0; e < 4; ++e)
                for (int s = 0; s < 4; ++s) {
                    if (!bsV[e][s])
                        continue;
                    for (int k = 0; k < 4; ++k) {
                        uint8_t* q = &recY_[size_t(mby * 16 + s * 4 + k) * padW_ + mbx * 16 + e * 4];
                        const EdgeQp& eq = e == 0 ? leftEdge : inner;
                        filterLine(q, 1, bsV[e][s], eq.alphaY, eq.betaY, eq.indexY, false);
                    }
                }
            for (int e = 0; e < 4; ++e)
                for (int s = 0; s < 4; ++s) {
                    if (!bsH[e][s])
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
    if (cfg_.aqStrength <= 0 || cfg_.mode == IntraMode::Pcm) {
        std::fill(mbQp_.begin(), mbQp_.end(), int8_t(cfg_.qp));
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
        const double offset = std::clamp(cfg_.aqStrength * (mbEnergy_[size_t(m)] - mean) * 0.5, -8.0, 8.0);
        mbQp_[size_t(m)] = int8_t(std::clamp(int(std::lround(cfg_.qp + offset)), 0, 51));
    }
}

} // namespace schneidi::h264
