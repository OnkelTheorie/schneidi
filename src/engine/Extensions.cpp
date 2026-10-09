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

#ifdef Q_OS_WIN
constexpr const char* kProgramName = "whisper-cli.exe";
#else
constexpr const char* kProgramName = "whisper-cli";
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

QVector<const Item*> withDependencies(const QVector<const Item*>& items)
{
    QVector<const Item*> out;
    const auto add = [&out](const Item* i) {
        if (i && !out.contains(i)) out << i;
    };
    for (const Item* i : items) {
        // A model is no use without the program (unless one is on PATH)
        if (i->kind == Kind::WhisperModel && whisperProgram().isEmpty()) add(find("whisper"));
        add(i);
        if (i->id == "whisper") add(find("whisper-vad"));
    }
    if (out.contains(find("whisper"))) add(find("whisper-vad"));
    return out;
}

QString installedPath(const Item& item)
{
    const QString path = item.kind == Kind::Program ? QDir(binDir()).filePath(kProgramName)
                                                    : QDir(modelsDir()).filePath(item.fileName);
    return QFileInfo::exists(path) ? path : QString();
}

bool isInstalled(const Item& item) { return !installedPath(item).isEmpty(); }

bool remove(const Item& item)
{
    if (item.kind == Kind::Program) return QDir(binDir()).removeRecursively();
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
    const QString dir = item.kind == Kind::Program ? whisperDir() : modelsDir();
    if (!QDir().mkpath(dir)) {
        if (error) *error = "cannot create " + dir;
        return false;
    }
    m_file = std::make_unique<QFile>(QDir(dir).filePath(item.fileName + ".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = m_file->errorString();
        return false;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    QNetworkRequest request{QUrl(item.url)};
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
        emit progress(received, total > 0 ? total : m_item.size);
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
    if (m_file->size() != m_item.size || m_hash->result().toHex() != m_item.sha256) {
        m_file->remove();
        m_file.reset();
        emit finished(false, T("Die heruntergeladene Datei ist beschädigt oder unerwartet (Prüfsumme falsch)."));
        return;
    }
    const QString part = m_file->fileName();
    m_file.reset();
    QString error;
    if (m_item.kind == Kind::Program) {
        error = unpack(part);
        QFile::remove(part);
    } else {
        const QString target = part.chopped(5); // without ".part"
        QFile::remove(target);
        if (!QFile::rename(part, target)) error = T("Datei konnte nicht abgelegt werden: %1").arg(target);
    }
    if (!error.isEmpty()) emit finished(false, error);
    else emit finished(true, {});
}

QString Installer::unpack(const QString& archive)
{
    // Unpack next to the old version, then swap: a failed update keeps the working one
    const QDir dir(whisperDir());
    const QString staging = dir.filePath("bin.new");
    QDir(staging).removeRecursively();
    dir.mkpath("bin.new");
    QProcess tar;
    tar.start(Bundle::tool("tar"), {"-xf", archive, "-C", staging});
    if (!tar.waitForFinished(120000) || tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
        QDir(staging).removeRecursively();
        return T("Entpacken fehlgeschlagen: %1").arg(QString::fromLocal8Bit(tar.readAllStandardError()).trimmed());
    }
    // The archive has a folder (whisper-bin-ubuntu-x64/, Release/): use the one with the program
    QString programDir;
    QDirIterator it(staging, {kProgramName}, QDir::Files, QDirIterator::Subdirectories);
    if (it.hasNext()) programDir = QFileInfo(it.next()).absolutePath();
    if (programDir.isEmpty()) {
        QDir(staging).removeRecursively();
        return T("Im Archiv fehlt %1.").arg(kProgramName);
    }
    QDir(binDir()).removeRecursively();
    if (!QDir().rename(programDir, binDir())) {
        QDir(staging).removeRecursively();
        return T("Programm konnte nicht abgelegt werden: %1").arg(binDir());
    }
    QDir(staging).removeRecursively();
    QFile::setPermissions(QDir(binDir()).filePath(kProgramName),
                          QFile::permissions(QDir(binDir()).filePath(kProgramName)) | QFile::ExeOwner | QFile::ExeUser);
    return {};
}

} // namespace Extensions
