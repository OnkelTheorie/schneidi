// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Text to speech with Piper (engine/Extensions downloads it and its voices): text -> piper (a separate process, text on
// stdin) -> a WAV file in the project's speech folder (core/Speech). The same text with the same voice and options
// is made only once (the file name carries the cache key). On request word times too: measured by whisper when it
// is installed, else estimated from the text (Speech::estimateWords); kept beside the WAV (<file>.words.json).
//
// Seam for other providers (a cloud voice later): everything outside this file only sees Request -> Result.

#include "core/Speech.h"
#include "core/Types.h"

#include <QObject>
#include <QProcess>

#include <functional>
#include <memory>
#include <optional>

class Transcriber;

namespace Tts {

struct Request {
    QString text;
    Speech::Options options;
    QString folder;     // where the file goes (Speech::folder of the project)
    bool words = false; // also word times (captions)
};

struct Result {
    QString path;           // the WAV
    bool cached = false;    // it was there already
    qint64 durationMs = 0;
    QVector<TranscriptWord> words; // ms from the file start (Request::words)
    QString wordSource;            // "whisper" (measured) or "estimate" (from the text)
    QString voice;                 // the voice used (id or path)
};

// The voice for an empty Options::voice: an installed one of the UI language, else the first installed; empty = none
QString defaultVoice();
// Error text for the user when speech cannot be made (no piper, no voice); empty = ready
QString notReady(const QString& voice = {});

// What a 16-bit PCM WAV holds (piper's output)
struct WavInfo {
    int sampleRate = 0, channels = 0;
    qint64 durationMs = 0;
    double peakDb = -120;       // highest sample
    qint64 speechFromMs = 0;    // first / last stretch louder than -40 dBFS (piper's silence before and after)
    qint64 speechToMs = 0;
};
std::optional<WavInfo> readWav(const QString& path);

class Speaker : public QObject {
    Q_OBJECT
public:
    explicit Speaker(QObject* parent = nullptr);
    ~Speaker() override;
    // false + error: cannot start (no piper, no voice, empty text, folder not writable). A file that is there already
    // finishes right away (cached, still through the signal).
    bool start(const Request& request, QString* error);
    void cancel();
    bool isRunning() const { return m_running; }
    const Result& result() const { return m_result; }

signals:
    void progress(double fraction);
    void finished(bool ok, const QString& error);

private:
    void piperDone(int exitCode, QProcess::ExitStatus status);
    void wordsDone(const QVector<TranscriptWord>& measured, const QString& source);
    bool startWords();
    void done(bool ok, const QString& error);

    Request m_request;
    Result m_result;
    QProcess m_process;
    std::unique_ptr<Transcriber> m_transcriber;
    QString m_part;
    bool m_running = false;
    bool m_cancelled = false;
};

// Blocking (an event loop until the speaker is done): for schneidi-cli. false + error on failure.
bool speak(const Request& request, Result* out, QString* error, const std::function<void(double)>& progress = {});

} // namespace Tts
