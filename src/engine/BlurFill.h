// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Blurred fill around the fitted picture (ClipTransform::fill == FitFill::Blur): the clip's own picture, scaled up to
// cover the frame and blurred, behind the transformed picture - a 16:9 clip in a 9:16 timeline on one track with one
// decode. Before, that took a second copy of every clip on the track below (decoded twice, blurred at full size;
// 8 of 60 pictures per second in the preview of an AI's TikTok edit, docs archive 2026-10-26).
//
// The background has the frame's shape and its blur counts in shares of the frame height, so preview and export
// (any output size) show the same picture.

#include <cstdint>
#include <vector>

namespace BlurFill {

// The small background: RGBA with premultiplied alpha (a transparent source, e.g. keyed, stays transparent)
struct Background {
    int width = 0, height = 0;
    bool opaque = true; // every alpha 255 (then composite() needs no division)
    std::vector<uint8_t> rgba;
};

// Background from a picture (rgba, w x h, straight alpha): the part of it that covers a frame of width/height
// `frameAspect`, centered inside the crop (shares of the picture: left, top, right, bottom), averaged down and blurred.
// `pictureAspect` = width/height of the picture as displayed (pixel aspect, rotation), <= 0 = w/h. blur 0..100 and
// dim 0..100 % like ClipTransform::fillBlur/fillDim.
Background makeBackground(const uint8_t* rgba, int w, int h, double pictureAspect, const double crop[4],
                          double frameAspect, double blur, double dim);

// Lay the picture (rgba, w x h, straight alpha) onto the background scaled up to w x h, in place
void composite(uint8_t* rgba, int w, int h, const Background& bg);


} // namespace BlurFill
