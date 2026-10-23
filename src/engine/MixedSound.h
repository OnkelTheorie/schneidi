#pragma once
// Export with the own audio engine (docs/archiv/plan-0.10-ton.md, step 3): MLT still renders and encodes the picture, the
// sound of every frame comes from Audio::Mixer. A filter on the producer the avformat consumer reads replaces the
// frame's sound with the mix of the frame's time span; the MLT graph itself carries no sound (audio tracks left out),
// so nothing is decoded twice.
//
// Every output frame n gets exactly the samples [start(n), start(n + 1)) of the mix, counted from the export's first
// frame on an exact rational grid: the file has as many samples as the timeline span, and the sound does not drift
// against the picture (also when the frame rate is converted). Integer formats are rounded to the output's bit depth
// (24-bit files get 24 bits; MLT's own path delivered s16 to the consumer).
//
// The sound is fetched in frame order by one consumer thread; a jump (restart) starts the mixer over.

#include "core/ProjectFormat.h"
#include "core/Types.h"
#include "engine/audio/Mixer.h"

#include <cstdint>
#include <memory>

namespace Mlt {
class Filter;
class Profile;
} // namespace Mlt

namespace MixedSound {

struct Output {
    int rate = 48000;
    int channels = 2;       // 1 = L and R mixed, more = L, R and silence
    int bits = 0;           // integer formats: round to this many bits (16, 24, 32); 0 = the full format
    FrameRate fps;          // frame rate of the frames the filter sees (the file's)
    int64_t firstFrame = 0; // timeline frame where output frame 0 begins
    // Frame position (mlt_frame_get_position) of output frame 0: the timeline's tractor counts from the timeline
    // start, also behind an in point; a producer of its own (frame rate converted) from 0
    int64_t firstPosition = 0;
};

// Filter for the producer the export consumer reads
std::unique_ptr<Mlt::Filter> make(Mlt::Profile& profile, const Timeline& timeline, FrameRate timelineFps,
                                  const Audio::Mixer::Options& options, const Output& output);

} // namespace MixedSound
