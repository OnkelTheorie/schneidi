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
// Projective map from source pixel coordinates to destination ones (a picture tilted in depth):
// x' = (m[0]*x + m[1]*y + m[2]) / w, y' = (m[3]*x + m[4]*y + m[5]) / w, w = m[6]*x + m[7]*y + m[8]
struct Homography {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};
// The homography taking the source rectangle's corners (0,0) (sw,0) (sw,sh) (0,sh) onto the four destination points
// `to` (x, y pairs in that order); false if they do not make a quad (three on a line, crossed)
bool homography(int sw, int sh, const double to[8], Homography* out);
// The homography taking the four points `from` onto the four points `to` (x, y pairs, same order); false if either
// does not make a quad (three on a line, crossed)
bool homography(const double from[8], const double to[8], Homography* out);
// a then b (b * a)
Homography multiply(const Homography& b, const Homography& a);
// The inverse map; false when singular
bool invert(const Homography& h, Homography* out);
// Where h puts (x, y); false when the point lands at or behind the horizon (w <= 0)
bool mapPoint(const Homography& h, double x, double y, double* X, double* Y);
// The affine placement `m` of a sw x sh source, tilted in depth around the destination point (px, py): tiltX turns
// about the horizontal axis (positive = the top goes away), tiltY about the vertical one (positive = the right side
// goes away), both in degrees; seen by a camera with vertical field of view fov (degrees) centered on the dw x dh
// picture. False when a corner would be behind the camera (nothing to draw).
bool tilted(const Affine& m, int sw, int sh, double px, double py, double tiltX, double tiltY, double fov, int dw,
            int dh, Homography* out);
// draw() through a homography: the same sampling (bilinear, mip steps by the overall shrink), soft one-pixel edge
void draw(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh, const Homography& h, double opacity);
// Source over of two pictures of the same size w x h (RGBA, straight alpha)
void over(uint8_t* dst, const uint8_t* src, int w, int h, double opacity);
// Keep the rectangle (x0, y0) - (x1, y1) (pixels, fractional) of an RGBA picture, the rest becomes color (RGBA,
// straight alpha); a pixel the edge cuts through gets the color by the share outside
void crop(uint8_t* rgba, int w, int h, double x0, double y0, double x1, double y1, const uint8_t color[4]);

} // namespace Compose
