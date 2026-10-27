// Filmstrip pass over many files: the ffmpeg process per file (as MediaCache did before) against the own decoder.
//   bench_filmstrips ffmpeg|own <file>...   (one file after another, as MediaCache's strip thread works)
// Prints the time, the pictures and the peak memory of this process (VmHWM) and of the largest child (ffmpeg).
#include "engine/video/Thumbnails.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QThread>

#include <algorithm>
#include <cstdio>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace {

long vmHwmKB()
{
    QFile f("/proc/self/status");
    if (!f.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray& line : f.readAll().split('\n'))
        if (line.startsWith("VmHWM:")) return line.mid(6).trimmed().split(' ').value(0).toLong();
    return -1;
}

int ffmpegPass(const QString& path, const QSize& size)
{
    const QString vf = QString("scale='if(gt(sar,0),iw*sar,iw)':ih,scale=%1:%2:force_original_aspect_ratio=decrease,"
                               "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:black,setsar=1,showinfo")
                           .arg(size.width())
                           .arg(size.height());
    QProcess ff;
    ff.start("ffmpeg", {"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-skip_frame", "nokey", "-i", path,
                        "-map", "0:v:0", "-an", "-sn", "-vf", vf, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt",
                        "rgba", "-"});
    qint64 bytes = 0;
    ff.waitForStarted();
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(100);
        bytes += ff.readAllStandardOutput().size();
        ff.readAllStandardError();
    }
    bytes += ff.readAllStandardOutput().size();
    return int(bytes / (qint64(size.width()) * size.height() * 4));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.size() < 2) {
        std::fprintf(stderr, "bench_filmstrips ffmpeg|own <file>...\n");
        return 2;
    }
    const bool own = args[0] == "own";
    const QSize size(160, 90);
    const int threads = std::clamp(QThread::idealThreadCount() / 2, 1, 4); // as MediaCache
    QElapsedTimer t;
    t.start();
    long pictures = 0;
    for (const QString& path : args.mid(1)) {
        if (own)
            pictures += std::max(0, Video::keyFrames(path, {size, threads}, [](double, const QImage&) { return true; }));
        else
            pictures += ffmpegPass(path, size);
    }
    long childKB = -1;
#ifdef Q_OS_UNIX
    rusage children{};
    getrusage(RUSAGE_CHILDREN, &children);
    childKB = children.ru_maxrss;
#endif
    std::printf("%s: %d files, %ld pictures, %.2f s, peak this process %ld MB, largest child %ld MB\n", own ? "own" : "ffmpeg",
                int(args.size() - 1), pictures, t.elapsed() / 1000.0, vmHwmKB() / 1024, childKB / 1024);
    return 0;
}
