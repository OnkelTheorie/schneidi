// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Extensions.h"

#include "core/I18n.h"
#include "engine/Bundle.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>

namespace Extensions {

namespace {

// whisper.cpp release b5454 (v1.9.5); sums from the GitHub release / Hugging Face (LFS sha256)
constexpr const char* kWhisperRelease = "https://github.com/ggml-org/whisper.cpp/releases/download/b5454/";
constexpr const char* kModels = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/";

// ffmpeg with x264/x265: BtbN/FFmpeg-Builds, release branch 8.1 from the month-end autobuild of October 2026 (the
// month-end builds stay on GitHub; the daily ones are deleted). "shared": half the download of the static build; the
// archive's folder is kept as it is (Linux: bin/ffmpeg finds lib/ by its RUNPATH, Windows: the DLLs are in bin/).
constexpr const char* kEncoderRelease = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-31-12-59/";

// Piper 2023.11.14-2 (rhasspy/piper, MIT; the last release of that repository, archived 2025). The archive holds the
// program with its libraries (onnxruntime MIT, piper-phonemize MIT, espeak-ng GPL-3) and espeak-ng-data in one folder.
constexpr const char* kPiperRelease = "https://github.com/rhasspy/piper/releases/download/2023.11.14-2/";
// Voices of rhasspy/piper-voices, pinned to one revision (model cards: dataset licence of each voice)
constexpr const char* kVoices = "https://huggingface.co/rhasspy/piper-voices/resolve/b4e358ffcb232591bf73a540e70f7de528e4174a/";
constexpr const char* kVoicePrefix = "piper-voice-";

#ifdef Q_OS_WIN
constexpr const char* kProgramName = "whisper-cli.exe";
constexpr const char* kFfmpegName = "ffmpeg.exe";
constexpr const char* kPiperName = "piper.exe";
#else
constexpr const char* kProgramName = "whisper-cli";
constexpr const char* kFfmpegName = "ffmpeg";
constexpr const char* kPiperName = "piper";
#endif

QVector<Item> makeCatalog()
{
    QVector<Item> list;
    const auto program = [&](const char* archive, const char* sha, qint64 size) {
        list << Item{"whisper", Kind::Program, N_("Whisper (Spracherkennung)"),
                     N_("whisper.cpp: erkennt Sprache offline und erzeugt Untertitel und Transkripte"),
                     QString(kWhisperRelease) + archive, sha, size, archive};
    };
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    program("whisper-bin-x64.zip", "6ba69e3482d7826214f90a6a9c84ca07782aec1e1d0c6a7c30c994fd5d816ccb", 8928640);
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    program("whisper-bin-ubuntu-x64.tar.gz", "a72becf15d7917f990f6313867a52638b82b7f9ef237fb0c980dac56a135781c", 10364195);
#endif
    const auto encoder = [&](const char* archive, const char* sha, qint64 size) {
        list << Item{"x264", Kind::Encoder, N_("x264/x265 (bessere Export-Qualität)"),
                     N_("FFmpeg mit x264 und x265: H.264/H.265 in besserer Qualität, wenn keine Grafikkarte kodiert (GPL, "
                        "eigenes Programm)"),
                     QString(kEncoderRelease) + archive, sha, size, archive};
    };
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    encoder("ffmpeg-n8.1.3-27-g8dca1586f4-win64-gpl-shared-8.1.zip",
            "3adc00b67118a8c4bdd6624a000e139df283b1eeb8b126fcec4849bd5b778dd4", 89702793);
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    encoder("ffmpeg-n8.1.3-27-g8dca1586f4-linux64-gpl-shared-8.1.tar.xz",
            "202fe67679a0860b155817a0907ffcc5c1ea82e186acaa21cb87d307f54fd59a", 72031160);
#endif
    const auto piper = [&](const char* archive, const char* sha, qint64 size) {
        list << Item{"piper", Kind::Speech, N_("Piper (Sprachausgabe)"),
                     N_("Piper: spricht Text offline mit natürlichen Stimmen (für Sprechertexte; Stimmen einzeln)"),
                     QString(kPiperRelease) + archive, sha, size, archive};
    };
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    piper("piper_windows_amd64.zip", "f3c58906402b24f3a96d92145f58acba6d86c9b5db896d207f78dc80811efcea", 22477236);
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    piper("piper_linux_x86_64.tar.gz", "a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992", 26460462);
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_ARM_64)
    piper("piper_linux_aarch64.tar.gz", "fea0fd2d87c54dbc7078d0f878289f404bd4d6eea6e7444a77835d1537ab88eb", 26004717);
#endif
    // Voices whose training data allows any use: Thorsten (CC0), LJ Speech (public domain)
    const auto voice = [&](const char* name, const char* title, const char* description, const char* folder,
                           const char* sha, qint64 size, const char* configSha, qint64 configSize) {
        const QString file = QString(name) + ".onnx";
        const QString base = QString(kVoices) + folder + file;
        list << Item{kVoicePrefix + QString(name).section('-', 0, 1), Kind::Voice, title, description, base, sha, size,
                     file, base + ".json", configSha, configSize};
    };
    voice("de_DE-thorsten-medium", N_("Stimme Deutsch: Thorsten"), N_("männlich, klar (Piper, freie Lizenz CC0)"),
          "de/de_DE/thorsten/medium/", "7e64762d8e5118bb578f2eea6207e1a35a8e0c30595010b666f983fc87bb7819", 63201294,
          "974adee790533adb273a1ac88f49027d2a1b8f0f2cf4905954a4791e79264e85", 4819);
    voice("en_US-ljspeech-medium", N_("Stimme Englisch: LJ Speech"), N_("weiblich, amerikanisch (Piper, gemeinfrei)"),
          "en/en_US/ljspeech/medium/", "6f52a751e2349abe7a76735eb09dc1875298c77ea2342ffd2fef79ff81b87f22", 63531379,
          "141d612cc0a95ed7efc1ca936b845c2364967f2e9217c5dbfcf69fc4d6c65860", 4972);
    list << Item{"whisper-vad", Kind::WhisperVad, N_("Sprach-Erkennung für Whisper (Silero VAD)"),
                 N_("findet die Stellen mit Sprache, damit Wortzeiten nicht in Pausen rutschen (gehört zu Whisper)"),
                 "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin",
                 "2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987", 885098, "ggml-silero-v6.2.0.bin"};
    const auto model = [&](const char* id, const char* name, const char* description, const char* file,
                           const char* sha, qint64 size) {
        list << Item{id, Kind::WhisperModel, name, description, QString(kModels) + file, sha, size, file};
    };
    // Best first (installedWhisperModels keeps this order); all multilingual, quantized (small download)
    model("whisper-model-turbo", N_("Whisper-Modell „Sehr gut“ (large-v3-turbo)"),
          N_("beste Erkennung, braucht deutlich mehr Rechenzeit"), "ggml-large-v3-turbo-q5_0.bin",
          "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2", 574041195);
    model("whisper-model-small", N_("Whisper-Modell „Gut“ (small)"), N_("empfohlen für Deutsch"),
          "ggml-small-q5_1.bin", "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb", 190085487);
    model("whisper-model-base", N_("Whisper-Modell „Ausgewogen“ (base)"), N_("schnell, für klare Sprache"),
          "ggml-base-q5_1.bin", "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898", 59707625);
    model("whisper-model-tiny", N_("Whisper-Modell „Schnell“ (tiny)"), N_("sehr schnell, ungenau"),
          "ggml-tiny-q5_1.bin", "818710568da3ca15689e31a743197b520007872ff9576237bda97bd1b469c3d7", 32152673);
    return list;
}

QString whisperDir() { return QDir(rootDir()).filePath("whisper"); }
QString modelsDir() { return QDir(whisperDir()).filePath("models"); }
QString binDir() { return QDir(whisperDir()).filePath("bin"); }
QString encoderDir() { return QDir(rootDir()).filePath("ffmpeg-gpl"); }
QString encoderTree() { return QDir(encoderDir()).filePath("bin"); }
QString piperDir() { return QDir(rootDir()).filePath("piper"); }
QString piperBin() { return QDir(piperDir()).filePath("bin"); }
QString voicesDir() { return QDir(piperDir()).filePath("voices"); }

// Folder an item's download is unpacked or stored in
QString downloadDir(const Item& item)
{
    switch (item.kind) {
    case Kind::Program: return whisperDir();
    case Kind::Encoder: return encoderDir();
    case Kind::Speech: return piperDir();
    case Kind::Voice: return voicesDir();
    default: return modelsDir();
    }
}

// The installed ffmpeg of the encoder extension: <tree>/bin/ffmpeg (the archive's folder as it was)
QString installedFfmpeg()
{
    const QString direct = QDir(encoderTree()).filePath(QString("bin/") + kFfmpegName);
    if (QFileInfo(direct).isFile()) return direct;
    QDirIterator it(encoderTree(), {kFfmpegName}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
}

} // namespace

const QVector<Item>& catalog()
{
    static const QVector<Item> list = makeCatalog();
    return list;
}

const Item* find(const QString& id)
{
    for (const Item& i : catalog())
        if (i.id == id) return &i;
    return nullptr;
}

QString rootDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("extensions");
}

QVector<const Item*> parseList(const QString& list, QString* unknown)
{
    QVector<const Item*> out;
    for (QString id : list.split(',', Qt::SkipEmptyParts)) {
        id = id.trimmed().toLower();
        if (id.isEmpty()) continue;
        if (id == "x265" || id == "x264-x265" || id == "x264/x265") id = "x264";
        const Item* i = find(id);
        if (!i) i = find("whisper-model-" + id);
        if (!i) i = find(kVoicePrefix + id);
        if (!i) // "de_DE-thorsten-medium" (the file name), "thorsten"
            for (const Item& v : catalog())
                if (v.kind == Kind::Voice
                    && (QString(v.fileName).chopped(5).toLower() == id || voiceName(v.id).section('-', 1).toLower() == id))
                    i = &v;
        if (!i) {
            if (unknown) *unknown = id;
            return {};
        }
        if (!out.contains(i)) out << i;
    }
    return out;
}

QVector<const Item*> withDependencies(const QVector<const Item*>& items)
{
    QVector<const Item*> out;
    const auto add = [&out](const Item* i) {
        if (i && !out.contains(i)) out << i;
    };
    for (const Item* i : items) {
        // A model is no use without the program (unless one is on PATH)
        if (i->kind == Kind::WhisperModel && whisperProgram().isEmpty()) add(find("whisper"));
        if (i->kind == Kind::Voice && piperProgram().isEmpty()) add(find("piper"));
        add(i);
        if (i->id == "whisper") add(find("whisper-vad"));
    }
    if (out.contains(find("whisper"))) add(find("whisper-vad"));
    return out;
}

QString installedPath(const Item& item)
{
    if (item.kind == Kind::Encoder) return installedFfmpeg();
    if (item.kind == Kind::Voice) { // both files
        const QString path = QDir(voicesDir()).filePath(item.fileName);
        return QFileInfo::exists(path) && QFileInfo::exists(path + ".json") ? path : QString();
    }
    const QString path = item.kind == Kind::Program  ? QDir(binDir()).filePath(kProgramName)
                         : item.kind == Kind::Speech ? QDir(piperBin()).filePath(kPiperName)
                                                     : QDir(modelsDir()).filePath(item.fileName);
    return QFileInfo::exists(path) ? path : QString();
}

bool isInstalled(const Item& item) { return !installedPath(item).isEmpty(); }

bool remove(const Item& item)
{
    if (item.kind == Kind::Program) return QDir(binDir()).removeRecursively();
    if (item.kind == Kind::Encoder) return QDir(encoderTree()).removeRecursively();
    if (item.kind == Kind::Speech) return QDir(piperBin()).removeRecursively();
    if (item.kind == Kind::Voice) {
        const QString path = QDir(voicesDir()).filePath(item.fileName);
        QFile::remove(path + ".json");
        return !QFileInfo::exists(path) || QFile::remove(path);
    }
    const QString path = installedPath(item);
    return path.isEmpty() || QFile::remove(path);
}

QString whisperProgram()
{
    if (const Item* i = find("whisper"); i && isInstalled(*i)) return installedPath(*i);
    return QStandardPaths::findExecutable("whisper-cli");
}

QString whisperVadModel()
{
    const Item* i = find("whisper-vad");
    return i ? installedPath(*i) : QString();
}

QString encoderProgram()
{
    const QString own = qEnvironmentVariable("SCHNEIDI_ENCODER_FFMPEG");
    if (!own.isEmpty()) return QFileInfo(own).isFile() ? QFileInfo(own).absoluteFilePath() : QString();
    const Item* i = find("x264");
    return i ? installedPath(*i) : QString();
}

QString piperProgram()
{
    const QString own = qEnvironmentVariable("SCHNEIDI_PIPER");
    if (!own.isEmpty()) return QFileInfo(own).isFile() ? QFileInfo(own).absoluteFilePath() : QString();
    const Item* i = find("piper");
    return i ? installedPath(*i) : QString();
}

QStringList installedVoices()
{
    QStringList ids;
    for (const Item& i : catalog())
        if (i.kind == Kind::Voice && isInstalled(i)) ids << i.id;
    return ids;
}

QString voiceName(const QString& id) { return id.startsWith(kVoicePrefix) ? id.mid(QString(kVoicePrefix).size()) : id; }

QString voiceModelPath(const QString& idOrPath)
{
    if (idOrPath.isEmpty()) return {};
    if (const QVector<const Item*> items = parseList(idOrPath); items.size() == 1 && items.first()->kind == Kind::Voice)
        return installedPath(*items.first());
    const QFileInfo file(idOrPath);
    if (file.isFile() && QFileInfo::exists(file.absoluteFilePath() + ".json")) return file.absoluteFilePath();
    return {};
}

QStringList installedWhisperModels()
{
    QStringList ids;
    for (const Item& i : catalog())
        if (i.kind == Kind::WhisperModel && isInstalled(i)) ids << i.id;
    return ids;
}

QString whisperModelPath(const QString& idOrPath)
{
    if (idOrPath.isEmpty()) {
        const QStringList ids = installedWhisperModels();
        return ids.isEmpty() ? QString() : installedPath(*find(ids.first()));
    }
    for (const Item& i : catalog()) // also short names: "small", "turbo"
        if (i.kind == Kind::WhisperModel && (i.id == idOrPath || i.id == "whisper-model-" + idOrPath))
            return installedPath(i);
    return QFileInfo(idOrPath).isFile() ? QFileInfo(idOrPath).absoluteFilePath() : QString();
}

// ---------- Installer ----------

Installer::Installer(QObject* parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

Installer::~Installer()
{
    if (m_reply) cancel();
}

bool Installer::start(const Item& item, QString* error)
{
    if (m_reply) {
        if (error) *error = "a download is already running";
        return false;
    }
    m_item = item;
    m_cancelled = false;
    m_part = 0;
    return startPart(error);
}

bool Installer::startPart(QString* error)
{
    const Item& item = m_item;
    const QString dir = downloadDir(item);
    if (!QDir().mkpath(dir)) {
        if (error) *error = "cannot create " + dir;
        return false;
    }
    const QString name = m_part == 0 ? item.fileName : item.fileName + ".json";
    m_file = std::make_unique<QFile>(QDir(dir).filePath(name + ".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = m_file->errorString();
        return false;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    QNetworkRequest request{QUrl(m_part == 0 ? item.url : item.configUrl)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, "schneidi");
    m_reply = m_net->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        const QByteArray data = m_reply->readAll();
        m_hash->addData(data);
        if (m_file->write(data) != data.size()) {
            fail(T("Schreiben fehlgeschlagen: %1").arg(m_file->errorString()));
            return;
        }
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (m_part == 0) emit progress(received, total > 0 ? total : m_item.size);
    });
    connect(m_reply, &QNetworkReply::finished, this, &Installer::onFinished);
    return true;
}

void Installer::cancel()
{
    if (!m_reply) return;
    m_cancelled = true;
    m_reply->abort(); // -> onFinished
}

void Installer::fail(const QString& message)
{
    if (m_reply) {
        QNetworkReply* r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    if (m_file) {
        m_file->close();
        m_file->remove();
        m_file.reset();
    }
    emit finished(false, message);
}

void Installer::onFinished()
{
    if (!m_reply) return;
    QNetworkReply* r = m_reply;
    if (r->bytesAvailable()) {
        const QByteArray rest = r->readAll();
        m_hash->addData(rest);
        m_file->write(rest);
    }
    if (m_cancelled) return fail(T("Abgebrochen"));
    if (r->error() != QNetworkReply::NoError) return fail(T("Download fehlgeschlagen: %1").arg(r->errorString()));
    m_reply = nullptr;
    r->deleteLater();
    m_file->close();
    const qint64 size = m_part == 0 ? m_item.size : m_item.configSize;
    const QByteArray sha = m_part == 0 ? m_item.sha256 : m_item.configSha256;
    if (m_file->size() != size || m_hash->result().toHex() != sha) {
        m_file->remove();
        m_file.reset();
        emit finished(false, T("Die heruntergeladene Datei ist beschädigt oder unerwartet (Prüfsumme falsch)."));
        return;
    }
    const QString part = m_file->fileName();
    m_file.reset();
    QString error;
    if (m_item.kind == Kind::Program || m_item.kind == Kind::Encoder || m_item.kind == Kind::Speech) {
        error = unpack(part);
        QFile::remove(part);
    } else {
        const QString target = part.chopped(5); // without ".part"
        QFile::remove(target);
        if (!QFile::rename(part, target)) error = T("Datei konnte nicht abgelegt werden: %1").arg(target);
    }
    // A voice: then its config (the model alone is no use; installedPath wants both)
    if (error.isEmpty() && m_item.kind == Kind::Voice && m_part == 0) {
        m_part = 1;
        if (!startPart(&error)) emit finished(false, error);
        return;
    }
    if (!error.isEmpty()) emit finished(false, error);
    else emit finished(true, {});
}

QString Installer::unpack(const QString& archive)
{
    // Unpack next to the old version, then swap: a failed update keeps the working one
    const bool encoder = m_item.kind == Kind::Encoder, piper = m_item.kind == Kind::Speech;
    const QDir dir(encoder ? encoderDir() : piper ? piperDir() : whisperDir());
    const QString target = encoder ? encoderTree() : piper ? piperBin() : binDir();
    const char* program = encoder ? kFfmpegName : piper ? kPiperName : kProgramName;
    const QString staging = dir.filePath("bin.new");
    QDir(staging).removeRecursively();
    dir.mkpath("bin.new");
    QProcess tar;
    tar.start(Bundle::tool("tar"), {"-xf", archive, "-C", staging});
    if (!tar.waitForFinished(300000) || tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
        QDir(staging).removeRecursively();
        return T("Entpacken fehlgeschlagen: %1").arg(QString::fromLocal8Bit(tar.readAllStandardError()).trimmed());
    }
    // The archive has a folder (whisper-bin-ubuntu-x64/, Release/, piper/, ffmpeg-…-gpl-shared-8.1/bin/): whisper and
    // piper keep the folder with the program (piper: its libraries and espeak-ng-data), the encoder the whole tree
    // above its bin/ (lib/ beside it)
    QString programPath;
    QDirIterator it(staging, {program}, QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) programPath = it.next();
    if (programPath.isEmpty()) {
        QDir(staging).removeRecursively();
        return T("Im Archiv fehlt %1.").arg(program);
    }
    QString keep = QFileInfo(programPath).absolutePath();
    if (encoder && QFileInfo(keep).fileName() == "bin") keep = QFileInfo(keep).absolutePath();
    QDir(target).removeRecursively();
    if (!QDir().rename(keep, target)) {
        QDir(staging).removeRecursively();
        return T("Programm konnte nicht abgelegt werden: %1").arg(target);
    }
    QDir(staging).removeRecursively();
    const QString installed = encoder ? installedFfmpeg() : QDir(target).filePath(program);
    QFile::setPermissions(installed, QFile::permissions(installed) | QFile::ExeOwner | QFile::ExeUser);
    return {};
}

} // namespace Extensions
