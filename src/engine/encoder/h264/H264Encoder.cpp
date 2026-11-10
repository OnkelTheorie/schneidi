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
        bw.flag(false); // vui_parameters_present_flag
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

    BitWriter bw;
    // slice_header (7.3.3), IDR I slice
    bw.ue(0);                               // first_mb_in_slice
    bw.ue(7);                               // slice_type: I (all slices)
    bw.ue(0);                               // pic_parameter_set_id
    bw.u(0, 4);                             // frame_num
    bw.ue(uint32_t(frameIndex_ & 0xFFFF));  // idr_pic_id (differs between neighbours)
    bw.flag(false);                         // no_output_of_prior_pics_flag
    bw.flag(false);                         // long_term_reference_flag
    bw.se(cfg_.qp - 26);                    // slice_qp_delta
    bw.ue(cfg_.deblock ? 0 : 1);            // disable_deblocking_filter_idc
    if (cfg_.deblock) {
        bw.se(0); // slice_alpha_c0_offset_div2
        bw.se(0); // slice_beta_offset_div2
    }

    for (int mby = 0; mby < mbH_; ++mby) {
        for (int mbx = 0; mbx < mbW_; ++mbx) {
            if (cfg_.mode == IntraMode::Pcm)
                encodeMbPcm(bw, mbx, mby);
            else
                encodeMbIntra(bw, mbx, mby);
        }
    }
    bw.rbspTrailing();

    std::vector<uint8_t> out;
    appendNal(out, 3, 5, bw.bytes());
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
    const bool hasA = bx > 0, hasB = by > 0;
    const int nA = hasA ? nzLuma_[size_t(by) * w + bx - 1] : 0;
    const int nB = hasB ? nzLuma_[size_t(by - 1) * w + bx] : 0;
    if (hasA && hasB)
        return (nA + nB + 1) >> 1;
    return hasA ? nA : nB;
}

int Encoder::chromaNc(int c, int bx, int by) const
{
    const int w = mbW_ * 2;
    const bool hasA = bx > 0, hasB = by > 0;
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

int satd4x4(const int* diff)
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

void Encoder::analyseChroma(int mbx, int mby, ChromaMb& ch)
{
    const bool left = mbx > 0, top = mby > 0;
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
    for (int mode = 0; mode < 4; ++mode) {
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

    const int qpc = chromaQp(cfg_.qp);
    const int qpcDiv = qpc / 6, qpcMod = qpc % 6;
    const int cqbits = 15 + qpcDiv;
    const int cOffset = (1 << cqbits) / 3;
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

void Encoder::encodeLuma16(BitWriter& bw, int mbx, int mby, const ChromaMb& ch)
{
    const bool left = mbx > 0, top = mby > 0;
    const int qp = cfg_.qp;
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
    bw.ue(uint32_t(1 + bestMode + 4 * ch.cbp + (cbpLuma ? 12 : 0))); // mb_type I_16x16_*
    bw.ue(uint32_t(ch.mode)); // intra_chroma_pred_mode
    bw.se(0);                 // mb_qp_delta

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
}

void Encoder::encodeLuma4(BitWriter& bw, int mbx, int mby, const ChromaMb& ch)
{
    const int qp = cfg_.qp;
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
        const bool left = px > 0, top = py > 0;
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
            int diff[16];
            for (int i = 0; i < 16; ++i)
                diff[i] = src[i] - cand[i];
            const int cost = satd4x4(diff) + lambda * (mode == predMode ? 1 : 4);
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
    bw.ue(0);
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
        bw.se(0); // mb_qp_delta
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
}

void Encoder::encodeMbIntra(BitWriter& bw, int mbx, int mby)
{
    ChromaMb ch;
    analyseChroma(mbx, mby, ch);
    if (cfg_.mode == IntraMode::Intra16) {
        encodeLuma16(bw, mbx, mby, ch);
        return;
    }
    if (cfg_.mode == IntraMode::Intra4) {
        encodeLuma4(bw, mbx, mby, ch);
        return;
    }

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

    const double lambda = 0.85 * std::pow(2.0, (cfg_.qp - 12) / 3.0);
    BitWriter a, b;
    encodeLuma16(a, mbx, mby, ch);
    const double costA = double(ssd()) + lambda * double(a.bitCount());
    Snapshot snapA;
    save(snapA);
    encodeLuma4(b, mbx, mby, ch);
    const double costB = double(ssd()) + lambda * double(b.bitCount());
    if (costA <= costB) {
        restore(snapA);
        bw.append(a);
    } else {
        bw.append(b);
    }
}

} // namespace schneidi::h264
