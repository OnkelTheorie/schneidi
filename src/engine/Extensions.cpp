// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Extensions.h"

#include "core/I18n.h"
#include "engine/Bundle.h"

#include <QCoreApplication>
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
#include <QUuid>

#include <thread>

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
constexpr const char* kOnnxRuntimeName = "onnxruntime.dll";
#elif defined(Q_OS_MACOS)
constexpr const char* kProgramName = "whisper-cli";
constexpr const char* kFfmpegName = "ffmpeg";
constexpr const char* kPiperName = "piper";
constexpr const char* kOnnxRuntimeName = "libonnxruntime.dylib";
#else
constexpr const char* kProgramName = "whisper-cli";
constexpr const char* kFfmpegName = "ffmpeg";
constexpr const char* kPiperName = "piper";
constexpr const char* kOnnxRuntimeName = "libonnxruntime.so";
#endif
// ONNX Runtime 1.20.1 (CPU build) from the GitHub releases of microsoft/onnxruntime
constexpr const char* kOnnxRelease = "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/";
// YuNet in opencv/opencv_zoo, one commit: the model is a Git LFS file (media.githubusercontent.com), its licence not
constexpr const char* kOpencvZoo = "https://media.githubusercontent.com/media/opencv/opencv_zoo/"
                                   "48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_detection_yunet/";
constexpr const char* kOpencvZooRaw = "https://raw.githubusercontent.com/opencv/opencv_zoo/"
                                      "48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_detection_yunet/";
constexpr const char* kYunetModel = "face_detection_yunet_2023mar.onnx";
constexpr const char* kLicenseName = "LICENSE";
constexpr const char* kCutoutModel = "human_segmentation_pphumanseg_2023mar.onnx";
constexpr const char* kYamnetModel = "yamnet.onnx";
constexpr const char* kYamnetLabels = "yamnet_class_map.csv";

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
    // Voices (model cards in piper-voices). The first of a language is its default: trained from scratch on data that
    // allows any use - Ramona (M-AILABS, LibriVox recordings, public domain, BSD-style licence), LJ Speech (public
    // domain). Thorsten sounds better, but piper's model was fine-tuned from the en_US "lessac" voice, whose training
    // data (Blizzard 2013) is licensed for research only: offered with that note, never the default.
    const auto voice = [&](const char* name, const char* title, const char* description, const char* folder,
                           const char* sha, qint64 size, const char* configSha, qint64 configSize) {
        const QString file = QString(name) + ".onnx";
        const QString base = QString(kVoices) + folder + file;
        list << Item{kVoicePrefix + QString(name).section('-', 0, 1), Kind::Voice, title, description, base, sha, size,
                     file, base + ".json", configSha, configSize};
    };
    voice("de_DE-ramona-low", N_("Stimme Deutsch: Ramona"),
          N_("weiblich (Piper, 16 kHz; Trainingsdaten gemeinfrei: M-AILABS/LibriVox)"), "de/de_DE/ramona/low/",
          "a1068592532aad8c49bf3d1a01b8749c11d7dba726689ee28ad64e7b627ae4c5", 63104526,
          "64906ab7a050ac685fa3dcea044baa798f67ed77be164f9c668a69298c6487d2", 4157);
    voice("en_US-ljspeech-medium", N_("Stimme Englisch: LJ Speech"), N_("weiblich, amerikanisch (Piper, gemeinfrei)"),
          "en/en_US/ljspeech/medium/", "6f52a751e2349abe7a76735eb09dc1875298c77ea2342ffd2fef79ff81b87f22", 63531379,
          "141d612cc0a95ed7efc1ca936b845c2364967f2e9217c5dbfcf69fc4d6c65860", 4972);
    voice("de_DE-thorsten-medium", N_("Stimme Deutsch: Thorsten"),
          N_("männlich, klarer Klang – nur für private, nicht-kommerzielle Videos (Piper; weitertrainiert aus einer "
             "Stimme, deren Daten nur für die Forschung freigegeben sind)"),
          "de/de_DE/thorsten/medium/", "7e64762d8e5118bb578f2eea6207e1a35a8e0c30595010b666f983fc87bb7819", 63201294,
          "974adee790533adb273a1ac88f49027d2a1b8f0f2cf4905954a4791e79264e85", 4819);
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

    // ONNX Runtime 1.20.1 (microsoft/onnxruntime, MIT), the CPU build of its GitHub release; sums of exactly these
    // archives. They hold onnxruntime-<os>-x64-1.20.1/lib/ with the library (libonnxruntime.so* / onnxruntime.dll),
    // which Installer::unpack keeps, and LICENSE and ThirdPartyNotices.txt beside lib/, which it copies into it.
    const auto onnx = [&](const char* archive, const char* sha, qint64 size) {
        list << Item{"onnxruntime", Kind::Runtime, N_("ONNX Runtime (lokale KI-Modelle)"),
                     N_("rechnet kleine KI-Modelle offline auf dem Prozessor (für Gesichter und Ton-Ereignisse)"),
                     QString(kOnnxRelease) + archive, sha, size, archive};
    };
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    onnx("onnxruntime-win-x64-1.20.1.zip", "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581", 65585496);
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    onnx("onnxruntime-linux-x64-1.20.1.tgz", "67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105", 6191581);
#endif
    // YuNet 2023mar (opencv/opencv_zoo, MIT, Copyright (c) 2020 Shiqi Yu), pinned to one revision of the zoo; its
    // licence text of the same revision is the second download and lies beside the model as LICENSE. Offered on every
    // system: without the runtime extension a system onnxruntime or Piper's copy runs it.
    list << Item{"faces", Kind::FaceModel, N_("Gesichtserkennung (YuNet)"),
                 N_("findet Gesichter im Bild für den Material-Index (braucht ONNX Runtime)"),
                 QString(kOpencvZoo) + "face_detection_yunet_2023mar.onnx",
                 "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4", 232589, kYunetModel,
                 QString(kOpencvZooRaw) + "LICENSE", "c83b8120c50ccbd4c4f96edf53141bdd566ebb8f8e9227e415326aa1b1aba958",
                 1085, kLicenseName};

    // ---- TODO (0.17, audio events): placeholders, NOT offered until a human fills in a verified download ----
    // Entries without URL, size or SHA-256 never reach the catalog (nothing unchecked is downloaded). To enable one:
    // URL pinned to a release/revision, size in bytes and SHA-256 of exactly that file, licence read by a human.
    const auto pending = [&](const Item& item) {
        if (!item.url.isEmpty() && !item.sha256.isEmpty() && item.size > 0
            && (item.configUrl.isEmpty() || (!item.configSha256.isEmpty() && item.configSize > 0)))
            list << item;
    };
    // YAMNet (Google, Apache-2.0) as ONNX: waveform float32 [N] at 16 kHz -> scores [frames, 521] (or fixed windows
    // of 15600 samples -> [1, 521]); the class list yamnet_class_map.csv (index,mid,display_name) as the second file.
    // There is no ONNX of it to download: the official model is TFLite,
    // https://storage.googleapis.com/download.tensorflow.org/models/tflite/task_library/audio_classification/android/lite-model_yamnet_classification_tflite_1.tflite
    // with the class map
    // https://raw.githubusercontent.com/tensorflow/models/master/research/audioset/yamnet/yamnet_class_map.csv
    // (to be pinned to a commit). Needs a conversion to ONNX and a pinned host for the result: the lead's decision.
    pending(Item{"audio-events", Kind::AudioModel, N_("Ton-Ereignisse (YAMNet)"),
                 N_("erkennt Lachen, Applaus, Jubel, Musik, Sprache, Schüsse und Schreie im Ton genauer (braucht ONNX "
                    "Runtime)"),
                 /*TODO url*/ QString(), /*TODO sha256*/ QByteArray(), /*TODO size*/ 0, kYamnetModel,
                 /*TODO class map url*/ QString(), /*TODO sha256*/ QByteArray(), /*TODO size*/ 0, kYamnetLabels});
    // PP-HumanSeg 2023mar (person segmentation, PaddleSeg via opencv/opencv_zoo, Apache-2.0, (c) PaddlePaddle Authors)
    // for the compositing node "cutout" (engine/PersonMatte): RGB 1 x 3 x 192 x 192 in, background/person
    // 1 x 2 x 192 x 192 out. Same zoo revision as YuNet; its licence text is the second download, beside it as LICENSE.
    // Checked 7.11.2026: both files downloaded at that revision, sizes and SHA-256 below, LICENSE is Apache-2.0.
    list << Item{"cutout", Kind::CutoutModel, N_("Freistellen (Personen)"),
                 N_("stellt Personen ohne Green Screen frei, z. B. für Text hinter einer Person (braucht ONNX Runtime)"),
                 QString(kOpencvZoo).replace("face_detection_yunet", "human_segmentation_pphumanseg") + kCutoutModel,
                 "552d8a984054e59b5d773d24b9b12022b22046ceb2bbc4c9aaeaceb36a9ddf24", 6163938, kCutoutModel,
                 QString(kOpencvZooRaw).replace("face_detection_yunet", "human_segmentation_pphumanseg") + "LICENSE",
                 "d9cf3b547fa4232fb9285f2db52e909f10f00781aaf64a6d0bb90c55149f1363", 11437, kLicenseName};
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
QString onnxDir() { return QDir(rootDir()).filePath("onnxruntime"); }
QString onnxLib() { return QDir(onnxDir()).filePath("lib"); }
QString audioEventsDir() { return QDir(rootDir()).filePath("audio-events"); }
QString facesDir() { return QDir(rootDir()).filePath("faces"); }
QString cutoutDir() { return QDir(rootDir()).filePath("cutout"); }

} // namespace

QString soundPacksDir() { return QDir(rootDir()).filePath("sounds"); }

namespace {

QString soundPackDir(const Item& item) { return QDir(soundPacksDir()).filePath(item.id); }

// Folder an item's download is unpacked or stored in
QString downloadDir(const Item& item)
{
    switch (item.kind) {
    case Kind::Program: return whisperDir();
    case Kind::Encoder: return encoderDir();
    case Kind::Speech: return piperDir();
    case Kind::Voice: return voicesDir();
    case Kind::Runtime: return onnxDir();
    case Kind::AudioModel: return audioEventsDir();
    case Kind::FaceModel: return facesDir();
    case Kind::CutoutModel: return cutoutDir();
    case Kind::SoundPack: return soundPacksDir();
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

// The program that unpacks the archives. Windows: its own bsdtar (System32, since Windows 10 1803), which reads
// .zip; a GNU tar from MSYS2 or Git on PATH reads "C:\…" as a remote host and cannot unpack zip files.
QString tarProgram()
{
#ifdef Q_OS_WIN
    const QString root = qEnvironmentVariable("SystemRoot");
    if (!root.isEmpty()) {
        const QString own = QDir(QDir::fromNativeSeparators(root)).filePath("System32/tar.exe");
        if (QFileInfo(own).isFile()) return own;
    }
#endif
    return Bundle::tool("tar");
}

// Moves an installed folder out of the way in one step before it is replaced or deleted, where deleting file by file
// would leave a half-removed install. A program running from it (an export with this ffmpeg, a voice, a transcription)
// goes on from "<dir>.old-<random>" (Windows allows the rename, only deleting its files fails; cleanUp() takes the
// rest at the next start). A name of its own each time: a leftover a program still holds never blocks the update, and
// cleanUp() never deletes the folder just moved aside. False = the rename was refused (a file held open without
// sharing): nothing touched; *aside = where it went ("" = none)
bool moveAside(const QString& dir, QString* aside)
{
    aside->clear();
    if (!QFileInfo::exists(dir)) return true;
    QString old;
    do {
        old = dir + ".old-" + QUuid::createUuid().toString(QUuid::Id128).left(8);
    } while (QFileInfo::exists(old));
    if (!QDir().rename(dir, old)) return false;
    *aside = old;
    return true;
}

} // namespace

void cleanUp()
{
    // Every "<dir>.old*" there now (moveAside's names, "<dir>.old" of older versions); one moved aside later has a new
    // name and stays for its own caller
    QStringList left;
    for (const QString& dir : {binDir(), encoderTree(), piperBin(), onnxLib()}) {
        const QFileInfo info(dir);
        const QFileInfoList old =
            info.dir().entryInfoList({info.fileName() + ".old*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& o : old) left << o.absoluteFilePath();
    }
    // Sound packs: "<id>.old-*" of a reinstall or a removal that could not delete everything
    for (const QFileInfo& o : QDir(soundPacksDir()).entryInfoList({"*.old-*"}, QDir::Dirs | QDir::NoDotAndDotDot))
        left << o.absoluteFilePath();
    if (left.isEmpty()) return;
    // The x264 ffmpeg is ~220 MB: never on the start's way
    std::thread([left] {
        for (const QString& dir : left) QDir(dir).removeRecursively();
    }).detach();
}

void provideRuntime(const QString& program)
{
#ifdef Q_OS_WIN
    if (program.isEmpty()) return;
    const QFileInfo exe(program);
    const QString root = QDir(rootDir()).absolutePath() + '/';
    if (!exe.absoluteFilePath().startsWith(root, Qt::CaseInsensitive)) return;
    const QDir from(QDir(QCoreApplication::applicationDirPath()).filePath("msvc-runtime"));
    const QDir to = exe.absoluteDir();
    // Copied under a temporary name and then renamed: a second process (or Piper starting next to the face index)
    // never sees a half-written DLL under its real name and fails to load it
    for (const QString& dll : from.entryList({"*.dll"}, QDir::Files)) {
        if (to.exists(dll)) continue;
        const QString tmp = to.filePath(dll + ".tmp-" + QUuid::createUuid().toString(QUuid::Id128));
        if (QFile::copy(from.filePath(dll), tmp) && !QFile::rename(tmp, to.filePath(dll))) QFile::remove(tmp);
    }
#else
    Q_UNUSED(program);
#endif
}

QString missingRuntimeHint(int exitCode)
{
    // STATUS_DLL_NOT_FOUND: whisper and piper are built with MSVC and need its runtime, which a fresh Windows lacks
    if (uint32_t(exitCode) != 0xC0000135u) return {};
    return T("Es fehlt die Microsoft Visual C++ Runtime. Bitte „vc_redist.x64.exe“ von Microsoft installieren "
             "(aka.ms/vs/17/release/vc_redist.x64.exe).");
}

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
        if (!i) // "de_DE-thorsten" (ids are lower case only up to here), its file name, "thorsten"
            for (const Item& v : catalog())
                if (v.kind == Kind::Voice
                    && (v.id.toLower() == id || voiceName(v.id).toLower() == id
                        || QString(v.fileName).chopped(5).toLower() == id || voiceName(v.id).section('-', 1).toLower() == id))
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
        if ((i->kind == Kind::AudioModel || i->kind == Kind::FaceModel || i->kind == Kind::CutoutModel) && onnxRuntimeLibrary().isEmpty()
            && piperOnnxRuntime().isEmpty())
            add(find("onnxruntime"));
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
    if (item.kind == Kind::AudioModel || item.kind == Kind::FaceModel || item.kind == Kind::CutoutModel) { // model and class list / licence
        const QDir dir(downloadDir(item));
        const QString path = dir.filePath(item.fileName);
        return QFileInfo::exists(path) && QFileInfo::exists(dir.filePath(item.configName)) ? path : QString();
    }
    if (item.kind == Kind::Runtime) return onnxRuntimeLibrary();
    if (item.kind == Kind::SoundPack) {
        const QString dir = soundPackDir(item);
        return QFileInfo(dir).isDir() && soundPackLicenseError(dir).isEmpty() ? dir : QString();
    }
    const QString path = item.kind == Kind::Program  ? QDir(binDir()).filePath(kProgramName)
                         : item.kind == Kind::Speech ? QDir(piperBin()).filePath(kPiperName)
                                                     : QDir(modelsDir()).filePath(item.fileName);
    return QFileInfo::exists(path) ? path : QString();
}

bool isInstalled(const Item& item) { return !installedPath(item).isEmpty(); }

bool remove(const Item& item)
{
    if (item.kind == Kind::Program || item.kind == Kind::Encoder || item.kind == Kind::Speech
        || item.kind == Kind::Runtime) {
        QString aside;
        if (!moveAside(item.kind == Kind::Program   ? binDir()
                       : item.kind == Kind::Encoder ? encoderTree()
                       : item.kind == Kind::Runtime ? onnxLib()
                                                    : piperBin(),
                       &aside))
            return false;
        if (!aside.isEmpty()) QDir(aside).removeRecursively();
        return true;
    }
    if (item.kind == Kind::SoundPack) {
        // Moved aside first: a file of it open elsewhere (Windows: a sound on the timeline) leaves the pack whole
        // instead of half deleted; what cannot go now goes at the next start (cleanUp)
        QString aside;
        if (!moveAside(soundPackDir(item), &aside)) return false;
        if (!aside.isEmpty()) QDir(aside).removeRecursively();
        return true;
    }
    if (item.kind == Kind::Voice) {
        const QString path = QDir(voicesDir()).filePath(item.fileName);
        QFile::remove(path + ".json");
        return !QFileInfo::exists(path) || QFile::remove(path);
    }
    if (item.kind == Kind::AudioModel || item.kind == Kind::FaceModel || item.kind == Kind::CutoutModel) {
        const QDir dir(downloadDir(item));
        const QString path = dir.filePath(item.fileName);
        QFile::remove(dir.filePath(item.configName));
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

namespace {
// The onnxruntime library in a folder (or below it): the plain name first, else a versioned one
// (libonnxruntime.so.1.21.0, libonnxruntime.1.21.0.dylib)
QString onnxRuntimeIn(const QString& dir)
{
    if (!QFileInfo(dir).isDir()) return {};
    QDirIterator exact(dir, {kOnnxRuntimeName}, QDir::Files, QDirIterator::Subdirectories);
    if (exact.hasNext()) return exact.next();
#ifdef Q_OS_WIN
    return {};
#else
    QDirIterator versioned(dir, {"libonnxruntime.so.*", "libonnxruntime.*.dylib"}, QDir::Files,
                           QDirIterator::Subdirectories);
    return versioned.hasNext() ? versioned.next() : QString();
#endif
}
} // namespace

QString onnxRuntimeLibrary() { return onnxRuntimeIn(onnxLib()); }

QString piperOnnxRuntime()
{
#ifdef Q_OS_WIN
    // Not on Windows: loaded into schneidi's process it locks Piper's folder (a mapped DLL), so updating or removing
    // Piper fails until a restart; the face and sound models bring their own runtime there
    return {};
#else
    return onnxRuntimeIn(piperBin());
#endif
}

QString audioEventsModel()
{
    const QString own = qEnvironmentVariable("SCHNEIDI_YAMNET");
    if (!own.isEmpty()) return QFileInfo(own).isFile() ? QFileInfo(own).absoluteFilePath() : QString();
    if (const Item* i = find("audio-events")) return installedPath(*i);
    // Not in the catalog yet (see the TODO there): a model put into the folder by hand works the same
    const QString path = QDir(audioEventsDir()).filePath(kYamnetModel);
    return QFileInfo::exists(path) && QFileInfo::exists(QDir(audioEventsDir()).filePath(kYamnetLabels)) ? path
                                                                                                        : QString();
}

QString audioEventsLabels()
{
    const QString model = audioEventsModel();
    if (model.isEmpty()) return {};
    const QFileInfo m(model);
    // Beside the model: the catalog's name, or <model>.csv (SCHNEIDI_YAMNET=.../my-yamnet.onnx + my-yamnet.csv)
    for (const QString& name : {QString(kYamnetLabels), m.completeBaseName() + ".csv"}) {
        const QString path = m.dir().filePath(name);
        if (QFileInfo(path).isFile()) return path;
    }
    return {};
}

QString faceModel()
{
    const QString own = qEnvironmentVariable("SCHNEIDI_YUNET");
    if (!own.isEmpty()) return QFileInfo(own).isFile() ? QFileInfo(own).absoluteFilePath() : QString();
    const Item* i = find("faces");
    return i ? installedPath(*i) : QString();
}

QString cutoutModel()
{
    const QString own = qEnvironmentVariable("SCHNEIDI_CUTOUT_MODEL");
    if (!own.isEmpty()) return QFileInfo(own).isFile() ? QFileInfo(own).absoluteFilePath() : QString();
    const Item* i = find("cutout");
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
    m_part = 0;
    dropVoicePart();
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
    const QString name = m_part == 0                   ? item.fileName
                         : !item.configName.isEmpty() ? item.configName
                                                      : item.fileName + ".json";
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

void Installer::dropVoicePart()
{
    if (!m_voicePart.isEmpty()) QFile::remove(m_voicePart);
    m_voicePart.clear();
}

void Installer::fail(const QString& message)
{
    dropVoicePart();
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
        dropVoicePart();
        emit finished(false, T("Die heruntergeladene Datei ist beschädigt oder unerwartet (Prüfsumme falsch)."));
        return;
    }
    const QString part = m_file->fileName();
    m_file.reset();
    QString error;
    if (m_item.kind == Kind::Program || m_item.kind == Kind::Encoder || m_item.kind == Kind::Speech
        || m_item.kind == Kind::Runtime) {
        error = unpack(part);
        QFile::remove(part);
    } else if (m_item.kind == Kind::SoundPack) {
        error = installSoundPack(part, soundPackDir(m_item));
        QFile::remove(part);
    } else if (!m_item.configUrl.isEmpty() && m_part == 0) {
        // A voice (an audio model, the face model): first its config (class list, licence) too (the model alone is
        // no use; installedPath wants both), then both in place
        m_voicePart = part;
        m_part = 1;
        if (!startPart(&error)) {
            dropVoicePart();
            emit finished(false, error);
        }
        return;
    } else {
        // The config first out of the way: should a rename fail, the voice is missing rather than mismatched
        const QString model = m_voicePart.isEmpty() ? QString() : m_voicePart.chopped(5);
        const QString target = part.chopped(5); // without ".part"
        QFile::remove(target);
        if (!model.isEmpty()) {
            QFile::remove(model);
            if (!QFile::rename(m_voicePart, model)) error = T("Datei konnte nicht abgelegt werden: %1").arg(model);
            dropVoicePart();
        }
        if (!error.isEmpty()) QFile::remove(part);
        else if (!QFile::rename(part, target)) error = T("Datei konnte nicht abgelegt werden: %1").arg(target);
    }
    if (!error.isEmpty()) emit finished(false, error);
    else emit finished(true, {});
}

QStringList installedSoundPacks()
{
    QStringList out;
    for (const QFileInfo& fi : QDir(soundPacksDir()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (!fi.fileName().endsWith(".new") && !fi.fileName().contains(".old-")
            && soundPackLicenseError(fi.absoluteFilePath()).isEmpty())
            out << fi.absoluteFilePath();
    return out;
}

QString soundPackLicenseError(const QString& dir)
{
    // LICENSE, LICENCE, COPYING, with or without an extension (LICENSE.txt, License.md), at the top or one folder down
    // (an archive with its own folder)
    const auto has = [](const QString& d) {
        for (const QFileInfo& fi : QDir(d).entryInfoList(QDir::Files)) {
            const QString base = fi.completeBaseName().toUpper();
            if (base == "LICENSE" || base == "LICENCE" || base == "COPYING") return true;
        }
        return false;
    };
    if (has(dir)) return {};
    for (const QFileInfo& sub : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (has(sub.absoluteFilePath())) return {};
    return T("Im Klangpaket fehlt die Lizenzdatei (LICENSE): nicht installiert.");
}

QString installSoundPack(const QString& archive, const QString& target)
{
    const QString staging = target + ".new";
    QDir(staging).removeRecursively();
    QDir().mkpath(staging);
    QProcess tar;
    tar.start(tarProgram(), {"-xf", archive, "-C", staging});
    if (!tar.waitForFinished(300000) || tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
        QDir(staging).removeRecursively();
        return T("Entpacken fehlgeschlagen: %1").arg(QString::fromLocal8Bit(tar.readAllStandardError()).trimmed());
    }
    if (const QString error = soundPackLicenseError(staging); !error.isEmpty()) {
        QDir(staging).removeRecursively();
        return error;
    }
    // The old version out of the way first and back if the new one cannot take its place (like unpack())
    QString aside;
    if (!moveAside(target, &aside)) {
        QDir(staging).removeRecursively();
        return T("Datei konnte nicht abgelegt werden: %1").arg(target);
    }
    if (!QDir().rename(staging, target)) {
        if (!aside.isEmpty()) QDir().rename(aside, target);
        QDir(staging).removeRecursively();
        return T("Datei konnte nicht abgelegt werden: %1").arg(target);
    }
    if (!aside.isEmpty()) QDir(aside).removeRecursively();
    return {};
}

QString Installer::unpack(const QString& archive)
{
    // Unpack next to the old version, then swap: a failed update keeps the working one
    const bool encoder = m_item.kind == Kind::Encoder, piper = m_item.kind == Kind::Speech;
    const bool onnx = m_item.kind == Kind::Runtime;
    const QDir dir(encoder ? encoderDir() : piper ? piperDir() : onnx ? onnxDir() : whisperDir());
    const QString target = encoder ? encoderTree() : piper ? piperBin() : onnx ? onnxLib() : binDir();
    const char* program = encoder ? kFfmpegName : piper ? kPiperName : onnx ? kOnnxRuntimeName : kProgramName;
    const QString staging = dir.filePath("bin.new");
    QDir(staging).removeRecursively();
    dir.mkpath("bin.new");
    QProcess tar;
    tar.start(tarProgram(), {"-xf", archive, "-C", staging});
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
    if (onnx) {
        // ONNX Runtime keeps only lib/: its licence and the notices of what it contains (beside lib/ in the archive)
        // go into it, its debug symbols and import libraries (Windows: a 300 MB .pdb) not
        const QDir from = QFileInfo(keep).dir(), to(keep);
        for (const QString& name : {QString("LICENSE"), QString("ThirdPartyNotices.txt")})
            if (from.exists(name) && !to.exists(name)) QFile::copy(from.filePath(name), to.filePath(name));
        for (const QString& name : to.entryList({"*.pdb", "*.lib"}, QDir::Files)) QFile::remove(to.filePath(name));
    }
    QString aside;
    if (!moveAside(target, &aside)) {
        QDir(staging).removeRecursively();
        return T("Wird gerade benutzt: Bitte laufende Exporte, Sprachausgabe und Transkription beenden und es dann noch "
                 "einmal versuchen.");
    }
    if (!QDir().rename(keep, target)) {
        if (!aside.isEmpty()) QDir().rename(aside, target); // the working version back
        QDir(staging).removeRecursively();
        return T("Programm konnte nicht abgelegt werden: %1").arg(target);
    }
    if (!aside.isEmpty()) QDir(aside).removeRecursively();
    QDir(staging).removeRecursively();
    if (onnx) { // a library: nothing to run; on Windows the MSVC runtime beside it (engine/Inference loads it first)
        provideRuntime(QDir(target).filePath(kOnnxRuntimeName));
        return {};
    }
    const QString installed = encoder ? installedFfmpeg() : QDir(target).filePath(program);
    QFile::setPermissions(installed, QFile::permissions(installed) | QFile::ExeOwner | QFile::ExeUser);
    if (!encoder) provideRuntime(installed);
    return {};
}

} // namespace Extensions
