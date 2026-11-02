// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Tts.h"

#include "core/I18n.h"
#include "engine/Extensions.h"
#include "engine/Transcriber.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUuid>

#include <QtEndian>

#include <cmath>
#include <cstdlib>

namespace Tts {

namespace {

QString wordsFile(const QString& wav) { return wav + ".words.json"; }

bool loadWords(const QString& wav, Result* r)
{
    QFile f(wordsFile(wav));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (!o.contains("words")) return false;
    r->words.clear();
    for (const QJsonValue& v : o.value("words").toArray()) {
        const QJsonObject w = v.toObject();
        r->words << TranscriptWord{qint64(w.value("from").toDouble()), qint64(w.value("to").toDouble()),
                                   w.value("text").toString()};
    }
    r->wordSource = o.value("source").toString();
    return !r->words.isEmpty();
}

void saveWords(const QString& wav, const Result& r)
{
    QJsonArray words;
    for (const TranscriptWord& w : r.words)
        words << QJsonObject{{"from", double(w.from)}, {"to", double(w.to)}, {"text", w.text}};
    QFile f(wordsFile(wav));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(QJsonObject{{"source", r.wordSource}, {"words", words}}).toJson(QJsonDocument::Compact));
}

quint32 le32(const char* p) { return quint8(p[0]) | quint8(p[1]) << 8 | quint8(p[2]) << 16 | quint32(quint8(p[3])) << 24; }
quint16 le16(const char* p) { return quint16(quint8(p[0]) | quint8(p[1]) << 8); }

// Build of a voice for the cache key: the catalog's checksum (pinned download), else size and time of the file
QString voiceVersion(const QString& voice)
{
    if (const QVector<const Extensions::Item*> items = Extensions::parseList(voice);
        items.size() == 1 && items.first()->kind == Extensions::Kind::Voice)
        return QString::fromLatin1(items.first()->sha256.left(16));
    const QFileInfo model(Extensions::voiceModelPath(voice));
    const QFileInfo config(model.filePath() + ".json");
    return QString("%1-%2-%3")
        .arg(model.size())
        .arg(model.lastModified().toMSecsSinceEpoch())
        .arg(config.lastModified().toMSecsSinceEpoch());
}

} // namespace

QString defaultVoice()
{
    const QStringList voices = Extensions::installedVoices();
    for (const QString& v : voices)
        if (Speech::voiceLanguage(Extensions::voiceName(v)) == I18n::language()) return v;
    return voices.value(0);
}

QString notReady(const QString& voice)
{
    if (Extensions::piperProgram().isEmpty())
        return T("Piper ist nicht installiert (Arbeitsbereich → Erweiterungen).");
    const QString v = voice.isEmpty() ? defaultVoice() : voice;
    if (v.isEmpty()) return T("Keine Stimme installiert (Arbeitsbereich → Erweiterungen).");
    if (Extensions::voiceModelPath(v).isEmpty()) return T("Stimme nicht installiert: %1").arg(voice);
    return {};
}

std::optional<WavInfo> readWav(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QByteArray data = f.readAll();
    if (data.size() < 12 || !data.startsWith("RIFF") || data.mid(8, 4) != "WAVE") return std::nullopt;
    WavInfo info;
    int bits = 0;
    qsizetype pos = 12, dataAt = -1, dataSize = 0;
    while (pos + 8 <= data.size()) {
        const QByteArray id = data.mid(pos, 4);
        const qsizetype size = le32(data.constData() + pos + 4);
        if (id == "fmt " && size >= 16 && pos + 8 + 16 <= data.size()) {
            const char* p = data.constData() + pos + 8;
            if (le16(p) != 1) return std::nullopt; // PCM only
            info.channels = le16(p + 2);
            info.sampleRate = int(le32(p + 4));
            bits = le16(p + 14);
        } else if (id == "data") {
            dataAt = pos + 8;
            dataSize = std::min<qsizetype>(size, data.size() - dataAt);
            break;
        }
        pos += 8 + size + (size & 1);
    }
    if (dataAt < 0 || bits != 16 || info.channels <= 0 || info.sampleRate <= 0) return std::nullopt;
    const qint64 frames = dataSize / (2 * info.channels);
    info.durationMs = frames * 1000 / info.sampleRate;
    // Level per 10 ms block (all channels): the peak and where it is louder than -40 dBFS
    const auto* s = reinterpret_cast<const qint16*>(data.constData() + dataAt);
    const qint64 block = std::max(1, info.sampleRate / 100);
    int peak = 0;
    qint64 first = -1, last = -1;
    for (qint64 b = 0; b * block < frames; ++b) {
        int blockPeak = 0;
        for (qint64 i = b * block; i < std::min(frames, (b + 1) * block); ++i)
            for (int c = 0; c < info.channels; ++c) {
                const qint16 v = qFromLittleEndian(s[i * info.channels + c]);
                blockPeak = std::max(blockPeak, std::abs(int(v)));
            }
        peak = std::max(peak, blockPeak);
        if (blockPeak > 327) { // -40 dBFS
            if (first < 0) first = b;
            last = b;
        }
    }
    info.peakDb = peak > 0 ? 20 * std::log10(peak / 32768.0) : -120;
    info.speechFromMs = first < 0 ? 0 : first * 10;
    info.speechToMs = last < 0 ? info.durationMs : std::min(info.durationMs, (last + 1) * 10);
    return info;
}

// ---------- Speaker ----------

Speaker::Speaker(QObject* parent) : QObject(parent)
{
    connect(&m_process, &QProcess::finished, this, &Speaker::piperDone);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && m_running) done(false, T("Piper konnte nicht gestartet werden."));
    });
}

Speaker::~Speaker()
{
    m_process.disconnect(this); // no finished() from a dying speaker (its owner is being destroyed)
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
        QFile::remove(m_part);
    }
}

bool Speaker::start(const Request& request, QString* error)
{
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (m_running) return fail("already running");
    if (Speech::normalizedText(request.text).isEmpty()) return fail(T("Kein Text."));
    m_request = request;
    if (m_request.options.voice.isEmpty()) m_request.options.voice = defaultVoice();
    if (const QString why = notReady(m_request.options.voice); !why.isEmpty()) return fail(why);
    m_request.options.voiceVersion = voiceVersion(m_request.options.voice);
    if (!QDir().mkpath(request.folder)) return fail(T("Ordner kann nicht angelegt werden: %1").arg(request.folder));
    m_result = {};
    m_result.voice = m_request.options.voice;
    m_result.path = QDir(request.folder).filePath(Speech::fileName(m_request.text, m_request.options));
    m_cancelled = false;
    m_running = true;
    if (QFileInfo(m_result.path).size() > 44) {
        // Made before: only the words may be missing
        m_result.cached = true;
        QTimer::singleShot(0, this, [this] { piperDone(0, QProcess::NormalExit); });
        return true;
    }
    // A part file of its own: two runs of the same text (CLI and app, two edits) must not write into one file
    m_part = m_result.path.chopped(4) + "." + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".part.wav";
    const QString model = Extensions::voiceModelPath(m_request.options.voice);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.start(Extensions::piperProgram(), Speech::piperArguments(model, m_request.options, m_part));
    m_process.write(Speech::stdinText(m_request.text));
    m_process.closeWriteChannel();
    return true;
}

void Speaker::cancel()
{
    if (!m_running) return;
    m_cancelled = true;
    if (m_transcriber) m_transcriber->cancel();
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
}

void Speaker::piperDone(int exitCode, QProcess::ExitStatus status)
{
    if (!m_running) return;
    if (m_cancelled) {
        if (!m_result.cached) QFile::remove(m_part); // killed half way
        return done(false, T("Abgebrochen"));
    }
    if (!m_result.cached) {
        if (status != QProcess::NormalExit || exitCode != 0 || QFileInfo(m_part).size() <= 44) {
            const QString log = QString::fromLocal8Bit(m_process.readAllStandardError()).trimmed().right(400);
            QFile::remove(m_part);
            return done(false, T("Piper ist fehlgeschlagen: %1").arg(log.isEmpty() ? T("keine Ausgabe") : log));
        }
        if (QFileInfo(m_result.path).size() > 44) {
            QFile::remove(m_part); // another run of the same text was faster: the same sound
        } else {
            QFile::remove(m_result.path);
            if (!QFile::rename(m_part, m_result.path)) {
                QFile::remove(m_part);
                return done(false, T("Datei konnte nicht abgelegt werden: %1").arg(m_result.path));
            }
        }
    }
    const std::optional<WavInfo> info = readWav(m_result.path);
    if (!info) return done(false, T("Piper hat keine lesbare WAV-Datei erzeugt."));
    m_result.durationMs = info->durationMs;
    emit progress(m_request.words ? 0.5 : 1.0);
    if (!m_request.words || loadWords(m_result.path, &m_result)) return done(true, {});
    if (!startWords()) {
        wordsDone({}, {});
    }
}

bool Speaker::startWords()
{
    // SCHNEIDI_TTS_WORDS=estimate: never whisper (tests: the same result with or without whisper on the machine)
    if (qEnvironmentVariable("SCHNEIDI_TTS_WORDS") == "estimate") return false;
    const QString model = Extensions::whisperModelPath();
    if (Extensions::whisperProgram().isEmpty() || model.isEmpty()) return false;
    TranscribeRequest req;
    req.media = m_result.path;
    req.model = model;
    const QString lang = Speech::voiceLanguage(Extensions::voiceName(m_request.options.voice));
    req.language = lang.isEmpty() ? QString("auto") : lang;
    m_transcriber = std::make_unique<Transcriber>();
    connect(m_transcriber.get(), &Transcriber::progress, this, [this](double p) { emit progress(0.5 + p / 2); });
    connect(m_transcriber.get(), &Transcriber::finished, this, [this](bool ok, const QString&) {
        if (!m_running) return;
        if (m_cancelled) return done(false, T("Abgebrochen"));
        wordsDone(ok ? m_transcriber->result().words : QVector<TranscriptWord>{}, "whisper");
    });
    QString error;
    if (!m_transcriber->start(req, &error)) {
        m_transcriber.reset();
        return false;
    }
    return true;
}

void Speaker::wordsDone(const QVector<TranscriptWord>& measured, const QString& source)
{
    const QStringList textWords = Speech::normalizedText(m_request.text).split(' ', Qt::SkipEmptyParts);
    const qint64 pauseMs = qint64(std::lround(m_request.options.pause * 1000));
    if (!measured.isEmpty() && measured.size() == textWords.size()) {
        // Whisper heard as many words as were written: its times, the words as written (names, spelling)
        m_result.words = measured;
        for (int i = 0; i < textWords.size(); ++i) m_result.words[i].text = textWords[i];
        m_result.wordSource = source;
    } else if (!measured.isEmpty()) {
        // Other words heard (numbers, names): its start and end of speech, the words spread between
        m_result.words = Speech::estimateWords(m_request.text, measured.first().from, measured.last().to, pauseMs);
        m_result.wordSource = "whisper-span";
    } else {
        const std::optional<WavInfo> info = readWav(m_result.path);
        m_result.words = Speech::estimateWords(m_request.text, info ? info->speechFromMs : 0,
                                               info ? info->speechToMs : m_result.durationMs, pauseMs);
        m_result.wordSource = "estimate";
    }
    saveWords(m_result.path, m_result);
    done(true, {});
}

void Speaker::done(bool ok, const QString& error)
{
    m_running = false;
    if (m_transcriber) m_transcriber.release()->deleteLater(); // may be inside its own signal
    if (ok) emit progress(1.0);
    emit finished(ok, error);
}

bool speak(const Request& request, Result* out, QString* error, const std::function<void(double)>& progress)
{
    Speaker speaker;
    QEventLoop loop;
    bool ok = false;
    QString message;
    QObject::connect(&speaker, &Speaker::progress, &loop, [&](double p) {
        if (progress) progress(p);
    });
    QObject::connect(&speaker, &Speaker::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        message = msg;
        loop.quit();
    });
    if (!speaker.start(request, &message)) {
        if (error) *error = message;
        return false;
    }
    loop.exec();
    if (!ok) {
        if (error) *error = message;
        return false;
    }
    if (out) *out = speaker.result();
    return true;
}

} // namespace Tts
