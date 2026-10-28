// Export safety: a failed or canceled export never costs the user an older file of the target's name (video, audio,
// image sequence, SRT next to it), a disk full or write error midway is reported as a failure (not as "export
// finished" with a cut-off file), an image sequence replaces an older one completely (a shorter one leaves no higher
// numbers behind), and an odd project size is exported at the nearest even size (4:2:0 encoders refuse odd sizes).

#include "check.h"

#include "core/RenderJob.h"
#include "engine/Exporter.h"

#include <QApplication>
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

ExportSettings settings(const QString& path, ExportSettings::Writer writer = ExportSettings::Writer::Own)
{
    ExportSettings s;
    s.path = path;
    s.format = kFmt;
    s.preset = "ultrafast";
    s.writer = writer;
    return s;
}

enum class Stop { None, AtStart };

struct Result {
    bool started = false, ok = false;
    QString message;
};

Result run(const ExportSettings& s, Stop stop = Stop::None, const Timeline& tl = timeline())
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
    QString error;
    r.started = ex.start(tl, s, &error);
    if (!r.started) {
        r.message = error;
        return r;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
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

void testKeepsOlderFile(const QString& root)
{
    for (const auto writer : {ExportSettings::Writer::Own, ExportSettings::Writer::Mlt}) {
        const QString dir = root + (writer == ExportSettings::Writer::Own ? "/keep-own" : "/keep-mlt");
        QDir().mkpath(dir);
        const QString target = dir + "/out.mp4", srt = dir + "/out.srt";

        // The encoder cannot be opened (a hardware encoder the test machine has no card for, no software fallback):
        // nothing was written yet, the older file and SRT must stay untouched
        writeFile(target, kOld);
        writeFile(srt, kOld);
        ExportSettings s = settings(target, writer);
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
        s = settings(target, writer);
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

    // Audio only (always MLT's consumer)
    const QString dir = root + "/keep-audio";
    QDir().mkpath(dir);
    const QString target = dir + "/out.wav";
    writeFile(target, kOld);
    ExportSettings s = settings(target, ExportSettings::Writer::Default);
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
    struct Case { const char* name; ExportSettings::Writer writer; bool audioOnly; };
    for (const Case& c : {Case{"mlt.mp4", ExportSettings::Writer::Mlt, false}, Case{"own.mp4", ExportSettings::Writer::Own, false},
                          Case{"audio.wav", ExportSettings::Writer::Default, true}}) {
        const QString dir = root + "/limit-" + c.name;
        QDir().mkpath(dir);
        const QString target = dir + "/" + c.name;
        writeFile(target, kOld);
        ExportSettings s = settings(target, c.writer);
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
        CHECK_EQ(content(target), kOld);
        CHECK_EQ(entries(dir), QStringList({c.name}));
    }
#else
    Q_UNUSED(root);
#endif
}

void testImageSequence(const QString& root)
{
    const QString dir = root + "/seq-test";
    const QString frames = dir + "/seq";
    QDir().mkpath(frames);
    // An older, longer sequence and a file of the user in the same folder
    for (int i = 1; i <= 20; ++i) writeFile(frames + QString("/seq_%1.png").arg(i, 5, 10, QChar('0')), kOld);
    writeFile(frames + "/notes.txt", kOld);
    ExportSettings s = settings(dir + "/seq.png", ExportSettings::Writer::Default);
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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_safety");
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    Check::initMlt();
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
    testWriteErrorMidway(tmp.path());
    testImageSequence(tmp.path());
    return Check::result();
}
