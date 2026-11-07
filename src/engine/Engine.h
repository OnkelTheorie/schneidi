// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Preview, stills and probing for the UI: the own picture engine (engine/picture) shows frames by the clock of the
// own audio engine (engine/audio). UI only talks via this class.

#include "core/ProjectFormat.h"
#include "core/Types.h"
#include "engine/picture/Pump.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <memory>
#include <mutex>

namespace Audio {
class Player;
}
class ProxyManager;
class RenderCache;
class IdleRelease;
namespace MemoryRelease {
struct Activity;
}
class StillFetcher;
struct SharedLoudness;
namespace Picture {
class Renderer;
struct Options;
} // namespace Picture

// Grid of a multicam clip for the viewer (ui/MulticamView): all angles of a page in their cells (ui/MulticamGrid),
// frame = frame of the multicam sequence. Sound = only the active angle.
struct MulticamGridOptions {
    int columns = 0;     // 0 = auto by the number of angles, 2..4 = fixed (MulticamGrid::columns)
    int page = 0;        // clamped to the last page
    int activeAngle = 0; // whose sound plays
};

// Loudness meter (ITU-R BS.1770) of the preview: measured at the master, only during playback at normal speed
struct LoudnessReading {
    double momentary = -200, shortTerm = -200, integrated = -200; // LUFS (-200 = nothing yet)
    double range = 0;   // LU
    double seconds = 0; // measured duration
    double truePeak = -200; // highest true peak since reset (dBTP, all channels; -200 = nothing yet)
};

class Engine : public QObject {
    Q_OBJECT
public:
    // Multicam: the grid of a multicam clip (ui/MulticamView) plays instead of the timeline; positions are still
    // timeline frames (grid frame + offset), so playhead and cuts work as in Timeline mode
    enum class Mode { Timeline, Source, Multicam };

    explicit Engine(QObject* parent = nullptr);
    ~Engine() override;

    bool init(QString* error);
    // What the engine needs without a preview (logging, the crop converter's probe); init() calls it.
    // Enough for headless use (schneidi-cli): probing, rendering, analysis.
    static void initHeadless();
    // Project settings: new renderers in the new format (Viewer then shows the timeline)
    void setFormat(const ProjectFormat& format);
    const ProjectFormat& format() const { return m_format; }
    int fps() const { return m_format.rate.timebase(); } // timecode frames per second (29.97 -> 30)
    QSize frameSize() const { return m_format.size(); }  // project format in pixels
    // Preview frames at the project size instead of scaled down (longer edge 960): the viewer is zoomed to 100 % or
    // more and would otherwise show the small picture enlarged
    void setPreviewFullSize(bool full);
    bool previewFullSize() const { return m_previewFull; }
    // Longer edge the scaled-down preview frames at least get (0 = the default 960): the full-screen viewer asks for
    // its screen's pixels, so the picture is sharp there without rendering beyond what the screen shows
    void setPreviewEdge(int px);
    int previewEdge() const { return m_previewEdge; }
    QSize previewSize() const { return m_previewSize; }

    // Proxy media: preview (timeline video tracks, source view) reads the proxy if present and
    // enabled; audio and export always from the original. Call updateTimeline() after changes.
    ProxyManager* proxies() const { return m_proxies; }
    // Render cache: preview plays pre-rendered clip outputs (export never); updateTimeline() queues missing ones
    RenderCache* renderCache() const { return m_renderCache; }
    // Single frames from media files in their own thread (trim view while dragging an edit); follows setFormat()
    StillFetcher* stills() const { return m_stills; }
    // Gives the picture pools and the free heap back once nothing renders (after stopping, rebuilds, exports)
    IdleRelease* idleRelease() const { return m_idleRelease; }
    // What renders right now: playback, an export, the render cache (MemoryRelease's policy)
    MemoryRelease::Activity activity() const;

    MediaInfo probe(const QString& path);
    // Same without an Engine instance: lengths count in the frame rate of `format`. Reads the file's header itself
    // (engine/MediaProbe, ~1 ms); unreadable files give an empty MediaInfo (no video, no audio)
    static MediaInfo probe(const ProjectFormat& format, const QString& path);
    // Timecode of the file's first frame ("01:00:00:00", from cameras and other editors' renders); empty = none
    static QString startTimecode(const QString& path);
    // Width / height of the file's picture as the timeline shows it (pixel aspect, rotation), 0 = none/unreadable.
    // initHeadless() installs it as MediaAspect's probe (core: converting crop values).
    static double pictureAspect(const QString& path);
    QImage thumbnail(const QString& path, int frame, const QSize& size);
    // Same without an Engine instance (usable from worker threads): the picture in the shape of `format`
    // (as a timeline shows it at zoom 1), with ownShape in the file's own shape (pixel aspect and rotation included:
    // Media Pool tiles, where a landscape clip fills its 16:9 tile also in a 9:16 project), centered in `size` with
    // black borders. aspect > 0: the picture shown at that shape instead of the file's (Clip/MediaInfo::pictureAspect,
    // a stretched 4:3 recording), also as the own shape
    static QImage thumbnail(const ProjectFormat& format, const QString& path, int frame, const QSize& size,
                            bool ownShape = false, double aspect = 0);

    // Still (Grab Still) of the currently shown image (timeline resp. source) at full project resolution,
    // always from the originals (not from proxies)
    QImage grabStill(const Timeline& tl);
    // Frame `position` of `tl` the same way, but RGBA with transparency where nothing covers the frame
    // (Green Screen pipette: one clip alone without its key)
    QImage grabFrame(const Timeline& tl, int position);

    // Model changed: the renderer takes it as a new snapshot (playback goes on); files open lazily in the render
    // threads, the window never waits for them
    void updateTimeline(const Timeline& tl);
    // Kept for callers from the time files were opened ahead: the own engine never waits for files
    void setOpenInBackground(bool) {}
    bool opening() const { return false; }
    // Lengths of the project's media in frames (Project::media()): the sound is mixed with them like in the export
    void setMediaLengths(const QHash<QString, int>& lengths);
    // Next updateTimeline() only changes fader/pan/mute/solo (the player takes every change live anyway)
    void mixerOnlyNext() { m_mixerOnlyNext = true; }
    bool mixerOnlyPending() const { return m_mixerOnlyNext; }
    // keepSpeed: keep playing (leaving the multicam grid during playback)
    void showTimeline(int position, bool keepSpeed = false);
    // aspect > 0: the picture at that shape (MediaInfo::pictureAspect, a stretched 4:3 recording)
    void showSource(const QString& path, int position = 0, double aspect = 0);
    Mode mode() const { return m_mode; }
    // Positions are timeline frames (Timeline or Multicam mode)
    bool timelinePositions() const { return m_mode != Mode::Source; }
    // Multicam grid of a multicam sequence (content = Project::renderTimeline(id), angles = Sequence::angles):
    // offset = timeline frame minus sequence frame, position = timeline frame. Keeps playing. Same grid as shown:
    // only offset and sound are applied (no rebuild, so cutting/switching does not restart playback).
    // false = nothing to show (no angles).
    bool showMulticam(const Timeline& content, const QVector<MulticamAngle>& angles, const MulticamGridOptions& options,
                      int offset, int position);
    // Sound of this angle in the grid, live
    void setMulticamAudio(int angle);
    // Last file shown in the source view (kept after Q/timeline, empty = none yet)
    const QString& sourcePath() const { return m_sourcePath; }
    double sourceAspect() const { return m_sourceAspect; }

    void play();
    void pause();
    void togglePlay();
    void setSpeed(double speed);
    double speed() const { return m_speed; }
    // Seeks in quick succession while stopped (dragging the playhead, holding an arrow key) scrub: a close picture
    // is shown at once, the exact one follows (own picture engine)
    void seek(int frame);
    int position() const { return m_position; }
    // Counters of the preview's frame pump (tests, benchmarks)
    Picture::Pump::Stats pictureStats() const;
    // While playing: the frame heard right now (fractional, in positions like position()); NaN when stopped.
    // position() = the frame on screen, which follows it.
    double soundFrame() const;

    // Before/After (Color page, like DaVinci "Bypass Color Grades"): preview without color correction; export untouched
    void setColorBypass(bool on);
    bool colorBypass() const { return m_gradeBypass->load(); }
    LoudnessReading loudness() const;
    void resetLoudness(); // restart Integrated/LRA (like the reset button in DaVinci)

    // Audio output (menu Playback → Audio output, saved in QSettings). Larger buffer against crackling/
    // dropouts (Windows/WASAPI needs more than PulseAudio), at the cost of slightly more delay. Takes effect from the next play.
    static int audioBuffer();
    void setAudioBuffer(int samples);
    // Audio driver (empty = automatic; Windows: "directsound" (default)/"wasapi"): miniaudio's backend of that name
    // (from the next play)
    static QString audioDriver();
    static void setAudioDriver(const QString& driver);
    // Output device of the own sound engine (Audio::Device::Output::id, empty = system default); at once
    static QString audioOutput();
    void setAudioOutput(const QString& id);

signals:
    void frameReady(const QImage& image);
    void positionChanged(int frame);
    void speedChanged(double speed);
    void colorBypassChanged(bool on);
    void modeChanged(Engine::Mode mode);
    // Only during playback: peak levels in dBFS, [A1 L, A1 R, A2 L, …, Master L, Master R];
    // the master measures true peak (dBTP, BS.1770 4x oversampled) like the delivery check
    void audioLevels(const QVector<float>& db);
    // internal: position from the pump thread, epoch = jump counter when displaying (older ones are discarded)
    void framePosition(int frame, int epoch);
    // Files are being opened in the background (setOpenInBackground) / done and built
    void openingChanged(bool opening); // never emitted any more (files open lazily)

private:
    void createPump();
    void applyPreviewSize(); // m_previewSize from full/edge, renderers at the new size

    void refresh();
    void applyAudioState();
    void applyTimeline(const Timeline& tl);
    void emitLevels();
    // The pump shows `content` from `position` (Timeline/Source/Multicam), frames come to onPumpShown (pump thread)
    bool connected() const;
    void showContent(std::shared_ptr<const Picture::Pump::Content> content, int position);
    void onPumpShown(const QImage& image, int64_t position, double speed);
    Picture::Options previewOptions(const QSize& output) const;
    std::shared_ptr<const Picture::Pump::Content> rendererContent(std::shared_ptr<Picture::Renderer> renderer, bool timeline);
    // Proxy and render-cache files of the shown timeline, resolved on the UI thread (the renderers ask from theirs);
    // true = something changed; timeline = the render cache's files too (else only proxies are added)
    bool resolveFiles(const Timeline& tl, bool timeline);
    QImage ownStill(const Timeline& tl, int position, bool transparent);
    // The clock in frames of what is shown, where playback of `frame` (content frame) starts in samples, and what
    // the player plays in the current mode (positions = content frames)
    double clockFrames() const;
    double soundStart(int frame, double speed) const;
    void syncPlayer(int position, bool restart);
    // Playback ends at the last frame of what is shown (timeline frames; -1 = nothing there), going backwards at 0.
    // Multicam grid: no end of its own, the window moves on to the next segment.
    int lastFrame() const;
    bool atEnd(double speed) const;
    void stopAtEnd();

    ProxyManager* m_proxies;
    RenderCache* m_renderCache;
    StillFetcher* m_stills;
    IdleRelease* m_idleRelease;
    ProjectFormat m_format;
    bool m_formatSet = false;
    Timeline m_built; // the timeline shown
    quint64 m_builtTrust = 0; // CodeTrust::epoch() of m_built: trusting code changes the picture, not the timeline
    std::unique_ptr<SharedLoudness> m_loudness; // fed by the master level meter in the audio thread
    bool m_mixerOnlyNext = false;
    std::shared_ptr<std::atomic<bool>> m_gradeBypass = std::make_shared<std::atomic<bool>>(false);
    Timeline m_gridContent;
    QVector<MulticamAngle> m_gridAngles;
    int m_gridColumns = 0, m_gridPage = 0;
    std::atomic<int> m_offset{0}; // timeline frame minus content frame (Multicam mode, else 0)
    QString m_sourcePath;
    double m_sourceAspect = 0;

    // Atomic: onPumpShown reads them on the pump's thread while the UI thread switches mode/speed
    std::atomic<Mode> m_mode{Mode::Timeline};
    std::atomic<double> m_speed{0.0};
    std::atomic<int> m_position{0};
    // Jump during playback: no longer report positions from before the jump
    std::atomic<int> m_seekEpoch{0};
    QElapsedTimer m_lastSeek;  // seeks in quick succession = dragging (muted until it rests)
    QTimer* m_seekRelease;     // no seek for a moment: dragging ended, sound fades in
    QSize m_previewSize{960, 540}; // project format scaled down (longer edge 960), or full (m_previewFull)
    bool m_previewFull = false;
    int m_previewEdge = 0; // setPreviewEdge

    // Own audio engine (docs/archiv/plan-0.10-ton.md): the player plays the sound, the picture follows its clock
    std::unique_ptr<Audio::Player> m_player;
    Timeline m_sourceSound;  // source view: the file as a timeline
    int m_gridSoundAngle = 0; // multicam grid: whose sound plays

    // Own picture engine: the pump renders ahead and shows by the sound clock, the renderers evaluate the model
    std::unique_ptr<Picture::Pump> m_pump;
    std::shared_ptr<Picture::Renderer> m_renderer;       // timeline
    std::shared_ptr<Picture::Renderer> m_sourceRenderer; // source view
    std::vector<std::shared_ptr<Picture::Renderer>> m_gridRenderers; // multicam cells of the shown page
    std::shared_ptr<const Picture::Pump::Content> m_timelineContent;        // Pump::Content of m_renderer
    int m_sourceLength = 0;
    std::shared_ptr<std::atomic<int64_t>> m_lastFrame = std::make_shared<std::atomic<int64_t>>(0);
    struct Resolved;
    std::shared_ptr<Resolved> m_resolved;
};
