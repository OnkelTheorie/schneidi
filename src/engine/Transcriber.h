// SPDX-License-Identifier: AGPL-3.0-only
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
    QVector<int> audioStreams; // several streams of the media file mixed (e.g. two microphones); empty = audioStream
    Timeline timeline;
    ProjectFormat format;
    int from = 0, to = -1; // timeline frames; to < 0 = until the end
    QVector<int> audioTracks; // timeline: only these audio tracks (also muted ones), e.g. the voice without music; empty = as heard
    QString model;               // model file (Extensions::whisperModelPath)
    QString language = "auto";   // "de", "en", … or "auto"
    bool music = false;          // sung words and shouts in a song (no voice detection, see runWhisper)
    int threads = 0;             // 0 = Background::threads (setting, else half of the logical processors)
    // With a chosen language: afterwards let whisper detect the language too (one more short pass over the first
    // 30 s), so a wrong choice can be told (Transcriber::detectedLanguage)
    bool checkLanguage = false;
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
    // checkLanguage: what whisper detects ("en") and how sure it is (0..1); empty = not checked or not readable
    QString detectedLanguage() const { return m_detected; }
    double detectedProbability() const { return m_detectedP; }
    // checkLanguage: another language than the chosen one that whisper is sure of (kSureLanguage); empty = none. Only
    // a clear one is worth a word: German gamer talk with English bits was "en" at 55 % (detection hears only the
    // first 30 s), a really wrong choice 100 %
    static constexpr double kSureLanguage = 0.8;
    QString otherLanguage() const
    {
        return !m_detected.isEmpty() && m_detected != m_request.language && m_detectedP >= kSureLanguage ? m_detected
                                                                                                        : QString();
    }
    static int defaultThreads();

signals:
    void progress(double fraction); // 0..1
    void finished(bool ok, const QString& error); // ok: result() holds the words (times from the start of the source)

private:
    void runWhisper();
    void runDetection();
    void done(bool ok, const QString& error);

    TranscribeRequest m_request;
    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<Exporter> m_exporter;
    QProcess m_process;
    QByteArray m_log;
    bool m_running = false;
    bool m_cancelled = false;
    int m_stage = 0; // 0 = audio, 1 = whisper, 2 = language detection
    Transcript::Result m_result;
    QString m_detected;
    double m_detectedP = 0;
};
