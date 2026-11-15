// Lookahead of the H.264 encoder: queued source pictures with a cheap half-resolution analysis
// (intra and motion-compensated SATD per macroblock) that drives the rate control and the
// macroblock tree (lower QP where later pictures will reference a block).
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>

namespace schneidi::h264 {

// One motion search of a picture against another picture at the lookahead's resolution.
struct LookaheadSearch {
    std::vector<int32_t> cost;     // per macroblock: SATD + vector bits of the best motion prediction
    std::vector<int16_t> mvx, mvy; // half-pel units of the half picture
    double sum = 0;                // sum over the macroblocks of min(intra, cost)
    bool valid() const { return !cost.empty(); }
};

// B pictures with up to maxB in a row: index of the combination "a pictures after the reference
// before, c before the one after" (a, c >= 1, a + c <= maxB + 1).
inline int bComboCount(int maxB) { return maxB * (maxB + 1) / 2; }
inline int bComboIndex(int a, int c, int maxB) { return (c - 1) * (maxB + 1) - (c - 1) * c / 2 + (a - 1); }

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
    // B pictures (Lookahead::searchFwd / searchBack / biCost), each kept in the later picture of the two:
    std::vector<LookaheadSearch> fwd;          // [d], d >= 2: this picture predicted from the one d before ([1] = interCost)
    std::vector<LookaheadSearch> back;         // [c], c >= 1: the picture c before predicted from this one
    std::vector<std::vector<int32_t>> bi;      // [bComboIndex(a, c)]: the picture c before, from the average of the
                                               // predictions out of the picture a before it and this one
    std::vector<double> biSum;                 // [bComboIndex(a, c)]: that picture's total as a B picture (min of intra,
                                               // both lists and the average per macroblock)
    bool bRef = false;                         // a B picture other B pictures refer to (B pyramid)
    bool nearB = false;                        // a reference picture with B pictures before or after it
    int refDist = 1, nextDist = 1;             // planned distance to the reference before (P, B) / after (B)
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
    // src: the picture whose half picture is searched (default f itself).
    // prevRows / ownRows (threads = 1): for analysing consecutive pictures side by side; a row waits
    // until prev's analysis (on another thread) counts more rows done than its index, and the rows
    // done here are counted in ownRows (the caller sets it past the last row should this throw).
    void costs(LookaheadFrame& f, const LookaheadFrame* prev, int threads = 1, const LookaheadFrame* seed = nullptr,
               int seedScale = 1, const LookaheadFrame* src = nullptr, const std::atomic<int>* prevRows = nullptr,
               std::atomic<int>* ownRows = nullptr) const;
    // B pictures: the searches the macroblock tree and the rate control need along the planned
    // references (maxB = longest run). searchFwd: f from ref, the picture d >= 2 before it (f.fwd[d]).
    // searchBack: b, the picture c before next, from next (next.back[c]); after = the picture right
    // after b (next itself for c = 1), whose vectors start the search. biCost: b between r0 (a
    // before it) and next (c after it), after both of its searches (next.bi, next.biSum).
    void searchFwd(LookaheadFrame& f, const LookaheadFrame& ref, int d, int maxB, int threads = 1) const;
    void searchBack(LookaheadFrame& next, const LookaheadFrame& b, const LookaheadFrame& after, int c, int maxB, int threads = 1) const;
    void biCost(LookaheadFrame& next, const LookaheadFrame& b, const LookaheadFrame& r0, int a, int c, int maxB, bool weighted) const;

    // Macroblock tree over the queue: propagates the cost of every block back through the
    // motion vectors to the blocks it was predicted from, last picture first. Keyframes do not
    // propagate into their predecessor.
    void propagate(std::deque<LookaheadFrame>& frames) const;
    // The same along the real references of planned B pictures (LookaheadFrame::type, refDist,
    // nextDist): non-reference B pictures pass their cost to both references (half each where the
    // average of both predicts best), P pictures to the reference picture before them.
    void propagateB(std::deque<LookaheadFrame>& frames, int maxB, bool weighted) const;

    // QP offset of a macroblock from its propagated cost (strength 2 = 5 * (1 - 0.6), i.e. a
    // quantiser-curve compression of 0.6).
    static float qpOffset(const LookaheadFrame& f, int mb, double strength);

    int halfW() const { return halfW_; }
    int halfH() const { return halfH_; }
    // The half picture of a prepared frame without its border: halfW() x halfH(), rows one after another.
    std::vector<uint8_t> halfPicture(const LookaheadFrame& f) const;

private:
    void searchInto(const LookaheadFrame& cur, const LookaheadFrame& ref, LookaheadSearch& out, const LookaheadFrame* seed,
                    int seedScale, int threads) const;
    int width_, height_, mbW_, mbH_, halfW_, halfH_;
};

} // namespace schneidi::h264
