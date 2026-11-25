// SPDX-License-Identifier: AGPL-3.0-only
// Render to a file size (`render` max_mb, Deliver's "Max. Dateigröße"; core/TargetSize): the plan (video rate from the
// length, the sound and the container, the sound at 128/96/64 kbit/s, the picture stepped down 1080 -> 720 -> 540 ->
// 360 below the bits-per-pixel floor, "poor" below it at the smallest step, too small = an error naming the MB a 360p
// needs, the lower rate of a retry), the settings' JSON and summary, the exporter's settings (rate target, no smart
// render), real renders of a synthetic 10 s clip with the software encoders (h264, h265, vp9, av1) that land under
// their limits and play (ffprobe/ffmpeg decode the whole file), the refusals (formats without a bit rate, quality,
// review, too small), the help text, the Deliver page's field and one render through the queue.
#include "check.h"

#include "cli/Commands.h"
#include "core/Project.h"
#include "core/RenderJob.h"
#include "core/TargetSize.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/RenderQueue.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

namespace {

TargetSize::Input input(double maxMb, double seconds, QSize size = {1920, 1080})
{
    TargetSize::Input in;
    in.maxMb = maxMb;
    in.seconds = seconds;
    in.fps = 30;
    in.size = size;
    in.sound = true;
    in.audioK = 320; // the settings' default
    in.extension = "mp4";
    in.videoCodec = "libx264";
    return in;
}

void testPlan()
{
    // Formats
    for (const char* id : {"h264", "h265", "av1", "vp9", "s264", "h265-10", "av1-10"})
        CHECK(TargetSize::refusal(renderFormat(id)).isEmpty());
    for (const char* id : {"prores", "dnxhr-hq", "gif", "png", "jpeg", "aac", "wav"})
        CHECK(!TargetSize::refusal(renderFormat(id)).isEmpty());
    CHECK(TargetSize::refusal(renderFormat("prores")).contains("fixed quality"));
    CHECK(TargetSize::refusal(renderFormat("gif")).contains("GIF"));
    CHECK(TargetSize::refusal(renderFormat("aac")).contains("audio only"));
    CHECK_EQ(TargetSize::floorBitsPerPixel("libx264"), 0.05);
    CHECK_EQ(TargetSize::floorBitsPerPixel("h264_nvenc"), 0.05);
    CHECK_EQ(TargetSize::floorBitsPerPixel("libx265"), 0.035);
    CHECK_EQ(TargetSize::floorBitsPerPixel("libsvtav1"), 0.035);

    // Discord, 30 s of 1080p30: sound 128 kbit/s, ~2.2 Mbit/s video is below H.264's floor at 1080p -> 720p
    TargetSize::Plan p = TargetSize::plan(input(9, 30));
    CHECK(p.ok);
    CHECK_EQ(p.audioK, 128);
    CHECK_EQ(p.limitBytes, qint64(9000000));
    const double total = (p.videoK + p.audioK) * 1000.0 / 8 * 30 + p.overheadBytes;
    std::printf("       9 MB / 30 s: video %d kbit/s, %dx%d, %.3f bpp, planned %.0f bytes\n", p.videoK, p.size.width(),
                p.size.height(), p.bitsPerPixel, total);
    CHECK(total <= 9000000 * TargetSize::kAim + 1000); // aims ~3 % under the limit
    CHECK(total >= 9000000 * 0.95);
    CHECK(p.scaled);
    CHECK(p.size == QSize(1280, 720));
    CHECK(!p.poor);
    // Not allowed to scale: stays 1080p, poor
    TargetSize::Input fixed = input(9, 30);
    fixed.scale = false;
    p = TargetSize::plan(fixed);
    CHECK(p.ok && p.size == QSize(1920, 1080) && !p.scaled && p.poor);
    // H.265 needs fewer bits: 1080p stays
    TargetSize::Input hevc = input(9, 30);
    hevc.videoCodec = "libx265";
    p = TargetSize::plan(hevc);
    std::printf("       h265: %.3f bpp at %dx%d\n", p.bitsPerPixel, p.size.width(), p.size.height());
    CHECK(p.ok && p.size == QSize(1920, 1080));
    // Plenty of room: nothing scaled
    p = TargetSize::plan(input(25, 10));
    CHECK(p.ok && !p.scaled && p.size == QSize(1920, 1080) && p.audioK == 128);
    // Portrait: the shorter edge steps (1080x1920 -> 720x1280)
    p = TargetSize::plan(input(9, 30, {1080, 1920}));
    CHECK(p.ok && p.size == QSize(720, 1280));
    // Sound named: kept
    TargetSize::Input loud = input(9, 30);
    loud.audioFixed = true;
    p = TargetSize::plan(loud);
    CHECK_EQ(p.audioK, 320);
    // Sound asked lower than 128: kept lower
    TargetSize::Input quiet = input(9, 30);
    quiet.audioK = 96;
    CHECK_EQ(TargetSize::plan(quiet).audioK, 96);
    // Long video in a small file: the sound goes down to 64, the picture to 360p, poor
    p = TargetSize::plan(input(10, 300));
    std::printf("       10 MB / 300 s: video %d kbit/s, audio %d, %dx%d, %.3f bpp\n", p.videoK, p.audioK, p.size.width(),
                p.size.height(), p.bitsPerPixel);
    CHECK(p.ok && p.audioK == 64 && p.size == QSize(640, 360) && p.poor);
    // No sound: all for the picture
    TargetSize::Input mute = input(9, 30);
    mute.sound = false;
    CHECK_EQ(TargetSize::plan(mute).audioK, 0);
    CHECK(TargetSize::plan(mute).videoK > TargetSize::plan(input(9, 30)).videoK);
    // Too small: an error with the MB a 360p would need
    p = TargetSize::plan(input(1, 600));
    CHECK(!p.ok);
    std::printf("       %s\n", qPrintable(p.error));
    CHECK(p.error.contains("too small") && p.error.contains("360p") && p.error.contains("MB"));
    CHECK(!TargetSize::plan(input(9, 0)).ok);
    // Retry: lower, and lower the more it was over
    p = TargetSize::plan(input(9, 30));
    const int a = TargetSize::retryVideoK(p, 9300000), b = TargetSize::retryVideoK(p, 12000000);
    std::printf("       retry: %d -> %d (9.3 MB), %d (12 MB)\n", p.videoK, a, b);
    CHECK(a < p.videoK && b < a && b > 0);
    // Planned 9.3 MB came out: the new rate brings the video part to land ~5 % under the limit
    const double other = p.audioK * 1000.0 / 8 * 30 + p.overheadBytes;
    const double predicted = (9300000 - other) * a / p.videoK + other;
    CHECK(predicted <= 9000000 * 0.96);
}

void testSettings()
{
    RenderSettings s;
    s.maxMb = 9.5;
    CHECK(s.hasMaxMb());
    CHECK(!s.sourceQuality());
    const RenderSettings back = RenderSettings::fromJson(s.toJson());
    CHECK_EQ(back.maxMb, 9.5);
    CHECK(back == s);
    CHECK(!RenderSettings::fromJson(RenderSettings().toJson()).toJson().contains("maxMb"));
    CHECK(s.summary({1280, 720}).contains("MB"));
    s.quality = RenderSettings::kQualitySource; // the limit wins
    CHECK(!s.sourceQuality());
    s.format = "prores";
    CHECK(!s.hasMaxMb());
    s.format = "gif";
    CHECK(!s.hasMaxMb());
    RenderSettings off;
    CHECK(!off.hasMaxMb());
    CHECK(RenderSettings::fromJson(QJsonObject{{"maxMb", -3}}).maxMb == 0);
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QString& name, const QJsonObject& raw, QString* message = nullptr)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        if (message) *message = e.message;
        return e.code;
    }
    return "none";
}

QJsonObject ffprobe(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-show_entries", "stream=codec_type,codec_name,width,height",
                        "-show_entries", "format=duration,size", "-of", "json", path});
    p.waitForFinished(30000);
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object();
}

// The whole file decodes without an error (it plays)
bool plays(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-nostdin", "-v", "error", "-i", path, "-f", "null", "-"});
    if (!p.waitForFinished(120000)) return false;
    const QByteArray errors = p.readAllStandardError().trimmed();
    if (!errors.isEmpty()) std::printf("       decode: %s\n", errors.left(300).constData());
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 && errors.isEmpty();
}

// Renders with max_mb and checks the file: under the limit, the answer's numbers, ffprobe's streams, decodes
QJsonObject renderSized(const QString& project, const QString& out, const QString& format, double mb,
                        const QJsonObject& more = {})
{
    QJsonObject args{{"project", project}, {"out", out}, {"format", format}, {"max_mb", mb},
                     {"encoder", "software"}, {"threads", 4}, {"overwrite", true}};
    for (auto it = more.begin(); it != more.end(); ++it) args[it.key()] = it.value();
    QJsonObject r;
    try {
        r = run("render", args);
    } catch (const Cli::Error& e) {
        Check::report(false, qPrintable(QString("%1: %2 %3").arg(format, e.code, e.message)), __FILE__, __LINE__);
        return {};
    }
    const qint64 bytes = QFileInfo(r["out"].toString()).size();
    std::printf("       %-5s max %.2f MB -> %.3f MB (%lld bytes, %d%% of the limit), video %d kbit/s, audio %d, "
                "%dx%d, %.3f bpp, tries %d, encoder %s, %.1f s\n",
                qPrintable(format), mb, bytes / 1e6, static_cast<long long>(bytes), int(bytes * 100 / (mb * 1e6)),
                r["video_bitrate_k"].toInt(), r["audio_bitrate_k"].toInt(), r["width"].toInt(), r["height"].toInt(),
                r["bits_per_pixel"].toDouble(), r["tries"].toInt(1), qPrintable(r["encoder"].toString()),
                r["render_s"].toDouble());
    for (const QJsonValue& n : r["notes"].toArray()) std::printf("         note: %s\n", qPrintable(n.toString()));
    CHECK(bytes > 0);
    CHECK(bytes <= qint64(mb * 1e6));
    CHECK(r["fits"].toBool());
    CHECK_EQ(r["max_mb"].toDouble(), mb);
    CHECK(std::abs(r["size_mb"].toDouble() - bytes / 1e6) < 0.006);
    CHECK(r["video_bitrate_k"].toInt() > 0);
    // Not far under it either (a size target, not a quality): at least 70 % of the limit
    CHECK(bytes >= qint64(mb * 1e6 * 0.7));
    const QJsonObject probe = ffprobe(r["out"].toString());
    CHECK(std::abs(probe["format"]["duration"].toString().toDouble() - 10) < 0.2);
    bool video = false, audio = false;
    for (const QJsonValue& st : probe["streams"].toArray()) {
        if (st["codec_type"] == "video") {
            video = true;
            CHECK_EQ(st["width"].toInt(), r["width"].toInt());
            CHECK_EQ(st["height"].toInt(), r["height"].toInt());
        }
        if (st["codec_type"] == "audio") audio = true;
    }
    CHECK(video && audio);
    CHECK(plays(r["out"].toString()));
    return r;
}

void testRender(const QTemporaryDir& dir)
{
    // 10 s of a busy 720p30 picture (moving test pattern with noise) and a tone
    const QString src = Check::makeMedia(
        dir.filePath("src.mp4"),
        {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=10,noise=alls=12:allf=t", "-f", "lavfi", "-i",
         "sine=frequency=440:duration=10", "-c:v", "libx264", "-preset", "ultrafast", "-crf", "16", "-pix_fmt",
         "yuv420p", "-c:a", "aac", "-b:a", "192k", "-shortest"});
    if (!CHECK(!src.isEmpty())) return;
    const QString proj = dir.filePath("p.schneidi");
    run("new", {{"project", proj}, {"width", 1280}, {"height", 720}, {"fps", 30}});
    run("edit", {{"project", proj}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(src)}, {"full", true}});

    // H.264 at a few sizes: 4 MB keeps 720p, 1.2 MB steps down, 0.6 MB lower still
    QJsonObject r = renderSized(proj, dir.filePath("h264-4.mp4"), "h264", 4);
    CHECK_EQ(r["height"].toInt(), 720);
    CHECK_EQ(r["audio_bitrate_k"].toInt(), 128);
    r = renderSized(proj, dir.filePath("h264-1.2.mp4"), "h264", 1.2);
    CHECK(r["height"].toInt() < 720);
    CHECK(QJsonDocument(r["notes"].toArray()).toJson().contains("scaled down"));
    r = renderSized(proj, dir.filePath("h264-0.6.mp4"), "h264", 0.6);
    CHECK(r["height"].toInt() <= 540);
    CHECK(r["audio_bitrate_k"].toInt() < 128); // the sound shrinks with it
    // Height given: kept, and the answer says the quality will be poor
    r = renderSized(proj, dir.filePath("h264-0.6-720.mp4"), "h264", 0.6, {{"height", 720}});
    CHECK_EQ(r["height"].toInt(), 720);
    CHECK(QJsonDocument(r["notes"].toArray()).toJson().contains("quality will be poor"));
    // audio_bitrate given: kept
    r = renderSized(proj, dir.filePath("h264-audio.mp4"), "h264", 3, {{"audio_bitrate", 192}});
    CHECK_EQ(r["audio_bitrate_k"].toInt(), 192);
    // The other encoders
    renderSized(proj, dir.filePath("h265.mp4"), "h265", 1.5);
    renderSized(proj, dir.filePath("vp9.webm"), "vp9", 1.5);
    renderSized(proj, dir.filePath("av1.mp4"), "av1", 1.5);

    // Refused
    QString message;
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.mov")}, {"format", "prores"}, {"max_mb", 9}},
                     &message),
             QString("BAD_ARGUMENT"));
    CHECK(message.contains("fixed quality"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.gif")}, {"format", "gif"}, {"max_mb", 9}}),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.m4a")}, {"format", "aac"}, {"max_mb", 9}}),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.png")}, {"format", "png"}, {"max_mb", 9}}),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.srt")}, {"format", "srt"}, {"max_mb", 9}}),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.mp4")}, {"max_mb", 9}, {"quality", "web"}}),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.mp4")}, {"max_mb", 9}, {"review", "video"}}),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.mp4")}, {"max_mb", 0}}), QString("BAD_ARGUMENT"));
    // Too small for 10 s: says what a 360p needs
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.mp4")}, {"max_mb", 0.1}}, &message),
             QString("BAD_ARGUMENT"));
    CHECK(message.contains("too small") && message.contains("MB"));
    CHECK(!QFileInfo::exists(dir.filePath("x.mp4")));
}

void testHelp()
{
    const QString text = QString::fromUtf8(QJsonDocument(run("help", {{"command", "render"}})).toJson());
    CHECK(text.contains("max_mb"));
    CHECK(text.contains("size_mb"));
}

bool waitFor(RenderQueue& q, int ms = 300000)
{
    if (!q.isRunning()) return true;
    QEventLoop loop;
    QObject::connect(&q, &RenderQueue::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    return !q.isRunning();
}

void testDeliver(const QTemporaryDir& dir)
{
    const QString src = dir.filePath("src.mp4");
    Project p;
    {
        ProjectData d;
        d.format.width = 1280;
        d.format.height = 720;
        d.format.rate = {30, 1};
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        Clip c;
        c.id = 1;
        c.mediaPath = src;
        c.out = 299;
        d.timeline.video[0].clips << c;
        d.lastClipId = 1;
        p.load(d);
        p.addMedia(Engine::probe(p.format(), src));
    }
    DeliverPanel panel(&p);
    panel.resize(520, 900);
    auto* maxMb = panel.findChild<QDoubleSpinBox*>("renderMaxMb");
    auto* quality = panel.findChild<QComboBox*>("renderQuality");
    auto* format = panel.findChild<QComboBox*>("renderFormat");
    if (!CHECK(maxMb && quality)) return;
    if (auto* folder = panel.findChild<QLineEdit*>("renderFolder")) folder->setText(dir.path());
    CHECK_EQ(maxMb->value(), 0.0); // off by default
    CHECK(quality->isEnabled());
    maxMb->setValue(1.5);
    CHECK(!quality->isEnabled()); // the limit sets the rate
    RenderJob job = panel.makeJob();
    CHECK_EQ(job.settings.maxMb, 1.5);
    CHECK(job.settings.hasMaxMb());
    if (format) { // a format without a bit rate: the field is off
        const int prores = format->findData(QStringLiteral("prores"));
        if (CHECK(prores >= 0)) {
            format->setCurrentIndex(prores);
            CHECK(!maxMb->isEnabled());
            CHECK(!panel.makeJob().settings.hasMaxMb());
            format->setCurrentIndex(format->findData(QStringLiteral("h264")));
            CHECK(maxMb->isEnabled());
        }
    }
    // What the queue hands the exporter
    job.id = 1;
    job.path = dir.filePath("queue.mp4");
    job.settings.encoder = VideoEncoder::Software;
    job.size = QSize(1280, 720);
    const ExportSettings es = RenderQueue::exportSettings(job, p);
    CHECK(es.targetBitrateK > 0);
    CHECK(!es.smartRender);
    CHECK(!es.sourceQuality);
    CHECK_EQ(es.audioBitrateK, 128);
    // One render through the queue: under the limit
    p.setRenderQueue({job});
    RenderQueue queue(&p);
    CHECK(queue.start());
    CHECK(waitFor(queue));
    const RenderJob done = p.renderQueue().value(0);
    CHECK(done.status == RenderStatus::Done);
    const qint64 bytes = QFileInfo(job.path).size();
    std::printf("       queue: max 1.50 MB -> %.3f MB, %s\n", bytes / 1e6, qPrintable(done.message));
    CHECK(bytes > 0 && bytes <= 1500000);
    CHECK(plays(job.path));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("render_max_mb");
    testPlan();
    testSettings();
    testHelp();
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    Check::initEngine();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    try {
        testRender(tmp);
        testDeliver(tmp);
    } catch (const Cli::Error& e) {
        Check::report(false, qPrintable(QString("unexpected error %1: %2").arg(e.code, e.message)), __FILE__, __LINE__);
    }
    return Check::result();
}
