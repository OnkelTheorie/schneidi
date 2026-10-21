// Audio regression tests through the normal export path (Exporter + RenderQueue::exportSettings), checked with
// ffprobe/ffmpeg on the written file:
// - length of the render = timeline length, level of the render = level of the source
// - A/V sync: a white flash frame and a beep at the same source time stay together (also behind an in-point)
// - a hard cut between two clips with different sine phases: no peak above the clip level, no gap
// - every software video/audio encoder not covered elsewhere (H.265, AAC) writes a file of the right length
// - audio packets with overlapping timestamps (the user's clip program writes some, e.g. 2026-10-11_19-17-02):
//   the sound follows the timestamps like a player, not the raw packet order (that drifts by the overlaps)
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

// Renders through the normal export path; false = failed (reported)
bool run(const Timeline& tl, const RenderJob& job)
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
    if (!CHECK(ex.start(tl, RenderQueue::exportSettings(job, kFmt), &error))) {
        std::printf("       %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!ok) std::printf("       %s: %s\n", qPrintable(job.path), qPrintable(message.isEmpty() ? "timeout" : message));
    return CHECK(ok) && CHECK(QFileInfo(job.path).size() > 0);
}

RenderJob job(const QString& path, const QString& format)
{
    RenderJob j;
    j.id = 1;
    j.path = path;
    j.settings.format = format;
    j.settings.encoder = VideoEncoder::Software; // same result on every machine
    return j;
}

bool render(const QString& path, const QString& format, int frames = kFrames)
{
    RenderJob j = job(path, format);
    if (frames != kFrames) {
        j.inOut = true;
        j.from = 0;
        j.to = frames - 1;
    }
    return run(timeline(), j);
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

// Times (s) where the signal first exceeds the threshold after at least 0.3 s of quiet: beep onsets
std::vector<double> onsets(const std::vector<float>& s, float threshold = 0.25f)
{
    std::vector<double> out;
    long quiet = kRate;
    for (size_t i = 0; i < s.size(); ++i) {
        if (std::abs(s[i]) > threshold) {
            if (quiet >= kRate * 3 / 10) out.push_back(double(i) / kRate);
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    return out;
}

void testOverlappingAudio(const QString& dir)
{
    // Beeps at 1, 3, 5, 7 s; AAC in Matroska, then every 40th packet onwards 21 ms earlier (overlaps the one before,
    // 9 times in 8 s: ~190 ms), like the clip program's files
    const QString clean = Check::makeMedia(dir + "/beeps.mkv", {
        "-f", "lavfi", "-i", "color=c=black:s=320x180:r=25:d=8", "-f", "lavfi", "-i",
        "aevalsrc=if(lt(mod(t-1\\,2)\\,0.1)*gte(t\\,1)\\,0.5*sin(2*PI*1000*t)\\,0):s=48000:d=8",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-shortest"});
    const QString overlapping = Check::makeMedia(dir + "/overlap.mkv", {
        "-i", clean, "-c", "copy", "-bsf:a", "setts=ts=TS-21*floor(N/40)"});
    if (!CHECK(!clean.isEmpty() && !overlapping.isEmpty())) return;
    // Reference: decoded by the timestamps (aresample async) – and the raw order, which must really differ here
    auto decode = [](const QString& path, bool byTimestamps) {
        QProcess p;
        QStringList args{"-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-af",
                         QString("pan=mono|c0=c0%1").arg(byTimestamps ? ",aresample=async=1:min_hard_comp=0.01:first_pts=0" : ""),
                         "-f", "f32le", "-ar", QString::number(kRate), "-"};
        p.start("ffmpeg", args);
        p.waitForFinished(60000);
        const QByteArray d = p.readAllStandardOutput();
        const auto* f = reinterpret_cast<const float*>(d.constData());
        return std::vector<float>(f, f + d.size() / qsizetype(sizeof(float)));
    };
    const std::vector<double> want = onsets(decode(overlapping, true)), raw = onsets(decode(overlapping, false));
    if (!CHECK_EQ(int(want.size()), 4) || !CHECK_EQ(int(raw.size()), 4)) return;
    CHECK(raw[3] - want[3] > 0.1); // the material does what it should

    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << clip(1, overlapping, 0, 0, 199, 1);
    tl.audio[0].clips << clip(2, overlapping, 0, 0, 199, 1);
    const RenderJob j = job(dir + "/overlap.wav", "wav");
    if (!run(tl, j)) return;
    const std::vector<double> got = onsets(decodeAudio(j.path));
    if (!CHECK_EQ(int(got.size()), 4)) return;
    for (int i = 0; i < 4; ++i)
        if (!CHECK(std::abs(got[size_t(i)] - want[size_t(i)]) <= 1 / kFps))
            std::printf("       beep %d at %.3f s, by timestamps %.3f s (raw order %.3f s)\n", i, got[size_t(i)],
                        want[size_t(i)], raw[size_t(i)]);
}

// Phone video with variable frame rate (frames dropped irregularly, timestamps kept): flash and beep at 1, 3, 5 s
// stay together in the 25 fps render, and picture and sound are as long as the source
void testVariableFrameRate(const QString& dir)
{
    const QString vfr = Check::makeMedia(dir + "/vfr.mp4", {
        "-f", "lavfi", "-i",
        "color=c=black:s=320x180:r=60:d=6,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:"
        "enable=eq(n\\,60)+eq(n\\,180)+eq(n\\,300),select='not(mod(n\\,2))+eq(mod(n\\,7)\\,3)'",
        "-f", "lavfi", "-i",
        "aevalsrc=if(lt(mod(t-1\\,2)\\,0.1)*gte(t\\,1)\\,0.5*sin(2*PI*1000*t)\\,0):s=48000:d=6",
        "-fps_mode", "vfr", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    if (!CHECK(!vfr.isEmpty())) return;
    const QJsonObject src = probe(vfr, "video");
    CHECK(src.value("avg_frame_rate").toString() != src.value("r_frame_rate").toString()); // really variable
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << clip(1, vfr, 0, 0, 149, 1);
    tl.audio[0].clips << clip(2, vfr, 0, 0, 149, 1);
    const RenderJob j = job(dir + "/vfr-out.mp4", "h264");
    if (!run(tl, j)) return;
    const QJsonObject v = probe(j.path, "video"), a = probe(j.path, "audio");
    CHECK_EQ(v.value("nb_read_frames").toString().toInt(), 150);
    CHECK(std::abs(number(a, "duration") - 6.0) <= 1 / kFps);
    const std::vector<double> bright = frameBrightness(j.path);
    std::vector<int> flashes;
    for (int f = 0; f < int(bright.size()); ++f)
        if (bright[size_t(f)] > 180 && (flashes.empty() || f > flashes.back() + 5)) flashes.push_back(f);
    const std::vector<double> beeps = onsets(decodeAudio(j.path));
    if (!CHECK_EQ(int(flashes.size()), 3) || !CHECK_EQ(int(beeps.size()), 3)) return;
    for (int i = 0; i < 3; ++i) {
        const int want = 25 + 50 * i; // 1, 3, 5 s
        if (!CHECK(std::abs(flashes[size_t(i)] - want) <= 1)) std::printf("       flash %d at frame %d, expected %d\n", i, flashes[size_t(i)], want);
        if (!CHECK(std::abs(beeps[size_t(i)] * kFps - flashes[size_t(i)]) <= 1.0))
            std::printf("       beep %d at frame %.2f, flash at %d\n", i, beeps[size_t(i)] * kFps, flashes[size_t(i)]);
    }
}

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
    testOverlappingAudio(tmp.path());
    testVariableFrameRate(tmp.path());
    return Check::result();
}
