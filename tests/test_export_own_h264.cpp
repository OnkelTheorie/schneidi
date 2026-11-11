// SPDX-License-Identifier: AGPL-3.0-only
// schneidi's own H.264 encoder as the experimental export format "h264_own": never the default, in no preset; a short
// synthetic timeline into MP4 and MKV (ffprobe sees H.264, frame count, duration, frame rate, decodes without errors,
// PSNR against the source, a key frame at the cut); the bit rate mode (two passes) lands within 10 % of its target;
// 10-bit output is refused before starting; a cancel leaves no file behind. Checked with ffprobe/ffmpeg.

#include "check.h"

#include "core/RenderJob.h"
#include "engine/ExportWriter.h"
#include "engine/Exporter.h"
#include "engine/HardwareEncoders.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cmath>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
constexpr int kFrames = 75; // 3 s
QString g_media, g_noisy;

// One clip of `media` (frames [0, kFrames)), or two with a hard cut at `cut` (the second from frame 0 of the file
// again: the picture jumps)
Timeline timeline(const QString& media, int cut = 0)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.out = (cut > 0 ? cut : kFrames) - 1;
    c.linkId = 1;
    tl.video[0].clips << c;
    c.id = 2;
    tl.audio[0].clips << c;
    if (cut > 0) {
        Clip d = c;
        d.id = 3;
        d.linkId = 2;
        d.start = cut;
        d.in = 0;
        d.out = kFrames - cut - 1;
        tl.video[0].clips << d;
        d.id = 4;
        tl.audio[0].clips << d;
    }
    return tl;
}

ExportSettings settings(const QString& path, int quality = 0)
{
    RenderJob j;
    j.id = 1;
    j.path = path;
    j.settings.format = "h264_own";
    j.settings.quality = quality;
    ExportSettings s = RenderQueue::exportSettings(j, kFmt);
    s.threads = 4;
    return s;
}

bool render(const Timeline& tl, const ExportSettings& s, QString* message = nullptr)
{
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QString msg;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& m) {
        ok = success;
        msg = m;
        loop.quit();
    });
    QString error;
    if (!ex.start(tl, s, &error)) {
        if (message) *message = error;
        return false;
    }
    QTimer::singleShot(180000, &loop, &QEventLoop::quit);
    loop.exec();
    if (message) *message = msg;
    if (!ok) std::printf("       %s: %s\n", qPrintable(s.path), qPrintable(msg.isEmpty() ? "timeout" : msg));
    return ok;
}

QJsonObject probe(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-count_frames", "-select_streams", "v:0", "-show_streams", "-show_format", "-of",
                        "json", path});
    p.waitForFinished(60000);
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    QJsonObject s = o.value("streams").toArray().first().toObject();
    s["format_duration"] = o.value("format").toObject().value("duration");
    s["format_bit_rate"] = o.value("format").toObject().value("bit_rate");
    return s;
}

// Pictures that are key frames (decode order = picture order)
QVector<int> keyFrames(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "frame=key_frame", "-of", "csv=p=0", path});
    p.waitForFinished(60000);
    QVector<int> keys;
    const QList<QByteArray> lines = p.readAllStandardOutput().split('\n');
    for (int i = 0; i < lines.size(); ++i)
        if (lines[i].trimmed() == "1") keys << i;
    return keys;
}

// Decoder messages (empty = decodes cleanly)
QString decodeErrors(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-nostdin", "-v", "error", "-i", path, "-f", "null", "-"});
    p.waitForFinished(60000);
    return QString::fromUtf8(p.readAllStandardError()).trimmed();
}

// PSNR (Y) of the first `frames` pictures against `reference`
double psnr(const QString& path, const QString& reference, int frames)
{
    QProcess p;
    const QString graph = QString("[0:v]trim=end_frame=%1[a];[1:v]trim=end_frame=%1[b];[a][b]psnr").arg(frames);
    p.start("ffmpeg", {"-nostdin", "-i", path, "-i", reference, "-lavfi", graph, "-f", "null", "-"});
    p.waitForFinished(60000);
    const QRegularExpressionMatch m = QRegularExpression("PSNR y:([0-9.]+)").match(QString::fromUtf8(p.readAllStandardError()));
    return m.hasMatch() ? m.captured(1).toDouble() : 0;
}

void testNotDefault()
{
    CHECK(RenderSettings().format != QLatin1String("h264_own"));
    CHECK_EQ(QString(renderFormat("h264_own").id), QString("h264_own"));
    CHECK_EQ(QString(renderFormat("h264_own").videoCodec), QString(ExportWriter::kOwnH264));
    CHECK(!renderFormat("h264_own").hardware); // no encoder choice: always the own one
    CHECK(HardwareEncoders::formatAvailable("h264_own"));
    for (const RenderPreset& p : RenderPresets::builtins())
        if (!CHECK(p.settings.format != QLatin1String("h264_own"))) std::printf("       preset %s\n", qPrintable(p.id));
    // An unknown format falls back to H.264 with x264/OpenH264, never to the own encoder
    CHECK_EQ(QString(renderFormat("nonsense").id), QString("h264"));
    // Quality levels -> QP: finer for better, in range
    CHECK(ExportWriter::ownH264Qp(18) < ExportWriter::ownH264Qp(22));
    CHECK(ExportWriter::ownH264Qp(22) < ExportWriter::ownH264Qp(28));
    CHECK(ExportWriter::ownH264Qp(0) >= 10 && ExportWriter::ownH264Qp(60) <= 51);
}

void testContainers(const QString& dir)
{
    for (const char* name : {"own.mp4", "own.mkv"}) {
        const QString path = dir + "/" + name;
        if (!CHECK(render(timeline(g_media, 40), settings(path)))) continue;
        const QJsonObject v = probe(path);
        CHECK_EQ(v.value("codec_name").toString(), QString("h264"));
        CHECK_EQ(v.value("width").toInt(), 640);
        CHECK_EQ(v.value("height").toInt(), 360);
        CHECK_EQ(v.value("pix_fmt").toString(), QString("yuv420p"));
        CHECK_EQ(v.value("r_frame_rate").toString(), QString("25/1"));
        if (!CHECK_EQ(v.value("nb_read_frames").toString().toInt(), kFrames)) std::printf("       %s\n", name);
        const double duration = v.value("format_duration").toString().toDouble();
        if (!CHECK(std::abs(duration - kFrames / 25.0) < 0.1)) std::printf("       %s: %.3f s\n", name, duration);
        const QString errors = decodeErrors(path);
        if (!CHECK(errors.isEmpty())) std::printf("       %s\n", qPrintable(errors.left(400)));
        // The picture against the timeline's: the first 40 frames are the source's first 40
        const double db = psnr(path, g_media, 40);
        if (!CHECK(db > 38)) std::printf("       %s: PSNR %.2f dB\n", name, db);
        // Key frames: the first picture and the cut at frame 40
        const QVector<int> keys = keyFrames(path);
        CHECK(keys.contains(0));
        if (!CHECK(keys.contains(40))) qDebug() << name << "key frames" << keys;
    }
    CHECK_EQ(Exporter::cutFrames(timeline(g_media, 40), 0, kFrames, {25, 1}, {25, 1}), QVector<int>{40});
    CHECK_EQ(Exporter::cutFrames(timeline(g_media, 40), 0, kFrames, {25, 1}, {50, 1}), QVector<int>{80});
    CHECK_EQ(Exporter::cutFrames(timeline(g_media, 40), 41, 20, {25, 1}, {25, 1}), QVector<int>{});
}

void testBitrate(const QString& dir)
{
    const QString path = dir + "/rate.mp4";
    ExportSettings s = settings(path, RenderSettings::kQualityWeb);
    CHECK(s.maxBitrateK > 0); // "web" quality = bit rate mode
    s.maxBitrateK = 1500;
    s.audioCodec.clear(); // the video's bit rate alone
    if (!CHECK(render(timeline(g_noisy), s))) return;
    const QJsonObject v = probe(path);
    CHECK_EQ(v.value("nb_read_frames").toString().toInt(), kFrames);
    const double kbit = QFileInfo(path).size() * 8.0 / 1000 / (kFrames / 25.0);
    if (!CHECK(std::abs(kbit / 1500 - 1) < 0.10)) std::printf("       %.0f kbit/s for 1500\n", kbit);
    CHECK(decodeErrors(path).isEmpty());
}

void testRefused(const QString& dir)
{
    ExportSettings s = settings(dir + "/deep.mp4");
    s.pixFmt = "yuv420p10le";
    QString message;
    CHECK(!render(timeline(g_media), s, &message));
    if (!CHECK(message.contains("8-bit 4:2:0"))) std::printf("       %s\n", qPrintable(message));
    CHECK(!QFile::exists(s.path));
    s.pixFmt = "yuv444p";
    CHECK(!render(timeline(g_media), s, &message));
    // No H.264 in WebM
    CHECK(!render(timeline(g_media), settings(dir + "/x.webm"), &message));
}

void testCancel(const QString& dir)
{
    const QString sub = dir + "/cancel";
    QDir().mkpath(sub);
    for (int quality : {0, int(RenderSettings::kQualityWeb)}) { // also in the first pass
        ExportSettings s = settings(sub + "/c.mp4", quality);
        Exporter ex;
        QEventLoop loop;
        bool finished = false, ok = true;
        QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
            finished = true;
            ok = success;
            loop.quit();
        });
        QObject::connect(&ex, &Exporter::progress, &ex, [&](int) {
            if (ex.framesWritten() > 0 || quality > 0) ex.cancel();
        });
        QString error;
        if (!CHECK(ex.start(timeline(g_noisy), s, &error))) continue;
        QTimer::singleShot(60000, &loop, &QEventLoop::quit);
        if (!finished) loop.exec();
        CHECK(finished);
        CHECK(!ok);
        CHECK(!ex.isRunning());
        if (!CHECK(QDir(sub).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty()))
            qDebug() << QDir(sub).entryList(QDir::Files);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_own_h264");
    testNotDefault();
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("src.mkv"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=3", "-f", "lavfi", "-i",
                                "sine=frequency=440:duration=3", "-vf", "scale=out_color_matrix=bt709", "-colorspace",
                                "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-c:v", "ffv1", "-pix_fmt",
                                "yuv420p", "-c:a", "flac",
                                "-shortest"});
    // Hard to code (moving noise): the bit rate mode has something to spend its bits on
    g_noisy = Check::makeMedia(tmp.filePath("noisy.mkv"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=3,noise=alls=12:allf=t",
                                "-c:v", "ffv1", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_media.isEmpty() && !g_noisy.isEmpty())) return Check::result();
    testContainers(tmp.path());
    testBitrate(tmp.path());
    testRefused(tmp.path());
    testCancel(tmp.path());
    return Check::result();
}
