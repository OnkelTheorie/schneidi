// schneidi's own H.264 encoder (prototype).
// Pure C++20, no external libraries: YUV 4:2:0 8-bit frames in, Annex B H.264 out.
// Baseline profile, CAVLC: I (I_PCM, Intra 16x16, Intra 4x4) and P frames (16x16..8x8, quarter-pel).
#pragma once

#include "BitWriter.h"

#include <cstdint>
#include <vector>

namespace schneidi::h264 {

enum class IntraMode {
    Pcm,     // raw samples, only to prove the bitstream structure
    Intra16, // Intra 16x16 prediction + 4x4 transform + CAVLC
    Intra4,  // Intra 4x4 prediction (9 directions per 4x4 block)
    Auto,    // per macroblock the cheaper of Intra16 and Intra4 (rate-distortion)
};

struct EncoderConfig {
    int width = 0;  // even
    int height = 0; // even
    int qp = 26;    // 0..51 (values below ~10 may clip levels)
    IntraMode mode = IntraMode::Auto;
    bool deblock = true; // in-loop deblocking filter (8.7), run by the encoder too
    int gop = 1;         // keyframe distance; > 1 adds P frames (quarter-pel motion, 16x16)
    int threads = 1;     // one slice per thread (rows of macroblocks)
    int effort = 1;      // 0 = fast (16x16 motion only), 1 = default (partitions, more RD)
    int bitrateKbps = 0; // > 0: frame-level rate control towards this bitrate, qp is the start value
    double fps = 60.0;   // for the rate control and the VUI timing
    double aqStrength = 1.0; // adaptive quantisation (finer QP on flat areas); 0 = off
    int refs = 1;        // reference pictures for P frames (1..16)
};

struct Picture {
    const uint8_t* y = nullptr;
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    int strideY = 0;
    int strideC = 0;
};

class Encoder {
public:
    explicit Encoder(const EncoderConfig& config);

    // SPS + PPS as Annex B.
    std::vector<uint8_t> headers() const;
    // One access unit (IDR or P picture) as Annex B.
    std::vector<uint8_t> encode(const Picture& picture);

    // Reconstruction of the last encoded picture as the decoder outputs it (padded size).
    const std::vector<uint8_t>& reconY() const { return recY_; }
    const std::vector<uint8_t>& reconU() const { return recU_; }
    const std::vector<uint8_t>& reconV() const { return recV_; }
    int paddedWidth() const { return padW_; }
    int paddedHeight() const { return padH_; }
    int lastQp() const { return lastQp_; }

private:
    struct RefPic {
        std::vector<uint8_t> y, u, v;    // reconstruction (padded size)
        std::vector<uint8_t> F, B, V, J; // luma with margin: full, horizontal/vertical/centre half
    };
    void loadSource(const Picture& picture);
    void encodeMbPcm(BitWriter& bw, int mbx, int mby);
    struct ChromaMb;
    bool encodeMbIntra(BitWriter& bw, int mbx, int mby);
    void commitQp(int mbx, int mby, bool codedDelta);
    void planMacroblockQp();
    int qpPredOf(int mby) const { return qpPredByTop_[size_t(sliceTopOfRow_[size_t(mby)])]; }
    // interPred: motion-compensated prediction (inter macroblock); nullptr = intra.
    void analyseChroma(int mbx, int mby, ChromaMb& ch, const uint8_t (*interPred)[64] = nullptr);
    void writeChroma(BitWriter& bw, int mbx, int mby, const ChromaMb& ch);
    // Return whether the macroblock sent mb_qp_delta.
    bool encodeLuma16(BitWriter& bw, int mbx, int mby, const ChromaMb& ch);
    bool encodeLuma4(BitWriter& bw, int mbx, int mby, const ChromaMb& ch);
    void encodeMbP(BitWriter& bw, int mbx, int mby, int& skipRun);
    void rdoQuantize(int* levels, const int* residual, int qp, double lambda, int nC) const;
    struct MvNb {
        bool avail = false;
        int ref = -1;
        int mvx = 0, mvy = 0;
    };
    MvNb mvNeighbour(int mbx, int mby, int x4, int y4, const bool* decoded) const;
    void predictMvPart(int mbx, int mby, int x4, int y4, int w4, int shape, int refIdx, const bool* decoded, int& mvx,
                       int& mvy) const;
    void motionCompensate(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy,
                          uint8_t* luma, uint8_t (*chroma)[64]) const;
    int64_t mbSsd(int mbx, int mby) const;
    void buildRefPlanes(RefPic& R);
    void deblockPicture();
    int boundaryStrength(int pbx, int pby, int qbx, int qby, bool mbEdge) const;
    void mcLuma(const RefPic& R, int x0, int y0, int fx, int fy, int w, int h, uint8_t* out) const;
    int sadPart(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy, bool satd) const;
    struct MbState;
    void saveMb(int mbx, int mby, MbState& s) const;
    void restoreMb(int mbx, int mby, const MbState& s);
    int lumaNc(int bx, int by) const;
    // The macroblock row above belongs to the same slice.
    bool topMbAvailable(int mby) const { return mby > sliceTopOfRow_[size_t(mby)]; }
    int chromaNc(int c, int bx, int by) const;

    EncoderConfig cfg_;
    int lastQp_ = 0;
    int mbW_ = 0, mbH_ = 0, padW_ = 0, padH_ = 0;
    int frameIndex_ = 0;
    std::vector<uint8_t> srcY_, srcU_, srcV_;
    std::vector<uint8_t> recY_, recU_, recV_;
    std::vector<uint8_t> nzLuma_;      // TotalCoeff per 4x4 luma block
    std::vector<int8_t> modes4x4_;     // Intra 4x4 mode per 4x4 block, -1 = macroblock not Intra 4x4
    std::vector<uint8_t> nzChroma_[2]; // TotalCoeff per 4x4 chroma AC block
    std::vector<int16_t> mvX_, mvY_;        // per 4x4 block, quarter-pel units
    std::vector<int8_t> refIdx_;            // per 4x4 block: 0 inter, -1 intra
    std::vector<int16_t> prevMvX_, prevMvY_; // vectors of the previous picture (search start points)
    std::vector<int8_t> prevRefIdx_;
    // Reference luma with a margin: full-pel, horizontal half, vertical half, centre half (8.4.2.2.1).
    static constexpr int kMargin = 96;
    int extW_ = 0, extH_ = 0;
    std::vector<RefPic> refs_; // reference list, newest first
    int activeRefs_ = 0;
    std::vector<int> vInter_; // vertical 6-tap intermediate (unclipped)
    std::vector<int> sliceTopOfRow_;        // first macroblock row of the slice of each row
    std::vector<int> qpPredByTop_;          // QP predictor of each slice, indexed by its first row
    std::vector<int8_t> mbQp_, mbQpFinal_;  // planned QP per macroblock / QP it really has
    std::vector<float> mbEnergy_;
    int mbTypeOffset_ = 0;                  // 5 for intra macroblocks in P slices
    bool pSlice_ = false;
    int idrCount_ = 0;
    // Rate control state.
    double rcQp_ = 0;               // QP for P frames (I frames use 3 less)
    double rcBits_ = 0, rcTarget_ = 0; // produced / wanted bits so far
    double rcComplexity_[2] = {0, 0};  // [1]: bits * 2^(qp/6) of the last P frame
};

} // namespace schneidi::h264
