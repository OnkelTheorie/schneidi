// Hardware video encoders (Deliver): encoder choice in the render settings JSON (old files = software), choosing the
// encoder (Auto, not available -> software), a real render with every hardware encoder that works on this computer
// (skipped when there is none) and the fallback to software when a hardware encoder does not work.

#include "check.h"

#include "core/RenderJob.h"
#include "engine/Exporter.h"
#include "engine/HardwareEncoders.h"
#include "engine/RenderQueue.h"
#include "core/Project.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"

#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
constexpr int kFrames = 50; // 2 s
QString g_media;

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
    return tl;
}

struct Result {
    bool ok = false;
    QString codec, note;
};

Result render(const ExportSettings& s)
{
    Exporter ex;
    QEventLoop loop;
    Result r;
    QString message;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& msg) {
        r.ok = success;
        message = msg;
        loop.quit();
    });
    QString error;
    if (!CHECK(ex.start(timeline(), s, &error))) {
        std::printf("       %s\n", qPrintable(error));
        return r;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!r.ok) std::printf("       %s: %s\n", qPrintable(s.path), qPrintable(message.isEmpty() ? "timeout" : message));
    r.codec = ex.usedCodec();
    r.note = ex.note();
    return r;
}

QJsonObject videoStream(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_streams", "-of", "json", path});
    p.waitForFinished(30000);
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("streams").toArray().first().toObject();
}

RenderJob job(const QString& path, const QString& format, VideoEncoder encoder, int quality = 0)
{
    RenderJob j;
    j.id = 1;
    j.path = path;
    j.settings.format = format;
    j.settings.encoder = encoder;
    j.settings.quality = quality;
    return j;
}

void testJson()
{
    // New settings: Auto; saved with the choice
    RenderSettings s;
    CHECK(s.encoder == VideoEncoder::Auto);
    CHECK_EQ(s.toJson().value("encoder").toString(), QString("auto"));
    // Old files/presets without "encoder": software, so nothing changes for them
    CHECK(RenderSettings::fromJson(QJsonObject{{"format", "h264"}, {"quality", 1}}).encoder == VideoEncoder::Software);
    CHECK(RenderSettings::fromJson(QJsonObject{{"format", "h264"}, {"encoder", "bogus"}}).encoder == VideoEncoder::Software);
    for (VideoEncoder e : {VideoEncoder::Auto, VideoEncoder::Software, VideoEncoder::Nvenc, VideoEncoder::Vaapi, VideoEncoder::Qsv}) {
        s.encoder = e;
        const RenderSettings back = RenderSettings::fromJson(s.toJson());
        CHECK(back == s);
        CHECK(back.encoder == e);
        bool ok = false;
        CHECK(videoEncoderFromId(videoEncoderId(e), VideoEncoder::Software, &ok) == e && ok);
    }
    RenderSettings a, b;
    b.encoder = VideoEncoder::Software;
    CHECK(a != b); // the encoder counts for presets/undo
    bool ok = true;
    CHECK(videoEncoderFromId("NVENC", VideoEncoder::Auto, &ok) == VideoEncoder::Nvenc && ok);
    videoEncoderFromId("x", VideoEncoder::Auto, &ok);
    CHECK(!ok);

    // Queue: the encoder that rendered the job is kept
    RenderJob j = job("/x/a.mp4", "h265", VideoEncoder::Nvenc);
    j.status = RenderStatus::Done;
    j.encoderUsed = "hevc_nvenc";
    j.message = "note";
    const QVector<RenderJob> back = RenderQueueJson::fromJson(RenderQueueJson::toJson({j}));
    CHECK(back.size() == 1 && back[0] == j);
    CHECK_EQ(back.value(0).encoderUsed, QString("hevc_nvenc"));
    CHECK(back.value(0).settings.encoder == VideoEncoder::Nvenc);

    // Built-in presets: video ones pick the encoder automatically
    for (const RenderPreset& p : RenderPresets::builtins())
        if (p.settings.hasEncoderChoice()) CHECK(p.settings.encoder == VideoEncoder::Auto);
    // Summary names an explicit hardware encoder
    RenderSettings nv;
    nv.encoder = VideoEncoder::Nvenc;
    CHECK(nv.summary(QSize(1920, 1080)).contains("NVENC"));
    RenderSettings wav;
    wav.format = "wav";
    CHECK(!wav.hasEncoderChoice());
}

void testChoice()
{
    using namespace HardwareEncoders;
    CHECK_EQ(codecName(VideoEncoder::Nvenc, "h264"), QString("h264_nvenc"));
    CHECK_EQ(codecName(VideoEncoder::Vaapi, "h265"), QString("hevc_vaapi"));
    CHECK_EQ(codecName(VideoEncoder::Qsv, "h265"), QString("hevc_qsv"));
    CHECK_EQ(codecName(VideoEncoder::Software, "h265"), QString("libx265"));
    CHECK_EQ(codecName(VideoEncoder::Nvenc, "prores"), QString("prores_ks")); // no hardware encoder there
    CHECK(isHardwareCodec("hevc_qsv") && !isHardwareCodec("libx264") && !isHardwareCodec(""));
    // Quality levels map to different values of the encoder's own scale
    CHECK(codecOptions("h264_nvenc", 18) != codecOptions("h264_nvenc", 28));
    CHECK(codecOptions("libx264", 18).isEmpty());
#ifndef Q_OS_LINUX
    CHECK(vaapiDevice().isEmpty()); // VAAPI only on Linux
#endif

    // Software: no probe, no fallback
    Choice c = resolve(VideoEncoder::Software, "h264");
    CHECK(c.codec == "libx264" && c.fallback.isEmpty() && c.note.isEmpty());
    c = resolve(VideoEncoder::Auto, "wav");
    CHECK(c.codec.isEmpty() && c.fallback.isEmpty());
    // Auto: first working hardware encoder, else software (no note)
    for (const char* format : {"h264", "h265"}) {
        const QVector<VideoEncoder> list = available(format);
        c = resolve(VideoEncoder::Auto, format);
        CHECK(c.note.isEmpty());
        if (list.isEmpty()) {
            CHECK(c.codec == renderFormat(format).videoCodec && c.fallback.isEmpty());
        } else {
            CHECK_EQ(c.codec, codecName(list.first(), format));
            CHECK_EQ(c.fallback, QString(renderFormat(format).videoCodec));
        }
        // A hardware encoder that does not work here: software with a note
        for (VideoEncoder e : {VideoEncoder::Nvenc, VideoEncoder::Vaapi, VideoEncoder::Qsv}) {
            c = resolve(e, format);
            if (list.contains(e)) CHECK(c.codec == codecName(e, format) && c.note.isEmpty());
            else CHECK(c.codec == renderFormat(format).videoCodec && !c.note.isEmpty());
        }
    }
    CHECK(probed());
    std::printf("Hardware encoders here: H.264 %d, H.265 %d\n", int(available("h264").size()), int(available("h265").size()));
}

void testRender(const QString& dir)
{
    int rendered = 0;
    for (const char* format : {"h264", "h265"}) {
        const QString expected = QLatin1String(format) == QLatin1String("h264") ? "h264" : "hevc";
        for (VideoEncoder e : HardwareEncoders::available(format)) {
            const QString codec = HardwareEncoders::codecName(e, format);
            // High and small quality: both play, small is smaller
            qint64 sizes[2] = {0, 0};
            for (int q : {0, 2}) {
                const QString path = QString("%1/%2-q%3.mp4").arg(dir, codec).arg(q);
                const ExportSettings s = RenderQueue::exportSettings(job(path, format, e, q), kFmt);
                CHECK_EQ(s.videoCodec, codec);
                const Result r = render(s);
                if (!CHECK(r.ok)) continue;
                CHECK_EQ(r.codec, codec);
                if (!CHECK(r.note.isEmpty())) std::printf("       %s\n", qPrintable(r.note));
                const QJsonObject v = videoStream(path);
                CHECK_EQ(v.value("codec_name").toString(), expected);
                CHECK_EQ(v.value("pix_fmt").toString(), QString("yuv420p"));
                CHECK(v.value("width").toInt() == 640 && v.value("height").toInt() == 360);
                CHECK_EQ(v.value("nb_frames").toString().toInt(), kFrames);
                sizes[q / 2] = QFileInfo(path).size();
                ++rendered;
            }
            if (sizes[0] && sizes[1] && !CHECK(sizes[1] < sizes[0]))
                std::printf("       %s: high %lld, small %lld bytes\n", qPrintable(codec), sizes[0], sizes[1]);
        }
    }
    if (!rendered) std::printf("übersprungen: Rendern mit Hardware-Encoder (keiner funktioniert hier)\n");
    else std::printf("Rendered %d files with hardware encoders\n", rendered);
}

void testFallback(const QString& dir)
{
    // A hardware encoder that does not work here: the test encode fails -> software, with a note (MLT itself would
    // crash on an encoder it cannot open, so this must never reach MLT)
    QString broken;
    for (VideoEncoder e : {VideoEncoder::Qsv, VideoEncoder::Vaapi, VideoEncoder::Nvenc})
        if (!HardwareEncoders::available("h264").contains(e)) broken = HardwareEncoders::codecName(e, "h264");
    if (broken.isEmpty()) {
        std::printf("übersprungen: Rückfall auf Software (alle Hardware-Encoder funktionieren hier)\n");
        return;
    }
    ExportSettings s = RenderQueue::exportSettings(job(dir + "/fallback.mp4", "h264", VideoEncoder::Software), kFmt);
    s.softwareCodec = s.videoCodec;
    s.videoCodec = broken;
    for (int round = 0; round < 2; ++round) { // second time: from the session cache, no test encode
        const Result r = render(s);
        CHECK(r.ok);
        CHECK_EQ(r.codec, QString("libx264"));
        CHECK(!r.note.isEmpty());
        CHECK_EQ(videoStream(s.path).value("codec_name").toString(), QString("h264"));
        CHECK(HardwareEncoders::cachedResult(broken, QSize(640, 360)) < 0);
    }
    // Without a software codec to fall back to: fails cleanly
    s.softwareCodec.clear();
    s.path = dir + "/nofallback.mp4";
    Exporter ex;
    QEventLoop loop;
    bool ok = true;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    QString error;
    if (ex.start(timeline(), s, &error)) {
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        loop.exec();
    } else {
        ok = false;
        CHECK(!error.isEmpty());
    }
    CHECK(!ok);
    CHECK(!QFileInfo::exists(s.path));

    // Job asks for a hardware encoder that is not here: software, and the job keeps a note
    const ExportSettings viaJob = RenderQueue::exportSettings(
        job(dir + "/job.mp4", "h264", HardwareEncoders::encoderOfCodec(broken)), kFmt);
    CHECK_EQ(viaJob.videoCodec, QString("libx264"));
    CHECK(!viaJob.encoderNote.isEmpty());
    const Result r = render(viaJob);
    CHECK(r.ok && r.codec == "libx264" && !r.note.isEmpty());
}

void testCancelDuringCheck(const QString& dir)
{
    // Cancel while the test encode of a hardware encoder runs (new size -> not cached)
    const QVector<VideoEncoder> list = HardwareEncoders::available("h264");
    if (list.isEmpty()) return;
    ExportSettings s = RenderQueue::exportSettings(job(dir + "/cancel.mp4", "h264", list.first()), kFmt);
    s.size = QSize(320, 180);
    if (HardwareEncoders::cachedResult(s.videoCodec, s.size) != 0) return;
    Exporter ex;
    bool finished = false, ok = true;
    QObject::connect(&ex, &Exporter::finished, [&](bool success, const QString&) {
        finished = true;
        ok = success;
    });
    QString error;
    CHECK(ex.start(timeline(), s, &error));
    CHECK(ex.isRunning());
    ex.cancel();
    CHECK(finished && !ok);
    CHECK(!ex.isRunning());
    CHECK(!Exporter::anyRunning());
    CHECK(!QFileInfo::exists(s.path));
}

void testPanel()
{
    // Deliver page: Auto, Software and the hardware encoders that work here (probe already done)
    Project p;
    DeliverPanel panel(&p);
    panel.show(); // offscreen
    auto* encoder = panel.findChild<QComboBox*>("renderEncoder");
    if (!CHECK(encoder)) return;
    const QVector<VideoEncoder> list = HardwareEncoders::available("h264");
    CHECK_EQ(encoder->count(), 2 + int(list.size()));
    for (int i = 0; i < list.size(); ++i) CHECK(VideoEncoder(encoder->itemData(2 + i).toInt()) == list[i]);
    CHECK(VideoEncoder(encoder->currentData().toInt()) == VideoEncoder::Auto);
    if (const QByteArray dump = qgetenv("HWENC_DUMP"); !dump.isEmpty()) {
        std::printf("%s\n", qPrintable(encoder->itemText(0)));
        panel.grab().save(QString::fromLocal8Bit(dump));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("hw_encoders");
    testJson();
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testChoice();
    testPanel();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=2", "-f", "lavfi", "-i",
                                "sine=frequency=440:duration=2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                "-shortest"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    testRender(tmp.path());
    testFallback(tmp.path());
    testCancelDuringCheck(tmp.path());
    return Check::result();
}
