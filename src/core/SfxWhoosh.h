// SPDX-License-Identifier: AGPL-3.0-only AND MIT
#pragma once
// Whoosh and swish sounds (core/SfxSynth): a C++ port of the whoosh "pass engine" of earshot (Leon Lin, MIT;
// earshot/forge/recipes/whoosh.py `render`, licence in third_party/earshot/LICENSE and at the top of SfxWhoosh.cpp)
// with six of its delivered forge_v2 parameter sets. Mono (the original's stereo image is left out; its mono sum is
// the same), deterministic per seed (own noise, not numpy's: the same recipe, other noise than earshot's files).

#include <vector>

namespace SfxWhoosh {

enum class Member { Windy, Long, Glide, Flick, Cloth, Paper };

struct Result {
    std::vector<float> samples; // 48 kHz mono, peak 1
    double anchor = 0;          // seconds: the pass (the level curve's peak)
};

// The member's own natural length (seconds, as the bank renders it)
double naturalLength(Member m);
// stretch: time scale of the motion (1 = as designed; 2 = a pass twice as slow)
Result render(Member m, unsigned seed, double stretch = 1.0);

} // namespace SfxWhoosh
