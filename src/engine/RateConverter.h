// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Export with a frame rate other than the timeline's: plays the timeline (built in its own profile) in the output's
// profile. Port of MLT's "consumer" producer (modules/core/producer_consumer.c) that takes the tractor directly
// instead of loading a file. The old way (timeline written as XML, loaded via "consumer:<file>") lost every filter made
// with mlt_filter_new (color grade, volume envelopes, click ramps, denoise …): they have no service name, so the XML
// cannot recreate them.
//
// Output frame n shows the timeline frame round(n * timeline fps / output fps); the sound of each timeline frame is
// delivered once, with the sample count of the slower rate (repeated frames carry none), so it stays gapless.

#include <memory>

namespace Mlt {
class Producer;
class Profile;
} // namespace Mlt

namespace RateConverter {

// Producer in outProfile over `timeline` (a producer in timelineProfile, its in point = output frame 0). Holds its
// own references to both; nullptr if MLT could not create it.
std::unique_ptr<Mlt::Producer> make(Mlt::Profile& outProfile, Mlt::Profile& timelineProfile, Mlt::Producer& timeline);

} // namespace RateConverter
