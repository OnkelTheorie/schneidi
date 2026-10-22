#pragma once
// Own audio engine (docs/plan-0.10-ton.md), second block: mixes the sound of a timeline into samples. The same code
// serves the export, the analyses and later the live output; it has no GUI or MLT dependency.
//
// Time is counted in samples of the output rate. Timeline frame f starts at sample frameStart(f); cuts, fades and
// keyframes stay on whole frames, but gain curves run sample by sample between the frame boundaries (a fade reaches
// 0 exactly at the clip edge), sub-frame offsets shift a clip by whole samples, and every source is read
// sample-exact by Audio::Reader. Signal path per clip: source (file stream or the mix of a compound/multicam
// sequence) -> noise reduction -> volume, pan, fades, transitions -> declick at hard cuts -> sub-frame offset;
// per track: EQ -> compressor -> fader/pan, solo/mute; master: fader -> limiter. Latency of noise reduction and
// limiter is compensated (the sound stays where it is in the timeline).
//
// Not thread-safe: one mixer per user. render() is meant to be called with increasing positions; any jump starts the
// state (filters, readers) over at the new position.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace Audio {

class Mixer {
public:
    static constexpr int kChannels = 2; // the mix is stereo; sources are mixed down or spread on reading

    struct Options {
        int rate = 48000;
        // Length of a media file in frames of the project rate (the length the project knows, needed for
        // transition handles and reversed clips); unset or <= 0 = taken from the file
        std::function<int(const QString& path)> fileLength;
        // Behave like the MLT audio path, only to compare with references rendered through it (goes with that path):
        // gain and pan curves reach a frame's value at its end and start constant in every excerpt, sub-frame clips
        // are cut at the clip end, latency is not compensated, frame boundaries use MLT's float frame rate.
        bool mltCompat = false;
    };

    Mixer(const Timeline& timeline, FrameRate fps, const Options& options);
    Mixer(const Timeline& timeline, FrameRate fps) : Mixer(timeline, fps, Options{}) {}
    ~Mixer();

    int rate() const;
    // Samples up to the end of the timeline's content
    int64_t length() const;
    // First sample of timeline frame f, and the frame a sample belongs to
    int64_t frameStart(int64_t frame) const;
    int64_t frameAt(int64_t sample) const;

    // Mixes [pos, pos + count) into out[0..kChannels) (planar). Positions outside the timeline give silence.
    void render(int64_t pos, int count, float* const* out);

    // Counters for tests and the log
    struct Stats {
        int voices = 0;      // clip sounds in the plan (all levels of nesting)
        int readersOpened = 0;
        int missing = 0;     // sources that could not be opened (silent)
        int restarts = 0;    // render() calls that did not continue the previous one
    };
    const Stats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Audio
