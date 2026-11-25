// SPDX-License-Identifier: AGPL-3.0-only
// The captions of a short (cli/CommandsShort: voiceCaptionWords, shortFinishOps) with a fake whisper: each voice
// stream heard only in the parts the timeline plays (not the whole recording), its transcript cached with the model
// that made it, the check by the other installed model with the rule that says when "auto" runs, the timeline's sound
// as the source when no voice stream could be heard, the check on that source too, and a note of its own when the
// check leaves out every line.
//
// whisper itself is not needed: this program copies itself as "whisper-cli" and writes one word in the middle of the
// sound it got ("Servus" with a "small" model, else "Hallo"), logging the model and the sound's length.
#include "cli/Commands.h"
#include "cli/DirectDetail.h"
#include "engine/AnalysisCache.h"
#include "engine/Extensions.h"

#include "check.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

int fakeWhisper(int argc, char** argv)
{
    QString out, input, model, language;
    for (int i = 1; i + 1 < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "-of") out = QString::fromLocal8Bit(argv[i + 1]);
        if (a == "-f") input = QString::fromLocal8Bit(argv[i + 1]);
        if (a == "-m") model = QFileInfo(QString::fromLocal8Bit(argv[i + 1])).fileName();
        if (a == "-l") language = QString::fromLocal8Bit(argv[i + 1]);
    }
    if (out.isEmpty()) return 0; // a language detection pass: nothing to write
    if (qEnvironmentVariableIsSet("FAKE_WHISPER_FAIL")) return 3;
    const double seconds = std::max<qint64>(0, QFileInfo(input).size() - 44) / 32000.0;
    if (const QString log = qEnvironmentVariable("FAKE_WHISPER_LOG"); !log.isEmpty()) {
        QFile l(log);
        if (l.open(QIODevice::Append)) l.write(QString("%1 %2 %3\n").arg(model).arg(seconds, 0, 'f', 1).arg(language).toUtf8());
    }
    // A short piece heard without a language: whisper takes it for English (as it did with a part of only laughter)
    const QString heardIn = language != "auto" ? language : seconds < 12 ? "en" : "de";
    const qint64 mid = qint64(seconds * 500);
    const QJsonObject json{{"result", QJsonObject{{"language", heardIn}}},
                           {"transcription", QJsonArray{QJsonObject{{"text", model.contains("small") ? " Servus" : " Hallo"},
                                                                    {"offsets", QJsonObject{{"from", mid}, {"to", mid + 800}}}}}}};
    QFile o(out + ".json");
    if (!o.open(QIODevice::WriteOnly)) return 4;
    o.write(QJsonDocument(json).toJson());
    return 0;
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       %s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"error", e.code}};
    }
}

// The logged whisper runs since the last call: "model seconds" each
QStringList takeLog(const QString& log)
{
    QFile l(log);
    QStringList lines;
    if (l.open(QIODevice::ReadOnly)) lines = QString::fromUtf8(l.readAll()).split('\n', Qt::SkipEmptyParts);
    QFile::remove(log);
    return lines;
}

// A project whose timeline plays `seconds` of the file from `from` (its stream `stream` only, on an audio track)
QString projectOf(const QString& dir, const QString& name, const QString& file, double from, double seconds, int stream)
{
    const QString p = QDir(dir).filePath(name + ".schneidi");
    run("new", {{"project", p}});
    QJsonObject op{{"op", "append"}, {"media", file}, {"in", QString("%1s").arg(from)}, {"duration", QString("%1s").arg(seconds)},
                   {"audio_only", true}};
    if (stream >= 0) op["audio_streams"] = QJsonArray{stream};
    const QJsonObject e = run("edit", {{"project", p}, {"ops", QJsonArray{op}}});
    CHECK(!e.contains("error"));
    return p;
}

QStringList texts(const QJsonArray& words)
{
    QStringList out;
    for (const QJsonValue& w : words) out << w["text"].toString();
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).baseName() == QLatin1String("whisper-cli")) return fakeWhisper(argc, argv);
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("caption_check");
    Check::initEngine();
    if (!Check::haveFfmpeg()) return Check::result();
    QTemporaryDir dir;
    const QString log = dir.filePath("whisper.log");
    qputenv("FAKE_WHISPER_LOG", log.toUtf8());

    // The fake whisper on the PATH, two (fake) models: large-v3 (the default here: the first installed) and small
    {
        const QString suffix = QFileInfo(QCoreApplication::applicationFilePath()).suffix();
        const QString fake = dir.filePath(suffix.isEmpty() ? "bin/whisper-cli" : "bin/whisper-cli." + suffix);
        QDir().mkpath(dir.filePath("bin"));
        QFile::copy(QCoreApplication::applicationFilePath(), fake);
        QFile::setPermissions(fake, QFile::permissions(fake) | QFileDevice::ExeOwner);
        qputenv("PATH", (QDir::toNativeSeparators(dir.filePath("bin")) + QDir::listSeparator() + qEnvironmentVariable("PATH")).toUtf8());
        const QString models = QDir(Extensions::rootDir()).filePath("whisper/models");
        QDir().mkpath(models);
        for (const char* name : {"ggml-large-v3-q5_0.bin", "ggml-small-q5_1.bin"}) {
            QFile m(QDir(models).filePath(name));
            if (m.open(QIODevice::WriteOnly)) m.write("model");
        }
    }
    CHECK_EQ(QFileInfo(Extensions::whisperModelPath()).fileName(), QString("ggml-large-v3-q5_0.bin"));
    // When "auto" checks on a processor: nothing to hear (cached), or a fast model under a minute
    CHECK(Extensions::whisperGpuBackend().isEmpty());
    CHECK(!Cli::detail::fastCheckModel("/m/ggml-large-v3-q5_0.bin"));
    CHECK(Cli::detail::fastCheckModel("ggml-large-v3-turbo-q5_0.bin") && Cli::detail::fastCheckModel("ggml-small-q5_1.bin"));
    CHECK(Cli::detail::cheapCheck(false, 0) && !Cli::detail::cheapCheck(false, 10));
    CHECK(Cli::detail::cheapCheck(true, 59) && !Cli::detail::cheapCheck(true, 61));
    CHECK_EQ(QFileInfo(Cli::detail::checkModelFor(Extensions::whisperModelPath())).fileName(), QString("ggml-small-q5_1.bin"));

    // A 70 s game recording: the game sound and a microphone that talks all the time (noise, loud enough)
    const auto recording = [&](const QString& name) {
        return Check::makeMedia(dir.filePath(name),
                                {"-f", "lavfi", "-i", "sine=f=220:d=70:r=48000", "-f", "lavfi", "-i",
                                 "anoisesrc=a=0.3:d=70:r=48000", "-map", "0:a", "-map", "1:a", "-metadata:s:a:0",
                                 "title=System Output", "-metadata:s:a:1", "title=Mikrofon", "-c:a", "aac"});
    };
    const QString game = recording("game.mkv");
    if (!CHECK(!game.isEmpty())) return Check::result();
    const QString project = projectOf(dir.path(), "short", game, 34, 12, 1);
    Cli::Context ctx;

    // A transcript cached without its model (as before 25.11.: under the key of whatever model was the default then)
    // is not taken: another model may have made it; the microphone is heard only in the part the timeline plays
    // (34-46 s, padded and widened: 30-50 s), not over the whole 70 s
    AnalysisCache::put(AnalysisCache::key("speech-words", game, QJsonObject{{"language", ""}, {"stream", 1}, {"verbatim", false}}, 1),
                       QJsonObject{{"stream", 1}, {"language", "de"}, {"words", QJsonArray{QJsonArray{40000, 40500, "Alt"}}}});
    takeLog(log);
    {
        Cli::detail::VoiceCaptions v;
        const QJsonArray words = Cli::detail::voiceCaptionWords(project, "@current", ctx, &v);
        if (!CHECK_EQ(texts(words).join(' '), QString("Hallo"))) std::printf("       words %s\n", qPrintable(texts(words).join(' ')));
        CHECK_EQ(v.voices, QStringList{"Mikrofon"});
        const QStringList runs = takeLog(log);
        if (CHECK_EQ(runs.size(), 1)) {
            CHECK(runs[0].startsWith("ggml-large-v3-q5_0.bin"));
            const double heard = runs[0].section(' ', 1, 1).toDouble();
            if (!CHECK(heard > 11 && heard <= 21)) std::printf("       whisper: %s\n", qPrintable(runs[0]));
        }
    }
    // The check: "auto" on a processor runs with a fast model and under 60 s of sound to hear (the 20 s part, not
    // the 70 s file); the two models hear different words: the line is left out, with both readings
    {
        Cli::detail::VoiceCaptions v;
        v.check = "auto";
        const QJsonArray words = Cli::detail::voiceCaptionWords(project, "@current", ctx, &v);
        if (!CHECK_EQ(v.checkedWith, QString("ggml-small-q5_1.bin"))) std::printf("       skipped: %s\n", qPrintable(v.skipped));
        CHECK(words.isEmpty());
        if (CHECK_EQ(v.leftOut.size(), 1)) {
            CHECK_EQ(v.leftOut[0].toObject()["heard"].toString(), QString("Hallo"));
            CHECK_EQ(v.leftOut[0].toObject()["other"].toString(), QString("Servus"));
        }
        const QStringList runs = takeLog(log);
        CHECK(runs.size() == 1 && runs.value(0).startsWith("ggml-small-q5_1.bin"));
    }
    // The short's finish: every line left out says so (not "no speech in the short")
    const auto finishKeys = [&](const QString& p, const QString& check, QJsonArray* ops = nullptr) {
        Cli::detail::ShortFinish f;
        f.captionsCheck = check;
        QJsonArray done, notes, keys;
        QJsonArray o;
        try {
            o = Cli::detail::shortFinishOps(p, "Timeline 1", f, ctx, &done, &notes, &keys);
        } catch (const Cli::Error& e) {
            std::printf("       short finish: %s %s\n", qPrintable(e.code), qPrintable(e.message));
            keys << QString("error");
        }
        if (ops) *ops = o;
        QStringList out;
        for (const QJsonValue& k : keys) out << k.toString();
        return out;
    };
    {
        const QStringList keys = finishKeys(project, "on");
        if (!CHECK(keys.contains("captions_all_left_out") && !keys.contains("no_captions_speech")))
            std::printf("       notes %s\n", qPrintable(keys.join(", ")));
    }
    // whisper fails on every voice stream (not "no speech"): no voices, so the short takes the timeline's sound
    {
        const QString other = recording("broken.mkv");
        const QString p = projectOf(dir.path(), "broken", other, 10, 8, 1);
        qputenv("FAKE_WHISPER_FAIL", "1");
        Cli::detail::VoiceCaptions v;
        const QJsonArray words = Cli::detail::voiceCaptionWords(p, "@current", ctx, &v);
        CHECK(words.isEmpty());
        CHECK(v.voices.isEmpty());
        // The finish goes on to the timeline's sound (whisper fails there too: its error, not "no speech")
        const QStringList keys = finishKeys(p, "off");
        if (!CHECK(keys.contains("error") && !keys.contains("no_captions_speech"))) std::printf("       notes %s\n", qPrintable(keys.join(", ")));
        qunsetenv("FAKE_WHISPER_FAIL");
    }
    // Two parts of one recording: the longest heard first, its language for the other one (heard alone, the short
    // part would be "English")
    {
        const QString two = recording("two-parts.mkv");
        const QString p = projectOf(dir.path(), "two-parts", two, 5, 4, 1);
        run("edit", {{"project", p}, {"ops", QJsonArray{QJsonObject{{"op", "append"}, {"media", two}, {"in", "40s"}, {"duration", "14s"},
                                                                  {"audio_only", true}, {"audio_streams", QJsonArray{1}}}}}});
        takeLog(log);
        Cli::detail::VoiceCaptions v;
        Cli::detail::voiceCaptionWords(p, "@current", ctx, &v);
        const QStringList runs = takeLog(log);
        if (CHECK_EQ(runs.size(), 2)) {
            CHECK(runs[0].section(' ', 1, 1).toDouble() > 15 && runs[0].endsWith(" auto"));
            if (!CHECK(runs[1].section(' ', 1, 1).toDouble() < 12 && runs[1].endsWith(" de"))) std::printf("       %s\n", qPrintable(runs.join(" | ")));
        }
    }
    // A recording with one sound stream (no voice stream of its own): its timeline sound is checked the same way
    {
        const QString single = Check::makeMedia(dir.filePath("talk.mkv"), {"-f", "lavfi", "-i", "anoisesrc=a=0.3:d=30:r=48000", "-c:a", "aac"});
        const QString p = projectOf(dir.path(), "single", single, 5, 10, -1);
        takeLog(log);
        QJsonArray ops;
        const QStringList keys = finishKeys(p, "on", &ops);
        if (!CHECK(keys.contains("captions_all_left_out"))) std::printf("       notes %s\n", qPrintable(keys.join(", ")));
        CHECK(ops.isEmpty());
        const QStringList runs = takeLog(log);
        CHECK(runs.size() == 2 && runs.value(1).startsWith("ggml-small-q5_1.bin"));
        // check off: the line stays
        CHECK(!finishKeys(p, "off", &ops).contains("captions_all_left_out"));
        CHECK(!ops.isEmpty());
    }
    return Check::result();
}
