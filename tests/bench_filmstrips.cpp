// SPDX-License-Identifier: AGPL-3.0-only
// Filmstrip pass over many files: the ffmpeg process per file (as MediaCache did before) against the own decoder.
//   bench_filmstrips ffmpeg|own[:threads] <file>...   (one file after another, as MediaCache's strip thread works)
//   bench_filmstrips still-ffmpeg|still-own <file>...  Media Storage previews (the picture at 1 s, 144×81)
//   bench_filmstrips compare <file>...                 same pictures as the ffmpeg pass?
// Prints the time, the pictures and the peak memory of this process (VmHWM) and of the largest child (ffmpeg).
#include "engine/video/Thumbnails.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QImage>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
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

// Pictures of the ffmpeg pass, kept if `keep` is given (RGBA, `size` large, in order)
int ffmpegPass(const QString& path, const QSize& size, QByteArray* keep = nullptr)
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
        const QByteArray out = ff.readAllStandardOutput();
        bytes += out.size();
        if (keep) *keep += out;
        ff.readAllStandardError();
    }
    const QByteArray out = ff.readAllStandardOutput();
    bytes += out.size();
    if (keep) *keep += out;
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
    if (args[0] == "compare") { // same pictures? count and mean difference per channel (0..255) per file
        const QSize size(160, 90);
        for (const QString& path : args.mid(1)) {
            QByteArray ref;
            const int n = ffmpegPass(path, size, &ref);
            std::vector<QImage> own;
            Video::keyFrames(path, {size, 4}, [&](double, const QImage& img) {
                own.push_back(img);
                return true;
            });
            double worst = 0;
            const qint64 bytes = qint64(size.width()) * size.height() * 4;
            for (int i = 0; i < std::min(n, int(own.size())); ++i) {
                const QImage r = QImage(reinterpret_cast<const uchar*>(ref.constData() + i * bytes), size.width(), size.height(),
                                        QImage::Format_RGBA8888)
                                     .convertToFormat(QImage::Format_RGB32);
                double sum = 0;
                for (int y = 0; y < size.height(); ++y)
                    for (int x = 0; x < size.width(); ++x) {
                        const QRgb a = r.pixel(x, y), b = own[size_t(i)].pixel(x, y);
                        sum += std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
                    }
                worst = std::max(worst, sum / (3.0 * size.width() * size.height()));
            }
            std::printf("%s: ffmpeg %d, own %d pictures, worst mean difference %.2f\n", qPrintable(path), n, int(own.size()),
                        worst);
        }
        return 0;
    }
    if (args[0].startsWith("still-")) {
        const bool own = args[0] == "still-own";
        QElapsedTimer t;
        t.start();
        int pictures = 0;
        for (const QString& path : args.mid(1)) {
            if (own) {
                pictures += !Video::stillAt(path, 1.0, QSize(144, 81)).isNull();
                continue;
            }
            QProcess ff; // the command Media Storage ran before
            ff.start("ffmpeg", {"-hide_banner", "-nostdin", "-loglevel", "error", "-ss", "1", "-i", path, "-frames:v", "1", "-vf",
                                "scale=144:81:force_original_aspect_ratio=decrease", "-f", "image2pipe", "-c:v", "png", "-"});
            ff.waitForFinished(60000);
            QImage img;
            pictures += img.loadFromData(ff.readAllStandardOutput(), "PNG");
        }
        std::printf("%s: %d files, %d pictures, %.2f s, peak this process %ld MB\n", qPrintable(args[0]), int(args.size() - 1),
                    pictures, t.elapsed() / 1000.0, vmHwmKB() / 1024);
        return 0;
    }
    const bool own = args[0].startsWith("own");
    const QSize size(160, 90);
    int threads = std::clamp(QThread::idealThreadCount() / 2, 1, 4); // as MediaCache
    if (args[0].startsWith("own:")) threads = args[0].mid(4).toInt();
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
    std::printf("%s (%d threads): %d files, %ld pictures, %.2f s, peak this process %ld MB, largest child %ld MB\n", own ? "own" : "ffmpeg", own ? threads : 0,
                int(args.size() - 1), pictures, t.elapsed() / 1000.0, vmHwmKB() / 1024, childKB / 1024);
    return 0;
}
