// Lookahead of the H.264 encoder: queued source pictures with a cheap half-resolution analysis
// (intra and motion-compensated SATD per macroblock) that drives the rate control and the
// macroblock tree (lower QP where later pictures will reference a block).
#pragma once

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
    double intraSum = 0, interSum = 0;         // picture totals: all intra / min(intra, inter)
    bool keyframe = false;                     // the schedule or the caller makes this an IDR picture
    std::vector<uint8_t> thumb;                // quarter-size luma for the scene change detection
    bool sceneChange = false;                  // motion no longer helps against the previous picture
};

class Lookahead {
public:
    Lookahead(int width, int height, int mbW, int mbH);

    // Copies the picture and analyses it against prev (nullptr: the first picture or a keyframe).
    void analyse(LookaheadFrame& f, const LookaheadFrame* prev) const;

    // Macroblock tree over the queue: propagates the cost of every block back through the
    // motion vectors to the blocks it was predicted from, last picture first. Keyframes do not
    // propagate into their predecessor.
    void propagate(std::deque<LookaheadFrame>& frames) const;

    // QP offset of a macroblock from its propagated cost (strength 2 at x264's default qcomp).
    static float qpOffset(const LookaheadFrame& f, int mb, double strength);

    int halfW() const { return halfW_; }
    int halfH() const { return halfH_; }

private:
    int width_, height_, mbW_, mbH_, halfW_, halfH_;
};

} // namespace schneidi::h264
