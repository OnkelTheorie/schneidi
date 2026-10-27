#pragma once
// schneidi's own picture effects: Green Screen key, glow, halftone, noise, scan lines, broken sync, gradient and alpha
// operations. They replace the frei0r plugins of the same name (GPL, not bundled any more) and keep their effect
// ids and parameters ("frei0r.glow", key "0" …), so old projects and Kdenlive/Shotcut presets map onto them unchanged;
// the descriptors live in core/EffectRegistry. Written from scratch (no frei0r code).
// Each effect is a pure function on an RGBA image (8 bit straight alpha, rows without gaps; testable without MLT)
// plus one own MLT filter made like engine/ColorGrade (mlt_filter_new + process): the TimelineBuilder sets the
// parameters as properties (keyframes via anim_set), the filter reads them per frame, so a live parameter change
// (engine/LiveUpdate) needs no rebuild. Thread-safe: frames may render in parallel (export).

#include <QColor>
#include <QString>
#include <cstdint>

#include <framework/mlt_types.h>

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
// Glow: the picture blurred and laid over itself with "screen" (blur 0..1 = up to 1/20 of the picture height)
void glow(uint8_t* rgba, int width, int height, double blur);
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

// ---- MLT ----
// Is the effect one of these (by effect id, e.g. "chromakey", "frei0r.glow")?
bool isOwn(const QString& effectId);
// New filter for the effect (nullptr = not an own effect). The parameters are set as properties named like the
// effect's mltProperty ("0", "1" …); numbers may be animated (anim_set, positions from the filter's in point).
mlt_filter create(const QString& effectId);

} // namespace VideoEffects
