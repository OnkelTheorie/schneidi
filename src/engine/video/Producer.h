#pragma once
// MLT producer around the own video decoder (engine/video/Reader, plan 0.11 point 7): stands in for MLT's avformat
// producer for the picture of a media file. Same properties (width, height, aspect_ratio, video_index,
// meta.media.*), same frame positions, same image conversion (pixel formats, color range, colorspace tag) and the
// same normalizing filters the loader attaches (deinterlace, crop, swscale, resize, avcolor_space) -> the graph
// above sees no difference, only the pictures arrive sooner. No sound (video tracks never take it from here).
//
// Used for files with a video stream and a known length (not still images, image sequences, cover art or streams).
// SCHNEIDI_DECODER=mlt switches back to MLT's producer (comparison, fallback).

#include <QString>

#include <memory>

namespace Mlt {
class Producer;
class Profile;
} // namespace Mlt

namespace Video {

struct ProducerOptions {
    int threads = 0;       // decoder threads (MLT "threads"; 0 = FFmpeg's choice)
    int parallelFrames = 1; // frames rendered at once (export workers): kept decoded so none waits for a seek
    // Preview: YUV -> RGB without swscale's accurate rounding (±1 step), so its SIMD paths run (NEON, x86). With
    // accurate rounding swscale stays in C code: a Raspberry Pi showed 1080p at 1 frame/s. Export stays as MLT.
    // SCHNEIDI_EXACT_PREVIEW=1 switches it off (comparisons).
    bool fastConvert = false;
};

// nullptr if the own decoder does not take this file (MLT opens it then)
std::unique_ptr<Mlt::Producer> openProducer(Mlt::Profile& profile, const QString& path, const ProducerOptions& options);

// Own decoder switched on (SCHNEIDI_DECODER != "mlt", else the setting "video/decoder" != "mlt"); read once per run
bool ownDecoder();
// The setting (compatibility mode in the Playback menu): takes effect after a restart
void setOwnDecoder(bool own);

} // namespace Video
