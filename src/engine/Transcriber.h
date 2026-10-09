#pragma once
// Speech to text with whisper.cpp (engine/Extensions downloads it): audio of a media file (ffmpeg) or of the timeline
// (Exporter, mixed like the export) -> whisper-cli -> words with times (core/Transcript). Runs in the background.

#include "core/ProjectFormat.h"
#include "core/Transcript.h"
#include "core/Types.h"

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>

#include <memory>

class Exporter;

struct TranscribeRequest {
    // Source: a media file (audioStream = index of its audio streams) or, if media is empty, the timeline [from, to)
    QString media;
    int audioStream = 0;
    Timeline timeline;
    ProjectFormat format;
    int from = 0, to = -1; // timeline frames; to < 0 = until the end
    QString model;               // model file (Extensions::whisperModelPath)
    QString language = "auto";   // "de", "en", … or "auto"
    int threads = 0;             // 0 = half of the logical processors (the computer stays usable)
};

class Transcriber : public QObject {
    Q_OBJECT
public:
    explicit Transcriber(QObject* parent = nullptr);
    ~Transcriber() override;
    bool start(const TranscribeRequest& request, QString* error);
    void cancel();
    bool isRunning() const { return m_running; }
    const Transcript::Result& result() const { return m_result; }
    static int defaultThreads();

signals:
    void progress(double fraction); // 0..1
    void finished(bool ok, const QString& error); // ok: result() holds the words (times from the start of the source)

private:
    void runWhisper();
    void done(bool ok, const QString& error);

    TranscribeRequest m_request;
    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<Exporter> m_exporter;
    QProcess m_process;
    QByteArray m_log;
    bool m_running = false;
    bool m_cancelled = false;
    int m_stage = 0; // 0 = audio, 1 = whisper
    Transcript::Result m_result;
};
