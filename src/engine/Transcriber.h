// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Speech to text with whisper.cpp (engine/Extensions downloads it): audio of a media file (ffmpeg) or of the timeline
// (Exporter, mixed like the export) -> whisper-cli -> words with times (core/Transcript). Runs in the background.

#include "core/ProjectFormat.h"
#include "core/Transcript.h"
#include "core/Types.h"

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QTemporaryDir>

#include <functional>
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
    QString model;               // model file (Extensions::whisperModelPath, or Parakeet: Extensions::parakeetModel)
    // With Parakeet as `model`: the whisper model for a language Parakeet does not know (detected or chosen); empty =
    // none (Parakeet writes it anyway, badly)
    QString whisperModel;
    QString language = "auto";   // "de", "en", … or "auto"
    bool music = false;          // sung words and shouts in a song (no voice detection, see runWhisper)
    int threads = 0;             // 0 = Background::threads (setting, else half of the logical processors)
    // With a chosen language: afterwards let whisper detect the language too (one more short pass over the first
    // 30 s), so a wrong choice can be told (Transcriber::detectedLanguage)
    bool checkLanguage = false;
    // Write the hesitation sounds too (äh, um, euh …): whisper leaves them out unless its starting text (the prompt)
    // has them (verbatimPrompt). For cutting filler words.
    bool verbatim = false;
    // Names, slang, game terms (core/Vocabulary): after the starting text, so whisper spells them like this. Parakeet
    // has no starting text and leaves them out (promptNote says so).
    QStringList vocabulary;
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
    // The starting text for TranscribeRequest::verbatim: a short line with that language's hesitation sounds ("auto"
    // or an unknown language: a mix of the common ones)
    static QString verbatimPrompt(const QString& language);
    // What whisper really gets: verbatimPrompt when `fits` (the code page whisper-cli reads its arguments in) holds it;
    // else for a Latin-script language the same sounds in plain ASCII, for other scripts nothing (*note says so).
    // Never a prompt that would arrive with '?' in it
    static QString verbatimPromptFor(const QString& language, const std::function<bool(const QString&)>& fits,
                                     QString* note = nullptr);
    // Windows: `text` can be written in the ANSI code page without a substitute character; elsewhere always true
    static bool fitsCodePage(const QString& text);
    // verbatim without a starting text (the code page could not hold it): why, for the answer; empty = none
    QString promptNote() const { return m_promptNote; }
    // The model file that wrote the result: the request's, or its whisperModel when Parakeet does not know the language
    QString usedModel() const { return m_model; }
    // Parakeet was asked for but whisper wrote it (or Parakeet wrote a language it does not know): why, for the user;
    // empty = as asked
    QString engineNote() const { return m_engineNote; }

    // NVIDIA Parakeet (parakeet-cli) instead of whisper: a model file named like it ("ggml-parakeet-tdt-0.6b-v3-…")
    static bool isParakeet(const QString& model);
    // The 25 European languages Parakeet TDT 0.6B v3 knows ("de", "en", …; not Turkish)
    static bool parakeetLanguage(const QString& language);

    // Progress between whisper's own updates (a 30 s clip jumped from 10 % to 100 % after a minute): estimated from
    // the time whisper has run against what it is expected to take (whisperSeconds). Follows the time up to 90 % of
    // it, then creeps on more and more slowly; never 1.
    static double heartbeat(double elapsed, double expected);
    // What whisper is expected to take for `audioSeconds` of sound with `model`: the speed measured on this computer
    // in earlier runs (settings, per model file), else a guess from the model's size. Short sounds cost as much as
    // 30 s (whisper works in 30 s windows).
    static double whisperSeconds(const QString& model, double audioSeconds);
    // A run took `seconds` for `audioSeconds`: remembered for whisperSeconds (mixed with the earlier measurements)
    static void measured(const QString& model, double audioSeconds, double seconds);
    // whisper's DTW preset for word times of a model file ("ggml-large-v3-q5_0.bin" -> "large.v3"); empty = unknown
    static QString dtwPreset(const QString& model);
    // Speech segments (ms) in the output of whisper-vad-speech-segments ("start = 151.00, end = 195.00", 1/100 s)
    static QVector<QPair<qint64, qint64>> parseSpeechSegments(const QString& output);
    // A short starting text with punctuation in the language (de, en; empty otherwise): without it whisper wrote whole
    // pieces of fast chat in lower case without a comma (measured 20.11.)
    static QString punctuationPrompt(const QString& language);
    // Loudness of a 16-bit PCM WAV (ffmpeg or the Exporter wrote it), channels mixed; empty if unreadable. Reads the
    // file in blocks (a 3 h sound is never held at once)
    static Transcript::Levels wavLevels(const QString& path);

signals:
    void progress(double fraction); // 0..1
    void finished(bool ok, const QString& error); // ok: result() holds the words (times from the start of the source)

private:
    void detectSpeech();     // stage 3: voice detection -> pieces (or straight to piecesReady without it)
    void piecesReady();      // after the voice detection: language first (auto, pieces) or the recognition
    void recognise();        // stage 1: runWhisper or runParakeet
    void runParakeet();
    bool useWhisperInstead(const QString& language); // Parakeet lacks it: whisperModel takes over (false = none)
    QString detectionModel() const; // the whisper model that tells the language
    bool writePieces(QString* error);
    void detectLanguageFirst(); // stage 4
    void runWhisper();
    void runDetection();
    bool readResult(QString* error); // whisper's output files -> m_result.words
    bool readParakeet(QString* error); // parakeet-cli's token times (m_output) -> m_result.words
    double whisperWork() const; // seconds of sound whisper works through (pieces: a window each)
    void done(bool ok, const QString& error);
    void beat();

    TranscribeRequest m_request;
    QString m_model;       // the model file that runs (m_request.model, or its whisperModel)
    bool m_parakeet = false; // m_model is Parakeet (parakeet-cli)
    QString m_engineNote;
    QByteArray m_output;   // parakeet-cli's whole log of stage 1 (its token times; m_log keeps only the end)
    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<Exporter> m_exporter;
    QProcess m_process;
    QByteArray m_log;
    bool m_running = false;
    bool m_cancelled = false;
    // 0 = audio, 3 = voice detection, 4 = language first (auto, pieces), 1 = whisper, 2 = language check
    int m_stage = 0;
    QString m_language;    // the language whisper is told: the request's, or the one detected first (auto + pieces)
    bool m_noDtw = false;  // a run with DTW word times failed (a model whose alignment heads do not fit): without
    bool m_usedDtw = false; // the whisper run under way has DTW times
    // The speech in pieces (Transcript::speechPieces, ms of audio.wav) whisper transcribes one by one in one run
    // (piece_000.wav …); empty = the whole sound at once.
    QVector<Transcript::Piece> m_pieces;
    QString m_prompt; // the starting text whisper got (its echo is removed)
    int m_repeats = 0; // words of repeated pieces readResult dropped (Transcript::dropRepeats)
    int m_piece = 0, m_lastPercent = 0; // piece whisper works on now (its progress restarts per file)
    QString m_speedModel; // model file (+ "@gpu"): the key of its measured speed (whisperSeconds)
    bool m_cpuOnly = false; // whisper failed on the graphics card: this request runs on the CPU (-ng)
    Transcript::Result m_result;
    QString m_promptNote;
    QString m_basePrompt; // m_prompt without the word list (an echo of the list alone is spoken words: kept)
    QString m_detected;
    double m_detectedP = 0;
    // Stage 1: whisper's own progress (0..1), what was shown, the time it runs and is expected to take
    QTimer m_beat;
    QElapsedTimer m_clock;
    double m_whisper = 0, m_shown = 0, m_audioSeconds = 0, m_expected = 0;
};
