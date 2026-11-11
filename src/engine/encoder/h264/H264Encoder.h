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

class FrameCoder;
struct RefPic;

enum class IntraMode {
    Pcm,     // raw samples, only to prove the bitstream structure
    Intra16, // Intra 16x16 prediction + 4x4 transform + CAVLC
    Intra4,  // Intra 4x4 prediction (9 directions per 4x4 block)
    Auto,    // per macroblock the cheaper of Intra16 and Intra4 (rate-distortion)
};

// Result of the first pass (FirstPass): per picture the lookahead costs of the whole clip, so the
// rate control can pick one QP for the target bitrate instead of guessing from what it has seen.
struct FirstPassStats {
    std::vector<double> intraSum, interSum; // per picture, like LookaheadFrame
    std::vector<uint8_t> sceneChange, forceKeyframe;
    size_t size() const { return intraSum.size(); }
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
    // Bitrate mode with a first pass over the whole clip (FirstPass): one QP for the clip, only a
    // small correction while encoding. The pictures must come in the same order as analysed.
    std::shared_ptr<const FirstPassStats> firstPass;
    // idr_pic_id of the first IDR picture: neighbouring chunks of a clip encoded side by side
    // (planChunks) start far apart so two IDR pictures in a row never share one.
    int idrPicIdBase = 0;
    // Frame threading (CABAC, threads >= 2): the next picture starts while the last rows of the
    // previous one are still coded; one more picture of delay(). Same stream either way.
    bool frameThreads = true;
    // VUI video signal (E.2.1, ITU-T H.273 codes; 2 = unspecified): written when one is set or the
    // range is full. E.g. BT.709: 1, 1, 1.
    int colourPrimaries = 2;
    int transferCharacteristics = 2;
    int matrixCoefficients = 2;
    bool fullRange = false;
};

// level_idc (Table A-1) for this configuration: the lowest level whose macroblock rate, frame
// size, decoded picture buffer and (bitrate mode, 1.5x the target as peak) bitrate fit.
int levelIdcFor(const EncoderConfig& cfg);

// Parallel encoding in chunks: independent encoders, each on its own part of the clip starting
// with an IDR picture, run side by side; their streams are concatenated (SPS/PPS once, from any of
// them - they are the same). Seams go where the whole clip would have a keyframe anyway (caller's
// keyframes, scene cuts, the GOP schedule) if one lies near the even split. In the bitrate mode the
// clip's budget is shared by the predicted bits of each chunk at one QP (first pass), and every
// chunk runs the first-pass rate control on its own part.
struct ChunkPlan {
    std::vector<int> start;                                       // first picture of each chunk
    std::vector<int> bitrateKbps;                                 // per chunk (bitrate mode)
    std::vector<std::shared_ptr<const FirstPassStats>> firstPass; // per chunk (bitrate mode)
    // Configuration of chunk i: cfg with its bitrate, first pass, idr_pic_id base and threads.
    EncoderConfig configFor(const EncoderConfig& cfg, int i, int threads) const;
};
// minFrames: shortest chunk; 0 = 15 s of pictures (each seam costs about 8 %·s of BD-rate over the
// clip, measured: 15 s chunks stay near 0.5 %). Short clips get one chunk.
ChunkPlan planChunks(const FirstPassStats& stats, const EncoderConfig& cfg, int chunks, int minFrames = 0);

struct Picture {
    const uint8_t* y = nullptr;
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    int strideY = 0;
    int strideC = 0;
};

// Cheap first pass for the bitrate mode: the half-resolution lookahead analysis of every
// picture of the clip (a few ms per 1080p picture), without encoding anything.
class FirstPass {
public:
    // threads > 1: pictures are analysed in batches side by side (same numbers).
    FirstPass(int width, int height, int threads = 1);
    ~FirstPass();
    void add(const Picture& picture, bool forceKeyframe = false);
    std::shared_ptr<const FirstPassStats> stats() const;

private:
    void analyseBatch() const;
    int width_, height_, threads_;
    std::unique_ptr<Lookahead> la_;
    // Pictures waiting for the next batch, and the last analysed one (their predecessor).
    mutable std::vector<LookaheadFrame> batch_;
    mutable LookaheadFrame prev_;
    mutable bool havePrev_ = false;
    std::shared_ptr<FirstPassStats> stats_;
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
    const std::vector<uint8_t>& reconY() const;
    const std::vector<uint8_t>& reconU() const;
    const std::vector<uint8_t>& reconV() const;
    int paddedWidth() const { return padW_; }
    int paddedHeight() const { return padH_; }
    int lastQp() const { return lastQp_; }

private:
    struct Pool;
    FrameCoder* startFrame(LookaheadFrame& frame);
    std::vector<uint8_t> finishFrame(FrameCoder& fc);
    std::vector<uint8_t> step(LookaheadFrame* next);
    bool pipelined() const { return cfg_.frameThreads && useCabac() && cfg_.threads >= 2; }
    void applyRc(int upTo);
    std::unique_ptr<Pool> pool_;
    FrameCoder* inFlight_ = nullptr;           // started, not finished (frame threading)
    const FrameCoder* lastStarted_ = nullptr;  // the picture before the next one (search start points)
    // Rate control learns from a picture two pictures later (the one between may still be coded),
    // with or without frame threading, so the stream does not depend on the thread count.
    struct RcRecord {
        int index;
        bool idr, lookahead;
        double cost;
        int qp;
        double bits;
    };
    std::deque<RcRecord> rcDone_;
    void markKeyframes();
    bool useLookahead() const { return cfg_.lookahead > 0 && cfg_.gop > 1 && (cfg_.bitrateKbps > 0 || cfg_.mbtree); }
    bool useCabac() const { return cfg_.cabac && cfg_.mode != IntraMode::Pcm; }
    bool useT8() const { return useCabac() && cfg_.transform8x8; }

    EncoderConfig cfg_;
    int mbW_ = 0, mbH_ = 0, padW_ = 0, padH_ = 0;
    // Pictures in flight (round robin) and the one that came out last.
    std::unique_ptr<FrameCoder> coders_[3];
    int nextCoder_ = 0;
    const FrameCoder* last_ = nullptr;
    std::deque<std::shared_ptr<RefPic>> refList_; // reference pictures, newest first
    int lastQp_ = 0;
    int frameIndex_ = 0;
    int idrCount_ = 0;
    int framesSinceIdr_ = 0;
    bool lastWasSceneCut_ = false;
    std::vector<uint8_t> prevThumb_; // quarter-size luma of the previous picture
    // Lookahead queue (front = next picture to encode) and the macroblock tree's QP offsets.
    std::unique_ptr<Lookahead> la_;
    std::deque<LookaheadFrame> queue_;
    std::unique_ptr<LookaheadFrame> pendingFrame_; // being analysed on pendingThread_ while a picture is encoded
    std::thread pendingThread_;
    void joinPending();
    // Rate control state.
    double rcQp_ = 0;               // QP for P frames (I frames use 3 less)
    double rcBits_ = 0, rcTarget_ = 0; // produced / wanted bits so far
    double rcCplxSum_ = 0, rcCplxCount_ = 0; // complexity (bits * 2^(qp/6)) of the P frames so far
    // Lookahead model: bits = k * cost * 2^(-qp/6), k per picture type from the pictures coded so far.
    double rcKSum_[2] = {0, 0}, rcCostSum_[2] = {0, 0}; // [0] I, [1] P
    double rcNeedSeen_ = 0; // predicted bits at QP 0 of the pictures coded so far
    int rcFramesSeen_ = 0;
    // First-pass rate control: predicted keyframes and the costs of the pictures not coded yet.
    std::vector<uint8_t> tpKey_;
    double tpRemI_ = 0, tpRemP_ = 0;
    double twoPassQp(double kI, double kP) const;
    double frameCost(const LookaheadFrame& f, bool intra) const;

};

} // namespace schneidi::h264
