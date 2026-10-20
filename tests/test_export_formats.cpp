// Export formats of the Deliver page: MP3, AIFF, Apple Lossless, FLAC, Opus, bit depths (16/24/32 float), AV1, VP9,
// ProRes/DNxHR profiles, animated GIF, image sequences and a different frame rate than the timeline (frames converted,
// length correct, audio without jumps); BT.601 sources converted exactly to BT.709. Checked with ffprobe/ffmpeg.

#include "check.h"

#include "core/RenderJob.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <array>
#include <clocale>
#include <cmath>
#include <tuple>
#include <vector>

namespace {

const ProjectFormat kFmt{640, 360, {25, 1}};
constexpr int kFrames = 100; // 4 s
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

bool render(const RenderJob& job)
{
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QString message;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        message = msg;
        loop.quit();
    });
    QString error;
    if (!CHECK(ex.start(timeline(), RenderQueue::exportSettings(job, kFmt), &error))) {
        std::printf("       %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    // Rare failure on CI (2026-10-10, not reproducible locally): say which export it was
    if (!ok) std::printf("       %s: %s\n", qPrintable(job.path), qPrintable(message.isEmpty() ? "timeout" : message));
    return CHECK(ok);
}

// first stream of the kind ("audio"/"video") according to ffprobe
QJsonObject probe(const QString& path, const char* type)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-show_streams", "-show_format", "-of", "json", path});
    p.waitForFinished(30000);
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    for (const QJsonValue& v : o.value("streams").toArray())
        if (v.toObject().value("codec_type").toString() == QLatin1String(type)) {
            QJsonObject s = v.toObject();
            s["format_duration"] = o.value("format").toObject().value("duration");
            return s;
        }
    return {};
}

RenderJob job(const QString& path, const QString& format, int bits = 24)
{
    RenderJob j;
    j.id = 1;
    j.path = path;
    j.settings.format = format;
    j.settings.audioBits = bits;
    return j;
}

void testAudioFormats(const QString& dir)
{
    struct Case { const char* format; int bits; const char* file; const char* codec; const char* sampleFmt; };
    const Case cases[] = {
        {"mp3", 24, "a.mp3", "mp3", nullptr},
        {"wav", 16, "a16.wav", "pcm_s16le", nullptr},
        {"wav", 24, "a24.wav", "pcm_s24le", nullptr},
        {"wav", 32, "a32.wav", "pcm_f32le", nullptr},
        {"aiff", 24, "a24.aiff", "pcm_s24be", nullptr},
        {"aiff", 32, "a32.aiff", "pcm_f32be", nullptr},
        {"alac", 16, "a16.m4a", "alac", "s16p"},
        {"alac", 24, "a24.m4a", "alac", "s32p"},
        {"prores", 32, "p32.mov", "pcm_f32le", nullptr},
        {"flac", 16, "a16.flac", "flac", "s16"},
        {"flac", 24, "a24.flac", "flac", "s32"},
        {"opus", 24, "a.opus", "opus", nullptr},
    };
    for (const Case& c : cases) {
        const QString path = dir + "/" + c.file;
        if (!render(job(path, c.format, c.bits))) continue;
        const QJsonObject a = probe(path, "audio");
        if (!CHECK_EQ(a.value("codec_name").toString(), QString(c.codec))) std::printf("       %s\n", c.file);
        if (c.sampleFmt) CHECK_EQ(a.value("sample_fmt").toString(), QString(c.sampleFmt));
        CHECK(std::abs(a.value("format_duration").toString().toDouble() - 4.0) < 0.1);
    }
    // MP3: bit rate as chosen
    RenderJob mp3 = job(dir + "/b.mp3", "mp3");
    mp3.settings.audioBitrateK = 192;
    if (render(mp3)) CHECK_EQ(probe(mp3.path, "audio").value("bit_rate").toString(), QString("192000"));
}

// largest jump in the audio (second derivative, mono 48 kHz) from 0.05 s - sine 440 Hz stays far below
int maxJump(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-i", path, "-map", "0:a", "-f", "s16le", "-ac", "1", "-ar", "48000", "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* s = reinterpret_cast<const qint16*>(d.constData());
    const int n = int(d.size() / 2);
    int jump = 0;
    for (int i = 2400; i < n - 2400; ++i) {
        const int j = std::abs(s[i] - 2 * s[i - 1] + s[i - 2]);
        if (j > 300 && qEnvironmentVariableIsSet("JUMPS")) std::printf("Sprung %d bei %.4f s\n", j, i / 48000.0);
        jump = std::max(jump, j);
    }
    return n > 4800 ? jump : 99999;
}

void testFrameRate(const QString& dir)
{
    RenderJob j = job(dir + "/r30.mov", "prores");
    j.settings.rateNum = 30;
    j.settings.rateDen = 1;
    if (render(j)) {
        const QJsonObject v = probe(j.path, "video");
        CHECK_EQ(v.value("r_frame_rate").toString(), QString("30/1"));
        CHECK_EQ(v.value("nb_frames").toString(), QString("120"));
        const double dur = probe(j.path, "audio").value("duration").toString().toDouble();
        if (!CHECK(std::abs(dur - 4.0) < 0.05)) std::printf("       Ton %.3f s\n", dur);
        const int jump = maxJump(j.path);
        if (!CHECK(jump < 300)) std::printf("       Sprung %d\n", jump);
    }
    // In/Out range (frames 25-74 = 2 s) at 50 fps -> 100 frames
    RenderJob r = job(dir + "/r50.mov", "prores");
    r.settings.rateNum = 50;
    r.inOut = true;
    r.from = 25;
    r.to = 74;
    if (render(r)) {
        CHECK_EQ(probe(r.path, "video").value("nb_frames").toString(), QString("100"));
        CHECK(maxJump(r.path) < 300);
    }
    // same frame rate as the timeline: normal path
    RenderJob same = job(dir + "/r25.mov", "prores");
    same.settings.rateNum = 25;
    if (render(same)) CHECK_EQ(probe(same.path, "video").value("nb_frames").toString(), QString("100"));
}

// ffprobe -count_frames: frames really in the file (GIF has no nb_frames)
int countFrames(const QString& path)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
                        "stream=nb_read_frames", "-of", "default=nw=1:nk=1", path});
    p.waitForFinished(60000);
    return p.readAllStandardOutput().trimmed().toInt();
}

void testVideoFormats(const QString& dir)
{
    struct Case { const char* format; const char* file; const char* codec; const char* profile; const char* pixFmt; const char* audio; };
    const Case cases[] = {
        {"av1", "v.av1.mp4", "av1", nullptr, "yuv420p", "aac"},
        {"vp9", "v.webm", "vp9", nullptr, "yuv420p", "opus"},
        {"prores", "hq.mov", "prores", "HQ", "yuv422p10le", "pcm_s24le"},
        {"prores422", "std.mov", "prores", "Standard", "yuv422p10le", "pcm_s24le"},
        {"proreslt", "lt.mov", "prores", "LT", "yuv422p10le", "pcm_s24le"},
        {"proresproxy", "proxy.mov", "prores", "Proxy", "yuv422p10le", "pcm_s24le"},
        {"prores4444", "4444.mov", "prores", "4444", "yuv444p12le", "pcm_s24le"}, // decoder reports 4444 as 12 bit
        {"dnxhr-sq", "sq.mov", "dnxhd", "DNXHR SQ", "yuv422p", "pcm_s24le"},
        {"dnxhr-hq", "dhq.mov", "dnxhd", "DNXHR HQ", "yuv422p", "pcm_s24le"},
        {"dnxhr-hqx", "hqx.mov", "dnxhd", "DNXHR HQX", "yuv422p10le", "pcm_s24le"},
    };
    for (const Case& c : cases) {
        const QString path = dir + "/" + c.file;
        if (!render(job(path, c.format))) continue;
        const QJsonObject v = probe(path, "video");
        bool ok = CHECK_EQ(v.value("codec_name").toString(), QString(c.codec));
        if (c.profile) ok = CHECK_EQ(v.value("profile").toString(), QString(c.profile)) && ok;
        ok = CHECK_EQ(v.value("pix_fmt").toString(), QString(c.pixFmt)) && ok;
        ok = CHECK_EQ(probe(path, "audio").value("codec_name").toString(), QString(c.audio)) && ok;
        ok = CHECK_EQ(countFrames(path), kFrames) && ok;
        if (!ok) std::printf("       %s\n", c.file);
    }
    // Quality levels take effect (CRF per format): "small" is clearly smaller than "high"
    for (const char* format : {"av1", "vp9"}) {
        const QString ext = renderFormat(format).extension;
        RenderJob high = job(dir + "/q0-" + format + "." + ext, format), small = job(dir + "/q2-" + format + "." + ext, format);
        small.settings.quality = 2;
        if (render(high) && render(small))
            if (!CHECK(QFileInfo(small.path).size() * 3 < QFileInfo(high.path).size() * 2))
                std::printf("       %s: %lld vs %lld\n", format, QFileInfo(small.path).size(), QFileInfo(high.path).size());
    }
}

// Frame n as 8-bit 4:4:4 Y'CbCr planes (ffmpeg only resamples, the matrix stays)
QByteArray frame444(const QString& path, int n)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-i", path, "-vf", QString("select=eq(n\\,%1)").arg(n), "-frames:v", "1", "-f",
                       "rawvideo", "-pix_fmt", "yuv444p", "-"});
    p.waitForFinished(60000);
    return p.readAllStandardOutput();
}

// BT.601 source (small, unmarked: MLT takes it for 601) into BT.709 exports: colors converted exactly, in every path
// of the workers' conversion (yuv420p, packed 4:2:2, 10-bit 4:4:4; OutputColor)
void testColorStandard(const QString& dir)
{
    const int n = 50, size = kFmt.width * kFmt.height;
    const QByteArray src = frame444(g_media, n);
    if (!CHECK_EQ(src.size(), 3 * size)) return;
    // Exact 601 -> 709 per pixel (via R'G'B', limited range)
    auto convert = [](double y, double cb, double cr, std::array<double, 3>& out) {
        const double yy = (y - 16) / 219, pb = (cb - 128) / 224, pr = (cr - 128) / 224;
        const double r = yy + 1.402 * pr, b = yy + 1.772 * pb, g = (yy - 0.299 * r - 0.114 * b) / 0.587;
        const double y2 = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        out = {16 + 219 * y2, 128 + 224 * (b - y2) / 1.8556, 128 + 224 * (r - y2) / 1.5748};
    };
    for (const char* file : {"c.mp4", "hq.mov", "4444.mov"}) {
        const QString path = dir + "/" + file;
        if (QString(file) == "c.mp4" && !render(job(path, "h264"))) continue;
        if (!CHECK_EQ(probe(path, "video").value("color_space").toString(), QString("bt709"))) continue;
        const QByteArray out = frame444(path, n);
        if (!CHECK_EQ(out.size(), 3 * size)) continue;
        // Compared as 16x16 block means: chroma resampling (4:2:0 -> 4:2:2 -> 4:4:4 vs 4:2:0 -> 4:4:4) differs at
        // testsrc2's sharp edges, a wrong matrix shifts whole areas
        constexpr int kBlock = 16;
        const int bw = kFmt.width / kBlock, bh = kFmt.height / kBlock;
        std::vector<std::array<double, 3>> got(size_t(bw) * bh), want(got.size());
        for (int y = 0; y < bh * kBlock; ++y)
            for (int x = 0; x < bw * kBlock; ++x) {
                const int i = y * kFmt.width + x;
                const size_t bi = size_t(y / kBlock) * bw + x / kBlock;
                std::array<double, 3> w;
                convert(uint8_t(src[i]), uint8_t(src[size + i]), uint8_t(src[2 * size + i]), w);
                for (int c = 0; c < 3; ++c) {
                    got[bi][c] += uint8_t(out[c * size + i]);
                    want[bi][c] += std::clamp(w[c], 0.0, 255.0);
                }
            }
        std::array<double, 3> bias{}, sq{};
        const double perBlock = kBlock * kBlock;
        for (size_t bi = 0; bi < got.size(); ++bi)
            for (int c = 0; c < 3; ++c) {
                const double d = (got[bi][c] - want[bi][c]) / perBlock;
                bias[c] += d;
                sq[c] += d * d;
            }
        // Lossy codecs and MLT's 4:2:0 -> 4:2:2 chroma: rms ~1, no shift. Without the matrix (601 values as 709)
        // rms is 6-18; the encoder thread's swscale path (via RGB) shifted luma by -0.5
        for (int c = 0; c < 3; ++c) {
            const double b = bias[c] / got.size(), rms = std::sqrt(sq[c] / got.size());
            if (!CHECK(std::abs(b) < 0.3 && rms < 1.5))
                std::printf("       %s channel %d: bias %+.3f, rms %.3f\n", file, c, b, rms);
        }
    }
}

void testGif(const QString& dir)
{
    RenderJob j = job(dir + "/anim.gif", "gif");
    j.settings.shortSide = 180;
    j.settings.rateNum = 10;
    j.settings.rateDen = 1;
    j.size = j.settings.outputSize(kFmt.size());
    if (!render(j)) return;
    const QJsonObject v = probe(j.path, "video");
    CHECK_EQ(v.value("codec_name").toString(), QString("gif"));
    CHECK_EQ(v.value("width").toInt(), 320);
    CHECK_EQ(v.value("height").toInt(), 180);
    CHECK(probe(j.path, "audio").isEmpty());
    CHECK_EQ(countFrames(j.path), 40); // 4 s at 10 fps
    CHECK(!QFileInfo::exists(j.path + ".part"));
    // the intermediate in the temp folder is gone again
    CHECK(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).entryList({"schneidi-gif-*"}).isEmpty());
}

void testImageSequence(const QString& dir)
{
    RenderJob j = job(dir + "/seq.png", "png");
    if (render(j)) {
        const QDir frames(dir + "/seq");
        CHECK_EQ(frames.entryList({"seq_*.png"}, QDir::Files).size(), qsizetype(kFrames));
        CHECK(frames.exists("seq_00001.png"));
        CHECK(frames.exists("seq_00100.png"));
        CHECK(!QFileInfo::exists(j.path)); // no file of the plain name
        const QJsonObject v = probe(frames.filePath("seq_00001.png"), "video");
        CHECK_EQ(v.value("codec_name").toString(), QString("png"));
        CHECK_EQ(v.value("width").toInt(), 640);
        CHECK_EQ(firstOutputFile(j.path, j.settings), frames.filePath("seq_00001.png"));
    }
    for (const auto& [format, file, codec] : {std::tuple{"jpeg", "j.jpg", "mjpeg"}, std::tuple{"tiff", "t.tif", "tiff"}}) {
        RenderJob k = job(dir + "/" + file, format);
        k.inOut = true; // 10 frames are enough
        k.from = 0;
        k.to = 9;
        if (!render(k)) continue;
        const QString first = firstOutputFile(k.path, k.settings);
        CHECK_EQ(probe(first, "video").value("codec_name").toString(), QString(codec));
        CHECK_EQ(QDir(imageSequenceDir(k.path)).entryList(QDir::Files).size(), qsizetype(10));
    }
}

void testSettingsJson()
{
    RenderSettings s;
    s.format = "aiff";
    s.audioBits = 32;
    s.rateNum = 30000;
    s.rateDen = 1001;
    const RenderSettings back = RenderSettings::fromJson(s.toJson());
    CHECK(back == s);
    CHECK_EQ(back.audioCodec(), QString("pcm_f32be"));
    RenderSettings alac;
    alac.format = "alac";
    alac.audioBits = 32; // does not exist for Apple Lossless -> 24 bit
    CHECK_EQ(alac.audioSampleFormat(), QString("s32p"));
    RenderSettings audio;
    audio.format = "wav";
    audio.rateNum = 50;
    CHECK(audio.outputRate(kFmt.rate) == kFmt.rate); // audio only: frame rate irrelevant

    // Quality per format; "web" (bit rate cap) only where it exists
    RenderSettings av1;
    av1.format = "av1";
    CHECK_EQ(av1.crf(), 28);
    av1.quality = RenderSettings::kQualityWeb;
    CHECK_EQ(av1.crf(), 28);
    CHECK_EQ(av1.maxBitrateK(QSize(1920, 1080), {25, 1}), 0);
    CHECK(!av1.hasEncoderChoice());
    RenderSettings h264;
    h264.quality = RenderSettings::kQualityWeb;
    CHECK(h264.maxBitrateK(QSize(1920, 1080), {25, 1}) > 0);
    CHECK(h264.hasEncoderChoice());
    RenderSettings gif;
    gif.format = "gif";
    CHECK(!gif.hasAudio() && !gif.audioOnly() && gif.kind() == RenderKind::Gif);
    CHECK(gif.audioBitChoices().isEmpty());
    RenderSettings opus;
    opus.format = "opus";
    CHECK(opus.hasAudioBitrate() && opus.audioOnly());
    RenderSettings flac;
    flac.format = "flac";
    CHECK(flac.audioBitChoices() == QVector<int>({16, 24}));
    CHECK_EQ(imageSequencePattern("/x/a b.png"), QString("/x/a b/a b_%05d.png"));
    const FrameRate fps15{15, 1}, fps25{25, 1};
    CHECK(nearestExportRate(15) == fps15);
    CHECK(nearestExportRate(24.9) == fps25);
    // every format id is unique and found again
    QStringList ids;
    for (const RenderFormatInfo& f : renderFormats()) {
        CHECK(!ids.contains(f.id));
        ids << f.id;
        CHECK_EQ(QString(renderFormat(f.id).id), QString(f.id));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_formats");
    testSettingsJson();
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4", "-f", "lavfi", "-i",
                                "sine=frequency=440:duration=4", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                "-shortest"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    testAudioFormats(tmp.path());
    testVideoFormats(tmp.path());
    testColorStandard(tmp.path());
    testGif(tmp.path());
    testImageSequence(tmp.path());
    testFrameRate(tmp.path());
    return Check::result();
}
