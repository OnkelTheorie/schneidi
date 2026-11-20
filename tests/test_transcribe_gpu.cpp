// SPDX-License-Identifier: AGPL-3.0-only
// whisper on the graphics card: a ggml GPU backend beside whisper-cli (libggml-vulkan.so / ggml-vulkan.dll) is what
// makes it compute there (Extensions::whisperGpuBackend). When whisper fails on the GPU (a crashing driver, too little
// video memory), the Transcriber runs it once more on the CPU (-ng) instead of failing; without a GPU backend a failure
// stays a failure, and a run that works on the GPU never gets -ng.
// whisper itself is not needed: this program copies itself as "whisper-cli" and logs its arguments.
#include "check.h"

#include "engine/Extensions.h"
#include "engine/Transcriber.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace {

// --- The fake whisper-cli: appends its arguments to FAKE_WHISPER_LOG; with FAKE_GPU_FAILS set it fails unless
// told to stay off the GPU (-ng), else it writes one word
int fakeWhisper(int argc, char** argv)
{
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    QFile log(qEnvironmentVariable("FAKE_WHISPER_LOG"));
    if (log.open(QIODevice::Append)) log.write(args.join(' ').toUtf8() + '\n');
    if (qEnvironmentVariableIsSet("FAKE_GPU_FAILS") && !args.contains("-ng")) {
        std::fputs("ggml_vulkan: device lost\n", stderr);
        return 3;
    }
    const QString out = args.value(args.indexOf("-of") + 1);
    const QJsonObject json{{"result", QJsonObject{{"language", "de"}}},
                           {"transcription", QJsonArray{QJsonObject{{"text", " hallo"},
                                                                    {"offsets", QJsonObject{{"from", 100}, {"to", 900}}}}}}};
    QFile o(out + ".json");
    if (!o.open(QIODevice::WriteOnly)) return 4;
    o.write(QJsonDocument(json).toJson());
    return 0;
}

struct Outcome {
    bool ok = false;
    QString error;
    QStringList runs; // whisper's argument lines
};

Outcome transcribe(const QString& media, const QString& model, const QString& logPath)
{
    QFile::remove(logPath);
    Transcriber t;
    TranscribeRequest req;
    req.media = media;
    req.model = model;
    req.language = "de";
    Outcome o;
    QEventLoop loop;
    QObject::connect(&t, &Transcriber::finished, &loop, [&](bool ok, const QString& error) {
        o.ok = ok;
        o.error = error;
        loop.quit();
    });
    QString error;
    if (!t.start(req, &error)) {
        o.error = error;
        return o;
    }
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();
    QFile log(logPath);
    if (log.open(QIODevice::ReadOnly))
        for (const QByteArray& line : log.readAll().split('\n'))
            if (!line.isEmpty()) o.runs << QString::fromUtf8(line);
    return o;
}

} // namespace

int main(int argc, char** argv)
{
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).baseName() == QLatin1String("whisper-cli")) return fakeWhisper(argc, argv);
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("transcribe_gpu");

    QTemporaryDir dir;
    // The fake whisper on the PATH (the extension is not installed in the isolated test folders)
    QDir().mkpath(dir.filePath("bin"));
#ifdef Q_OS_WIN
    const QString fake = dir.filePath("bin/whisper-cli.exe");
    const QString backend = dir.filePath("bin/ggml-vulkan.dll");
#else
    const QString fake = dir.filePath("bin/whisper-cli");
    const QString backend = dir.filePath("bin/libggml-vulkan.so");
#endif
    CHECK(QFile::copy(QCoreApplication::applicationFilePath(), fake));
    QFile::setPermissions(fake, QFile::permissions(fake) | QFileDevice::ExeOwner);
    qputenv("PATH", (QDir::toNativeSeparators(dir.filePath("bin")) + QDir::listSeparator() + qEnvironmentVariable("PATH"))
                        .toUtf8());
    const QString logPath = dir.filePath("whisper.log");
    qputenv("FAKE_WHISPER_LOG", QDir::toNativeSeparators(logPath).toUtf8());
    if (!CHECK_EQ(QFileInfo(Extensions::whisperProgram()).canonicalFilePath(), QFileInfo(fake).canonicalFilePath()))
        return Check::result();

    // Material: one second of a tone (not silence: whisper's words of a silent sound are dropped) and a model file
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        std::puts("skipped: no ffmpeg");
        return 77;
    }
    const QString media = dir.filePath("tone.wav");
    QProcess::execute(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                               "sine=frequency=440:duration=1", media});
    const QString model = dir.filePath("ggml-model.bin");
    {
        QFile m(model);
        CHECK(m.open(QIODevice::WriteOnly));
        m.write("model");
    }
    if (!CHECK(QFile::exists(media))) return Check::result();

    // 1. No GPU backend: CPU, one run, no -ng (whisper decides itself); a failure stays one
    CHECK(Extensions::whisperGpuBackend().isEmpty());
    {
        const Outcome o = transcribe(media, model, logPath);
        CHECK(o.ok);
        CHECK_EQ(o.runs.size(), 1);
        CHECK(!o.runs.value(0).contains("-ng"));
        qputenv("FAKE_GPU_FAILS", "1");
        const Outcome failed = transcribe(media, model, logPath);
        qunsetenv("FAKE_GPU_FAILS");
        // (the fake fails without -ng; with no GPU backend there is no second try)
        CHECK(!failed.ok);
        CHECK_EQ(failed.runs.size(), 1);
        CHECK(failed.error.contains("device lost"));
    }

    // 2. A GPU backend beside it: found; a run that works stays on the GPU
    {
        QFile b(backend);
        CHECK(b.open(QIODevice::WriteOnly));
    }
    CHECK_EQ(QFileInfo(Extensions::whisperGpuBackend()).canonicalFilePath(), QFileInfo(backend).canonicalFilePath());
    {
        const Outcome o = transcribe(media, model, logPath);
        CHECK(o.ok);
        CHECK_EQ(o.runs.size(), 1);
        CHECK(!o.runs.value(0).contains("-ng"));
    }

    // 3. It fails on the GPU: once more on the CPU (-ng), and that result counts
    qputenv("FAKE_GPU_FAILS", "1");
    {
        const Outcome o = transcribe(media, model, logPath);
        CHECK(o.ok);
        if (!o.ok) std::printf("       error: %s\n", qPrintable(o.error));
        CHECK_EQ(o.runs.size(), 2);
        CHECK(!o.runs.value(0).contains("-ng"));
        CHECK(o.runs.value(1).contains("-ng"));
        // The next request tries the GPU again (a passing driver hiccup does not make it CPU for good)
        const Outcome next = transcribe(media, model, logPath);
        CHECK(!next.runs.value(0).contains("-ng"));
    }
    qunsetenv("FAKE_GPU_FAILS");
    return Check::result();
}
