// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Frame pump of the preview (0.15, own picture engine).
//
// - Worker threads render the frames ahead of the one shown next (in playback direction, `step` frames apart at
//   shuttle speeds) through Content::render (Renderer::render, or the multicam grid tiling several renderers). A
//   frame rendered for an older content generation (refresh(): model or bypass changed) is dropped.
// - The show thread takes the next frame in order and hands it to the Show callback when the sound clock reaches it
//   (picture follows sound; the clock is the own audio engine's). A frame that comes late is shown at once; frames
//   whose time has passed while a later one is already rendered are skipped.
// - Keeping up: when the picture is more than a moment behind the clock and rendering does not make up for it by
//   itself, the pump jumps ahead to where the clock will be once the jump is done (its cost - decoding from the
//   keyframe - is measured on every jump). On a machine fast enough nothing jumps.
// - Stopped (speed 0): the frame at the position is rendered and shown once, again after every seek()/refresh().
//   The newest seek wins: renders for older positions are cancelled (Content::renderWant) or finish unseen.
//   Scrubbing (seek(p, true)): first a close picture that is there at once (Want::nearest: the decoders' newest
//   picture before p, after a jump the key frame), then always the exact one - never a stand-in stays on screen.
// - Recently shown frames are kept (a byte budget, preview size): stepping back, J-K-L around the playhead and
//   playing a stretch again take them instead of rendering. Dropped when the content changes.
// - After seek()/setSpeed()/setContent() return, no frame from before is shown any more (the Show callback runs
//   under a lock those take).
//
// Positions are content frames (the multicam grid's are sequence frames; the Engine adds its offset).

#include "core/SystemMemory.h"

#include <QImage>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace Picture {

class Pump {
public:
    // How a still frame may be rendered (Content::renderWant)
    struct Want {
        bool nearest = false;                      // a close picture will do (scrubbing)
        const std::atomic<bool>* cancel = nullptr; // set when the frame is not wanted any more: return soon
        bool exact = true;                         // out: the picture is exactly frame f's
    };
    struct Content {
        // The picture of content frame f (any thread, several at once); null image = nothing (shown as black)
        std::function<QImage(int64_t f)> render;
        // Optional: the same for still frames with a Want (cancellable, scrubbing); without it `render` is used
        std::function<QImage(int64_t f, Want& want)> renderWant;
        // Last frame (inclusive) that may be rendered, < 0 = no end; read often (cheap)
        std::function<int64_t()> last;
        // Playback direction changes (1, -1, 0 = stopped): the renderer reads ahead that way (optional)
        std::function<void(int)> direction;
        // Called from a worker now and then while playing forward with frame = next frame to show: open decoders and
        // decode the first pictures of clips that start soon (optional)
        std::function<void(int64_t)> prepare;
        // Called now and then (idle workers) with the frame shown (the next one due): drop decoders unused for a while,
        // never the shown frame's (optional)
        std::function<void(int64_t shown)> trim;
        double fps = 25; // content frames per second (clock frames to seconds)
    };
    // Where the sound is, in content frames (fractional: frame f plays from f to f + 1); NaN = no clock (show at once)
    using Clock = std::function<double()>;
    // A frame to show (show thread); speed = the playback speed it was rendered for (0 = stopped)
    using Show = std::function<void(const QImage& image, int64_t position, double speed)>;

    // workers <= 0: chosen from the number of cores
    Pump(Clock clock, Show show, int workers = 0);
    ~Pump();
    Pump(const Pump&) = delete;
    Pump& operator=(const Pump&) = delete;

    // Shows `content` from `position` on at `speed` (nullptr = nothing: no frames)
    void setContent(std::shared_ptr<const Content> content, int64_t position, double speed);
    // Speed change; position = where playback goes on (the frame shown when stopping). Same direction while playing:
    // the frames rendered ahead stay.
    void setSpeed(double speed, int64_t position);
    // scrub: seeks in quick succession (dragging the playhead): a close picture first, then the exact one
    void seek(int64_t position, bool scrub = false);
    // The content changed (model, grade bypass, output size): renders again what is shown/ahead, keeps playing
    void refresh();
    int workers() const;

    struct Stats {
        int rendered = 0;  // frames rendered
        int shown = 0;     // frames handed to Show
        int skipped = 0;   // rendered frames never shown (late, or behind a jump)
        int jumps = 0;     // catch-up jumps
        double jumpCost = 0; // seconds, the estimate used for the next jump
        int recent = 0;      // frames taken from the recently shown ones instead of rendering
        int nearest = 0;     // close pictures shown while scrubbing (each followed by the exact one)
        int cancelled = 0;   // still renders given up because the position moved on
        int64_t recentBytes = 0; // memory the recently shown frames hold now
    };
    // Memory for recently shown frames (bytes; tests make it small). Default: recentBudgetFor(SystemMemory::read())
    void setRecentBudget(int64_t bytes);
    // The default budget for a machine: 1/16 of its RAM (or of the cgroup limit, the smaller), 16 to 192 MB - a
    // 16-GB computer keeps 192 MB, a Raspberry Pi Zero 2 W (415 MB) 26 MB
    static int64_t recentBudgetFor(const SystemMemory::Info& memory);
    // Forgets the recently shown frames (memory given back while idle; they are rendered again when wanted)
    void dropRecent();
    // The picture of the content changed without a new generation (a stabilization finished while playing: a refresh
    // would drop the frames prepared ahead, a hitch): the recently shown frames are forgotten, and frames whose
    // render began before are still shown but not kept - stepping back renders them again with the change
    void forgetRecent();
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Picture
