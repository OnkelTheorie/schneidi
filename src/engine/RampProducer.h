#pragma once

#include "core/Types.h"

#include <mlt++/Mlt.h>
#include <memory>

// Speed Ramp (Clip::ramp) in the engine. Positions of both producers = source frames of the clip (as with timewarp),
// i.e. Clip::in/out and keyframe filters work unchanged.
namespace RampProducer {

// Video: own producer that fetches the source position from the RetimeMap per frame (one decoder for all speeds,
// smooth transitions frame by frame). `inner` = normal producer of the file in the project profile.
std::unique_ptr<Mlt::Producer> video(Mlt::Profile& profile, std::unique_ptr<Mlt::Producer> inner, const Clip& c);

// Audio: playlist of timewarp excerpts, constant speed per section; smooth transitions in short steps.
// Keep pitch (warp_pitch) only in sections with constant speed: the pitch filter needs 1–2 frames to settle after every change,
// in the short steps the audio would otherwise be full of gaps. 100 % forward = normal
// producer (no settling). nullptr = file not readable.
std::unique_ptr<Mlt::Producer> audio(Mlt::Profile& profile, const QString& file, const Clip& c);

// Audio steps (also for tests): source [m0, m1) at constant speed from source position s0
struct AudioStep {
    int m0, m1;
    double s0, speed;
    bool smooth = false; // step of a smooth transition (without pitch correction)
};
QVector<AudioStep> audioSteps(const Clip& c, int fileLength);

} // namespace RampProducer
