#include "engine/Transcriber.h"

#include "core/I18n.h"
#include "engine/Bundle.h"
#include "engine/Exporter.h"
#include "engine/Extensions.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>

namespace {
constexpr double kAudioShare = 0.1; // progress: preparing the audio, then whisper

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
        if (percent >= 0) emit progress(kAudioShare + (1 - kAudioShare) * percent / 100.0);
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_running) return;
        if (m_cancelled) return done(false, T("Abgebrochen"));
        const QString tail = QString::fromUtf8(m_log.right(400)).trimmed();
        if (status != QProcess::NormalExit || code != 0) {
            return done(false, m_stage == 0 ? T("Ton konnte nicht gelesen werden: %1").arg(tail)
                                            : T("Whisper ist fehlgeschlagen: %1").arg(tail));
        }
        if (m_stage == 0) return runWhisper();
        QFile json(m_tmp->filePath("out.json"));
        if (!json.open(QIODevice::ReadOnly)) return done(false, T("Whisper hat kein Ergebnis geschrieben: %1").arg(tail));
        QString error;
        if (!Transcript::parseWhisperJson(json.readAll(), &m_result, &error)) return done(false, error);
        m_result.levels = wavLevels(m_tmp->filePath("audio.wav"));
        Transcript::fitToSpeech(m_result.words, m_result.levels);
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

int Transcriber::defaultThreads() { return std::max(1, QThread::idealThreadCount() / 2); }

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
    m_log.clear();
    m_cancelled = false;
    m_stage = 0;
    const QString wav = m_tmp->filePath("audio.wav");
    if (!request.media.isEmpty()) {
        // 16 kHz mono like whisper wants it; only the chosen audio stream
        m_process.start(Bundle::tool("ffmpeg"),
                        {"-hide_banner", "-nostdin", "-loglevel", "error", "-i", request.media, "-map",
                         QString("0:a:%1").arg(request.audioStream), "-vn", "-ac", "1", "-ar", "16000", "-c:a",
                         "pcm_s16le", wav});
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
    const int threads = m_request.threads > 0 ? m_request.threads : defaultThreads();
    // One word per segment (-ml 1 -sow): word times for cutting; Transcript::toCues builds subtitle lines from them
    QStringList args{"-m", m_request.model, "-f", m_tmp->filePath("audio.wav"), "-t", QString::number(threads), "-l",
                     m_request.language.isEmpty() ? QString("auto") : m_request.language, "-ml", "1", "-sow", "-oj",
                     "-of", m_tmp->filePath("out"), "-np", "-pp"};
    // Without voice detection whisper puts the first words of a passage at the start of a pause before it
    // Threshold 0.35 instead of 0.5: keeps quiet or laughed words (measured: "Diese Scheiß-Pferd" instead of "Das")
    if (const QString vad = Extensions::whisperVadModel(); !vad.isEmpty()) args << "--vad" << "-vm" << vad << "-vt" << "0.35";
    m_process.start(Extensions::whisperProgram(), args);
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
    if (m_exporter) m_exporter.release()->deleteLater(); // may be inside its own signal
    if (ok) emit progress(1.0);
    emit finished(ok, error);
}
