// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Rasterizing mask shapes (core/Masks) into one 8-bit plane (255 = inside), the pixels of the compositing node
// "mask". Only the shape's bounds are touched; the edge is anti-aliased by a soft falloff of at least one pixel
// outside the shape (smoothstep over the distance to the outline, the same edge as the Color page windows:
// rect/ellipse ARE GradeWindows::Mask). Rows are spread over threads (Compose::forRows) when asked.

#include "core/Masks.h"

#include <QRect>

#include <cstdint>

namespace MaskRaster {

// Draws g (without its invert) into plane (w x h bytes, rows without gaps) which must be 0 inside the returned
// rectangle; returns the pixels written (empty = nothing). threads = rows on several threads
QRect render(const Masks::Geometry& g, int w, int h, uint8_t* plane, bool threads = true);
// 255 - v over the whole plane
void invert(uint8_t* plane, int w, int h);
// dst = dst combined with src (both w x h); srcArea / dstArea = where they can be > 0 (empty = nothing). Returns
// where the result can be > 0
QRect combine(uint8_t* dst, QRect dstArea, const uint8_t* src, QRect srcArea, int w, int h, Masks::Combine mode);

} // namespace MaskRaster
