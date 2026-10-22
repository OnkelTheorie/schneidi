// Own audio engine, step 3 (docs/plan-0.10-ton.md): the export takes its sound from Audio::Mixer (engine/MixedSound),
// MLT only renders and encodes the picture.
// Generated source (always): the written file holds exactly the mixer's samples, rounded to the file's bit depth:
// - WAV 24 bit (really 24 bit: the MLT path wrote 16-bit samples into it) and 16 bit, audio only
// - In/Out range: the samples of exactly that span of the timeline
// - frame rate converted (25 -> 30 fps, ProRes with 24-bit PCM), also with In/Out: the same samples, the length of
//   the span
// - 16 kHz mono 16 bit (speech to text): the mixer at 16 kHz, L and R mixed
// - H.264 + AAC: the sound sits on the picture (best correlation at lag 0), as long as the picture
// - the MLT path still answers when chosen (fallback): its "24 bit" stays on the 16-bit grid
// Against the sound references of step 0 (private docs folder; skipped without it): every project exported through
// the own path = the mixer in exact mode, sample for sample at 24 bit, and the time of the export.

#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/TimelineOps.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "engine/audio/Mixer.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef SCHNEIDI_AUDIO_REFERENCE
#define SCHNEIDI_AUDIO_REFERENCE ""
#endif

namespace {

const ProjectFormat kFmt{320, 180, {25, 1}};
constexpr int kRate = 48000;
constexpr int kFrames = 150; // timeline length (6 s at 25 fps)

QString g_src;

// Interleaved float samples of a file (channels/rate as given, no conversion when the file already has them)
std::vector<float> decode(const QString& path, int channels = 2, int rate = kRate)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-f", "f32le", "-ac",
                       QString::number(channels), "-ar", QString::number(rate), "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* s = reinterpret_cast<const float*>(d.constData());
    return std::vector<float>(s, s + d.size() / qsizetype(sizeof(float)));
}

double probeDuration(const QString& path, const char* stream)
{
    QProcess p;
    p.start("ffprobe", {"-v", "error", "-select_streams", stream, "-show_entries", "stream=duration", "-of", "csv=p=0", path});
    p.waitForFinished(30000);
    return p.readAllStandardOutput().trimmed().toDouble();
}

Clip clip(int id, int start, int in, int out, int link)
{
    Clip c;
    c.id = id;
    c.mediaPath = g_src;
    c.start = start;
    c.in = in;
    c.out = out;
    c.linkId = link;
    return c;
}

// Picture and sound of the source on V1/A1 in two pieces with a gap, a third piece on A2 with fades and volume
Timeline timeline()
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(2);
    tl.video[0].clips << clip(1, 0, 10, 69, 2) << clip(3, 80, 100, 169, 4);
    tl.audio[0].clips << clip(2, 0, 10, 69, 1) << clip(4, 80, 100, 169, 3);
    Clip c = clip(5, 40, 0, 99, 0);
    c.volumeDb = -4;
    c.fadeIn = 12;
    c.fadeOut = 20;
    tl.audio[1].clips << c;
    tl.audio[1].pan = -30;
    return tl;
}

// Mixer output for [from, from + count) samples, interleaved stereo
std::vector<float> mix(const Timeline& tl, FrameRate fps, int rate, int64_t from, int64_t count,
                       const QHash<QString, int>& lengths = {}, qint64* ms = nullptr)
{
    Audio::Mixer::Options o;
    o.rate = rate;
    o.fileLength = [&](const QString& p) { return lengths.value(p); };
    QElapsedTimer t;
    t.start();
    Audio::Mixer m(tl, fps, o);
    std::vector<float> L(static_cast<size_t>(count)), R(static_cast<size_t>(count)), out(static_cast<size_t>(count) * 2);
    float* planes[] = {L.data(), R.data()};
    for (int64_t pos = 0; pos < count;) {
        const int n = int(std::min<int64_t>(count - pos, qEnvironmentVariableIntValue("EXPORT_AUDIO_BLOCK") > 0 ? qEnvironmentVariableIntValue("EXPORT_AUDIO_BLOCK") : 4096));
        float* at[] = {planes[0] + pos, planes[1] + pos};
        m.render(from + pos, n, at);
        pos += n;
    }
    if (ms) *ms = t.elapsed();
    for (int64_t i = 0; i < count; ++i) {
        out[size_t(2 * i)] = L[size_t(i)];
        out[size_t(2 * i + 1)] = R[size_t(i)];
    }
    return out;
}

int64_t frameStart(const Timeline& tl, FrameRate fps, int64_t f, int rate = kRate)
{
    Audio::Mixer::Options o;
    o.rate = rate;
    return Audio::Mixer(tl, fps, o).frameStart(f);
}

float quantize(float v, int bits)
{
    const double scale = std::ldexp(1.0, bits - 1);
    return float(std::clamp(std::nearbyint(double(v) * scale), -scale, scale - 1) / scale);
}

// Largest difference after rounding `want` like the file; lengths must match
bool same(const char* what, const std::vector<float>& got, std::vector<float> want, int bits)
{
    for (float& v : want) v = quantize(v, bits);
    double worst = 0;
    size_t at = 0;
    const size_t n = std::min(got.size(), want.size());
    for (size_t i = 0; i < n; ++i)
        if (const double d = std::abs(double(got[i]) - double(want[i])); d > worst) {
            worst = d;
            at = i;
        }
    const bool ok = got.size() == want.size() && worst == 0;
    std::printf("  %-28s %8zu Samples (erwartet %zu), max. Abweichung %.2e%s\n", what, got.size(), want.size(), worst,
                ok ? "" : "  <- ANDERS");
    if (!ok && worst > 0) std::printf("       bei Sample %zu: %.8f statt %.8f\n", at, got[at], want[at]);
    return CHECK(ok);
}

bool onGrid(const std::vector<float>& s, int bits)
{
    for (float v : s)
        if (quantize(v, bits) != v) return false;
    return true;
}

bool run(const Timeline& tl, ExportSettings s, qint64* ms = nullptr)
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
    QElapsedTimer t;
    t.start();
    QString error;
    if (!CHECK(ex.start(tl, s, &error))) {
        std::printf("       %s\n", qPrintable(error));
        return false;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (ms) *ms = t.elapsed();
    if (!ok) std::printf("       %s: %s\n", qPrintable(s.path), qPrintable(message.isEmpty() ? "timeout" : message));
    return CHECK(ok) && CHECK(QFileInfo(s.path).size() > 0);
}

ExportSettings settings(const QString& path, const QString& format, int bits = 24,
                        ExportSettings::AudioPath audioPath = ExportSettings::AudioPath::Own)
{
    RenderJob j;
    j.id = 1;
    j.path = path;
    j.settings.format = format;
    j.settings.audioBits = bits;
    j.settings.encoder = VideoEncoder::Software;
    ExportSettings s = RenderQueue::exportSettings(j, kFmt);
    s.audioPath = audioPath;
    return s;
}

void checkGenerated(const QString& dir)
{
    const Timeline tl = timeline();
    const FrameRate fps = kFmt.rate;
    const int64_t total = frameStart(tl, fps, kFrames);

    const QString wav24 = dir + "/24.wav";
    if (run(tl, settings(wav24, "wav", 24))) {
        const std::vector<float> got = decode(wav24);
        same("WAV 24 bit", got, mix(tl, fps, kRate, 0, total), 24);
        CHECK(!onGrid(got, 16)); // really 24 bit
    }
    const QString wav16 = dir + "/16.wav";
    if (run(tl, settings(wav16, "wav", 16))) same("WAV 16 bit", decode(wav16), mix(tl, fps, kRate, 0, total), 16);

    // In/Out: frames 30..119
    ExportSettings range = settings(dir + "/range.wav", "wav", 24);
    range.from = 30;
    range.to = 119;
    if (run(tl, range)) {
        const int64_t a = frameStart(tl, fps, 30), b = frameStart(tl, fps, 120);
        same("In/Out 30-119", decode(range.path), mix(tl, fps, kRate, a, b - a), 24);
    }

    // Frame rate converted: 30 fps ProRes, sound 24-bit PCM; length of the span at the new rate
    ExportSettings conv = settings(dir + "/30fps.mov", "proresproxy", 24);
    conv.rate = {30, 1};
    if (run(tl, conv)) {
        const int64_t frames = std::lround(kFrames * 30.0 / 25.0);
        same("30 fps umgerechnet", decode(conv.path), mix(tl, fps, kRate, 0, frames * kRate / 30), 24);
        CHECK(std::abs(probeDuration(conv.path, "a:0") - probeDuration(conv.path, "v:0")) < 0.001);
    }
    // ... with In/Out (frames 30..119 = 3.6 s)
    conv.path = dir + "/30fps-range.mov";
    conv.from = 30;
    conv.to = 119;
    if (run(tl, conv)) {
        const int64_t frames = std::lround(90 * 30.0 / 25.0);
        same("30 fps + In/Out", decode(conv.path), mix(tl, fps, kRate, frameStart(tl, fps, 30), frames * kRate / 30), 24);
    }

    // Speech to text: 16 kHz mono 16 bit (as Transcriber asks for it)
    ExportSettings mono = settings(dir + "/mono.wav", "wav", 16);
    mono.audioCodec = "pcm_s16le";
    mono.audioRate = 16000;
    mono.audioChannels = 1;
    if (run(tl, mono)) {
        const int64_t n = frameStart(tl, fps, kFrames, 16000);
        const std::vector<float> st = mix(tl, fps, 16000, 0, n);
        std::vector<float> want(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) // rounded from the exact sum, like the export
            want[size_t(i)] = float(std::clamp(std::nearbyint((double(st[size_t(2 * i)]) + double(st[size_t(2 * i + 1)])) * 16384.0),
                                               -32768.0, 32767.0) / 32768.0);
        same("16 kHz mono", decode(mono.path, 1, 16000), want, 16);
    }

    // H.264 + AAC: lossy, so the sound must sit on the picture (correlation) and be as long
    const QString mp4 = dir + "/video.mp4";
    if (run(tl, settings(mp4, "h264"))) {
        const std::vector<float> got = decode(mp4), want = mix(tl, fps, kRate, 0, total);
        int bestLag = 0;
        double best = -1;
        for (int lag = -2048; lag <= 2048; lag += 1) {
            double sum = 0;
            for (size_t i = 48000; i < 96000 && i + size_t(std::abs(lag)) < want.size() / 2; ++i) {
                const size_t j = size_t(int64_t(i) + lag);
                if (2 * j + 1 < got.size()) sum += double(got[2 * j]) * double(want[2 * i]);
            }
            if (sum > best) {
                best = sum;
                bestLag = lag;
            }
        }
        const double aDur = probeDuration(mp4, "a:0"), vDur = probeDuration(mp4, "v:0");
        std::printf("  %-28s Versatz %d Samples, Ton %.3f s, Bild %.3f s\n", "H.264 + AAC", bestLag, aDur, vDur);
        CHECK(bestLag == 0);
        CHECK(std::abs(aDur - vDur) < 0.03); // AAC ends on whole 1024-sample packets
    }

    // Fallback: the MLT path is still there and still delivers 16-bit samples
    const QString mlt = dir + "/mlt.wav";
    if (run(tl, settings(mlt, "wav", 24, ExportSettings::AudioPath::Mlt))) {
        const std::vector<float> got = decode(mlt);
        std::printf("  %-28s %8zu Samples, auf 16-bit-Raster: %s\n", "MLT-Weg (Rückfall)", got.size(),
                    onGrid(got, 16) ? "ja" : "nein");
        CHECK(got.size() == size_t(total) * 2);
        CHECK(onGrid(got, 16));
    }
}

void checkReferences(const QString& refDir, const QString& dir)
{
    QStringList projects;
    for (const QString& f : QDir(refDir + "/projekte").entryList({"*.schneidi"}, QDir::Files, QDir::Name))
        projects << refDir + "/projekte/" + f;
    if (QFileInfo::exists(refDir + "/echt/tiktok.schneidi")) projects << refDir + "/echt/tiktok.schneidi";
    const QStringList only = qEnvironmentVariable("EXPORT_AUDIO_CASES").split(',', Qt::SkipEmptyParts);
    for (const QString& path : projects) {
        const QString name = QFileInfo(path).completeBaseName();
        if (!only.isEmpty() && !only.contains(name)) continue;
        ProjectData d;
        QString error;
        if (!CHECK(ProjectFile::load(path, &d, &error))) {
            std::printf("       %s: %s\n", qPrintable(path), qPrintable(error));
            continue;
        }
        Project project;
        project.load(d);
        const Timeline tl = project.renderTimeline();
        bool missing = false;
        for (const MediaInfo& m : project.media()) missing = missing || !QFileInfo::exists(m.path);
        if (missing) {
            std::printf("  %-28s übersprungen: Quellen fehlen\n", qPrintable(name));
            continue;
        }
        RenderJob j;
        j.id = 1;
        j.path = QString("%1/%2.wav").arg(dir, name);
        j.settings.format = "wav";
        j.settings.audioBits = 24;
        ExportSettings s = RenderQueue::exportSettings(j, project);
        s.audioPath = ExportSettings::AudioPath::Own;
        qint64 exportMs = 0, mixMs = 0;
        if (!run(tl, s, &exportMs)) continue;
        QHash<QString, int> lengths;
        for (const MediaInfo& m : project.media()) lengths.insert(m.path, m.length);
        Audio::Mixer::Options o;
        const int64_t total = Audio::Mixer(tl, d.format.rate, o).frameStart(TimelineOps::endFrame(tl));
        const std::vector<float> want = mix(tl, d.format.rate, kRate, 0, total, lengths, &mixMs);
        same(qPrintable(QString("%1 (%2 ms, Mischer %3 ms)").arg(name).arg(exportMs).arg(mixMs)), decode(j.path), want, 24);
        if (!qEnvironmentVariableIsSet("EXPORT_AUDIO_KEEP")) QFile::remove(j.path);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("export_audio");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    if (qEnvironmentVariableIsSet("EXPORT_AUDIO_KEEP")) { // look at the files afterwards
        tmp.setAutoRemove(false);
        std::printf("Dateien bleiben in %s\n", qPrintable(tmp.path()));
    }
    Check::initMlt();

    // 8 s picture + stereo noise (L and R different), PCM 24 bit: the source itself is exact
    g_src = Check::makeMedia(tmp.filePath("src.mov"),
                             {"-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=8", "-f", "lavfi", "-i",
                              "aevalsrc=0.3*(random(0)*2-1)|0.2*sin(2*PI*330*t)+0.1*(random(1)*2-1):s=48000:d=8",
                              "-c:v", "mpeg4", "-q:v", "5", "-c:a", "pcm_s24le", "-shortest"});
    if (!CHECK(!g_src.isEmpty())) return Check::result();
    std::printf("Erzeugte Quelle:\n");
    checkGenerated(tmp.path());

    const QString refDir = qEnvironmentVariable("AUDIO_REFERENCE_DIR", SCHNEIDI_AUDIO_REFERENCE);
    if (refDir.isEmpty() || !QFileInfo::exists(refDir + "/manifest.json")) {
        std::printf("Referenzen übersprungen: keine Ton-Referenzen (docs/testdaten/ton-referenz)\n");
        return Check::result();
    }
    std::printf("Referenzprojekte (eigener Weg = Mischer):\n");
    checkReferences(refDir, tmp.path());
    return Check::result();
}
