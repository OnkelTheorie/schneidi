// Lookahead of the H.264 encoder: queued source pictures with a cheap half-resolution analysis
// (intra and motion-compensated SATD per macroblock) that drives the rate control and the
// macroblock tree (lower QP where later pictures will reference a block).
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>

namespace schneidi::h264 {

struct LookaheadFrame {
    std::vector<uint8_t> y, u, v; // source copy, strides width / width/2
    bool forceKeyframe = false;
    std::vector<uint8_t> half;                 // half-resolution luma, one 8x8 block per macroblock
    std::vector<int32_t> intraCost, interCost; // per macroblock: SATD of the best intra / motion prediction
    std::vector<int16_t> mvx, mvy;             // per macroblock, half-pel units of the half picture, into the previous frame
    std::vector<float> propagate;              // macroblock tree: cost that later pictures inherit from this block
    // The macroblock tree's links of this picture into its predecessor, made from the costs and vectors
    // above on first use (Lookahead::propagate) and dropped whenever they change.
    struct TreeLinks {
        std::vector<int32_t> mb;          // source block (blocks that propagate something, raster order)
        std::vector<float> intra, diff;   // float(intra), float(intra - inter)
        std::vector<uint8_t> count;       // targets of each source block (1..4)
        std::vector<int32_t> target;      // blocks in the predecessor, all sources one after another
        std::vector<float> weight;        // overlap of each target in 1/256 of a block
        bool ready = false;
    } treeLinks;
    double intraSum = 0, interSum = 0;         // picture totals: all intra / min(intra, inter)
    bool keyframe = false;                     // the schedule or the caller makes this an IDR picture
    std::vector<uint8_t> thumb;                // quarter-size luma for the scene change detection
    bool sceneChange = false;                  // motion no longer helps against the previous picture
    // B pictures (Encoder::planQueue): the picture's place and type.
    int index = 0;        // display index
    int gopPos = 0;       // display distance from the IDR picture of its GOP (half its picture order count)
    uint8_t type = 1;     // 0 I, 1 P, 2 B
    bool decided = false; // the type is final
    bool coded = false;   // a reference picture coded ahead of the B pictures before it
    bool cutKey = false;  // keyframe because of a detected scene change
    // How badly the picture is predicted when the picture before it is left out: its cost against
    // the picture two back over the summed costs of both single steps (0.5 = steady motion, towards
    // 1 and above = motion that changes from picture to picture). -1 = not measured.
    double bScore = -1;
};

class Lookahead {
public:
    Lookahead(int width, int height, int mbW, int mbH);

    // Copies the picture and analyses it against prev (nullptr: the first picture or a keyframe).
    void analyse(LookaheadFrame& f, const LookaheadFrame* prev) const;
    // The two halves of analyse: the half-resolution picture and thumbnail (independent of other
    // pictures), then the costs against prev (which must be prepared).
    void prepare(LookaheadFrame& f) const;
    void search(LookaheadFrame& f, const LookaheadFrame* prev) const;
    // search = sceneCut (needs only prev's thumbnail) + costs (starts from prev's vectors, so
    // pictures go one after another).
    void sceneCut(LookaheadFrame& f, const LookaheadFrame* prev) const;
    // seed: another start point of the search, that picture's vector times seedScale.
    // prevRows / ownRows (threads = 1): for analysing consecutive pictures side by side; a row waits
    // until prev's analysis (on another thread) counts more rows done than its index, and the rows
    // done here are counted in ownRows (the caller sets it past the last row should this throw).
    void costs(LookaheadFrame& f, const LookaheadFrame* prev, int threads = 1, const LookaheadFrame* seed = nullptr,
               int seedScale = 1, const std::atomic<int>* prevRows = nullptr, std::atomic<int>* ownRows = nullptr) const;
    // bScore of f (analysed against prev) from a second search against prev2, the picture before prev.
    double bScore(const LookaheadFrame& f, const LookaheadFrame& prev, const LookaheadFrame& prev2) const;

    // Macroblock tree over the queue: propagates the cost of every block back through the
    // motion vectors to the blocks it was predicted from, last picture first. Keyframes do not
    // propagate into their predecessor.
    void propagate(std::deque<LookaheadFrame>& frames) const;

    // QP offset of a macroblock from its propagated cost (strength 2 = 5 * (1 - 0.6), i.e. a
    // quantiser-curve compression of 0.6).
    static float qpOffset(const LookaheadFrame& f, int mb, double strength);

    int halfW() const { return halfW_; }
    int halfH() const { return halfH_; }

private:
    int width_, height_, mbW_, mbH_, halfW_, halfH_;
};

} // namespace schneidi::h264
