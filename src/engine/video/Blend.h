// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Blend modes of two colors (formulas of the PDF / W3C compositing specification), shared by the gradient effect
// (VideoEffects::gradient) and the compositing node "merge". Channels as 0..1 floats; a = backdrop (the picture
// below), b = source (the color laid on it).

#include <algorithm>
#include <cmath>
#include <iterator>
#include <QString>

namespace Blend {

// Mode ids by index (the order is stored nowhere, only the names are)
inline constexpr const char* kModes[] = {"normal",     "add",       "saturate",  "multiply",   "screen",     "overlay",
                                         "darken",     "lighten",   "colordodge", "colorburn", "hardlight",  "softlight",
                                         "difference", "exclusion", "hslhue",    "hslsaturation", "hslcolor", "hslluminosity"};
inline constexpr int kModeCount = int(std::size(kModes));
enum Mode {
    Normal = 0, Add, Saturate, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn, HardLight, SoftLight,
    Difference, Exclusion, HslHue, HslSaturation, HslColor, HslLuminosity
};

// Index of a mode name; unknown or empty = normal
inline int modeIndex(const QString& name)
{
    for (int i = 0; i < kModeCount; ++i)
        if (name == QLatin1String(kModes[i])) return i;
    return Normal;
}

// One channel of a separable mode (everything but saturate and the HSL modes)
inline float channel(int mode, float a, float b)
{
    switch (mode) {
    case Add: return std::min(1.0f, a + b);
    case Multiply: return a * b;
    case Screen: return a + b - a * b;
    case Overlay: return a <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);
    case Darken: return std::min(a, b);
    case Lighten: return std::max(a, b);
    case ColorDodge: return a <= 0 ? 0 : b >= 1 ? 1 : std::min(1.0f, a / (1 - b));
    case ColorBurn: return a >= 1 ? 1 : b <= 0 ? 0 : 1 - std::min(1.0f, (1 - a) / b);
    case HardLight: return b <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);
    case SoftLight: {
        if (b <= 0.5f) return a - (1 - 2 * b) * a * (1 - a);
        const float d = a <= 0.25f ? ((16 * a - 12) * a + 4) * a : std::sqrt(a);
        return a + (2 * b - 1) * (d - a);
    }
    case Difference: return std::abs(a - b);
    case Exclusion: return a + b - 2 * a * b;
    default: return b; // normal
    }
}

inline float lum(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
inline void clipColor(float c[3])
{
    const float l = lum(c), n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
    for (int i = 0; i < 3; ++i) {
        if (n < 0) c[i] = l + (c[i] - l) * l / std::max(1e-6f, l - n);
        if (x > 1) c[i] = l + (c[i] - l) * (1 - l) / std::max(1e-6f, x - l);
    }
}
inline void setLum(float c[3], float l)
{
    const float d = l - lum(c);
    for (int i = 0; i < 3; ++i) c[i] += d;
    clipColor(c);
}
inline float sat(const float c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }
inline void setSat(float c[3], float s)
{
    int mx = 0, mn = 0;
    for (int i = 1; i < 3; ++i) {
        if (c[i] > c[mx]) mx = i;
        if (c[i] < c[mn]) mn = i;
    }
    if (mx == mn) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    const int mid = 3 - mx - mn;
    c[mid] = (c[mid] - c[mn]) * s / (c[mx] - c[mn]);
    c[mx] = s;
    c[mn] = 0;
}

// The blended color B(a, b) of every mode but saturate (which depends on alpha: see VideoEffects::gradient)
inline void pixel(int mode, const float a[3], const float b[3], float out[3])
{
    if (mode >= HslHue) { // non-separable (HSL) modes
        float bb[3] = {b[0], b[1], b[2]};
        float c[3] = {a[0], a[1], a[2]};
        if (mode == HslHue) { // hue of b, saturation and brightness of a
            setSat(bb, sat(a));
            setLum(bb, lum(a));
            std::copy(bb, bb + 3, out);
        } else if (mode == HslSaturation) {
            setSat(c, sat(bb));
            setLum(c, lum(a));
            std::copy(c, c + 3, out);
        } else if (mode == HslColor) {
            setLum(bb, lum(a));
            std::copy(bb, bb + 3, out);
        } else {
            setLum(c, lum(bb));
            std::copy(c, c + 3, out);
        }
        return;
    }
    for (int c = 0; c < 3; ++c) out[c] = channel(mode, a[c], b[c]);
}

} // namespace Blend
