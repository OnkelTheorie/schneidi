// SPDX-License-Identifier: AGPL-3.0-only
// schneidi's own H.264 encoder s264 as the experimental export format "s264" (id "h264_own" up to 0.23, still read):
// never the default, in no preset, never a fallback (not even in the GPL-free build); a short
// synthetic timeline into MP4 and MKV (ffprobe sees H.264, frame count, duration, frame rate, decodes without errors,
// PSNR against the source, no key frame at a cut between two near-identical sources (soft cut keys), BT.709 limited range in the
// SPS's VUI and the container, the level from levelIdcFor); which cuts force key frames (no splits, jump cuts or
// montage storms, only clips inside the range); clips shorter than the lookahead; a size that is no multiple of 16; a
// shorter lookahead (low memory) reaches the encoder; the "web" cap: two passes land within 10 % of it on noise, a
// still picture keeps the fixed QP far below it, a cap below the fixed QP's rate is met; 10-bit/4:4:4/WebM refused with a reason and no file, also by the
// writer itself; cancel during the first pass and while encoding leaves no file; B pictures: pts/dts in MP4 and MKV
// (testBFrames). Checked with ffprobe/ffmpeg.

#include "check.h"

#include "core/Keyframes.h"
#include "core/RenderJob.h"
#include "engine/ExportWriter.h"
#include "engine/Exporter.h"
#include "engine/HardwareEncoders.h"
#include "engine/encoder/h264/H264Encoder.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <clocale>
#include <cmath>
#include <tuple>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
constexpr int kFrames = 75; // 3 s
QString g_media, g_near, g_noisy, g_still, g_zoom;

// A piece of a source on the main track
struct Piece {
    QString media;
    int in = 0, length = 0;
};

// The pieces one after the other on one video track (no sound)
Timeline sequence(const QVector<Piece>& pieces)
{
    Timeline tl;
    tl.video.resize(1);
    int start = 0, id = 1;
    for (const Piece& p : pieces) {
        Clip c;
        c.id = id++;
        c.mediaPath = p.media;
        c.start = start;
        c.in = p.in;
        c.out = p.in + p.length - 1;
        tl.video[0].clips << c;
        start += p.length;
    }
    return tl;
}

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

ExportSettings settings(const QString& path, int quality = 0, const ProjectFormat& format = kFmt)
{
    RenderJob j;
    j.id = 1;
    j.path = path;
    j.settings.format = "s264";
    j.settings.quality = quality;
    ExportSettings s = RenderQueue::exportSettings(j, format);
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

// Fields of the first SPS as FFmpeg's trace_headers reads them from the bitstream (not the container's tags), e.g.
// "level_idc", "colour_primaries", "video_full_range_flag"; -1 = not present
QMap<QString, int> spsFields(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-nostdin", "-v", "info", "-i", path, "-map", "0:v:0", "-frames:v", "1", "-c:v", "copy", "-bsf:v",
                       "trace_headers", "-f", "null", "-"});
    p.waitForFinished(60000);
    QMap<QString, int> fields;
    const QString log = QString::fromUtf8(p.readAllStandardError());
    // Lines like "[trace_headers @ 0x..] 24          level_idc                                  00011110 = 30"
    static const QRegularExpression line(R"(^\[trace_headers[^\]]*\]\s+\d+\s+(\w+)\s+[01]+\s+=\s+(-?\d+)\s*$)",
                                         QRegularExpression::MultilineOption);
    for (auto it = line.globalMatch(log); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        if (!fields.contains(m.captured(1))) fields[m.captured(1)] = m.captured(2).toInt();
    }
    return fields;
}

// The level the encoder picks for this export (640x360, 25 fps, defaults otherwise)
int expectedLevel(int bitrateKbps)
{
    schneidi::h264::EncoderConfig c;
    c.width = kFmt.width;
    c.height = kFmt.height;
    c.fps = 25;
    c.bitrateKbps = bitrateKbps;
    return schneidi::h264::levelIdcFor(c);
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

// Packets of the video stream in file order (= decode order): pts, dts (stream time base) and the time base
struct Packets {
    QVector<qint64> pts, dts;
    double timeBase = 0;
    double videoStart = 0, audioStart = -1; // start_time of the streams (s); -1 = no audio
};
Packets packets(const QString& path)
{
    Packets r;
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-show_entries", "packet=stream_index,codec_type,pts,dts:stream=index,codec_type,time_base,start_time",
                        "-of", "json", path});
    p.waitForFinished(60000);
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    int video = -1;
    for (const QJsonValue& v : o.value("streams").toArray()) {
        const QJsonObject st = v.toObject();
        if (st.value("codec_type").toString() == "video" && video < 0) {
            video = st.value("index").toInt();
            const QStringList tb = st.value("time_base").toString().split('/');
            if (tb.size() == 2 && tb[1].toDouble() > 0) r.timeBase = tb[0].toDouble() / tb[1].toDouble();
            r.videoStart = st.value("start_time").toString().toDouble();
        } else if (st.value("codec_type").toString() == "audio" && r.audioStart < 0) {
            r.audioStart = st.value("start_time").toString().toDouble();
        }
    }
    for (const QJsonValue& v : o.value("packets").toArray()) {
        const QJsonObject pk = v.toObject();
        if (pk.value("stream_index").toInt() != video) continue;
        r.pts << qint64(pk.value("pts").toDouble());
        r.dts << qint64(pk.value("dts").toDouble());
    }
    return r;
}

// Picture types in display order ("IBBP...")
QString pictureTypes(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "frame=pict_type", "-of", "csv=p=0", path});
    p.waitForFinished(60000);
    QString types;
    for (const QByteArray& line : p.readAllStandardOutput().split('\n'))
        if (!line.trimmed().isEmpty()) types += QChar(line.trimmed().at(0));
    return types;
}

void testNotDefault()
{
    CHECK(RenderSettings().format != QLatin1String("s264"));
    CHECK_EQ(QString(renderFormat("s264").id), QString("s264"));
    CHECK_EQ(QString(renderFormat("s264").videoCodec), QString(ExportWriter::kOwnH264));
    CHECK(!renderFormat("s264").hardware); // no encoder choice: always s264
    CHECK(HardwareEncoders::formatAvailable("s264"));
    // The id up to 0.23 still means s264 (saved render settings, user presets)
    CHECK_EQ(QString(renderFormat("h264_own").id), QString("s264"));
    CHECK(ExportWriter::isOwnH264("h264_own"));
    RenderSettings old;
    old.format = "h264_own";
    CHECK_EQ(RenderSettings::fromJson(old.toJson()).format, QString("s264"));
    // Never chosen automatically: software H.264 is x264 or OpenH264, also in the GPL-free build
    CHECK(!ExportWriter::isOwnH264(HardwareEncoders::softwareCodec("h264")));
    if (HardwareEncoders::builtIn("libx264")) CHECK_EQ(HardwareEncoders::softwareCodec("h264"), QString("libx264"));
    for (const RenderPreset& p : RenderPresets::builtins())
        if (!CHECK(p.settings.format != QLatin1String("s264"))) std::printf("       preset %s\n", qPrintable(p.id));
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
        // At frame 40 the near-identical second source continues where the first one is: the cut is passed on as a
        // key frame request, but the picture does not change there, so s264 continues with a P picture
        // (EncoderConfig::softCutKeys, since 22.11.2026; an IDR until then)
        Timeline tl = timeline(g_media, 40);
        tl.video[0].clips[1].mediaPath = g_near;
        tl.video[0].clips[1].in = 40;
        if (!CHECK(render(tl, settings(path)))) continue;
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
        // Key frames: only the first picture (the cut at frame 40 continues the picture)
        const QVector<int> keys = keyFrames(path);
        if (!CHECK_EQ(keys, (QVector<int>{0}))) qDebug() << name << "key frames" << keys;
        // Colour tags as the libx264 path writes them (the export's BT.709, limited range): container ...
        CHECK_EQ(v.value("color_space").toString(), QString("bt709"));
        CHECK_EQ(v.value("color_primaries").toString(), QString("bt709"));
        CHECK_EQ(v.value("color_transfer").toString(), QString("bt709"));
        CHECK_EQ(v.value("color_range").toString(), QString("tv"));
        CHECK_EQ(v.value("profile").toString(), QString("High"));
        // ... and the bitstream itself (VUI of the SPS), with the level from levelIdcFor
        const QMap<QString, int> sps = spsFields(path);
        if (!CHECK_EQ(sps.value("level_idc", -1), expectedLevel(0))) qDebug() << name << sps;
        CHECK_EQ(v.value("level").toInt(), expectedLevel(0));
        CHECK_EQ(sps.value("colour_primaries", -1), 1);
        CHECK_EQ(sps.value("transfer_characteristics", -1), 1);
        CHECK_EQ(sps.value("matrix_coefficients", -1), 1);
        CHECK_EQ(sps.value("video_full_range_flag", -1), 0);
    }
}

// B pictures (the encoder's default, adaptive; with bframes=3 short runs as a B pyramid): access units come in coding
// order, the writer takes pts from the encoder's display index and dts from the access unit count minus its reorder
// delay (2 with the pyramid). In MP4 and MKV: dts strictly
// rising (MP4; Matroska stores no dts, FFmpeg guesses them: never falling), every picture's pts once (one frame
// apart in display order), pts never before dts, B pictures really there, the right length, clean decoding, sound and
// picture start apart by the same amount as in the reference export (x264 where built in, else the own encoder
// without B pictures). With "bframes=0": no B pictures, pts = dts. A slow zoom with grain: B pictures pay there
// (on testsrc2 and still pictures the adaptive placement keeps P pictures). Prints the MD5 of the video stream (the
// same input and settings must give the same stream on every run, whatever the load).
// The option "speed=1" (the encoder's fast stage) reaches the encoder: with B pictures another stream than the default,
// the same picture types and count, and it plays (ffmpeg decodes every picture).
void testSpeedOption(const QString& dir)
{
    QByteArray sum[2];
    for (int fast = 0; fast < 2; ++fast) {
        const QString path = dir + (fast ? "/speed-fast.mp4" : "/speed-default.mp4");
        Timeline tl = timeline(g_media);
        tl.video[0].clips[0].mediaPath = g_zoom;
        ExportSettings s = settings(path);
        s.codecOptions << "bframes=3";
        if (fast) s.codecOptions << "speed=1";
        if (!CHECK(render(tl, s))) return;
        QProcess md5;
        md5.start("ffmpeg", {"-nostdin", "-v", "error", "-i", path, "-map", "0:v", "-c", "copy", "-f", "md5", "-"});
        md5.waitForFinished(60000);
        sum[fast] = md5.readAllStandardOutput().trimmed();
        const QString types = pictureTypes(path);
        CHECK_EQ(int(types.size()), kFrames);
        CHECK(types.contains('B'));
    }
    CHECK(!sum[0].isEmpty());
    CHECK(sum[0] != sum[1]);
}

void testBFrames(const QString& dir)
{
    // Start of the picture minus start of the sound in the reference export per container
    QMap<QString, double> referenceOffset;
    for (const char* ext : {"mp4", "mkv"}) {
        const QString path = dir + "/bref." + ext;
        Timeline tl = timeline(g_media);
        tl.video[0].clips[0].mediaPath = g_zoom; // (the sound stays g_media's sine)
        ExportSettings s = settings(path);
        if (HardwareEncoders::builtIn("libx264")) {
            s.videoCodec = s.softwareCodec = "libx264";
        } else {
            s.codecOptions << "bframes=0";
        }
        if (!CHECK(render(tl, s))) continue;
        const Packets pk = packets(path);
        referenceOffset[ext] = pk.videoStart - pk.audioStart;
        std::printf("  reference %s (%s): video starts %.4f s, audio %.4f s\n", ext, qPrintable(s.videoCodec), pk.videoStart,
                    pk.audioStart);
    }
    // 0 = the default (runs of up to 8 B pictures, since the encoder's B pyramid also on long runs a pyramid), 1 = runs of
    // at most 3 (bframes=3): B pyramid, the middle B picture decoded before its outer ones, 2 = no B pictures
    for (const int variant : {0, 1, 2})
        for (const char* name : {"b.mp4", "b.mkv"}) {
            const bool withB = variant != 2, pyramid = withB;
            const QString path = dir + "/" + QStringList{"", "pyr-", "nob-"}[variant] + name;
            const bool mkv = QString(name).endsWith("mkv");
            Timeline tl = timeline(g_media);
            tl.video[0].clips[0].mediaPath = g_zoom;
            ExportSettings s = settings(path);
            if (!withB) s.codecOptions << "bframes=0";
            if (pyramid) s.codecOptions << "bframes=3";
            if (!CHECK(render(tl, s))) continue;
            {
                QProcess md5;
                md5.start("ffmpeg", {"-nostdin", "-v", "error", "-i", path, "-map", "0:v", "-c", "copy", "-f", "md5", "-"});
                md5.waitForFinished(60000);
                std::printf("  %s video stream %s\n", qPrintable(QFileInfo(path).fileName()),
                            md5.readAllStandardOutput().trimmed().constData());
            }
            const QString types = pictureTypes(path);
            if (!CHECK_EQ(int(types.size()), kFrames)) std::printf("       %s: %s\n", qPrintable(path), qPrintable(types));
            if (!CHECK_EQ(types.contains('B'), withB)) std::printf("       %s: %s\n", qPrintable(path), qPrintable(types));
            const Packets pk = packets(path);
            if (!CHECK_EQ(int(pk.pts.size()), kFrames) || !CHECK(pk.timeBase > 0)) continue;
            const qint64 step = std::llround(1.0 / 25 / pk.timeBase); // one frame in the stream's time base
            bool rising = true, shownAfterDecode = true, reordered = false, pyramidOrder = false;
            for (int i = 0; i < pk.dts.size(); ++i) {
                if (i > 0 && (mkv ? pk.dts[i] < pk.dts[i - 1] : pk.dts[i] <= pk.dts[i - 1])) rising = false;
                if (pk.pts[i] < pk.dts[i]) shownAfterDecode = false;
                if (pk.pts[i] != pk.dts[i] + (pk.pts[0] - pk.dts[0])) reordered = true;
                // Two pictures in a row decoded before an earlier-shown one (P4 B2 B1): only the pyramid does that,
                // plain runs fall once (P9 B1 B2 ...). Needs the reorder delay 2 or B1 would be shown before decoded
                if (i >= 2 && pk.pts[i - 2] > pk.pts[i - 1] && pk.pts[i - 1] > pk.pts[i]) pyramidOrder = true;
            }
            if (!CHECK(rising)) qDebug() << path << "dts" << pk.dts;
            if (!CHECK(shownAfterDecode)) qDebug() << path << "pts" << pk.pts << "dts" << pk.dts;
            CHECK_EQ(reordered, withB);
            if (!CHECK_EQ(pyramidOrder, pyramid)) qDebug() << path << "pts" << pk.pts.mid(0, 20) << types.left(30);
            QVector<qint64> sorted = pk.pts;
            std::sort(sorted.begin(), sorted.end());
            bool steps = true;
            for (int i = 1; i < sorted.size(); ++i)
                if (std::llabs(sorted[i] - sorted[i - 1] - step) > 1) steps = false; // (MKV: milliseconds, rounded)
            if (!CHECK(steps)) qDebug() << path << "pts" << sorted;
            const QJsonObject v = probe(path);
            // The stream says how far pictures are reordered (SPS max_num_reorder_frames = the writer's dts shift): 2
            // whenever the pyramid may be used (bframes >= 3, also where the runs come out too long for it), else 0
            CHECK_EQ(v.value("has_b_frames").toInt(-1), withB ? 2 : 0);
            const double duration = v.value("format_duration").toString().toDouble();
            if (!CHECK(std::abs(duration - kFrames / 25.0) < 0.1)) std::printf("       %s: %.3f s\n", qPrintable(path), duration);
            const QString errors = decodeErrors(path);
            if (!CHECK(errors.isEmpty())) std::printf("       %s\n", qPrintable(errors.left(400)));
            // Sound against picture as in the reference export (MP4: edit list; MKV: every stream shifted alike)
            const double offset = pk.videoStart - pk.audioStart;
            if (!CHECK(std::abs(offset - referenceOffset.value(mkv ? "mkv" : "mp4", 1e9)) < 0.002))
                std::printf("       %s: video starts %.4f s, audio %.4f s\n", qPrintable(path), pk.videoStart, pk.audioStart);
            // The picture is the source's, decoded in display order (pictures out of order: far below 30 dB; the grain
            // costs a few dB against the testsrc2 clips)
            const double db = psnr(path, g_zoom, kFrames);
            if (!CHECK(db > 35)) std::printf("       %s: PSNR %.2f dB\n", qPrintable(path), db);
        }
}

// Which cuts force a key frame (Exporter::cutFrames)
void testCutFrames()
{
    const auto cuts = [](const Timeline& tl, int from = 0, int length = kFrames, FrameRate out = {25, 1}) {
        return Exporter::cutFrames(tl, from, length, {25, 1}, out);
    };
    const Timeline ab = sequence({{g_media, 0, 40}, {g_near, 40, 35}});
    CHECK_EQ(cuts(ab), QVector<int>{40});
    CHECK_EQ(cuts(ab, 0, kFrames, {50, 1}), QVector<int>{80});
    CHECK_EQ(cuts(ab, 41, 20), QVector<int>{});
    CHECK_EQ(cuts(ab, 30, 20), QVector<int>{10});
    // A transition into the clip: no hard cut
    Timeline dissolve = ab;
    dissolve.video[0].clips[1].transIn = 10;
    CHECK_EQ(cuts(dissolve), QVector<int>{});
    // Splits (the same file continues) and jump cuts (the same file a bit later): left to the scene detection
    CHECK_EQ(cuts(sequence({{g_media, 0, 20}, {g_media, 20, 20}, {g_media, 40, 35}})), QVector<int>{});
    CHECK_EQ(cuts(sequence({{g_media, 0, 20}, {g_media, 30, 20}, {g_media, 5, 35}})), QVector<int>{});
    // ... but the same file after a gap or after another file is a cut
    Timeline gap = sequence({{g_media, 0, 20}, {g_media, 20, 20}});
    gap.video[0].clips[1].start = 30;
    CHECK_EQ(cuts(gap), QVector<int>{30});
    CHECK_EQ(cuts(sequence({{g_media, 0, 20}, {g_near, 0, 20}, {g_media, 0, 35}})), (QVector<int>{20, 40}));
    // A montage of 2-frame clips: forced key frames at least kOwnMinKeyDistance apart (and from the first picture)
    QVector<Piece> montage;
    for (int i = 0; i < kFrames / 2; ++i) montage << Piece{i % 2 ? g_near : g_media, i * 2, 2};
    const QVector<int> m = cuts(sequence(montage));
    CHECK(!m.isEmpty());
    for (int i = 0; i < m.size(); ++i)
        if (!CHECK(m[i] - (i ? m[i - 1] : 0) >= Exporter::kOwnMinKeyDistance)) qDebug() << "montage" << m;
    // Main track = the lowest visible track with pictures inside the range: track 0's clip outside the range does
    // not make it the main track
    Timeline two = sequence({{g_media, 0, 10}});
    two.video.resize(2);
    two.video[1] = ab.video[0];
    CHECK_EQ(cuts(two, 20, 50), QVector<int>{20});
    CHECK_EQ(cuts(two, 0, kFrames), QVector<int>{}); // (track 0 has a picture in the range: the main track)
    two.video[0].hidden = true;
    CHECK_EQ(cuts(two, 0, kFrames), QVector<int>{40});
}

// What the timeline tells the encoder (Exporter::encodeHints), and a render with it
void testEncodeHints(const QString& dir)
{
    const auto hints = [](const Timeline& tl, QSize format = {640, 360}, QSize out = {640, 360}, FrameRate rate = {25, 1},
                          int from = 0, int length = kFrames) {
        return Exporter::encodeHints(tl, from, length, format, out, {25, 1}, rate);
    };
    // Moving pictures only: nothing to say
    const Timeline moving = sequence({{g_media, 0, 40}, {g_near, 40, 35}});
    CHECK(hints(moving).isEmpty());
    // A freeze frame: its frames probably repeat (also under a title; not under a second moving clip)
    Timeline frozen = moving;
    frozen.video[0].clips[1].freeze = true;
    QVector<ExportHintSpan> h = hints(frozen);
    if (CHECK_EQ(h.size(), 1)) {
        CHECK_EQ(h[0].from, 40);
        CHECK_EQ(h[0].to, kFrames);
        CHECK(h[0].still);
        CHECK(h[0].background.isEmpty());
    }
    h = hints(frozen, {640, 360}, {640, 360}, {50, 1});
    if (CHECK_EQ(h.size(), 1)) {
        CHECK_EQ(h[0].from, 80);
        CHECK_EQ(h[0].to, 2 * kFrames);
    }
    h = hints(frozen, {640, 360}, {640, 360}, {25, 1}, 50, 20); // a range inside the freeze frame
    if (CHECK_EQ(h.size(), 1)) {
        CHECK_EQ(h[0].from, 0);
        CHECK_EQ(h[0].to, 20);
    }
    Timeline titled = frozen;
    titled.video.resize(2);
    Clip title;
    title.id = 9;
    title.kind = ClipKind::Title;
    title.start = 50;
    title.out = 9;
    titled.video[1].clips << title;
    h = hints(titled);
    CHECK(h.size() == 1 && h[0].from == 40 && h[0].to == kFrames && h[0].still);
    titled.video[1].clips[0].kind = ClipKind::Media;
    titled.video[1].clips[0].mediaPath = g_media;
    h = hints(titled); // moving pictures over frames 50..59
    if (CHECK_EQ(h.size(), 2)) {
        CHECK(h[0].from == 40 && h[0].to == 50 && h[1].from == 60 && h[1].to == kFrames);
    }
    // No clip at all (black): still
    Timeline gap = sequence({{g_media, 0, 20}});
    h = hints(gap);
    CHECK(h.size() == 1 && h[0].from == 20 && h[0].to == kFrames && h[0].still);

    // 16:9 pictures fitted into a 9:16 frame with the blurred fill: the bars above and below are background
    Timeline fill = sequence({{g_media, 0, kFrames}});
    Clip& c = fill.video[0].clips[0];
    c.pictureAspect = 16.0 / 9.0;
    c.transform.fill = FitFill::Blur;
    h = hints(fill, {1080, 1920}, {1080, 1920});
    if (CHECK_EQ(h.size(), 1)) {
        CHECK(!h[0].still && h[0].from == 0 && h[0].to == kFrames);
        // The picture is 607.5 high around the middle (656.25 .. 1263.75), a pixel of margin
        if (!CHECK_EQ(h[0].background, (QVector<QRect>{QRect(0, 0, 1080, 655), QRect(0, 1265, 1080, 655)}))) qDebug() << h[0].background;
    }
    h = hints(fill, {1080, 1920}, {540, 960}); // rendered at half the size
    if (CHECK_EQ(h.size(), 1))
        if (!CHECK_EQ(h[0].background, (QVector<QRect>{QRect(0, 0, 540, 327), QRect(0, 633, 540, 327)}))) qDebug() << h[0].background;
    // Zoomed to 0.5 and moved right and up: bars on all four sides
    c.transform.zoomX = c.transform.zoomY = 0.5;
    c.transform.posX = 100;
    c.transform.posY = 200;
    h = hints(fill, {1080, 1920}, {1080, 1920});
    if (CHECK_EQ(h.size(), 1))
        if (!CHECK_EQ(h[0].background, (QVector<QRect>{QRect(0, 0, 1080, 607), QRect(0, 913, 1080, 1007), QRect(0, 607, 369, 306),
                                                        QRect(911, 607, 169, 306)})))
            qDebug() << h[0].background;
    // A place that is animated (a zoom punch): followed frame by frame - at zoom 1 the bars of the picture above,
    // at zoom 2 (1080 x 1215 around the middle) smaller ones, between them one span per frame
    Timeline animated = sequence({{g_media, 0, kFrames}});
    animated.video[0].clips[0].pictureAspect = 16.0 / 9.0;
    animated.video[0].clips[0].transform.fill = FitFill::Blur;
    for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY}) {
        Keys::setKey(animated.video[0].clips[0], p, 10, 1);
        Keys::setKey(animated.video[0].clips[0], p, 20, 2);
    }
    h = hints(animated, {1080, 1920}, {1080, 1920});
    if (CHECK(h.size() > 3)) {
        CHECK(h.first().from == 0 && h.first().to == 11);
        if (!CHECK_EQ(h.first().background, (QVector<QRect>{QRect(0, 0, 1080, 655), QRect(0, 1265, 1080, 655)}))) qDebug() << h.first().background;
        CHECK(h.last().from == 20 && h.last().to == kFrames);
        if (!CHECK_EQ(h.last().background, (QVector<QRect>{QRect(0, 0, 1080, 351), QRect(0, 1569, 1080, 351)}))) qDebug() << h.last().background;
        for (qsizetype i = 1; i < h.size(); ++i) CHECK(h[i].from == h[i - 1].to);
    }
    // None while the picture is rotated, where the fill is off, or where a clip without the fill shows as well
    Timeline rotated = fill;
    rotated.video[0].clips[0].transform.rotation = 5;
    CHECK(hints(rotated, {1080, 1920}, {1080, 1920}).isEmpty());
    Timeline off = fill;
    off.video[0].clips[0].transform.transformOn = false;
    CHECK(hints(off, {1080, 1920}, {1080, 1920}).isEmpty());
    Timeline over = fill;
    over.video.resize(2);
    over.video[1].clips << moving.video[0].clips[0];
    over.video[1].clips[0].id = 7;
    h = hints(over, {1080, 1920}, {1080, 1920}); // frames 0..39 covered by the plain clip
    CHECK(h.size() == 1 && h[0].from == 40 && h[0].to == kFrames && !h[0].background.isEmpty());

    // Rendered: a clip, then its freeze frame, with the hints and without ("hints=0"). Both decode, the same
    // length; the freeze frame's pictures are found as repeats, so the hinted file is not larger
    {
        Timeline tl = timeline(g_media, 30);
        tl.video[0].clips[1].freeze = true;
        const QString with = dir + "/hints.mp4", without = dir + "/nohints.mp4";
        ExportSettings s = settings(with);
        if (CHECK(render(tl, s))) {
            s = settings(without);
            s.codecOptions << "hints=0";
            if (CHECK(render(tl, s))) {
                for (const QString& path : {with, without}) {
                    CHECK_EQ(probe(path).value("nb_read_frames").toString().toInt(), kFrames);
                    const QString errors = decodeErrors(path);
                    if (!CHECK(errors.isEmpty())) std::printf("       %s\n", qPrintable(errors.left(400)));
                    const double db = psnr(path, g_media, 30);
                    if (!CHECK(db > 38)) std::printf("       PSNR %.2f dB\n", db);
                }
                if (!CHECK(QFileInfo(with).size() <= QFileInfo(without).size()))
                    std::printf("       with hints %lld bytes, without %lld\n", static_cast<long long>(QFileInfo(with).size()),
                                static_cast<long long>(QFileInfo(without).size()));
            }
        }
    }
}

// Rendered: splits, jump cuts and a montage of 2-frame clips make no row of key frames
void testNoKeyStorm(const QString& dir)
{
    QVector<Piece> splits;
    for (int i = 0; i < kFrames / 3; ++i) splits << Piece{g_media, i * 3, 3};
    const QString a = dir + "/splits.mp4";
    if (CHECK(render(sequence(splits), settings(a)))) {
        const QVector<int> keys = keyFrames(a);
        if (!CHECK_EQ(keys, QVector<int>{0})) qDebug() << "splits: key frames" << keys;
    }
    QVector<Piece> montage;
    for (int i = 0; i < kFrames / 2; ++i) montage << Piece{i % 2 ? g_near : g_media, i * 2, 2};
    const QString b = dir + "/montage.mp4";
    if (CHECK(render(sequence(montage), settings(b)))) {
        const QVector<int> keys = keyFrames(b);
        CHECK(keys.size() <= kFrames / Exporter::kOwnMinKeyDistance + 1);
        for (int i = 1; i < keys.size(); ++i)
            if (!CHECK(keys[i] - keys[i - 1] >= Exporter::kOwnMinKeyDistance)) qDebug() << "montage: key frames" << keys;
        CHECK(decodeErrors(b).isEmpty());
    }
}

// Shorter than the lookahead (40 pictures), in both modes; a size that is no multiple of 16
void testShortAndOdd(const QString& dir)
{
    for (int quality : {0, int(RenderSettings::kQualityWeb)}) {
        const QString path = dir + QString("/short%1.mp4").arg(quality);
        ExportSettings s = settings(path, quality);
        s.audioCodec.clear();
        if (!CHECK(render(sequence({{g_media, 0, 12}}), s))) continue;
        const QJsonObject v = probe(path);
        if (!CHECK_EQ(v.value("nb_read_frames").toString().toInt(), 12)) std::printf("       quality %d\n", quality);
        CHECK(decodeErrors(path).isEmpty());
        CHECK(psnr(path, g_media, 12) > 38);
    }
    const ProjectFormat odd{650, 362, {25, 1}};
    const QString path = dir + "/odd.mkv";
    ExportSettings s = settings(path, 0, odd);
    s.audioCodec.clear();
    if (!CHECK(render(sequence({{g_media, 0, 30}}), s))) return;
    const QJsonObject v = probe(path);
    CHECK_EQ(v.value("width").toInt(), 650);
    CHECK_EQ(v.value("height").toInt(), 362);
    CHECK_EQ(v.value("nb_read_frames").toString().toInt(), 30);
    const QString errors = decodeErrors(path);
    if (!CHECK(errors.isEmpty())) std::printf("       %s\n", qPrintable(errors.left(400)));
}

// Low memory: the shorter lookahead (Exporter::lowMemoryOptions, with at most 3 B pictures so the encoder keeps it
// at 10, and no kept first-pass analysis) reaches the encoder - the macroblock tree then sees fewer pictures ahead
// and the stream differs; two passes towards a cap without the kept analysis still work
void testLookahead(const QString& dir)
{
    const auto o = Exporter::lowMemoryOptions(ExportWriter::kOwnH264);
    CHECK(o.contains({QByteArray("lookahead"), QByteArray("10")}));
    CHECK(o.contains({QByteArray("bframes"), QByteArray("3")}));
    CHECK(o.contains({QByteArray("analysis_mb"), QByteArray("0")}));
    {
        const QString path = dir + "/lowmem-cap.mp4";
        ExportSettings s = settings(path, RenderSettings::kQualityWeb);
        s.maxBitrateK = 1500;
        s.audioCodec.clear();
        for (const auto& [key, value] : o) s.codecOptions << QString::fromUtf8(key + "=" + value);
        if (CHECK(render(timeline(g_noisy), s))) {
            CHECK_EQ(probe(path).value("nb_read_frames").toString().toInt(), kFrames);
            const QString errors = decodeErrors(path);
            if (!CHECK(errors.isEmpty())) std::printf("       %s\n", qPrintable(errors.left(400)));
        }
    }
    QByteArray streams[2];
    for (int i = 0; i < 2; ++i) {
        const QString path = dir + QString("/lookahead%1.mkv").arg(i);
        ExportSettings s = settings(path);
        s.audioCodec.clear();
        if (i)
            for (const auto& [key, value] : o) s.codecOptions << QString::fromUtf8(key + "=" + value);
        if (!CHECK(render(timeline(g_media), s))) return;
        CHECK_EQ(probe(path).value("nb_read_frames").toString().toInt(), kFrames);
        QProcess p;
        p.start("ffmpeg", {"-nostdin", "-v", "error", "-i", path, "-map", "0:v", "-c", "copy", "-f", "h264", "-"});
        p.waitForFinished(60000);
        streams[i] = p.readAllStandardOutput();
    }
    CHECK(!streams[0].isEmpty() && !streams[1].isEmpty());
    CHECK(streams[0] != streams[1]);
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
    // The level covers the bit rate (levelIdcFor: 1.5x the target as the peak)
    CHECK_EQ(spsFields(path).value("level_idc", -1), expectedLevel(1500));
    CHECK_EQ(v.value("level").toInt(), expectedLevel(1500));
    // A still picture is far below the cap at the "high" level's QP: kept, not filled up to the cap (like x264's
    // CRF under a VBV cap); the level then is the one of the fixed QP
    const QString still = dir + "/still.mp4";
    s.path = still;
    if (!CHECK(render(timeline(g_still), s))) return;
    const double stillKbit = QFileInfo(still).size() * 8.0 / 1000 / (kFrames / 25.0);
    if (!CHECK(stillKbit < 1500 * 0.5)) std::printf("       still: %.0f kbit/s for the cap 1500\n", stillKbit);
    CHECK_EQ(probe(still).value("nb_read_frames").toString().toInt(), kFrames);
    CHECK(decodeErrors(still).isEmpty());
    CHECK(psnr(still, g_still, kFrames) > 40);
    CHECK_EQ(spsFields(still).value("level_idc", -1), expectedLevel(0));
}

// A cap below the clip's rate at the quality level's QP is met (the fixed-QP prediction of the first pass used to take
// a calm picture pair as the whole clip's measure: 2.6-7x too low on real projects, the cap then exceeded by 60 %).
// Moving noise with two equal still pictures at the start, a third and two thirds in (where the old pair probes sat).
void testCapBelowFixedRate(const QString& dir)
{
    const Timeline tl = sequence({{g_still, 0, 2}, {g_noisy, 2, 23}, {g_still, 0, 2}, {g_noisy, 27, 23}, {g_still, 0, 2},
                                  {g_noisy, 52, 23}});
    const auto kbitOf = [](const QString& path) { return QFileInfo(path).size() * 8.0 / 1000 / (kFrames / 25.0); };
    // The rate at the "web" level's QP: a cap far above it keeps the fixed QP
    const QString fixed = dir + "/cap-fixed.mp4";
    ExportSettings s = settings(fixed, RenderSettings::kQualityWeb);
    s.maxBitrateK = 200000;
    s.audioCodec.clear();
    if (!CHECK(render(tl, s))) return;
    const double fixedKbit = kbitOf(fixed);
    // Half of it as the cap: met (two passes, or the fixed QP held by the VBV)
    const QString capped = dir + "/cap-half.mp4";
    s.path = capped;
    s.maxBitrateK = int(fixedKbit / 2);
    if (!CHECK(render(tl, s))) return;
    const double kbit = kbitOf(capped);
    if (!CHECK(kbit < s.maxBitrateK * 1.1 && kbit > s.maxBitrateK * 0.7))
        std::printf("       %.0f kbit/s for the cap %d (fixed QP: %.0f)\n", kbit, s.maxBitrateK, fixedKbit);
    CHECK_EQ(probe(capped).value("nb_read_frames").toString().toInt(), kFrames);
    CHECK(decodeErrors(capped).isEmpty());
}

void testRefused(const QString& dir)
{
    const auto refused = [&](ExportSettings s, const QString& reason) {
        QString message;
        CHECK(!render(timeline(g_media), s, &message));
        if (!CHECK(!message.isEmpty() && (reason.isEmpty() || message.contains(reason))))
            std::printf("       %s: %s\n", qPrintable(s.path), qPrintable(message));
        if (!CHECK(!QFile::exists(s.path))) std::printf("       %s left behind\n", qPrintable(s.path));
    };
    ExportSettings s = settings(dir + "/deep.mp4");
    s.pixFmt = "yuv420p10le";
    refused(s, "8-bit 4:2:0");
    s = settings(dir + "/full.mp4");
    s.pixFmt = "yuv444p";
    refused(s, "8-bit 4:2:0");
    refused(settings(dir + "/x.webm"), ""); // no H.264 in WebM
    // The writer refuses on its own too (a caller that skips supportsOwn)
    ExportWriter::Settings w;
    w.path = dir + "/writer.mp4";
    w.videoCodec = ExportWriter::kOwnH264;
    w.pixFmt = "yuv444p";
    w.length = 5;
    ExportWriter::Own own;
    own.size = QSize(64, 64);
    own.rate = own.timelineRate = {25, 1};
    {
        ExportWriter writer(w, own);
        writer.start();
        QElapsedTimer t;
        t.start();
        while (writer.state() == ExportWriter::State::Running && t.elapsed() < 10000) QThread::msleep(10);
        CHECK(writer.state() == ExportWriter::State::Failed);
        if (!CHECK(writer.error().contains("8-bit 4:2:0"))) std::printf("       %s\n", qPrintable(writer.error()));
    }
    QFile::remove(w.path); // (the writer leaves removing to its caller)
}

// Sizes: a tiny picture is refused with a reason (no file), the smallest allowed one is written; above the highest
// H.264 level (6.2: 8K at 120 fps at most) refused instead of a stream that breaks the level it claims
void testSizes(const QString& dir)
{
    CHECK(!ExportWriter::ownSizeProblem(QSize(16, 16), 25).isEmpty());
    CHECK(!ExportWriter::ownSizeProblem(QSize(640, 30), 25).isEmpty());
    CHECK(ExportWriter::ownSizeProblem(QSize(32, 32), 25).isEmpty());
    CHECK(ExportWriter::ownSizeProblem(QSize(1920, 1080), 60).isEmpty());
    CHECK(ExportWriter::ownSizeProblem(QSize(7680, 4320), 120).isEmpty());
    CHECK(ExportWriter::ownSizeProblem(QSize(8192, 4320), 60).isEmpty());
    const QString fast = ExportWriter::ownSizeProblem(QSize(7680, 4320), 144);
    if (!CHECK(fast.contains("level"))) std::printf("       %s\n", qPrintable(fast));
    CHECK(!ExportWriter::ownSizeProblem(QSize(15360, 8640), 25).isEmpty()); // 16K
    CHECK(!ExportWriter::ownSizeProblem(QSize(16896, 512), 25).isEmpty());  // wider than 6.2 allows (1056 macroblocks)
    // The level fits where the encoder finds one (the export's defaults)
    for (const auto& [w, h, fps] : {std::tuple{7680, 4320, 120.0}, std::tuple{3840, 2160, 60.0}, std::tuple{640, 360, 25.0}}) {
        schneidi::h264::EncoderConfig c;
        c.width = w;
        c.height = h;
        c.fps = fps;
        CHECK(schneidi::h264::levelIdcFor(c) <= 62);
    }
    // A tiny export: refused by the writer, before the first pass, no file left
    {
        const QString path = dir + "/tiny.mp4";
        ExportSettings s = settings(path, 0, ProjectFormat{16, 16, {25, 1}});
        s.audioCodec.clear();
        QString message;
        CHECK(!render(sequence({{g_media, 0, 5}}), s, &message));
        if (!CHECK(message.contains("32x32"))) std::printf("       %s\n", qPrintable(message));
        CHECK(!QFile::exists(path));
    }
    // The smallest allowed: written, decodes cleanly
    {
        const QString path = dir + "/small.mp4";
        ExportSettings s = settings(path, 0, ProjectFormat{32, 32, {25, 1}});
        s.audioCodec.clear();
        if (CHECK(render(sequence({{g_media, 0, 10}}), s))) {
            const QJsonObject v = probe(path);
            CHECK_EQ(v.value("width").toInt(), 32);
            CHECK_EQ(v.value("height").toInt(), 32);
            CHECK_EQ(v.value("nb_read_frames").toString().toInt(), 10);
            CHECK(decodeErrors(path).isEmpty());
        }
    }
}

// A review render (marks drawn on, "ultrafast") with the own encoder: plain H.264 instead (the own encoder has no fast
// setting and would ignore the preset)
void testReview()
{
    RenderJob j;
    j.id = 1;
    j.path = "/nowhere/review.mp4";
    j.settings.format = "s264";
    j.review = true;
    const ExportSettings s = RenderQueue::exportSettings(j, kFmt);
    CHECK(!ExportWriter::isOwnH264(s.videoCodec));
    CHECK_EQ(s.videoCodec, HardwareEncoders::resolve(VideoEncoder::Software, "h264").codec);
    CHECK_EQ(s.preset, QString("ultrafast"));
    CHECK(!s.smartRender);
    j.review = false;
    CHECK(ExportWriter::isOwnH264(RenderQueue::exportSettings(j, kFmt).videoCodec));
    // Quality "Like source": the review still small and fast (a constant quality, not the recordings' bit rate)
    ExportSettings like;
    like.sourceQuality = true;
    like.crf = 18;
    RenderQueue::makeReview(like);
    CHECK(!like.sourceQuality);
    CHECK(like.crf >= 28);
}

// Cancel while encoding and during the first pass of the "web" mode: a long timeline (60 s of noise), so neither is
// over before the first progress report (Exporter polls every 200 ms)
void testCancel(const QString& dir)
{
    const QString sub = dir + "/cancel";
    QDir().mkpath(sub);
    QVector<Piece> pieces;
    for (int i = 0; i < 20; ++i) pieces << Piece{g_noisy, 0, kFrames};
    const Timeline tl = sequence(pieces);
    for (int quality : {0, int(RenderSettings::kQualityWeb)}) {
        ExportSettings s = settings(sub + "/c.mp4", quality);
        s.audioCodec.clear();
        Exporter ex;
        QEventLoop loop;
        bool finished = false, ok = true, canceled = false;
        int at = -1, writtenAt = -1;
        QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
            finished = true;
            ok = success;
            loop.quit();
        });
        QObject::connect(&ex, &Exporter::progress, &ex, [&](int percent) {
            if (canceled) return;
            // Quality: once encoding has begun; web: at once (the first pass)
            if (quality == 0 && ex.framesWritten() == 0) return;
            at = percent;
            writtenAt = ex.framesWritten();
            canceled = true;
            ex.cancel();
        });
        QString error;
        if (!CHECK(ex.start(tl, s, &error))) continue;
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        if (!finished) loop.exec();
        CHECK(canceled);
        CHECK(finished);
        CHECK(!ok);
        CHECK(!ex.isRunning());
        if (quality == 0) CHECK(writtenAt > 0 && writtenAt < int(pieces.size()) * kFrames);
        // Web: still in the first pass (it counts for 15 %, nothing handed to the encoder yet)
        if (quality > 0 && !CHECK(at < 15 && writtenAt == 0)) std::printf("       canceled at %d %%, %d written\n", at, writtenAt);
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
    // Near-identical to g_media (a touch brighter): a cut between them is no scene change
    g_near = Check::makeMedia(tmp.filePath("near.mkv"),
                              {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=3", "-vf",
                               "eq=brightness=0.02,scale=out_color_matrix=bt709", "-colorspace", "bt709",
                               "-color_primaries", "bt709", "-color_trc", "bt709", "-c:v", "ffv1", "-pix_fmt", "yuv420p"});
    g_still = Check::makeMedia(tmp.filePath("still.mkv"), {"-f", "lavfi", "-i", "smptebars=size=640x360:rate=25:duration=3",
                                                           "-c:v", "ffv1", "-pix_fmt", "yuv420p"});
    // A slow zoom into a still picture with grain: the adaptive B picture placement takes B pictures there
    // (testBFrames), far from its threshold (probe gain about -0.5 against -0.15, on the CPU and the GPU picture
    // engine alike; a mandelbrot zoom sat at -0.167 on the CPU, -0.149 on the GPU = no B pictures there)
    g_zoom = Check::makeMedia(tmp.filePath("zoom.mkv"),
                              {"-f", "lavfi", "-i",
                               "testsrc2=size=640x360:rate=25:duration=1,trim=end_frame=1,loop=loop=-1:size=1,"
                               "zoompan=z='1+0.002*on':d=1:s=640x360:fps=25,noise=alls=5:allf=t",
                               "-t", "3", "-c:v", "ffv1", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_media.isEmpty() && !g_near.isEmpty() && !g_noisy.isEmpty() && !g_still.isEmpty() && !g_zoom.isEmpty()))
        return Check::result();
    testContainers(tmp.path());
    testBFrames(tmp.path());
    testSpeedOption(tmp.path());
    testCutFrames();
    testEncodeHints(tmp.path());
    testNoKeyStorm(tmp.path());
    testShortAndOdd(tmp.path());
    testLookahead(tmp.path());
    testBitrate(tmp.path());
    testCapBelowFixedRate(tmp.path());
    testRefused(tmp.path());
    testSizes(tmp.path());
    testReview();
    testCancel(tmp.path());
    return Check::result();
}
