// schneidi's own H.264 encoder (prototype).
// Pure C++20, no external libraries: YUV 4:2:0 8-bit frames in, Annex B H.264 out.
// Current state: Baseline profile, CAVLC, intra-only (I_PCM, Intra 16x16, Intra 4x4).
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

private:
    void loadSource(const Picture& picture);
    void encodeMbPcm(BitWriter& bw, int mbx, int mby);
    struct ChromaMb;
    void encodeMbIntra(BitWriter& bw, int mbx, int mby);
    // interPred: motion-compensated prediction (inter macroblock); nullptr = intra.
    void analyseChroma(int mbx, int mby, ChromaMb& ch, const uint8_t (*interPred)[64] = nullptr);
    void writeChroma(BitWriter& bw, int mbx, int mby, const ChromaMb& ch);
    void encodeLuma16(BitWriter& bw, int mbx, int mby, const ChromaMb& ch);
    void encodeLuma4(BitWriter& bw, int mbx, int mby, const ChromaMb& ch);
    void encodeMbP(BitWriter& bw, int mbx, int mby, int& skipRun);
    void predictMv(int mbx, int mby, int& mvx, int& mvy) const;
    void motionCompensate(int mbx, int mby, int mvx, int mvy, uint8_t* luma, uint8_t (*chroma)[64]) const;
    int64_t mbSsd(int mbx, int mby) const;
    void buildRefPlanes();
    void deblockPicture();
    int boundaryStrength(int pbx, int pby, int qbx, int qby, bool mbEdge) const;
    void mcLuma(int x0, int y0, int fx, int fy, uint8_t* out) const;
    int sadLuma(int mbx, int mby, int mvx, int mvy) const;
    struct MbState;
    void saveMb(int mbx, int mby, MbState& s) const;
    void restoreMb(int mbx, int mby, const MbState& s);
    int lumaNc(int bx, int by) const;
    int chromaNc(int c, int bx, int by) const;

    EncoderConfig cfg_;
    int mbW_ = 0, mbH_ = 0, padW_ = 0, padH_ = 0;
    int frameIndex_ = 0;
    std::vector<uint8_t> srcY_, srcU_, srcV_;
    std::vector<uint8_t> recY_, recU_, recV_;
    std::vector<uint8_t> nzLuma_;      // TotalCoeff per 4x4 luma block
    std::vector<int8_t> modes4x4_;     // Intra 4x4 mode per 4x4 block, -1 = macroblock not Intra 4x4
    std::vector<uint8_t> nzChroma_[2]; // TotalCoeff per 4x4 chroma AC block
    std::vector<uint8_t> refY_, refU_, refV_; // previous reconstruction (reference for P)
    std::vector<int16_t> mvX_, mvY_;        // per macroblock, quarter-pel units
    std::vector<int8_t> refIdx_;            // per macroblock: 0 inter, -1 intra
    // Reference luma with a margin: full-pel, horizontal half, vertical half, centre half (8.4.2.2.1).
    static constexpr int kMargin = 96;
    int extW_ = 0, extH_ = 0;
    std::vector<uint8_t> extF_, extB_, extV_, extJ_;
    int mbTypeOffset_ = 0;                  // 5 for intra macroblocks in P slices
    bool pSlice_ = false;
    int idrCount_ = 0;
};

} // namespace schneidi::h264
