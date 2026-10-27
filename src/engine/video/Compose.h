#pragma once
// Own compositing services instead of MLT's qtblend (Qt module, GPL): the CPU core of laying pictures over each other.
// - transformFilter: draws the clip's picture into a rectangle of a transparent frame of the requested size
//   (position/size keyframed, rotation around the rectangle's center, bilinear sampling). Same properties as
//   qtblend's filter, so the timeline code sets them unchanged: "rect" (profile pixels "x y w h opacity"),
//   "rotation" (degrees, clockwise on screen), "rotate_center" (1 = around the rectangle's center, else its top
//   left), "distort" (1 = stretch onto the rectangle, else keep the picture's aspect ratio inside it).
//   The source is fetched at its media size (meta.media.width/height, the profile size if unknown) like qtblend did,
//   so TimelineBuilderDetail::attachSourceCap keeps working. A rectangle that is the whole frame without rotation
//   passes the picture through at the requested size in the requested format (no RGBA round trip).
// - blendTransition: lays the upper track (b) over the lower one (a). An opaque b that fills the frame is passed
//   through without asking a for its picture (covered tracks are not decoded, tests/test_occlusion); a blank b
//   (gap in the playlist) leaves a alone.
// - cropFilter: keeps the rectangle "rect" (profile pixels, keyframed, scaled to the picture's size like qtcrop did)
//   and paints the rest in "color" (default transparent); edges that cut through a pixel cover it partly.
// Pixels: MLT's 8-bit RGBA with straight alpha; mixed in premultiplied fixed point, rows split over threads.

#include <cstdint>

namespace Mlt {
class Filter;
class Profile;
class Transition;
} // namespace Mlt

namespace Compose {

Mlt::Filter transformFilter(Mlt::Profile& profile);
Mlt::Transition blendTransition(Mlt::Profile& profile);
Mlt::Filter cropFilter(Mlt::Profile& profile);
// With MLT's factory (after Mlt::Factory::init, Bundle::prepareMltCaches): "schneidi_transform", "schneidi_crop",
// "schneidi_blend", and MLT's names qtblend/qtcrop for them where the Qt module is not loaded
void registerServices();

// Image comparison against the old services (tests, bench): true = MLT's qtblend where it loads, else the own ones
void setQtReference(bool on);
bool qtReference();

// Affine map from source pixel coordinates to destination ones: x' = a*x + b*y + c, y' = d*x + e*y + f
struct Affine {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
};

// Source over: src (sw x sh) through m onto dst (dw x dh), both RGBA with straight alpha, opacity 0..1. Bilinear in
// premultiplied alpha; outside the source counts as transparent (soft one-pixel edge).
void draw(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh, const Affine& m, double opacity);
// Source over of two pictures of the same size w x h (RGBA, straight alpha)
void over(uint8_t* dst, const uint8_t* src, int w, int h, double opacity);
// Keep the rectangle (x0, y0) - (x1, y1) (pixels, fractional) of an RGBA picture, the rest becomes color (RGBA,
// straight alpha); a pixel the edge cuts through gets the color by the share outside
void crop(uint8_t* rgba, int w, int h, double x0, double y0, double x1, double y1, const uint8_t color[4]);

} // namespace Compose
