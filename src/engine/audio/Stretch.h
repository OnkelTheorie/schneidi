#pragma once
// Own audio engine: changes the speed of a sound while keeping its pitch (phase vocoder). Used by Audio::Retimer
// for "keep pitch"; no GUI, MLT or third-party stretcher.
//
// Pull design on a fixed grid: the output is cut into frames at output samples k * hop (Hann window of 4 hops,
// overlap-add). Frame k takes its sound from the source window centred on map(k * hop), so the time course is exactly
// the caller's map - any speed, speed ramps included, no drift, no start delay to compensate: the window is read
// around the mapped position, the source is random access. The same output sample reads the same however the output
// is split into blocks.
//
// Phases (the part that makes it sound right):
// - Instantaneous frequency per bin from a second analysis one hop before the frame (independent of the speed, so
//   it works for 1 % as for 100x).
// - Identity phase locking (Laroche & Dolson 1999): peaks of the spectrum are propagated by their frequency, the bins
//   around a peak keep their phase relative to it, so partials stay one coherent wave instead of a smeared one.
//   The rotation of a peak follows the peak of the frame before (light peak tracking for glides and vibrato).
// - Transients: onsets (many bins rising by more than 3 dB within a hop) reset the phases of the rising regions
//   at the frame where the onset is strongest, so drums and clicks keep their attack; sustained parts go on.
// - Channels: one rotation per region for all channels (taken from the channel loudest at the peak), so the phase
//   and level differences between the channels stay as in the source (stereo image, no phasing between L and R).
//
// Not thread-safe.

#include <cstdint>
#include <functional>
#include <memory>

namespace Audio {

class Stretch {
public:
    // Source samples [from, from + count) per channel (planar, playback direction); positions outside the material
    // must be filled too (silence)
    using Source = std::function<void(int64_t from, int count, float* const* out)>;
    // Source position (samples) of output sample t; increasing (speed > 0)
    using Map = std::function<double(double t)>;

    struct Options {
        int window = 0; // FFT size; 0 = by sample rate (2048 at 44.1/48 kHz)
    };

    Stretch(int channels, int sampleRate, Source source, Map map, const Options& options);
    Stretch(int channels, int sampleRate, Source source, Map map)
        : Stretch(channels, sampleRate, std::move(source), std::move(map), Options{})
    {
    }
    ~Stretch();

    int channels() const;
    int window() const;
    int hop() const;

    // Output samples [t, t + count) into out[0..channels()). Reading on where the last read ended continues;
    // any other position starts over (primed with the frames before t, so the start sounds like the middle).
    void read(int64_t t, int count, float* const* out);

    // Onsets that reset phases so far (tests, bench)
    int transients() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Audio
