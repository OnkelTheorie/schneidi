#pragma once
// Own video stabilization, the computation part (no MLT, works on plain pictures, testable): global camera motion
// between frames, the smoothed camera path with the zoom that hides the borders, and the warp that applies it.
//
// Motion: corner features (one per grid cell, Shi-Tomasi score) followed from frame to frame with pyramidal
// Lucas-Kanade on the luma plane, started from a coarse whole-picture shift search; a similarity transform (shift,
// rotation, scale) is fitted to them with RANSAC, so moving objects do not count. Cells that keep still while the
// picture moves (a game HUD, a logo, black bars) are left out after a few frames.
//
// Coordinates ("normalized"): centered on the picture, in picture widths; y uses the same unit on screen (display
// aspect ratio), so one result fits every output size with that aspect ratio.

#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace StabilizeMotion {

using Complex = std::complex<double>;

// Motion from the previous frame to this one: a point p of the previous frame is at scale * R(angle) * p + (dx, dy)
struct Motion {
    double dx = 0, dy = 0, angle = 0, scale = 1;
    bool valid = true; // false = not measurable (cut, flash, no texture): treated as no motion
};

// Similarity transform in normalized coordinates: q = a * p + t (complex numbers: x = real, y = imaginary part)
struct Similarity {
    Complex a = 1, t = 0;
    Complex map(Complex p) const { return a * p + t; }
    bool isIdentity() const { return std::abs(a - 1.0) < 1e-9 && std::abs(t) < 1e-9; }
};

class Estimator {
public:
    // width x height: size of the luma pictures (square pixels); shakiness 1..10 = how far the coarse search looks
    // (2 % .. 20 % of the width per frame), accuracy 1..15 = how many features (more = slower, steadier)
    Estimator(int width, int height, int shakiness, int accuracy);
    ~Estimator();
    // Next frame (8 bit luma, `stride` bytes per row); returns the motion from the previous frame (first: identity)
    Motion push(const uint8_t* luma, int stride);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

enum class Border {
    Auto,     // one zoom for the whole clip, so no border shows
    Adaptive, // zoom follows the needed amount (smoothly)
    Black,    // no zoom, uncovered parts black
    Keep,     // no zoom, uncovered parts keep the previous output picture
};

struct PathParams {
    int smoothing = 15;   // frames on each side of the low-pass; 0 = still camera (tripod)
    double zoom = 0;      // additional zoom in percent (negative = zoom out)
    Border border = Border::Auto;
    double aspect = 16.0 / 9.0; // display aspect ratio (width / height)
};

// Camera path smoothed, per frame the map output position -> position in the input frame (normalized coordinates).
// motions[0] is ignored (the first frame has no previous one).
std::vector<Similarity> corrections(const std::vector<Motion>& motions, const PathParams& p);

// Pixel-space affine map for a picture of w x h pixels with display aspect `aspect`: output pixel center
// (x + 0.5, y + 0.5) -> continuous input position (pixel i covers [i, i + 1)): xin = m[0]x + m[1]y + m[2], yin = m[3]x + m[4]y + m[5]
void pixelMap(const Similarity& s, int w, int h, double aspect, double m[6]);

enum class Outside {
    Clamp, // edge pixels repeated
    Fill,  // fill value
    Keep,  // destination left as it is
};
// Bilinear warps (dst and src the same size, no overlap). Plane: one 8-bit channel; RGBA: 4 bytes per pixel.
// fill: value for Outside::Fill (RGBA: the 4 bytes in memory order as uint32)
void warpPlane(const uint8_t* src, int srcStride, uint8_t* dst, int dstStride, int w, int h, const double m[6],
               Outside outside, uint8_t fill = 0);
void warpRgba(const uint8_t* src, int srcStride, uint8_t* dst, int dstStride, int w, int h, const double m[6],
              Outside outside, uint32_t fill = 0);
void warpRgb(const uint8_t* src, int srcStride, uint8_t* dst, int dstStride, int w, int h, const double m[6],
             Outside outside);

} // namespace StabilizeMotion
