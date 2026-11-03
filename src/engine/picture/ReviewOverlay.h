// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The marks of a review render (core/ReviewSheet) burned into each frame: a separate drawing step after the frame is
// composited (Picture::Options::overlay), on the CPU with QPainter. Only the review render sets it; a normal export
// never has an overlay, and the timeline/project are not touched.

#include "core/ReviewSheet.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace Picture {

// For Options::overlay / ExportSettings::overlay: draws ReviewSheet::drawOverlay() of the frame onto its RGBA picture
std::function<void(uint8_t* rgba, int width, int height, int64_t frame)>
reviewOverlay(std::shared_ptr<const ReviewSheet::Data> data);

} // namespace Picture
