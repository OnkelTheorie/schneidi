// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Applying a stabilization result (effect "stabilize", engine/Stabilizer) without MLT: the correction of a clip and
// the pure RGBA warp. The MLT filter (Stabilizer::attach) and the own picture engine (Picture::Renderer) use both.

#include "core/ProjectFormat.h"
#include "core/Types.h"
#include "engine/StabilizeMotion.h"

#include <memory>
#include <vector>

namespace StabilizeCorrection {

// Per analyzed frame the map output -> input (smoothing, zoom, border applied)
struct Correction {
    std::vector<StabilizeMotion::Similarity> corrections; // frame `from` first
    int from = 0;
    double aspect = 16.0 / 9.0; // display aspect ratio of the pictures it applies to
    StabilizeMotion::Border border = StabilizeMotion::Border::Auto;
};

// The correction of clip c in a project of frame rate `rate` and display aspect `aspect` (effect switched on, result
// there); nullptr otherwise. Cached (thread-safe): cheap to ask every frame; a missing result is looked for again
// after a second.
std::shared_ptr<const Correction> of(const Clip& c, FrameRate rate, double aspect);

// Pure warp of one RGBA picture (w x h, display aspect c.aspect) for producer frame `pos` (the clip's material frame:
// in + clip frame) into dst; false = nothing to do there (dst untouched). Border "keep" takes the uncovered parts from
// `previous` (the last output picture), without it the edge pixels.
bool warpRgba(const Correction& c, int pos, const uint8_t* src, uint8_t* dst, int w, int h,
              const uint8_t* previous = nullptr);

} // namespace StabilizeCorrection
