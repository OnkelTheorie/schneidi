// Audio regression tests through the normal export path (Exporter + RenderQueue::exportSettings), checked with
// ffprobe/ffmpeg on the written file:
// - length of the render = timeline length, level of the render = level of the source
// - A/V sync: a white flash frame and a beep at the same source time stay together (also behind an in-point)
// - a hard cut between two clips with different sine phases: no peak above the clip level, no gap
// - every software video/audio encoder not covered elsewhere (H.265, AAC) writes a file of the right length
// Already covered elsewhere: hardware encoders (test_hw_encoders), MP3/WAV/AIFF/ALAC/ProRes + frame rate conversion
// (test_export_formats), per-stream volume/streams (test_audio_streams), seek click in the preview (test_audio).

#include "check.h"

#include "core/RenderJob.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <vector>

namespace {

const ProjectFormat kFmt{320, 180, {25, 1}};
constexpr double kFps = 25.0;
constexpr int kRate = 48000;

// Timeline: sine A [0,100), cosine B [100,150) (phase jump at the cut, same level), sync clip [150,240) from in 10
// -> its flash (source frame 50) lands on timeline frame 190
constexpr int kCut = 100;
constexpr int kSyncStart = 150, kSyncIn = 10, kFlashSource = 50;
constexpr int kFrames = 240;
constexpr int kFlash = kSyncStart + kFlashSource - kSyncIn;
constexpr double kAmp = 0.1; // -20 dBFS

QString g_sin, g_cos, g_sync;

Clip clip(int id, const QString& path, int start, int in, int out, int link)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = out;
    c.linkId = link;
    return c;
}

Timeline timeline()
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    const struct { const QString& path; int start, in, out; } parts[] = {
        {g_sin, 0, 0, kCut - 1},
        {g_cos, kCut, 25, 25 + kSyncStart - kCut - 1},
        {g_sync, kSyncStart, kSyncIn, kSyncIn + kFrames - kSyncStart - 1},
    };
    int id = 1;
    for (const auto& p : parts) {
        const int link = id;
        tl.video[0].clips << clip(id++, p.path, p.start, p.in, p.out, link);
        tl.audio[0].clips << clip(id++, p.path, p.start, p.in, p.out, link);
    }
    return tl;
}

bool render(const QString& path, const QString& format, int frames = kFrames)
{
    RenderJob job;
    job.id = 1;
    job.path = path;
    job.settings.format = format;
    job.settings.encoder = VideoEncoder::Software; // same result on every machine
    if (frames != kFrames) {
        job.inOut = true;
        job.from = 0;
        job.to = frames - 1;
    }
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
    if (!ok) std::printf("       %s: %s\n", qPrintable(path), qPrintable(message.isEmpty() ? "timeout" : message));
    return CHECK(ok) && CHECK(QFileInfo(path).size() > 0);
}

// first stream of the type ("audio"/"video") per ffprobe, with the container duration as "format_duration"
QJsonObject probe(const QString& path, const char* type)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-count_frames", "-show_streams", "-show_format", "-of", "json", path});
    p.waitForFinished(60000);
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    for (const QJsonValue& v : o.value("streams").toArray())
        if (v.toObject().value("codec_type").toString() == QLatin1String(type)) {
            QJsonObject s = v.toObject();
            s["format_duration"] = o.value("format").toObject().value("duration");
            return s;
        }
    return {};
}

double number(const QJsonObject& o, const char* key) { return o.value(key).toString().toDouble(); }

// first audio stream decoded to mono float 48 kHz (channel 0 only, so no downmix gain)
std::vector<float> decodeAudio(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-af", "pan=mono|c0=c0", "-f", "f32le",
                       "-ar", QString::number(kRate), "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* s = reinterpret_cast<const float*>(d.constData());
    return std::vector<float>(s, s + d.size() / qsizetype(sizeof(float)));
}

// mean brightness (0–255) of every video frame
std::vector<double> frameBrightness(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-map", "0:v:0", "-vf", "scale=16:9,format=gray", "-f",
                       "rawvideo", "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    std::vector<double> out;
    for (qsizetype at = 0; at + 144 <= d.size(); at += 144) {
        double sum = 0;
        for (int i = 0; i < 144; ++i) sum += quint8(d[at + i]);
        out.push_back(sum / 144);
    }
    return out;
}

double rmsDb(const std::vector<float>& s, double from, double to)
{
    const size_t a = size_t(from * kRate), b = std::min(s.size(), size_t(to * kRate));
    double sum = 0;
    for (size_t i = a; i < b; ++i) sum += double(s[i]) * s[i];
    return b > a ? 10 * std::log10(sum / double(b - a) + 1e-20) : -200;
}

float peakOf(const std::vector<float>& s, size_t from, size_t to)
{
    float p = 0.f;
    for (size_t i = from; i < std::min(to, s.size()); ++i) p = std::max(p, std::abs(s[i]));
    return p;
}

// Length, level and A/V sync of an H.264/AAC render (the usual export)
void testMainRender(const QString& dir)
{
    const QString out = dir + "/main.mp4";
    if (!render(out, "h264")) return;
    const QJsonObject v = probe(out, "video"), a = probe(out, "audio");
    const int frames = v.value("nb_read_frames").toString().toInt();
    if (!CHECK(std::abs(frames - kFrames) <= 1)) std::printf("       video frames %d, expected %d\n", frames, kFrames);
    const double audioDur = number(a, "duration");
    if (!CHECK(std::abs(audioDur - kFrames / kFps) <= 1 / kFps))
        std::printf("       audio %.3f s, expected %.3f s\n", audioDur, kFrames / kFps);

    // Level: RMS of the sine part equals the source (AAC: ±0.5 dB)
    const std::vector<float> src = decodeAudio(g_sin), got = decodeAudio(out);
    const double want = rmsDb(src, 0.4, 3.6), level = rmsDb(got, 0.4, 3.6);
    CHECK(std::abs(want - (20 * std::log10(kAmp / std::sqrt(2.0)))) < 0.1);
    if (!CHECK(std::abs(level - want) <= 0.5)) std::printf("       render %.2f dB, source %.2f dB\n", level, want);

    // A/V sync: first bright frame and first beep sample after the sync clip starts
    const std::vector<double> bright = frameBrightness(out);
    int flash = -1;
    for (int f = kSyncStart; f < int(bright.size()); ++f)
        if (bright[size_t(f)] > 180) {
            flash = f;
            break;
        }
    long beep = -1;
    for (size_t i = size_t(kSyncStart / kFps * kRate); i < got.size(); ++i)
        if (std::abs(got[i]) > 0.25f) {
            beep = long(i);
            break;
        }
    const double beepFrame = beep / double(kRate) * kFps;
    if (!CHECK(flash == kFlash)) std::printf("       flash at frame %d, expected %d\n", flash, kFlash);
    if (!CHECK(beep >= 0 && std::abs(beepFrame - flash) <= 1.0))
        std::printf("       beep at frame %.2f, flash at frame %d\n", beepFrame, flash);
}

// Lossless render (WAV): exact level, and the hard cut between sine and cosine neither overshoots nor drops out;
// the edge ramps (attachEdgeDeclick) dip only within ~4 ms on each side and the wave no longer jumps there (click)
void testCut(const QString& dir)
{
    const QString out = dir + "/cut.wav";
    if (!render(out, "wav")) return;
    const std::vector<float> s = decodeAudio(out);
    CHECK(std::abs(double(s.size()) - kFrames / kFps * kRate) <= kRate / kFps);
    CHECK(std::abs(rmsDb(s, 0.4, 3.6) - 20 * std::log10(kAmp / std::sqrt(2.0))) < 0.1);
    const size_t cut = size_t(kCut / kFps * kRate);
    const float peak = peakOf(s, cut - 4800, cut + 4800);
    if (!CHECK(peak <= kAmp * 1.01f)) std::printf("       peak at the cut %.4f (clip level %.4f)\n", peak, kAmp);
    // no gap: every 1 ms window (= one period of 1 kHz) around the cut still has the full sine, except the ramps
    float weakest = 1.f;
    for (size_t at = cut - 2400; at + 48 <= cut + 2400; at += 48)
        if (at + 48 <= cut - 240 || at >= cut + 240) weakest = std::min(weakest, peakOf(s, at, at + 48));
    if (!CHECK(weakest > kAmp * 0.7f)) std::printf("       weakest 1 ms window at the cut %.4f\n", weakest);
    // no click: from sample to sample the wave changes at most as fast as the 1 kHz sine itself (2 pi f / rate)
    float jump = 0.f;
    for (size_t i = cut - 2400; i + 1 < cut + 2400; ++i) jump = std::max(jump, std::abs(s[i + 1] - s[i]));
    if (!CHECK(jump <= kAmp * 2 * M_PI * 1000 / kRate * 1.1)) std::printf("       jump at the cut %.4f\n", jump);
}

// Software encoders not rendered by other tests: file written, right length
void testEncoders(const QString& dir)
{
    constexpr int frames = 50; // 2 s
    for (const char* format : {"h265", "aac"}) {
        const RenderFormatInfo& info = renderFormat(format);
        const QString out = QString("%1/enc-%2.%3").arg(dir, format, info.extension);
        if (!render(out, format, frames)) continue;
        const QJsonObject a = probe(out, "audio");
        CHECK_EQ(a.value("codec_name").toString(), QString(info.audioCodec));
        if (!CHECK(std::abs(number(a, "duration") - frames / kFps) <= 1 / kFps))
            std::printf("       %s: audio %.3f s\n", format, number(a, "duration"));
        if (*info.videoCodec) {
            const int n = probe(out, "video").value("nb_read_frames").toString().toInt();
            if (!CHECK_EQ(n, frames)) std::printf("       %s\n", format);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("audio_regression");
    if (!Check::haveFfmpeg() || QStandardPaths::findExecutable("ffprobe").isEmpty())
        return Check::skip("ffmpeg/ffprobe nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QString video = "testsrc2=size=320x180:rate=25:duration=4";
    const auto tone = [&](const QString& file, const QString& wave) {
        return Check::makeMedia(tmp.filePath(file),
                                {"-f", "lavfi", "-i", video, "-f", "lavfi", "-i",
                                 QString("aevalsrc=%1*%2(2*PI*1000*t)|%1*%2(2*PI*1000*t):s=48000:d=4").arg(kAmp).arg(wave),
                                 "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le", "-shortest"});
    };
    g_sin = tone("sin.mov", "sin");
    g_cos = tone("cos.mov", "cos");
    // black with one white frame (50 = 2.0 s) and a 100 ms beep starting at 2.0 s
    g_sync = Check::makeMedia(
        tmp.filePath("sync.mov"),
        {"-f", "lavfi", "-i",
         "color=c=black:s=320x180:r=25:d=4,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable=eq(n\\,50)", "-f",
         "lavfi", "-i",
         "aevalsrc=if(between(t\\,2\\,2.1)\\,0.5*sin(2*PI*1000*t)\\,0)|if(between(t\\,2\\,2.1)\\,0.5*sin(2*PI*1000*t)\\,0):s=48000:d=4",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le", "-shortest"});
    if (!CHECK(!g_sin.isEmpty() && !g_cos.isEmpty() && !g_sync.isEmpty())) return Check::result();
    Check::initMlt(); // pins LC_NUMERIC to C itself (no setlocale here, so the de_DE run tests that)
    testMainRender(tmp.path());
    testCut(tmp.path());
    testEncoders(tmp.path());
    return Check::result();
}
