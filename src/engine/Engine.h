#pragma once
// The only place that knows MLT (together with TimelineBuilder).
// UI only talks via this class -> engine would be replaceable.

#include "core/ProjectFormat.h"
#include "core/Types.h"

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

namespace Mlt {
class Profile;
class Producer;
class Consumer;
class Tractor;
} // namespace Mlt
class TimelineBuilder;
class ProducerFactory;
class PreviewConsumer;
namespace Audio {
class Player;
}
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
    // Same without an Engine instance (own profile; usable from worker threads), image in the aspect ratio of `format`
    static QImage thumbnail(const ProjectFormat& format, const QString& path, int frame, const QSize& size);

    // Still (Grab Still) of the currently shown image (timeline resp. source) at full project resolution,
    // always from the originals (not from proxies)
    QImage grabStill(const Timeline& tl);

    void updateTimeline(const Timeline& tl); // model changed -> rebuild
    // Files the timeline needs that are not open yet are opened in a thread, the window stays usable; the newest
    // timeline is built afterwards (until then the Viewer shows the old picture, or black for other content).
    // Off (tests, CLI): updateTimeline() opens them itself and returns when the timeline is built.
    void setOpenInBackground(bool on) { m_openInBackground = on; }
    bool opening() const { return m_opening != nullptr; }
    // Lengths of the project's media in frames (Project::media()): the sound is mixed with them like in the export
    void setMediaLengths(const QHash<QString, int>& lengths);
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
    // Own audio engine while playing: the frame heard right now (fractional, in positions like position()); NaN
    // otherwise (stopped, sound through MLT). position() = the frame on screen, which follows it.
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
    // Audio driver (empty = automatic; Windows: "directsound" (default)/"wasapi"): SDL's on the MLT sound path (after a
    // restart), miniaudio's backend of the same name on the own one (from the next play)
    static QString audioDriver();
    static void setAudioDriver(const QString& driver);
    // Output device of the own sound engine (Audio::Device::Output::id, empty = system default); at once
    static QString audioOutput();
    void setAudioOutput(const QString& id);
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
    // Files are being opened in the background (setOpenInBackground) / done and built
    void openingChanged(bool opening);

private:
    bool createConsumer(QString* error);
    void connectProducer(Mlt::Producer* producer, int position);
    void refresh();
    // Moves the connected producer to `at` (its frames) while connected and drops what was rendered before
    void seekProducer(int at);
    void applyAudioState();
    void onFrameShown(void* mltFrame);
    void applyTimeline(const Timeline& tl, bool hinted);
    // Starts opening the files `picture` needs in a thread; false = all open (build now)
    bool openInBackground(const Timeline& picture, const Timeline& tl);
    void finishOpening(quint64 id);
    void stopOpening(); // cancels and waits (before the builder goes away)
    void emitLevels();
    // Own audio engine: the clock in frames of the connected producer, where playback of `frame` (producer frame)
    // starts in samples, and what the player plays in the current mode (positions = producer frames)
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
    struct Opening;
    std::unique_ptr<Opening> m_opening; // files being opened for m_builder (openInBackground)
    quint64 m_openings = 0;
    Timeline m_waiting;                 // newest timeline while opening
    bool m_waitingHinted = false;
    bool m_openInBackground = false;
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

    // Atomic: onFrameShown reads them on the consumer's thread while the UI thread switches mode/speed
    std::atomic<Mode> m_mode{Mode::Timeline};
    std::atomic<double> m_speed{0.0};
    std::atomic<int> m_position{0};
    // Jump during playback: no longer show/report images from the buffer from before the jump
    std::atomic<int> m_seekEpoch{0};
    std::atomic<int> m_seekTarget{-1};
    QElapsedTimer m_lastSeek;  // seeks in quick succession = dragging (SeekFade::hold)
    QTimer* m_seekRelease;     // no seek for a moment: dragging ended, sound fades in
    std::atomic<int> m_seekSkipped{0};
    QSize m_previewSize{960, 540}; // project format scaled down (longer edge 960)

    // Own audio engine (Audio::ownPath(), docs/archiv/plan-0.10-ton.md): the player plays the sound, the picture follows its
    // clock (m_preview is the consumer); the MLT graph has no sound. nullptr = sound through MLT (sdl2_audio).
    std::unique_ptr<Audio::Player> m_player;
    std::unique_ptr<PreviewConsumer> m_preview;
    Timeline m_sourceSound;  // source view: the file as a timeline
    int m_gridSoundAngle = 0; // multicam grid: whose sound plays

    friend struct EngineCallbacks;
};
