// SPDX-License-Identifier: AGPL-3.0-only
// Which sound `transcribe` hears: the voice stream/track of a game recording by default (no song lyrics from the
// music stream in the captions), chosen streams by number or name (several mixed), the language check (CLI and the
// app's dialog "Untertitel aus Audio erzeugen": a hint after the run).
// whisper itself is not needed: this program copies itself as "whisper-cli" onto the PATH and then answers like
// whisper with the pitch of the sound it got ("f1000" = a 1 kHz tone), so the test sees which stream reached it.
#include "app/TranscribeDialog.h"
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Transcript.h"
#include "engine/Extensions.h"
#include "engine/Transcriber.h"

#include "check.h"

#include <QDir>
#include <QFile>
#include <QApplication>
#include <QEventLoop>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// --- The fake whisper-cli ---------------------------------------------------------------------------------------
// Reads the 16-bit mono WAV of -f, writes -of + ".json" with one word "f<pitch rounded to 100 Hz>"; -dl prints a
// detected "en" like whisper's log line. The language of -l (auto: "de") is the result's language. FAKE_WHISPER_MS:
// works that long first, without a progress line (like whisper on a short clip).
int fakeWhisper(int argc, char** argv)
{
    QString wav, out, lang = "de";
    bool detect = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        const QString next = i + 1 < argc ? QString::fromLocal8Bit(argv[i + 1]) : QString();
        if (a == "-f") wav = next;
        else if (a == "-of") out = next;
        else if (a == "-l" && next != "auto") lang = next;
        else if (a == "-dl") detect = true;
    }
    // The test reads the arguments back: the files must come by name (whisper runs in their folder)
    if (const QString log = qEnvironmentVariable("SCHNEIDI_FAKE_WHISPER_LOG"); !log.isEmpty()) {
        QFile l(log);
        if (l.open(QIODevice::Append)) l.write(("-f " + wav + " -of " + out + "\n").toUtf8());
    }
    if (detect) {
        std::fprintf(stderr, "whisper_full_with_state: auto-detected language: en (p = 0.930000)\n");
        return 0;
    }
    QThread::msleep(qEnvironmentVariableIntValue("FAKE_WHISPER_MS"));
    QFile f(wav);
    if (!f.open(QIODevice::ReadOnly)) return 2;
    const QByteArray d = f.readAll();
    const qsizetype data = d.indexOf("data");
    if (data < 0) return 3;
    const auto* s = reinterpret_cast<const qint16*>(d.constData() + data + 8);
    const qint64 n = (d.size() - data - 8) / 2;
    // Pitch of the sounding part (the tones have silence around them; the timeline mix may be quieter)
    int peak = 0;
    for (qint64 i = 0; i < n; ++i) peak = std::max(peak, std::abs(int(s[i])));
    qint64 first = -1, last = -1, crossings = 0;
    for (qint64 i = 0; i < n; ++i)
        if (std::abs(int(s[i])) > peak / 4) {
            if (first < 0) first = i;
            last = i;
        }
    for (qint64 i = std::max<qint64>(1, first); i <= last; ++i) crossings += (s[i - 1] < 0) != (s[i] < 0);
    const double seconds = (last - first) / 16000.0;
    const int pitch = seconds > 0 ? int(std::lround(crossings / 2.0 / seconds / 100)) * 100 : 0;
    std::fprintf(stderr, "whisper_print_progress_callback: progress =  50%%\n");
    const QJsonObject json{
        {"result", QJsonObject{{"language", lang}}},
        {"transcription", QJsonArray{QJsonObject{{"text", QString(" f%1").arg(pitch)},
                                                 {"offsets", QJsonObject{{"from", 100}, {"to", 900}}}}}}};
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
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << e.code << e.message;
        return {{"error", e.code}};
    }
}

QString errorOf(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

QString text(const QJsonObject& r) { return r["segments"][0]["text"].toString(); }

} // namespace

int main(int argc, char** argv)
{
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).baseName() == QLatin1String("whisper-cli")) return fakeWhisper(argc, argv);
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("transcribe_pick");

    // --- Names that say "voice"
    for (const char* yes : {"Mikrofon", "Mic/Aux", "Microphone", "Voice", "voice 2", "Stimme", "Micro", "Głos",
                            "Микрофон", "Kommentar", "Headset Mic", "Narration"})
        if (!CHECK(Transcript::isVoiceName(QString::fromUtf8(yes)))) std::printf("       %s\n", yes);
    for (const char* no : {"Mix", "Music", "Spotify", "Discord", "System Output", "Desktop Audio", "Comic Sans",
                           "Mikser", "Game", "", "Spur 3", "System + Mikrofon", "Mix (Mic)", "Game & Voice"})
        if (!CHECK(!Transcript::isVoiceName(QString::fromUtf8(no)))) std::printf("       %s\n", no);

    // --- Streams of a file: the voice ones, else the first
    {
        MediaInfo m;
        m.hasAudio = true;
        CHECK(Transcript::defaultStreams(m) == QVector<int>{0}); // one stream, no titles
        m.audioStreams = {{2, "Mix"}, {2, "Spotify"}, {2, "Discord"}, {2, "System Output"}, {1, "Mikrofon"}};
        CHECK(Transcript::defaultStreams(m) == QVector<int>{4});
        m.audioStreams = {{2, "Mix"}, {1, "Mic 1"}, {1, "Mic 2"}};
        CHECK((Transcript::defaultStreams(m) == QVector<int>{1, 2})); // a podcast: both voices
        m.audioStreams = {{2, "System + Mikrofon"}, {2, "System"}, {1, "Mikrofon"}}; // Clipi's Discord clips
        CHECK(Transcript::defaultStreams(m) == QVector<int>{2});
        m.audioStreams = {{2, "Game"}, {2, "Music"}};
        CHECK(Transcript::defaultStreams(m) == QVector<int>{0});
        m.audioStreams = {{1, "Mikrofon"}}; // the only stream: nothing to choose
        CHECK(Transcript::defaultStreams(m) == QVector<int>{0});
    }

    // --- Tracks of a timeline: voice tracks only when there is more to hear
    {
        Timeline tl;
        tl.audio.resize(4);
        for (int i = 0; i < 4; ++i) {
            Clip c;
            c.id = i + 1;
            c.mediaPath = QString("rec.mkv");
            c.audioStream = i;
            c.start = 0;
            c.out = 9;
            tl.audio[i].clips << c;
        }
        tl.audio[0].muted = true; // the Mix, as the game template puts it
        const QStringList streams{"Mix", "Spotify", "Discord", "Mikrofon"};
        const auto name = [&](const Clip& c) { return streams.value(c.audioStream); };
        CHECK(Transcript::voiceTracks(tl, name) == QVector<int>{3});
        // Named track instead of the stream name
        Timeline named = tl;
        named.audio[3].name = "Voice";
        CHECK(Transcript::voiceTracks(named, {}) == QVector<int>{3});
        // The voice muted by the user: as heard
        Timeline muted = tl;
        muted.audio[3].muted = true;
        CHECK(Transcript::voiceTracks(muted, name).isEmpty());
        // Only voice tracks there: as heard (nothing to leave out)
        Timeline only = tl;
        only.audio[1].muted = only.audio[2].muted = true;
        CHECK(Transcript::voiceTracks(only, name).isEmpty());
        // Solo on another track: that is what is heard
        Timeline solo = tl;
        solo.audio[1].solo = true;
        CHECK(Transcript::voiceTracks(solo, name).isEmpty());
    }

    if (!Check::haveFfmpeg()) return Check::result();
    Check::initEngine();

    // --- The CLI op with the fake whisper on the PATH
    QTemporaryDir dir;
    const QString suffix = QFileInfo(QCoreApplication::applicationFilePath()).suffix(); // "exe" on Windows
    const QString fake = dir.filePath(suffix.isEmpty() ? "bin/whisper-cli" : "bin/whisper-cli." + suffix);
    QDir().mkpath(dir.filePath("bin"));
    if (!CHECK(QFile::copy(QCoreApplication::applicationFilePath(), fake))) return Check::result();
    QFile::setPermissions(fake, QFile::permissions(fake) | QFileDevice::ExeOwner);
    qputenv("PATH", (QDir::toNativeSeparators(dir.filePath("bin")) + QDir::listSeparator() + qEnvironmentVariable("PATH")).toUtf8());
    const QString model = dir.filePath("ggml-test.bin");
    {
        QFile m(model);
        CHECK(m.open(QIODevice::WriteOnly) && m.write("model") == 5);
    }

    // A game recording: Mix (400 Hz), Spotify (400 Hz, the "song"), Mikrofon (1 kHz, the voice); each tone sounds
    // from 0.1 to 1 s (where the fake's word is) with silence after it, so the cue counts as spoken
    const auto tone = [](int hz) {
        return QString("sine=f=%1:d=2:sample_rate=48000,volume='between(t,0.1,1)':eval=frame").arg(hz);
    };
    const QString rec = Check::makeMedia(
        dir.filePath("rec.mkv"),
        {"-f", "lavfi", "-i", "color=c=gray:s=160x120:r=25:d=2", "-f", "lavfi", "-i", tone(400), "-f", "lavfi", "-i",
         tone(400), "-f", "lavfi", "-i", tone(1000), "-map", "0", "-map", "1", "-map", "2", "-map", "3", "-c:v",
         "libx264", "-c:a", "aac", "-metadata:s:a:0",
         "title=Mix", "-metadata:s:a:1", "title=Spotify", "-metadata:s:a:2", "title=Mikrofon"});
    if (!CHECK(!rec.isEmpty())) return Check::result();
    const QJsonObject base{{"source", rec}, {"model", model}};

    {
        // Default: the microphone, and the answer says so
        const QJsonObject r = run("transcribe", base);
        CHECK_EQ(text(r), QString("f1000"));
        CHECK_EQ(r["audio_stream"].toInt(-1), 2);
        CHECK_EQ(r["audio_stream_name"].toString(), QString("Mikrofon"));
        CHECK(QJsonDocument(r["notes"].toArray()).toJson().contains("2 = Mikrofon"));
        CHECK(!r.contains("detected_language")); // auto: nothing to check
    }
    {
        // The files go to whisper by name, it runs in their folder: whisper-cli reads its arguments in the ANSI code
        // page on Windows, a temporary folder under a user name with other letters became '?'
        const QString log = dir.filePath("whisper-args.log");
        qputenv("SCHNEIDI_FAKE_WHISPER_LOG", log.toUtf8());
        CHECK_EQ(text(run("transcribe", base)), QString("f1000"));
        qunsetenv("SCHNEIDI_FAKE_WHISPER_LOG");
        QFile l(log);
        CHECK(l.open(QIODevice::ReadOnly));
        CHECK_EQ(QString::fromUtf8(l.readAll()).trimmed(), QString("-f audio.wav -of out"));
    }
    {
        // By number (also as a JSON number) and by name
        QJsonObject a = base;
        a["audio_stream"] = 0;
        CHECK_EQ(text(run("transcribe", a)), QString("f400"));
        a["audio_stream"] = "spotify";
        const QJsonObject r = run("transcribe", a);
        CHECK_EQ(text(r), QString("f400"));
        CHECK_EQ(r["audio_stream"].toInt(-1), 1);
        CHECK(!r.contains("notes")); // chosen: no word about it
        // Several: mixed into one
        a["audio_stream"] = "Spotify,Mikrofon";
        CHECK((run("transcribe", a)["audio_stream"].toArray() == QJsonArray{1, 2}));
        // Not there: the error lists the streams
        a["audio_stream"] = "Discord";
        const QString e = errorOf("transcribe", a);
        CHECK(e.startsWith("BAD_ARGUMENT") && e.contains("2 = Mikrofon"));
        a["audio_stream"] = 7;
        CHECK(errorOf("transcribe", a).startsWith("BAD_ARGUMENT"));
    }
    {
        // A chosen language whisper does not hear: told, not changed
        QJsonObject a = base;
        a["language"] = "fr";
        const QJsonObject r = run("transcribe", a);
        CHECK_EQ(r["language"].toString(), QString("fr"));
        CHECK_EQ(r["detected_language"].toString(), QString("en"));
        CHECK(QJsonDocument(r["notes"].toArray()).toJson().contains("whisper hears 'en'"));
        a["language"] = "en";
        CHECK(!run("transcribe", a).contains("detected_language"));
    }
    {
        // A project made from the recording: every stream on its own track, the voice track is heard
        const QString proj = dir.filePath("p.schneidi");
        run("new", {{"project", proj}, {"media", QJsonArray{rec}}, {"template", "game"}});
        run("edit", {{"project", proj}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(rec)}});
        const QJsonObject r = run("transcribe", {{"source", proj}, {"model", model}});
        CHECK_EQ(text(r), QString("f1000"));
        const QJsonArray tracks = r["tracks"].toArray();
        if (CHECK_EQ(tracks.size(), 1)) CHECK(tracks[0].toString().contains("Mikrofon"));
        CHECK(QJsonDocument(r["notes"].toArray()).toJson().contains("voice track"));
        // "all": everything audible, as before
        const QJsonObject all = run("transcribe", {{"source", proj}, {"model", model}, {"tracks", "all"}});
        CHECK(!all.contains("tracks"));
        CHECK(all["segments"].toArray().size() == 1);
        // Named tracks as before
        const QJsonObject spotify = run("transcribe", {{"source", proj}, {"model", model}, {"tracks", "Spotify"}});
        CHECK_EQ(text(spotify), QString("f400"));
    }
    {
        // schneidi-cli itself: JSON on stdout, progress lines with the step on stderr
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char* var : {"HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "APPDATA", "LOCALAPPDATA"})
            env.insert(var, dir.filePath("home")); // no whisper installed for the user: the fake on the PATH
        p.setProcessEnvironment(env);
        p.start(SCHNEIDI_CLI, {"transcribe", rec, "--model", model, "--language", "fr"});
        CHECK(p.waitForFinished(60000) && p.exitCode() == 0);
        const QJsonObject out = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
        CHECK(out["ok"].toBool());
        CHECK_EQ(out["detected_language"].toString(), QString("en"));
        int lines = 0, last = -1;
        bool staged = true;
        for (const QByteArray& line : p.readAllStandardError().split('\n')) {
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            if (!o.contains("progress")) continue;
            ++lines;
            CHECK(o["progress"].toInt() >= last);
            last = o["progress"].toInt();
            staged = staged && o["stage"].toString() == QLatin1String("transcribe");
        }
        CHECK(lines >= 2);
        CHECK(staged);
        CHECK_EQ(last, 100);
    }
    {
        // The app's dialog: a chosen language whisper is sure it does not hear (the fake: "en" at 93 %) gives a hint
        // after the run, the subtitles are made anyway; the right language or "auto": no hint
        const Extensions::Item* tiny = Extensions::find("whisper-model-tiny");
        const QString installed = QDir(Extensions::rootDir()).filePath("whisper/models/" + tiny->fileName);
        QDir().mkpath(QFileInfo(installed).absolutePath());
        QFile::remove(installed); // from an earlier run (test settings and data stay)
        CHECK(QFile::copy(model, installed));
        Timeline tl;
        tl.audio.resize(1);
        Clip c;
        c.id = 1;
        c.mediaPath = rec;
        c.audioStream = 2; // the microphone
        c.out = 49;
        tl.audio[0].clips << c;
        const ProjectFormat format{640, 360, {25, 1}};
        const auto hintFor = [&](const char* language, int* cues) {
            QSettings().setValue("transcribe/language", language);
            TranscribeDialog dlg(tl, format, 0, 50, {"A1"});
            QPushButton* start = nullptr;
            for (QPushButton* b : dlg.findChildren<QPushButton*>())
                if (b->text() == T("Erzeugen")) start = b;
            if (!CHECK(start && start->isEnabled())) return QString("no start");
            QEventLoop loop;
            QObject::connect(&dlg, &QDialog::finished, &loop, &QEventLoop::quit);
            QTimer::singleShot(60000, &loop, &QEventLoop::quit);
            start->click();
            loop.exec();
            CHECK_EQ(dlg.result(), int(QDialog::Accepted));
            *cues = dlg.results().isEmpty() ? 0 : int(dlg.results()[0].cues.size());
            return dlg.languageHint();
        };
        int cues = 0;
        const QString hint = hintFor("fr", &cues);
        CHECK(hint.contains(T("Englisch")) && hint.contains(T("Französisch")) && hint.contains("93"));
        CHECK_EQ(cues, 1);
        CHECK(hintFor("en", &cues).isEmpty());
        CHECK(hintFor("auto", &cues).isEmpty());
        CHECK_EQ(cues, 1);
    }
    {
        // Progress while whisper says nothing: estimated from the time against the expected run time, rising,
        // never 100 % before it is done; the run's speed is remembered for the next estimate
        CHECK_EQ(Transcriber::heartbeat(0, 10), 0.0);
        CHECK_EQ(Transcriber::heartbeat(5, 10), 0.5);
        CHECK_EQ(Transcriber::heartbeat(3, 0), 0.0);
        double before = 0;
        bool rising = true;
        for (double t = 0.5; t < 60; t += 0.5) { // six times as long as expected
            const double h = Transcriber::heartbeat(t, 10);
            rising = rising && h > before && h < 1;
            before = h;
        }
        CHECK(rising);
        CHECK(before > 0.98);
        // Expected: the remembered speed times the sound, at least one 30 s window
        QSettings().setValue("transcribe/speed/ggml-test.bin", 0.05);
        CHECK(std::abs(Transcriber::whisperSeconds(model, 2) - 1.5) < 1e-9);
        CHECK(std::abs(Transcriber::whisperSeconds(model, 60) - 3) < 1e-9);
        CHECK(Transcriber::whisperSeconds(dir.filePath("none.bin"), 30) > 0); // a guess without a measurement
        // The fake works 2 s without a word (expected 1.5 s): several steps in between, below 100 % until done
        qputenv("FAKE_WHISPER_MS", "2000");
        Transcriber t;
        QVector<double> seen;
        bool finished = false, ok = false;
        QObject::connect(&t, &Transcriber::progress, [&](double p) {
            if (!finished) seen << p;
        });
        QEventLoop loop;
        QObject::connect(&t, &Transcriber::finished, [&](bool good, const QString&) {
            ok = good;
            finished = true;
            loop.quit();
        });
        TranscribeRequest req;
        req.media = rec;
        req.audioStream = 2;
        req.model = model;
        QString error;
        if (CHECK(t.start(req, &error))) {
            QTimer::singleShot(30000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        qunsetenv("FAKE_WHISPER_MS");
        CHECK(ok);
        int between = 0;
        bool forward = true;
        for (int i = 0; i < seen.size(); ++i) {
            between += seen[i] > 0.15 && seen[i] < 0.95;
            forward = forward && (i == 0 || seen[i] >= seen[i - 1]);
        }
        CHECK(between >= 4);
        CHECK(forward);
        CHECK(seen.size() >= 2 && seen[seen.size() - 2] < 1.0 && seen.last() == 1.0);
        // Measured: ~2 s for one 30 s window = 0.067 per second, mixed with the 0.05 before
        const double speed = QSettings().value("transcribe/speed/ggml-test.bin").toDouble();
        CHECK(speed > 0.055 && speed < 0.075);

        // The same Transcriber again (the Transcript panel keeps one for every file): the run before left whisper's
        // working directory on its temporary folder, which is gone now; reading the next file's sound must not
        // start in it (ffmpeg "could not be started", every second transcription in a session failed)
        ok = finished = false;
        QString message;
        QObject::connect(&t, &Transcriber::finished, [&](bool, const QString& m) { message = m; });
        req.audioStream = 0;
        if (CHECK(t.start(req, &error))) {
            QTimer::singleShot(30000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        if (!CHECK(ok)) std::printf("       second run: %s\n", qPrintable(message));
        if (CHECK_EQ(t.result().words.size(), qsizetype(1))) CHECK_EQ(t.result().words[0].text, QString("f400"));
    }
    return Check::result();
}
