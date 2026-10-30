// SPDX-License-Identifier: AGPL-3.0-only
// Export timing with the own picture engine (engine/ExportWriter::Own): every output frame shows the timeline frame it
// should, also with In/Out and another output frame rate (frame k shows timeline frame from + round(k * 25 / 30), as
// engine/RateConverter did), the file has exactly that many frames, and the sound stays in sync with the picture (a
// click at timeline frame 20 is at 0.8 s in the file, whatever its frame rate). The source shows its frame number as
// its brightness (frame n: luma 16 + 4 n), the click is a 40 ms tone.

#include "check.h"

#include "engine/Exporter.h"

#include <QApplication>
#include <QEventLoop>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>

namespace {

const ProjectFormat kFmt{64, 64, {25, 1}};
constexpr int kFrames = 50;
QString g_media;

bool render(const Timeline& tl, const ExportSettings& s)
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
    if (!ex.start(tl, s, &error)) {
        std::printf("       %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!ok) std::printf("       %s: %s\n", qPrintable(s.path), qPrintable(message.isEmpty() ? "timeout" : message));
    return ok;
}

QByteArray decode(const QString& path, bool video)
{
    QProcess p;
    QStringList args{"-v", "error", "-i", path, "-map", video ? "0:v:0" : "0:a:0"};
    if (video) args << "-f" << "rawvideo" << "-pix_fmt" << "yuv420p"; // code values as written (gray would be full range)
    else args << "-f" << "s16le" << "-ac" << "1";
    args << "-";
    p.start("ffmpeg", args);
    p.waitForFinished(60000);
    return p.readAllStandardOutput();
}

// Mean luma of each frame (yuv420p: the Y plane, then the chroma planes)
QVector<double> lumas(const QByteArray& gray, int pixels)
{
    QVector<double> out;
    for (qsizetype at = 0; at + pixels * 3 / 2 <= gray.size(); at += pixels * 3 / 2) {
        double sum = 0;
        for (int i = 0; i < pixels; ++i) sum += uint8_t(gray[at + i]);
        out << sum / pixels;
    }
    return out;
}

// First sample louder than half scale (mono s16), -1 = none
qsizetype firstLoud(const QByteArray& pcm)
{
    const auto* s = reinterpret_cast<const int16_t*>(pcm.constData());
    for (qsizetype i = 0; i < pcm.size() / 2; ++i)
        if (std::abs(int(s[i])) > 8000) return i;
    return -1;
}

void check(const char* name, const QString& dir, int from, int to, FrameRate rate)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip v;
    v.id = 1;
    v.mediaPath = g_media;
    v.out = kFrames - 1;
    v.linkId = 1;
    tl.video[0].clips << v;
    Clip a = v;
    a.id = 2;
    tl.audio[0].clips << a;

    ExportSettings s;
    s.path = dir + "/" + name + ".mkv";
    s.format = kFmt;
    s.videoCodec = "ffv1";
    s.pixFmt = "yuv420p";
    s.crf = 0;
    s.audioCodec = "pcm_s16le";
    s.from = from;
    s.to = to;
    s.rate = rate;
    if (!CHECK(render(tl, s))) return;

    const double outFps = rate.fps(), inFps = kFmt.rate.fps();
    const int length = to - from + 1;
    const int expected = rate == kFmt.rate ? length : std::max(1, int(std::lround(length * outFps / inFps)));
    const QVector<double> y = lumas(decode(s.path, true), kFmt.width * kFmt.height);
    if (!CHECK_EQ(int(y.size()), expected)) std::printf("       %s: frame count\n", name);
    int wrong = 0;
    for (int k = 0; k < y.size(); ++k) {
        const int shown = from + int(std::lrint(k * inFps / outFps));
        const double want = 16 + 4 * shown;
        if (std::abs(y[k] - want) > 1.5) {
            if (wrong++ < 3) std::printf("       %s: frame %d luma %.1f, wanted %.1f (timeline frame %d)\n", name, k, y[k], want, shown);
        }
    }
    CHECK_EQ(wrong, 0);
    // Sound: exactly as long as the picture, the click where its frame is
    const QByteArray pcm = decode(s.path, false);
    CHECK_EQ(qint64(pcm.size() / 2), qint64(std::llround(expected / outFps * 48000)));
    if (from <= 20 && to >= 21) {
        const qsizetype at = firstLoud(pcm);
        const double seconds = double(at) / 48000, want = (20 - from) / inFps;
        if (!CHECK(at >= 0 && std::abs(seconds - want) < 0.002))
            std::printf("       %s: click at %.4f s, wanted %.4f s\n", name, seconds, want);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_timing");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // Frame n: luma 16 + 4 n (full frame, limited range), lossless; a 1 kHz tone from 0.8 s to 0.84 s
    g_media = Check::makeMedia(tmp.filePath("count.mkv"),
                               {"-f", "lavfi", "-i", "nullsrc=s=64x64:r=25:d=2,format=yuv420p,geq=lum='16+4*N':cb=128:cr=128",
                                "-f", "lavfi", "-i", "aevalsrc='0.9*sin(2*PI*1000*t)*between(t,0.8,0.84)':s=48000:d=2",
                                "-c:v", "ffv1", "-c:a", "pcm_s16le", "-color_range", "tv", "-colorspace", "bt709",
                                "-color_primaries", "bt709", "-color_trc", "bt709"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    Check::initEngine();
    check("all", tmp.path(), 0, kFrames - 1, kFmt.rate);
    check("inout", tmp.path(), 10, 39, kFmt.rate);
    check("rate30", tmp.path(), 0, kFrames - 1, FrameRate{30, 1});
    check("rate30-inout", tmp.path(), 7, 33, FrameRate{30, 1});
    check("rate24", tmp.path(), 0, kFrames - 1, FrameRate{24, 1});
    check("rate60", tmp.path(), 5, 44, FrameRate{60, 1});
    return Check::result();
}
