// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/Effects.h"

#include <algorithm>
#include <cmath>

namespace Picture {

// Color correction as a 3x3 matrix (avfilter.colorchannelmixer): white balance like DaVinci Temp/Tint as
// channel gain, normalized to equal brightness (Rec. 709), then saturation (mix with the brightness).
// Temperature +100 = warm (more red, less blue), tint +100 = magenta (less green), saturation -100 = gray.
Matrix colorMatrix(double temp, double tint, double saturation)
{
    const double k = temp / 100.0 * 0.3, n = tint / 100.0 * 0.25;
    double g[3] = {1.0 + k, 1.0 - n, 1.0 - k};
    const double w[3] = {0.2126, 0.7152, 0.0722};
    const double l = w[0] * g[0] + w[1] * g[1] + w[2] * g[2];
    for (double& x : g) x /= l;
    const double s = std::max(0.0, 1.0 + saturation / 100.0);
    Matrix out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.m[i][j] = std::clamp(((i == j ? s : 0.0) + (1.0 - s) * w[j]) * g[j], -2.0, 2.0);
    return out;
}

// Brightness/contrast as tone correction (avfilter.colorlevels): out = 0.5 + (in - 0.5)·c + b, limited to 0..1.
// colorlevels allows input -1..1 and output 0..1 -> cut the line where it reaches 0 resp. 1.
Levels levels(double brightness, double contrast)
{
    const double c = std::max(0.0, 1.0 + contrast / 100.0), b = brightness / 200.0;
    const double d = 0.5 - 0.5 * c + b; // out = c·in + d
    Levels lv{0.0, 1.0, d, c + d};
    if (lv.omin < 0 && c > 0) {
        lv.imin = std::min(-d / c, 1.0);
        lv.omin = 0;
    }
    if (lv.omax > 1 && c > 0) {
        lv.imax = std::max((1.0 - d) / c, -1.0);
        lv.omax = 1;
    }
    if (lv.imax - lv.imin < 1e-4) lv.imin = lv.imax - 1e-4; // completely dark/bright: jump instead of division by 0
    lv.omin = std::clamp(lv.omin, 0.0, 1.0);
    lv.omax = std::clamp(lv.omax, 0.0, 1.0);
    return lv;
}

void applyMatrix(uint8_t* rgba, int width, int height, const Matrix& m)
{
    // Each product rounded on its own and summed (an 8-bit channel mixer works with per-channel tables)
    int lut[3][3][256];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int v = 0; v < 256; ++v) lut[i][j][v] = int(std::lrint(v * m.m[i][j]));
    uint8_t* p = rgba;
    for (size_t n = size_t(width) * height; n; --n, p += 4) {
        const int r = p[0], g = p[1], b = p[2];
        for (int i = 0; i < 3; ++i) p[i] = uint8_t(std::clamp(lut[i][0][r] + lut[i][1][g] + lut[i][2][b], 0, 255));
    }
}

void applyLevels(uint8_t* rgba, int width, int height, const Levels& lv)
{
    uint8_t table[256];
    const double imin = lv.imin * 255, imax = lv.imax * 255, omin = lv.omin * 255, omax = lv.omax * 255;
    const double k = imax - imin != 0 ? (omax - omin) / (imax - imin) : 0;
    for (int v = 0; v < 256; ++v) table[v] = uint8_t(std::clamp(std::lround((v - imin) * k + omin), 0L, 255L));
    uint8_t* p = rgba;
    for (size_t n = size_t(width) * height; n; --n, p += 4)
        for (int i = 0; i < 3; ++i) p[i] = table[p[i]];
}

} // namespace Picture
