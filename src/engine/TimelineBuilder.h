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
// starts it mid-wave. The master meter notices the jump itself (frame position not the next one) and turns the step
// into a dip: held-back old sound fades out, new sound fades in. (Fading out "the frames still coming" after the
// seek did not work: usually none come, the old sound was already in the device buffer and ended mid-wave.)
struct SeekFade {
    // Engine side
    std::atomic<int> direction{1};    // 1 / -1 = normal playback forward/backward; 0 = other speed: untouched
    std::atomic<bool> restart{true};  // playback (re)starts: nothing old to fade out, fade in the first sound
    // Seeks in quick succession (dragging the playhead while playing): stay silent until release() – a fade-in per
    // mouse move crackled
    std::atomic<bool> hold{false};
    // Counted up before every seek while playing: a chunk rendered during the seek can carry the new sound under
    // the next frame position (race with MLT's read-ahead) – positions alone missed such jumps
    std::atomic<int> seeks{0};
    // Playback starts or changes speed: the sound jumps in mid-wave otherwise
    void play(double speed)
    {
        direction = speed == 1.0 ? 1 : speed == -1.0 ? -1 : 0;
        restart = true;
    }
    void release() { hold = false; }

    // Master meter side (only the thread rendering the audio): the last 5 ms of every chunk come out with the next
    // one, so at a jump in the frame positions the old sound can still be faded out
    std::vector<float> tail;
    int tailChannels = 0;
    int lastPos = -1;
    int seenSeeks = 0;
    bool silent = true;
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

// Grid of a multicam clip for the viewer (ui/MulticamView): all angles of a page in their cells (ui/MulticamGrid),
// frame = frame of the multicam sequence. Sound = only the active angle.
struct MulticamGridOptions {
    int columns = 0;     // 0 = auto by the number of angles, 2..4 = fixed (MulticamGrid::columns)
    int page = 0;        // clamped to the last page
    int activeAngle = 0; // whose sound plays
};
// Live switching of the grid's sound (applyMulticamAudio), without rebuilding
struct MulticamGridHooks {
    std::vector<std::shared_ptr<Mlt::Playlist>> audio; // per angle (nullptr = angle without sound)
    int active = -1;
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
    // The built timeline lasts at least this long (preview with the own audio engine: the picture graph has no
    // sound tracks but must last as long as the sound)
    void setMinimumLength(int frames) { m_minLength = frames; }
    // buildMulticamGrid() with the angles' sound (MLT sound path); off = picture only
    void setGridSound(bool on) { m_gridSound = on; }
    // Set from another thread: build() stops opening files and returns an empty tractor (export cancelled)
    void setCancelFlag(std::shared_ptr<const std::atomic<bool>> flag) { m_cancel = std::move(flag); }

    // hooks != nullptr: always attach mixer filters (even at 0 dB) and build in level meters
    std::unique_ptr<Mlt::Tractor> build(const Timeline& tl, MixerHooks* hooks = nullptr);
    // Jump spots of the decoders from the last build() (outer timeline only), sorted by frame
    const std::vector<PrerollPoint>& prerollPoints() const { return m_preroll; }
    // Apply fader/pan/limiter/EQ/compressor live to the filters (the track count must match)
    static bool applyMixer(const Timeline& tl, MixerHooks& hooks);
    // `after` differs from `before` only in what applyMixer() switches live (or not at all in the picture/sound:
    // track names, colors, locks) – also for changes from the CLI/AI, which do not say so like the Mixer panel
    static bool mixerOnlyChange(const Timeline& before, const Timeline& after);
    // Audio of a clip as the timeline reads it (speed/reverse included), without
    // clip volume/fades; frames in..out of the clip. nullptr = title/still/file missing.
    // Owned by the builder (cache), valid until the next call.
    Mlt::Producer* clipAudioSource(const Clip& c);
    // Image of a single video clip with effects, transform, speed and keyframes (without fades and
    // transitions) on a transparent background; frame 0 = first clip frame. For the render cache.
    std::unique_ptr<Mlt::Tractor> buildClipOutput(const Clip& c);
    // Grid of a multicam sequence (content = Sequence::timeline, angles = Sequence::angles); nullptr = no angles
    std::unique_ptr<Mlt::Tractor> buildMulticamGrid(const Timeline& content, const QVector<MulticamAngle>& angles,
                                                    const MulticamGridOptions& options, MulticamGridHooks* hooks = nullptr);
    // Let the sound of `angle` play in the grid (live); false = no such angle (unchanged)
    static bool applyMulticamAudio(MulticamGridHooks& hooks, int angle);

private:
    // second: second producer of the same file. Clips at a crossfade alternate (otherwise one
    // decoder seeks back and forth between two spots of the same file); a clip reads transition and body from the same one.
    // retime: clip whose speed/reverse/still and audio stream apply (nullptr = original speed, stream 0)
    Mlt::Producer* producerFor(const QString& path, TrackKind kind, int trackIndex, bool second = false,
                               const Clip* retime = nullptr);
    // What producerFor reads: cache key, file (after the proxy resolver), MLT resource (timewarp: …), speed ramp
    struct Source {
        QString key, file, resource;
        bool ramp = false;
    };
    Source sourceOf(const QString& path, TrackKind kind, int trackIndex, bool second, const Clip* retime) const;
    // Opens the producers a build will need in parallel threads into m_opened (opening is slow, see prefetch)
    void prefetch(const Timeline& tl);
    // A new producer of the file: from m_opened if prefetch() opened it, otherwise opened now
    std::unique_ptr<Mlt::Producer> openFile(TrackKind kind, const QString& resource);
    static QString openKey(TrackKind kind, const QString& resource);

    // A timeline without cleaning up the producer cache (also for nested sequences)
    std::unique_ptr<Mlt::Tractor> buildTimeline(const Timeline& tl, MixerHooks* hooks);
    // Compound clip: content of the sequence (from Timeline::nested) as a tractor; nullptr = missing/loop.
    // angle >= 0 (multicam segment): only the tracks of that angle (its picture resp. its sound)
    Mlt::Producer* nestedProducer(int sequenceId, TrackKind kind, int trackIndex, bool second, int angle = -1);
    // A sequence's content as a nested tractor (transparent, no subtitles, own decoders under keyPrefix)
    std::unique_ptr<Mlt::Tractor> buildInner(const Timeline& tl, const QString& keyPrefix);

    // Remembers where p is read; if the decoder jumps there, a PrerollPoint spot is noted
    void noteUse(Mlt::Producer* p, TrackKind kind, int from, int to, int in);

    Mlt::Profile& m_profile;
    std::unique_ptr<ProducerFactory> m_factory; // before m_cache: must outlive the producers
    MediaResolver m_resolver;
    std::shared_ptr<std::atomic<bool>> m_gradeBypass;
    ClipCache m_clipCache;
    bool m_subtitles = true;
    int m_minLength = 0;
    bool m_gridSound = true;
    bool m_transparent = false; // background transparent instead of black (buildClipOutput)
    // Per file *and* track one own producer, so that video and audio tracks do
    // not seek each other back and forth in the decoder.
    std::map<QString, std::unique_ptr<Mlt::Producer>> m_cache;
    std::set<QString> m_used; // keys used in the current build(); the rest gets dropped afterwards
    std::shared_ptr<const std::atomic<bool>> m_cancel;
    bool cancelled() const { return m_cancel && m_cancel->load(); }
    std::map<QString, std::vector<std::unique_ptr<Mlt::Producer>>> m_opened; // openKey -> opened by prefetch(), unused
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
