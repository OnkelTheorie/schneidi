// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// schneidi's own picture effects: Green Screen key, glow, halftone, noise, scan lines, broken sync, gradient and alpha
// operations. They replace the frei0r plugins of the same name (GPL, not bundled any more) and keep their effect
// ids and parameters ("frei0r.glow", key "0" …), so old projects and Kdenlive/Shotcut presets map onto them unchanged;
// the descriptors live in core/EffectRegistry. Written from scratch (no frei0r code).
// Each effect is a pure function on an RGBA image (8 bit straight alpha, rows without gaps), applied by the picture
// engine (engine/picture) by effect id. Thread-safe: frames may render in parallel (export).

#include <QColor>
#include <QString>
#include <cstdint>
#include <functional>

namespace VideoEffects {

// ---- Green Screen ("chromakey") ----
struct KeyParams {
    QColor color{0, 255, 0}; // key color (picked with the eyedropper)
    double tolerance = 0.3;  // 0..1: how far from the key color still counts as background (chroma distance)
    double softness = 0.2;   // 0..1: width of the soft edge between keyed and kept
    double spill = 1.0;      // 0..1: how much of the key color's tint is taken out of what stays near the key
                             // (green fringe, green light on the subject; colors far from the key keep theirs)
};
// Distance of a color from the key in the chroma plane (YCbCr, brightness left out), in units of the key's own
// chroma: 0 = key color, ~0.6 = gray/white/black, skin ~0.75. Darker or brighter shades of the key count as close.
double keyDistance(const QColor& key, const QColor& pixel);
void key(uint8_t* rgba, int width, int height, const KeyParams& p);

// ---- frei0r replacements (parameters 0..1 as in the frei0r plugins) ----
// Glow: the picture blurred and laid over itself with "screen" (blur 0..1 = up to 1/20 of the picture height);
// strength 0..1 mixes the result with the picture (1 = the frei0r plugin's full screen, 0 = no glow); threshold 0..1:
// only what is brighter glows (0 = the whole picture, as the plugin; higher = no haze over dark parts)
void glow(uint8_t* rgba, int width, int height, double blur, double strength = 1.0, double threshold = 0.0);
// Color halftone: per channel dots of its ink (cyan for red …) on rotated grids, black in that channel, the picture
// between them (dot 0..1 = cell size, angles 0..1 = 0..360°)
void halftone(uint8_t* rgba, int width, int height, double dot, double cyan, double magenta, double yellow);
// Noise per channel (amount 0..1), different on every frame (seed)
void noise(uint8_t* rgba, int width, int height, double amount, uint32_t seed);
// Every other line at half brightness, like an interlaced TV picture
void scanlines(uint8_t* rgba, int width, int height);
// Broken sync: the picture rolled up by offset 0..1 of its height, the top coming in again from the bottom
void noSync(uint8_t* rgba, int width, int height, double offset);

struct GradientParams {
    bool radial = false;
    QColor startColor{0, 0, 0}, endColor{255, 255, 255};
    double startOpacity = 0.5, endOpacity = 0.5;
    double startX = 0.5, startY = 0, endX = 0.5, endY = 1; // 0..1 of the picture size
    double offset = 0; // where the first color starts on the line (0..1)
    QString blend = "normal"; // normal, add, saturate, multiply, screen, overlay, darken, lighten, colordodge,
                              // colorburn, hardlight, softlight, difference, exclusion, hslhue, hslsaturation,
                              // hslcolor, hslluminosity
};
// A two-color gradient laid over the picture with a blend mode. As in the frei0r plugin, the end color/opacity sit at
// the start point and the start color/opacity at the end point (old projects look the same)
void gradient(uint8_t* rgba, int width, int height, const GradientParams& p);

struct AlphaParams {
    double display = 0;   // 0 image, 0.21 alpha as gray, 0.36 gray + red, 0.5/0.64/0.79/1 selection on black/gray/white/checkers
    bool inputAlpha = false; // show the alpha as it came in (before the operation)
    double operation = 0; // 0 none, 0.2 shave, 0.3 shrink hard, 0.4 shrink soft, 0.6 grow hard, 0.7 grow soft, 0.8 threshold, 1 blur
    double threshold = 0.5;
    double amount = 0.333; // shrink/grow/blur amount 0..1
    bool invert = false;
};
// Operations on the alpha channel (shrink, grow, soften the key's edge) and ways to look at it
void alphaOps(uint8_t* rgba, int width, int height, const AlphaParams& p);

// ---- By effect id ----
// Parameter values of an own effect by property name ("0", "softness", … as in EffectRegistry's mltProperty, the
// key old projects and presets use): the picture engine reads the clip's values. has() false = default.
struct Params {
    std::function<bool(const char*)> has;
    std::function<double(const char*)> num;
    std::function<QColor(const char*)> color;
    std::function<QString(const char*)> text;
};
// Applies the own effect effectId (isOwn) to an RGBA picture; frameNo seeds per-frame patterns (noise). false = not own.
bool apply(const QString& effectId, uint8_t* rgba, int width, int height, const Params& params, int64_t frameNo);
// Gaussian blur (three box blurs) of an RGBA picture with straight alpha, sigma in pixels
void gaussian(uint8_t* rgba, int width, int height, double sigma);
// The same blur on one 16-bit plane (e.g. a premultiplied alpha 0..65535: gaussian's values for that channel)
void gaussianPlane(uint16_t* plane, int width, int height, double sigma);

// Is the effect one of these (by effect id, e.g. "chromakey", "frei0r.glow")?
bool isOwn(const QString& effectId);

// ---- Keyer and matte operations of the compositing nodes (0.19, engine/picture/CompNodesKey.cpp) ----
// Luma key: what is as bright as low..high (Rec. 709 luma, 0..1) stays, the rest becomes transparent; soft = width of
// the ramp outside the range (0 = hard); invert = the range becomes transparent. The color stays as it is
struct LumaKeyParams {
    double low = 0.1, high = 1.0, soft = 0.1;
    bool invert = false;
};
void lumaKey(uint8_t* rgba, int width, int height, const LumaKeyParams& p);
// Row pass of a square minimum (grow = false: the matte shrinks) or maximum (grow) of radius r pixels on a band of
// rows of one 8-bit plane, as alphaOps' shrink/grow (the column pass is the same on the transposed plane, so the
// caller can spread both over threads)
void matteMinMaxRows(uint8_t* plane, int width, int rows, int radius, bool grow);
// Gaussian blur of one 8-bit plane in bands for threads, the same bytes as alphaOps' blur: for each of the three
// radii (false = sigma below half a pixel, no blur) the box along the rows on bands of rows (src -> dst), then the box
// along the columns on bands of columns x0..x1 of the whole height (dst -> src)
bool matteBlurRadii(double sigma, int radii[3]);
void matteBoxRows(const uint8_t* src, uint8_t* dst, int width, int rows, int radius);
void matteBoxCols(const uint8_t* src, uint8_t* dst, int width, int height, int x0, int x1, int radius);
// Takes the key color's tint out of every color (amount 0..1; the share along the key's hue in the chroma plane,
// brightness kept): a green fringe turns neutral, colors without green in them stay. Nothing for a gray key
void despill(uint8_t* rgba, int width, int height, const QColor& key, double amount);

// ---- For the GPU path of the compositing nodes (0.19 package F, engine/picture/gpu/CompGpu.cpp) ----
// The chroma key's table as key() uses it: keyTableSide x keyTableSide entries of 4 bytes (alpha, then the color
// change in steps of 2 levels as int8 r, g, b), row = chroma Cb + 128, column = chroma Cr + 128 of keyChroma(); false =
// a gray key (key() measures the distance in RGB there, no table)
constexpr int keyTableSide = 257;
bool keyTable(const KeyParams& p, uint8_t* out);
// The luma key's alpha per Rec. 709 luma level (256 bytes), as lumaKey() uses it
void lumaKeyTable(const LumaKeyParams& p, uint8_t out[256]);

} // namespace VideoEffects
