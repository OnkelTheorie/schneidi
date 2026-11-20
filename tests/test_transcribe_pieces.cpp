// SPDX-License-Identifier: AGPL-3.0-only
// Transcribing in speech pieces (engine/Transcriber, 20.11.): the voice detection program beside whisper-cli finds the
// speech, the Transcriber packs it into pieces (long pauses cut short), whisper transcribes all pieces in one run with
// DTW word times and a starting text, and the words come back at their times in the whole sound. Without the voice
// detection program: the whole sound at once (whisper's own voice detection, no DTW - its times would count without
// the pauses).
// whisper itself is not needed: this program copies itself as "whisper-cli" and "whisper-vad-speech-segments".
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

void logArgs(const QStringList& args)
{
    QFile log(qEnvironmentVariable("FAKE_WHISPER_LOG"));
    if (log.open(QIODevice::Append)) log.write(args.join(' ').toUtf8() + '\n');
}

// Speech at 0.5-1.5 s and 6.0-7.0 s (centiseconds, as whisper-vad-speech-segments prints them)
int fakeVad(const QStringList& args)
{
    logArgs(QStringList{"VAD"} + args);
    std::puts("Detected 2 speech segments:");
    std::puts("Speech segment 0: start = 50.00, end = 150.00");
    std::puts("Speech segment 1: start = 600.00, end = 700.00");
    return 0;
}

QByteArray word(const QString& text, int fromMs, int dtwCs)
{
    const QJsonObject seg{{"text", " " + text},
                          {"offsets", QJsonObject{{"from", fromMs}, {"to", fromMs + 300}}},
                          {"tokens", QJsonArray{QJsonObject{{"text", " " + text}, {"t_dtw", dtwCs}}}}};
    return QJsonDocument(seg).toJson(QJsonDocument::Compact);
}

// Per input file (-f file or a bare *.wav): one word 0.5 s into it ("eins") and one 2.0 s into it ("zwei");
// the whole sound (-of out): one word
int fakeWhisper(const QStringList& args)
{
    logArgs(args);
    if (args.contains("-dl")) { // language detection only
        std::fputs("whisper_full_with_state: auto-detected language: de (p = 0.97)\n", stderr);
        return 0;
    }
    // A model without fitting alignment heads (FAKE_DTW_FAILS): whisper does not start with -dtw
    if (qEnvironmentVariableIsSet("FAKE_DTW_FAILS") && args.contains("-dtw")) return 5;
    QStringList files;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "-f") files << args.value(++i);
        else if (args[i].endsWith(".wav") && (i == 0 || !args[i - 1].startsWith('-'))) files << args[i];
    }
    const QString of = args.contains("-of") ? args.value(args.indexOf("-of") + 1) : QString();
    for (const QString& f : files) {
        const QString out = of.isEmpty() ? f + ".json" : of + ".json";
        QFile o(out);
        if (!o.open(QIODevice::WriteOnly)) return 4;
        o.write(R"({"result":{"language":"de"},"transcription":[)" + word("eins", 0, 50) + "," + word("zwei", 1500, 200)
                + "]}");
    }
    return 0;
}

struct Outcome {
    bool ok = false;
    QString error;
    QVector<Transcript::Word> words;
    QStringList runs;
};

Outcome transcribe(const QString& media, const QString& model, const QString& logPath, const QString& language = "de")
{
    QFile::remove(logPath);
    Transcriber t;
    TranscribeRequest req;
    req.media = media;
    req.model = model;
    req.language = language;
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
    o.words = t.result().words;
    QFile log(logPath);
    if (log.open(QIODevice::ReadOnly))
        for (const QByteArray& line : log.readAll().split('\n'))
            if (!line.isEmpty()) o.runs << QString::fromUtf8(line);
    return o;
}

} // namespace

int main(int argc, char** argv)
{
    const QString self = QFileInfo(QString::fromLocal8Bit(argv[0])).baseName();
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    if (self == QLatin1String("whisper-cli")) return fakeWhisper(args);
    if (self == QLatin1String("whisper-vad-speech-segments")) return fakeVad(args);
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("transcribe_pieces");

    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        std::puts("skipped: no ffmpeg");
        return 77;
    }
    QTemporaryDir dir;
    QDir().mkpath(dir.filePath("bin"));
#ifdef Q_OS_WIN
    const QString exe = ".exe";
#else
    const QString exe;
#endif
    const QString whisper = dir.filePath("bin/whisper-cli" + exe), vadProgram = dir.filePath("bin/whisper-vad-speech-segments" + exe);
    CHECK(QFile::copy(QCoreApplication::applicationFilePath(), whisper));
    QFile::setPermissions(whisper, QFile::permissions(whisper) | QFileDevice::ExeOwner);
    qputenv("PATH", (QDir::toNativeSeparators(dir.filePath("bin")) + QDir::listSeparator() + qEnvironmentVariable("PATH"))
                        .toUtf8());
    const QString logPath = dir.filePath("whisper.log");
    qputenv("FAKE_WHISPER_LOG", QDir::toNativeSeparators(logPath).toUtf8());
    if (!CHECK_EQ(QFileInfo(Extensions::whisperProgram()).canonicalFilePath(), QFileInfo(whisper).canonicalFilePath()))
        return Check::result();

    // 8 s of a tone (not silence: words in silence are dropped), a model named like large-v3 (DTW preset), the voice
    // detection model where the extension keeps it
    const QString media = dir.filePath("tone.wav");
    QProcess::execute(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                               "sine=frequency=440:duration=8", media});
    const QString model = dir.filePath("ggml-large-v3-q5_0.bin");
    {
        QFile m(model);
        CHECK(m.open(QIODevice::WriteOnly));
        m.write("model");
    }
    const Extensions::Item* vadItem = Extensions::find("whisper-vad");
    if (!CHECK(vadItem != nullptr)) return Check::result();
    const QString vadModel = QDir(Extensions::rootDir()).filePath("whisper/models/" + vadItem->fileName);
    QDir().mkpath(QFileInfo(vadModel).absolutePath());
    {
        QFile v(vadModel);
        CHECK(v.open(QIODevice::WriteOnly));
        v.write("vad");
    }
    CHECK_EQ(QFileInfo(Extensions::whisperVadModel()).canonicalFilePath(), QFileInfo(vadModel).canonicalFilePath());

    // 1. Without the voice detection program: the whole sound, whisper's own voice detection, no DTW
    CHECK(Extensions::whisperVadProgram().isEmpty());
    {
        const Outcome o = transcribe(media, model, logPath);
        CHECK(o.ok);
        if (!o.ok) std::printf("       error: %s\n", qPrintable(o.error));
        CHECK_EQ(o.runs.size(), 1);
        const QString run = o.runs.value(0);
        CHECK(run.contains("-f audio.wav") && run.contains("--vad"));
        CHECK(!run.contains("-dtw"));
        CHECK(run.contains("--prompt")); // German: the starting text with punctuation
    }

    // 2. With it: speech at 0.5-1.5 s and 6-7 s -> one piece (the 4.5 s pause cut to 0.4 s), one whisper run over it
    // with DTW; "eins" 0.5 s into the piece = 0.8 s of the sound (the piece starts at 0.3 s, padding 0.2 s), "zwei"
    // 2.0 s into it = in the second stretch (it starts at 1.4 + 0.4 = 1.8 s in the piece, at 5.8 s in the sound): 6.0 s
    CHECK(QFile::copy(QCoreApplication::applicationFilePath(), vadProgram));
    QFile::setPermissions(vadProgram, QFile::permissions(vadProgram) | QFileDevice::ExeOwner);
    CHECK_EQ(QFileInfo(Extensions::whisperVadProgram()).canonicalFilePath(), QFileInfo(vadProgram).canonicalFilePath());
    {
        const Outcome o = transcribe(media, model, logPath);
        CHECK(o.ok);
        if (!o.ok) std::printf("       error: %s\n", qPrintable(o.error));
        CHECK_EQ(o.runs.size(), 2); // voice detection, then whisper
        CHECK(o.runs.value(0).startsWith("VAD"));
        const QString run = o.runs.value(1);
        CHECK(run.contains("piece_000.wav"));
        CHECK(!run.contains("piece_001.wav"));
        CHECK(!run.contains("--vad"));
        CHECK(run.contains("-dtw large.v3") && run.contains("-nfa") && run.contains("-ojf"));
        CHECK_EQ(o.words.size(), 2);
        if (o.words.size() == 2) {
            std::printf("       eins %lld ms, zwei %lld ms\n", o.words[0].from, o.words[1].from);
            CHECK(std::abs(o.words[0].from - 800) <= 20);
            CHECK(std::abs(o.words[1].from - 6000) <= 20);
        }
    }


    // 3. Automatic language with pieces: detected once first (on the longest piece), then told to whisper
    {
        const Outcome o = transcribe(media, model, logPath, "auto");
        CHECK(o.ok);
        CHECK_EQ(o.runs.size(), 3);
        CHECK(o.runs.value(1).contains("-dl") && o.runs.value(1).contains("piece_000.wav"));
        CHECK(o.runs.value(2).contains("-l de") && o.runs.value(2).contains("--prompt"));
    }

    // 4. A model that fails with DTW word times: once more without them, and the transcript is there
    qputenv("FAKE_DTW_FAILS", "1");
    {
        const Outcome o = transcribe(media, model, logPath);
        CHECK(o.ok);
        CHECK_EQ(o.runs.size(), 3);
        CHECK(o.runs.value(1).contains("-dtw"));
        CHECK(!o.runs.value(2).contains("-dtw"));
        CHECK_EQ(o.words.size(), 2);
    }
    qunsetenv("FAKE_DTW_FAILS");
    return Check::result();
}
