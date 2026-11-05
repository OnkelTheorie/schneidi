// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Optional add-ons that schneidi downloads on request instead of shipping them (keeps the program small):
// whisper.cpp (speech to text) and its language models, an ffmpeg program with x264/x265 (better H.264/H.265 export
// without a graphics card encoder; GPL, so schneidi never links it: the export writer runs it as a separate process,
// see engine/ExternalEncoder), and Piper (text to speech, MIT; its build carries espeak-ng (GPL), so it too only ever
// runs as a separate process, see engine/Tts) with its voices, ONNX Runtime (MIT, a library loaded at run time, see
// engine/Inference) and the YuNet face model (MIT, see engine/FaceDetect). Fixed catalog with pinned versions and SHA-256 sums; everything lands in
// AppDataLocation/extensions and can be removed again.

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

enum class Kind { Program, WhisperModel, WhisperVad, Encoder, Speech, Voice, Runtime, AudioModel, SoundPack, FaceModel };

struct Item {
    QString id;          // "whisper", "whisper-model-small", "x264"
    Kind kind = Kind::Program;
    QString name;        // shown name (German source text, translated with T())
    QString description; // one line
    QString url;
    QByteArray sha256;   // hex
    qint64 size = 0;     // bytes of the download
    QString fileName;    // model: file in models/; program: archive name
    // Voice: its config file (<model>.onnx.json), a second download; audio model: its class list; face model: its
    // licence text (LICENSE, beside the model)
    QString configUrl;
    QByteArray configSha256;
    qint64 configSize = 0;
    QString configName; // file name of the second download; empty = <fileName>.json
};

// Items for this platform; "whisper-vad" (finds speech, keeps word times out of pauses) comes with "whisper"
// (withDependencies) (no whisper program where no build is offered; Linux x86_64 and Windows x64 have one)
const QVector<Item>& catalog();
const Item* find(const QString& id);
QString rootDir(); // AppDataLocation/extensions
// Items of a comma list as the CLI takes it ("whisper,small"): ids, short model names ("small" = whisper-model-small),
// short voice names ("de_DE-thorsten" = piper-voice-de_DE-thorsten) and aliases ("x265" = x264, the same program). unknown: the first id that names nothing (then the result is empty).
QVector<const Item*> parseList(const QString& list, QString* unknown = nullptr);
// The items plus what they need, in install order (whisper -> whisper, whisper-vad; a model -> whisper too if no
// whisper program is there; a voice -> piper too if it is missing); no duplicates
QVector<const Item*> withDependencies(const QVector<const Item*>& items);

bool isInstalled(const Item& item);
QString installedPath(const Item& item); // model file / whisper-cli / ffmpeg; empty = not installed
bool remove(const Item& item);
// Removes folders of old versions a running program still held at the last update/removal; in the background, at start
void cleanUp();
// Windows: whisper and piper are built with MSVC and need its runtime (VCRUNTIME140, MSVCP140 …), which a fresh Windows
// lacks. The package carries those DLLs in "msvc-runtime" next to schneidi.exe (Microsoft lets them be redistributed);
// this copies the missing ones next to an installed extension program. No-op elsewhere and for programs outside the
// extensions folder (an own whisper-cli on PATH)
void provideRuntime(const QString& program);
// The user's fix when an extension program could not start for a missing system library (Windows: the MSVC runtime of
// whisper/piper); empty = not that
QString missingRuntimeHint(int exitCode);

// whisper-cli to use: the installed one, else one on PATH; empty = none
QString whisperProgram();
QString whisperVadModel(); // empty = not installed (transcribing works without, word times are worse)
// Installed models, best first (ids)
QStringList installedWhisperModels();
// Model file for an id or a path; empty id = best installed. Empty result = not installed.
QString whisperModelPath(const QString& idOrPath = {});

// ffmpeg program of the x264/x265 extension (or the one SCHNEIDI_ENCODER_FFMPEG names); empty = not installed.
// Whether it really has libx264/libx265: HardwareEncoders::externalProgram.
QString encoderProgram();

// piper program: the installed one, or the one SCHNEIDI_PIPER names (tests, an own build); empty = none
QString piperProgram();
// Installed voices (ids, catalog order)
QStringList installedVoices();
// Voice model (.onnx) for an id, its short name ("de_DE-thorsten") or a path to an .onnx file whose .onnx.json lies
// beside it; empty = not installed / not found
QString voiceModelPath(const QString& idOrPath);
// Short name of a voice id: "piper-voice-de_DE-thorsten" -> "de_DE-thorsten"
QString voiceName(const QString& id);

// ONNX Runtime (MIT) for local models (engine/Inference): the library of the "onnxruntime" extension; empty = not
// installed
QString onnxRuntimeLibrary();
// The onnxruntime library inside the Piper extension (piper ships one); empty = none (always on Windows: it would
// lock Piper's folder while loaded)
QString piperOnnxRuntime();
// YAMNet (sound event classes, Apache-2.0) of the "audio-events" extension, or of SCHNEIDI_YAMNET (a model file with
// its class list beside it): the model (.onnx) and its class list (yamnet_class_map.csv: index,mid,display_name);
// empty = not installed
QString audioEventsModel();
QString audioEventsLabels();
// YuNet 2023mar (face detection, MIT) of the "faces" extension, or of SCHNEIDI_YUNET (a model file; tests): the model
// (.onnx); empty = not installed. Its licence text lies beside it as LICENSE.
QString faceModel();

// Sound packs (op 'sfx', Effects Library "Klänge"): free sound collections (CC0) as archives, each unpacked into
// soundPacksDir()/<id>. A pack must carry its licence: without a LICENSE (LICENCE, COPYING, also .txt/.md) file at
// its top or one folder down it is not installed.
QString soundPacksDir(); // AppDataLocation/extensions/sounds
QStringList installedSoundPacks(); // folders of the installed packs
// Error text when `dir` holds no licence file; empty = it has one
QString soundPackLicenseError(const QString& dir);
// Unpacks a downloaded (checked) pack archive into `target`, replacing an older version only when it worked and
// has its licence; error text, empty = ok
QString installSoundPack(const QString& archive, const QString& target);

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
    void dropVoicePart();
    QString unpack(const QString& archive); // error message, empty = ok
    bool startPart(QString* error); // the download of m_part (0 = the item's file, 1 = a voice's config)

    QNetworkAccessManager* m_net = nullptr;
    QNetworkReply* m_reply = nullptr;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    Item m_item;
    int m_part = 0;
    // A voice's model, downloaded and checked, waits as its part file until the config is there too: an update whose
    // config download fails keeps the old pair instead of a new model beside the old config
    QString m_voicePart;
    bool m_cancelled = false;
};

} // namespace Extensions
