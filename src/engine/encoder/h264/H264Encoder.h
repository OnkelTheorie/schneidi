// schneidi's own H.264 encoder (prototype).
// Pure C++20, no external libraries: YUV 4:2:0 8-bit frames in, Annex B H.264 out.
// Baseline profile, CAVLC: I (I_PCM, Intra 16x16, Intra 4x4) and P frames (16x16..8x8, quarter-pel).
#pragma once

#include "BitWriter.h"
#include "Lookahead.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

namespace schneidi::h264 {

class CabacEncoder;

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
    int threads = 1;     // CABAC: macroblock rows in a wavefront (one slice); CAVLC: one slice per thread
    int effort = 1;      // 0 = fast (16x16 motion only), 1 = default (partitions, more RD)
    int bitrateKbps = 0; // > 0: frame-level rate control towards this bitrate, qp is the start value
    double fps = 60.0;   // for the rate control and the VUI timing
    double aqStrength = 1.0; // adaptive quantisation (finer QP on flat areas); 0 = off
    int refs = 1;        // reference pictures for P frames (1..16)
    bool sceneCut = true; // keyframe on detected scene changes
    bool cabac = true;    // CABAC entropy coding (Main profile); false = CAVLC (Baseline)
    bool transform8x8 = true; // 8x8 transform for inter macroblocks (High profile; needs CABAC)
    int lookahead = 40;       // pictures analysed ahead for the rate control and the macroblock tree (0 = none)
    bool mbtree = true;       // macroblock tree: finer QP where later pictures reference a block (needs lookahead)
    double mbtreeStrength = 2.0;
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
    ~Encoder();

    // SPS + PPS as Annex B.
    std::vector<uint8_t> headers() const;
    // Queues a picture; returns the next access unit (IDR or P picture, Annex B) or nothing while
    // the lookahead is still filling. forceKeyframe: start a new GOP at this picture (e.g. a cut in
    // the timeline). Pictures come out in order, `delay()` pictures after they went in.
    std::vector<uint8_t> encode(const Picture& picture, bool forceKeyframe = false);
    // After the last picture: the remaining access units, one per call, empty when done.
    std::vector<uint8_t> flush();
    int delay() const;
    bool lastWasSceneCut() const { return lastWasSceneCut_; }

    // Reconstruction of the last picture that came out, as the decoder outputs it (padded size).
    const std::vector<uint8_t>& reconY() const { return recY_; }
    const std::vector<uint8_t>& reconU() const { return recU_; }
    const std::vector<uint8_t>& reconV() const { return recV_; }
    int paddedWidth() const { return padW_; }
    int paddedHeight() const { return padH_; }
    int lastQp() const { return lastQp_; }

private:
    // Syntax elements of one coded macroblock, kept so the entropy coder can run after the decision.
    struct MbSyntax {
        enum class Kind : uint8_t { Skip, I16, I4, Inter } kind = Kind::Skip;
        int i16Mode = 0, chromaMode = 0;
        int i4Mode[16] = {}, i4Pred[16] = {};
        int interType = 0; // 0 16x16, 1 16x8, 2 8x16, 3 8x8
        int ref[4] = {}, mvd[4][2] = {};
        int cbpLuma = 0, cbpChroma = 0;
        bool hasQpDelta = false;
        int qpDelta = 0;
        int lumaDc[16] = {};         // Intra 16x16 DC, scan order
        int luma[16][16] = {};       // per luma4x4BlkIdx, scan order (Intra 16x16: 15 AC levels)
        bool t8 = false;             // 8x8 transform (CABAC only); with Kind::I4 = Intra 8x8 (modes in i4Mode[0..3])
        int luma8[4][64] = {};       // per 8x8 block, scan order
        int chromaDc[2][4] = {};
        int chromaAc[2][4][15] = {};
    };
    struct MbOut {
        BitWriter bw; // CAVLC (also the bit estimate for decisions)
        MbSyntax syn;
    };
    struct RefPic {
        std::vector<uint8_t> y, u, v;    // reconstruction (padded size)
        std::vector<uint8_t> F, B, V, J; // luma with margin: full, horizontal/vertical/centre half
    };
    void loadSource(const Picture& picture);
    std::vector<uint8_t> encodeFrame(LookaheadFrame& frame);
    void markKeyframes();
    bool useLookahead() const { return cfg_.lookahead > 0 && cfg_.gop > 1 && (cfg_.bitrateKbps > 0 || cfg_.mbtree); }
    void encodeMbPcm(BitWriter& bw, int mbx, int mby);
    struct ChromaMb;
    bool encodeMbIntra(MbOut& out, int mbx, int mby);
    void commitQp(int mbx, int mby, bool codedDelta);
    void planMacroblockQp();
    int qpPredOf(int mby) const { return qpPredByTop_[size_t(rowKey_[size_t(mby)])]; }
    // interPred: motion-compensated prediction (inter macroblock); nullptr = intra.
    void analyseChroma(int mbx, int mby, ChromaMb& ch, const uint8_t (*interPred)[64] = nullptr);
    void writeChroma(MbOut& o, int mbx, int mby, const ChromaMb& ch);
    // Return whether the macroblock sent mb_qp_delta.
    bool encodeLuma16(MbOut& o, int mbx, int mby, const ChromaMb& ch);
    bool encodeLuma4(MbOut& o, int mbx, int mby, const ChromaMb& ch);
    bool encodeLuma8(MbOut& o, int mbx, int mby, const ChromaMb& ch);
    // Returns false for a skipped macroblock.
    bool encodeMbP(MbOut& result, int mbx, int mby);
    void rdoQuantize(int* levels, const int* residual, int qp, double lambda, int nC) const;
    bool skipResidualIsZero(int mbx, int mby, const uint8_t* lp, const uint8_t (*cp)[64], int qp) const;
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
    void deblockMb(int mbx, int mby);
    // CABAC neighbour state (what the context selection of later macroblocks reads). The
    // wavefront's estimates and the final raster-order write keep separate copies, so the
    // write can follow the decisions row by row on its own thread.
    struct CabacState {
        std::vector<uint8_t> kind, cbp, chromaMode, cbfDc, t8; // per macroblock
        std::vector<uint8_t> cbfLuma;                           // per 4x4 luma block
        std::vector<std::array<uint8_t, 2>> mvd;                // |mvd| per 4x4 block, x and y
        std::vector<uint8_t> cbfChroma[2];                      // per 4x4 chroma block
        void resize(int mbW, int mbH);
    };
    void cabacWriteMb(CabacEncoder& ce, CabacState& st, int mbx, int mby, const MbSyntax& s, bool& prevQpNonZero);
    bool useCabac() const { return cfg_.cabac && cfg_.mode != IntraMode::Pcm; }
    bool useT8() const { return useCabac() && cfg_.transform8x8; }
    int residual(BitWriter& bw, const int* coeffs, int count, int nC) const;
    double mbBits(const MbOut& o, int mbx, int mby, double cavlcBits);
    double skipBits(int mbx, int mby);
    std::vector<CabacEncoder*> cabacByTop_; // live CABAC engine of each slice (by first row), for bit estimates
    std::vector<bool*> prevQpNzByTop_;
    CabacState cabWave_, cabWrite_;
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
    std::vector<int> sliceTopOfRow_;        // first macroblock row of the slice of each row
    std::vector<int> rowKey_;               // per row: index of its live CABAC / QP predictor state (slice top, or the row itself in the wavefront)
    std::vector<MbSyntax> mbSyn_;           // wavefront: chosen syntax of every macroblock, written in raster order afterwards
    std::vector<int> qpPredByTop_;          // QP predictor of each slice, indexed by its first row
    std::vector<int8_t> mbQp_, mbQpFinal_;  // planned QP per macroblock / QP it really has
    std::vector<float> mbEnergy_;
    int mbTypeOffset_ = 0;                  // 5 for intra macroblocks in P slices
    bool pSlice_ = false;
    bool deblockedInline_ = false; // the wavefront filtered the picture already
    bool intraSkip8_ = false, intraSkip16_ = false; // intra try in a P picture without Intra 8x8 / 16x16 (experiments)
    int idrCount_ = 0;
    int framesSinceIdr_ = 0;
    bool lastWasSceneCut_ = false;
    std::vector<uint8_t> prevThumb_; // quarter-size luma of the previous picture
    bool detectSceneCut();
    // Lookahead queue (front = next picture to encode) and the macroblock tree's QP offsets.
    std::unique_ptr<Lookahead> la_;
    std::deque<LookaheadFrame> queue_;
    std::unique_ptr<LookaheadFrame> pendingFrame_; // being analysed on pendingThread_ while a picture is encoded
    std::thread pendingThread_;
    void joinPending();
    std::vector<float> treeOffset_; // per macroblock, 0 without the tree
    // Rate control state.
    double rcQp_ = 0;               // QP for P frames (I frames use 3 less)
    double rcBits_ = 0, rcTarget_ = 0; // produced / wanted bits so far
    double rcCplxSum_ = 0, rcCplxCount_ = 0; // complexity (bits * 2^(qp/6)) of the P frames so far
    // Lookahead model: bits = k * cost * 2^(-qp/6), k per picture type from the pictures coded so far.
    double rcKSum_[2] = {0, 0}, rcCostSum_[2] = {0, 0}; // [0] I, [1] P
    double rcNeedSeen_ = 0; // predicted bits at QP 0 of the pictures coded so far
    int rcFramesSeen_ = 0;
    double frameCost(const LookaheadFrame& f, bool intra) const;
};

} // namespace schneidi::h264
