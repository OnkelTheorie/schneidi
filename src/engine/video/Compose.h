// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The CPU core of laying pictures over each other (engine/picture): drawing a picture into a rectangle (placement,
// bilinear sampling), laying one picture over another, cropping. Pixels: 8-bit RGBA with straight alpha; mixed in
// premultiplied fixed point, rows split over threads.

#include <cstdint>
#include <functional>

namespace Compose {

// fn(first, last) for row ranges covering [0, rows), spread over a pool of threads whose jobs never wait (safe from
// any thread); minRows per job keeps small pictures on one thread. The calling thread takes the first share.
void forRows(int rows, int minRows, const std::function<void(int, int)>& fn);

// Affine map from source pixel coordinates to destination ones: x' = a*x + b*y + c, y' = d*x + e*y + f
struct Affine {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
};

// Where the transform filter puts a source of sw x sh: the rectangle (x, y, w, h) in pixels of the picture drawn on,
// rotated clockwise by angle degrees around its center (rotateCenter) or top left; distort = stretched onto it, else
// fitted keeping the aspect ratio
Affine placement(double x, double y, double w, double h, double angle, int sw, int sh, bool distort, bool rotateCenter);

// Source over: src (sw x sh) through m onto dst (dw x dh), both RGBA with straight alpha, opacity 0..1. Bilinear in
// premultiplied alpha, shrinking by 2:1 or more averages 2x2 blocks of texels first (mip steps) (no skipped texels: fine
// patterns do not flicker); outside the source counts as transparent (soft one-pixel edge).
void draw(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh, const Affine& m, double opacity);
// Source over of two pictures of the same size w x h (RGBA, straight alpha)
void over(uint8_t* dst, const uint8_t* src, int w, int h, double opacity);
// Keep the rectangle (x0, y0) - (x1, y1) (pixels, fractional) of an RGBA picture, the rest becomes color (RGBA,
// straight alpha); a pixel the edge cuts through gets the color by the share outside
void crop(uint8_t* rgba, int w, int h, double x0, double y0, double x1, double y1, const uint8_t color[4]);

} // namespace Compose
