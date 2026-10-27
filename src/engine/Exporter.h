#pragma once
#include "core/ProjectFormat.h"
#include "core/Types.h"
#include "engine/SmartRender.h"

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QVector>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <atomic>
#include <memory>

namespace Mlt {
class Profile;
class Consumer;
class Event;
class Tractor;
class Producer;
class Filter;
} // namespace Mlt
class TimelineBuilder;
class ExportWriter;
class QThread;

struct ExportSettings {
    QString path;
    ProjectFormat format; // project settings (frame rate, reference size for pixel values in the edit)
    QSize size;           // output size; empty = timeline resolution
    QString videoCodec = "libx264"; // empty = audio only (no video stream)
    // videoCodec is a hardware encoder (h264_nvenc …): software codec to use when it does not work (empty = none).
    // The hardware encoder is tested with the output size first (MLT crashes when it cannot open an encoder).
    QString softwareCodec;
    QString encoderNote; // note from choosing the encoder (e.g. not available here), reported with the result
    QString audioCodec = "aac"; // empty = no sound (GIF, image sequence)
    QString pixFmt = "yuv420p";
    // File: path is the file. Gif: renders a lossless intermediate (videoCodec/pixFmt, e.g. PNG in MOV) into the temp
    // folder, then ffmpeg turns it into path with its own palette. ImageSequence: path "…/name.png" writes
    // "…/name/name_00001.png" … (imageSequencePattern in core/RenderJob.h)
    enum class Output { File, Gif, ImageSequence };
    Output output = Output::File;
    QStringList codecOptions; // further consumer properties "key=value" (ProRes/DNxHR profile, VP9 speed …)
    int crf = 20;             // quality: smaller = better
    int maxBitrateK = 0;      // video bit rate cap (kbit/s), 0 = none
    QString preset = "medium"; // speed vs. file size
    int audioBitrateK = 192;
    QString audioSampleFormat; // empty = codec default (Apple Lossless: s16p/s32p for 16/24 bit)
    int audioRate = 0, audioChannels = 0; // 0 = 48 kHz stereo (speech to text wants 16 kHz mono)
    FrameRate rate{0, 1};      // frame rate of the file; num 0 = like the timeline (format.rate)
    bool burnSubtitles = false; // burn the visible subtitle track into the image
    QString subtitlePath;       // additionally write the visible subtitle track as SRT here (empty = not)
    int from = 0, to = -1; // range (frames, inclusive); to < 0 = until the end of the timeline
    int threads = 0;       // threads (logical processors) for rendering (images in parallel, decoder, encoder); 0 = all
    // Sound from the own audio engine (engine/MixedSound) or through MLT; Default = Audio::ownPath()
    enum class AudioPath { Default, Mlt, Own };
    AudioPath audioPath = AudioPath::Default;
    // Length of each media file in frames of the project rate (Project::media()), for the own audio engine: reversed
    // clips and transition handles mirror/stop at the length the project knows. Missing = taken from the file.
    QHash<QString, int> mediaLengths;
    // Who writes a video file: the own path (engine/ExportWriter: fetching and encoding in separate threads) or MLT's
    // avformat consumer. Default = ExportWriter::wanted(); formats the own path cannot write always go through MLT.
    enum class Writer { Default, Mlt, Own };
    Writer writer = Writer::Default;
    // Smart render (engine/SmartRender): copy what shows a file unchanged instead of encoding it (own path, H.264/H.265,
    // timeline size and frame rate, own audio engine). Falls back to encoding by itself.
    bool smartRender = false;
};

// Renders the timeline into a file. Own producer instances,
// so that preview and export do not get in each other's way.
class Exporter : public QObject {
    Q_OBJECT
public:
    explicit Exporter(QObject* parent = nullptr);
    ~Exporter() override;

    bool isRunning() const
    {
        return m_build != nullptr || m_consumer != nullptr || m_writer != nullptr || m_check != nullptr
            || m_analysis != nullptr || m_gif != nullptr;
    }
    // Is any export running right now? (render cache pauses meanwhile)
    static bool anyRunning();
    // Threads for rendering (Deliver page, applies per machine, QSettings "render/cores" – key stays for old settings): 0 = all
    static int savedThreads();
    static void setSavedThreads(int threads);
    static int availableThreads(); // logical processors (QThread::idealThreadCount) (at least 1)
    // How many images are rendered at once: threads (0 = all), but 1 if the timeline (also nested
    // sequences) uses a frei0r plugin (Open FX › frei0r, opt-in) – those are not thread-safe
    static int parallelFrames(const Timeline& tl, int threads);
    // Does the timeline (also nested sequences) read this file? Then the export must not overwrite it
    // (source and output destroyed) – refuse like DaVinci instead of asking.
    static bool readsFile(const Timeline& tl, const QString& path);
    // Do both paths point to the same existing file? (Windows: case does not matter)
    static bool sameFile(const QString& a, const QString& b);
    bool start(const Timeline& tl, const ExportSettings& settings, QString* error);
    void cancel();
    // After finished(): FFmpeg video codec that wrote the file (empty for audio only) and a note for the user
    // (e.g. the hardware encoder did not work and software was used; empty = nothing to say)
    QString usedCodec() const { return m_usedCodec; }
    QString note() const { return m_note; }
    // After finished(): share of the frames copied by smart render (0 = everything encoded)
    int copiedPercent() const { return m_copiedPercent; }

signals:
    void progress(int percent);
    // After start(): the graph is built (files opened, analyses done) and frames are being rendered. Until then the
    // export is being prepared in the background.
    void started();
    void finished(bool ok, const QString& message);

private:
    void poll();
    void cleanup();
    // After the build thread: test the hardware encoder or start the consumer; false = failed (error set)
    bool launch(QString* error);
    bool startConsumer(const QString& codec, QString* error);
    // Own export path (engine/ExportWriter) for this video codec? (settings, output kind, format support)
    bool useWriter(const QString& codec) const;
    bool startWriter(const QString& codec, QString* error);
    void pollWriter();
    // Encoder options as MLT consumer properties (the own writer takes the same names as AVOptions)
    QVector<QPair<QByteArray, QByteArray>> encoderOptions(const QString& codec) const;
    // Sample format of the audio encoder (what MLT's consumer would pick)
    QString audioSampleFormat() const;
    int audioBitrateK() const; // 0 = none for this codec (PCM, lossless)
    void onCheckFinished(bool works);
    // Hardware encoder failed: render again with ExportSettings::softwareCodec; false = no fallback possible
    bool fallBackToSoftware(QString* error);
    // Has the encoder written anything yet? (image sequences: the first frame)
    bool wroteOutput() const;
    // GIF: intermediate rendered -> ffmpeg palettegen/paletteuse into the target (m_gif)
    void convertToGif();
    void finishGif(bool ok, const QString& log);

    // Stabilization: missing analyses (engine/Stabilizer) run first in a thread, then start() again for real.
    // false = nothing to analyze
    bool runAnalyses(const Timeline& tl, const ExportSettings& s);

    QThread* m_analysis = nullptr;
    std::shared_ptr<std::atomic<bool>> m_analysisCancel;
    bool m_analysisDone = false; // start() after the analyses: do not look again
    Timeline m_pendingTimeline;
    ExportSettings m_pendingSettings;

    // MLT graph of the export, built in m_build (start), then moved into the members below
    struct Graph {
        std::unique_ptr<Mlt::Profile> profile;
        std::unique_ptr<TimelineBuilder> builder;
        std::unique_ptr<Mlt::Tractor> tractor;
        std::unique_ptr<Mlt::Profile> outProfile;
        std::unique_ptr<Mlt::Producer> source;
        std::unique_ptr<Mlt::Filter> sound; // own audio engine on source (engine/MixedSound)
        std::unique_ptr<Mlt::Producer> soundSource; // smart render: carries `sound` instead of source
        int length = 0;
        QString error;
    };
    QThread* m_build = nullptr;
    std::shared_ptr<std::atomic<bool>> m_buildCancel;
    void stopBuild(); // cancel and wait for m_build (only files being opened right now still finish)
    // Counts the build and analysis threads started and stopped. Stopping one right as it ends leaves its finished()
    // queued (disconnecting does not take back what is queued already): that one is stale and must do nothing.
    quint64 m_threadRun = 0;

    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Tractor> m_tractor;
    // Different frame rate than the timeline: m_source plays m_tractor in the output's profile (engine/RateConverter).
    // Otherwise m_source points to m_tractor.
    std::unique_ptr<Mlt::Profile> m_outProfile;
    std::unique_ptr<Mlt::Producer> m_source;
    std::unique_ptr<Mlt::Filter> m_sound; // own audio engine attached to m_source (smart render: to m_soundSource)
    std::unique_ptr<Mlt::Producer> m_soundSource;
    QVector<SmartRender::Span> m_copy; // smart render: what may be copied (empty = encode everything)
    int m_copiedPercent = 0;
    // Smart render failed as a whole: the sound goes back onto m_source, render again encoding everything
    bool smartFallback(QString* error);
    std::unique_ptr<Mlt::Consumer> m_consumer;
    std::unique_ptr<ExportWriter> m_writer; // instead of m_consumer (own export path)
    std::unique_ptr<Mlt::Event> m_stoppedEvent;
    std::unique_ptr<Mlt::Event> m_fatalEvent;
    std::atomic<bool> m_fatal{false}; // avformat reported a fatal error (e.g. the encoder failed while rendering)
    std::unique_ptr<QProcess> m_check; // test encode of the hardware encoder before the render starts
    std::unique_ptr<QProcess> m_gif;   // GIF: ffmpeg converts the intermediate m_path into m_target
    QString m_target;                  // file the user asked for (differs from m_path for GIF/image sequences)
    ExportSettings m_settings;
    bool m_convertRate = false;
    bool m_ownAudio = false;  // sound from engine/MixedSound
    QString m_sampleFormat;   // own audio: FFmpeg sample format of the audio encoder
    int m_threads = 0, m_frames = 1;
    QString m_usedCodec, m_note;
    std::atomic<bool> m_threadDone{false}; // encoder thread has ended ("consumer-stopped")
    int m_restarts = 0;
    QTimer m_timer;
    int m_length = 0;
    int m_from = 0;
    QString m_path;
};
