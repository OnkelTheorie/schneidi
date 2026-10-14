#pragma once
#include "core/Types.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

class ProducerFactory;

namespace Mlt {
class Filter;
class Profile;
class Producer;
class Tractor;
class Playlist;
} // namespace Mlt

// Translates the timeline model into an MLT tractor:
// track 0 = black background, then V1..Vn (composited on top of each other), then A1..An (mixed).
// The model stays the truth; the tractor is rebuilt on changes.

// Mixer connections for the preview: fader/pan filters (adjustable live, without rebuild)
// and level meters (own filter, after fader/pan/limiter). The export does not need them.
struct MixerHooks {
    struct Strip {
        std::shared_ptr<Mlt::Filter> volume, pan, meter;
        std::shared_ptr<Mlt::Filter> limiter; // master only: always attached, switched via "disable"
        std::shared_ptr<Mlt::Filter> dsp;     // tracks only: EQ + compressor (before the fader)
        bool audible = true; // false = muted/soloed away -> level -∞
        std::shared_ptr<Mlt::Playlist> playlist; // tracks only: mute/solo live via "hide" (without rebuild)
    };
    std::vector<Strip> tracks; // per audio track
    Strip master;
};

// Preview: a seek during playback cuts the sound mid-wave on both sides (audible click), starting playback
// starts it mid-wave. The engine sets the
// target, the master meter fades out the old sound still coming (frames before the target), keeps it silent and
// fades in at the target.
struct SeekFade {
    std::atomic<int> target{-1};    // timeline frame; -1 = nothing to do
    std::atomic<int> direction{1};  // playback direction (1 forward, -1 backward)
    std::atomic<bool> fadedOut{false};
    std::atomic<int> waited{0}; // frames silenced so far (emergency stop: the sound comes back after a few)
    void start(int frame, double speed)
    {
        direction = speed < 0 ? -1 : 1;
        fadedOut = false;
        waited = 0;
        target = frame;
    }
    // Playback starts (was silent before): only the fade-in at `frame`, the sound jumps in mid-wave otherwise
    void fadeIn(int frame, double speed)
    {
        start(frame, speed);
        fadedOut = true;
    }
};

// Preview: spot where a decoder has to jump (clip start, fading-in side of a transition).
// With long GOPs that takes up to ~1 s (audio ~150 ms) -> the engine (engine/Preroll) decodes the frame before it beforehand.
struct PrerollPoint {
    int frame = 0;       // timeline frame from which the producer is read
    int freeFrom = 0;    // from this timeline frame on nobody reads it anymore (occupied before; 0 = free)
    int source = 0;      // frame in the producer at `frame`
    bool audio = false;
    std::shared_ptr<Mlt::Producer> producer; // own MLT reference
};

class TimelineBuilder {
public:
    // Which file is read for a clip (preview: proxy instead of original); without = always the original
    using MediaResolver = std::function<QString(const QString& path, TrackKind kind)>;

    explicit TimelineBuilder(Mlt::Profile& profile);
    ~TimelineBuilder();

    void setResolver(MediaResolver resolver) { m_resolver = std::move(resolver); }
    // Preview "Before/After" (Color page): true = bypass color correction; takes effect live without rebuild.
    // Without it (export, still) the correction is always applied.
    void setGradeBypass(std::shared_ptr<std::atomic<bool>> flag) { m_gradeBypass = std::move(flag); }
    // Render cache (preview only, engine/RenderCache.h): fully rendered output of a video clip (file) or
    // empty = normally from the original with effects. Without = never (export, still).
    using ClipCache = std::function<QString(const Clip& clip)>;
    void setClipCache(ClipCache cache) { m_clipCache = std::move(cache); }

    // Subtitles (visible subtitle track) into the image too; default on (preview), export only when burning in
    void setSubtitles(bool on) { m_subtitles = on; }
    // Threads per video decoder (export); 0 = MLT default. Set before build().
    void setDecoderThreads(int threads);

    // hooks != nullptr: always attach mixer filters (even at 0 dB) and build in level meters
    std::unique_ptr<Mlt::Tractor> build(const Timeline& tl, MixerHooks* hooks = nullptr);
    // Jump spots of the decoders from the last build() (outer timeline only), sorted by frame
    const std::vector<PrerollPoint>& prerollPoints() const { return m_preroll; }
    // Apply fader/pan/limiter/EQ/compressor live to the filters (the track count must match)
    static bool applyMixer(const Timeline& tl, MixerHooks& hooks);
    // Audio of a clip as the timeline reads it (speed/reverse included), without
    // clip volume/fades; frames in..out of the clip. nullptr = title/still/file missing.
    // Owned by the builder (cache), valid until the next call.
    Mlt::Producer* clipAudioSource(const Clip& c);
    // Image of a single video clip with effects, transform, speed and keyframes (without fades and
    // transitions) on a transparent background; frame 0 = first clip frame. For the render cache.
    std::unique_ptr<Mlt::Tractor> buildClipOutput(const Clip& c);

private:
    // second: second producer of the same file. Clips at a crossfade alternate (otherwise one
    // decoder seeks back and forth between two spots of the same file); a clip reads transition and body from the same one.
    // retime: clip whose speed/reverse/still and audio stream apply (nullptr = original speed, stream 0)
    Mlt::Producer* producerFor(const QString& path, TrackKind kind, int trackIndex, bool second = false,
                               const Clip* retime = nullptr);

    // A timeline without cleaning up the producer cache (also for nested sequences)
    std::unique_ptr<Mlt::Tractor> buildTimeline(const Timeline& tl, MixerHooks* hooks);
    // Compound clip: content of the sequence (from Timeline::nested) as a tractor; nullptr = missing/loop
    Mlt::Producer* nestedProducer(int sequenceId, TrackKind kind, int trackIndex, bool second);

    // Remembers where p is read; if the decoder jumps there, a PrerollPoint spot is noted
    void noteUse(Mlt::Producer* p, TrackKind kind, int from, int to, int in);

    Mlt::Profile& m_profile;
    std::unique_ptr<ProducerFactory> m_factory; // before m_cache: must outlive the producers
    MediaResolver m_resolver;
    std::shared_ptr<std::atomic<bool>> m_gradeBypass;
    ClipCache m_clipCache;
    bool m_subtitles = true;
    bool m_transparent = false; // background transparent instead of black (buildClipOutput)
    // Per file *and* track one own producer, so that video and audio tracks do
    // not seek each other back and forth in the decoder.
    std::map<QString, std::unique_ptr<Mlt::Producer>> m_cache;
    std::set<QString> m_used; // keys used in the current build(); the rest gets dropped afterwards
    // Nested timelines (compound clips) of the running build()
    std::shared_ptr<const NestedTimelines> m_nested;
    std::map<QString, std::unique_ptr<Mlt::Producer>> m_nestedCache;
    std::set<int> m_nestStack; // sequences currently being built (loop protection)
    bool m_inNested = false;
    QString m_keyPrefix; // cache key prefix inside nested sequences
    std::vector<PrerollPoint> m_preroll;
    struct LastUse { int sourceEnd = -2, timelineEnd = 0; };
    std::map<Mlt::Producer*, LastUse> m_lastUse; // only during build()
};
