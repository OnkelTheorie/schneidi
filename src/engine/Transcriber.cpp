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
#include <QHash>
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

}

// Read block by block, never the whole file (3 h of 16 kHz = 345 MB, plus a mono copy: bad_alloc on a Pi right after
// whisper finished, review 25.11.): the chunk headers, then the data chunk in blocks of whole level windows
Transcript::Levels Transcriber::wavLevels(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const qint64 fileSize = f.size();
    const QByteArray riff = f.read(12);
    if (riff.size() < 12 || !riff.startsWith("RIFF") || riff.mid(8, 4) != "WAVE") return {};
    const auto u16 = [](const QByteArray& d, int at) { return quint16(uchar(d[at]) | uchar(d[at + 1]) << 8); };
    const auto u32 = [&](const QByteArray& d, int at) { return quint32(u16(d, at)) | quint32(u16(d, at + 2)) << 16; };
    int channels = 0, rate = 0, bits = 0;
    for (qint64 at = 12; at + 8 <= fileSize;) {
        if (!f.seek(at)) return {};
        const QByteArray h = f.read(8);
        if (h.size() < 8) return {};
        const qint64 size = u32(h, 4), body = at + 8;
        if (h.left(4) == "fmt " && body + 16 <= fileSize) {
            const QByteArray fmt = f.read(16);
            if (fmt.size() < 16) return {};
            channels = u16(fmt, 2);
            rate = int(u32(fmt, 4));
            bits = u16(fmt, 14);
        } else if (h.left(4) == "data") {
            if (channels <= 0 || rate <= 0 || bits != 16) return {};
            const qint64 frames = std::min(size, fileSize - body) / (2 * channels);
            Transcript::Levels levels;
            // Whole windows of levelsOf per block (1 s or so): the blocks' levels joined = the levels of all at once
            const qint64 window = std::max<qint64>(1, qint64(rate) * levels.frameMs / 1000);
            const qint64 blockFrames = window * 100;
            QVector<qint16> mono;
            for (qint64 done = 0; done < frames;) {
                const qint64 n = std::min(blockFrames, frames - done);
                const QByteArray block = f.read(n * 2 * channels);
                const qint64 got = block.size() / (2 * channels);
                if (got <= 0) break;
                const auto* s = reinterpret_cast<const qint16*>(block.constData());
                mono.resize(qsizetype(got));
                for (qint64 i = 0; i < got; ++i) {
                    int sum = 0;
                    for (int c = 0; c < channels; ++c) sum += s[i * channels + c];
                    mono[qsizetype(i)] = qint16(sum / channels);
                }
                levels.db += Transcript::levelsOf(mono.constData(), got, rate, levels.frameMs).db;
                done += got;
                if (got < n) break; // short read: the file ends early
            }
            return levels;
        }
        at = body + size + (size & 1);
    }
    return {};
}

Transcriber::Transcriber(QObject* parent) : QObject(parent)
{
    // Whisper keeps every thread it gets busy for minutes: lowest priority, so music, games and the preview still
    // get through (at normal priority the user's music crackled); also for the ffmpeg that reads the sound first
    // ... and it dies with this program (a killed schneidi-cli left whisper running for minutes)
    Background::lowerProcess(m_process, Background::Level::Lowest, true);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray data = m_process.readAllStandardError();
        m_log += data;
        if (m_log.size() > 64 * 1024) m_log = m_log.right(16 * 1024);
        if (m_stage != 1) return;
        if (m_parakeet) {
            // Its whole log holds the token times. No progress of its own: the files it has begun
            // ("Processing file: piece_003.wav"), looked for in the new data and the line before it
            const qsizetype from = std::max<qsizetype>(0, m_output.size() - 200);
            m_output += data;
            static const QRegularExpression file("Processing file:\\s*piece_(\\d+)\\.wav");
            for (auto f = file.globalMatch(QString::fromUtf8(m_output.mid(from))); f.hasNext();)
                m_piece = std::clamp(f.next().captured(1).toInt(), m_piece, std::max(0, int(m_pieces.size()) - 1));
            qint64 total = 0, before = 0;
            for (int i = 0; i < m_pieces.size(); ++i) {
                total += Transcript::pieceLength(m_pieces[i]);
                if (i < m_piece) before += Transcript::pieceLength(m_pieces[i]);
            }
            if (total > 0) m_whisper = std::max(m_whisper, before / double(total));
            beat();
            return;
        }
        // "whisper_print_progress_callback: progress =  45%"
        static const QRegularExpression re("progress\\s*=\\s*(\\d+)%");
        auto it = re.globalMatch(QString::fromUtf8(data));
        int percent = -1;
        while (it.hasNext()) percent = it.next().captured(1).toInt();
        if (percent < 0) return;
        if (m_pieces.size() > 1) {
            // One run over all pieces: whisper's progress starts again at each file
            // whisper prints from 5 % on and often only 100 % for a short piece: a lower value or a 100 % before
            // it means the next file
            if ((percent < m_lastPercent || m_lastPercent >= 100) && m_piece + 1 < m_pieces.size()) ++m_piece;
            m_lastPercent = percent;
            qint64 total = 0, before = 0;
            for (int i = 0; i < m_pieces.size(); ++i) {
                const qint64 len = Transcript::pieceLength(m_pieces[i]);
                total += len;
                if (i < m_piece) before += len;
            }
            const qint64 now = Transcript::pieceLength(m_pieces[m_piece]);
            m_whisper = std::max(m_whisper, total > 0 ? (before + now * percent / 100.0) / double(total) : 0.0);
        } else {
            m_whisper = std::max(m_whisper, percent / 100.0);
        }
        beat();
    });
    m_beat.setInterval(250);
    connect(&m_beat, &QTimer::timeout, this, &Transcriber::beat);
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_running) return;
        if (m_cancelled) return done(false, T("Abgebrochen"));
        if (m_stage == 3) {
            // Voice detection: speech segments -> pieces; anything wrong with it -> the whole sound at once
            const QString out = QString::fromUtf8(m_process.readAllStandardOutput());
            const QVector<QPair<qint64, qint64>> segments =
                status == QProcess::NormalExit && code == 0 ? parseSpeechSegments(out) : QVector<QPair<qint64, qint64>>{};
            const qint64 lengthMs = qint64(m_audioSeconds * 1000);
            m_pieces = segments.isEmpty() ? QVector<Transcript::Piece>{} : Transcript::speechPieces(segments, lengthMs);
            QString error;
            if (!m_pieces.isEmpty() && !writePieces(&error)) {
                qInfo("Whisper: pieces not written (%s), the whole sound at once", qPrintable(error));
                m_pieces.clear();
            }
            return piecesReady();
        }
        if (m_stage == 4) {
            static const QRegularExpression re("auto-detected language:\\s*([a-z]+)");
            if (const auto m = re.match(QString::fromUtf8(m_log)); m.hasMatch()) m_language = m.captured(1);
            // Parakeet and a language it does not know: whisper instead (if there is one)
            if (m_parakeet && m_language != QLatin1String("auto") && !parakeetLanguage(m_language))
                useWhisperInstead(m_language);
            return recognise(); // nothing detected: auto as before
        }
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
            // On the graphics card (a driver that crashes, too little video memory): once more on the CPU
            if (m_stage == 1 && !m_cpuOnly && !Extensions::whisperGpuBackend().isEmpty()) {
                qInfo("%s failed on the GPU (exit %d), again on the CPU: %s", m_parakeet ? "Parakeet" : "Whisper", code,
                      qPrintable(tail.right(200)));
                m_cpuOnly = true;
                m_log.clear();
                return recognise();
            }
            // With DTW word times (a model file named like one whose alignment heads it lacks): once more without
            if (m_stage == 1 && m_usedDtw && !m_noDtw) {
                qInfo("Whisper failed with DTW word times (exit %d), again without: %s", code, qPrintable(tail.right(200)));
                m_noDtw = true;
                m_log.clear();
                return runWhisper();
            }
            return done(false, m_stage == 0   ? T("Ton konnte nicht gelesen werden: %1").arg(tail)
                               : m_parakeet ? T("Parakeet ist fehlgeschlagen: %1").arg(tail)
                                            : T("Whisper ist fehlgeschlagen: %1").arg(tail));
        }
        if (m_stage == 0) return detectSpeech();
        m_beat.stop();
        if (m_clock.isValid() && m_audioSeconds > 0) measured(m_speedModel, whisperWork(), m_clock.elapsed() / 1000.0);
        QString error;
        if (!readResult(&error)) return done(false, error.isEmpty() ? T("Whisper hat kein Ergebnis geschrieben: %1").arg(tail) : error);
        m_result.levels = wavLevels(m_tmp->filePath("audio.wav"));
        // Silence: whatever whisper wrote is made up; else its well-known made-up lines go
        if (Transcript::isSilent(m_result.levels)) {
            m_result.dropped = int(m_result.words.size());
            m_result.words.clear();
        } else {
            m_result.dropped = Transcript::dropHallucinations(m_result.words, m_result.levels);
        }
        m_result.dropped += m_repeats;
        if (!m_prompt.isEmpty() && !m_request.verbatim) m_result.dropped += Transcript::dropPromptEcho(m_result.words, m_prompt);
        Transcript::fitToSpeech(m_result.words, m_result.levels);
        const QString lang = m_request.language.isEmpty() ? QString("auto") : m_request.language;
        // A chosen language is what whisper wrote in; it reports "en" when it heard nothing. Parakeet reports none:
        // the one detected first
        if (lang != QLatin1String("auto")) m_result.language = lang;
        else if (m_result.language.isEmpty() && m_language != QLatin1String("auto")) m_result.language = m_language;
        // Not after Parakeet: it hears every language it knows without being told, a wrong choice changes nothing
        if (m_request.checkLanguage && lang != QLatin1String("auto") && !m_result.words.isEmpty() && !m_parakeet)
            return runDetection();
        done(true, {});
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        // Queued: QProcess can report a failed start inside start() itself (Windows: CreateProcess; Linux: a working
        // directory that is gone). Before Transcriber::start returned, that was swallowed (m_running still false)
        // and the caller waited for finished() forever; from there finished() came before start() returned true.
        if (e == QProcess::FailedToStart && m_running)
            QTimer::singleShot(0, this, [this, program = m_process.program(), stage = m_stage] {
                if (!m_running) return;
                // The voice detection or the first language detection are helpers: without them, as before
                if (stage == 3) {
                    m_pieces.clear();
                    return piecesReady();
                }
                if (stage == 4) return recognise();
                done(false, T("%1 konnte nicht gestartet werden.").arg(program));
            });
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
    if (isParakeet(request.model)) {
        if (Extensions::parakeetProgram().isEmpty())
            return fail(T("Für „Sehr schnell“ (Parakeet) fehlt das Programm parakeet-cli aus der Erweiterung „Whisper“ "
                          "(Arbeitsbereich → Erweiterungen)."));
        if (!QFile::exists(request.model))
            return fail(T("Das Modell „Sehr schnell“ (Parakeet) ist nicht installiert (Arbeitsbereich → Erweiterungen)."));
    } else {
        if (Extensions::whisperProgram().isEmpty())
            return fail(T("Whisper ist nicht installiert (Arbeitsbereich → Erweiterungen)."));
        if (request.model.isEmpty() || !QFile::exists(request.model))
            return fail(T("Kein Whisper-Modell installiert (Arbeitsbereich → Erweiterungen)."));
    }
    m_request = request;
    m_model = request.model;
    m_parakeet = isParakeet(request.model);
    m_engineNote.clear();
    m_output.clear();
    m_tmp = std::make_unique<QTemporaryDir>();
    if (!m_tmp->isValid()) return fail(m_tmp->errorString());
    m_result = {};
    m_detected.clear();
    m_detectedP = 0;
    m_log.clear();
    m_cancelled = false;
    m_stage = 0;
    m_cpuOnly = false;
    m_noDtw = false;
    m_pieces.clear();
    m_language = request.language.isEmpty() ? QString("auto") : request.language;
    // A chosen language Parakeet does not know: whisper from the start (or Parakeet with a note)
    if (m_parakeet && m_language != QLatin1String("auto") && !parakeetLanguage(m_language)) useWhisperInstead(m_language);
    m_running = true; // a failed start below is reported through finished() (see errorOccurred)
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
        // Not in whisper's folder of the run before (runWhisper): that temporary folder is gone, and a process
        // cannot start in it - every second transcription of a file with the same Transcriber failed
        m_process.setWorkingDirectory(QString());
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
            detectSpeech();
        });
        QString exportError;
        if (!m_exporter->start(tl, s, &exportError)) {
            m_running = false;
            m_exporter.reset();
            return fail(exportError);
        }
    }
    return true;
}

void Transcriber::detectSpeech()
{
    // 16 kHz mono 16 bit: 32000 bytes a second after the 44-byte header
    m_audioSeconds = std::max<qint64>(0, QFileInfo(m_tmp->filePath("audio.wav")).size() - 44) / 32000.0;
    m_pieces.clear();
    const QString program = Extensions::whisperVadProgram(), vad = Extensions::whisperVadModel();
    // Music: voice detection hears no speech in a song (see runWhisper); without the program or its model: at once
    if (m_request.music || program.isEmpty() || vad.isEmpty() || !QFile::exists(vad)) return piecesReady();
    m_stage = 3;
    m_log.clear();
    m_process.setWorkingDirectory(m_tmp->path());
    Extensions::provideRuntime(program);
    // Threshold 0.35 instead of 0.5: keeps quiet or laughed words (measured: "Diese Scheiß-Pferd" instead of "Das");
    // a segment longer than 25 s is split (a piece stays within whisper's 30 s window)
    const int threads = m_request.threads > 0 ? m_request.threads : defaultThreads();
    m_process.start(program, {"-vm", narrowSafePath(vad), "-f", "audio.wav", "-vt", "0.35", "-vmsd", "25", "-t",
                              QString::number(std::min(threads, 4)), "-np"});
}

bool Transcriber::writePieces(QString* error)
{
    // Read piece by piece, never the whole sound (3 h of 16 kHz = 345 MB in one QByteArray: bad_alloc on a Pi, review
    // 21.11.): the WAV's chunk headers, then each span of the data chunk where it lies
    QFile in(m_tmp->filePath("audio.wav"));
    if (!in.open(QIODevice::ReadOnly)) {
        *error = in.errorString();
        return false;
    }
    const qint64 fileSize = in.size();
    if (in.read(12).left(4) != "RIFF") {
        *error = "no PCM data";
        return false;
    }
    // The data chunk of the 16 kHz mono 16-bit WAV (ffmpeg or the Exporter wrote it)
    qint64 data = -1, size = 0;
    for (qint64 at = 12; at + 8 <= fileSize;) {
        if (!in.seek(at)) break;
        const QByteArray h = in.read(8);
        if (h.size() < 8) break;
        const qint64 len = qint64(quint32(uchar(h[4]) | uchar(h[5]) << 8 | uchar(h[6]) << 16 | quint32(uchar(h[7])) << 24));
        if (h.left(4) == "data") {
            data = at + 8;
            size = std::min(len, fileSize - data);
            break;
        }
        at += 8 + len + (len & 1);
    }
    if (data < 0) {
        *error = "no PCM data";
        return false;
    }
    constexpr qint64 kBytesPerMs = 32; // 16000 samples/s, 2 bytes
    for (int i = 0; i < m_pieces.size(); ++i) {
        // The stretches one after the other, silence (zeros) for the cut-short pauses between them
        QByteArray pcm(qsizetype(Transcript::pieceLength(m_pieces[i]) * kBytesPerMs), '\0');
        for (const Transcript::PieceSpan& s : m_pieces[i]) {
            const qint64 from = std::min<qint64>(s.from * kBytesPerMs, size);
            const qint64 to = std::min<qint64>((s.from + s.length) * kBytesPerMs, size);
            if (to <= from) continue;
            if (!in.seek(data + from)) {
                *error = in.errorString();
                return false;
            }
            const QByteArray span = in.read(to - from);
            if (span.size() != to - from) {
                *error = "short read: " + in.errorString();
                return false;
            }
            pcm.replace(qsizetype(s.at * kBytesPerMs), span.size(), span);
        }
        QByteArray header("RIFF");
        const auto u32 = [&header](quint32 v) { for (int k = 0; k < 4; ++k) header += char((v >> (8 * k)) & 0xff); };
        const auto u16 = [&header](quint16 v) { header += char(v & 0xff); header += char(v >> 8); };
        u32(quint32(36 + pcm.size()));
        header += "WAVEfmt ";
        u32(16);
        u16(1);     // PCM
        u16(1);     // mono
        u32(16000); // rate
        u32(32000); // bytes per second
        u16(2);     // block align
        u16(16);    // bits
        header += "data";
        u32(quint32(pcm.size()));
        QFile out(m_tmp->filePath(QString("piece_%1.wav").arg(i, 3, 10, QChar('0'))));
        if (!out.open(QIODevice::WriteOnly) || out.write(header + pcm) != header.size() + pcm.size()) {
            *error = out.errorString();
            return false;
        }
    }
    return true;
}

double Transcriber::whisperWork() const
{
    if (m_pieces.size() <= 1) return m_audioSeconds;
    if (m_parakeet) { // no 30 s windows: the speech it hears
        qint64 ms = 0;
        for (const auto& p : m_pieces) ms += Transcript::pieceLength(p);
        return ms / 1000.0;
    }
    // Every piece costs about one 30 s window, however short
    double work = 0;
    for (const auto& p : m_pieces) work += std::max(Transcript::pieceLength(p) / 1000.0, kWindow);
    return work;
}

void Transcriber::detectLanguageFirst()
{
    m_stage = 4;
    m_log.clear();
    int longest = 0;
    for (int i = 1; i < m_pieces.size(); ++i)
        if (Transcript::pieceLength(m_pieces[i]) > Transcript::pieceLength(m_pieces[longest])) longest = i;
    const int threads = m_request.threads > 0 ? m_request.threads : defaultThreads();
    QStringList args{"-m", narrowSafePath(detectionModel()), "-f", QString("piece_%1.wav").arg(longest, 3, 10, QChar('0')),
                     "-t", QString::number(threads), "-l", "auto", "-dl"};
    if (m_cpuOnly) args << "-ng";
    m_process.setWorkingDirectory(m_tmp->path());
    Extensions::provideRuntime(Extensions::whisperProgram());
    m_process.start(Extensions::whisperProgram(), args);
}

void Transcriber::runWhisper()
{
    m_stage = 1;
    emit progress(kAudioShare);
    m_audioSeconds = std::max<qint64>(0, QFileInfo(m_tmp->filePath("audio.wav")).size() - 44) / 32000.0;
    // The speed is remembered apart for the graphics card (about 10x faster than the CPU)
    m_speedModel = m_model + (!m_cpuOnly && !Extensions::whisperGpuBackend().isEmpty() ? "@gpu" : "");
    m_expected = whisperSeconds(m_speedModel, whisperWork());
    m_whisper = m_shown = 0;
    m_piece = m_lastPercent = 0;
    m_clock.start();
    m_beat.start();
    const int threads = m_request.threads > 0 ? m_request.threads : defaultThreads();
    // One word per segment (-ml 1 -sow): word times for cutting; Transcript::toCues builds subtitle lines from them
    // Files by name in whisper's working directory (the temporary folder): whisper-cli reads its arguments in the
    // ANSI code page on Windows, a temp folder under a user name with other letters became '?'
    m_process.setWorkingDirectory(m_tmp->path());
    QStringList args{"-m", narrowSafePath(m_model), "-t", QString::number(threads), "-l", m_language, "-ml", "1",
                     "-sow", "-np", "-pp"};
    // Word times from DTW (the attention of the model's alignment heads; needs flash attention off): whisper's own
    // times hand a laugh or pause before a sentence to its first word ("Was bist du?" 2.4 s early, measured 20.11.)
    // Not with whisper's own voice detection (the whole sound at once): its DTW times count in the sound with the pauses
    // cut out and are not mapped back (a 6 min track came out squeezed to two thirds, measured 20.11.)
    const QString vadModel = Extensions::whisperVadModel();
    const bool ownVad = m_pieces.isEmpty() && !m_request.music && !vadModel.isEmpty();
    const QString dtw = m_noDtw || ownVad ? QString() : dtwPreset(m_model);
    m_usedDtw = !dtw.isEmpty();
    if (m_usedDtw) args << "-ojf" << "-nfa" << "-dtw" << dtw;
    else args << "-oj";
    // Verbatim: a starting text full of hesitation sounds, so whisper keeps writing them (it leaves them out otherwise)
    m_promptNote.clear();
    m_prompt.clear();
    if (m_request.verbatim) m_prompt = verbatimPromptFor(m_language, fitsCodePage, &m_promptNote);
    else if (const QString p = punctuationPrompt(m_language); !p.isEmpty() && fitsCodePage(p)) m_prompt = p;
    if (!m_prompt.isEmpty()) args << "--prompt" << m_prompt;
    if (m_cpuOnly) args << "-ng";
    const QString gpu = m_cpuOnly ? QString() : Extensions::whisperGpuBackend();
    qInfo("Whisper: %s, %s, %s", qPrintable(QFileInfo(m_model).fileName()),
          gpu.isEmpty() ? "CPU" : qPrintable("GPU (" + QFileInfo(gpu).fileName() + ")"),
          m_pieces.isEmpty() ? "the whole sound" : qPrintable(QString("%1 pieces of speech").arg(m_pieces.size())));
    if (!m_pieces.isEmpty()) {
        // Speech pieces, each within whisper's 30 s window (the whole sound at once skipped a sentence now and then,
        // and its voice detection glued the speech together: times mapped back across the cut-out pauses drifted);
        // one run, the model loaded once; whisper writes piece_000.wav.json …
        for (int i = 0; i < m_pieces.size(); ++i) args << QString("piece_%1.wav").arg(i, 3, 10, QChar('0'));
    } else {
        args << "-f" << "audio.wav" << "-of" << "out";
        // Without voice detection whisper puts the first words of a passage at the start of a pause before it
        // Music: voice detection hears no speech in a song (nothing found in a whole track, the shouted title and the
        // singing without it); without the previous text as context whisper does not loop on repeated lines
        if (m_request.music) args << "-mc" << "0";
        else if (ownVad) args << "--vad" << "-vm" << narrowSafePath(vadModel) << "-vt" << "0.35";
    }
    Extensions::provideRuntime(Extensions::whisperProgram()); // installed before the package carried it
    m_process.start(Extensions::whisperProgram(), args);
}

void Transcriber::piecesReady()
{
    // Parakeet has no voice detection of its own and hears a long sound at once (its memory grows with it): without
    // the voice detection's pieces, the sound in pieces of up to 28 s
    if (m_parakeet && m_pieces.isEmpty() && m_audioSeconds > 0) {
        const qint64 lengthMs = qint64(m_audioSeconds * 1000);
        m_pieces = Transcript::speechPieces({qMakePair(qint64(0), lengthMs)}, lengthMs);
        QString error;
        if (!writePieces(&error)) return done(false, T("Ton konnte nicht gelesen werden: %1").arg(error));
    }
    // Automatic language with pieces: whisper would detect it again on every short piece (a laugh heard as another
    // language): once first, on the longest piece. Parakeet: whether it knows the language (a whisper model tells it)
    if (!m_pieces.isEmpty() && m_language == QLatin1String("auto") && !detectionModel().isEmpty())
        return detectLanguageFirst();
    recognise();
}

void Transcriber::recognise()
{
    if (m_parakeet) runParakeet();
    else runWhisper();
}

QString Transcriber::detectionModel() const
{
    if (!m_parakeet) return m_model;
    if (Extensions::whisperProgram().isEmpty()) return {};
    // The smallest whisper model there is (the language of the first 30 s; tiny is enough for that), else the fallback
    if (const QString small = Extensions::detectionWhisperModel(); !small.isEmpty()) return small;
    return QFile::exists(m_request.whisperModel) ? m_request.whisperModel : QString();
}

bool Transcriber::useWhisperInstead(const QString& language)
{
    if (m_request.whisperModel.isEmpty() || !QFile::exists(m_request.whisperModel)
        || isParakeet(m_request.whisperModel) || Extensions::whisperProgram().isEmpty()) {
        m_engineNote = T("Parakeet kennt die Sprache „%1“ nicht – das Ergebnis kann unbrauchbar sein. Besser "
                         "„Genau“ oder „Schnell“ wählen.").arg(language);
        return false;
    }
    m_engineNote = T("Parakeet kennt die Sprache „%1“ nicht – Whisper hat sie erkannt.").arg(language);
    qInfo("Parakeet does not know '%s': whisper (%s) instead", qPrintable(language),
          qPrintable(QFileInfo(m_request.whisperModel).fileName()));
    m_model = m_request.whisperModel;
    m_parakeet = false;
    return true;
}

bool Transcriber::isParakeet(const QString& model)
{
    return QFileInfo(model).fileName().contains(QLatin1String("parakeet"), Qt::CaseInsensitive);
}

bool Transcriber::parakeetLanguage(const QString& language)
{
    // Parakeet TDT 0.6B v3 (model card): bg hr cs da nl en et fi fr de el hu it lv lt mt pl pt ro sk sl es sv ru uk
    static const QStringList known{"bg", "hr", "cs", "da", "nl", "en", "et", "fi", "fr", "de", "el", "hu", "it",
                                   "lv", "lt", "mt", "pl", "pt", "ro", "sk", "sl", "es", "sv", "ru", "uk"};
    return known.contains(language.trimmed().toLower().left(2));
}

void Transcriber::runParakeet()
{
    m_stage = 1;
    emit progress(kAudioShare);
    m_audioSeconds = std::max<qint64>(0, QFileInfo(m_tmp->filePath("audio.wav")).size() - 44) / 32000.0;
    const QString program = Extensions::parakeetProgram();
    m_speedModel = m_model + (!m_cpuOnly && !Extensions::whisperGpuBackend().isEmpty() ? "@gpu" : "");
    m_expected = whisperSeconds(m_speedModel, whisperWork());
    m_whisper = m_shown = 0;
    m_piece = m_lastPercent = 0;
    m_output.clear();
    m_prompt.clear(); // no starting text, no DTW: its own token times
    m_promptNote.clear();
    m_usedDtw = false;
    m_clock.start();
    m_beat.start();
    const int threads = m_request.threads > 0 ? m_request.threads : defaultThreads();
    // Token times (-ps, on stderr; 10 ms steps) for the words; one run over all pieces, the model loaded once
    m_process.setWorkingDirectory(m_tmp->path());
    QStringList args{"-m", narrowSafePath(m_model), "-t", QString::number(threads), "-ps"};
    if (m_cpuOnly) args << "-ng";
    for (int i = 0; i < m_pieces.size(); ++i) args << QString("piece_%1.wav").arg(i, 3, 10, QChar('0'));
    if (m_pieces.isEmpty()) args << "audio.wav"; // no sound at all
    const QString gpu = m_cpuOnly ? QString() : Extensions::whisperGpuBackend();
    qInfo("Parakeet: %s, %s, %d pieces", qPrintable(QFileInfo(m_model).fileName()),
          gpu.isEmpty() ? "CPU" : qPrintable("GPU (" + QFileInfo(gpu).fileName() + ")"), int(m_pieces.size()));
    Extensions::provideRuntime(program);
    m_process.start(program, args);
}

bool Transcriber::readParakeet(QString* error)
{
    const QHash<QString, QVector<Transcript::Word>> files = Transcript::parseParakeetLog(QString::fromUtf8(m_output));
    if (files.isEmpty()) {
        if (error) *error = T("Parakeet hat kein Ergebnis geschrieben: %1").arg(QString::fromUtf8(m_log.right(400)).trimmed());
        return false;
    }
    // No repeat filter: Parakeet does not write a piece twice, and its blind 28 s pieces can hold a real repeat
    if (m_pieces.isEmpty()) {
        m_result.words = files.value("audio.wav");
        return true;
    }
    for (int i = 0; i < m_pieces.size(); ++i) {
        const Transcript::Piece& piece = m_pieces[i];
        for (const Transcript::Word& w : files.value(QString("piece_%1.wav").arg(i, 3, 10, QChar('0')))) {
            Transcript::Word mapped{Transcript::pieceTime(piece, w.from), Transcript::pieceTime(piece, w.to, true), w.text};
            if (!m_result.words.isEmpty()) mapped.from = std::max(mapped.from, m_result.words.last().from);
            mapped.to = std::max(mapped.to, mapped.from);
            m_result.words << mapped;
        }
    }
    return true;
}

bool Transcriber::readResult(QString* error)
{
    m_result.words.clear();
    m_result.language.clear();
    m_repeats = 0;
    if (m_parakeet) return readParakeet(error);
    if (m_pieces.isEmpty()) {
        QFile json(m_tmp->filePath("out.json"));
        if (!json.open(QIODevice::ReadOnly)) return false;
        // The whole sound at once: no repeat filter (it is made for speech pieces)
        return Transcript::parseWhisperJson(json.readAll(), &m_result, error);
    }
    qsizetype previous = -1; // the words of the last piece that had any
    for (int i = 0; i < m_pieces.size(); ++i) {
        QFile json(m_tmp->filePath(QString("piece_%1.wav.json").arg(i, 3, 10, QChar('0'))));
        if (!json.open(QIODevice::ReadOnly)) return false;
        const Transcript::Piece& piece = m_pieces[i];
        const qsizetype first = m_result.words.size();
        if (!Transcript::appendWhisperJson(json.readAll(),
                                           [&piece](qint64 t, bool end) { return Transcript::pieceTime(piece, t, end); },
                                           &m_result, error))
            return false;
        // A piece written twice, or the piece before written once more (whisper turbo); not in a song (a chorus)
        if (!m_request.music) m_repeats += Transcript::dropRepeats(m_result.words, first, previous);
        if (m_result.words.size() > first) previous = first;
    }
    return true;
}

QString Transcriber::dtwPreset(const QString& model)
{
    // ggml-<name>[-q5_0].bin; whisper.cpp's presets: tiny(.en), base(.en), small(.en), medium(.en), large.v1/v2/v3,
    // large.v3.turbo
    const QString n = QFileInfo(model).fileName().toLower();
    // Distilled models have fewer decoder layers than the preset's alignment heads name: whisper would not start
    if (n.contains("distil")) return {};
    if (n.contains("large-v3-turbo")) return "large.v3.turbo";
    if (n.contains("large-v3")) return "large.v3";
    if (n.contains("large-v2")) return "large.v2";
    if (n.contains("large-v1") || n.contains("ggml-large.") || n.contains("ggml-large-q")) return "large.v1";
    for (const char* size : {"medium", "small", "base", "tiny"}) {
        if (n.contains(QString("ggml-%1.en").arg(size))) return QString("%1.en").arg(size);
        if (n.contains(QString("ggml-%1.").arg(size)) || n.contains(QString("ggml-%1-").arg(size))) return size;
    }
    return {};
}

QString Transcriber::punctuationPrompt(const QString& language)
{
    const QString lang = language.trimmed().toLower().left(2);
    // German only (measured 21.11.): with English in it ("… Okay, let's go!", "Okay, cool.") turbo kept English said in
    // German talk on one test but skipped 10 s of German speech or a whole English passage on others
    if (lang == QLatin1String("de")) return QStringLiteral("Hallo, wie geht's? Gut, und dir? Ja, passt.");
    if (lang == QLatin1String("en")) return QStringLiteral("Hello, how are you? Good, and you? Yeah, fine.");
    return {};
}

QVector<QPair<qint64, qint64>> Transcriber::parseSpeechSegments(const QString& output)
{
    static const QRegularExpression re("start\\s*=\\s*([0-9.]+)\\s*,\\s*end\\s*=\\s*([0-9.]+)");
    QVector<QPair<qint64, qint64>> segments;
    for (auto it = re.globalMatch(output); it.hasNext();) {
        const auto m = it.next();
        const qint64 from = qint64(std::llround(m.captured(1).toDouble() * 10)), to = qint64(std::llround(m.captured(2).toDouble() * 10));
        if (to > from) segments << qMakePair(from, to);
    }
    return segments;
}

void Transcriber::runDetection()
{
    m_stage = 2;
    m_log.clear(); // the detected language is a log line of whisper (not with -np)
    const int threads = m_request.threads > 0 ? m_request.threads : defaultThreads();
    m_process.setWorkingDirectory(m_tmp->path());
    QStringList args{"-m", narrowSafePath(detectionModel()), "-f", "audio.wav", "-t", QString::number(threads), "-l",
                     "auto", "-dl"};
    if (m_cpuOnly) args << "-ng";
    m_process.start(Extensions::whisperProgram(), args);
}

QString Transcriber::verbatimPrompt(const QString& language)
{
    // Only the sounds whisper should write as it hears them (core/ScriptAlign::isFiller knows them), no phrases that
    // could leak into the transcript
    static const QHash<QString, QString> prompts{
        {"de", QStringLiteral("Ähm, äh, hm, ähm, äh.")},
        {"en", QStringLiteral("Um, uh, hmm, um, uh.")},
        {"fr", QStringLiteral("Euh, hmm, euh, euh.")},
        {"es", QStringLiteral("Eh, ehm, mmm, eh.")},
        {"it", QStringLiteral("Ehm, eh, ehm, eh.")},
        {"pt", QStringLiteral("Ahn, hmm, ahn, ehm.")},
        {"nl", QStringLiteral("Eh, ehm, uh, ehm.")},
        {"pl", QStringLiteral("Yyy, eee, hmm, yyy.")},
        {"ru", QStringLiteral("Э-э, эм, ммм, э-э.")},
        {"uk", QStringLiteral("Еее, гм, ммм, еее.")},
        {"tr", QStringLiteral("Eee, ııı, hmm, eee.")},
        {"id", QStringLiteral("Eh, ehm, hmm, eh.")},
        {"ja", QStringLiteral("えー、えっと、うーん、えー。")},
        {"ko", QStringLiteral("음, 으음, 흠, 음.")},
        {"zh", QStringLiteral("呃，呃，呃呃。")},
        {"hi", QStringLiteral("उम्म, हम्म, उम्म।")},
    };
    const QString lang = language.trimmed().toLower().left(2);
    return prompts.value(lang, QStringLiteral("Umm, uhh, hmm, ehm."));
}

QString Transcriber::verbatimPromptFor(const QString& language, const std::function<bool(const QString&)>& fits,
                                       QString* note)
{
    const QString prompt = verbatimPrompt(language);
    if (!fits || fits(prompt)) return prompt;
    // Latin script: the same sounds without accents fit every code page; other scripts cannot be written at all
    static const QHash<QString, QString> ascii{
        {"de", QStringLiteral("Ehm, hm, hmm, ehm.")}, {"fr", QStringLiteral("Euh, hmm, euh.")},
        {"es", QStringLiteral("Eh, ehm, mmm, eh.")},  {"it", QStringLiteral("Ehm, eh, ehm.")},
        {"pt", QStringLiteral("Ahn, hmm, ehm.")},     {"nl", QStringLiteral("Eh, ehm, uh.")},
        {"pl", QStringLiteral("Yyy, eee, hmm.")},     {"tr", QStringLiteral("Eee, hmm, eee.")},
        {"id", QStringLiteral("Eh, ehm, hmm.")},      {"en", QStringLiteral("Um, uh, hmm, um.")},
    };
    const QString lang = language.trimmed().toLower().left(2);
    if (ascii.contains(lang) || !QStringList{"ru", "uk", "ja", "ko", "zh", "hi"}.contains(lang)) {
        const QString plain = ascii.value(lang, QStringLiteral("Umm, uhh, hmm, ehm."));
        if (fits(plain)) return plain;
    }
    if (note)
        *note = QStringLiteral("verbatim: whisper gets no starting text in this language here (the system's code page "
                               "cannot write it), so it may leave out more of the fillers");
    return {};
}

bool Transcriber::fitsCodePage(const QString& text)
{
#ifdef Q_OS_WIN
    // whisper-cli reads its arguments in the ANSI code page: a character it cannot hold would arrive as '?'
    const std::wstring w = text.toStdWString();
    BOOL usedDefault = FALSE;
    const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), int(w.size()), nullptr, 0, nullptr,
                                      &usedDefault);
    return n > 0 && !usedDefault;
#else
    Q_UNUSED(text);
    return true;
#endif
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
    // (on the graphics card about a tenth: large-v3 0.12 s on a GTX 1070)
    if (perSecond <= 0) {
        const bool gpu = model.endsWith(QLatin1String("@gpu"));
        const QString file = gpu ? model.chopped(4) : model;
        // Parakeet: ~0.15 s per second of sound on 4 cores (measured 22.11.)
        perSecond = (isParakeet(file) ? 0.2 : 0.5 + QFileInfo(file).size() / (150.0 * 1024 * 1024)) / (gpu ? 10 : 1);
    }
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
