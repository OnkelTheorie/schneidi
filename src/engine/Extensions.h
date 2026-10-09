#pragma once
// Optional add-ons that schneidi downloads on request instead of shipping them (keeps the program small):
// whisper.cpp (speech to text) and its language models. Fixed catalog with pinned versions and SHA-256 sums;
// everything lands in AppDataLocation/extensions and can be removed again.

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QCryptographicHash;

namespace Extensions {

enum class Kind { Program, WhisperModel, WhisperVad };

struct Item {
    QString id;          // "whisper", "whisper-model-small"
    Kind kind = Kind::Program;
    QString name;        // shown name (German source text, translated with T())
    QString description; // one line
    QString url;
    QByteArray sha256;   // hex
    qint64 size = 0;     // bytes of the download
    QString fileName;    // model: file in models/; program: archive name
};

// Items for this platform; "whisper-vad" (finds speech, keeps word times out of pauses) comes with "whisper"
// (withDependencies) (no whisper program where no build is offered; Linux x86_64 and Windows x64 have one)
const QVector<Item>& catalog();
const Item* find(const QString& id);
QString rootDir(); // AppDataLocation/extensions
// The items plus what they need, in install order (whisper -> whisper, whisper-vad; a model -> whisper too if no
// whisper program is there); no duplicates
QVector<const Item*> withDependencies(const QVector<const Item*>& items);

bool isInstalled(const Item& item);
QString installedPath(const Item& item); // model file / whisper-cli; empty = not installed
bool remove(const Item& item);

// whisper-cli to use: the installed one, else one on PATH; empty = none
QString whisperProgram();
QString whisperVadModel(); // empty = not installed (transcribing works without, word times are worse)
// Installed models, best first (ids)
QStringList installedWhisperModels();
// Model file for an id or a path; empty id = best installed. Empty result = not installed.
QString whisperModelPath(const QString& idOrPath = {});

// Downloads and installs one item: checks size and SHA-256, unpacks program archives (tar, also on Windows 10+),
// replaces the old version only when everything worked.
class Installer : public QObject {
    Q_OBJECT
public:
    explicit Installer(QObject* parent = nullptr);
    ~Installer() override;
    bool start(const Item& item, QString* error);
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }

signals:
    void progress(qint64 received, qint64 total);
    void finished(bool ok, const QString& message);

private:
    void onFinished();
    void fail(const QString& message);
    QString unpack(const QString& archive); // error message, empty = ok

    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply* m_reply = nullptr;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    Item m_item;
    bool m_cancelled = false;
};

} // namespace Extensions
