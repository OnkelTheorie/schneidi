// SPDX-License-Identifier: AGPL-3.0-only
// x264/x265 as an extension (0.16): catalog and install paths, CLI list parsing, encoder order (hardware > x264/x265
// built in or from the extension > free OpenH264/kvazaar), the hints of check/render, the external program's command
// line, and a real export through an ffmpeg with libx264 as the "extension" (the distribution's ffmpeg, when it has
// x264) plus the fallback to the free encoder when the program breaks. No network.

#include "check.h"

#include "core/RenderJob.h"
#include "engine/Exporter.h"
#include "engine/Extensions.h"
#include "engine/ExternalEncoder.h"
#include "engine/HardwareEncoders.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <QTimer>

#include <cmath>
#include <thread>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace {

void testCatalog()
{
    const Extensions::Item* x264 = Extensions::find("x264");
#if (defined(Q_OS_LINUX) || defined(Q_OS_WIN)) && defined(Q_PROCESSOR_X86_64)
    if (!CHECK(x264 != nullptr)) return;
    CHECK(x264->kind == Extensions::Kind::Encoder);
    CHECK(x264->url.startsWith("https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-"));
    CHECK(x264->url.endsWith(x264->fileName));
    CHECK_EQ(x264->sha256.size(), 64);
    CHECK(x264->size > 50'000'000);
#endif
    if (!x264) return;
    // CLI list: ids, aliases, short model names; an unknown id names itself
    QString unknown;
    CHECK_EQ(Extensions::parseList("x264", &unknown), QVector<const Extensions::Item*>{x264});
    CHECK_EQ(Extensions::parseList(" X265 ", &unknown), QVector<const Extensions::Item*>{x264});
    CHECK_EQ(Extensions::parseList("x264,x265", &unknown).size(), 1);
    CHECK(unknown.isEmpty());
    const auto two = Extensions::parseList("x264,small", &unknown);
    CHECK_EQ(two.size(), 2);
    CHECK(two.value(1) == Extensions::find("whisper-model-small"));
    CHECK(Extensions::parseList("x264,nope", &unknown).isEmpty());
    CHECK_EQ(unknown, QString("nope"));
    // No dependencies of its own
    CHECK_EQ(Extensions::withDependencies({x264}), QVector<const Extensions::Item*>{x264});
}

// Installed = the archive's folder below extensions/ffmpeg-gpl/bin, program in its bin/
void testInstallPaths()
{
    const Extensions::Item* x264 = Extensions::find("x264");
    if (!x264) return;
    qunsetenv("SCHNEIDI_ENCODER_FFMPEG");
    CHECK(!Extensions::isInstalled(*x264));
    CHECK(Extensions::encoderProgram().isEmpty());
#ifdef Q_OS_WIN
    const QString name = "ffmpeg.exe";
#else
    const QString name = "ffmpeg";
#endif
    const QString bin = QDir(Extensions::rootDir()).filePath("ffmpeg-gpl/bin/bin");
    CHECK(QDir().mkpath(bin));
    QFile f(QDir(bin).filePath(name));
    CHECK(f.open(QIODevice::WriteOnly));
    f.close();
    CHECK(Extensions::isInstalled(*x264));
    CHECK_EQ(Extensions::encoderProgram(), QDir(bin).filePath(name));
#ifndef Q_OS_WIN
    // In use (Windows: an export runs this ffmpeg, the folder cannot be renamed; here: a read-only parent folder):
    // removing fails as a whole and leaves the install working, instead of deleting it file by file
    const QString holder = QDir(bin + "/../..").canonicalPath(); // holds the encoder tree (bin/bin/ffmpeg)
    const QFileDevice::Permissions was = QFile::permissions(holder);
    QFile::setPermissions(holder, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    if (!QFileInfo(holder).isWritable()) { // root writes anyway
        CHECK(!Extensions::remove(*x264));
        CHECK(Extensions::isInstalled(*x264));
    }
    QFile::setPermissions(holder, was);
#endif
    CHECK(Extensions::remove(*x264));
    CHECK(!Extensions::isInstalled(*x264));
    CHECK(Extensions::encoderProgram().isEmpty());
    // Own program by environment
    qputenv("SCHNEIDI_ENCODER_FFMPEG", "/no/such/ffmpeg");
    CHECK(Extensions::encoderProgram().isEmpty());
    qunsetenv("SCHNEIDI_ENCODER_FFMPEG");
}

#ifdef Q_OS_UNIX
// A stand-in for the extension's ffmpeg: lists libx264/libx265, fails on everything else
QString fakeProgram(const QString& dir)
{
    const QString path = QDir(dir).filePath("fake-ffmpeg");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write("#!/bin/sh\n"
            "if [ \"$3\" = \"-encoders\" ]; then\n"
            "  echo ' V....D libx264              libx264 H.264 / AVC'\n"
            "  echo ' V....D libx265              libx265 H.265 / HEVC'\n"
            "  exit 0\n"
            "fi\n"
            "cat >/dev/null\n"
            "echo 'fake encoder broke' >&2\n"
            "exit 1\n");
    f.close();
    f.setPermissions(f.permissions() | QFile::ExeOwner);
    return path;
}
#endif

void testOrder(const QString& dir)
{
    // The extension path is only taken where x264/x265 are not built in: pretend so
    qputenv("SCHNEIDI_IGNORE_BUILTIN", "libx264,libx265");
    CHECK(!HardwareEncoders::builtIn("libx264"));
    CHECK(HardwareEncoders::externalProgram("libx264").isEmpty()); // no extension
    const QString free264 = HardwareEncoders::freeReplacement("libx264");
    CHECK_EQ(HardwareEncoders::softwareCodec("h264"), free264);
    CHECK(HardwareEncoders::externalProgram("libopenh264").isEmpty());

    // Hints: only for the free replacements, naming the install command
    const QString hint = HardwareEncoders::betterQualityHint("libopenh264");
    CHECK(hint.contains("extensions --install x264"));
    CHECK(hint.contains("OpenH264"));
    CHECK(HardwareEncoders::betterQualityHint("libkvazaar").contains("kvazaar"));
    CHECK(HardwareEncoders::betterQualityHint("libx264").isEmpty());
    CHECK(HardwareEncoders::betterQualityHint("h264_nvenc").isEmpty());
    CHECK(HardwareEncoders::betterQualityHint({}).isEmpty());

    // No hardware, no extension: the free one (and the hint) where this FFmpeg has it
    HardwareEncoders::fakeProbeForTests({});
    CHECK_EQ(HardwareEncoders::needsBetterEncoder("h264"), !free264.isEmpty());
    CHECK(!HardwareEncoders::needsBetterEncoder("prores"));
    CHECK(!HardwareEncoders::needsBetterEncoder("av1"));
    // Hardware first
    HardwareEncoders::fakeProbeForTests({"h264_nvenc"});
    CHECK(!HardwareEncoders::needsBetterEncoder("h264"));
    HardwareEncoders::Choice c = HardwareEncoders::resolve(VideoEncoder::Auto, "h264");
    CHECK_EQ(c.codec, QString("h264_nvenc"));
    CHECK_EQ(c.fallback, free264);

#ifdef Q_OS_UNIX
    // With the extension: hardware > extension > free
    const QString fake = fakeProgram(dir);
    if (!CHECK(!fake.isEmpty())) return;
    qputenv("SCHNEIDI_ENCODER_FFMPEG", fake.toUtf8());
    CHECK_EQ(HardwareEncoders::externalProgram("libx264"), fake);
    CHECK_EQ(HardwareEncoders::externalProgram("libx265"), fake);
    CHECK(HardwareEncoders::externalProgram("libsvtav1").isEmpty());
    CHECK_EQ(HardwareEncoders::softwareCodec("h264"), QString("libx264"));
    CHECK_EQ(HardwareEncoders::softwareCodec("h265"), QString("libx265"));
    c = HardwareEncoders::resolve(VideoEncoder::Auto, "h264");
    CHECK_EQ(c.codec, QString("h264_nvenc"));
    CHECK_EQ(c.fallback, QString("libx264"));
    HardwareEncoders::fakeProbeForTests({});
    c = HardwareEncoders::resolve(VideoEncoder::Auto, "h264");
    CHECK_EQ(c.codec, QString("libx264"));
    CHECK_EQ(c.fallback, free264); // the program might fail
    CHECK(!HardwareEncoders::needsBetterEncoder("h264"));
    c = HardwareEncoders::resolve(VideoEncoder::Software, "h265");
    CHECK_EQ(c.codec, QString("libx265"));
    qunsetenv("SCHNEIDI_ENCODER_FFMPEG");
#else
    Q_UNUSED(dir);
#endif
    qunsetenv("SCHNEIDI_IGNORE_BUILTIN");
    HardwareEncoders::resetForTests();
}

#ifdef Q_OS_UNIX
// The extension's encoder list is asked by the background hardware probe (not later on the GUI thread), once
void testListInProbe(const QString& dir)
{
    const QString log = QDir(dir).filePath("asked.log");
    const QString path = QDir(dir).filePath("fake-ffmpeg-counting");
    QFile f(path);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write(QString("#!/bin/sh\n"
                    "echo \"$3\" >> '%1'\n"
                    "echo ' V....D libx264              libx264 H.264 / AVC'\n").arg(log).toUtf8());
    f.close();
    f.setPermissions(f.permissions() | QFile::ExeOwner);
    qputenv("SCHNEIDI_IGNORE_BUILTIN", "libx264,libx265");
    qputenv("SCHNEIDI_ENCODER_FFMPEG", path.toUtf8());
    HardwareEncoders::resetForTests();
    HardwareEncoders::probeInBackground();
    for (int i = 0; i < 600 && !HardwareEncoders::probed(); ++i) QThread::msleep(50);
    CHECK(HardwareEncoders::probed());
    QFile asked(log);
    CHECK(asked.open(QIODevice::ReadOnly));
    CHECK_EQ(asked.readAll().count("-encoders"), qsizetype(1));
    asked.close();
    // Cached: the GUI's later questions do not start the program again
    CHECK_EQ(HardwareEncoders::softwareCodec("h264"), QString("libx264"));
    CHECK(!HardwareEncoders::needsBetterEncoder("h264"));
    CHECK(asked.open(QIODevice::ReadOnly));
    CHECK_EQ(asked.readAll().count("-encoders"), qsizetype(1));
    qunsetenv("SCHNEIDI_ENCODER_FFMPEG");
    qunsetenv("SCHNEIDI_IGNORE_BUILTIN");
    HardwareEncoders::resetForTests();
}
#endif

#ifdef Q_OS_UNIX
// A program that hangs (reads nothing): writing waits, a cancel ends it soon, the program is stopped; one that dies
// is reported instead of blocking
void testStall(const QString& dir)
{
    const QString path = QDir(dir).filePath("fake-ffmpeg-hangs");
    QFile f(path);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write("#!/bin/sh\nexec sleep 60\n");
    f.close();
    f.setPermissions(f.permissions() | QFile::ExeOwner);
    ExternalEncoder::Params p;
    p.program = path;
    p.codec = "libx264";
    p.width = 640;
    p.height = 360;
    p.output = QDir(dir).filePath("stall.nut");
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = p.width;
    frame->height = p.height;
    if (!CHECK(av_frame_get_buffer(frame, 0) >= 0)) return;
    QElapsedTimer t;
    t.start();
    {
        ExternalEncoder enc(p);
        QString why;
        CHECK(enc.start(&why));
        std::atomic<bool> stop{false};
        std::thread canceller([&stop] {
            QThread::msleep(300);
            stop = true;
        });
        bool ok = true;
        for (int i = 0; ok && i < 20; ++i) ok = enc.write(frame, stop, &why);
        canceller.join();
        CHECK(!ok);
        CHECK(t.elapsed() < 3000);
    } // stops the program
    CHECK(t.elapsed() < 6000);
    // Dies at once: the next writes fail with a reason
    f.open(QIODevice::WriteOnly);
    f.write("#!/bin/sh\necho 'no such option' >&2\nexit 1\n");
    f.close();
    {
        ExternalEncoder enc(p);
        QString why;
        CHECK(enc.start(&why));
        const std::atomic<bool> stop{false};
        bool ok = true;
        for (int i = 0; ok && i < 20; ++i) ok = enc.write(frame, stop, &why);
        if (ok) ok = enc.finish(stop, &why);
        CHECK(!ok);
        CHECK(why.contains("no such option"));
    }
    av_frame_free(&frame);
}
#endif

void testArguments()
{
    ExternalEncoder::Params p;
    p.program = "ffmpeg";
    p.codec = "libx264";
    p.width = 1920;
    p.height = 1080;
    p.rate = {30000, 1001};
    p.matrix = AVCOL_SPC_BT709;
    p.transfer = AVCOL_TRC_BT709;
    p.primaries = AVCOL_PRI_BT709;
    p.range = AVCOL_RANGE_MPEG;
    p.options = {{"crf", "22"}, {"preset", "medium"}, {"vprofile", "high"}, {"maxrate", "12000k"}};
    p.threads = 4;
    p.output = "/x/.out.mp4.schneidi-video.nut";
    const QStringList a = ExternalEncoder::arguments(p);
    const auto has = [&a](const QString& key, const QString& value) {
        const qsizetype i = a.indexOf(key);
        return i >= 0 && a.value(i + 1) == value;
    };
    CHECK(has("-f", "rawvideo"));
    CHECK(has("-i", "pipe:0"));
    CHECK(has("-video_size", "1920x1080"));
    CHECK(has("-framerate", "30000/1001"));
    CHECK(has("-pix_fmt", "yuv420p"));
    CHECK(has("-c:v", "libx264"));
    CHECK(has("-crf:v", "22"));
    CHECK(has("-preset:v", "medium"));
    CHECK(has("-profile:v", "high"));
    CHECK(has("-maxrate:v", "12000k"));
    CHECK(has("-threads:v", "4"));
    CHECK(has("-colorspace:v", "bt709"));
    CHECK(has("-color_range:v", "tv"));
    // The raw input carries the same tags (else ffmpeg converts the pictures to the output's colors)
    const QStringList input = a.mid(0, a.indexOf("-i"));
    const auto inputHas = [&input](const QString& key, const QString& value) {
        const qsizetype i = input.indexOf(key);
        return i >= 0 && input.value(i + 1) == value;
    };
    CHECK(inputHas("-colorspace", "bt709"));
    CHECK(inputHas("-color_trc", "bt709"));
    CHECK(inputHas("-color_primaries", "bt709"));
    CHECK(inputHas("-color_range", "tv"));
    CHECK(a.contains("-an"));
    CHECK_EQ(a.mid(a.size() - 3), (QStringList{"-f", "nut", p.output}));
    CHECK_EQ(ExternalEncoder::intermediatePath(QDir("/videos").filePath("a.mp4")),
             QDir("/videos").filePath(".a.mp4.schneidi-video.nut"));
    p.codec = "libx265";
    CHECK(ExternalEncoder::arguments(p).contains("log-level=error"));
}

// ffmpeg with libx264 to stand in for the extension (SCHNEIDI_TEST_ENCODER_FFMPEG: the real one); empty = none
QString standIn()
{
    const QString ffmpeg = qEnvironmentVariable("SCHNEIDI_TEST_ENCODER_FFMPEG", QStandardPaths::findExecutable("ffmpeg"));
    if (ffmpeg.isEmpty()) return {};
    QProcess p;
    p.start(ffmpeg, {"-hide_banner", "-encoders"});
    p.waitForFinished(20000);
    return p.readAllStandardOutput().contains("libx264") ? ffmpeg : QString();
}

// The program keeps the pictures as they come (lossless): no conversion to other colors on the way, for BT.709
// limited range and BT.601 full range
void testColors(const QString& dir)
{
    const QString ffmpeg = standIn();
    if (ffmpeg.isEmpty()) {
        std::printf("  colors skipped: no ffmpeg with libx264\n");
        return;
    }
    struct Case {
        AVColorSpace matrix;
        AVColorTransferCharacteristic transfer;
        AVColorPrimaries primaries;
        AVColorRange range;
    };
    for (const Case& k : {Case{AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_PRI_BT709, AVCOL_RANGE_MPEG},
                          Case{AVCOL_SPC_SMPTE170M, AVCOL_TRC_SMPTE170M, AVCOL_PRI_SMPTE170M, AVCOL_RANGE_JPEG}}) {
        ExternalEncoder::Params p;
        p.program = ffmpeg;
        p.codec = "libx264";
        p.width = 64;
        p.height = 64;
        p.matrix = k.matrix;
        p.transfer = k.transfer;
        p.primaries = k.primaries;
        p.range = k.range;
        p.options = {{"qp", "0"}, {"preset", "ultrafast"}};
        p.output = QDir(dir).filePath("colors.nut");
        AVFrame* frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = p.width;
        frame->height = p.height;
        if (!CHECK(av_frame_get_buffer(frame, 0) >= 0)) return;
        for (int y = 0; y < p.height; ++y)
            for (int x = 0; x < p.width; ++x) frame->data[0][y * frame->linesize[0] + x] = uint8_t((y * p.width + x) % 256);
        for (int plane : {1, 2})
            for (int y = 0; y < p.height / 2; ++y)
                for (int x = 0; x < p.width / 2; ++x) frame->data[plane][y * frame->linesize[plane] + x] = uint8_t(64 + x * 4);
        ExternalEncoder enc(p);
        const std::atomic<bool> stop{false};
        QString why;
        bool ok = enc.start(&why);
        for (int i = 0; ok && i < 3; ++i) ok = enc.write(frame, stop, &why);
        if (ok) ok = enc.finish(stop, &why);
        if (!CHECK(ok)) std::printf("  %s\n", qPrintable(why));
        // The first picture back, as it is in the stream
        QProcess dec;
        // In the stream's own pixel format (8-bit 4:2:0 either way): FFmpeg before 7.1 takes "-pix_fmt yuv420p" for
        // limited range and squeezes a full-range (yuvj420p) picture into it
        dec.start(ffmpeg, {"-v", "error", "-i", p.output, "-frames:v", "1", "-f", "rawvideo", "-"});
        dec.waitForFinished(20000);
        const QByteArray back = dec.readAllStandardOutput();
        if (CHECK_EQ(back.size(), qsizetype(p.width * p.height * 3 / 2))) {
            int differ = 0;
            for (int i = 0; i < p.width * p.height; ++i)
                if (uint8_t(back[i]) != frame->data[0][(i / p.width) * frame->linesize[0] + i % p.width]) ++differ;
            for (int i = 0; i < p.width * p.height / 4; ++i)
                if (uint8_t(back[p.width * p.height + i]) != uint8_t(64 + (i % (p.width / 2)) * 4)) ++differ;
            if (!CHECK_EQ(differ, 0))
                std::printf("  %s: Y 10/16/235/250 came back as %d/%d/%d/%d\n", av_color_space_name(k.matrix),
                            uint8_t(back[10]), uint8_t(back[16]), uint8_t(back[235]), uint8_t(back[250]));
        }
        av_frame_free(&frame);
        QFile::remove(p.output);
    }
}

struct Rendered {
    bool ok = false;
    QString codec, note, message;
};

Rendered render(const Timeline& tl, const ProjectFormat& fmt, const QString& format, const QString& path)
{
    RenderJob job;
    job.settings.format = format;
    job.settings.encoder = VideoEncoder::Auto;
    job.path = path;
    const ExportSettings s = RenderQueue::exportSettings(job, fmt);
    Exporter ex;
    QEventLoop loop;
    Rendered r;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& message) {
        r.ok = success;
        r.message = message;
        loop.quit();
    });
    QString error;
    if (!ex.start(tl, s, &error)) {
        r.message = error;
        return r;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    r.codec = ex.usedCodec();
    r.note = ex.note();
    return r;
}

QString probe(const QString& path, const QString& entries)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-count_frames", "-show_entries", entries, "-of", "csv=p=0", path});
    p.waitForFinished(30000);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// A real export with libx264 in a separate program: the distribution's ffmpeg stands in for the extension
void testExport(const QString& dir)
{
    // SCHNEIDI_TEST_ENCODER_FFMPEG: the real extension's program (e.g. after `extensions --install x264`)
    const QString ffmpeg = qEnvironmentVariable("SCHNEIDI_TEST_ENCODER_FFMPEG", QStandardPaths::findExecutable("ffmpeg"));
    QProcess p;
    p.start(ffmpeg, {"-hide_banner", "-encoders"});
    p.waitForFinished(20000);
    if (!p.readAllStandardOutput().contains("libx264")) {
        std::printf("  skipped: no ffmpeg with libx264 to stand in for the extension\n");
        return;
    }
    const QString clip = Check::makeMedia(QDir(dir).filePath("clip.mkv"),
                                          {"-f", "lavfi", "-i", "testsrc2=size=320x176:rate=25:duration=2", "-f", "lavfi",
                                           "-i", "sine=frequency=440:duration=2", "-c:v", "ffv1", "-c:a", "flac"});
    if (!CHECK(!clip.isEmpty())) return;
    const ProjectFormat fmt{320, 176, {25, 1}};
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = clip;
    c.out = 49;
    c.linkId = 1;
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;

    qputenv("SCHNEIDI_IGNORE_BUILTIN", "libx264,libx265");
    qputenv("SCHNEIDI_ENCODER_FFMPEG", ffmpeg.toUtf8());
    HardwareEncoders::fakeProbeForTests({}); // no graphics card
    for (const char* ext : {"mp4", "mkv"}) {
        const QString out = QDir(dir).filePath(QString("x264.%1").arg(ext));
        const Rendered r = render(tl, fmt, "h264", out);
        CHECK(r.ok);
        CHECK_EQ(r.codec, QString("libx264"));
        CHECK(r.note.isEmpty());
        CHECK_EQ(probe(out, "stream=codec_name,nb_read_frames").split('\n').value(0), QString("h264,50"));
        CHECK(probe(out, "stream=codec_type").contains("audio"));
        CHECK(probe(out, "stream=color_space").contains("bt709"));
        // Picture and sound start together (the program's muxer delays B-frame streams; the writer moves it back)
        const QStringList starts = probe(out, "stream=start_time").split('\n');
        CHECK(starts.size() == 2 && std::abs(starts[0].toDouble() - starts[1].toDouble()) < 0.03);
        std::printf("  %s start times: %s\n", ext, qPrintable(starts.join(" / ")));
        // The program's intermediate file is gone
        CHECK(QDir(dir).entryList({"*.schneidi-video.nut"}, QDir::Files | QDir::Hidden).isEmpty());
        std::printf("  %s: %lld bytes\n", ext, QFileInfo(out).size());
    }

#ifdef Q_OS_UNIX
    // The program breaks: the free encoder takes over (if this FFmpeg has one), with a note
    const QString fake = fakeProgram(dir);
    qputenv("SCHNEIDI_ENCODER_FFMPEG", fake.toUtf8());
    if (!HardwareEncoders::freeReplacement("libx264").isEmpty()) {
        const QString out = QDir(dir).filePath("fallback.mp4");
        const Rendered r = render(tl, fmt, "h264", out);
        CHECK(r.ok);
        CHECK_EQ(r.codec, HardwareEncoders::freeReplacement("libx264"));
        CHECK(r.note.contains("x264/x265"));
        CHECK(probe(out, "stream=codec_name").startsWith("h264"));
    } else {
        std::printf("  fallback skipped: no free H.264 encoder in this FFmpeg\n");
    }
#endif
    std::fflush(stdout);
    qunsetenv("SCHNEIDI_ENCODER_FFMPEG");
    qunsetenv("SCHNEIDI_IGNORE_BUILTIN");
    HardwareEncoders::resetForTests();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("encoder_extension");
    Check::initEngine();
    QDir(Extensions::rootDir()).removeRecursively();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testCatalog();
    testInstallPaths();
    testOrder(tmp.path());
#ifdef Q_OS_UNIX
    testListInProbe(tmp.path());
    testStall(tmp.path());
#endif
    testArguments();
    if (Check::haveFfmpeg()) testColors(tmp.path());
    if (Check::haveFfmpeg()) testExport(tmp.path());
    QDir(Extensions::rootDir()).removeRecursively();
    return Check::result();
}
