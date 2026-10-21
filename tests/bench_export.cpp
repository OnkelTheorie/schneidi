// Benchmark (not a CTest test): export of a real project or media file, like the Deliver page, next to plain FFmpeg.
// Reports how long Exporter::start() blocks the GUI thread, the time to the first rendered frame, frames per second,
// CPU load and peak memory. With a single media file (or --ref) also FFmpeg alone with the same encoder settings:
// shows what schneidi itself costs.
// Run: QT_QPA_PLATFORM=offscreen ./build-tests/tests/bench_export <project.schneidi | media file> [options]
//   --codec libx264|h264_nvenc|…  (default libx264)   --preset medium   --crf 20   --threads 0 (all)
//   --seconds N   only the first N seconds        --rate 30   other output frame rate
//   --ref         also FFmpeg alone (default for a media file, --no-ref: not, e.g. under perf)   --keep <file>  keep the rendered file
//   --an / --vn   no sound / no picture (which part is slow?)
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/Bundle.h"
#include "engine/Engine.h"
#include "engine/Exporter.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_WIN
#include <psapi.h>
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace {

double cpuSeconds()
{
#ifdef Q_OS_WIN
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    auto s = [](const FILETIME& f) { return (double(f.dwHighDateTime) * 4294967296.0 + f.dwLowDateTime) / 1e7; };
    return s(kernel) + s(user);
#else
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
#endif
}

// Peak resident memory of this process in MB
double peakMb()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS c{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c))) return 0;
    return c.PeakWorkingSetSize / 1048576.0;
#else
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_maxrss / 1024.0; // kB on Linux
#endif
}

// Format of a media file (ffprobe): size and frame rate; empty on failure
ProjectFormat probeFormat(const QString& path)
{
    QProcess p;
    p.start(Bundle::tool("ffprobe"), {"-v", "error", "-select_streams", "v:0", "-show_entries",
                                      "stream=width,height,avg_frame_rate", "-of", "json", path});
    p.waitForFinished(30000);
    const QJsonObject s = QJsonDocument::fromJson(p.readAllStandardOutput()).object()["streams"].toArray().at(0).toObject();
    ProjectFormat f;
    f.width = s["width"].toInt();
    f.height = s["height"].toInt();
    const QStringList r = s["avg_frame_rate"].toString().split('/');
    if (r.size() == 2 && r[1].toInt() > 0) f.rate = {r[0].toInt(), r[1].toInt()};
    return f;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-export");
    Check::initMlt();
    QStringList args = app.arguments().mid(1);
    auto option = [&](const QString& name, const QString& def) {
        const qsizetype i = args.indexOf(name);
        if (i < 0 || i + 1 >= args.size()) return def;
        const QString v = args[i + 1];
        args.remove(i, 2);
        return v;
    };
    auto flag = [&](const QString& name) { return args.removeAll(name) > 0; };
    const QString codec = option("--codec", "libx264");
    const QString preset = option("--preset", "medium");
    const int crf = option("--crf", "20").toInt();
    const int threads = option("--threads", "0").toInt();
    const double seconds = option("--seconds", "0").toDouble();
    const double outRate = option("--rate", "0").toDouble();
    const QString keep = option("--keep", "");
    bool ref = flag("--ref");
    const bool noRef = flag("--no-ref");
    const bool noAudio = flag("--an"), noVideo = flag("--vn");
    if (args.size() != 1) {
        std::printf("usage: bench_export <project.schneidi | media file> [--codec c] [--preset p] [--crf n] "
                    "[--threads n] [--seconds n] [--rate fps] [--ref | --no-ref] [--an | --vn] [--keep file]\n");
        return 2;
    }
    const QString input = args[0];

    Timeline tl;
    ProjectFormat format;
    QElapsedTimer loadClock;
    loadClock.start();
    if (input.endsWith(".schneidi")) {
        ProjectData data;
        QString error;
        if (!ProjectFile::load(input, &data, &error)) {
            std::printf("cannot load: %s\n", qPrintable(error));
            return 1;
        }
        Project project;
        project.load(data);
        tl = project.timeline();
        format = project.format();
    } else {
        format = probeFormat(input);
        if (format.width <= 0) {
            std::printf("not a video: %s\n", qPrintable(input));
            return 1;
        }
        const MediaInfo m = Engine::probe(format, input);
        Clip v;
        v.id = 1;
        v.mediaPath = input;
        v.in = 0;
        v.out = m.length - 1;
        v.linkId = 1;
        tl.video.resize(1);
        tl.audio.resize(1);
        tl.video[0].clips << v;
        if (m.hasAudio) {
            Clip a = v;
            a.id = 2;
            tl.audio[0].clips << a;
        }
        ref = !noRef;
    }
    const double loadMs = loadClock.elapsed();
    const double fps = format.rate.fps();
    const int frames = seconds > 0 ? std::min(int(seconds * fps), TimelineOps::endFrame(tl)) : TimelineOps::endFrame(tl);

    QTemporaryDir dir;
    ExportSettings s;
    s.path = keep.isEmpty() ? dir.filePath("bench.mp4") : keep;
    s.format = format;
    s.videoCodec = noVideo ? QString() : codec;
    if (noAudio) s.audioCodec.clear();
    s.softwareCodec = codec.startsWith("lib") ? QString() : QString("libx264");
    s.preset = preset;
    s.crf = crf;
    s.threads = threads;
    s.to = frames - 1;
    if (outRate > 0) s.rate = {int(std::lround(outRate * 1000)), 1000};

    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QString message;
    qint64 firstFrameMs = -1;
    QElapsedTimer clock;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString& m) {
        ok = success;
        message = m;
        loop.quit();
    });
    QObject::connect(&ex, &Exporter::progress, &loop, [&](int percent) {
        if (percent > 0 && firstFrameMs < 0) firstFrameMs = clock.elapsed();
    });
    const double cpu0 = cpuSeconds();
    clock.start();
    QString error;
    if (!ex.start(tl, s, &error)) {
        std::printf("export did not start: %s\n", qPrintable(error));
        return 1;
    }
    const qint64 startMs = clock.elapsed();
    QTimer::singleShot(3600 * 1000, &loop, &QEventLoop::quit);
    loop.exec();
    const qint64 totalMs = clock.elapsed();
    const double load = (cpuSeconds() - cpu0) * 1000.0 / std::max<qint64>(1, totalMs);
    if (!ok) {
        std::printf("export failed: %s\n", qPrintable(message));
        return 1;
    }
    const int outFrames = outRate > 0 ? int(std::lround(frames * outRate / fps)) : frames;
    std::printf("input      %s (%dx%d @ %.3f, %d frames, %.1f s)\n", qPrintable(input), format.width, format.height, fps,
                frames, frames / fps);
    std::printf("encoder    %s (%s, crf %d, %d threads)%s\n", qPrintable(ex.usedCodec()), qPrintable(preset), crf,
                threads > 0 ? threads : Exporter::availableThreads(),
                ex.note().isEmpty() ? "" : qPrintable(" – " + ex.note()));
    std::printf("load       %6.0f ms (project file / probe)\n", loadMs);
    std::printf("start()    %6lld ms blocking the GUI thread\n", startMs);
    std::printf("1st frame  %6lld ms\n", firstFrameMs);
    std::printf("schneidi   %6.1f s = %6.1f fps (%.1fx real time), CPU %.1f cores, peak %.0f MB\n", totalMs / 1000.0,
                outFrames * 1000.0 / totalMs, outFrames * 1000.0 / totalMs / (outRate > 0 ? outRate : fps), load, peakMb());
    if (!ref) return 0;

    // FFmpeg alone: decode the same file, encode with the same settings (first file of the timeline)
    const QString media = tl.video.isEmpty() || tl.video[0].clips.isEmpty() ? QString() : tl.video[0].clips[0].mediaPath;
    if (media.isEmpty()) return 0;
    QStringList a{"-v", "error", "-y", "-i", media, "-t", QString::number(frames / fps, 'f', 3), "-c:v", ex.usedCodec()};
    if (ex.usedCodec() == "libx264" || ex.usedCodec() == "libx265") a << "-preset" << preset << "-crf" << QString::number(crf);
    if (threads > 0) a << "-threads" << QString::number(threads);
    if (outRate > 0) a << "-r" << QString::number(outRate);
    if (noVideo) a << "-vn";
    else a << "-pix_fmt" << "yuv420p";
    if (noAudio) a << "-an";
    else a << "-c:a" << "aac" << "-b:a" << "192k";
    a << dir.filePath("ffmpeg.mp4");
    QProcess p;
    QElapsedTimer t;
    t.start();
    p.start(Bundle::tool("ffmpeg"), a);
    p.waitForFinished(-1);
    const qint64 ffMs = t.elapsed();
    if (p.exitCode() != 0) {
        std::printf("ffmpeg     failed: %s\n", p.readAllStandardError().trimmed().constData());
        return 0;
    }
    std::printf("ffmpeg     %6.1f s = %6.1f fps -> schneidi needs %.2fx the time\n", ffMs / 1000.0,
                outFrames * 1000.0 / ffMs, double(totalMs) / ffMs);
    return 0;
}
