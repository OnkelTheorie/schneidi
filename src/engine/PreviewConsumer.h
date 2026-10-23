#pragma once
// Preview with the own audio engine (docs/plan-0.10-ton.md, step 5): picture follows sound. An MLT consumer that
// only shows pictures – MLT renders them ahead in its read-ahead thread (real_time 1, audio_off), this consumer's
// thread takes them in order and fires "consumer-frame-show" when the sound clock reaches each one. A frame that
// comes late is shown at once; frames MLT could not render in time (it drops images itself when rendering is too
// slow) are skipped. Stopped (speed 0): every "refresh" shows the current frame once, like MLT's sdl2_audio.
//
// Keeping up: skipping images does not make rendering faster (a producer still decodes every frame up to the one
// asked for), so on a machine too slow for the material (or shuttling, or long-GOP material backwards) the picture
// would fall further and further behind the sound. When it is more than a moment behind and rendering would not
// catch up by itself sooner, the consumer jumps the producer ahead to where the clock will be once the jump is done
// (its cost – up to a whole GOP of decoding – is measured on every jump). The picture becomes a slideshow in sync,
// the sound runs on untouched. On a machine fast enough nothing jumps.
//
// Everything else is a normal MLT consumer (connect, start, stop, purge, properties) – the Engine drives it like the
// sdl2_audio consumer of the MLT sound path, except that it moves the producer through seek() while connected.

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

    // Moves the connected producer to `position` (its frames) and drops what was rendered before. Seeks of the
    // connected producer go through here: a catch-up jump decided on a frame from before never overrides them.
    void seek(int position);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
