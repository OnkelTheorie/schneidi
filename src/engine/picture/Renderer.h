// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Own picture engine (0.15, CPU path): evaluates the timeline model directly for one frame number and returns an RGBA
// picture – the replacement for TimelineBuilder + MLT tractor + consumer. No MLT here (no <Mlt.h>).
//
// Design:
// - A Renderer is built from a Timeline + Options. It keeps an immutable snapshot of the timeline (shared_ptr, swapped
//   by update()) and a per-source cache of decoders (video/Reader, stills, titles). Building it opens nothing: sources
//   are opened lazily by the first frame that needs them (or ahead by prepare()).
// - render(frame) is thread-safe and may run for several frames at once (export workers, preview read-ahead). Each
//   call takes the current snapshot, works out what is visible at that frame (track by track from the top, skipping
//   what an opaque full-frame layer covers – covered clips are not decoded) and composites into a pooled buffer.
// - update(tl) is cheap: it swaps the snapshot (frames already being rendered finish with the old one) and keeps every
//   decoder whose file is still used, so a change in the Inspector costs one re-render, not a rebuild. Replaces
//   LiveUpdate.
// - Decoders are shared per (file, track lane): two renders of neighbouring frames use the same Reader, which keeps a
//   window of decoded pictures around the last request and decodes ahead in the direction set by setDirection().
//   Clips of the same file meeting in a transition get two lanes (as TimelineBuilder's "second" producer), so one
//   decoder does not seek back and forth.
// - Pixels: 8-bit RGBA, straight alpha (what video/Compose, ColorGrade, VideoEffects, BlurFill work on). Timeline
//   values (Clip::transform, titles) are in project pixels (Options::format); the output may be smaller (preview) –
//   everything is scaled by output/project size, so a preview frame looks like a shrunk export frame.
// - Frame carries its size, colour description and a pooled buffer; a GPU texture handle can be added to it later
//   (package 7) without changing callers that only read pixels.
//
// Semantics follow TimelineBuilder (tests/test_picture_parity compares both paths pixel by pixel).

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QImage>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace Picture {

// Colour description of a frame's pixels (what OutputColor/the encoder needs to convert them)
struct ColorInfo {
    int colorspace = 709;   // 601 / 709 / 2020 (matrix the RGB came from; the RGB itself is display RGB)
    bool fullRange = true;  // RGBA is always full range; kept for a later YUV/GPU frame
};

// One pooled RGBA buffer. Returned to its pool when the last Frame referring to it goes away.
class Buffer;

// A rendered picture. Cheap to copy (shared buffer). Invalid (isNull) when nothing could be rendered.
class Frame {
public:
    Frame() = default;
    bool isNull() const { return !m_buffer; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    int stride() const { return m_width * 4; }           // bytes per row (rows are tightly packed)
    const uint8_t* bits() const;                         // RGBA, straight alpha, width*height*4 bytes
    uint8_t* bits();                                     // writable only while you hold the only reference
    int64_t position() const { return m_position; }      // timeline frame it shows
    const ColorInfo& color() const { return m_color; }
    // Fully opaque everywhere (no transparent background, nothing see-through at the bottom)
    bool opaque() const { return m_opaque; }
    // Copy as QImage (Format_RGBA8888 for straight alpha); for tests, stills and the QPainter viewer
    QImage toImage() const;
    // A view as QImage without copying (valid while this Frame lives)
    QImage view() const;

private:
    friend class Renderer;
    friend struct FrameAccess;
    std::shared_ptr<Buffer> m_buffer;
    int m_width = 0, m_height = 0;
    int64_t m_position = 0;
    ColorInfo m_color;
    bool m_opaque = false;
};

enum class Mode {
    Preview, // fast YUV->RGB conversion, proxies, render cache allowed, grade bypass honoured
    Export,  // exact conversion, originals only
    Still,   // like Export (one frame: thumbnails, grabFrame, snapshot)
};

struct Options {
    ProjectFormat format;              // project size and frame rate (pixel values of the model refer to it)
    QSize output;                      // size of the produced frames; empty = format.size()
    Mode mode = Mode::Export;
    // Which file is read for a media file (preview: proxy instead of original); empty function = always the original
    std::function<QString(const QString& path)> proxy;
    // Render cache (preview): fully rendered output of a video clip (frame 0 = clip start), empty string = none
    std::function<QString(const Clip& clip)> renderCache;
    // Frames outside [rangeStart, rangeEnd] are never asked for (export of in/out, render cache): decoders are not
    // opened for clips entirely outside; rangeEnd < 0 = to the end
    int64_t rangeStart = 0, rangeEnd = -1;
    bool subtitles = true;             // draw the visible subtitle track (export: only when burning in)
    bool transparent = false;          // transparent instead of black where nothing covers (grabFrame, compound content)
    int decoderThreads = 0;            // threads per video decoder (0 = FFmpeg's choice)
    int parallelFrames = 1;            // frames rendered at once: decoders keep that many pictures around a request
    // Preview "Before/After": true = colour correction bypassed (read at every render)
    std::shared_ptr<const std::atomic<bool>> gradeBypass;

    QSize outputSize() const { return output.isEmpty() ? format.size() : output; }
};

class Renderer {
public:
    Renderer(const Timeline& tl, const Options& options);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // The picture of timeline frame `frame`. Thread-safe; several calls may run at once.
    Frame render(int64_t frame);
    // Takes a changed timeline (same project format). Renders already running finish with the old one.
    void update(const Timeline& tl);
    // Output size changes (viewer resized); renders already running finish at the old size
    void setOutputSize(const QSize& size);
    // Playback direction for decoding ahead: 1 forward, -1 backward, 0 = still (seeks/scrubbing)
    void setDirection(int direction);
    // Open the decoders the frames around `frame` will need (call from a worker thread before playback starts)
    void prepare(int64_t frame);
    // Drop decoders and cached pictures not used for a while (memory pressure, idle)
    void trim();

    // Frames of content (TimelineOps::endFrame of the current snapshot)
    int64_t length() const;
    const Options& options() const;

    // Counters for tests and the log
    struct Stats {
        int framesRendered = 0;
        int sourcesOpen = 0;       // decoders/stills/titles currently cached
        int layersDecoded = 0;     // clip pictures fetched (covered clips do not count)
        int layersSkipped = 0;     // clip pictures not fetched because a layer above covered the frame
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// SCHNEIDI_PICTURE=own|mlt (read once): which picture path the engine uses while both exist. Default "mlt" until
// package 4/5 switch preview and export over.
bool ownPicture();

} // namespace Picture
