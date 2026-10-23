// Picture/sound sync in milliseconds (docs/plan-0.10-ton.md, step 0): sources with a white flash frame and a beep
// starting at the same moment, five times at irregular places, in the containers and stream layouts real recordings
// have – sound stream starting later than the picture (phones, OBS), picture starting later, AAC with its encoder
// delay, Matroska with its 1 ms time base. Two clips per source (from the file start and from an in point) are
// exported losslessly; per flash: beep onset minus flash frame start. A player (ffmpeg) puts every beep exactly on its
// flash, so 0 ms is right; the test prints every offset and fails above kExportTolMs.
// Coarser sync checks (±1 frame, variable frame rate, overlapping packets): test_audio_regression.

#include "check.h"

#include "core/RenderJob.h"
#include "engine/Engine.h"
#include "engine/Exporter.h"
#include "engine/audio/Path.h"
#include "engine/RenderQueue.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace {

const ProjectFormat kFmt{320, 180, {25, 1}};
constexpr int kFps = 25;
constexpr int kRate = 48000;
constexpr double kSeconds = 8;
// Sound through MLT (until the own audio engine is the default, SCHNEIDI_AUDIO): its known offsets are reported, not
// counted; the own engine must be exact
bool mltAudio() { return !Audio::ownPath(); }
// Flash/beep times of the sources (frames at 25 fps = seconds * 25)
constexpr int kFlashes[] = {25, 58, 92, 131, 170};
// Timeline: [0, 80) from in 0, [100, 215) from in 85 -> flashes 25, 58 and 107, 146, 185
constexpr int kInB = 85, kStartB = 100, kLenA = 80, kLenB = 115;
// Matroska stores 1 ms ticks: a sample-exact beep may land up to 1 ms off; everything else is sample-exact
constexpr double kExportTolMs = 1.0;
// Preview with the own audio engine: a frame is shown when the sound reaches it (+ the time to hand it to the UI)
constexpr double kPreviewTolMs = 10.0;

struct Variant {
    const char* name;
    const char* what;
    double audioDelay = 0; // the sound stream starts this much after the picture
    double videoDelay = 0; // the picture starts this much after the sound
    const char* container = "mov";
    const char* audioCodec = "pcm_s16le";
    // MLT 7.30 plays a clip that starts at the file start 65-87 ms early when the sound stream starts later than the
    // picture (from an in point it is right): found 2026-10-22, fixed by the own audio engine
    bool mltOffset = false;
};

const Variant kVariants[] = {
    {"mov", "MOV, PCM, streams start together"},
    {"late_audio", "MOV, PCM, sound starts 0.3 s after the picture", 0.3, 0, "mov", "pcm_s16le", true},
    {"late_video", "MOV, PCM, picture starts 0.2 s after the sound", 0, 0.2},
    {"aac", "MP4, AAC (encoder delay)", 0, 0, "mp4", "aac"},
    {"aac_late_audio", "MP4, AAC, sound starts 0.3 s after the picture", 0.3, 0, "mp4", "aac", true},
    {"mkv", "Matroska, PCM (1 ms time base)", 0, 0, "mkv"},
    {"mkv_aac_late_audio", "Matroska, AAC, sound starts 0.3 s after the picture", 0.3, 0, "mkv", "aac", true},
};

// Beeps at the flash times, as an expression of the stream's own time (which begins `delay` after time 0)
QString beepExpression(double delay)
{
    QString any;
    for (int f : kFlashes) {
        const double t = f / double(kFps) - delay;
        any += QString("%1between(t\\,%2\\,%3)").arg(any.isEmpty() ? "" : "+").arg(t, 0, 'f', 4).arg(t + 0.02, 0, 'f', 4);
    }
    // onsets fall on whole milliseconds: the 1 kHz sine starts at phase 0, the first sample above the threshold is
    // right at the start
    const QString e = QString("if(%1\\,0.5*sin(2*PI*1000*t)\\,0)").arg(any);
    return e + "|" + e;
}

QString flashFilter(double delay)
{
    QString on;
    for (int f : kFlashes) on += QString("%1eq(n\\,%2)").arg(on.isEmpty() ? "" : "+").arg(f - int(std::lround(delay * kFps)));
    return QString("color=c=black:s=320x180:r=25:d=%1,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable=%2")
        .arg(kSeconds - delay).arg(on);
}

QString makeSource(const QString& dir, const Variant& v)
{
    QStringList args;
    if (v.videoDelay > 0) args << "-itsoffset" << QString::number(v.videoDelay);
    args << "-f" << "lavfi" << "-i" << flashFilter(v.videoDelay);
    if (v.audioDelay > 0) args << "-itsoffset" << QString::number(v.audioDelay);
    args << "-f" << "lavfi" << "-i"
         << QString("aevalsrc=%1:s=48000:d=%2").arg(beepExpression(v.audioDelay)).arg(kSeconds - v.audioDelay);
    args << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-pix_fmt" << "yuv420p" << "-g" << "25"
         << "-c:a" << v.audioCodec;
    if (QString(v.audioCodec) == "aac") args << "-b:a" << "192k";
    return Check::makeMedia(QString("%1/%2.%3").arg(dir, v.name, v.container), args);
}

Clip clip(int id, const QString& path, int start, int in, int len, int link)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + len - 1;
    c.linkId = link;
    return c;
}

bool exportTimeline(const Timeline& tl, const QString& path)
{
    RenderJob job;
    job.id = 1;
    job.path = path;
    job.settings.format = "proresproxy"; // lossless sound (PCM 24 bit) next to the picture
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
    if (!ok) std::printf("       %s\n", qPrintable(message.isEmpty() ? "timeout" : message));
    return CHECK(ok);
}

// Like a player: silence before a sound stream that starts after time 0 (first_pts=0)
std::vector<float> decodeAudio(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-af",
                       "aresample=async=1:first_pts=0,pan=mono|c0=c0", "-f", "f32le",
                       "-ar", QString::number(kRate), "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* s = reinterpret_cast<const float*>(d.constData());
    return std::vector<float>(s, s + d.size() / qsizetype(sizeof(float)));
}

std::vector<int> flashFrames(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-map", "0:v:0", "-vf", "scale=16:9,format=gray", "-f",
                       "rawvideo", "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    std::vector<int> out;
    for (qsizetype at = 0, f = 0; at + 144 <= d.size(); at += 144, ++f) {
        int sum = 0;
        for (int i = 0; i < 144; ++i) sum += quint8(d[at + i]);
        if (sum / 144 > 180) out.push_back(int(f));
    }
    return out;
}

// Sample positions where the sound starts after at least 0.3 s of silence
std::vector<long> onsets(const std::vector<float>& s)
{
    std::vector<long> out;
    long quiet = kRate;
    for (size_t i = 0; i < s.size(); ++i) {
        if (std::abs(s[i]) > 0.02f) {
            if (quiet >= kRate * 3 / 10) out.push_back(long(i));
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    return out;
}

// Offset per flash in ms (beep minus picture); empty = flashes/beeps not found (reported)
std::vector<double> offsets(const QString& path)
{
    const std::vector<int> flashes = flashFrames(path);
    const std::vector<long> beeps = onsets(decodeAudio(path));
    std::vector<double> out;
    for (int f : flashes) {
        long nearest = -1;
        for (long b : beeps)
            if (nearest < 0 || std::abs(b - f * kRate / kFps) < std::abs(nearest - f * kRate / kFps)) nearest = b;
        if (nearest >= 0 && std::abs(nearest - f * kRate / kFps) < kRate / 2)
            out.push_back((nearest - f * double(kRate) / kFps) * 1000.0 / kRate);
    }
    if (!CHECK_EQ(int(flashes.size()), 5) || !CHECK_EQ(int(beeps.size()), 5) || !CHECK_EQ(int(out.size()), 5)) {
        std::printf("       %s: %d flashes, %d beeps\n", qPrintable(QFileInfo(path).fileName()), int(flashes.size()),
                    int(beeps.size()));
        return {};
    }
    return out;
}

void testExport(const QString& dir)
{
    std::printf("Export (Versatz Ton - Bild je Blitz, ms; Clip A ab Dateianfang, Clip B ab In-Punkt):\n");
    for (const Variant& v : kVariants) {
        const QString src = makeSource(dir, v);
        if (!CHECK(!src.isEmpty())) continue;
        // The source itself, as a player sees it: every beep on its flash (else the test material is wrong)
        const std::vector<double> own = offsets(src);
        bool sourceOk = own.size() == 5;
        for (double ms : own) sourceOk = sourceOk && std::abs(ms) <= 1.0;
        if (!CHECK(sourceOk)) {
            std::printf("       %s: Quelle selbst nicht synchron\n", v.name);
            continue;
        }
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        tl.video[0].clips << clip(1, src, 0, 0, kLenA, 1) << clip(3, src, kStartB, kInB, kLenB, 3);
        tl.audio[0].clips << clip(2, src, 0, 0, kLenA, 1) << clip(4, src, kStartB, kInB, kLenB, 3);
        const QString out = QString("%1/%2-out.mov").arg(dir, v.name);
        if (!exportTimeline(tl, out)) continue;
        const std::vector<double> got = offsets(out);
        if (got.empty()) continue;
        double worst = 0;
        QString line;
        for (size_t i = 0; i < got.size(); ++i) {
            worst = std::max(worst, std::abs(got[i]));
            line += QString("%1%2").arg(i == 2 ? "  | B:" : " ").arg(got[i], 7, 'f', 2);
        }
        const bool ok = worst <= kExportTolMs, known = !ok && mltAudio() && v.mltOffset;
        std::printf("  %-20s A:%s%s\n", v.name, qPrintable(line),
                    ok ? "" : known ? "  <- versetzt (bekannt: MLT-Ton-Weg)" : "  <- versetzt");
        if (!known && !CHECK(ok)) std::printf("       %s\n", v.what);
    }
}

// Watches the file SDL's "disk" driver writes: when each block of sound was handed to the "speaker"
struct SpeakerClock {
    QString path;
    QElapsedTimer clock;
    std::atomic<bool> stop{false};
    std::vector<std::pair<qint64, qint64>> writes; // (file size after the write, ns)
    std::thread thread;
    void start()
    {
        thread = std::thread([this] {
            qint64 last = -1;
            while (!stop) {
                const qint64 size = QFileInfo(path).size();
                if (size != last) {
                    writes.emplace_back(size, clock.nsecsElapsed());
                    last = size;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        });
    }
    void finish()
    {
        stop = true;
        thread.join();
    }
    // When sample `frame` (s16 stereo) left for the speaker: start of its block + its place in the block (ns);
    // -1 = not written
    qint64 timeOf(qint64 frame) const
    {
        const qint64 byte = frame * 4;
        for (size_t i = 1; i < writes.size(); ++i)
            if (writes[i].first > byte) return writes[i].second + (byte - writes[i - 1].first) / 4 * 1000000000LL / kRate;
        return -1;
    }
};

// Preview: the real Engine plays a timeline with three sources from their file start (the MLT offset of the export
// above shows here too) – flash shown (frameReady) against beep handed to the speaker. MLT sound path: only measured
// and printed. Own audio engine (picture follows its clock): |offset| <= kPreviewTolMs. The "speaker" (SDL's disk
// driver resp. the engine's file device) has no device latency; with a sound card the clock subtracts its buffer.
void testPreview(const QString& dir, const QString& speaker, SpeakerClock& clock)
{
    const Variant* parts[] = {&kVariants[0], &kVariants[1], &kVariants[3]}; // mov, late_audio, aac
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    for (int i = 0; i < 3; ++i) {
        const QString src = QString("%1/%2.%3").arg(dir, parts[i]->name, parts[i]->container);
        if (!CHECK(QFileInfo::exists(src))) return;
        tl.video[0].clips << clip(10 + 2 * i, src, 100 * i, 0, kLenA, 10 + 2 * i);
        tl.audio[0].clips << clip(11 + 2 * i, src, 100 * i, 0, kLenA, 10 + 2 * i);
    }
    // The disk driver waits whole milliseconds per block (SDL_Delay): 1024 samples = 21.33 ms played in 21 ms, the
    // "speaker" ran 1.6 % fast and the offset drifted 11 ms per second. 960 samples = exactly 20 ms
    QSettings().setValue("audio/buffer", 960);
    // Before the engine: its consumer may still show a frame while it shuts down (a destroyed mutex threw on Windows)
    std::mutex lock;
    std::vector<qint64> shown; // ns of each flash frame (rising edge)
    bool bright = false;
    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("Vorschau übersprungen: %s\n", qPrintable(error));
        return;
    }
    engine.setFormat(kFmt);
    QObject::connect(&engine, &Engine::frameReady, &engine, [&](const QImage& img) {
        const qint64 now = clock.clock.nsecsElapsed(); // in the consumer thread, at the moment of showing
        const bool white = qGray(img.pixel(img.width() / 2, img.height() / 2)) > 180;
        std::lock_guard<std::mutex> g(lock);
        if (white && !bright) shown.push_back(now);
        bright = white;
    }, Qt::DirectConnection);
    engine.updateTimeline(tl);
    engine.showTimeline(0);
    QEventLoop wait;
    QTimer::singleShot(800, &wait, &QEventLoop::quit);
    wait.exec();
    engine.play();
    QTimer::singleShot((200 + kLenA) * 1000 / kFps + 1000, &wait, &QEventLoop::quit);
    wait.exec();
    engine.pause();
    QTimer::singleShot(400, &wait, &QEventLoop::quit);
    wait.exec();
    clock.finish();

    QFile f(speaker);
    if (!CHECK(f.open(QIODevice::ReadOnly))) return;
    const QByteArray raw = f.readAll();
    const auto* s = reinterpret_cast<const int16_t*>(raw.constData());
    std::vector<float> left(size_t(raw.size() / 4));
    for (size_t i = 0; i < left.size(); ++i) left[i] = s[i * 2] / 32768.f;
    const std::vector<long> beeps = onsets(left);
    std::vector<qint64> flashes;
    {
        std::lock_guard<std::mutex> g(lock);
        flashes = shown;
    }
    std::printf("Vorschau (Ton am Lautsprecher - Bild gezeigt, ms; je Quelle ab Dateianfang):\n");
    if (!CHECK_EQ(int(flashes.size()), 6) || !CHECK_EQ(int(beeps.size()), 6)) return;
    for (int i = 0; i < 3; ++i) {
        QString line;
        for (int k = 0; k < 2; ++k) {
            const size_t n = size_t(2 * i + k);
            const qint64 heard = clock.timeOf(beeps[n]);
            if (!CHECK(heard >= 0)) return;
            const double ms = (heard - flashes[n]) / 1e6;
            line += QString(" %1").arg(ms, 7, 'f', 1);
            if (!mltAudio() && std::abs(ms) > kPreviewTolMs) line += " <- versetzt";
            if (!mltAudio()) CHECK(std::abs(ms) <= kPreviewTolMs);
        }
        std::printf("  %-20s%s\n", parts[i]->name, qPrintable(line));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QTemporaryDir tmp;
    SpeakerClock clock;
    clock.path = tmp.filePath("speaker.raw");
    qputenv("SDL_AUDIODRIVER", "disk"); // the "speaker": SDL writes what it would play, in real time
    qputenv("SDL_DISKAUDIOFILE", QDir::toNativeSeparators(clock.path).toLocal8Bit());
    qputenv("SCHNEIDI_AUDIO_DEVICE", ("file:" + QDir::toNativeSeparators(clock.path)).toLocal8Bit()); // own engine
    QApplication app(argc, argv);
    Check::initApp("av_sync");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    if (!CHECK(tmp.isValid())) return Check::result();
    Check::initMlt();
    testExport(tmp.path());
    clock.clock.start();
    clock.start();
    testPreview(tmp.path(), clock.path, clock);
    if (clock.thread.joinable()) clock.finish();
    return Check::result();
}
