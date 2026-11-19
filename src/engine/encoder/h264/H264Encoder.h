// schneidi's own H.264 encoder (prototype).
// Pure C++20, no external libraries: YUV 4:2:0 8-bit frames in, Annex B H.264 out.
// Baseline profile, CAVLC: I (I_PCM, Intra 16x16, Intra 4x4) and P frames (16x16..8x8, quarter-pel).
#pragma once

#include "BitWriter.h"
#include "Lookahead.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
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
    // The lookahead analysis of the first pictures (per macroblock, as LookaheadFrame has it): the
    // encoder takes it instead of analysing the same pictures again. Kept up to a memory limit
    // (FirstPass), so it may cover fewer pictures than the rest, or none.
    struct Analysis {
        std::vector<int32_t> intraCost, interCost;
        std::vector<int16_t> mvx, mvy;
        double intraSum = 0, interSum = 0;
        bool sceneChange = false;
    };
    std::vector<std::shared_ptr<const Analysis>> analysis;
    // B picture placement (FirstPass with bframes > 0 = bMax): per picture the longest run of B
    // pictures that pays there (BProbe::gate: the run, 0 = none, with BProbe::kClear), so the whole
    // clip's picture types can be planned before encoding. Empty = not measured, B pictures everywhere.
    int bMax = 0;
    std::vector<uint8_t> bGate;
    // Per picture two histograms of its macroblocks' lookahead costs - [0] intra, [1] the better of
    // intra and motion, as intraSum / interSum add them up - in quarter octaves from 8 on (count
    // and sum per class): [picture * 2 * kHistBins + which * kHistBins + class]. The rate control
    // predicts a picture's bits at a QP from them: a block whose cost stays below the quantiser's
    // reach costs next to nothing, however many such blocks a picture has.
    static constexpr int kHistBins = 56;
    std::vector<uint32_t> histCount;
    std::vector<float> histSum;
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
    // Speed stage: 0 = default (the stream as without this option). 1 = fast: in B pictures the split layouts
    // (16x8, 8x16, 8x8) are not searched where the 16x16 prediction's residual is coded in all four quadrants
    // (dense texture); a small moving part against a calm background keeps them. Camera footage: 14 % fewer
    // instructions for about 1 % BD-rate; other material gains and loses less.
    int speed = 0;
    int bitrateKbps = 0; // > 0: frame-level rate control towards this bitrate, qp is the start value
    double fps = 60.0;   // for the rate control and the VUI timing
    // Constant quality (>= 0; < 0 = off; ignored with bitrateKbps > 0): one pass, no size target - the file is as
    // large as the content needs. The scale is x264's: a value gives about the file size and PSNR of x264 --crf
    // with the same value (18 = very good, 23 = good, 28 = small; +6 halves the size roughly). Each picture's QP
    // comes from the value, the frame rate, qcomp times the complexity around the picture against the clip's
    // mean (lookahead costs, so lookahead > 0 and gop > 1 for that; without them one QP) and the macroblock
    // tree's level; the adaptive quantisation centres on a fixed texture energy instead of the picture's mean.
    // qp is not used.
    double crf = -1.0;
    // Constant quality: the clip's mean complexity (crfClipComplexity), set by ChunkPlan::configFor so every
    // chunk measures against the whole clip; < 0 = the mean of the pictures seen so far.
    double crfClipComplexity = -1.0;
    // Adaptive quantisation (finer QP on flat areas); 0 = off. 1.0 until 14.11.2026. Macroblocks below a
    // texture energy of 3 (log2 of 256 x variance + 1) count as equally flat (since 16.11.2026).
    double aqStrength = 1.5;
    int refs = 1;        // reference pictures for P frames (1..16)
    bool sceneCut = true; // keyframe on detected scene changes
    bool cabac = true;    // CABAC entropy coding (Main profile); false = CAVLC (Baseline)
    bool transform8x8 = true; // 8x8 transform for inter macroblocks (High profile; needs CABAC)
    int lookahead = 40;       // pictures analysed ahead for the rate control and the macroblock tree (0 = none)
    bool mbtree = true;       // macroblock tree: finer QP where later pictures reference a block (needs lookahead)
    double mbtreeStrength = 2.0;
    // For reference pictures with runs of more than four B pictures before or after them (a slow
    // zoom; next to shorter runs the full strength, see Encoder::treeStrength).
    double mbtreeStrengthB = 1.0;
    // Bitrate mode with a first pass over the whole clip (FirstPass): one QP for the clip, only a
    // small correction while encoding. The pictures must come in the same order as analysed.
    std::shared_ptr<const FirstPassStats> firstPass;
    // idr_pic_id of the first IDR picture: neighbouring chunks of a clip encoded side by side
    // (planChunks) start far apart so two IDR pictures in a row never share one.
    int idrPicIdBase = 0;
    // Frame threading (CABAC, threads >= 2): the next picture starts while the last rows of the
    // previous ones are still coded (two in flight while the rate control settles, then three); two
    // more pictures of delay(). Same stream either way.
    bool frameThreads = true;
    // VUI video signal (E.2.1, ITU-T H.273 codes; 2 = unspecified): written when one is set or the
    // range is full. E.g. BT.709: 1, 1, 1.
    int colourPrimaries = 2;
    int transferCharacteristics = 2;
    int matrixCoefficients = 2;
    bool fullRange = false;
    // B pictures (CABAC only, Main/High): up to this many in a row between two reference pictures,
    // not used as references themselves. 0 = none (the stream is then the same as without this
    // option). Access units come out in coding order: lastDisplayIndex() says which picture one is.
    // Default 8 since 14.11.2026 (0 before): with the probe, B pictures appear only on material
    // where they save 15 % or more (calm camera footage), there in runs of 2 or 8. A CALLER THAT
    // MUXES MUST TAKE pts FROM lastDisplayIndex() AND dts FROM THE CODING ORDER (reorderDelay()),
    // or set bframes = 0.
    int bframes = 8;
    int bQpOffset = 2;     // QP of a B picture above the P QP
    // The same in runs of up to four B pictures (never more than bQpOffset). Their P pictures have
    // the tree's full strength; with +2 on top the B pictures fall 1.1 dB below them in a pan (a
    // flicker at a quarter of the frame rate), with 0 it is 0.8 dB at the same BD-rate.
    int bQpOffsetShort = 0;
    // Places B pictures only where they pay (BProbe: a few pictures coded with and without them;
    // with a first pass over the whole clip, which must then have been made with at least this
    // many bframes). false = runs of bframes B pictures everywhere.
    bool bAdaptive = true;
    // B pyramid: in a run of three or four B pictures the middle one is a reference picture for
    // the B pictures on both sides of it (one more picture in the decoder's buffer and of
    // reordering: reorderDelay() 2). The B pictures then predict from near pictures on both
    // sides, which is what makes them pay on moving material. Shorter and longer runs are unchanged.
    bool bPyramid = true;
    // Predictions from both lists weighted by the distance to the two reference pictures
    // (implicit weighted prediction) instead of their plain average. Measured on a slow zoom:
    // no gain (+0.7 % BD-rate), so off by default.
    bool weightedBipred = false;
    // B pictures keep the texture of their reference pictures. A prediction from both lists is the
    // average of two pictures; on a slowly drifting textured wall they show the texture at
    // different sub-sample positions and the average is softer than either. Skipped, it costs
    // nothing, so the B pictures were softer than the P pictures around them and the texture
    // pulsed at the P pictures' rate (seen in a blind test against x264, whose psy-RD weighs lost
    // texture in B pictures). The detail such an average has lost against its two single
    // predictions (4x4 Hadamard AC magnitudes) counts bSharp * sqrt(lambda) per unit in the cost
    // of every B candidate that uses it - skip, direct or searched - unless the average is clearly
    // closer to the source (noise); where the loss outweighs what the average saves, the
    // macroblock copies from one picture. Runs of up to four B pictures only. 0 = off (the stream
    // is then the same as without this option). It costs BD-rate where B pictures gained by
    // averaging: camera footage -13.3 -> -9.2 % PSNR-Y against x264 (sharpness step between
    // pictures 0.075 -> 0.060, x264 0.056).
    double bSharp = 1.5;
    // Mode decision of inter macroblocks in P pictures by estimate (CABAC with the 8x8 transform):
    // the candidates (16x16 or the best split, 4x4 or 8x8 transform) are ranked by the trellis
    // quantisation's own cost and only the best one per layout is reconstructed and counted in
    // full. false = every candidate coded in full (slower; the stream differs). In B pictures
    // (since 15.11.2026) the transform of each candidate (direct, 16x16, split) is chosen the same
    // way; false = all with 4x4 and the winner once more with 8x8 (the stream before).
    bool modeEstimate = true;
    // Psycho-visual rate-distortion: 0 = off (the stream is then the same as without this option).
    // The mode decision (skip, inter, intra and their transform sizes) adds psyRd * sqrt(lambda) *
    // the difference between the magnitudes of the source's and the reconstruction's AC Hadamard
    // coefficients to a candidate's cost, so one that keeps the texture or grain of the source
    // beats one that is closer in squared error but smoother; the winner is also tried with a
    // quantisation that keeps more of the small coefficients. PSNR and SSIM drop by design.
    // Like bSharp it follows the source macroblock's texture (measured by the adaptive quantisation, so only
    // with aqStrength > 0): none below an energy of 6, in full from 9 - a flat or smoothly shaded macroblock
    // has no texture to keep (a smooth gradient at 0.5 Mbit/s: +3 dB PSNR-Y without the two there).
    double psyRd = 1.0; // 0.0 until 14.11.2026
    // The psy strength by the picture's QP: psyRd up to psyQpFull, falling linearly to 0 at psyQpOff
    // (psyQpOff <= psyQpFull = the same strength at every QP).
    int psyQpFull = 26, psyQpOff = 38;
    // Two-pass complexity compression: a picture's QP follows its complexity (first-pass cost,
    // averaged over +-1.5 s) by qcomp * 6 * log2(complexity / the clip's mean), at most +-4, so
    // calm, flat or dark stretches (a fade, a death screen) get a finer QP than busy ones. 0 = one
    // QP for the whole clip (the stream before 14.11.).
    double qcomp = 0.3;
    // Two-pass: the clip's first startBoostSeconds get a QP finer by startBoostQp, fading out over as
    // long again; the rest of the clip pays for it through the plan. What a viewer sees first decides
    // the impression, and there is no picture before it that hides anything. The boost shrinks where
    // its bits would be more than 2.5 % of the clip's (a short clip with a costly start). 0 = off
    // (the stream before 16.11.2026). Chunks after the first never get it.
    double startBoostQp = 2.0;
    double startBoostSeconds = 1.0;
    // Explicit weighted prediction for P pictures (8.4.2.3; Main/High only, so not without CABAC):
    // when the brightness of a picture differs from a reference picture's as a whole (a fade, a
    // flash), the slice carries a weight and an offset per reference and the prediction is scaled
    // by them. Decided per picture from 16x16 block means of the two source pictures; pictures
    // without such a change are coded exactly as without the option (the stream then only has a
    // second picture parameter set in its headers).
    bool weightedPred = true;
    // chroma_qp_index_offset (-12..12): the chroma planes' QP against luma's. Negative = finer
    // chroma: fewer coloured blotches on smooth, coloured areas (smoke, sky) for a few bits.
    int chromaQpOffset = 0;
    // Weight of the chroma planes' squared error in the macroblock decisions (skip, inter, intra)
    // against luma's (1 = sample for sample). A chroma sample covers four pixels, and a small
    // coloured error (the ghost of a HUD element dragged along by a skip, a tint on smoke) is
    // cheap in squared error but plain to see: with more weight such macroblocks get their residual.
    int chromaRdWeight = 1;
    // Video buffering verifier (HRD, Annex C): with both > 0 no access unit may make the decoder's coded
    // picture buffer (vbvBufsizeKbits, filled at vbvMaxrateKbps) run empty - for players and services
    // with a peak rate (streaming, YouTube's recommended maxrate/bufsize). Works with every rate mode
    // (constant QP, crf, one-pass and two-pass bitrate): the mode's QP stays where the buffer allows it;
    // where the planned pictures (lookahead) would drain the buffer, the QP rises as little as keeps
    // it out of trouble. The stream then carries the limits (VUI nal_hrd_parameters, buffering period
    // and picture timing SEI). 0 = off (the stream is then the same as without these options).
    int vbvMaxrateKbps = 0;
    int vbvBufsizeKbits = 0;
    double vbvInit = 0.9; // buffer fullness at the first picture's removal (fraction of vbvBufsizeKbits)
    // At least this full at the end (fraction; 0 = no demand): set by ChunkPlan::configFor for all chunks but the last.
    double vbvEndFill = 0;
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
    double crfComplexity = -1.0;                                  // constant quality: the clip's (crfClipComplexity)
    // Configuration of chunk i: cfg with its bitrate, first pass, idr_pic_id base and threads.
    EncoderConfig configFor(const EncoderConfig& cfg, int i, int threads) const;
};
// minFrames: shortest chunk; 0 = 15 s of pictures (each seam costs about 8 %·s of BD-rate over the
// clip, measured: 15 s chunks stay near 0.5 %). Short clips get one chunk.
// VBV in chunks (ChunkPlan::configFor sets each chunk's buffer at its start and end): the first access unit of a
// chunk after the first, with its picture timing counted from the previous chunk's last buffering period -
// framesSinceBp = Encoder::vbvFramesSinceBp() of the chunk before after its last access unit. Unchanged without VBV.
std::vector<uint8_t> vbvJoinChunk(const std::vector<uint8_t>& firstAu, int framesSinceBp);
inline constexpr double kChunkVbvFill = 0.5, kChunkVbvEnd = 0.55;
ChunkPlan planChunks(const FirstPassStats& stats, const EncoderConfig& cfg, int chunks, int minFrames = 0);
// Constant quality: the clip's mean complexity from the first pass (crfQp's measure), < 0 without pictures.
double crfClipComplexity(const FirstPassStats& stats, const EncoderConfig& cfg);

// Where B pictures pay, measured instead of estimated: every kInterval pictures and after each
// scene change or keyframe the caller asks for, the next few half-size pictures (the lookahead's)
// are coded for real - with runs of two B pictures and twice without (two QPs, for the slope of
// bits over distortion). From there on B pictures are placed only if they needed clearly fewer
// bits at equal distortion; where they did and longer runs are allowed, the long run is measured
// against the short one the same way. The lookahead costs cannot tell any of this: a slow zoom (B
// pictures at a third of the bits) and noise that changes with every picture (B pictures a loss)
// look alike to them, and they see a zoom's distant P pictures as far too expensive.
class BProbe {
public:
    static constexpr int kInterval = 120;
    // The next picture in display order (prepared by the lookahead). True when an answer came
    // with it: run() then holds from picture resultStart() on (the second answer for the same
    // pictures replaces the first).
    // background: the first answer is measured on its own thread and comes with a later call (or
    // collectFirst()); its content does not depend on when.
    bool add(const Lookahead& la, const LookaheadFrame& f, int index, int bframes, int threads, bool background = false);
    bool collectFirst();
    BProbe() = default;
    ~BProbe();
    BProbe(const BProbe&) = delete;
    BProbe& operator=(const BProbe&) = delete;
    int run() const { return run_; } // longest run of B pictures that pays, 0 = none
    int resultStart() const { return resultStart_; }
    // run() with kClear set where the measurement found B pictures a clear gain or chose long runs
    // (FirstPassStats::bGate): such a clip may start with B pictures.
    static constexpr int kClear = 0x80;
    uint8_t gate() const { return uint8_t(run_ | (clear_ && run_ > 0 ? kClear : 0)); }
    // Bits of "I B.. P .." with runs of runs[k] B pictures against "I P.. P" at equal distortion,
    // counted over the pictures after the first (gains[k] = -0.5: half the bits; 0 = no
    // measurement). pics: w x h luma each, in display order.
    // Returns 0 = measured, 1 = next to nothing is coded (a still picture), 2 = no usable measurement.
    static int measure(const std::vector<std::vector<uint8_t>>& pics, int w, int h, int threads, const int* runs, int runCount,
                       double* gains);

private:
    bool startsAt(const LookaheadFrame& f, int index) const;
    std::vector<std::vector<uint8_t>> pics_;
    int start_ = -1, lastStart_ = -1, resultStart_ = 0;
    int run_ = 0;
    bool clear_ = false;
    // The first answer under way: where it starts, what was asked, and the measurement (thread).
    bool firstPending_ = false;
    int firstStart_ = 0, firstShort_ = 0, firstBframes_ = 0, firstResult_ = 0;
    double firstGain_ = 0;
    std::thread firstThread_;
    std::exception_ptr firstError_;
    // Activity (lookahead cost with motion / intra) of the last picture, summed over the pictures
    // being collected, and the mean of the last measurement's pictures.
    double lastActivity_ = 0, activitySum_ = 0, sampleActivity_ = 0;
};

struct Picture {
    const uint8_t* y = nullptr;
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    int strideY = 0;
    int strideC = 0;
};

// The encoder exists twice in a build that has the second one (CMake: SCHNEIDI_S264_V3, x86-64 Linux): compiled
// for every CPU, and compiled for x86-64-v3 (AVX2, BMI2, ... - the compiler then uses them everywhere, about 5 %
// fewer cycles). Encoder and FirstPass hand all their work to the second one where the CPU has these
// instructions (asked once per object); the streams are the same either way (no fused multiply-adds, see
// Simd.h). The second build's symbols are all local to its object file except the two functions that make
// these; SCHNEIDI_S264_CPU=generic in the environment keeps to the first build (for comparing the two).
struct EncoderAlt {
    // What Encoder's inline accessors answer from, fetched after every call that may change it.
    struct State {
        bool sceneCut = false;
        int displayIndex = -1, weightedPictures = 0, qp = 0, vbvUnderflows = 0, vbvFramesSinceBp = 0;
        char type = 'I';
        double vbvMinFill = 0;
    };
    virtual ~EncoderAlt() = default;
    virtual std::vector<uint8_t> headers() const = 0;
    virtual std::vector<uint8_t> encode(const Picture& picture, bool forceKeyframe) = 0;
    virtual std::vector<uint8_t> flush() = 0;
    virtual State state() const = 0;
    virtual const std::vector<uint8_t>& recon(int plane) const = 0;
};
struct FirstPassAlt {
    virtual ~FirstPassAlt() = default;
    virtual void add(const Picture& picture, bool forceKeyframe) = 0;
    virtual std::shared_ptr<const FirstPassStats> stats() const = 0;
};
// "x86-64-v3" where new Encoder and FirstPass objects use the second build, else "generic".
const char* cpuBuild();
// The CPU (and operating system) can run code built with -march=x86-64-v3: AVX, AVX2, BMI1/2, FMA, F16C, LZCNT,
// MOVBE and the x86-64-v2 set, with XCR0 enabling the SSE and AVX state. False off x86 and on other compilers.
bool cpuHasX86_64V3();

// Cheap first pass for the bitrate mode: the half-resolution lookahead analysis of every
// picture of the clip (a few ms per 1080p picture), without encoding anything.
class FirstPass {
public:
    // threads > 1: pictures are analysed in batches side by side (same numbers).
    // bframes > 0: also what the adaptive B picture placement needs for runs of up to that many
    // (the probe: about 3 pictures' worth of encoding at half size every 120 pictures).
    // keepAnalysisBytes: memory for FirstPassStats::analysis (about 12 bytes per macroblock and
    // picture: 1080p ~100 KB); pictures beyond it are analysed again by the encoder.
    FirstPass(int width, int height, int threads = 1, int bframes = 0, size_t keepAnalysisBytes = size_t(1) << 30);
    ~FirstPass();
    void add(const Picture& picture, bool forceKeyframe = false);
    std::shared_ptr<const FirstPassStats> stats() const;

private:
    void analyseBatch() const;
    void startBatch() const;
    void joinWorker() const;
    std::unique_ptr<FirstPassAlt> alt_; // does the work instead (see EncoderAlt)
    int width_, height_, threads_;
    std::unique_ptr<Lookahead> la_;
    // Pictures waiting for the next batch, the batch under analysis (on worker_ with threads > 1,
    // which alone touches it, prev_, the probe and the statistics until joined) and the last
    // analysed picture (their predecessor).
    mutable std::vector<LookaheadFrame> batch_, job_;
    mutable std::thread worker_;
    mutable std::exception_ptr workerError_;
    mutable LookaheadFrame prev_;
    mutable bool havePrev_ = false;
    mutable BProbe probe_;
    mutable BPlanner planner_;
    int bframes_ = 0;
    size_t keepBytes_ = 0;
    mutable size_t keptBytes_ = 0;
    std::shared_ptr<FirstPassStats> stats_;
};

class Encoder {
public:
    explicit Encoder(const EncoderConfig& config);
    ~Encoder();

    // SPS + PPS as Annex B.
    std::vector<uint8_t> headers() const;
    // Queues a picture; returns the next access unit (IDR or P picture, Annex B) or nothing: while
    // the lookahead is still filling, while the first pictures are in flight (frame threading), and
    // once more where the frame threading goes from two to three pictures in flight. forceKeyframe: start a new GOP at this picture (e.g. a cut in
    // the timeline). Pictures come out in order, `delay()` pictures after they went in.
    std::vector<uint8_t> encode(const Picture& picture, bool forceKeyframe = false);
    // Exceptions (also those on the encoder's own threads, e.g. std::bad_alloc) come out of
    // encode()/flush(); the encoder is then finished: every later call throws the same again.
    // After the last picture: the remaining access units, one per call, empty when done.
    std::vector<uint8_t> flush();
    int delay() const;
    bool lastWasSceneCut() const { return lastWasSceneCut_; }
    // The access unit that came out last: its position in input (display) order, counted from 0,
    // and its type ('I', 'P', 'B'). Without B pictures the display index simply counts up.
    int lastDisplayIndex() const { return lastDisplayIndex_; }
    // P pictures coded with explicit weighted prediction so far (fades, flashes).
    int weightedPictures() const { return weightedPictures_; }
    char lastType() const { return lastType_; }
    // Pictures an access unit may come out ahead of its display turn (the dts shift a muxer needs).
    int reorderDelay() const { return usePyramid() ? 2 : useB() ? 1 : 0; }

    // Reconstruction of the last picture that came out, as the decoder outputs it (padded size).
    const std::vector<uint8_t>& reconY() const;
    const std::vector<uint8_t>& reconU() const;
    const std::vector<uint8_t>& reconV() const;
    int paddedWidth() const { return padW_; }
    int paddedHeight() const { return padH_; }
    int lastQp() const { return lastQp_; }
    // VBV (EncoderConfig::vbvMaxrateKbps): access units that ran the buffer empty so far (should stay 0), and the
    // lowest buffer level just after a removal, in bits (exact: from the access units' real sizes).
    int vbvUnderflows() const { return vbvUnderflows_; }
    double vbvMinFill() const { return vbvMinFill_; }
    // Pictures since the last access unit with a buffering period, counting the next one (vbvJoinChunk).
    int vbvFramesSinceBp() const { return frameIndex_ - vbvBpFrame_; }

private:
    struct Pool;
    std::unique_ptr<EncoderAlt> alt_; // does the work instead (see EncoderAlt)
    void takeAltState();
    FrameCoder* startFrame(LookaheadFrame& frame);
    std::vector<uint8_t> finishFrame(FrameCoder& fc);
    std::vector<uint8_t> step(LookaheadFrame* next);
    bool pipelined() const { return cfg_.frameThreads && useCabac() && cfg_.threads >= 2; }
    void applyRc(int upTo);
    std::unique_ptr<Pool> pool_;
    // Frame threading: up to kInFlight pictures are started before the oldest comes out, so the
    // workers have rows of two pictures while the caller prepares the next one (tree, rate control).
    static constexpr int kInFlight = 3;
    // In flight, the one that came out last (reconY) and the one before the oldest in flight
    // (its vectors are start points).
    static constexpr int kCoders = kInFlight + 1;
    int inFlightAt(int index) const;
    std::deque<FrameCoder*> inFlight_;         // started, not finished (frame threading), oldest first
    const FrameCoder* lastStarted_ = nullptr;  // the picture before the next one (search start points)
    // Rate control learns from a picture inFlightAt() pictures later (the ones between may still be
    // coded), with or without frame threading, so the stream does not depend on the thread count.
    struct RcRecord {
        int index;
        int type; // 0 I, 1 P, 2 B
        bool afterB; // a P picture after a run of B pictures
        bool lookahead;
        double cost;
        int bin; // two-pass: a B picture's cost class (-1: none)
        int qp;
        double bits;
    };
    std::deque<RcRecord> rcDone_;
    void markKeyframes();
    // Constant quality (EncoderConfig::crf).
    bool crfOn() const { return cfg_.crf >= 0 && cfg_.bitrateKbps <= 0; }
    std::vector<float> crfCost_; // per picture (display index): its lookahead cost against the picture before
    std::vector<double> crfSum_; // prefix sums of crfCost_ over the pictures that do not repeat the one before
    std::vector<int> crfCount_;  // and their count
    double crfTreeShift_ = 0;    // the last reference picture's tree at full strength against the applied (mean)
    void noteCost(const LookaheadFrame& f);
    double crfQp(const LookaheadFrame& frame, double* complexity = nullptr) const;
    bool useLookahead() const
    {
        return (cfg_.lookahead > 0 && cfg_.gop > 1 && (cfg_.bitrateKbps > 0 || cfg_.mbtree || crfOn())) || useB();
    }
    // B pictures: the queue holds the pictures in display order; planQueue gives the undecided
    // ones their type, pickNext the next one in coding order (the reference picture after a run
    // of B pictures goes first and stays queued until they are coded).
    bool useB() const { return cfg_.bframes > 0 && useCabac() && cfg_.gop > 1; }
    bool usePyramid() const { return useB() && cfg_.bPyramid && cfg_.bframes >= 3 && cfg_.refs < 16; }
    // Explicit weighted prediction is not part of the Baseline profile (no CABAC = Baseline here).
    int chromaOffset() const { return cfg_.chromaQpOffset; } // clamped by the constructor
    bool useWeightedPred() const { return cfg_.weightedPred && useCabac() && cfg_.gop > 1; }
    // Adaptive placement: from the first pass when there is one (so its plan holds), else from the queue.
    bool bAdapt() const { return useB() && cfg_.bAdaptive && (!cfg_.firstPass || cfg_.firstPass->bMax >= cfg_.bframes); }
    // Picture types of the whole clip from the first pass (bAdapt with a first pass), and the
    // lookahead cost of each picture as planned.
    void planFromStats();
    // Without a first pass: the probe sees every picture as it joins the queue; from each
    // measurement's first picture on its result holds.
    BProbe probe_;
    BPlanner planner_;
    std::vector<std::pair<int, int>> gate_; // (first picture, longest run of B pictures that pays), in order
    void noteGate(const LookaheadFrame& f);
    // The lookahead's searches along the planned references of B pictures: collected when a
    // picture starts, run beside it (frame threading) and finished before the next one's tree.
    struct BSearchJob {
        int kind; // 0 searchFwd(f from other), 1 searchBack(f from other, third = the picture after f), 2 biCost(f between third and other)
        LookaheadFrame *f, *other, *third;
        int a, c;
    };
    std::vector<BSearchJob> bSearchJobs_;
    std::thread bSearchThread_;
    std::exception_ptr bSearchError_;
    void ensureBSearches();
    void runBSearches(int threads);
    void takeLookaheadVectors(const LookaheadFrame& frame, FrameCoder& fc, int type) const;
    void joinBSearches();
    bool flushing_ = false; // no more pictures come in: the whole queue's types are settled
    int gateAt(int index) const;
    bool movingAt(int index) const;
    std::vector<uint8_t> statType_;
    std::vector<double> statCost_;
    double queueCost(size_t j) const; // lookahead cost of queued picture j as planned (no tree)
    void planQueue();
    LookaheadFrame* pickNext();
    std::vector<uint8_t> codeNext();
    bool useCabac() const { return cfg_.cabac && cfg_.mode != IntraMode::Pcm; }
    bool useT8() const { return useCabac() && cfg_.transform8x8; }

    EncoderConfig cfg_;
    int mbW_ = 0, mbH_ = 0, padW_ = 0, padH_ = 0;
    // Pictures in flight (round robin) and the one that came out last.
    std::unique_ptr<FrameCoder> coders_[kCoders];
    int nextCoder_ = 0;
    const FrameCoder* last_ = nullptr;
    std::deque<std::shared_ptr<RefPic>> refList_; // reference pictures, newest first
    // B pyramid: the reference B picture of the run being coded (its display index and frame_num);
    // the next P or I picture takes it out of the decoder's buffer again.
    std::shared_ptr<RefPic> bRefPic_;
    int bRefIndex_ = -1, bRefFrameNum_ = -1;
    int lastRefIndex_ = -1; // display index of the last I or P picture that left the queue
    std::vector<std::shared_ptr<RefPic>> refSpare_; // retired ones, used again once no picture in flight holds them
    int lastQp_ = 0;
    int frameIndex_ = 0;
    int idrCount_ = 0;
    int framesSinceIdr_ = 0;
    bool lastWasSceneCut_ = false;
    int lastDisplayIndex_ = -1;
    int weightedPictures_ = 0;
    char lastType_ = 'I';
    int inCount_ = 0;      // pictures taken in (display index of the next one)
    int gopPos_ = 0;       // B mode: display distance from the last IDR picture of the first undecided picture
    int refCount_ = 0;     // B mode: reference pictures since the last IDR picture (frame_num)
    std::vector<uint8_t> prevThumb_; // quarter-size luma of the previous picture
    // Lookahead queue (front = next picture to encode) and the macroblock tree's QP offsets.
    std::unique_ptr<Lookahead> la_;
    std::deque<LookaheadFrame> queue_;
    std::unique_ptr<LookaheadFrame> pendingFrame_; // being analysed on pendingThread_ while a picture is encoded
    std::thread pendingThread_;
    std::exception_ptr pendingError_; // thrown on pendingThread_, rethrown by joinPending
    std::exception_ptr broken_;       // a failed encode()/flush(): every later call throws it again
    void joinPending();
    // Lookahead analysis of f against prev, or the first pass's when it kept it.
    void analyseFrame(LookaheadFrame& f, const LookaheadFrame* prev) const;
    std::vector<uint8_t> encodeImpl(const Picture& picture, bool forceKeyframe);
    std::vector<uint8_t> flushImpl();
    // Rate control state.
    double rcQp_ = 0;               // QP for P frames (I frames use 3 less)
    double rcBits_ = 0, rcTarget_ = 0; // produced / wanted bits so far
    double rcCplxSum_ = 0, rcCplxCount_ = 0; // complexity (bits * 2^(qp/6)) of the P frames so far
    // Lookahead model: bits = k * cost * 2^(-qp/6), k per picture type from the pictures coded so far.
    double rcKSum_[3] = {0, 0, 0}, rcCostSum_[3] = {0, 0, 0}; // [0] I, [1] P, [2] B
    double rcNeedSeen_ = 0; // predicted bits at QP 0 of the pictures coded so far
    int rcFramesSeen_ = 0;
    // First-pass rate control: the pictures' need (bits per k, see tpNeed) by class of picture and
    // QP of the clip's P pictures, summed over the pictures not coded yet / over all of them.
    // Classes: 0 I, 1 B (all in one), then the P pictures by kind and cost class (see below), then the B pictures by cost class.
    static constexpr int kTpQMin = -12, kTpQSteps = 2 * 76 + 1; // QP -12 .. 64 in half steps
    std::vector<double> tpRemTab_, tpAllTab_;                   // [class * kTpQSteps + step]
    std::vector<float> tpQOff_;                                 // per picture: its QP against the P QP (type, run, qcomp)
    std::vector<uint8_t> tpClass_;
    std::vector<int32_t> tpRef0_, tpRef1_;                      // per picture: its references as planned, where not the picture before (-1)
    double tpRawNeed(size_t j, bool key, double scale) const;
    double tpNeed(size_t i, double qp, bool plain = false) const;
    double tpRunCost(size_t i) const;
    void tpRow(size_t i, double* row) const;
    void tpBuildTables();
    void tpStartBoost(double kI, double kP);
    double tpBits(const std::vector<double>& tab, double qp, double kI, double kP, double kB) const;
    double tpSolve(const std::vector<double>& tab, double budget, double kI, double kP, double kB) const;
    // P pictures by cost per macroblock (half octaves): bits per cost are far from constant (a calm
    // picture's residual mostly quantises to nothing, a busy one's does not), so k is learned per
    // class and the not yet coded pictures of each class are predicted with their own k.
    static constexpr int kRcBins = 28;
    // Two kinds of P pictures, each with its own classes: [0] after another I or P picture, [1]
    // after a run of B pictures. The first pass knows every picture against the one before only; a P
    // picture several pictures after its reference needs more bits per cost than that (and with the
    // macroblock tree it carries the run's quality), and how much more depends on the material, so
    // where B pictures are placed in parts of a clip the two must not share one k.
    double rcKBin_[3][kRcBins] = {}, rcCostBin_[3][kRcBins] = {}; // [2]: the B pictures (two-pass)
    static constexpr int kTpClasses = 2 + 3 * kRcBins;
    double rcKSumPb_ = 0, rcCostSumPb_ = 0; // all P pictures after B runs coded so far
    std::vector<uint8_t> tpAfterB_;         // per picture of the plan: a P picture after a run of B pictures
    // Per picture QP offset from its (time-smoothed) complexity against the clip's (two-pass
    // complexity compression) and the cost weight 2^(-offset/6) the bit model applies with it.
    std::vector<uint8_t> tpKey_, tpType_;
    std::vector<float> tpOff_;
    int rcBin(double cost) const;
    double kPOfBin(int kind, int bin, double kP) const;
    double twoPassQp(double kI, double kP, double kB, double planQp = -1) const;
    double tpGap_ = 0; // two-pass: the gap between the QP that spends what is left and the plan QP, averaged (gTpLag)
    double twoPassPlanQp(double kI, double kP, double kB) const;
    double frameCost(const LookaheadFrame& f, bool intra, const std::vector<int32_t>* inter = nullptr) const;
    double treeStrength(const LookaheadFrame& f) const;

    // VBV: a buffer model per picture in coding order, level = bits in the buffer just before the next removal.
    // The decision side learns the real sizes as late as the rate control does (applyRc's lag, so the stream
    // does not depend on the thread count) and predicts the pictures in flight; the output side (finishFrame)
    // follows the real sizes for the HRD SEI and the underflow count.
    bool vbvOn() const { return cfg_.vbvMaxrateKbps > 0 && cfg_.vbvBufsizeKbits > 0; }
    // As signalled (hrd_parameters: the rate in steps of 64 bit/s rounded up, the size in steps of 16 bits
    // rounded down), so the model is the decoder's.
    double vbvRate() const { return std::ceil(cfg_.vbvMaxrateKbps * 1000.0 / 64.0) * 64.0; }
    double vbvSize() const { return std::floor(cfg_.vbvBufsizeKbits * 1000.0 / 16.0) * 16.0; }
    double vbvTick() const { return vbvRate() / (cfg_.fps > 0 ? cfg_.fps : 60.0); }
    struct VbvRecord {
        int frameNo;
        int type;
        double qp, cost, bits;
    };
    std::deque<VbvRecord> vbvDone_;           // finished, not learned yet
    std::deque<std::pair<int, double>> vbvPred_; // decided, not learned yet: coding index, predicted bits
    double vbvFill_ = 0;                      // decision side, after the learned pictures
    // Bits * 2^(qp/6) / cost per class (I, P, B, P after a run of B pictures: its cost is over a longer distance), decaying.
    double vbvKNum_[4] = {}, vbvKDen_[4] = {};
    int vbvClass(const LookaheadFrame& f, int type) const;
    double vbvOutFill_ = 0;                   // output side
    int vbvBpFrame_ = 0;                      // coding index of the last access unit with a buffering period
    int vbvUnderflows_ = 0;
    double vbvMinFill_ = 0;
    double vbvLow_ = 0; // the buffer before the picture being decided, the pictures in flight at gVbvSafe times their prediction
    size_t vbvHeaderBytes_ = 0;               // SPS/PPS in front of the first access unit (they count in its bits)
    void vbvLearn(int upTo);
    double vbvK(int type) const;
    double vbvQNum_[4] = {}, vbvQDen_[4] = {}; // the QP k was learned at, decaying mean
    double vbvPredict(int type, double cost, double qp) const;
    double vbvCost(const LookaheadFrame* f, int type, double* shift) const;
    void vbvProbe(const LookaheadFrame& frame, double qpI);
    double vbvRaise(const LookaheadFrame* frame, int type, double qp, double cost, double shift, int bQp);
    std::vector<uint8_t> vbvSei(const FrameCoder& fc, bool idr);

};

} // namespace schneidi::h264
