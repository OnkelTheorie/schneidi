// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/ProjectFormat.h"
#include "core/Subtitles.h"
#include "core/Types.h"
#include "engine/SmartRender.h"
#include "engine/picture/Renderer.h"

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QVector>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace Audio {
class Mixer;
}
class ExportWriter;
class QSaveFile;
class QThread;

struct ExportSettings {
    QString path;
    ProjectFormat format; // project settings (frame rate, reference size for pixel values in the edit)
    QSize size;           // output size; empty = timeline resolution
    QString videoCodec = "libx264"; // empty = audio only (no video stream)
    // videoCodec is a hardware encoder (h264_nvenc …): software codec to use when it does not work (empty = none).
    // A hardware encoder that cannot be opened (or fails while encoding) falls back to it by itself.
    QString softwareCodec;
    QString encoderNote; // note from choosing the encoder (e.g. not available here), reported with the result
    QString audioCodec = "aac"; // empty = no sound (GIF, image sequence)
    QString pixFmt = "yuv420p";
    // File: path is the file. Gif: renders a lossless intermediate (videoCodec/pixFmt, e.g. PNG in MOV) into the temp
    // folder, then ffmpeg turns it into path with its own palette. ImageSequence: path "…/name.png" writes
    // "…/name/name_00001.png" … (imageSequencePattern in core/RenderJob.h)
    enum class Output { File, Gif, ImageSequence };
    Output output = Output::File;
    QStringList codecOptions; // further encoder options "key=value" (ProRes/DNxHR profile, VP9 speed …)
    int crf = 20;             // quality: smaller = better
    int maxBitrateK = 0;      // video bit rate cap (kbit/s), 0 = none
    QString preset = "medium"; // speed vs. file size
    int audioBitrateK = 192;
    QString audioSampleFormat; // empty = codec default (Apple Lossless: s16p/s32p for 16/24 bit)
    int audioRate = 0, audioChannels = 0; // 0 = 48 kHz stereo (speech to text wants 16 kHz mono)
    FrameRate rate{0, 1};      // frame rate of the file; num 0 = like the timeline (format.rate)
    bool burnSubtitles = false; // burn the visible subtitle track into the image
    // Additionally write the visible subtitle track as SRT here, the other tracks with entries next to it
    // (Subtitles::srtFiles); empty = not
    QString subtitlePath;
    // Every subtitle track with entries as a text stream in the file (plain text; ExportWriter picks the codec by the
    // container: mov_text, SubRip or WebVTT; none in other containers)
    bool embedSubtitles = false;
    int from = 0, to = -1; // range (frames, inclusive); to < 0 = until the end of the timeline
    int threads = 0;       // threads (logical processors) for rendering (images in parallel, decoder, encoder); 0 = all
    // Length of each media file in frames of the project rate (Project::media()), for the audio mixer: reversed
    // clips and transition handles mirror/stop at the length the project knows. Missing = taken from the file.
    QHash<QString, int> mediaLengths;
    // Smart render (engine/SmartRender): copy what shows a file unchanged instead of encoding it (H.264/H.265,
    // timeline size and frame rate). Falls back to encoding by itself.
    bool smartRender = false;
    // Name part of the temporary files (8 hex digits: "<name>.<id>.part.<ext>", the GIF intermediate gifIntermediate());
    // empty = a random one. `serve` gives its own to remove exactly this render's files after killing its worker.
    QString partId;
    // false: the finished file never replaces one that appeared under its name while rendering (another render to the
    // same path that finished first): the export fails instead (targetTaken()). true = replace it (overwrite).
    bool replace = true;
    // Drawn onto every frame after compositing (Picture::Options::overlay): the marks of a review render
    // (engine/picture/ReviewOverlay). Empty for every normal export. Turns smart render off.
    std::function<void(uint8_t* rgba, int width, int height, int64_t frame)> overlay;
};

// Renders the timeline into a file (engine/ExportWriter with its own renderer and mixer, so that preview and export
// do not get in each other's way).
class Exporter : public QObject {
    Q_OBJECT
public:
    explicit Exporter(QObject* parent = nullptr);
    ~Exporter() override;

    bool isRunning() const
    {
        return m_build != nullptr || m_writer != nullptr || m_analysis != nullptr || m_gif != nullptr;
    }
    // Is any export running right now? (render cache pauses meanwhile)
    static bool anyRunning();
    // Threads for rendering (Deliver page, applies per machine, QSettings "render/cores" – key stays for old settings): 0 = all
    static int savedThreads();
    static void setSavedThreads(int threads);
    static int availableThreads(); // logical processors (QThread::idealThreadCount) (at least 1)
    // How many images are rendered at once: threads (0 = all), but 1 if a frame needs the one before (stabilizing
    // with border "keep")
    static int parallelFrames(const Timeline& tl, int threads);
    // Does the timeline (also nested sequences) read this file? Then the export must not overwrite it
    // (source and output destroyed) – refuse like DaVinci instead of asking.
    static bool readsFile(const Timeline& tl, const QString& path);
    // Do both paths point to the same existing file? (Windows: case does not matter)
    static bool sameFile(const QString& a, const QString& b);
    // The lossless intermediate of a GIF export with this part id (ExportSettings::partId), in the temp folder
    static QString gifIntermediate(const QString& partId);
    bool start(const Timeline& tl, const ExportSettings& settings, QString* error);
    void cancel();
    // After finished(): FFmpeg video codec that wrote the file (empty for audio only, "copy" when smart render copied
    // every frame) and a note for the user
    // (e.g. the hardware encoder did not work and software was used; empty = nothing to say)
    QString usedCodec() const { return m_usedCodec; }
    QString note() const { return m_note; }
    // After start(): effects the render leaves out because this version does not have them (frei0r/LADSPA plugins of
    // older projects, ids of newer versions); only those of what is written (picture, sound)
    const QVector<Picture::SkippedEffect>& skippedEffects() const { return m_skipped; }
    // After start(): processor threads the render uses (also caps the frames rendered at once)
    int threads() const { return m_threads; }
    // After finished(): share of the frames copied by smart render (0 = everything encoded)
    int copiedPercent() const { return m_copiedPercent; }
    // Frames handed to the encoder so far (0 before started())
    int framesWritten() const;

signals:
    void progress(int percent);
    // After start(): the graph is built (files opened, analyses done) and frames are being rendered. Until then the
    // export is being prepared in the background.
    void started();
    void finished(bool ok, const QString& message);

private:
    void poll();
    void cleanup();
    // After the build thread: start the writer; false = failed (error set)
    bool launch(QString* error);
    bool startWriter(const QString& codec, QString* error);
    void pollWriter();
    // Encoder options (AVOptions)
    QVector<QPair<QByteArray, QByteArray>> encoderOptions(const QString& codec) const;
    // Sample format of the audio encoder
    QString audioSampleFormat() const;
    int audioBitrateK() const; // 0 = none for this codec (PCM, lossless)
    // Hardware encoder failed: render again with ExportSettings::softwareCodec; false = no fallback possible
    bool fallBackToSoftware(QString* error);
    QString fallbackCodec(const QString& failed) const; // encoder to render with when `failed` fails; empty = none
    // Has the encoder written anything yet? (image sequences: the first frame)
    bool wroteOutput() const;
    // The export worked: move what was written under a temporary name (m_part) to the target the user asked for
    // (replacing an older file only now) and commit the SRT, then report it. Fails like fail() when that goes wrong.
    void succeed();
    bool commitOutput(QString* error);
    // The export failed or was canceled: stop everything, remove only what this export created (never an older file
    // of the target's name) and report it
    void fail(const QString& message);
    void discardOutput();
    // GIF: intermediate rendered -> ffmpeg palettegen/paletteuse into the target (m_gif)
    void convertToGif();
    void finishGif(bool ok, const QString& log);

    // Can the writer write this export? (output kind, encoders, formats)
    bool writable(const ExportSettings& s, const QString& sampleFormat, QString* why) const;
    QSize outputSize() const; // of the file
    double outputFps() const;

    // Stabilization: missing analyses (engine/Stabilizer) run first in a thread, then start() again for real.
    // false = nothing to analyze
    bool runAnalyses(const Timeline& tl, const ExportSettings& s);

    QThread* m_analysis = nullptr;
    std::shared_ptr<std::atomic<bool>> m_analysisCancel;
    bool m_analysisDone = false; // start() after the analyses: do not look again
    Timeline m_pendingTimeline;
    ExportSettings m_pendingSettings;

    // Renderer and mixer of the export, made in m_build (start), then moved into the members below
    struct Graph {
        std::shared_ptr<Picture::Renderer> renderer;
        std::shared_ptr<Audio::Mixer> mixer;
        int colorspace = 709;
        int length = 0;
        QString error;
    };
    QThread* m_build = nullptr;
    std::shared_ptr<std::atomic<bool>> m_buildCancel;
    void stopBuild(); // cancel and wait for m_build
    // Counts the build and analysis threads started and stopped. Stopping one right as it ends leaves its finished()
    // queued (disconnecting does not take back what is queued already): that one is stale and must do nothing.
    quint64 m_threadRun = 0;

    QVector<SmartRender::Span> m_copy; // smart render: what may be copied (empty = encode everything)
    int m_copiedPercent = 0;
    // Smart render failed as a whole: render again encoding everything
    bool smartFallback(QString* error);
    std::unique_ptr<ExportWriter> m_writer;
    std::unique_ptr<QProcess> m_gif;   // GIF: ffmpeg converts the intermediate m_path into m_target
    QString m_target;                  // file the user asked for (differs from m_path for GIF/image sequences)
    // What this export creates under a temporary name next to the target, renamed onto it only at success (a failed or
    // canceled export keeps an older file of that name): File = m_path, GIF = the converted file, image sequence = the
    // folder m_path writes into
    QString m_part;
    // SRT files next to the file (ExportSettings::subtitlePath and one per further track), committed at success
    std::vector<std::unique_ptr<QSaveFile>> m_subtitles;
    QVector<Subtitles::TextTrack> m_textTracks; // embedded subtitle streams (ExportSettings::embedSubtitles)
    ExportSettings m_settings;
    bool m_convertRate = false;
    std::shared_ptr<Picture::Renderer> m_renderer;
    std::shared_ptr<Audio::Mixer> m_mixer;
    QSize m_outSize;
    FrameRate m_outRate;
    int m_colorspace = 709;
    QString m_sampleFormat;   // FFmpeg sample format of the audio encoder
    int m_threads = 0, m_frames = 1;
    QString m_usedCodec, m_note;
    QVector<Picture::SkippedEffect> m_skipped;
    QTimer m_timer;
    int m_length = 0;
    int m_from = 0;
    QString m_path;
};
