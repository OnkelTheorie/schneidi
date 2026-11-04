// SPDX-License-Identifier: AGPL-3.0-only
// Audio events (engine/AudioEvents, heuristics): synthetic sounds (tests/synth_sounds.h) one after another with
// silence between - each kind is found at its time (edges within 0.35 s, a shot within 20 ms) - and each sound on its
// own gives no events of other kinds (false positives); a shot over speech; the file path (WAV at 48 kHz stereo, an
// AAC file made by ffmpeg lavfi) with the cache (second call from memory, then from disk, a changed file measured
// again); `only`/`minConf`; YAMNet required but missing = error, Auto falls back to the heuristics; an unloadable
// ONNX Runtime (garbage file, a library without OrtGetApiBase) is reported, nothing crashes; `peaks {events}`.
#include "cli/Commands.h"
#include "engine/AudioEvents.h"
#include "engine/Inference.h"

#include "check.h"
#include "synth_sounds.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QJsonArray>
#include <QLibraryInfo>
#include <QTemporaryDir>

#include <cstdio>

namespace {

using AudioEvents::Event;

QString list(const QVector<Event>& ev)
{
    QString s;
    for (const Event& e : ev) s += QString("\n         %1 %2-%3 (%4)").arg(e.kind).arg(e.from, 0, 'f', 2).arg(e.to, 0, 'f', 2).arg(e.conf, 0, 'f', 2);
    return s;
}

// An event of `kind` with both edges within tol of [from, to]
bool found(const QVector<Event>& ev, const QString& kind, double from, double to, double tol)
{
    for (const Event& e : ev)
        if (e.kind == kind && std::abs(e.from - from) <= tol && std::abs(e.to - to) <= tol) return true;
    return false;
}

// Seconds of `kind` events overlapping [from, to]
double covered(const QVector<Event>& ev, const QString& kind, double from = 0, double to = 1e9)
{
    double s = 0;
    for (const Event& e : ev)
        if (e.kind == kind) s += std::max(0.0, std::min(e.to, to) - std::max(e.from, from));
    return s;
}

int count(const QVector<Event>& ev, const QString& kind)
{
    int n = 0;
    for (const Event& e : ev) n += e.kind == kind;
    return n;
}

#define FOUND(ev, kind, from, to, tol) Check::report(found(ev, kind, from, to, tol), "found " kind " " #from "-" #to, __FILE__, __LINE__, list(ev))
#define COUNT(ev, kind, n) Check::report(count(ev, kind) == (n), "count " kind " == " #n, __FILE__, __LINE__, list(ev))
#define NONE(ev, kind, max) Check::report(covered(ev, kind) <= (max), "no " kind, __FILE__, __LINE__, list(ev))

constexpr int kRate = AudioEvents::kRate;

// Everything one after another, silence between (times in the comments of each part)
unsigned g_seed = 7; // AUDIO_EVENTS_TUNE=seeds runs the sequence with other seeds (how robust the rules are)
std::vector<float> sequence(int rate)
{
    Synth::Builder b(rate, 31.0, qEnvironmentVariableIntValue("AUDIO_EVENTS_SEED") > 0 ? unsigned(qEnvironmentVariableIntValue("AUDIO_EVENTS_SEED")) : g_seed);
    Synth::speech(b, 1.0, 6.0);   // 1-7
    Synth::music(b, 8.0, 6.0);    // 8-14
    Synth::applause(b, 15.0, 3.0); // 15-18
    Synth::laughter(b, 19.0);     // 19-~20.3
    Synth::shot(b, 22.0);         // 22
    Synth::scream(b, 24.0, 1.2);  // 24-25.2
    Synth::cheering(b, 26.5, 3.0); // 26.5-29.5
    b.floor();
    return b.s;
}

void testSequence()
{
    const QVector<Event> ev = AudioEvents::heuristics(sequence(kRate));
    // Speech ends with its last syllable (the generator leaves up to 0.45 s of pause at the end)
    CHECK(covered(ev, "speech", 1, 7) >= 4.0);
    FOUND(ev, "speech", 1.0, 6.6, 0.45);
    FOUND(ev, "music", 8.0, 14.0, 0.35);
    FOUND(ev, "applause", 15.0, 18.0, 0.5); // its last 0.8 s fade out
    FOUND(ev, "laughter", 19.0, 20.2, 0.3);
    FOUND(ev, "scream", 24.0, 25.2, 0.3);
    FOUND(ev, "cheering", 26.5, 29.5, 0.5);
    FOUND(ev, "silence", 0.0, 1.0, 0.1);
    FOUND(ev, "silence", 22.6, 24.0, 0.2);
    // The shot: its onset to the frame
    bool shotAt = false;
    for (const Event& e : ev) shotAt |= e.kind == "shot" && std::abs(e.from - 22.0) <= 0.02;
    Check::report(shotAt, "shot at 22.0 s", __FILE__, __LINE__, list(ev));
    COUNT(ev, "shot", 1);
    // Nothing of a kind outside its part (a little slack at the edges)
    CHECK(covered(ev, "speech") - covered(ev, "speech", 0.5, 7.5) <= 0.3);
    CHECK(covered(ev, "music") - covered(ev, "music", 7.5, 14.5) <= 0.3);
    CHECK(covered(ev, "applause") - covered(ev, "applause", 14.5, 18.5) <= 0.3);
    CHECK(covered(ev, "cheering") - covered(ev, "cheering", 26, 30) <= 0.3);
    COUNT(ev, "laughter", 1);
    COUNT(ev, "scream", 1);
    for (const Event& e : ev) {
        CHECK(e.conf > 0 && e.conf <= 1);
        CHECK(e.to > e.from);
        CHECK_EQ(e.by, QString("heuristic"));
        CHECK(AudioEvents::kinds().contains(e.kind));
    }
}

// Each sound alone (20 s or a few times over): only its own kind (and silence)
void testAlone()
{
    {
        Synth::Builder b(kRate, 22, 11);
        Synth::speech(b, 1.0, 20.0, 120);
        b.floor();
        const QVector<Event> ev = AudioEvents::heuristics(b.s);
        CHECK(covered(ev, "speech") >= 14);
        NONE(ev, "music", 0.5);
        NONE(ev, "applause", 0);
        NONE(ev, "cheering", 0.5);
        NONE(ev, "laughter", 0);
        NONE(ev, "shot", 0);
        NONE(ev, "scream", 0);
    }
    {
        Synth::Builder b(kRate, 22, 12);
        Synth::speech(b, 1.0, 20.0, 210); // a higher voice
        b.floor();
        const QVector<Event> ev = AudioEvents::heuristics(b.s);
        CHECK(covered(ev, "speech") >= 14);
        NONE(ev, "music", 0.5);
        NONE(ev, "laughter", 0);
        NONE(ev, "scream", 0);
        NONE(ev, "shot", 0);
    }
    {
        Synth::Builder b(kRate, 22, 13);
        Synth::music(b, 1.0, 20.0);
        b.floor();
        const QVector<Event> ev = AudioEvents::heuristics(b.s);
        FOUND(ev, "music", 1.0, 21.0, 0.35);
        NONE(ev, "speech", 0.5);
        NONE(ev, "applause", 0);
        NONE(ev, "cheering", 0.5);
        NONE(ev, "laughter", 0);
        NONE(ev, "shot", 0);
        NONE(ev, "scream", 0);
    }
    {
        Synth::Builder b(kRate, 12, 14);
        Synth::applause(b, 1.0, 10.0);
        b.floor();
        const QVector<Event> ev = AudioEvents::heuristics(b.s);
        FOUND(ev, "applause", 1.0, 11.0, 0.5);
        NONE(ev, "speech", 0);
        NONE(ev, "music", 0);
        NONE(ev, "cheering", 0.5);
        NONE(ev, "laughter", 0);
        NONE(ev, "shot", 0);
        NONE(ev, "scream", 0);
    }
    {
        Synth::Builder b(kRate, 12, 15);
        Synth::cheering(b, 1.0, 10.0);
        b.floor();
        const QVector<Event> ev = AudioEvents::heuristics(b.s);
        FOUND(ev, "cheering", 1.0, 11.0, 0.6);
        NONE(ev, "speech", 0.5);
        NONE(ev, "music", 0.5);
        NONE(ev, "applause", 0.5);
        NONE(ev, "laughter", 0);
        NONE(ev, "shot", 0);
        NONE(ev, "scream", 0);
    }
    {
        // Laughter three times, a shot and a scream, with pauses
        Synth::Builder b(kRate, 14, 16);
        Synth::laughter(b, 1.0);
        Synth::laughter(b, 4.0, 5);
        Synth::laughter(b, 7.0, 7);
        Synth::shot(b, 10.0, 0.5);
        Synth::scream(b, 11.5, 0.8);
        b.floor();
        const QVector<Event> ev = AudioEvents::heuristics(b.s);
        COUNT(ev, "laughter", 3);
        COUNT(ev, "shot", 1);
        COUNT(ev, "scream", 1);
        NONE(ev, "speech", 0.5);
        NONE(ev, "music", 0);
        NONE(ev, "applause", 0);
        NONE(ev, "cheering", 0);
    }
}

// A shot in the middle of speech and one under music; no shot on a quiet word after a pause
void testShotOverSound()
{
    Synth::Builder b(kRate, 16, 21);
    Synth::speech(b, 0.5, 7.0);
    Synth::shot(b, 3.3, 0.9);
    Synth::music(b, 8.0, 7.0);
    Synth::shot(b, 11.15, 0.9);
    b.floor();
    const QVector<Event> ev = AudioEvents::heuristics(b.s);
    int near = 0;
    for (const Event& e : ev)
        if (e.kind == "shot") near += std::abs(e.from - 3.3) <= 0.02 || std::abs(e.from - 11.15) <= 0.02;
    Check::report(near == 2 && count(ev, "shot") == 2, "two shots at 3.3 and 11.15 s", __FILE__, __LINE__, list(ev));
}

void testSilenceOnly()
{
    CHECK(AudioEvents::heuristics({}).isEmpty());
    const QVector<Event> ev = AudioEvents::heuristics(std::vector<float>(size_t(kRate * 3), 0.f));
    CHECK_EQ(ev.size(), 1);
    CHECK(found(ev, "silence", 0, 3, 0.02));
    CHECK(!ev.isEmpty() && ev[0].conf >= 0.99);
}

void testProgressCancels()
{
    int calls = 0;
    const QVector<Event> ev = AudioEvents::heuristics(sequence(kRate), [&calls](double) { return ++calls < 2; });
    CHECK(ev.isEmpty());
    CHECK_EQ(calls, 2);
}

// The file path: decoding at 16 kHz from a 48 kHz stereo WAV, the cache
void testFile(const QDir& dir)
{
    const QString wav = dir.filePath("Szene ä.wav");
    CHECK(Synth::writeWav(wav, sequence(48000), 48000, 2));
    AudioEvents::clearMemoryCache();
    const int before = AudioEvents::measuredCount();
    AudioEvents::Result r = AudioEvents::analyzeFile(wav);
    CHECK(r.error.isEmpty());
    CHECK_EQ(r.by, QString("heuristic"));
    CHECK(!r.cached);
    CHECK(std::abs(r.duration - 31.0) < 0.05);
    CHECK_EQ(AudioEvents::measuredCount(), before + 1);
    FOUND(r.events, "music", 8.0, 14.0, 0.35);
    FOUND(r.events, "applause", 15.0, 18.0, 0.35);
    bool shotAt = false;
    for (const Event& e : r.events) shotAt |= e.kind == "shot" && std::abs(e.from - 22.0) <= 0.02;
    CHECK(shotAt);
    // Same answer from the memory cache, then from the disk
    const AudioEvents::Result again = AudioEvents::analyzeFile(wav);
    CHECK(again.cached);
    CHECK_EQ(again.events.size(), r.events.size());
    AudioEvents::clearMemoryCache();
    const AudioEvents::Result disk = AudioEvents::analyzeFile(wav);
    CHECK(disk.cached);
    CHECK_EQ(AudioEvents::measuredCount(), before + 1);
    CHECK_EQ(disk.events.size(), r.events.size());
    for (int i = 0; i < std::min(disk.events.size(), r.events.size()); ++i) {
        CHECK_EQ(disk.events[i].kind, r.events[i].kind);
        CHECK(std::abs(disk.events[i].from - r.events[i].from) < 0.001);
    }
    CHECK(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/audio-events").entryList({"*.json"}).size() >= 1);
    // A changed file is measured again
    {
        QFile f(wav);
        CHECK(f.open(QIODevice::ReadWrite));
        CHECK(f.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
    }
    AudioEvents::Result changed = AudioEvents::analyzeFile(wav);
    CHECK(!changed.cached);
    CHECK_EQ(AudioEvents::measuredCount(), before + 2);
    // Without the cache: measured, nothing taken
    AudioEvents::Options fresh;
    fresh.useCache = false;
    CHECK(!AudioEvents::analyzeFile(wav, 0, fresh).cached);
    CHECK_EQ(AudioEvents::measuredCount(), before + 3);
    // Filters
    AudioEvents::Options only;
    only.only = {"shot", "music"};
    const QVector<Event> some = AudioEvents::analyze(wav, 0, only);
    CHECK_EQ(count(some, "shot") + count(some, "music"), int(some.size()));
    CHECK(count(some, "music") >= 1);
    AudioEvents::Options sure;
    sure.minConf = 0.99;
    for (const Event& e : AudioEvents::analyze(wav, 0, sure)) CHECK(e.conf >= 0.99);
    // Errors: no file, no such stream
    CHECK(!AudioEvents::analyzeFile(dir.filePath("missing.wav")).error.isEmpty());
    CHECK(!AudioEvents::analyzeFile(wav, 3).error.isEmpty());
    CHECK(AudioEvents::analyze(wav, 3).isEmpty());
}

// YAMNet missing: required = error and no events; Auto = the heuristics
void testWithoutYamnet(const QDir& dir)
{
    if (AudioEvents::yamnetAvailable()) {
        std::printf("YAMNet is installed here - the 'missing' part is skipped\n");
        return;
    }
    const QString wav = dir.filePath("short.wav");
    Synth::Builder b(16000, 4, 3);
    Synth::shot(b, 1.0);
    CHECK(Synth::writeWav(wav, b.s, 16000));
    AudioEvents::Options o;
    o.engine = AudioEvents::Engine::Yamnet;
    const AudioEvents::Result r = AudioEvents::analyzeFile(wav, 0, o);
    CHECK(r.events.isEmpty());
    CHECK(r.error.contains("YAMNet") || r.error.contains("ONNX"));
    const AudioEvents::Result autoR = AudioEvents::analyzeFile(wav);
    CHECK(autoR.error.isEmpty());
    CHECK_EQ(autoR.by, QString("heuristic"));
    CHECK_EQ(count(autoR.events, "shot"), 1);
    QString why;
    CHECK(AudioEvents::yamnet(b.s, &why).isEmpty());
    CHECK(!why.isEmpty());
}

// ONNX Runtime that cannot be used: never a crash, a message instead
void testBrokenRuntime(const QDir& dir)
{
    const QString garbage = dir.filePath("libonnxruntime-broken.so");
    {
        QFile f(garbage);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(4096, 'x'));
    }
    QString error;
    CHECK(!Inference::probe(garbage, &error));
    CHECK(error.contains("cannot load"));
    CHECK(!Inference::probe(dir.filePath("does-not-exist.so"), &error));
    // A real library that is no ONNX Runtime (Qt's own)
#ifdef Q_OS_LINUX
    const QString qt = QLibraryInfo::path(QLibraryInfo::LibrariesPath) + "/libQt6Core.so.6";
    if (QFileInfo::exists(qt)) {
        CHECK(!Inference::probe(qt, &error));
        CHECK(error.contains("OrtGetApiBase"));
    }
#endif
    // The shared runtime with only that file: unavailable, with the reason; a model cannot be opened
    qputenv("SCHNEIDI_ONNXRUNTIME", garbage.toUtf8());
    CHECK_EQ(Inference::candidates(), QStringList{garbage});
    CHECK(!Inference::available(&error));
    CHECK(error.contains("cannot load"));
    CHECK(Inference::libraryPath().isEmpty());
    CHECK(!Inference::Session::fromBytes(QByteArray("not a model"), &error));
    CHECK(!error.isEmpty());
    // A model "installed" by hand: YAMNet still reports the runtime, the heuristics answer in Auto
    const QString model = dir.filePath("yamnet.onnx");
    QFile(model).open(QIODevice::WriteOnly);
    QFile labels(dir.filePath("yamnet_class_map.csv"));
    CHECK(labels.open(QIODevice::WriteOnly));
    labels.write("index,mid,display_name\n0,/m/09x0r,Speech\n");
    labels.close();
    qputenv("SCHNEIDI_YAMNET", model.toUtf8());
    QString why;
    CHECK(!AudioEvents::yamnetAvailable(&why));
    CHECK(why.contains("cannot load"));
    Synth::Builder b(16000, 3, 4);
    Synth::shot(b, 1.0);
    const QString wav = dir.filePath("shot.wav");
    CHECK(Synth::writeWav(wav, b.s, 16000));
    const AudioEvents::Result r = AudioEvents::analyzeFile(wav);
    CHECK_EQ(r.by, QString("heuristic"));
    CHECK_EQ(count(r.events, "shot"), 1);
    qunsetenv("SCHNEIDI_YAMNET");
}

// An AAC file made by ffmpeg (lavfi): a bang at 2 s on silence, a tone chord from 4 s
// A file that ends in a read error (cut off: a copy that broke, a network share gone, a recording still being
// written): the events of what was read, but nothing cached - the next call measures again
void testCutOff(const QDir& dir)
{
    if (!Check::haveFfmpeg()) {
        std::printf("no ffmpeg - cut-off part skipped\n");
        return;
    }
    const QString whole = Check::makeMedia(dir.filePath("whole.m4a"), {"-f", "lavfi", "-i", "sine=f=440:d=20", "-c:a",
                                                                         "aac", "-movflags", "+faststart"});
    if (!CHECK(!whole.isEmpty())) return;
    const QString cut = dir.filePath("cut.m4a");
    {
        QFile in(whole), out(cut);
        if (!CHECK(in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly))) return;
        out.write(in.read(in.size() / 2)); // the index at the start says 20 s, half of the samples are there
    }
    AudioEvents::clearMemoryCache();
    const int before = AudioEvents::measuredCount();
    const AudioEvents::Result r = AudioEvents::analyzeFile(cut);
    CHECK(r.incomplete);
    CHECK(!r.cached);
    AudioEvents::clearMemoryCache();
    const AudioEvents::Result again = AudioEvents::analyzeFile(cut);
    CHECK(!again.cached);
    CHECK_EQ(AudioEvents::measuredCount(), before + 2);
    // The whole file reads to its end: cached as before
    CHECK(!AudioEvents::analyzeFile(whole).incomplete);
    CHECK(AudioEvents::analyzeFile(whole).cached);
}

void testLavfi(const QDir& dir)
{
    if (!Check::haveFfmpeg()) {
        std::printf("no ffmpeg - lavfi part skipped\n");
        return;
    }
    const QString file = Check::makeMedia(
        dir.filePath("lavfi.m4a"),
        {"-f", "lavfi", "-i",
         "aevalsrc='if(between(t,2,2.6),0.8*(2*random(0)-1)*exp(-(t-2)/0.05),0)"
         "+if(gte(t,4),0.1*(sin(2*PI*261.6*t)+sin(2*PI*329.6*t)+sin(2*PI*392*t)+0.5*sin(2*PI*523.2*t)),0)"
         "+0.0009*(2*random(1)-1)':s=48000:d=12",
         "-c:a", "aac", "-b:a", "192k"});
    if (file.isEmpty()) {
        CHECK(false);
        return;
    }
    const QVector<Event> ev = AudioEvents::analyze(file);
    bool shotAt = false;
    for (const Event& e : ev) shotAt |= e.kind == "shot" && std::abs(e.from - 2.0) <= 0.03;
    Check::report(shotAt, "lavfi shot at 2 s", __FILE__, __LINE__, list(ev));
    FOUND(ev, "music", 4.0, 12.0, 0.4);
    FOUND(ev, "silence", 2.25, 4.0, 0.15); // the bang has faded by 2.25 s
    NONE(ev, "speech", 0);
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    if (!cmd) return {{"failed", "NO_COMMAND"}};
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"failed", e.code}};
    }
}

// peaks {events}: the events beside the peaks (seconds and frames); without the option nothing changes
void testCli(const QDir& dir)
{
    const QString wav = dir.filePath("cli.wav");
    Synth::Builder b(48000, 12, 5);
    Synth::music(b, 1.0, 5.0);
    Synth::shot(b, 8.0);
    b.floor();
    CHECK(Synth::writeWav(wav, b.s, 48000));
    const QJsonObject plain = run("peaks", {{"file", wav}});
    CHECK(!plain.contains("failed"));
    CHECK(!plain.contains("events"));
    const QJsonObject o = run("peaks", {{"file", wav}, {"events", true}});
    const QJsonArray ev = o["events"].toArray();
    CHECK(!ev.isEmpty());
    CHECK_EQ(o["events_by"].toString(), QString("heuristic"));
    bool music = false, shot = false;
    for (const QJsonValue& v : ev) {
        const QJsonObject e = v.toObject();
        music |= e["kind"] == "music" && std::abs(e["from_s"].toDouble() - 1.0) < 0.4;
        shot |= e["kind"] == "shot" && std::abs(e["from_s"].toDouble() - 8.0) < 0.03;
        CHECK(e.contains("from") && e.contains("to") && e.contains("conf") && e.contains("by"));
    }
    CHECK(music);
    CHECK(shot);
    const QJsonObject only = run("peaks", {{"file", wav}, {"events", "heuristic"}, {"event_kinds", "shot"}});
    CHECK_EQ(only["events"].toArray().size(), 1);
    const QJsonObject bad = run("peaks", {{"file", wav}, {"events", "magic"}});
    CHECK_EQ(bad["failed"].toString(), QString("BAD_ARGUMENT"));
    const QJsonObject badKind = run("peaks", {{"file", wav}, {"events", true}, {"event_kinds", "speech,dogs"}});
    CHECK_EQ(badKind["failed"].toString(), QString("BAD_ARGUMENT"));
    if (!AudioEvents::yamnetAvailable()) {
        const QJsonObject y = run("peaks", {{"file", wav}, {"events", "yamnet"}});
        CHECK_EQ(y["failed"].toString(), QString("NOT_INSTALLED"));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("audio-events");
    Check::initEngine();
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/audio-events").removeRecursively();
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    // Tuning: AUDIO_EVENTS_DUMP=1 AUDIO_EVENTS_TUNE=1 prints the block features of the sequence only
    if (qEnvironmentVariableIsSet("AUDIO_EVENTS_TUNE")) {
        const QString t = qEnvironmentVariable("AUDIO_EVENTS_TUNE");
        if (t == "seeds") {
            for (unsigned seed = 1; seed <= 30; ++seed) {
                g_seed = seed;
                const int before = Check::fails();
                testSequence();
                std::printf("seed %u: %d failed\n", seed, Check::fails() - before);
            }
        } else if (t == "shot") testShotOverSound();
        else if (t == "file") testFile(dir);
        else testSequence();
        return Check::result();
    }
    testSequence();
    testAlone();
    testShotOverSound();
    testSilenceOnly();
    testProgressCancels();
    testFile(dir);
    testLavfi(dir);
    testCutOff(dir);
    testCli(dir);
    testWithoutYamnet(dir);
    testBrokenRuntime(dir); // last: it points the shared runtime at a broken file for the rest of the program
    return Check::result();
}
