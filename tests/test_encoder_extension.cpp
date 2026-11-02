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
#include <QTimer>

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
    CHECK(a.contains("-an"));
    CHECK_EQ(a.mid(a.size() - 3), (QStringList{"-f", "nut", p.output}));
    CHECK_EQ(ExternalEncoder::intermediatePath(QDir("/videos").filePath("a.mp4")),
             QDir("/videos").filePath(".a.mp4.schneidi-video.nut"));
    p.codec = "libx265";
    CHECK(ExternalEncoder::arguments(p).contains("log-level=error"));
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
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
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
    testArguments();
    if (Check::haveFfmpeg()) testExport(tmp.path());
    QDir(Extensions::rootDir()).removeRecursively();
    return Check::result();
}
