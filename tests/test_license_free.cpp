// SPDX-License-Identifier: AGPL-3.0-only
// No GPL in the bundled packages (0.14): Licenses::report() finds FFmpeg's license and the GPL libraries in the
// process. A SCHNEIDI_NO_GPL build (or
// SCHNEIDI_NO_GPL=1 in the environment, for checking an installed bundle) fails on any GPL part.

#include "check.h"

#include "core/RenderJob.h"
#include "engine/Exporter.h"
#include "engine/HardwareEncoders.h"
#include "engine/Licenses.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstring>
#include <memory>
#include <algorithm>
#include <vector>

namespace {

void testNames()
{
    CHECK(!Licenses::knownGplLicense("libx264.so.164").isEmpty());
    CHECK(!Licenses::knownGplLicense("x265-215.dll").isEmpty());
    CHECK(!Licenses::knownGplLicense("libpostproc.so.58").isEmpty());
    CHECK(Licenses::knownGplLicense("librubberband.so.2").contains("GPL"));
    CHECK(Licenses::knownGplLicense("libopenh264.so.8").isEmpty());
    CHECK(Licenses::knownGplLicense("libavcodec.so.61.19.101").isEmpty());
    CHECK(Licenses::knownGplLicense("avcodec-61.dll").isEmpty());
}

// Every render format has an encoder, with x264/x265 (Debian's FFmpeg) or without them (the LGPL build: OpenH264,
// kvazaar); codecs without crf get their own rate control
void testEncoders()
{
    for (const RenderFormatInfo& f : renderFormats()) {
        Check::report(HardwareEncoders::formatAvailable(f.id), "format without encoder", __FILE__, __LINE__, f.id);
        if (*f.videoCodec)
            Check::report(!HardwareEncoders::softwareCodec(f.id).isEmpty(), "no software encoder", __FILE__, __LINE__, f.id);
    }
    CHECK(!HardwareEncoders::formatAvailable("no-such-format"));
    const QString h264 = HardwareEncoders::softwareCodec("h264");
    CHECK(h264 == "libx264" || h264 == "libopenh264");
    const QString h265 = HardwareEncoders::softwareCodec("h265");
    CHECK(h265 == "libx265" || h265 == "libkvazaar");
    CHECK_EQ(HardwareEncoders::codecName(VideoEncoder::Software, "h264"), h264);
    CHECK_EQ(HardwareEncoders::resolve(VideoEncoder::Software, "h264").codec, h264);
    CHECK(HardwareEncoders::softwareOptions("libx264", 22, QSize(1920, 1080), 30).isEmpty());
    const auto open = HardwareEncoders::softwareOptions("libopenh264", 22, QSize(1920, 1080), 30);
    CHECK(open.contains(qMakePair(QString("b"), QString("6220k")))); // 0.1 bit per pixel at CRF 22
    const auto small = HardwareEncoders::softwareOptions("libopenh264", 28, QSize(1920, 1080), 30);
    CHECK(small.contains(qMakePair(QString("b"), QString("3110k")))); // half the rate 6 steps up
    const auto kvazaar = HardwareEncoders::softwareOptions("libkvazaar", 22, QSize(1920, 1080), 30);
    CHECK(kvazaar.contains(qMakePair(QString("kvazaar-params"), QString("preset=fast,qp=24"))));
    CHECK(HardwareEncoders::proxyVideoArguments().contains(h264));
}

// Export through the render queue's settings (encoder chosen like the Deliver page, software): H.264 and H.265 with
// whatever this FFmpeg has (x264/x265 or OpenH264/kvazaar), Opus and AAC sound. The file must decode with sound.
void testExport(const QString& dir)
{
    const QString clip = Check::makeMedia(QDir(dir).filePath("clip.mkv"),
                                          {"-f", "lavfi", "-i", "testsrc2=size=320x176:rate=25:duration=2", "-f", "lavfi",
                                           "-i", "sine=frequency=440:duration=2", "-c:v", "ffv1", "-c:a", "flac"});
    if (!CHECK(!clip.isEmpty())) return;
    const ProjectFormat fmt{320, 176, {25, 1}}; // kvazaar takes only sizes in multiples of 8
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
    for (const char* format : {"h264", "h265", "opus", "aac"}) {
        RenderJob job;
        job.settings.format = format;
        job.settings.encoder = VideoEncoder::Software;
        job.settings.smartRender = false;
        job.path = QDir(dir).filePath(QString("out-%1.%2").arg(format, renderFormat(format).extension));
        ExportSettings s = RenderQueue::exportSettings(job, fmt);
        Exporter ex;
        QEventLoop loop;
        bool ok = false;
        QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
            ok = success;
            loop.quit();
        });
        QString error;
        if (!Check::report(ex.start(tl, s, &error), "export start", __FILE__, __LINE__, QString(format) + ": " + error))
            continue;
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
        Check::report(ok, "export", __FILE__, __LINE__, QString("%1 with %2").arg(format, s.videoCodec));
        std::printf("  %s: %s + %s, %lld bytes\n", format, qPrintable(s.videoCodec.isEmpty() ? "-" : s.videoCodec),
                    qPrintable(s.audioCodec), QFileInfo(job.path).size());
        // Decodes completely, with sound
        QProcess p;
        p.start("ffprobe", {"-v", "error", "-show_entries", "stream=codec_type", "-of", "csv=p=0", job.path});
        p.waitForFinished(30000);
        const QString streams = QString::fromUtf8(p.readAllStandardOutput());
        Check::report(streams.contains("audio"), "no sound", __FILE__, __LINE__, QString(format) + ": " + streams);
        if (*renderFormat(format).videoCodec)
            Check::report(streams.contains("video"), "no picture", __FILE__, __LINE__, QString(format) + ": " + streams);
    }
    std::fflush(stdout);
}

void testReport()
{
    const QVector<Licenses::Part> parts = Licenses::report();
    bool avcodec = false;
    for (const Licenses::Part& p : parts) {
        std::printf("  %-40s %-30s %s\n", qPrintable(p.name), qPrintable(p.license), p.gpl ? "GPL" : "");
        if (p.name.startsWith("libavcodec ")) avcodec = !p.license.isEmpty();
    }
    std::fflush(stdout);
    CHECK(avcodec);
    if (Licenses::builtWithoutGpl() || qEnvironmentVariableIntValue("SCHNEIDI_NO_GPL") > 0)
        for (const Licenses::Part& p : Licenses::gplParts())
            Check::report(false, "GPL part in a SCHNEIDI_NO_GPL build", __FILE__, __LINE__, p.name + " " + p.license);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("license_free");
    Check::initEngine();
    testNames();
    testEncoders();
    QTemporaryDir tmp;
    if (Check::haveFfmpeg() && CHECK(tmp.isValid())) testExport(tmp.path());
    testReport();
    return Check::result();
}
