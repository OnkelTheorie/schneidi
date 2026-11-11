// SPDX-License-Identifier: AGPL-3.0-only
// Export safety: a failed or canceled export never costs the user an older file of the target's name (video, audio,
// image sequence, SRT next to it), a disk full or write error midway is reported as a failure (not as "export
// finished" with a cut-off file), an image sequence replaces an older one completely (a shorter one leaves no higher
// numbers behind), and an odd project size is exported at the nearest even size (4:2:0 encoders refuse odd sizes).
// An export that cannot go on (no memory for the sound, a video decoder lost for good) fails saying so instead of
// hanging; a cancel while preparing returns at once; the x264/x265 intermediate of a killed render is removed later.

#include "check.h"

#include "core/RenderJob.h"
#include "engine/ExportWriter.h"
#include "engine/Exporter.h"
#include "engine/video/Reader.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
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
#include <clocale>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
constexpr int kFrames = 100; // 4 s
QString g_media;
const QByteArray kOld = "an older export the user wants to keep";

Timeline timeline()
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = g_media;
    c.out = kFrames - 1;
    c.linkId = 1;
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    SubtitleTrack subs;
    subs.cues << SubtitleCue{3, 0, 50, "Hallo"};
    tl.subtitles << subs;
    return tl;
}

ExportSettings settings(const QString& path)
{
    ExportSettings s;
    s.path = path;
    s.format = kFmt;
    s.preset = "ultrafast";
    return s;
}

enum class Stop { None, AtStart, TakeTarget };

struct Result {
    bool started = false, ok = false;
    QString message;
};

void writeFile(const QString& path, const QByteArray& data);

Result run(const ExportSettings& s, Stop stop = Stop::None, const Timeline& tl = timeline(), int timeoutMs = 120000)
{
    Exporter ex;
    QEventLoop loop;
    Result r;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& msg) {
        r.ok = success;
        r.message = msg;
        loop.quit();
    });
    if (stop == Stop::AtStart) QObject::connect(&ex, &Exporter::started, &ex, [&] { ex.cancel(); });
    // Another render to the same name finishes first: its video lands under the target while this one still runs
    if (stop == Stop::TakeTarget) QObject::connect(&ex, &Exporter::started, &ex, [&] { writeFile(s.path, kOld); });
    QString error;
    r.started = ex.start(tl, s, &error);
    if (!r.started) {
        r.message = error;
        return r;
    }
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return r;
}

QByteArray content(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data);
}

// Files of a folder besides the source clip (leftovers of an export show up here)
QStringList entries(const QString& dir)
{
    QStringList l = QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    l.removeAll("src.mp4");
    l.removeAll("temp");
    return l;
}

QJsonObject probeVideo(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_streams", "-of", "json", path});
    p.waitForFinished(30000);
    const QJsonArray streams = QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("streams").toArray();
    return streams.isEmpty() ? QJsonObject() : streams.first().toObject();
}

// Two renders to the same new name at once (two AIs, a batch twice): the one finishing later must not replace the
// first one's video without overwrite (replace = false) - it fails and leaves nothing behind; with replace it does
void testNoReplaceOfNewFile(const QString& root)
{
    const QString dir = root + "/no-replace";
    QDir().mkpath(dir);
    const QString target = dir + "/out.mp4";
    ExportSettings s = settings(target);
    s.replace = false;
    Result r = run(s, Stop::TakeTarget);
    CHECK(r.started);
    CHECK(!r.ok);
    CHECK_EQ(content(target), kOld);
    CHECK_EQ(entries(dir), QStringList{"out.mp4"});

    QFile::remove(target);
    s.replace = true;
    r = run(s, Stop::TakeTarget);
    CHECK(r.ok);
    CHECK(content(target) != kOld);
    CHECK_EQ(entries(dir), QStringList{"out.mp4"});
}

// A render killed midway (SIGKILL, crash) leaves its part file: the next finished render to that name removes it
// once it is an hour old, but not the fresh one of a render still running, nor other files
void testStaleParts(const QString& root)
{
    const QString dir = root + "/stale";
    QDir().mkpath(dir);
    const QString stale = dir + "/out.deadbeef.part.mp4", fresh = dir + "/out.cafe0123.part.mp4",
                  other = dir + "/other.deadbeef.part.mp4";
    // The x264/x265 extension's intermediate (ExternalEncoder::intermediatePath of the part file, up to several GB):
    // a killed render's goes once an hour old - also one of another render into this folder; a running render's
    // (written all the time) and a user's file of a similar name stay
    const QString staleNut = dir + "/.out.deadbeef.part.mp4.schneidi-video.nut",
                  freshNut = dir + "/.out.cafe0123.part.mp4.schneidi-video.nut",
                  otherNut = dir + "/.clip.0123abcd.part.mkv.schneidi-video.nut", userNut = dir + "/.notes.schneidi-video.nut";
    for (const QString& f : {stale, fresh, other, staleNut, freshNut, otherNut, userNut}) writeFile(f, kOld);
    for (const QString& f : {stale, other, staleNut, otherNut, userNut}) {
        QFile file(f);
        if (file.open(QIODevice::ReadWrite))
            CHECK(file.setFileTime(QDateTime::currentDateTime().addSecs(-7200), QFileDevice::FileModificationTime));
    }
    CHECK(run(settings(dir + "/out.mp4")).ok);
    CHECK_EQ(entries(dir), (QStringList{".notes.schneidi-video.nut", ".out.cafe0123.part.mp4.schneidi-video.nut",
                                        "other.deadbeef.part.mp4", "out.cafe0123.part.mp4", "out.mp4"}));
}

void testKeepsOlderFile(const QString& root)
{
    {
        const QString dir = root + "/keep-video";
        QDir().mkpath(dir);
        const QString target = dir + "/out.mp4", srt = dir + "/out.srt";

        // The encoder cannot be opened (a hardware encoder the test machine has no card for, no software fallback):
        // nothing was written yet, the older file and SRT must stay untouched
        writeFile(target, kOld);
        writeFile(srt, kOld);
        ExportSettings s = settings(target);
        s.videoCodec = "h264_nvenc";
        s.subtitlePath = srt;
        Result r = run(s);
        CHECK(r.started);
        if (r.ok) { // this machine has the card: nothing to check, put the older files back
            std::printf("       h264_nvenc works here, failure case not checked\n");
            writeFile(target, kOld);
            writeFile(srt, kOld);
        }
        CHECK_EQ(content(target), kOld);
        CHECK_EQ(content(srt), kOld);
        CHECK_EQ(entries(dir), QStringList({"out.mp4", "out.srt"}));

        // Canceled while rendering: the older files stay, the half-finished ones go
        s = settings(target);
        s.subtitlePath = srt;
        r = run(s, Stop::AtStart);
        CHECK(!r.ok);
        CHECK_EQ(content(target), kOld);
        CHECK_EQ(content(srt), kOld);
        CHECK_EQ(entries(dir), QStringList({"out.mp4", "out.srt"}));

        // Finished: both replaced, nothing else left
        r = run(s);
        CHECK(r.ok);
        CHECK_EQ(probeVideo(target).value("codec_name").toString(), QString("h264"));
        CHECK(content(srt).contains("-->"));
        CHECK_EQ(entries(dir), QStringList({"out.mp4", "out.srt"}));
    }

    // Audio only
    const QString dir = root + "/keep-audio";
    QDir().mkpath(dir);
    const QString target = dir + "/out.wav";
    writeFile(target, kOld);
    ExportSettings s = settings(target);
    s.videoCodec.clear();
    s.audioCodec = "pcm_s16le";
    Result r = run(s, Stop::AtStart);
    CHECK(!r.ok);
    CHECK_EQ(content(target), kOld);
    CHECK_EQ(entries(dir), QStringList({"out.wav"}));
    r = run(s);
    CHECK(r.ok);
    CHECK(content(target).startsWith("RIFF"));
    CHECK_EQ(entries(dir), QStringList({"out.wav"}));
}

// Disk full or a write error midway (here: a file size limit for this process): "export failed", the older file stays
void testWriteErrorMidway(const QString& root)
{
#ifdef Q_OS_UNIX
    struct Case { const char* name; bool audioOnly; };
    for (const Case& c : {Case{"video.mp4", false}, Case{"audio.wav", true}}) {
        const QString dir = root + "/limit-" + c.name;
        QDir().mkpath(dir);
        const QString target = dir + "/" + c.name;
        writeFile(target, kOld);
        ExportSettings s = settings(target);
        s.crf = 1; // large frames: the limit is reached early
        if (c.audioOnly) {
            s.videoCodec.clear();
            s.audioCodec = "pcm_s16le";
        }
        rlimit old{};
        getrlimit(RLIMIT_FSIZE, &old);
        rlimit small = old;
        small.rlim_cur = 64 * 1024; // the export writes more than that
        std::signal(SIGXFSZ, SIG_IGN); // write() fails with EFBIG instead of ending the process
        setrlimit(RLIMIT_FSIZE, &small);
        const Result r = run(s);
        setrlimit(RLIMIT_FSIZE, &old);
        std::signal(SIGXFSZ, SIG_DFL);
        if (!CHECK(!r.ok)) std::printf("       %s reported as finished: %s\n", c.name, qPrintable(r.message));
        // The writer's cause in the message, not only "failed"
        if (!CHECK(r.message.contains(" (") && r.message.endsWith(")"))) std::printf("       %s\n", qPrintable(r.message));
        CHECK_EQ(content(target), kOld);
        CHECK_EQ(entries(dir), QStringList({c.name}));
    }
#else
    Q_UNUSED(root);
#endif
}

// A container that cannot hold the codecs (H.264 in WebM, video in WAV): refused at the start, saying why
void testWrongContainer(const QString& root)
{
    const QString dir = root + "/container";
    QDir().mkpath(dir);
    Result r = run(settings(dir + "/out.webm"));
    CHECK(!r.started && !r.ok);
    if (!CHECK(r.message.contains("the .webm container cannot hold libx264 video"))) std::printf("       %s\n", qPrintable(r.message));
    r = run(settings(dir + "/out.wav"));
    CHECK(!r.started && r.message.contains("the .wav container cannot hold libx264 video"));
    r = run(settings(dir + "/out.txt"));
    CHECK(!r.started && r.message.contains("no container for .txt files"));
    CHECK_EQ(entries(dir), QStringList());
}

void testImageSequence(const QString& root)
{
    const QString dir = root + "/seq-test";
    const QString frames = dir + "/seq";
    QDir().mkpath(frames);
    // An older, longer sequence and a file of the user in the same folder
    for (int i = 1; i <= 20; ++i) writeFile(frames + QString("/seq_%1.png").arg(i, 5, 10, QChar('0')), kOld);
    writeFile(frames + "/notes.txt", kOld);
    ExportSettings s = settings(dir + "/seq.png");
    s.output = ExportSettings::Output::ImageSequence;
    s.videoCodec = "png";
    s.pixFmt = "rgb24";
    s.audioCodec.clear();
    s.to = 9; // 10 frames

    // Fails at once (the encoder cannot be opened): older frames untouched
    ExportSettings bad = s;
    bad.videoCodec = "h264_nvenc";
    Result r = run(bad);
    if (r.ok) std::printf("       h264_nvenc works here, failure case not checked\n");
    else {
        CHECK_EQ(QDir(frames).entryList(QDir::Files).size(), qsizetype(21));
        CHECK_EQ(content(frames + "/seq_00001.png"), kOld);
    }
    QDir(dir + "/seq").removeRecursively(); // the older sequence again (also when the card worked)
    QDir().mkpath(frames);
    for (int i = 1; i <= 20; ++i) writeFile(frames + QString("/seq_%1.png").arg(i, 5, 10, QChar('0')), kOld);
    writeFile(frames + "/notes.txt", kOld);
    CHECK_EQ(QDir(frames).entryList(QDir::Files).size(), qsizetype(21));
    CHECK_EQ(content(frames + "/seq_00001.png"), kOld);
    CHECK_EQ(entries(dir), QStringList({"seq"}));

    // Canceled: the frames written so far go, the older ones stay
    r = run(s, Stop::AtStart);
    CHECK(!r.ok);
    CHECK_EQ(QDir(frames).entryList(QDir::Files).size(), qsizetype(21));
    CHECK_EQ(content(frames + "/seq_00001.png"), kOld);
    CHECK_EQ(entries(dir), QStringList({"seq"}));

    // Finished: exactly the 10 new frames (the older 11…20 are gone), the user's file stays
    r = run(s);
    CHECK(r.ok);
    QStringList want;
    for (int i = 1; i <= 10; ++i) want << QString("seq_%1.png").arg(i, 5, 10, QChar('0'));
    want << "notes.txt";
    want.sort();
    CHECK_EQ(QDir(frames).entryList(QDir::Files, QDir::Name), want);
    CHECK_EQ(probeVideo(frames + "/seq_00010.png").value("codec_name").toString(), QString("png"));
    CHECK_EQ(content(frames + "/notes.txt"), kOld);
    CHECK_EQ(entries(dir), QStringList({"seq"}));
}

// No memory for the sound encoder's frames: the export fails saying so. The encode thread used to end without a
// word, the export waited for it forever (progress stuck, only a cancel ended it).
void testSoundOutOfMemory(const QString& root)
{
    const QString dir = root + "/sound-memory";
    QDir().mkpath(dir);
    ExportWriter::failSoundBufferForTests(true);
    QElapsedTimer took;
    took.start();
    const Result r = run(settings(dir + "/out.mp4"), Stop::None, timeline(), 30000);
    ExportWriter::failSoundBufferForTests(false);
    CHECK(r.started);
    CHECK(!r.ok);
    if (!CHECK(r.message.contains("out of memory")))
        std::printf("       message after %lld ms: '%s'\n", took.elapsed(), qPrintable(r.message));
    CHECK_EQ(entries(dir), QStringList());
}

// A video's decoder lost for good (after a hardware decoder failed, software would not open: out of memory): the
// export fails saying so. Its frames used to wait for the decoder forever (and a cancel with them).
void testDecoderLost(const QString& root)
{
    const QString dir = root + "/decoder-lost";
    QDir().mkpath(dir);
    Video::Reader::loseDecoderForTests(true);
    QElapsedTimer took;
    took.start();
    const Result r = run(settings(dir + "/out.mp4"), Stop::None, timeline(), 30000);
    Video::Reader::loseDecoderForTests(false);
    CHECK(r.started);
    CHECK(!r.ok);
    if (!CHECK(!r.message.isEmpty())) std::printf("       no answer after %lld ms\n", took.elapsed());
    CHECK_EQ(entries(dir), QStringList());
}

// Cancel while the export is still being prepared (smart render opens every file it may copy from): returns at once,
// not after all of them were opened
void testCancelWhilePreparing(const QString& root)
{
    const QString dir = root + "/prepare";
    QDir().mkpath(dir);
    constexpr int kFiles = 100, kEach = 10;
    Timeline tl;
    tl.video.resize(1);
    for (int i = 0; i < kFiles; ++i) {
        const QString file = dir + QString("/clip%1.mp4").arg(i, 3, 10, QChar('0'));
        QFile::copy(g_media, file);
        Clip c;
        c.id = i + 1;
        c.mediaPath = file;
        c.start = i * kEach;
        c.out = kEach - 1;
        tl.video[0].clips << c;
    }
    ExportSettings s = settings(dir + "/out.mp4");
    s.smartRender = true;
    s.threads = 2;
    s.audioCodec.clear();
    // Time to get going without a cancel (prepared = every file opened)
    qint64 prepare = 0;
    {
        Exporter ex;
        QEventLoop loop;
        QElapsedTimer t;
        QObject::connect(&ex, &Exporter::started, &ex, [&] {
            prepare = t.elapsed();
            ex.cancel();
        });
        QObject::connect(&ex, &Exporter::finished, &loop, &QEventLoop::quit);
        t.start();
        QString error;
        if (!CHECK(ex.start(tl, s, &error))) return;
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    // Canceled right after the start
    qint64 cancel = 0;
    bool finished = false, ok = true;
    {
        Exporter ex;
        QObject::connect(&ex, &Exporter::finished, &ex, [&](bool success, const QString&) {
            finished = true;
            ok = success;
        });
        QString error;
        if (!CHECK(ex.start(tl, s, &error))) return;
        QElapsedTimer t;
        t.start();
        ex.cancel();
        cancel = t.elapsed();
        CHECK(!ex.isRunning());
    }
    CHECK(finished);
    CHECK(!ok);
    std::printf("       preparing %lld ms, cancel %lld ms\n", prepare, cancel);
    if (prepare < 60) std::printf("       preparing too fast to tell, cancel time not checked\n");
    else CHECK(cancel * 3 < prepare);
    CHECK_EQ(QDir(dir).entryList({"out*"}, QDir::Files | QDir::Hidden), QStringList());
}

// Odd project size (an import or a project file can bring one) exported at project size: the nearest even size
void testOddSize(const QString& root)
{
    const ProjectFormat odd{641, 361, {25, 1}};
    {
        const QString path = root + "/odd.mp4";
        ExportSettings s = settings(path);
        s.format = odd;
        s.to = 24;
        const Result r = run(s);
        if (!CHECK(r.ok)) std::printf("       %s\n", qPrintable(r.message));
        const QJsonObject v = probeVideo(path);
        CHECK_EQ(v.value("width").toInt(), 640);
        CHECK_EQ(v.value("height").toInt(), 360);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_safety");
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QString ownTemp = tmp.filePath("temp");
    QDir().mkpath(ownTemp);
    for (const char* var : {"TMPDIR", "TMP", "TEMP"}) qputenv(var, QDir::toNativeSeparators(ownTemp).toLocal8Bit());
    g_media = Check::makeMedia(tmp.filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4", "-f", "lavfi", "-i",
                                "sine=frequency=440:duration=4", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                "-shortest"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    testKeepsOlderFile(tmp.path());
    testNoReplaceOfNewFile(tmp.path());
    testStaleParts(tmp.path());
    testWriteErrorMidway(tmp.path());
    testWrongContainer(tmp.path());
    testImageSequence(tmp.path());
    testOddSize(tmp.path());
    testSoundOutOfMemory(tmp.path());
    testCancelWhilePreparing(tmp.path());
    testDecoderLost(tmp.path()); // last: before the fix it hangs
    return Check::result();
}
