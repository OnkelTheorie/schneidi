// schneidi's own H.264 encoder (prototype).
// Pure C++20, no external libraries: YUV 4:2:0 8-bit frames in, Annex B H.264 out.
// Current state: Baseline profile, CAVLC, intra-only (I_PCM or Intra 16x16).
#pragma once

#include "BitWriter.h"

#include <cstdint>
#include <vector>

namespace schneidi::h264 {

enum class IntraMode {
    Pcm,     // raw samples, only to prove the bitstream structure
    Intra16, // Intra 16x16 prediction + 4x4 transform + CAVLC
};

struct EncoderConfig {
    int width = 0;  // even
    int height = 0; // even
    int qp = 26;    // 0..51 (values below ~10 may clip levels)
    IntraMode mode = IntraMode::Intra16;
    bool deblock = true; // decoder-side loop filter (does not affect intra prediction)
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
    // One access unit (one IDR picture) as Annex B.
    std::vector<uint8_t> encode(const Picture& picture);

    // Reconstruction of the last encoded picture before deblocking (padded size).
    const std::vector<uint8_t>& reconY() const { return recY_; }
    const std::vector<uint8_t>& reconU() const { return recU_; }
    const std::vector<uint8_t>& reconV() const { return recV_; }
    int paddedWidth() const { return padW_; }
    int paddedHeight() const { return padH_; }

private:
    void loadSource(const Picture& picture);
    void encodeMbPcm(BitWriter& bw, int mbx, int mby);
    void encodeMbIntra16(BitWriter& bw, int mbx, int mby);
    int lumaNc(int bx, int by) const;
    int chromaNc(int c, int bx, int by) const;

    EncoderConfig cfg_;
    int mbW_ = 0, mbH_ = 0, padW_ = 0, padH_ = 0;
    int frameIndex_ = 0;
    std::vector<uint8_t> srcY_, srcU_, srcV_;
    std::vector<uint8_t> recY_, recU_, recV_;
    std::vector<uint8_t> nzLuma_;      // TotalCoeff per 4x4 luma block
    std::vector<uint8_t> nzChroma_[2]; // TotalCoeff per 4x4 chroma AC block
};

} // namespace schneidi::h264
