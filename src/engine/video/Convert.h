// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A decoded picture (engine/video/Reader) as 8-bit RGBA (straight alpha) at a given size, with the same swscale calls
// the MLT producer around the reader makes (engine/video/Producer) – the own picture engine (engine/picture) gets the
// same pixels as the MLT path. Unscaled: MLT's convert_image; scaled: one swscale pass with bilinear interpolation
// (MLT's swscale filter), fast = preview conversion (SIMD, ±1 step). No MLT in this header.
// The implementation still lives in Producer.cpp next to the helpers it shares; it moves here with the MLT removal.

#include "engine/video/Reader.h"

#include <cstdint>

namespace Video {

// out: dw * dh * 4 bytes, rows tightly packed; false = conversion not possible (unsupported format, size change)
bool toRgba(const Picture& pic, const Info& info, uint8_t* out, int dw, int dh, bool fast);

} // namespace Video
