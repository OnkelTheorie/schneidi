// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Export renders several frames at once (MLT worker threads, real_time < -1). They reach a clip's decoder (MLT's
// avformat producer, one lock per file) in random order: when frame N+1 gets the lock before N, the producer decodes
// past N, and N then costs a seek back to the last keyframe and decoding the whole stretch again. As long as the encoder
// was the bottleneck (MLT's avformat consumer) only one worker ran at a time; with the own export path the workers
// race and a 1080p60 export ran at half speed with 3.5 times the CPU (GOP 60).
//
// This filter on a video producer numbers the frames in the order MLT creates them (one thread) and lets a frame
// decode only when every earlier frame of the producer is decoded or released. A frame that is never decoded (track
// covered, qtblend skips it) counts as done when it is released, so it holds up the next ones only until then. Waiting
// is capped (1 s, a warning), so even an unforeseen case cannot deadlock.
//
// Only for MLT's avformat producers (SCHNEIDI_DECODER=mlt, files the own decoder leaves to MLT): the own decoder
// (engine/video) decodes in order by design and keeps neighboring frames for the workers.

namespace Mlt {
class Producer;
}

namespace DecodeOrder {

void attach(Mlt::Producer& producer);

} // namespace DecodeOrder
