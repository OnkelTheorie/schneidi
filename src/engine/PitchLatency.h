#pragma once

#include <mlt++/Mlt.h>

// MLT audio path only (fallback, Audio::ownPath() false; the own engine stretches with engine/audio/Stretch):
// delay of the audio caused by "keep pitch" (MLT timewarp with warp_pitch -> MLT's Rubberband filter rbpitch, only if
// that module is installed - the bundled builds leave it out, then there is no pitch correction and no delay). MLT does not
// compensate for it: depending on the speed the audio runs 2–10 frames behind the video. It is fixed per speed/sample rate (also after
// seeks), but depends on the Rubberband version -> measure once at runtime instead of a table.
// Compensation: fetch the audio that many source frames earlier from the timewarp producer (cut-in + frames()).
namespace PitchLatency {

// Delay in timeline frames at speed |speed| (1 or without pitch: 0). First time ~50 ms (measurement with a
// click in a temporary WAV, with and without pitch), afterwards from the cache. Thread-safe.
int frames(Mlt::Profile& profile, double speed, int sampleRate = 48000);

// Extend the timewarp producer by `lead` frames so that cuts with lead-in reach to the end of the file (otherwise MLT trims
// the cut). Reading there goes past the end of the file; it would only be audible after the clip end – so never. Only "length",
// not "out": backwards timewarp computes from "out".
void extend(Mlt::Producer& p, int lead);

// Sample rate of the first audio stream of a producer (for frames()), 48000 if unknown
int sampleRateOf(Mlt::Producer& p);

} // namespace PitchLatency
