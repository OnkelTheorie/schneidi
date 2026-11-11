// Per-picture state of schneidi's own H.264 encoder: the source, the reconstruction and every
// macroblock decision of one picture, plus the macroblock coding itself. The encoder keeps a few
// of these so pictures can be in flight at the same time.
#pragma once

#include "BitWriter.h"
#include "Cabac.h"
#include "H264Encoder.h"

#include <array>
#include <atomic>
#include <climits>
#include <thread>
#include <memory>
#include <vector>

namespace schneidi::h264 {

class CabacEncoder;
struct PhaseClock;

// A reference picture: the reconstruction and its luma with a margin: full-pel, horizontal half,
// vertical half, centre half (8.4.2.2.1).
struct RefPic {
    std::vector<uint8_t> y, u, v;
    std::vector<uint8_t> F, B, V, J;
    std::atomic<int> rowsReady{0}; // macroblock rows that are final (planes and margin included)
};

class FrameCoder {
public:
    explicit FrameCoder(const EncoderConfig& config);

    void loadSource(const Picture& picture);
    // Codes the picture (decisions, entropy coding, deblocking) into its slice NAL units.
    std::vector<uint8_t> code(bool idr, int frameNum, int idrPicId, PhaseClock& clock);

    // Pieces of code(), also used to keep two pictures in flight (CABAC only): reset, waveBegin,
    // then waveRow for every row claimRow hands out (any thread, in order), waveEnd, assemble.
    void reset(bool idr, int framesSinceIdr, int idrCount);
    void waveBegin(int threads, RefPic* out, bool writerThread);
    int claimRow()
    {
        const int r = wave_.nextRow.fetch_add(1, std::memory_order_acq_rel);
        return r < wave_.rows ? r : -1;
    }
    bool rowsLeft() const { return wave_.nextRow.load(std::memory_order_acquire) < wave_.rows; }
    void waveRow(int r);
    void waveEnd();
    std::vector<uint8_t> assemble(PhaseClock& clock);

    struct WaveState {
        int rows = 0, allocated = 0, spin = 128;
        bool inlineDeblock = false;
        std::vector<CabacEncoder> rowEngine, rowSnap;
        std::unique_ptr<std::atomic<int>[]> progress, deblocked; // macroblocks decided / filtered per row
        std::unique_ptr<bool[]> rowPrevNz;
        std::atomic<int> written{0};          // rows the entropy writer has finished
        std::atomic<int> nextRow{INT_MAX / 2}; // next row to hand out (none before waveBegin)
        std::atomic<int> rowsDone{0};
        std::atomic<long> waitUs{0}, refWaitUs{0}; // waiting for the row above / the reference (H264_TIMING)
        std::thread writer;
        RefPic* out = nullptr; // reference picture filled band by band, or nullptr
        int yCopied = 0, cCopied = 0, fBuilt = 0, hBuilt = 0;
    };
    WaveState wave_;
    // Picture results kept until the encoder hands the picture out.
    std::vector<uint8_t> done_;
    std::shared_ptr<RefPic> outPic_; // the reference picture being filled, kept alive while coding
    int frameNo_ = 0, outQp_ = 0;
    bool outSceneCut_ = false, rcIdr_ = false, rcLookahead_ = false;
    double rcCost_ = 0;
    bool idr_ = false;
    int framesSinceIdr_ = 0, idrCount_ = 0;
    std::vector<int> sliceTop_;
    std::vector<BitWriter> slices_;
    void sliceHeader(BitWriter& bw, int s) const;
    void encodeSlice(int s);
    void waveWrite();
    void publishBand(int k);

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
    void refFullRows(RefPic& R, int y0, int y1) const;
    void refHalfRows(RefPic& R, int y0, int y1) const;
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
    int mbW_ = 0, mbH_ = 0, padW_ = 0, padH_ = 0;
    std::vector<uint8_t> srcY_, srcU_, srcV_;
    std::vector<uint8_t> recY_, recU_, recV_;
    std::vector<uint8_t> nzLuma_;      // TotalCoeff per 4x4 luma block
    std::vector<int8_t> modes4x4_;     // Intra 4x4 mode per 4x4 block, -1 = macroblock not Intra 4x4
    std::vector<uint8_t> nzChroma_[2]; // TotalCoeff per 4x4 chroma AC block
    std::vector<int16_t> mvX_, mvY_;        // per 4x4 block, quarter-pel units
    std::vector<int8_t> refIdx_;            // per 4x4 block: 0 inter, -1 intra
    const FrameCoder* prev_ = nullptr; // the previous picture (its vectors are search start points)
    // Reference luma with a margin: full-pel, horizontal half, vertical half, centre half (8.4.2.2.1).
    static constexpr int kMargin = 96;
    int extW_ = 0, extH_ = 0;
    std::vector<std::shared_ptr<const RefPic>> refs_; // reference list, newest first
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
    bool detectSceneCut(std::vector<uint8_t>& prevThumb);
    std::vector<float> treeOffset_; // per macroblock, 0 without the tree
};

} // namespace schneidi::h264
