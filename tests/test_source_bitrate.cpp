// SPDX-License-Identifier: AGPL-3.0-only
// Export quality "Like source" (0.22): the target bit rate (duration-weighted over the video tracks' media, scaled by
// output / source pixel rate; titles, stills, hidden tracks, disabled clips and unknown files left out; nested
// sequences counted; nothing known -> 0), its label, the settings' JSON (older versions read "High"), the encoder
// options per codec, MediaProbe::videoRate of MP4/Matroska/stills, an export that lands within 25 % of the source's
// bit rate (also scaled down), and `render quality:"source"` with its answer, fallback note and help text.
#include "check.h"

#include "cli/Commands.h"
#include "core/I18n.h"
#include "core/RenderJob.h"
#include "core/SourceBitrate.h"
#include "engine/Exporter.h"
#include "engine/HardwareEncoders.h"
#include "engine/MediaProbe.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <memory>

namespace {

Clip media(int id, const QString& path, int start, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = 0;
    c.out = length - 1;
    return c;
}

bool near(double a, double b, double tolerance)
{
    const bool ok = std::abs(a - b) <= tolerance * b;
    if (!ok) std::printf("       %.1f vs %.1f (±%.0f %%)\n", a, b, tolerance * 100);
    return ok;
}

void testTarget()
{
    using SourceBitrate::Video;
    const QHash<QString, Video> files{
        {"/a.mp4", {10000, QSize(1920, 1080), 60}},
        {"/b.mkv", {2000, QSize(1920, 1080), 60}},
        {"/c.mov", {8000, QSize(), 0}}, // size and rate unknown: not scaled
    };
    const SourceBitrate::Lookup lookup = [&](const QString& p) -> std::optional<Video> {
        if (files.contains(p)) return files.value(p);
        return std::nullopt; // stills, unknown files
    };
    const QSize hd(1920, 1080);
    const FrameRate r60{60, 1};

    Timeline tl;
    tl.video.resize(2);
    tl.video[0].clips << media(1, "/a.mp4", 0, 100) << media(2, "/b.mkv", 100, 300);
    // One file
    CHECK_EQ(SourceBitrate::target(tl, 0, 99, hd, r60, lookup).targetK, 10000);
    // Mixed, weighted by length: (10000 x 100 + 2000 x 300) / 400
    SourceBitrate::Result r = SourceBitrate::target(tl, 0, -1, hd, r60, lookup);
    CHECK_EQ(r.targetK, 4000);
    CHECK_EQ(r.files, 2);
    CHECK_EQ(r.frames, 400);
    // A range: 50 frames of a, 50 of b
    CHECK_EQ(SourceBitrate::target(tl, 50, 149, hd, r60, lookup).targetK, 6000);
    // Scaled: half the size and half the frame rate = an eighth
    CHECK_EQ(SourceBitrate::target(tl, 0, 99, QSize(960, 540), {30, 1}, lookup).targetK, 1250);
    // 4K output of a 1080p source: four times
    CHECK_EQ(SourceBitrate::target(tl, 0, 99, QSize(3840, 2160), r60, lookup).targetK, 40000);
    // A source of unknown size and rate keeps its rate
    Timeline unknownSize;
    unknownSize.video.resize(1);
    unknownSize.video[0].clips << media(1, "/c.mov", 0, 50);
    CHECK_EQ(SourceBitrate::target(unknownSize, 0, -1, QSize(640, 360), {25, 1}, lookup).targetK, 8000);

    // Stills, titles and unknown files do not count; nothing known = 0
    Timeline other;
    other.video.resize(2);
    other.video[0].clips << media(1, "/still.png", 0, 100);
    Clip title;
    title.id = 2;
    title.kind = ClipKind::Title;
    title.start = 100;
    title.out = 49;
    other.video[0].clips << title;
    r = SourceBitrate::target(other, 0, -1, hd, r60, lookup);
    CHECK_EQ(r.targetK, 0);
    CHECK_EQ(r.files, 0);
    // ... next to a known one: only the known one, at its rate (not diluted by the still)
    other.video[1].clips << media(3, "/b.mkv", 0, 150);
    CHECK_EQ(SourceBitrate::target(other, 0, -1, hd, r60, lookup).targetK, 2000);
    // Hidden track, disabled clip, freeze frame: left out
    other.video[1].hidden = true;
    CHECK_EQ(SourceBitrate::target(other, 0, -1, hd, r60, lookup).targetK, 0);
    other.video[1].hidden = false;
    other.video[1].clips[0].enabled = false;
    CHECK_EQ(SourceBitrate::target(other, 0, -1, hd, r60, lookup).targetK, 0);
    other.video[1].clips[0].enabled = true;
    other.video[1].clips[0].freeze = true;
    CHECK_EQ(SourceBitrate::target(other, 0, -1, hd, r60, lookup).targetK, 0);
    // No lookup, empty timeline, empty range
    CHECK_EQ(SourceBitrate::target(tl, 0, -1, hd, r60, {}).targetK, 0);
    CHECK_EQ(SourceBitrate::target(Timeline{}, 0, -1, hd, r60, lookup).targetK, 0);
    CHECK_EQ(SourceBitrate::target(tl, 500, 600, hd, r60, lookup).targetK, 0);

    // A compound clip shows its sequence's media (the part it shows): frames 100-199 of the inner = b
    Timeline inner;
    inner.video.resize(1);
    inner.video[0].clips << media(1, "/a.mp4", 0, 100) << media(2, "/b.mkv", 100, 100);
    auto nested = std::make_shared<NestedTimelines>();
    nested->insert(7, inner);
    Timeline outer;
    outer.video.resize(1);
    Clip compound;
    compound.id = 1;
    compound.kind = ClipKind::Compound;
    compound.sequenceId = 7;
    compound.in = 100;
    compound.out = 199;
    outer.video[0].clips << compound;
    outer.nested = nested;
    CHECK_EQ(SourceBitrate::target(outer, 0, -1, hd, r60, lookup).targetK, 2000);
    outer.video[0].clips[0].in = 50; // half a, half b
    outer.video[0].clips[0].out = 149;
    CHECK_EQ(SourceBitrate::target(outer, 0, -1, hd, r60, lookup).targetK, 6000);

    // Label
    CHECK(SourceBitrate::label(30400).contains("30") && SourceBitrate::label(30400).contains("Mbit/s"));
    CHECK(SourceBitrate::label(30400).startsWith(QStringLiteral("≈ ")));
    CHECK(SourceBitrate::label(2500).contains("Mbit/s"));
    CHECK(SourceBitrate::label(850).contains("850") && SourceBitrate::label(850).contains("kbit/s"));
    CHECK(SourceBitrate::label(0).isEmpty());
}

void testSettings()
{
    RenderSettings s;
    s.quality = RenderSettings::kQualitySource;
    CHECK(s.sourceQuality());
    CHECK_EQ(s.crf(), 18); // "High" where nothing is known
    CHECK_EQ(s.maxBitrateK(QSize(1920, 1080), {25, 1}), 0);
    // Saved as "High" + a flag: versions before 0.22 clamp "quality" to 0-3 and must not read "Web"
    const QJsonObject o = s.toJson();
    CHECK_EQ(o["quality"].toInt(-1), 0);
    CHECK(o["qualitySource"].toBool());
    CHECK(RenderSettings::fromJson(o) == s);
    CHECK_EQ(RenderSettings::fromJson(QJsonObject{{"quality", 3}}).quality, RenderSettings::kQualityWeb);
    CHECK_EQ(RenderSettings::fromJson(QJsonObject{{"quality", 9}}).quality, RenderSettings::kQualityWeb);
    RenderSettings high;
    CHECK(!high.toJson().contains("qualitySource"));
    // Every format with a quality choice offers it; fixed-quality formats and audio do not
    for (const char* f : {"h264", "h265", "av1", "vp9", "h264_own"}) {
        s.format = f;
        if (!CHECK(s.sourceQuality())) std::printf("       %s\n", f);
    }
    for (const char* f : {"prores", "dnxhr-hq", "gif", "png", "wav", "aac"}) {
        s.format = f;
        if (!CHECK(!s.sourceQuality())) std::printf("       %s\n", f);
    }
    // Summary: the label, with the target when known
    s.format = "h264";
    const QString plain = s.summary(QSize(1920, 1080));
    const QString with = s.summary(QSize(1920, 1080), 30000);
    CHECK(plain.contains(s.qualityLabel()));
    CHECK(with.contains(s.qualityLabel()) && with.contains("30") && with.contains("Mbit/s"));
    // The job keeps the target (queue card), saved with the project
    RenderJob j;
    j.id = 1;
    j.path = "/x/a.mp4";
    j.settings = s;
    j.sourceBitrateK = 12345;
    const QVector<RenderJob> back = RenderQueueJson::fromJson(RenderQueueJson::toJson({j}));
    CHECK(back.size() == 1 && back[0] == j);
}

void testEncoderOptions()
{
    const auto has = [](const QVector<QPair<QString, QString>>& o, const QString& k, const QString& v) {
        return o.contains(qMakePair(k, v));
    };
    const auto x264 = HardwareEncoders::softwareBitrateOptions("libx264", 30000);
    CHECK(has(x264, "b", "30000k") && has(x264, "maxrate", "45000k") && has(x264, "bufsize", "60000k"));
    const auto nvenc = HardwareEncoders::bitrateOptions("h264_nvenc", 30000);
    CHECK(has(nvenc, "rc", "vbr") && has(nvenc, "b", "30000k") && has(nvenc, "maxrate", "45000k"));
    CHECK(!std::any_of(nvenc.begin(), nvenc.end(), [](const auto& p) { return p.first == "cq"; }));
    CHECK(has(HardwareEncoders::bitrateOptions("h264_vaapi", 8000), "rc_mode", "VBR"));
    CHECK(has(HardwareEncoders::bitrateOptions("hevc_qsv", 8000), "b", "8000k"));
    CHECK(has(HardwareEncoders::softwareBitrateOptions("libopenh264", 8000), "rc_mode", "bitrate"));
    CHECK(HardwareEncoders::bitrateOptions("libx264", 8000).isEmpty());
}

QJsonObject ffprobe(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=bit_rate,width,height",
                        "-show_entries", "format=duration,size", "-of", "json", path});
    p.waitForFinished(30000);
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object();
}

double streamKbit(const QString& path)
{
    return ffprobe(path)["streams"][0]["bit_rate"].toString().toDouble() / 1000.0;
}

int renderSource(const QString& src, const QString& out, QSize size, int* targetK)
{
    const ProjectFormat fmt{640, 360, {25, 1}};
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << media(1, src, 0, 150);
    RenderJob job;
    job.id = 1;
    job.path = out;
    job.settings.quality = RenderSettings::kQualitySource;
    job.settings.encoder = VideoEncoder::Software;
    job.settings.smartRender = false; // encode, not copy: the encoder's rate is what is tested
    job.size = size;
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        if (!ok) std::printf("       %s\n", qPrintable(msg));
        loop.quit();
    });
    QString error;
    ExportSettings es = RenderQueue::exportSettings(job, fmt);
    CHECK(es.sourceQuality);
    if (!CHECK(ex.start(tl, es, &error))) {
        std::printf("       %s\n", qPrintable(error));
        return 0;
    }
    QTimer::singleShot(180000, &loop, &QEventLoop::quit);
    loop.exec();
    *targetK = ex.targetBitrateK();
    if (!CHECK(ok)) return 0;
    std::printf("       %s: target %d kbit/s, codec %s\n", qPrintable(QFileInfo(out).fileName()), *targetK,
                qPrintable(ex.usedCodec()));
    return 1;
}

const QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QString& name, const QJsonObject& raw)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

void testMedia(const QTemporaryDir& dir)
{
    // A demanding picture (noise) at an average bit rate of 3 Mbit/s, with sound; MP4 knows the stream's rate,
    // Matroska (written by ffmpeg, no statistics tags) only the file's
    const QStringList input{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=6,noise=alls=25:allf=t",
                            "-f", "lavfi", "-i", "sine=frequency=440:duration=6"};
    const QStringList video{"-c:v", "libx264", "-b:v", "3000k", "-maxrate", "3300k", "-bufsize", "6000k",
                            "-pix_fmt", "yuv420p", "-g", "50", "-c:a", "aac", "-b:a", "128k", "-shortest"};
    const QString mp4 = Check::makeMedia(dir.filePath("src.mp4"), input + video);
    const QString mkv = Check::makeMedia(dir.filePath("src.mkv"), input + video);
    const QString png = Check::makeMedia(dir.filePath("still.png"),
                                         {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=1:duration=1", "-frames:v", "1"});
    if (!CHECK(!mp4.isEmpty() && !mkv.isEmpty() && !png.isEmpty())) return;
    const double srcK = streamKbit(mp4);
    std::printf("       source: %.0f kbit/s (ffprobe)\n", srcK);
    CHECK(srcK > 2000);

    // Probe
    const std::optional<SourceBitrate::Video> a = MediaProbe::videoRate(mp4);
    if (CHECK(a.has_value())) {
        CHECK(near(a->kbit, srcK, 0.05));
        CHECK(a->size == QSize(640, 360));
        CHECK(std::abs(a->fps - 25) < 0.01);
    }
    const std::optional<SourceBitrate::Video> b = MediaProbe::videoRate(mkv);
    if (CHECK(b.has_value())) {
        std::printf("       mkv: %.0f kbit/s (file minus sound)\n", b->kbit);
        CHECK(near(b->kbit, srcK, 0.15));
        CHECK(b->size == QSize(640, 360));
    }
    CHECK(!MediaProbe::videoRate(png).has_value());
    CHECK(!MediaProbe::videoRate(dir.filePath("missing.mp4")).has_value());

    // Export: within 25 % of the source's rate; at half the size a quarter of it
    int target = 0;
    const QString out = dir.filePath("out.mp4");
    if (renderSource(mp4, out, {}, &target)) {
        CHECK(near(target, srcK, 0.05));
        const double got = streamKbit(out);
        std::printf("       export: %.0f kbit/s (source %.0f)\n", got, srcK);
        CHECK(near(got, srcK, 0.25));
    }
    const QString small = dir.filePath("small.mp4");
    if (renderSource(mp4, small, QSize(320, 180), &target)) {
        CHECK(near(target, srcK / 4, 0.05));
        const double got = streamKbit(small);
        std::printf("       export 320x180: %.0f kbit/s (target %d)\n", got, target);
        CHECK(near(got, target, 0.25));
    }

    // CLI
    // (the timeline as big as the source: a 1080p timeline would ask for 9 times the bits, scaled up)
    const QString proj = dir.filePath("p.schneidi");
    run("new", {{"project", proj}, {"width", 640}, {"height", 360}, {"fps", 25}});
    run("edit", {{"project", proj}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(mkv)}, {"full", true}});
    const QJsonObject r = run("render", {{"project", proj}, {"out", dir.filePath("cli.mp4")}, {"quality", "source"},
                                         {"encoder", "software"}});
    CHECK_EQ(r["quality"].toString(), QString("source"));
    CHECK(near(r["video_bitrate_k"].toDouble(), b ? b->kbit : srcK, 0.05));
    const QString unknown = T("Bitrate der Quelle unbekannt, mit Qualität „Hoch“ gerendert.");
    CHECK(!r["encoder_note"].toString().contains(unknown));
    const double cliK = streamKbit(r["out"].toString());
    std::printf("       cli: %.0f kbit/s, target %d, encoder %s\n", cliK, r["video_bitrate_k"].toInt(),
                qPrintable(r["encoder"].toString()));
    CHECK(near(cliK, r["video_bitrate_k"].toDouble(), 0.25));
    // Scaled down by the CLI's height: about a quarter
    const QJsonObject half = run("render", {{"project", proj}, {"out", dir.filePath("cli-half.mp4")}, {"quality", "4"},
                                            {"height", 180}});
    CHECK(near(half["video_bitrate_k"].toDouble(), r["video_bitrate_k"].toDouble() / 4, 0.05));
    // (encoded, not copied: another size; with "auto" a graphics card encoder where one works)
    const double halfK = streamKbit(half["out"].toString());
    std::printf("       cli 320x180: %.0f kbit/s, target %d, encoder %s\n", halfK, half["video_bitrate_k"].toInt(),
                qPrintable(half["encoder"].toString()));
    CHECK(near(halfK, half["video_bitrate_k"].toDouble(), 0.25));
    // Old callers: numbers still work, other names are refused
    CHECK(!run("render", {{"project", proj}, {"out", dir.filePath("cli-web.mp4")}, {"quality", 3}, {"height", 180}})
               .contains("video_bitrate_k"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.mp4")}, {"quality", "best"}}),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.mp4")}, {"quality", 5}}), QString("BAD_ARGUMENT"));
    // Nothing known (a still only): rendered as "high", said in the answer
    const QString stillProj = dir.filePath("still.schneidi");
    run("new", {{"project", stillProj}});
    run("edit", {{"project", stillProj}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(png)}, {"full", true}});
    const QJsonObject still = run("render", {{"project", stillProj}, {"out", dir.filePath("still.mp4")}, {"quality", "source"},
                                             {"to", "1s"}});
    CHECK_EQ(still["video_bitrate_k"].toInt(-1), 0);
    CHECK(still["encoder_note"].toString().contains(unknown));
    CHECK(QFileInfo(still["out"].toString()).size() > 0);
}

void testHelp()
{
    const QJsonObject help = run("help", {{"command", "render"}});
    const QString text = QString::fromUtf8(QJsonDocument(help).toJson());
    CHECK(text.contains("source (4)"));
    CHECK(text.contains("video_bitrate_k"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("source_bitrate");
    testTarget();
    testSettings();
    testEncoderOptions();
    testHelp();
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    Check::initEngine();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    try {
        testMedia(tmp);
    } catch (const Cli::Error& e) {
        Check::report(false, qPrintable(QString("unexpected error %1: %2").arg(e.code, e.message)), __FILE__, __LINE__);
    }
    return Check::result();
}
