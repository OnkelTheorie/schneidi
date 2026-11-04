// SPDX-License-Identifier: AGPL-3.0-only AND MIT
#pragma once
// Riser, click and hit sounds (core/SfxSynth): C++ ports of earshot's forge recipes (Leon Lin, MIT; licence in
// third_party/earshot/LICENSE and at the top of SfxForge.cpp): darkriser (a dark noise build with a sub and an
// accelerating stutter gate, no rising tone), shutter (a two-stage mechanical click), knock (a struck block) and
// subdrop (a sub falling under a click, the impact), each with parameter sets of earshot's banks. Mono, deterministic per seed.

#include <vector>

namespace SfxForge {

enum class Riser { Dark, Snatch, Build };
enum class Click { Snap, Mirror, Dslr };
enum class Knock { Snap, Wood };
enum class Drop { Soft, Bright, Quick };

struct Result {
    std::vector<float> samples; // 48 kHz mono, peak 1
    double anchor = 0;          // seconds: the riser's peak / the hit's main contact
};

double riserLength(Riser m); // its own build (seconds to the peak)
// length: seconds to the peak (0 = its own)
Result riser(Riser m, unsigned seed, double length = 0);
Result click(Click m, unsigned seed);
Result knock(Knock m, unsigned seed);
// A designed sub drop (a low sine falling under a click): the impact / boom
Result drop(Drop m, unsigned seed);

} // namespace SfxForge
