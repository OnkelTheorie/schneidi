// schneidi's own H.264 encoder (prototype).
// Clause numbers refer to ITU-T H.264. The encoder mirrors the decoder's
// reconstruction exactly (8.3, 8.5), so later P frames can reference it.
#include "H264Encoder.h"

#include "Cavlc.h"

#include <algorithm>
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
                encodeMbIntra16(bw, mbx, mby);
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

void Encoder::encodeMbIntra16(BitWriter& bw, int mbx, int mby)
{
    const bool left = mbx > 0, top = mby > 0;
    const int qp = cfg_.qp;
    const int qpDiv = qp / 6, qpMod = qp % 6;

    // ---- Luma: pick the prediction mode with the lowest SAD.
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

    // Transform each 4x4 block (indexed spatially: b = y * 4 + x).
    int coef[16][16];
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

    // Luma DC: Hadamard over the 16 DC terms, quantised with qbits + 1.
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

    // Reconstruct exactly like the decoder (8.5.10, 8.5.12).
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

    // ---- Chroma: one mode for both components.
    const int cw = padW_ / 2;
    const size_t chromaOff = size_t(mby * 8) * cw + mbx * 8;
    const std::vector<uint8_t>* csrc[2] = {&srcU_, &srcV_};
    std::vector<uint8_t>* crec[2] = {&recU_, &recV_};
    uint8_t csrcBlk[2][64];
    for (int c = 0; c < 2; ++c)
        for (int y = 0; y < 8; ++y)
            std::memcpy(csrcBlk[c] + y * 8, &(*csrc[c])[chromaOff + size_t(y) * cw], 8);

    uint8_t cpred[2][64], ccand[2][64];
    int chromaMode = 0;
    bestCost = 1 << 30;
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
            chromaMode = mode;
            std::memcpy(cpred, ccand, sizeof(cpred));
        }
    }

    const int qpc = chromaQp(qp);
    const int qpcDiv = qpc / 6, qpcMod = qpc % 6;
    const int cqbits = 15 + qpcDiv;
    const int cOffset = (1 << cqbits) / 3;
    int cDcLevel[2][4];
    int cAcLevel[2][4][16];
    bool anyChromaDc = false, anyChromaAc = false;
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
            cDcLevel[c][i] = quantize(f[i], kQuantMf[qpcMod][0], cqbits + 1, 2 * cOffset);
            anyChromaDc |= cDcLevel[c][i] != 0;
        }
        for (int blk = 0; blk < 4; ++blk) {
            cAcLevel[c][blk][0] = 0;
            for (int i = 1; i < 16; ++i) {
                cAcLevel[c][blk][i] = quantize(ccoef[blk][i], kQuantMf[qpcMod][posClass(i)], cqbits, cOffset);
                anyChromaAc |= cAcLevel[c][blk][i] != 0;
            }
        }
    }
    const int cbpChroma = anyChromaAc ? 2 : anyChromaDc ? 1 : 0;

    for (int c = 0; c < 2; ++c) {
        const int* l = cDcLevel[c];
        const int f[4] = {l[0] + l[1] + l[2] + l[3], l[0] - l[1] + l[2] - l[3], l[0] + l[1] - l[2] - l[3],
                          l[0] - l[1] - l[2] + l[3]};
        uint8_t* r0 = &(*crec[c])[chromaOff];
        for (int blk = 0; blk < 4; ++blk) {
            int d[16];
            d[0] = ((f[blk] * 16 * kDequantV[qpcMod][0]) << qpcDiv) >> 5;
            for (int i = 1; i < 16; ++i)
                d[i] = (cAcLevel[c][blk][i] * kDequantV[qpcMod][posClass(i)]) << qpcDiv;
            int r[16];
            inverse4x4(d, r);
            const int ox = (blk & 1) * 4, oy = (blk >> 1) * 4;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    r0[size_t(oy + y) * cw + ox + x] = clip1(cpred[c][(oy + y) * 8 + ox + x] + r[y * 4 + x]);
        }
    }

    // ---- Syntax (7.3.5).
    bw.ue(uint32_t(1 + bestMode + 4 * cbpChroma + (cbpLuma ? 12 : 0))); // mb_type I_16x16_*
    bw.ue(uint32_t(chromaMode)); // intra_chroma_pred_mode
    bw.se(0);                    // mb_qp_delta

    const int mbBx = mbx * 4, mbBy = mby * 4;
    {
        int scan[16];
        for (int k = 0; k < 16; ++k)
            scan[k] = dcLevel[kZigzag[k]];
        writeResidualBlock(bw, scan, 16, lumaNc(mbBx, mbBy));
    }
    const int lw = mbW_ * 4;
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
    }

    if (cbpChroma) {
        for (int c = 0; c < 2; ++c)
            writeResidualBlock(bw, cDcLevel[c], 4, -1);
    }
    const int chw = mbW_ * 2;
    for (int c = 0; c < 2; ++c) {
        for (int blk = 0; blk < 4; ++blk) {
            const int bx = mbx * 2 + (blk & 1), by = mby * 2 + (blk >> 1);
            int total = 0;
            if (cbpChroma == 2) {
                int scan[15];
                for (int k = 1; k < 16; ++k)
                    scan[k - 1] = cAcLevel[c][blk][kZigzag[k]];
                total = writeResidualBlock(bw, scan, 15, chromaNc(c, bx, by));
            }
            nzChroma_[c][size_t(by) * chw + bx] = uint8_t(total);
        }
    }
}

} // namespace schneidi::h264
