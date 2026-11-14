// Per-picture state of schneidi's own H.264 encoder: the source, the reconstruction and every
// macroblock decision of one picture, plus the macroblock coding itself. The encoder keeps a few
// of these so pictures can be in flight at the same time.
#pragma once

#include "BitWriter.h"
#include "Cabac.h"
#include "H264Encoder.h"
#include "ThreadErrors.h"

#include <cstring>
#include <array>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <mutex>
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
    // With B pictures: the picture's list 0 motion per 4x4 block (ref -1 = intra), final for the
    // rows in rowsReady. Direct prediction reads the co-located block, the search its start points.
    std::vector<int16_t> mvx, mvy;
    std::vector<int8_t> ref;
    int gopPos = 0; // display distance from the IDR picture
};

// Partitions of an inter macroblock in a B slice by its mb_type (table 7-14; 0 = B_Direct_16x16
// has none, 22 = B_8x8 takes them from the sub types): shape 0 16x16, 1 16x8, 2 8x16, 3 8x8, and per
// partition which lists predict it (1 list 0, 2 list 1, 3 both; 0 = direct).
struct BParts {
    int shape = 0, count = 0;
    int use[4] = {};
};
inline BParts bPartsOf(int bType, const uint8_t* sub)
{
    BParts b;
    if (bType == 0)
        return b;
    if (bType <= 3) {
        b.count = 1;
        b.use[0] = bType;
    } else if (bType < 22) {
        static constexpr uint8_t kPairs[9][2] = {{1, 1}, {2, 2}, {1, 2}, {2, 1}, {1, 3}, {2, 3}, {3, 1}, {3, 2}, {3, 3}};
        b.shape = 1 + (bType - 4) % 2;
        b.count = 2;
        b.use[0] = kPairs[(bType - 4) / 2][0];
        b.use[1] = kPairs[(bType - 4) / 2][1];
    } else {
        b.shape = 3;
        b.count = 4;
        for (int i = 0; i < 4; ++i)
            b.use[i] = sub[i];
    }
    return b;
}

// Wakes the worker pool of the frame threading: every change that may let a parked row go on bumps
// the epoch; a worker that found nothing to do sleeps only while the epoch stays what it was
// before it looked.
struct WaveSignal {
    std::atomic<uint32_t> epoch{0};
    std::atomic<int> sleepers{0};
    std::mutex m;
    std::condition_variable cv;
    void wake(bool all = false)
    {
        epoch.fetch_add(1, std::memory_order_seq_cst);
        if (sleepers.load(std::memory_order_seq_cst) > 0) {
            {
                std::lock_guard<std::mutex> l(m); // the sleeper is past its check and waiting
            }
            if (all)
                cv.notify_all();
            else
                cv.notify_one();
        }
    }
};

class FrameCoder {
public:
    explicit FrameCoder(const EncoderConfig& config);

    void loadSource(const Picture& picture);
    // Codes the picture (decisions, entropy coding, deblocking) into its slice NAL units.
    std::vector<uint8_t> code(bool idr, int frameNum, int idrPicId, PhaseClock& clock);

    // Pieces of code(), also used to keep two pictures in flight (CABAC only): reset, waveBegin,
    // then waveRow for every row claimRow hands out (any thread, in order), waveEnd, assemble.
    void reset(bool idr, int frameNum, int idrCount);
    void waveBegin(int threads, RefPic* out, bool writerThread);
    int claimRow()
    {
        const int r = wave_.nextRow.fetch_add(1, std::memory_order_acq_rel);
        return r < wave_.rows ? r : -1;
    }
    void waveRow(int r); // never throws: a failure stops the picture (rethrowWaveError)
    // The worker pool's way (frame threading): rows are not waited in but parked. claimRunnableRow
    // hands out a row that can go on by a batch of macroblocks (-1 = none right now), waveRowRun
    // takes it as far as it goes without waiting and gives it back.
    int claimRunnableRow();
    void waveRowRun(int r); // never throws
    void waveEnd();
    void rethrowWaveError(); // after waveEnd: the first exception of a row or the writer
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
        // Where each row stands (a row is advanced by one thread at a time: rowBusy).
        std::unique_ptr<std::atomic<int>[]> rowMb, rowDb; // next macroblock to decide / to filter in the row above
        std::unique_ptr<std::atomic<uint8_t>[]> rowPhase, rowBusy;
        std::atomic<int> firstOpen{0}; // no row above this one is unfinished
        int batch = 0;                 // a parked row goes on once it can do this many macroblocks more than one
        WaveSignal* signal = nullptr;  // the pool's, while the picture is in it
        // Pool workers inside waveRowRun of this picture (claimed under the pool's lock). The pool
        // lets go of a picture only once this is zero: a worker past its row's last macroblock
        // still reads this state (rowBusy, wakeRow, signal).
        std::atomic<int> running{0};
        RefPic* out = nullptr; // reference picture filled band by band, or nullptr
        int yCopied = 0, cCopied = 0, fBuilt = 0, hBuilt = 0;
    };
    WaveState wave_;
    // Picture results kept until the encoder hands the picture out.
    std::vector<uint8_t> done_;
    std::shared_ptr<RefPic> outPic_; // the reference picture being filled, kept alive while coding
    int frameNo_ = 0, outQp_ = 0, displayIndex_ = 0;
    int rcType_ = 0; // 0 I, 1 P, 2 B
    bool outSceneCut_ = false, rcLookahead_ = false;
    double rcCost_ = 0;
    bool idr_ = false;
    int framesSinceIdr_ = 0, idrCount_ = 0;
    std::vector<int> sliceTop_;
    std::vector<BitWriter> slices_;
    void sliceHeader(BitWriter& bw, int s) const;
    void encodeSlice(int s);
    void waveWrite();
    void waveWriteGuarded();
    int waveRowBody(int r, bool block); // 1 done, 0 parked (only without block), -1 the picture failed
    bool rowRunnable(int r) const;
    bool refRowsReady(int r) const;
    void wakeRow(int r);
    void abandonRow(int r);
    void waveFail();
    ThreadErrors waveErrors_;
    void publishBand(int k);

    // Syntax elements of one coded macroblock, kept so the entropy coder can run after the decision.
    struct MbSyntax {
        enum class Kind : uint8_t { Skip, I16, I4, Inter } kind = Kind::Skip;
        int i16Mode = 0, chromaMode = 0;
        int i4Mode[16] = {}, i4Pred[16] = {};
        int interType = 0; // 0 16x16, 1 16x8, 2 8x16, 3 8x8
        int ref[4] = {}, mvd[4][2] = {};
        // B slices (Kind::Inter): mb_type of table 7-14 (0 = B_Direct_16x16 .. 22 = B_8x8), the
        // list 1 vector differences and for B_8x8 the sub types (0 direct, 1 L0, 2 L1, 3 Bi).
        uint8_t bType = 0, bSub[4] = {};
        int mvd1[4][2] = {};
        int cbpLuma = 0, cbpChroma = 0;
        bool hasQpDelta = false;
        int qpDelta = 0;
        int lumaDc[16] = {};         // Intra 16x16 DC, scan order
        int luma[16][16] = {};       // per luma4x4BlkIdx, scan order (Intra 16x16: 15 AC levels)
        bool t8 = false;             // 8x8 transform (CABAC only); with Kind::I4 = Intra 8x8 (modes in i4Mode[0..3])
        int luma8[4][64] = {};       // per 8x8 block, scan order
        int chromaDc[2][4] = {};
        int chromaAc[2][4][15] = {};

        // Copies what the entropy coder reads of s: the header fields and only the level arrays
        // its type and coded_block_pattern select (the others keep whatever they held).
        void assignUsed(const MbSyntax& s)
        {
            kind = s.kind;
            if (kind == Kind::Skip)
                return; // a skipped macroblock is its kind alone
            i16Mode = s.i16Mode;
            chromaMode = s.chromaMode;
            std::memcpy(i4Mode, s.i4Mode, sizeof i4Mode);
            std::memcpy(i4Pred, s.i4Pred, sizeof i4Pred);
            interType = s.interType;
            std::memcpy(ref, s.ref, sizeof ref);
            std::memcpy(mvd, s.mvd, sizeof mvd);
            bType = s.bType;
            std::memcpy(bSub, s.bSub, sizeof bSub);
            std::memcpy(mvd1, s.mvd1, sizeof mvd1);
            cbpLuma = s.cbpLuma;
            cbpChroma = s.cbpChroma;
            hasQpDelta = s.hasQpDelta;
            qpDelta = s.qpDelta;
            t8 = s.t8;
            if (kind == Kind::I16)
                std::memcpy(lumaDc, s.lumaDc, sizeof lumaDc);
            for (int b8 = 0; b8 < 4; ++b8) {
                if (!((cbpLuma >> b8) & 1))
                    continue;
                if (t8)
                    std::memcpy(luma8[b8], s.luma8[b8], sizeof luma8[b8]);
                else
                    std::memcpy(luma[b8 * 4], s.luma[b8 * 4], 4 * sizeof luma[0]);
            }
            if (cbpChroma)
                std::memcpy(chromaDc, s.chromaDc, sizeof chromaDc);
            if (cbpChroma == 2)
                std::memcpy(chromaAc, s.chromaAc, sizeof chromaAc);
        }
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
    // satdOut: the sum of the chosen modes' SATD + lambda * mode bits. Both give up (satdOut -1, the
    // macroblock state half written) once that sum exceeds bound.
    bool encodeLuma4(MbOut& o, int mbx, int mby, const ChromaMb& ch, int* satdOut = nullptr, int bound = INT_MAX);
    bool encodeLuma8(MbOut& o, int mbx, int mby, const ChromaMb& ch, int bound = INT_MAX, int* satdOut = nullptr);
    // Returns false for a skipped macroblock.
    bool encodeMbP(MbOut& result, int mbx, int mby);
    bool encodeMbB(MbOut& result, int mbx, int mby);
    // Spatial direct prediction (8.4.1.2.2) of a macroblock in a B slice: per list the reference
    // (-1 = unused) and a vector per 8x8 quadrant.
    struct DirectPred {
        int ref[2];
        int mv[2][4][2];
    };
    void directPredict(int mbx, int mby, DirectPred& d) const;
    // Prediction of one block of a B macroblock from list 0, list 1 or their average.
    void predictBi(int mbx, int mby, int x4, int y4, int w4, int h4, int use, const int* mv0, const int* mv1, uint8_t* luma,
                   uint8_t (*chroma)[64]) const;
    // Motion search of one partition in reference R around the predictor (pX, pY): full-pel
    // diamond on SAD from the best start point, then half/quarter-pel on SATD. Returns the cost
    // (SATD + lambdaSad * vector bits).
    double motionSearch(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int pX, int pY, double lambdaSad,
                        const std::pair<int, int>* starts, int startCount, int subpel, int& outX, int& outY,
                        double predCost = -1, double* fullPelCost = nullptr) const;
    double refineQuarter(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int pX, int pY, double lambdaSad,
                         int& vx, int& vy) const;
    // The residual of an inter macroblock against its prediction: luma levels and reconstruction
    // (codeInterLuma), then after the caller wrote the macroblock header the coded block pattern,
    // mb_qp_delta and coefficients (writeInterResidual; returns whether mb_qp_delta was sent).
    struct InterResidual {
        int levels[16][16];
        int levels8[4][64];
        int cbpLuma;
        double energy8[4]; // 8x8 plan with estimate: squared error of each quadrant left empty
    };
    void codeInterLuma(int mbx, int mby, const uint8_t* lp, int qp, double lambda, bool t8, bool trellis, MbOut& io,
                       InterResidual& ir, double fineLambda = -1.0);
    // emptyOf8 (4x4 plan): the 8x8 plan of the same prediction; its empty quadrants stay empty.
    void planInterLuma(int mbx, int mby, const uint8_t* lp, int qp, double lambda, bool t8, bool trellis, InterResidual& ir,
                       double* est, double fineLambda = -1.0, const InterResidual* emptyOf8 = nullptr);
    void reconInterLuma(int mbx, int mby, const uint8_t* lp, int qp, bool t8, MbOut& io, const InterResidual& ir);
    bool writeInterResidual(int mbx, int mby, int qp, bool t8, const InterResidual& res, const ChromaMb& ch, MbOut& io);
    void rdoQuantize(int* levels, const int* residual, int qp, double lambda, int nC) const;
    bool skipResidualIsZero(int mbx, int mby, const uint8_t* lp, const uint8_t (*cp)[64], int qp) const;
    struct MvNb {
        bool avail = false;
        int ref = -1;
        int mvx = 0, mvy = 0;
    };
    MvNb mvNeighbour(int mbx, int mby, int x4, int y4, const bool* decoded, int list = 0) const;
    void predictMvPart(int mbx, int mby, int x4, int y4, int w4, int shape, int refIdx, const bool* decoded, int& mvx,
                       int& mvy, int list = 0) const;
    void motionCompensate(const RefPic& R, int mbx, int mby, int x4, int y4, int w4, int h4, int mvx, int mvy,
                          uint8_t* luma, uint8_t (*chroma)[64]) const;
    int64_t mbSsd(int mbx, int mby) const;
    // Psycho-visual RD (cfg_.psyRd). psyBegin, before a macroblock is decided: the Hadamard
    // coefficient magnitudes of its source luma, kept for the thread. psyCost: the weighted distance
    // between those and the magnitudes of the luma reconstruction as it stands in recY_, to add to
    // SSD + lambda * bits (0.0 when off).
    void psyBegin(int mbx, int mby) const;
    double psyCost(int mbx, int mby) const;
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
        std::vector<std::array<uint8_t, 2>> mvd1;               // the same for list 1 (B slices)
        std::vector<uint8_t> direct;                            // per macroblock: B_Direct_16x16
        std::vector<uint8_t> cbfChroma[2];                      // per 4x4 chroma block
        void resize(int mbW, int mbH);
    };
    // Bits of an inter macroblock's luma coded_block_pattern bins and transform_size_8x8_flag in the
    // contexts' current states (for estimates).
    double cabacLumaSideBits(const CabacEncoder& ce, int mbx, int mby, int cbpLuma, bool t8) const;
    // E = CabacEncoder (writing, or its counting mode) or CabacCounter (bit estimate).
    template <class E>
    void cabacWriteMb(E& ce, CabacState& st, int mbx, int mby, const MbSyntax& s, bool& prevQpNonZero);
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
    std::vector<float> fullPelCost_;        // per macroblock: full-pel cost of its 16x16 search, -1 = none (P pictures)
    // List 1 of a B picture (there refIdx_ is list 0: -1 = intra or list 1 only).
    std::vector<int16_t> mvX1_, mvY1_;
    std::vector<int8_t> refIdx1_;
    const FrameCoder* prev_ = nullptr; // the previous picture (its vectors are search start points)
    // Reference luma with a margin: full-pel, horizontal half, vertical half, centre half (8.4.2.2.1).
    static constexpr int kMargin = 96;
    int extW_ = 0, extH_ = 0;
    std::vector<std::shared_ptr<const RefPic>> refs_; // reference list, newest first
    std::vector<std::shared_ptr<const RefPic>> refs1_; // B picture: list 1 (the next reference picture in display order)
    int activeRefs_ = 0;
    bool bSlice_ = false;  // B picture (pSlice_ is set too: an inter slice)
    bool refPic_ = true;   // the picture is a reference (nal_ref_idc)
    bool pocLsb_ = false;  // pic_order_cnt_type 0: the slice header carries pic_order_cnt_lsb
    int gopPos_ = 0;       // display distance from the IDR picture
    int biW0_ = 32, biW1_ = 32; // B picture: weights of the list 0 / list 1 prediction when both are used (of 64)
    void setBiWeights();
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
