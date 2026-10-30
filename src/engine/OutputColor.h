// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Export: finished RGBA frames into the encoder's picture format (planar/semi-planar Y'CbCr of the output's
// standard and range, or packed RGB), in the render workers, so the encoder thread only encodes.

#include <cstdint>

namespace OutputColor {

// Layout of an encoder picture fromRgba() can write
enum class Layout {
    Yuv420, Yuv422, Yuv444, // planar Y, Cb, Cr
    Nv12,                   // Y + interleaved CbCr, 4:2:0
    Rgb24, Rgba,            // packed
};
struct Target {
    Layout layout = Layout::Yuv420;
    int bits = 8;            // 8, or 10 (16-bit little-endian words, value in the low bits; P010: high bits)
    bool msb = false;        // 10 bit in the high bits of the word (P010)
    bool fullRange = false;  // YUV: 0-255 (JPEG) instead of 16-235/240
    int colorspace = 709;    // matrix: 601 family, 240, 709, 2020
    uint8_t* planes[4] = {};
    int strides[4] = {};
};
// The FFmpeg pixel format (AVPixelFormat number) as a Target layout; false = not one of them (convert another way)
bool targetOf(int avPixelFormat, Target& t);

// Straight-alpha RGBA (display R'G'B', alpha ignored: export frames are opaque) -> the target's layout. Rows
// [rowBegin, rowEnd) only (even begin for 4:2:0) so callers may split a picture over threads; rowEnd < 0 = all.
// Chroma of 4:2:0/4:2:2 is the mean of the 2x2 / 2x1 pixels it covers (centred siting). width/height even for
// subsampled layouts.
void fromRgba(const uint8_t* rgba, int width, int height, int stride, const Target& t, int rowBegin = 0, int rowEnd = -1);

} // namespace OutputColor
