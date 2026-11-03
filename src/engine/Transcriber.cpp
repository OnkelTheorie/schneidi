// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Transcriber.h"

#include "core/I18n.h"
#include "engine/Background.h"
#include "engine/Bundle.h"
#include "engine/Exporter.h"
#include "engine/Extensions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>

#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX // std::max/std::clamp
#endif
#include <windows.h>

#include <string>
#endif

namespace {
constexpr double kAudioShare = 0.1; // progress: preparing the audio, then whisper
constexpr double kWindow = 30;      // seconds whisper works on at once: a shorter sound costs as much

// A path whisper-cli can open: it reads its arguments in the ANSI code page on Windows (letters outside it became
// '?'). Windows: the short (8.3) name where the volume has one; elsewhere the path as it is.
QString narrowSafePath(const QString& path)
{
#ifdef Q_OS_WIN
    const std::wstring w = QDir::toNativeSeparators(path).toStdWString();
    DWORD n = GetShortPathNameW(w.c_str(), nullptr, 0);
    if (n == 0) return path;
    std::wstring out(n, L'\0');
    n = GetShortPathNameW(w.c_str(), out.data(), n);
    if (n == 0 || n >= out.size()) return path;
    out.resize(n);
    return QString::fromStdWString(out);
#else
    return path;
#endif
}

// Loudness of a 16-bit PCM WAV (ffmpeg or the Exporter wrote it), channels mixed; empty if unreadable
Transcript::Levels wavLevels(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray d = f.readAll();
    const auto u16 = [&](qsizetype at) { return quint16(uchar(d[at]) | uchar(d[at + 1]) << 8); };
    const auto u32 = [&](qsizetype at) { return quint32(u16(at)) | quint32(u16(at + 2)) << 16; };
    if (d.size() < 12 || !d.startsWith("RIFF") || d.mid(8, 4) != "WAVE") return {};
    int channels = 0, rate = 0, bits = 0;
    for (qsizetype at = 12; at + 8 <= d.size();) {
        const QByteArray id = d.mid(at, 4);
        const qsizetype size = u32(at + 4), body = at + 8;
        if (id == "fmt " && body + 16 <= d.size()) {
            channels = u16(body + 2);
            rate = int(u32(body + 4));
            bits = u16(body + 14);
        } else if (id == "data") {
            if (channels <= 0 || rate <= 0 || bits != 16) return {};
            const qsizetype bytes = std::min(size, d.size() - body);
            const qint64 frames = bytes / (2 * channels);
            const auto* s = reinterpret_cast<const qint16*>(d.constData() + body);
            QVector<qint16> mono(frames);
            for (qint64 i = 0; i < frames; ++i) {
                int sum = 0;
                for (int c = 0; c < channels; ++c) sum += s[i * channels + c];
                mono[i] = qint16(sum / channels);
            }
            return Transcript::levelsOf(mono.constData(), frames, rate);
        }
        at = body + size + (size & 1);
    }
    return {};
}
}

Transcriber::Transcriber(QObject* parent) : QObject(parent)
{
    // Whisper keeps every thread it gets busy for minutes: lowest priority, so music, games and the preview still
    // get through (at normal priority the user's music crackled); also for the ffmpeg that reads the sound first
    Background::lowerProcess(m_process, Background::Level::Lowest);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray data = m_process.readAllStandardError();
        m_log += data;
        if (m_log.size() > 64 * 1024) m_log = m_log.right(16 * 1024);
        if (m_stage != 1) return;
        // "whisper_print_progress_callback: progress =  45%"
        static const QRegularExpression re("progress\\s*=\\s*(\\d+)%");
        auto it = re.globalMatch(QString::fromUtf8(data));
        int percent = -1;
        while (it.hasNext()) percent = it.next().captured(1).toInt();
        if (percent < 0) return;
        m_whisper = std::max(m_whisper, percent / 100.0);
        beat();
    });
    m_beat.setInterval(250);
    connect(&m_beat, &QTimer::timeout, this, &Transcriber::beat);
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_running) return;
        if (m_cancelled) return done(false, T("Abgebrochen"));
        if (m_stage == 2) {
            // "auto-detected language: en (p = 0.976562)"; a failed check never fails the transcript
            static const QRegularExpression re("auto-detected language:\\s*([a-z]+)\\s*\\(p\\s*=\\s*([0-9.]+)\\)");
            if (const auto m = re.match(QString::fromUtf8(m_log)); m.hasMatch()) {
                m_detected = m.captured(1);
                m_detectedP = m.captured(2).toDouble();
            }
            return done(true, {});
        }
        const QString tail = QString::fromUtf8(m_log.right(400)).trimmed();
        if (status != QProcess::NormalExit || code != 0) {
            if (const QString hint = Extensions::missingRuntimeHint(code); m_stage > 0 && !hint.isEmpty())
                return done(false, hint);
            return done(false, m_stage == 0 ? T("Ton konnte nicht gelesen werden: %1").arg(tail)
                                            : T("Whisper ist fehlgeschlagen: %1").arg(tail));
        }
        if (m_stage == 0) return runWhisper();
        m_beat.stop();
        if (m_clock.isValid() && m_audioSeconds > 0) measured(m_request.model, m_audioSeconds, m_clock.elapsed() / 1000.0);
        QFile json(m_tmp->filePath("out.json"));
        if (!json.open(QIODevice::ReadOnly)) return done(false, T("Whisper hat kein Ergebnis geschrieben: %1").arg(tail));
        QString error;
        if (!Transcript::parseWhisperJson(json.readAll(), &m_result, &error)) return done(false, error);
        m_result.levels = wavLevels(m_tmp->filePath("audio.wav"));
        Transcript::fitToSpeech(m_result.words, m_result.levels);
        const QString lang = m_request.language.isEmpty() ? QString("auto") : m_request.language;
        if (m_request.checkLanguage && lang != QLatin1String("auto") && !m_result.words.isEmpty()) return runDetection();
        done(true, {});
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && m_running)
            done(false, T("%1 konnte nicht gestartet werden.").arg(m_process.program()));
    });
}

Transcriber::~Transcriber()
{
    m_running = false;
    if (m_exporter) m_exporter->cancel();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }
}

int Transcriber::defaultThreads() { return Background::threads(QThread::idealThreadCount() / 2); }

bool Transcriber::start(const TranscribeRequest& request, QString* error)
{
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (m_running) return fail("already running");
    if (Extensions::whisperProgram().isEmpty())
        return fail(T("Whisper ist nicht installiert (Arbeitsbereich → Erweiterungen)."));
    if (request.model.isEmpty() || !QFile::exists(request.model))
        return fail(T("Kein Whisper-Modell installiert (Arbeitsbereich → Erweiterungen)."));
    m_request = request;
    m_tmp = std::make_unique<QTemporaryDir>();
    if (!m_tmp->isValid()) return fail(m_tmp->errorString());
    m_result = {};
    m_detected.clear();
    m_detectedP = 0;
    m_log.clear();
    m_cancelled = false;
    m_stage = 0;
    const QString wav = m_tmp->filePath("audio.wav");
    if (!request.media.isEmpty()) {
        // 16 kHz mono like whisper wants it; only the chosen audio stream(s), several mixed at their own level
        QStringList args{"-hide_banner", "-nostdin", "-loglevel", "error", "-i", request.media};
        const QVector<int> streams = request.audioStreams.isEmpty() ? QVector<int>{request.audioStream} : request.audioStreams;
        if (streams.size() == 1) {
            args << "-map" << QString("0:a:%1").arg(streams.first());
        } else {
            QString graph;
            for (int s : streams) graph += QString("[0:a:%1]").arg(s);
            graph += QString("amix=inputs=%1:duration=longest:normalize=0[voice]").arg(streams.size());
            args << "-filter_complex" << graph << "-map" << "[voice]";
        }
        args << "-vn" << "-ac" << "1" << "-ar" << "16000" << "-c:a" << "pcm_s16le" << wav;
        m_process.start(Bundle::tool("ffmpeg"), args);
    } else {
        ExportSettings s;
        s.path = wav;
        s.format = request.format;
        s.videoCodec.clear();
        s.audioCodec = "pcm_s16le";
        s.audioRate = 16000; // what whisper reads; its own conversion of 48 kHz stereo recognises worse
        s.audioChannels = 1;
        s.from = request.from;
        s.to = request.to < 0 ? -1 : request.to - 1;
        s.threads = defaultThreads();
        Timeline tl = request.timeline;
        if (!request.audioTracks.isEmpty())
            for (int i = 0; i < tl.audio.size(); ++i) {
                tl.audio[i].muted = !request.audioTracks.contains(i);
                tl.audio[i].solo = false;
            }
        m_exporter = std::make_unique<Exporter>();
        connect(m_exporter.get(), &Exporter::progress, this, [this](int p) { emit progress(kAudioShare * p / 100.0); });
        connect(m_exporter.get(), &Exporter::finished, this, [this](bool ok, const QString& message) {
            if (!m_running) return;
            if (m_cancelled) return done(false, T("Abgebrochen"));
            if (!ok) return done(false, T("Ton der Timeline konnte nicht erzeugt werden: %1").arg(message));
            runWhisper();
        });
        QString exportError;
        if (!m_exporter->start(tl, s, &exportError)) return fail(exportError);
    }
    m_running = true;
    return true;
}

void Transcriber::runWhisper()
{
    m_stage = 1;
    emit progress(kAudioShare);
    // 16 kHz mono 16 bit: 32000 bytes a second after the 44-byte header
    m_audioSeconds = std::max<qint64>(0, QFileInfo(m_tmp->filePath("audio.wav")).size() - 44) / 32000.0;
    m_expected = whisperSeconds(m_request.model, m_audioSeconds);
    m_whisper = m_shown = 0;
    m_clock.start();
    m_beat.start();
    const int threads = m_request.threads > 0 ? m_request.threads : defaultThreads();
    // One word per segment (-ml 1 -sow): word times for cutting; Transcript::toCues builds subtitle lines from them
    // Files by name in whisper's working directory (the temporary folder): whisper-cli reads its arguments in the
    // ANSI code page on Windows, a temp folder under a user name with other letters became '?'
    m_process.setWorkingDirectory(m_tmp->path());
    QStringList args{"-m", narrowSafePath(m_request.model), "-f", "audio.wav", "-t", QString::number(threads), "-l",
                     m_request.language.isEmpty() ? QString("auto") : m_request.language, "-ml", "1", "-sow", "-oj",
                     "-of", "out", "-np", "-pp"};
    // Without voice detection whisper puts the first words of a passage at the start of a pause before it
    // Threshold 0.35 instead of 0.5: keeps quiet or laughed words (measured: "Diese Scheiß-Pferd" instead of "Das")
    // Music: voice detection hears no speech in a song (nothing found in a whole track, the shouted title and the
    // singing without it); without the previous text as context whisper does not loop on repeated lines
    if (m_request.music) args << "-mc" << "0";
    else if (const QString vad = Extensions::whisperVadModel(); !vad.isEmpty())
        args << "--vad" << "-vm" << narrowSafePath(vad) << "-vt" << "0.35";
    Extensions::provideRuntime(Extensions::whisperProgram()); // installed before the package carried it
    m_process.start(Extensions::whisperProgram(), args);
}

void Transcriber::runDetection()
{
    m_stage = 2;
    m_log.clear(); // the detected language is a log line of whisper (not with -np)
    const int threads = m_request.threads > 0 ? m_request.threads : defaultThreads();
    m_process.setWorkingDirectory(m_tmp->path());
    m_process.start(Extensions::whisperProgram(), {"-m", narrowSafePath(m_request.model), "-f", "audio.wav", "-t",
                                                    QString::number(threads), "-l", "auto", "-dl"});
}

// Whisper's own progress or the estimate from the time, whichever is further; never backwards
void Transcriber::beat()
{
    if (m_stage != 1) return;
    m_shown = std::max({m_shown, m_whisper, heartbeat(m_clock.elapsed() / 1000.0, m_expected)});
    // The language check after it gets the last bit
    const double end = m_request.checkLanguage ? 0.95 : 1.0;
    emit progress(kAudioShare + (end - kAudioShare) * m_shown);
}

double Transcriber::heartbeat(double elapsed, double expected)
{
    if (expected <= 0 || elapsed <= 0) return 0;
    const double r = elapsed / expected;
    if (r <= 0.9) return r;
    return 0.9 + 0.09 * (1 - std::exp(-(r - 0.9) / 0.5)); // slower than expected: up to 99 %
}

namespace {
QString speedKey(const QString& model) { return "transcribe/speed/" + QFileInfo(model).fileName(); }
}

double Transcriber::whisperSeconds(const QString& model, double audioSeconds)
{
    double perSecond = QSettings().value(speedKey(model)).toDouble();
    // Guess from the model file: tiny ~0.7, small ~1.8, large-v3-turbo ~4.3 s per second of sound
    if (perSecond <= 0) perSecond = 0.5 + QFileInfo(model).size() / (150.0 * 1024 * 1024);
    return perSecond * std::max(audioSeconds, kWindow);
}

void Transcriber::measured(const QString& model, double audioSeconds, double seconds)
{
    if (audioSeconds <= 0 || seconds <= 0) return;
    QSettings settings;
    const double now = seconds / std::max(audioSeconds, kWindow), before = settings.value(speedKey(model)).toDouble();
    settings.setValue(speedKey(model), before > 0 ? (before + now) / 2 : now);
}

void Transcriber::cancel()
{
    if (!m_running) return;
    m_cancelled = true;
    if (m_exporter && m_exporter->isRunning()) m_exporter->cancel();
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
}

void Transcriber::done(bool ok, const QString& error)
{
    m_running = false;
    m_beat.stop();
    if (m_exporter) m_exporter.release()->deleteLater(); // may be inside its own signal
    if (ok) emit progress(1.0);
    emit finished(ok, error);
}
