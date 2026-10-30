// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A decoded picture (engine/video/Reader) as 8-bit RGBA (straight alpha) at a given size: unscaled with point
// sampling (exact), scaled in one swscale pass with bilinear interpolation, fast = preview conversion (SIMD, ±1 step).

#include "engine/video/Reader.h"

#include <cstdint>

namespace Video {

// out: dw * dh * 4 bytes, rows tightly packed; false = conversion not possible (unsupported format, size change)
bool toRgba(const Picture& pic, const Info& info, uint8_t* out, int dw, int dh, bool fast);

} // namespace Video
