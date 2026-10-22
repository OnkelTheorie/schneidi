#pragma once
// Preview with the own audio engine (docs/plan-0.10-ton.md, step 5): picture follows sound. An MLT consumer that
// only shows pictures – MLT renders them ahead in its read-ahead thread (real_time 1, audio_off), this consumer's
// thread takes them in order and fires "consumer-frame-show" when the sound clock reaches each one. A frame that
// comes late is shown at once; frames MLT could not render in time (it drops images itself when rendering is too
// slow) are skipped. Stopped (speed 0): every "refresh" shows the current frame once, like MLT's sdl2_audio.
//
// Everything else is a normal MLT consumer (connect, start, stop, purge, properties) – the Engine drives it like the
// sdl2_audio consumer of the MLT sound path.

#include <functional>
#include <memory>

namespace Mlt {
class Consumer;
class Profile;
} // namespace Mlt

class PreviewConsumer {
public:
    // Where the sound is, in frames of the connected producer (fractional: frame f plays from f to f + 1); NaN = no
    // clock (show at once)
    using Clock = std::function<double()>;

    PreviewConsumer(Mlt::Profile& profile, Clock clock);
    ~PreviewConsumer();

    // The MLT consumer (own reference)
    Mlt::Consumer& consumer();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
