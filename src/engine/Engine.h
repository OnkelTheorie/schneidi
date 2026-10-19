#pragma once
// The only place that knows MLT (together with TimelineBuilder).
// UI only talks via this class -> engine would be replaceable.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QVector>
#include <atomic>
#include <memory>
#include <mutex>

namespace Mlt {
class Profile;
class Producer;
class Consumer;
class Tractor;
} // namespace Mlt
class TimelineBuilder;
class ProducerFactory;
class ProxyManager;
class RenderCache;
class Preroll;
class StillFetcher;
struct MixerHooks;
struct MulticamGridHooks;
struct MulticamGridOptions;
struct SharedLoudness;
struct SeekFade;

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
    // MLT itself (modules, logging, frei0r effects) without a preview consumer; init() calls it.
    // Enough for headless use (schneidi-cli): probing, rendering, analysis.
    static bool initMlt(QString* error);
    // Project settings: rebuilds profile, preview consumer and all producers (Viewer then shows
    // the timeline). The timeline itself comes with the next updateTimeline().
    void setFormat(const ProjectFormat& format);
    const ProjectFormat& format() const { return m_format; }
    int fps() const { return m_format.rate.timebase(); } // timecode frames per second (29.97 -> 30)
    QSize frameSize() const { return m_format.size(); }  // project format in pixels

    // Proxy media: preview (timeline video tracks, source view) reads the proxy if present and
    // enabled; audio and export always from the original. Call updateTimeline() after changes.
    ProxyManager* proxies() const { return m_proxies; }
    // Render cache: preview plays pre-rendered clip outputs (export never); updateTimeline() queues missing ones
    RenderCache* renderCache() const { return m_renderCache; }
    // Single frames from media files in their own thread (trim view while dragging an edit); follows setFormat()
    StillFetcher* stills() const { return m_stills; }

    MediaInfo probe(const QString& path);
    // Same without an Engine instance: lengths count in the frame rate of `format`
    static MediaInfo probe(const ProjectFormat& format, const QString& path);
    // Timecode of the file's first frame ("01:00:00:00", from cameras and other editors' renders); empty = none
    static QString startTimecode(const QString& path);
    QImage thumbnail(const QString& path, int frame, const QSize& size);

    // Still (Grab Still) of the currently shown image (timeline resp. source) at full project resolution,
    // always from the originals (not from proxies)
    QImage grabStill(const Timeline& tl);

    void updateTimeline(const Timeline& tl); // model changed -> rebuild
    // Next updateTimeline() only changes fader/pan/mute/solo -> apply live instead of rebuilding
    // (otherwise playback stutters while dragging the fader)
    void mixerOnlyNext() { m_mixerOnlyNext = true; }
    bool mixerOnlyPending() const { return m_mixerOnlyNext; }
    // keepSpeed: keep playing (leaving the multicam grid during playback)
    void showTimeline(int position, bool keepSpeed = false);
    void showSource(const QString& path, int position = 0);
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

    void play();
    void pause();
    void togglePlay();
    void setSpeed(double speed);
    double speed() const { return m_speed; }
    void seek(int frame);
    int position() const { return m_position; }

    // Before/After (Color page, like DaVinci "Bypass Color Grades"): preview without color correction; export untouched
    void setColorBypass(bool on);
    bool colorBypass() const { return m_gradeBypass->load(); }
    LoudnessReading loudness() const;
    void resetLoudness(); // restart Integrated/LRA (like the reset button in DaVinci)

    // Audio output (menu Playback → Audio output, saved in QSettings). Larger buffer against crackling/
    // dropouts (Windows/WASAPI needs more than PulseAudio), at the cost of slightly more delay. Takes effect from the next play.
    static int audioBuffer();
    void setAudioBuffer(int samples);
    // Audio driver of SDL (empty = automatic; Windows: "directsound" (default)/"wasapi"). Takes effect after restart.
    static QString audioDriver();
    static void setAudioDriver(const QString& driver);
    // Call before init(): sets the chosen driver for SDL (an already set environment variable takes precedence)
    static void applyAudioSettings();

signals:
    void frameReady(const QImage& image);
    void positionChanged(int frame);
    void speedChanged(double speed);
    void colorBypassChanged(bool on);
    void modeChanged(Engine::Mode mode);
    // Only during playback: peak levels in dBFS, [A1 L, A1 R, A2 L, …, Master L, Master R];
    // the master measures true peak (dBTP, BS.1770 4x oversampled) like the delivery check
    void audioLevels(const QVector<float>& db);
    // internal: position from the consumer thread, epoch = jump counter when displaying (older ones are discarded)
    void framePosition(int frame, int epoch);

private:
    bool createConsumer(QString* error);
    void connectProducer(Mlt::Producer* producer, int position);
    void refresh();
    void applyAudioState();
    void onFrameShown(void* mltFrame);
    void emitLevels();

    ProxyManager* m_proxies;
    RenderCache* m_renderCache;
    StillFetcher* m_stills;
    ProjectFormat m_format;
    std::unique_ptr<Mlt::Profile> m_profile;
    // The preview consumer's own copy: setting its width/height writes them into its profile (mlt_consumer.c), which
    // with a shared profile made every producer/filter take 960x540 for the project size (titles, positions in the
    // preview twice as large)
    std::unique_ptr<Mlt::Profile> m_consumerProfile;
    std::unique_ptr<ProducerFactory> m_factory; // source view (BT.601 detour, Profiles.h); outlives m_source
    std::unique_ptr<Mlt::Consumer> m_consumer;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Tractor> m_timeline;
    Timeline m_built; // model m_timeline was built from (LiveUpdate)
    std::unique_ptr<MixerHooks> m_mixer;
    std::unique_ptr<SharedLoudness> m_loudness; // fed by the master level meter in the audio thread
    std::unique_ptr<SeekFade> m_seekFade;       // seek while playing: fade out/in at the master (no click)
    std::unique_ptr<Preroll> m_preroll;         // Pre-position the decoder before jump spots
    std::mutex m_mixerMutex; // m_mixer is read in the consumer thread
    bool m_mixerOnlyNext = false;
    std::shared_ptr<std::atomic<bool>> m_gradeBypass = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<Mlt::Producer> m_source;
    // Multicam grid: own builder (the grid would otherwise clear the timeline's producer cache)
    std::unique_ptr<TimelineBuilder> m_gridBuilder;
    std::unique_ptr<Mlt::Tractor> m_grid;
    std::unique_ptr<MulticamGridHooks> m_gridHooks;
    Timeline m_gridContent;
    QVector<MulticamAngle> m_gridAngles;
    int m_gridColumns = 0, m_gridPage = 0;
    std::atomic<int> m_offset{0}; // timeline frame minus producer frame (Multicam mode, else 0)
    QString m_sourcePath;
    Mlt::Producer* m_current = nullptr;

    Mode m_mode = Mode::Timeline;
    double m_speed = 0.0;
    std::atomic<int> m_position{0};
    // Jump during playback: no longer show/report images from the buffer from before the jump
    std::atomic<int> m_seekEpoch{0};
    std::atomic<int> m_seekTarget{-1};
    std::atomic<int> m_seekSkipped{0};
    QSize m_previewSize{960, 540}; // project format scaled down (longer edge 960)

    friend struct EngineCallbacks;
};
