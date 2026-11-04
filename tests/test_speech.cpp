// SPDX-License-Identifier: AGPL-3.0-only
// Text to speech (0.16): Piper and its voices in the extension catalog, the cache key and file names, the piper
// command line, word time estimates, the free audio track, WAV reading, the Speaker with a fake piper (this test
// program itself, started with SCHNEIDI_FAKE_PIPER: writes a sine WAV) and the CLI edit op 'speak' with captions and
// its project round trip. No network, no real piper.

#include "check.h"
#include "fake_piper.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "app/SpeechDialog.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Speech.h"
#include "engine/Extensions.h"
#include "engine/Tts.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <clocale>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace {

// ---------- the fake piper (fake_piper.h) ----------

int fakeRuns(const QString& log)
{
    QFile f(log);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    return int(f.readAll().count("--model"));
}

// ---------- tests ----------

void testCatalog()
{
    const Extensions::Item* thorsten = Extensions::find("piper-voice-de_DE-thorsten");
    const Extensions::Item* lj = Extensions::find("piper-voice-en_US-ljspeech");
    const Extensions::Item* ramona = Extensions::find("piper-voice-de_DE-ramona");
    if (!CHECK(thorsten && lj && ramona)) return;
    // The default German voice is the one with a clean licence chain (trained from scratch on public domain data):
    // the first German voice of the catalog
    for (const Extensions::Item& i : Extensions::catalog())
        if (i.kind == Extensions::Kind::Voice && Speech::voiceLanguage(Extensions::voiceName(i.id)) == "de") {
            CHECK_EQ(i.id, ramona->id);
            break;
        }
    for (const Extensions::Item* v : {thorsten, lj, ramona}) {
        CHECK(v->kind == Extensions::Kind::Voice);
        CHECK(v->url.startsWith("https://huggingface.co/rhasspy/piper-voices/resolve/"));
        CHECK(v->url.endsWith(v->fileName));
        CHECK_EQ(v->configUrl, v->url + ".json");
        CHECK_EQ(v->sha256.size(), 64);
        CHECK_EQ(v->configSha256.size(), 64);
        CHECK(v->size > 50'000'000 && v->configSize > 1000);
    }
    CHECK_EQ(Extensions::voiceName(thorsten->id), QString("de_DE-thorsten"));
    const Extensions::Item* piper = Extensions::find("piper");
#if (defined(Q_OS_LINUX) || defined(Q_OS_WIN)) && defined(Q_PROCESSOR_X86_64)
    if (!CHECK(piper != nullptr)) return;
    CHECK(piper->kind == Extensions::Kind::Speech);
    CHECK(piper->url.startsWith("https://github.com/rhasspy/piper/releases/download/2023.11.14-2/"));
    CHECK_EQ(piper->sha256.size(), 64);
#endif
    // CLI names: id, short name, file name, speaker name
    QString unknown;
    CHECK_EQ(Extensions::parseList("de_DE-thorsten", &unknown), QVector<const Extensions::Item*>{thorsten});
    CHECK_EQ(Extensions::parseList("thorsten", &unknown), QVector<const Extensions::Item*>{thorsten});
    CHECK_EQ(Extensions::parseList("en_US-ljspeech-medium", &unknown), QVector<const Extensions::Item*>{lj});
    CHECK(unknown.isEmpty());
    if (piper) {
        CHECK_EQ(Extensions::parseList("piper,de_DE-thorsten", &unknown),
                 (QVector<const Extensions::Item*>{piper, thorsten}));
        // A voice brings piper along while it is missing
        qunsetenv("SCHNEIDI_PIPER");
        CHECK_EQ(Extensions::withDependencies({thorsten}), (QVector<const Extensions::Item*>{piper, thorsten}));
    }
    // Installed = model and config in extensions/piper/voices
    CHECK(!Extensions::isInstalled(*thorsten));
    const QString dir = QDir(Extensions::rootDir()).filePath("piper/voices");
    QDir().mkpath(dir);
    QFile(QDir(dir).filePath(thorsten->fileName)).open(QIODevice::WriteOnly);
    CHECK(!Extensions::isInstalled(*thorsten)); // config missing
    QFile(QDir(dir).filePath(thorsten->fileName + ".json")).open(QIODevice::WriteOnly);
    CHECK(Extensions::isInstalled(*thorsten));
    CHECK_EQ(Extensions::installedVoices(), QStringList{thorsten->id});
    CHECK_EQ(Extensions::voiceModelPath("thorsten"), QDir(dir).filePath(thorsten->fileName));
    // With Ramona installed too, she is the default German voice
    QFile(QDir(dir).filePath(ramona->fileName)).open(QIODevice::WriteOnly);
    QFile(QDir(dir).filePath(ramona->fileName + ".json")).open(QIODevice::WriteOnly);
    if (I18n::language() == "de") CHECK_EQ(Tts::defaultVoice(), ramona->id);
    CHECK(Extensions::remove(*ramona));
    CHECK(Extensions::remove(*thorsten));
    CHECK(!Extensions::isInstalled(*thorsten));
    CHECK(!QFileInfo::exists(QDir(dir).filePath(thorsten->fileName + ".json")));
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Installing a voice over an installed one (an update): both files first, then both in place. A config download that
// fails keeps the old pair (the new model beside the old config counted as installed before).
void testVoiceInstall(const QString& tmp)
{
    const Extensions::Item* thorsten = Extensions::find("piper-voice-de_DE-thorsten");
    if (!CHECK(thorsten)) return;
    const auto source = [&](const QString& name, const QByteArray& data) {
        const QString path = QDir(tmp).filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(data);
        return path;
    };
    const QByteArray model = "new model", config = "{\"new\": true}";
    Extensions::Item item = *thorsten;
    item.url = QUrl::fromLocalFile(source("new.onnx", model)).toString();
    item.size = model.size();
    item.sha256 = QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex();
    item.configUrl = QUrl::fromLocalFile(QDir(tmp).filePath("missing.onnx.json")).toString();
    item.configSize = config.size();
    item.configSha256 = QCryptographicHash::hash(config, QCryptographicHash::Sha256).toHex();
    const QString dir = QDir(Extensions::rootDir()).filePath("piper/voices");
    const QString installed = QDir(dir).filePath(item.fileName);
    QDir().mkpath(dir);
    source("old", "old model");
    QFile::copy(QDir(tmp).filePath("old"), installed);
    QFile::copy(QDir(tmp).filePath("old"), installed + ".json");
    const auto install = [&](QString* message) {
        Extensions::Installer installer;
        QEventLoop loop;
        bool ok = false;
        QObject::connect(&installer, &Extensions::Installer::finished, &loop, [&](bool success, const QString& m) {
            ok = success;
            *message = m;
            loop.quit();
        });
        QString error;
        if (!installer.start(item, &error)) {
            *message = error;
            return false;
        }
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        return ok;
    };
    QString message;
    CHECK(!install(&message));
    CHECK_EQ(readAll(installed), QByteArray("old model"));
    CHECK_EQ(readAll(installed + ".json"), QByteArray("old model"));
    CHECK(QDir(dir).entryList({"*.part"}, QDir::Files).isEmpty());
    // Both there: both replaced
    item.configUrl = QUrl::fromLocalFile(source("new.onnx.json", config)).toString();
    if (!CHECK(install(&message))) std::cerr << qPrintable(message) << "\n";
    CHECK_EQ(readAll(installed), model);
    CHECK_EQ(readAll(installed + ".json"), config);
    CHECK(QDir(dir).entryList({"*.part"}, QDir::Files).isEmpty());
    CHECK(Extensions::remove(item));
}

void testPure()
{
    Speech::Options a;
    a.voice = "piper-voice-de_DE-thorsten";
    // Same spoken text = same key (spaces and line breaks do not count); voice, speed, pause and text do
    CHECK_EQ(Speech::cacheKey("Hallo  Welt.\n", a), Speech::cacheKey(" Hallo Welt.", a));
    CHECK_EQ(Speech::cacheKey("Hallo", a).size(), 16);
    Speech::Options b = a;
    b.speed = 1.2;
    CHECK(Speech::cacheKey("Hallo", a) != Speech::cacheKey("Hallo", b));
    b = a;
    b.pause = 0.5;
    CHECK(Speech::cacheKey("Hallo", a) != Speech::cacheKey("Hallo", b));
    b = a;
    b.voice = "piper-voice-en_US-ljspeech";
    CHECK(Speech::cacheKey("Hallo", a) != Speech::cacheKey("Hallo", b));
    CHECK(Speech::cacheKey("Hallo", a) != Speech::cacheKey("Hallo!", a));
    const QString name = Speech::fileName("Grüße aus Köln, sagt schneidi!", a);
    CHECK(name.startsWith("speech-grue-aus-koln-sagt-") || name.startsWith("speech-gruse-aus-koln-sagt-"));
    CHECK(name.endsWith(Speech::cacheKey("Grüße aus Köln, sagt schneidi!", a) + ".wav"));
    CHECK_EQ(Speech::fileName("!!!", a), "speech-" + Speech::cacheKey("!!!", a) + ".wav");
    // Folder beside the project, else in the app's data folder
    CHECK_EQ(Speech::folder(QDir::cleanPath(QDir::tempPath() + "/x/Mein Short.schneidi")),
             QDir::cleanPath(QDir::tempPath() + "/x/Mein Short-tts"));
    CHECK(Speech::folder({}).endsWith("/tts"));
    // piper's command line: length_scale = 1 / speed, the pause, quiet; the text on stdin as one line
    Speech::Options o;
    o.speed = 2;
    o.pause = 0.5;
    CHECK_EQ(Speech::piperArguments("/v/x.onnx", o, "/o/a.wav"),
             (QStringList{"--model", "/v/x.onnx", "--output_file", "/o/a.wav", "--length_scale", "0.500",
                          "--sentence_silence", "0.500", "--quiet"}));
    // The config by its own path (a short 8.3 model path loses the ".onnx" piper appends ".json" to)
    CHECK_EQ(Speech::piperArguments("C:\\V\\DE_DE-~1.ONN", o, "a.wav", "C:\\V\\DE_DE-~1.JSO").mid(0, 4),
             (QStringList{"--model", "C:\\V\\DE_DE-~1.ONN", "--config", "C:\\V\\DE_DE-~1.JSO"}));
    o.speed = 9;
    CHECK(Speech::piperArguments("m", o, "o").contains("0.500")); // clamped to 2
    CHECK_EQ(Speech::stdinText("Eins.\nZwei.  "), QByteArray("Eins. Zwei.\n"));
    // Line breaks: inside a sentence a space; a list item or a paragraph ends a sentence; list markers and emoji are
    // not spoken (espeak reads some emoji by name); a text of emoji only is no text
    CHECK_EQ(Speech::normalizedText("Das ist ein\nlanger Satz"), QString("Das ist ein langer Satz"));
    CHECK_EQ(Speech::normalizedText("Drei Gründe:\n- Tempo\n- Preis\n• Freiheit"),
             QString("Drei Gründe: Tempo. Preis. Freiheit"));
    CHECK_EQ(Speech::normalizedText("Erster Absatz\n\nZweiter Absatz"), QString("Erster Absatz. Zweiter Absatz"));
    CHECK_EQ(Speech::normalizedText("Krass 🔥🔥 das war knapp 😂 echt. 👍🏽 🇩🇪"), QString("Krass das war knapp echt."));
    CHECK_EQ(Speech::normalizedText("30° im Schatten, 5 € - fertig"), QString("30° im Schatten, 5 € - fertig"));
    CHECK(Speech::normalizedText("🔥🔥🔥\n😂").isEmpty());
    CHECK_EQ(Speech::normalizedText("Er sagte „Hallo“\r\nund \"Tschüss\"."), QString("Er sagte „Hallo“ und \"Tschüss\"."));
    // The voice's build is part of the key: a new download of the same voice makes a new file
    b = a;
    b.voiceVersion = "other";
    CHECK(Speech::cacheKey("Hallo", a) != Speech::cacheKey("Hallo", b));
    CHECK_EQ(Speech::voiceLanguage("de_DE-thorsten"), QString("de"));
    CHECK_EQ(Speech::voiceLanguage("piper-voice-en_US-ljspeech"), QString("en"));
    CHECK_EQ(Speech::voiceLanguage("/x/en_GB-alan-medium.onnx"), QString("en"));
    CHECK(Speech::voiceLanguage("myvoice.onnx").isEmpty());
}

void testEstimate()
{
    // 1000 ms for "Eins zwei. Dreiundzwanzig": longer words take longer, the sentence end adds its pause
    const QVector<TranscriptWord> w = Speech::estimateWords("Eins zwei. Dreiundzwanzig", 100, 1100, 100);
    if (!CHECK_EQ(w.size(), 3)) return;
    CHECK_EQ(w[0].from, qint64(100));
    CHECK_EQ(w[2].to, qint64(1100));
    CHECK_EQ(w[1].text, QString("zwei."));
    for (int i = 0; i + 1 < w.size(); ++i) CHECK(w[i].to == w[i + 1].from);
    const qint64 lenEins = w[0].to - w[0].from, lenZwei = w[1].to - w[1].from, lenDrei = w[2].to - w[2].from;
    CHECK(lenZwei > lenEins + 50);  // the pause after "zwei."
    CHECK(lenDrei > 2 * lenEins);
    CHECK(Speech::estimateWords("", 0, 1000, 0).isEmpty());
    CHECK(Speech::estimateWords("x", 500, 500, 0).isEmpty());
}

void testFreeTrack()
{
    Timeline tl;
    tl.audio.resize(2);
    Clip c;
    c.start = 0;
    c.in = 0;
    c.out = 99;
    tl.audio[0].clips << c;
    CHECK_EQ(Speech::freeAudioTrack(tl, 50, 80), 1);  // A1 busy there
    CHECK_EQ(Speech::freeAudioTrack(tl, 100, 120), 0); // after the clip
    CHECK_EQ(Speech::freeAudioTrack(tl, 50, 80, 0), 1); // preferred busy -> first free
    CHECK_EQ(Speech::freeAudioTrack(tl, 50, 80, 4), 4); // a new track asked for
    tl.audio[1].locked = true;
    CHECK_EQ(Speech::freeAudioTrack(tl, 50, 80), 2); // all busy or locked -> new one
}

void testWav(const QString& dir)
{
    const QString path = dir + "/sine.wav";
    writeWav(path, 22050, 0.3, 1.0);
    const std::optional<Tts::WavInfo> info = Tts::readWav(path);
    if (!CHECK(info.has_value())) return;
    CHECK_EQ(info->sampleRate, 22050);
    CHECK_EQ(info->channels, 1);
    CHECK(std::abs(info->durationMs - 1600) <= 1);
    CHECK(std::abs(info->peakDb - 20 * std::log10(16000 / 32768.0)) < 0.2);
    CHECK(std::abs(info->speechFromMs - 300) <= 10);
    CHECK(std::abs(info->speechToMs - 1300) <= 10);
    QFile bad(dir + "/bad.wav");
    bad.open(QIODevice::WriteOnly);
    bad.write("RIFFxxxxWAVE");
    bad.close();
    CHECK(!Tts::readWav(dir + "/bad.wav"));
}

void testSpeaker(const QString& dir, const QString& voice, const QString& log)
{
    Tts::Request r;
    r.text = "Hallo, das ist ein Test.";
    r.options.voice = voice;
    r.folder = dir + "/Projekt-tts";
    r.words = true;
    Tts::Result res;
    QString error;
    if (!CHECK(Tts::speak(r, &res, &error))) {
        std::cerr << qPrintable(error) << "\n";
        return;
    }
    CHECK(!res.cached);
    CHECK(res.path.startsWith(r.folder + "/speech-hallo-das-ist-ein-test-"));
    CHECK(std::abs(res.durationMs - (400 + 60 * 24)) <= 2);
    CHECK_EQ(res.words.size(), 5);
    CHECK_EQ(res.wordSource, QString("estimate")); // no whisper in tests
    CHECK(QFileInfo::exists(res.path + ".words.json"));
    CHECK(!QFileInfo::exists(res.path.chopped(4) + ".part.wav"));
    CHECK_EQ(fakeRuns(log), 1);
    // Again: the file that is there, piper does not run
    Tts::Result again;
    CHECK(Tts::speak(r, &again, &error));
    CHECK(again.cached);
    CHECK_EQ(again.path, res.path);
    CHECK_EQ(again.words.size(), 5);
    CHECK_EQ(fakeRuns(log), 1);
    // Faster: a new file, piper with length_scale 0.5
    r.options.speed = 2;
    CHECK(Tts::speak(r, &again, &error));
    CHECK(again.path != res.path);
    CHECK_EQ(fakeRuns(log), 2);
    QFile f(log);
    f.open(QIODevice::ReadOnly);
    const QByteArray logged = f.readAll();
    CHECK(logged.contains("--length_scale|0.500|"));
    // The output by its file name only (piper runs in the folder): piper.exe reads its arguments in the ANSI code
    // page, a project folder with other letters became '?' on Windows
    CHECK(logged.contains("--output_file|speech-hallo-das-ist-ein-test-"));
    // piper fails: an error, no file left
    r.text = "fail";
    CHECK(!Tts::speak(r, &again, &error));
    CHECK(error.contains("Piper"));
    CHECK(QDir(r.folder).entryList({"*part*"}, QDir::Files).isEmpty());
    // piper hangs: killed after the timeout with an error (it blocked the CLI and the app's speech job forever)
    CHECK(Tts::piperTimeoutMs("Hallo") >= 30000);
    CHECK_EQ(Tts::piperTimeoutMs(QString(1000, 'a')), 50000);
    qputenv("SCHNEIDI_TTS_TIMEOUT_MS", "1500");
    r.text = "hang";
    QElapsedTimer clock;
    clock.start();
    CHECK(!Tts::speak(r, &again, &error));
    CHECK(clock.elapsed() < 20000);
    CHECK(error.contains("Piper"));
    CHECK(QDir(r.folder).entryList({"*part*"}, QDir::Files).isEmpty());
    qunsetenv("SCHNEIDI_TTS_TIMEOUT_MS");
    // Nothing to say / unknown voice
    r.text = "  ";
    CHECK(!Tts::speak(r, &again, &error));
    r.text = "Hallo";
    r.options.voice = "piper-voice-xx_XX-nobody";
    CHECK(!Tts::speak(r, &again, &error));
}

// Two runs of the same text at once (CLI and app, two edits): both get the one finished file, no part file stays
void testConcurrent(const QString& dir, const QString& voice)
{
    Tts::Request r;
    r.text = "Zweimal gleichzeitig gesagt.";
    r.options.voice = voice;
    r.folder = dir + "/Gleichzeitig-tts";
    Tts::Speaker a, b;
    int done = 0, ok = 0;
    QEventLoop loop;
    for (Tts::Speaker* s : {&a, &b})
        QObject::connect(s, &Tts::Speaker::finished, &loop, [&](bool success, const QString& error) {
            if (!success) std::cerr << qPrintable(error) << "\n";
            ok += success ? 1 : 0;
            if (++done == 2) loop.quit();
        });
    QString error;
    CHECK(a.start(r, &error));
    CHECK(b.start(r, &error));
    loop.exec();
    CHECK_EQ(ok, 2);
    CHECK_EQ(a.result().path, b.result().path);
    CHECK(Tts::readWav(a.result().path).has_value());
    CHECK_EQ(QDir(r.folder).entryList({"*.wav"}, QDir::Files), QStringList{QFileInfo(a.result().path).fileName()});

    // Cancelled half way: no part file left behind
    r.text = "Abgebrochen bevor es fertig ist.";
    Tts::Speaker c;
    bool cancelledOk = true;
    QObject::connect(&c, &Tts::Speaker::finished, &loop, [&](bool success, const QString&) {
        cancelledOk = success;
        loop.quit();
    });
    CHECK(c.start(r, &error));
    c.cancel();
    loop.exec();
    CHECK(!cancelledOk);
    CHECK(QDir(r.folder).entryList({"*part*"}, QDir::Files).isEmpty());
}

// A changed voice file (new download, own model replaced) makes a new file instead of the old sound
void testVoiceVersion(const QString& dir, const QString& voice)
{
    Tts::Request r;
    r.text = "Gleicher Text, neue Stimme.";
    r.options.voice = voice;
    r.folder = dir + "/Version-tts";
    Tts::Result first, second;
    QString error;
    CHECK(Tts::speak(r, &first, &error));
    QFile f(voice);
    CHECK(f.open(QIODevice::Append));
    f.write("new build");
    f.close();
    CHECK(Tts::speak(r, &second, &error));
    CHECK(!second.cached);
    CHECK(second.path != first.path);
}

void testCliOp(const QString& dir, const QString& voice)
{
    const QString path = dir + "/Short.schneidi";
    ProjectData d;
    d.format = {1080, 1920, {25, 1}};
    d.timeline.video.resize(1);
    d.timeline.audio.resize(1);
    QFile f(path);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write(ProjectFile::toJson(d, path));
    f.close();
    const auto run = [&](const QJsonArray& ops) {
        const Cli::Command* cmd = Cli::find("edit");
        Cli::Context ctx;
        try {
            return cmd->run(Cli::normalize(*cmd, QJsonObject{{"project", path}, {"ops", ops}}), ctx);
        } catch (const Cli::Error& e) {
            return QJsonObject{{"error", e.code + ": " + e.message}};
        }
    };
    const QString text = "Drei Kills in fünf Sekunden. Unglaublich!";
    QJsonObject o = run({QJsonObject{{"op", "speak"}, {"text", text}, {"voice", voice}, {"at", "2s"},
                                     {"captions", "word"}}});
    if (!CHECK(!o.contains("error"))) {
        std::cerr << qPrintable(o.value("error").toString()) << "\n";
        return;
    }
    const QString notes = QJsonDocument(o.value("notes").toArray()).toJson();
    CHECK(notes.contains("estimated from the text"));
    // Second speak at the same time: the next free track
    o = run({QJsonObject{{"op", "speak"}, {"text", "Noch was."}, {"voice", voice}, {"at", 60}, {"speed", 1.5}}});
    CHECK(!o.contains("error"));
    // Errors: no text, not an audio track
    CHECK(run({QJsonObject{{"op", "speak"}, {"voice", voice}}}).value("error").toString().startsWith("BAD_ARGUMENT"));
    CHECK(run({QJsonObject{{"op", "speak"}, {"text", "x"}, {"voice", voice}, {"track", "V1"}}})
              .value("error")
              .toString()
              .startsWith("BAD_ARGUMENT"));

    // Round trip: the clips, the file in the project's -tts folder, its words as transcript, the caption track
    ProjectData back;
    QFile in(path);
    CHECK(in.open(QIODevice::ReadOnly));
    CHECK(ProjectFile::fromJson(in.readAll(), path, &back, nullptr));
    if (!CHECK(back.timeline.audio.size() >= 2)) return;
    if (!CHECK_EQ(back.timeline.audio[0].clips.size(), 1)) return;
    const Clip& c = back.timeline.audio[0].clips[0];
    CHECK_EQ(c.start, 50);
    CHECK(c.mediaPath.startsWith(dir + "/Short-tts/speech-drei-kills-in-funf-"));
    // 0.4 s + 60 ms per character
    CHECK(std::abs(c.length() - std::lround((0.4 + 0.06 * text.size()) * 25)) <= 1);
    CHECK_EQ(back.timeline.audio[1].clips.size(), 1);
    CHECK_EQ(back.timeline.audio[1].clips.value(0).start, 60);
    const auto media = std::find_if(back.media.begin(), back.media.end(), [&](const MediaInfo& m) { return m.path == c.mediaPath; });
    if (CHECK(media != back.media.end()) && CHECK_EQ(media->transcripts.size(), 1)) {
        CHECK_EQ(media->transcripts[0].words.size(), 6);
        CHECK_EQ(media->transcripts[0].words[0].text, QString("Drei"));
    }
    if (CHECK_EQ(back.timeline.subtitles.size(), 1)) {
        const SubtitleTrack& st = back.timeline.subtitles[0];
        CHECK(st.captions.mode == CaptionMode::Word);
        CHECK(!st.cues.isEmpty());
        CHECK(st.cues.value(0).start >= 50);
        CHECK(st.cues.last().end <= c.end() + 1);
    }
}

// The dialog: an installed voice is offered, "Einfügen" with word captions makes the file and its words
void testDialog(const QString& dir)
{
    const Extensions::Item* ramona = Extensions::find("piper-voice-de_DE-ramona");
    if (!CHECK(ramona)) return;
    const QString voices = QDir(Extensions::rootDir()).filePath("piper/voices");
    QDir().mkpath(voices);
    QFile(QDir(voices).filePath(ramona->fileName)).open(QIODevice::WriteOnly);
    QFile(QDir(voices).filePath(ramona->fileName + ".json")).open(QIODevice::WriteOnly);
    SpeechDialog dlg(dir + "/Dialog-tts");
    auto* text = dlg.findChild<QPlainTextEdit*>();
    auto* voice = dlg.findChild<QComboBox*>();
    auto* captions = dlg.findChild<QCheckBox*>();
    if (!CHECK(text && voice && captions)) return;
    CHECK_EQ(voice->currentData().toString(), ramona->id);
    text->setPlainText("Hallo aus dem Dialog.\nZweite Zeile");
    captions->setChecked(true);
    dlg.show();
    QApplication::processEvents();
    if (const QString shot = qEnvironmentVariable("SCHNEIDI_TEST_SHOT"); !shot.isEmpty()) dlg.grab().save(shot);
    QPushButton* insert = nullptr;
    for (QPushButton* b : dlg.findChildren<QPushButton*>())
        if (b->text() == T("Einfügen")) insert = b;
    if (!CHECK(insert && insert->isEnabled())) return;
    QEventLoop loop;
    QObject::connect(&dlg, &QDialog::accepted, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    insert->click();
    if (dlg.isVisible()) loop.exec();
    CHECK(dlg.QDialog::result() == QDialog::Accepted);
    CHECK(dlg.captions());
    CHECK_EQ(dlg.result().words.size(), 6);
    CHECK_EQ(dlg.result().words.value(3).text, QString("Dialog."));
    CHECK(Extensions::remove(*ramona));
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsSet("SCHNEIDI_FAKE_PIPER")) {
        QCoreApplication app(argc, argv);
        return fakePiper(app.arguments());
    }
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("speech");
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    QDir(Extensions::rootDir()).removeRecursively();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testCatalog();
    testPure();
    testEstimate();
    testFreeTrack();
    testWav(tmp.path());
    testVoiceInstall(tmp.path());
    // The fake piper and a voice file of its own (a path works as voice)
    const QString log = tmp.filePath("piper.log");
    qputenv("SCHNEIDI_PIPER", QCoreApplication::applicationFilePath().toUtf8());
    qputenv("SCHNEIDI_FAKE_PIPER", log.toUtf8());
    const QString voice = tmp.filePath("de_DE-test-medium.onnx");
    QFile(voice).open(QIODevice::WriteOnly);
    QFile(voice + ".json").open(QIODevice::WriteOnly);
    qputenv("SCHNEIDI_TTS_WORDS", "estimate"); // a whisper on this machine must not change the results
    testSpeaker(tmp.path(), voice, log);
    testConcurrent(tmp.path(), voice);
    testCliOp(tmp.path(), voice);
    testVoiceVersion(tmp.path(), voice);
    testDialog(tmp.path());
    qunsetenv("SCHNEIDI_FAKE_PIPER");
    qunsetenv("SCHNEIDI_PIPER");
    QDir(Extensions::rootDir()).removeRecursively();
    return Check::result();
}
