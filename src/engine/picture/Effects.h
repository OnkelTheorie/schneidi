// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The simple colour correction (effect "color") as pure RGBA functions: white balance and saturation as a 3x3 matrix,
// brightness/contrast as tone levels. The MLT path hands the same numbers to avfilter.colorchannelmixer/colorlevels
// (TimelineBuilderClip), the own picture engine applies them itself.

#include <cstdint>

namespace Picture {

// White balance like Temp/Tint as channel gain normalized to equal brightness (Rec. 709), then saturation (mix with
// the brightness). temp +100 = warm, tint +100 = magenta, saturation -100 = gray. out[i] = sum_j m[i][j] * in[j].
struct Matrix {
    double m[3][3];
};
Matrix colorMatrix(double temp, double tint, double saturation);
// out = 0.5 + (in - 0.5) * contrast + brightness as levels: input imin..imax -> output omin..omax (0..1, clamped)
struct Levels {
    double imin, imax, omin, omax;
};
Levels levels(double brightness, double contrast);

void applyMatrix(uint8_t* rgba, int width, int height, const Matrix& m);
void applyLevels(uint8_t* rgba, int width, int height, const Levels& lv);

} // namespace Picture
