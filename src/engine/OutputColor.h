#pragma once
// Export: finished frames leave the render workers in the output's color standard.
//
// Sources tagged BT.601 get a profile with their own standard (ProducerFactory, around MLT's filter_swscale bug).
// MLT converts a frame into the format the consumer asks for in the standard of the frame's source profile, so a
// 601 clip that fills the picture (qtblend passes it through) reached the avformat consumer as YUV 601. Its single
// encoder thread then converted 601 -> 709 with a full matrix (swscale off its fast path) for every frame: export
// stayed at ~60 fps (1080p60, NVENC) while the workers waited. This filter on the export output does that matrix in
// the workers, so the encoder thread only repacks.

#include <framework/mlt_types.h>

#include <cstdint>
#include <memory>

namespace Mlt {
class Filter;
class Profile;
} // namespace Mlt

namespace OutputColor {

// Filter for the producer the export consumer reads: YUV frames in another standard are converted to the profile's
std::unique_ptr<Mlt::Filter> make(Mlt::Profile& profile);

// Matrix-only conversion between Y'CbCr standards (MLT colorspace numbers: 601 family, 240, 709, 2020), limited
// range, like MLT's consumer does it. Formats: yuv422 (packed), yuv420p, yuv420p10, yuv444p10; even width and height.
// src and dst are whole images of that format. false = not handled (nothing written).
bool convert(const uint8_t* src, uint8_t* dst, mlt_image_format format, int width, int height, int fromColorspace,
             int toColorspace);

} // namespace OutputColor
