#pragma once
// Own audio engine: the sound of a retimed clip (speed, reverse, speed ramp) from its file stream. Part of
// Audio::Mixer, no GUI or MLT dependency.
//
// Material sample mu (the clip's in/out count in material, like Clip::in) maps to a source position through the
// clip's RetimeMap. Reverse reads the file backwards from its end (material 0 = the file's last sample).
// - Without pitch correction: band-limited interpolation (windowed sinc) at that position, like tape: the pitch follows
//   the speed, with anti-aliasing when faster. Ramps change the speed smoothly, sample by sample.
// - Keep pitch: Rubber Band (R3 engine, real-time mode) stretches the source in playback direction; its start delay is
//   compensated by priming it with the material before the start, so the sound stays in place. Ramps change the
//   stretch ratio block by block.
// A reverse clip at 100 % needs neither (samples copied backwards).
//
// Not thread-safe. read() is meant for increasing positions; a jump starts over (primes again).

#include "core/Types.h"

#include <cstdint>
#include <memory>

namespace Audio {

class Reader;

class Retimer {
public:
    // fileFrames: length of the file in frames of the project rate (reverse mirrors at its end); samplesPerFrame:
    // output rate / project frame rate
    Retimer(std::unique_ptr<Reader> reader, const Clip& clip, int fileFrames, double samplesPerFrame);
    ~Retimer();

    int channels() const;
    // Material samples [mu, mu + count) into out[0..channels())
    void read(int64_t mu, int count, float* const* out);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Audio
