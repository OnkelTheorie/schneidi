// SPDX-License-Identifier: AGPL-3.0-only
// Speech recognition "Sehr schnell" with NVIDIA Parakeet (engine/Transcriber, 23.11.): parakeet-cli beside whisper-cli
// hears the speech pieces of the voice detection (or, without it, the sound in pieces of up to 28 s) in one run, its
// token times (-ps) become the words at their times in the whole sound. A language Parakeet does not know (chosen, or
// detected first by a whisper model) goes to the request's whisper model. Text said twice stays (no repeat filter).
// Neither whisper nor Parakeet is needed: this program copies itself as "whisper-cli", "parakeet-cli" and
// "whisper-vad-speech-segments".
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

void logArgs(const QString& program, const QStringList& args)
{
    QFile log(qEnvironmentVariable("FAKE_LOG"));
    if (log.open(QIODevice::Append)) log.write((program + ' ' + args.join(' ')).toUtf8() + '\n');
}

// Speech at 0.5-1.5 s and 6.0-7.0 s (centiseconds, as whisper-vad-speech-segments prints them)
int fakeVad(const QStringList& args)
{
    logArgs("VAD", args);
    std::puts("Detected 2 speech segments:");
    std::puts("Speech segment 0: start = 50.00, end = 150.00");
    std::puts("Speech segment 1: start = 600.00, end = 700.00");
    return 0;
}

// Language detection (-dl) says FAKE_LANG (default de); a transcription writes one word per file
int fakeWhisper(const QStringList& args)
{
    logArgs("WHISPER", args);
    if (args.contains("-dl")) {
        std::fprintf(stderr, "whisper_full_with_state: auto-detected language: %s (p = 0.97)\n",
                     qPrintable(qEnvironmentVariable("FAKE_LANG", "de")));
        return 0;
    }
    for (int i = 0; i < args.size(); ++i) {
        const bool file = args[i].endsWith(".wav") && (i == 0 || !args[i - 1].startsWith('-'));
        if (!file && args[i] != "-f") continue;
        const QString f = args[i] == "-f" ? args.value(++i) : args[i];
        const QString of = args.contains("-of") ? args.value(args.indexOf("-of") + 1) : QString();
        QFile o(of.isEmpty() ? f + ".json" : of + ".json");
        if (!o.open(QIODevice::WriteOnly)) return 4;
        const QJsonObject seg{{"text", " whisper"}, {"offsets", QJsonObject{{"from", 500}, {"to", 900}}}};
        o.write(QJsonDocument(QJsonObject{{"result", QJsonObject{{"language", "ja"}}},
                                          {"transcription", QJsonArray{seg}}})
                    .toJson(QJsonDocument::Compact));
    }
    return 0;
}

// parakeet-cli -ps: per file a "Processing file:" line and its tokens (t0/t1 in 10 ms) on stderr, like b5454 prints
// them. "Eins," 0.5 s into the piece (a word mark alone, then the word: still one word), "zwei." 2.0 s into it;
// FAKE_TWICE: the piece's text twice
int fakeParakeet(const QStringList& args)
{
    logArgs("PARAKEET", args);
    const auto token = [](int i, int t0, int t1, bool start, const char* text) {
        std::fprintf(stderr, "  [%2d] id=  515 frame= 48 dur_idx= 4 dur_val= 4 p=1.0000 plog=0.0000 t0= %d t1= %d "
                             "word_start=%s \"%s\"\r\n",
                     i, t0, t1, start ? "true" : "false", text);
    };
    for (const QString& a : args) {
        if (!a.endsWith(".wav")) continue;
        std::fprintf(stderr, "\nProcessing file: %s\nread_audio_data: reading audio data from '%s' ...\n", qPrintable(a),
                     qPrintable(a));
        std::fputs("Segments (1):\nSegment 0: [0 -> 250] \"Eins, zwei drei vier fünf.\"\nTokens [6]:\n", stderr);
        const int copies = qEnvironmentVariableIsSet("FAKE_TWICE") ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            const int at = c * 30;
            token(0, 50 + at, 52 + at, true, "\xe2\x96\x81");
            token(1, 52 + at, 60 + at, false, "Eins");
            token(2, 60 + at, 62 + at, false, ",");
            token(3, 200 + at, 204 + at, true, "\xe2\x96\x81zwei");
            token(4, 204 + at, 206 + at, true, "\xe2\x96\x81" "drei");
            token(5, 206 + at, 208 + at, true, "\xe2\x96\x81vier");
            token(6, 208 + at, 212 + at, true, "\xe2\x96\x81" "f\xc3\xbcnf");
            token(7, 212 + at, 214 + at, false, ".");
        }
        std::printf("Eins, zwei drei vier fünf.\n");
    }
    return 0;
}

struct Outcome {
    bool ok = false;
    QString error, note, used, language;
    QVector<Transcript::Word> words;
    QStringList runs;
};

Outcome transcribe(const QString& media, const QString& model, const QString& whisperModel, const QString& language)
{
    const QString logPath = qEnvironmentVariable("FAKE_LOG");
    QFile::remove(logPath);
    Transcriber t;
    TranscribeRequest req;
    req.media = media;
    req.model = model;
    req.whisperModel = whisperModel;
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
    o.language = t.result().language;
    o.note = t.engineNote();
    o.used = QFileInfo(t.usedModel()).fileName();
    QFile log(logPath);
    if (log.open(QIODevice::ReadOnly))
        for (const QByteArray& line : log.readAll().split('\n'))
            if (!line.isEmpty()) o.runs << QString::fromUtf8(line);
    if (!o.ok) std::printf("       error: %s\n", qPrintable(o.error));
    return o;
}

QString text(const QVector<Transcript::Word>& words)
{
    QStringList out;
    for (const auto& w : words) out << w.text;
    return out.join(' ');
}

void touch(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data);
}

} // namespace

int main(int argc, char** argv)
{
    const QString self = QFileInfo(QString::fromLocal8Bit(argv[0])).baseName();
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    if (self == QLatin1String("whisper-cli")) return fakeWhisper(args);
    if (self == QLatin1String("parakeet-cli")) return fakeParakeet(args);
    if (self == QLatin1String("whisper-vad-speech-segments")) return fakeVad(args);
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("transcribe_parakeet");

    // Pure parts: which model is Parakeet, which languages it knows, the quality levels
    CHECK(Transcriber::isParakeet("/x/ggml-parakeet-tdt-0.6b-v3-q8_0.bin"));
    CHECK(!Transcriber::isParakeet("/x/ggml-large-v3-q5_0.bin"));
    CHECK(Transcriber::parakeetLanguage("de") && Transcriber::parakeetLanguage("uk") && Transcriber::parakeetLanguage("en"));
    CHECK(!Transcriber::parakeetLanguage("tr") && !Transcriber::parakeetLanguage("ja") && !Transcriber::parakeetLanguage("auto"));
    CHECK_EQ(Extensions::qualityModel("accurate"), QString("whisper-model-large"));
    CHECK_EQ(Extensions::qualityModel("fast"), QString("whisper-model-turbo"));
    CHECK_EQ(Extensions::qualityModel("very_fast"), QString("parakeet"));
    CHECK(Extensions::qualityModel("best").isEmpty());

    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) {
        std::puts("skipped: no ffmpeg");
        return Check::result() ? Check::result() : 77;
    }
    QTemporaryDir dir;
    QDir().mkpath(dir.filePath("bin"));
#ifdef Q_OS_WIN
    const QString exe = ".exe";
#else
    const QString exe;
#endif
    const auto install = [&](const QString& name) {
        const QString path = dir.filePath("bin/" + name + exe);
        CHECK(QFile::copy(QCoreApplication::applicationFilePath(), path));
        QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeOwner);
        return path;
    };
    const QString whisper = install("whisper-cli");
    qputenv("PATH", (QDir::toNativeSeparators(dir.filePath("bin")) + QDir::listSeparator() + qEnvironmentVariable("PATH"))
                        .toUtf8());
    qputenv("FAKE_LOG", QDir::toNativeSeparators(dir.filePath("runs.log")).toUtf8());
    if (!CHECK_EQ(QFileInfo(Extensions::whisperProgram()).canonicalFilePath(), QFileInfo(whisper).canonicalFilePath()))
        return Check::result();

    // 8 s of a tone (words in silence are dropped), Parakeet's and a whisper model file, the voice detection model
    const QString media = dir.filePath("tone.wav");
    QProcess::execute(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                               "sine=frequency=440:duration=8", media});
    const QString model = dir.filePath("ggml-parakeet-tdt-0.6b-v3-q8_0.bin"), large = dir.filePath("ggml-large-v3-q5_0.bin");
    touch(model, "model");
    touch(large, "model");
    const Extensions::Item* vadItem = Extensions::find("whisper-vad");
    if (!CHECK(vadItem != nullptr)) return Check::result();
    touch(QDir(Extensions::rootDir()).filePath("whisper/models/" + vadItem->fileName), "vad");

    // No parakeet-cli: a clear error before anything runs
    {
        const Outcome o = transcribe(media, model, large, "de");
        CHECK(!o.ok);
        CHECK(o.error.contains("parakeet-cli"));
    }
    const QString parakeet = install("parakeet-cli");
    CHECK_EQ(QFileInfo(Extensions::parakeetProgram()).canonicalFilePath(), QFileInfo(parakeet).canonicalFilePath());

    // 1. Without the voice detection program: the sound in pieces of up to 28 s (8 s = one), Parakeet with token times;
    // "Eins," at 0.5 s (its bare word mark and the comma belong to it), "zwei." at 2.0 s
    {
        const Outcome o = transcribe(media, model, large, "de");
        CHECK(o.ok);
        CHECK_EQ(o.runs.size(), 1);
        const QString run = o.runs.value(0);
        CHECK(run.startsWith("PARAKEET") && run.contains("-ps") && run.contains("piece_000.wav"));
        CHECK(!run.contains("piece_001.wav"));
        CHECK_EQ(text(o.words), QString("Eins, zwei drei vier fünf."));
        if (CHECK_EQ(o.words.size(), 5)) {
            CHECK_EQ(o.words[0].from, 500);
            CHECK_EQ(o.words[0].to, 620);
            CHECK_EQ(o.words[1].from, 2000);
        }
        CHECK_EQ(o.language, QString("de"));
        CHECK(o.note.isEmpty());
    }

    // 2. With the voice detection: speech at 0.5-1.5 s and 6-7 s -> one piece (the 4.5 s pause cut to 0.4 s); "Eins,"
    // 0.5 s into it = 0.8 s of the sound, "zwei." 2.0 s into it = 6.0 s
    install("whisper-vad-speech-segments");
    {
        const Outcome o = transcribe(media, model, large, "de");
        CHECK(o.ok);
        CHECK_EQ(o.runs.size(), 2);
        CHECK(o.runs.value(0).startsWith("VAD"));
        CHECK(o.runs.value(1).startsWith("PARAKEET"));
        if (CHECK_EQ(o.words.size(), 5)) {
            CHECK(std::abs(o.words[0].from - 800) <= 20);
            CHECK(std::abs(o.words[1].from - 6000) <= 20);
        }
    }

    // 3. Automatic language: a whisper model tells it first (on the longest piece); Parakeet knows German
    {
        const Outcome o = transcribe(media, model, large, "auto");
        CHECK(o.ok);
        CHECK_EQ(o.runs.size(), 3);
        CHECK(o.runs.value(1).startsWith("WHISPER") && o.runs.value(1).contains("-dl"));
        CHECK(o.runs.value(1).contains("ggml-large-v3-q5_0.bin")); // no smaller whisper model installed
        CHECK(o.runs.value(2).startsWith("PARAKEET"));
        CHECK_EQ(o.language, QString("de"));
        CHECK_EQ(o.used, QString("ggml-parakeet-tdt-0.6b-v3-q8_0.bin"));
    }

    // 4. ... Japanese, which it does not know: whisper writes it, and says so
    qputenv("FAKE_LANG", "ja");
    {
        const Outcome o = transcribe(media, model, large, "auto");
        CHECK(o.ok);
        CHECK_EQ(o.runs.size(), 3);
        CHECK(o.runs.value(2).startsWith("WHISPER") && o.runs.value(2).contains("-l ja"));
        CHECK_EQ(o.used, QString("ggml-large-v3-q5_0.bin"));
        CHECK_EQ(text(o.words), QString("whisper"));
        CHECK(o.note.contains("ja"));
    }
    qunsetenv("FAKE_LANG");

    // 5. A chosen language it does not know: whisper from the start; without a whisper model Parakeet anyway, with a
    // note
    {
        const Outcome o = transcribe(media, model, large, "tr");
        CHECK(o.ok);
        CHECK(o.runs.value(1).startsWith("WHISPER") && o.runs.value(1).contains("-l tr"));
        CHECK_EQ(o.used, QString("ggml-large-v3-q5_0.bin"));
        const Outcome alone = transcribe(media, model, QString(), "tr");
        CHECK(alone.ok);
        CHECK(alone.runs.value(1).startsWith("PARAKEET"));
        CHECK(!alone.note.isEmpty());
        CHECK_EQ(alone.words.size(), 5);
    }

    // 6. The text twice in a piece: Parakeet does not make up repeats, so it was said twice and stays (no repeat
    // filter); a word whose times went back is moved up to the one before and never ends before it starts
    qputenv("FAKE_TWICE", "1");
    {
        const Outcome o = transcribe(media, model, large, "de");
        CHECK(o.ok);
        CHECK_EQ(text(o.words), QString("Eins, zwei drei vier fünf. Eins, zwei drei vier fünf."));
        for (int i = 0; i < o.words.size(); ++i) {
            CHECK(o.words[i].to >= o.words[i].from);
            if (i > 0) CHECK(o.words[i].from >= o.words[i - 1].from);
        }
    }
    qunsetenv("FAKE_TWICE");
    return Check::result();
}
