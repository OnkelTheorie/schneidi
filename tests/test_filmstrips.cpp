// Filmstrips and Media Storage previews inside the program (engine/video/Thumbnails) instead of an ffmpeg process per
// file: the same pictures at the same times as the ffmpeg command line that made them before (h264, hevc, turned phone
// video, variable frame rate, odd size with anamorphic pixels), audio only gives none; MediaCache fills its key frame
// index with them, Media Storage shows the picture at 1 s.
#include "check.h"

#include "engine/MediaCache.h"
#include "engine/video/Thumbnails.h"
#include "ui/MediaStorage.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cmath>
#include <utility>
#include <vector>

namespace {

using Strip = std::vector<std::pair<double, QImage>>;

// The command line MediaCache ran per file before (key frames, square pixels, fitted on black, showinfo times)
Strip ffmpegStrip(const QString& path, const QSize& size)
{
    const QString vf = QString("scale='if(gt(sar,0),iw*sar,iw)':ih,scale=%1:%2:force_original_aspect_ratio=decrease,"
                               "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:black,setsar=1,showinfo")
                           .arg(size.width())
                           .arg(size.height());
    QProcess ff;
    ff.start("ffmpeg", {"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-skip_frame", "nokey", "-i", path,
                        "-map", "0:v:0", "-an", "-sn", "-vf", vf, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt",
                        "rgba", "-"});
    ff.waitForFinished(60000);
    const QByteArray out = ff.readAllStandardOutput();
    static const QRegularExpression re(QStringLiteral("\\bn:\\s*\\d+.*\\bpts_time:(-?[0-9.]+)"));
    std::vector<double> times;
    for (const QString& line : QString::fromUtf8(ff.readAllStandardError()).split('\n'))
        if (const auto m = re.match(line); line.contains("showinfo") && m.hasMatch()) times.push_back(m.captured(1).toDouble());
    const qint64 bytes = qint64(size.width()) * size.height() * 4;
    Strip strip;
    for (size_t i = 0; i < times.size() && qint64(i + 1) * bytes <= out.size(); ++i) {
        const QImage raw(reinterpret_cast<const uchar*>(out.constData() + i * bytes), size.width(), size.height(),
                         QImage::Format_RGBA8888);
        strip.emplace_back(times[i], raw.convertToFormat(QImage::Format_RGB32));
    }
    return strip;
}

Strip ownStrip(const QString& path, const QSize& size, int* result = nullptr)
{
    Strip strip;
    const int n = Video::keyFrames(path, {size, 2}, [&](double t, const QImage& img) {
        strip.emplace_back(t, img);
        return true;
    });
    if (result) *result = n;
    return strip;
}

// Mean difference per channel (0..255)
double difference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size()) return 255;
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* pa = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* pb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            sum += std::abs(qRed(pa[x]) - qRed(pb[x])) + std::abs(qGreen(pa[x]) - qGreen(pb[x]))
                   + std::abs(qBlue(pa[x]) - qBlue(pb[x]));
    }
    return sum / (3.0 * a.width() * a.height());
}

double brightness(const QImage& img)
{
    double sum = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* p = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) sum += qGray(p[x]);
    }
    return sum / std::max(1, img.width() * img.height());
}

void compare(const char* name, const QString& path, const QSize& box, int keys)
{
    const Strip ref = ffmpegStrip(path, box);
    const Strip own = ownStrip(path, box);
    Check::report(int(own.size()) == keys && int(ref.size()) == keys, name, __FILE__, __LINE__,
                  QString("%1 pictures, ffmpeg %2, expected %3").arg(own.size()).arg(ref.size()).arg(keys));
    for (size_t i = 0; i < std::min(own.size(), ref.size()); ++i) {
        const double dt = std::abs(own[i].first - ref[i].first);
        const double diff = difference(own[i].second, ref[i].second);
        const bool ok = dt < 0.002 && own[i].second.size() == box && diff < 3 && brightness(own[i].second) > 15; // black would be 0
        Check::report(ok, name, __FILE__, __LINE__,
                      QString("picture %1: %2 s (ffmpeg %3 s), %4x%5, difference %6, brightness %7")
                          .arg(i)
                          .arg(own[i].first)
                          .arg(ref[i].first)
                          .arg(own[i].second.width())
                          .arg(own[i].second.height())
                          .arg(diff)
                          .arg(brightness(own[i].second)));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("filmstrips");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initMlt();

    QTemporaryDir dir;
    auto file = [&](const char* name) { return dir.filePath(name); };
    const QSize box(160, 90);
    const QString src = "testsrc2=size=640x360:rate=25";

    // H.264, a key frame every second
    const QString h264 = Check::makeMedia(file("h264.mp4"), {"-f", "lavfi", "-i", src, "-t", "4", "-c:v", "libx264", "-g",
                                                             "25", "-bf", "2", "-pix_fmt", "yuv420p"});
    CHECK(!h264.isEmpty());
    compare("h264", h264, box, 4);

    // HEVC in Matroska
    const QString hevc = Check::makeMedia(file("hevc.mkv"), {"-f", "lavfi", "-i", src, "-t", "3", "-c:v", "libx265", "-x265-params",
                                                             "keyint=25:min-keyint=25:log-level=error", "-pix_fmt", "yuv420p"});
    CHECK(!hevc.isEmpty());
    compare("hevc", hevc, box, 3);

    // Phone video: stored lying, shown upright (top half blue -> after a quarter turn one side blue)
    const QString lying = Check::makeMedia(
        file("lying.mp4"), {"-f", "lavfi", "-i", "color=red:size=320x180:rate=25,drawbox=x=0:y=0:w=320:h=90:color=blue:t=fill",
                            "-t", "2", "-c:v", "libx264", "-g", "25", "-pix_fmt", "yuv420p"});
    CHECK(!lying.isEmpty());
    for (const char* turn : {"90", "-90", "180"}) {
        const QString rotated = Check::makeMedia(file(qPrintable(QString("rotated%1.mp4").arg(turn))),
                                                 {"-display_rotation", turn, "-i", lying, "-c", "copy"});
        CHECK(!rotated.isEmpty());
        compare(qPrintable(QString("rotated %1").arg(turn)), rotated, box, 2);
    }
    {
        const QString rotated = file("rotated90.mp4");
        const Strip own = ownStrip(rotated, box);
        CHECK(!own.empty());
        if (!own.empty()) {
            // 90° counter-clockwise: the blue top goes to the left; the picture stands (narrow, black left and right)
            const QImage& img = own[0].second;
            const QRgb left = img.pixel(box.width() / 2 - 12, box.height() / 2), right = img.pixel(box.width() / 2 + 12, box.height() / 2);
            CHECK(qBlue(left) > 150 && qRed(left) < 100);
            CHECK(qRed(right) > 150 && qBlue(right) < 100);
            CHECK(qGray(img.pixel(5, box.height() / 2)) < 10);
        }
    }

    // Variable frame rate: a gap of 21 frames, key frames by count
    const QString vfr = Check::makeMedia(file("vfr.mkv"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30", "-t", "3", "-vf",
                                                           "select='not(between(n,20,40))'", "-fps_mode", "vfr", "-c:v",
                                                           "libx264", "-g", "15", "-pix_fmt", "yuv420p"});
    CHECK(!vfr.isEmpty());
    compare("vfr", vfr, box, int(ffmpegStrip(vfr, box).size()));
    CHECK(ffmpegStrip(vfr, box).size() >= 4);

    // Odd size with anamorphic pixels (4:4:4 allows odd sizes)
    const QString odd = Check::makeMedia(file("odd.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=321x181:rate=25", "-t", "2", "-vf",
                                                           "setsar=4/3", "-c:v", "libx264", "-g", "25", "-pix_fmt", "yuv444p"});
    CHECK(!odd.isEmpty());
    compare("odd size", odd, box, 2);
    compare("odd size, portrait project", odd, QSize(90, 160), 2);

    // Audio only: no pictures (MediaCache then reports the file as failed)
    const QString audio = Check::makeMedia(file("audio.m4a"), {"-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-c:a", "aac"});
    CHECK(!audio.isEmpty());
    {
        int result = 0;
        const Strip own = ownStrip(audio, box, &result);
        CHECK(own.empty());
        CHECK(result == -1);
        CHECK(Video::stillAt(audio, 1.0, QSize(144, 81)).isNull());
    }
    CHECK(Video::keyFrames(file("missing.mp4"), {box, 0}, [](double, const QImage&) { return true; }) == -1);

    // Stopping early
    {
        int calls = 0;
        Video::keyFrames(h264, {box, 0}, [&](double, const QImage&) { return ++calls < 2; });
        CHECK(calls == 2);
    }

    // Media Storage preview: the frame at 1 s (as ffmpeg -ss 1), the first one for clips shorter than that
    {
        const QImage still = Video::stillAt(h264, 1.0, QSize(144, 81));
        CHECK(still.size() == QSize(144, 81));
        QProcess ff;
        ff.start("ffmpeg", {"-hide_banner", "-nostdin", "-loglevel", "error", "-ss", "1", "-i", h264, "-frames:v", "1", "-vf",
                            "scale=144:81:force_original_aspect_ratio=decrease", "-f", "rawvideo", "-pix_fmt", "bgra", "-"});
        ff.waitForFinished(30000);
        const QByteArray raw = ff.readAllStandardOutput();
        CHECK(raw.size() == 144 * 81 * 4);
        if (raw.size() == 144 * 81 * 4) {
            const QImage ref = QImage(reinterpret_cast<const uchar*>(raw.constData()), 144, 81, QImage::Format_RGB32).copy();
            CHECK(difference(still, ref) < 6);
            // not the key frame at 0 s (testsrc2 moves)
            CHECK(difference(Video::stillAt(h264, 0, QSize(144, 81)), ref) > 3 * difference(still, ref));
        }
        const QString shortClip = Check::makeMedia(file("short.mp4"), {"-f", "lavfi", "-i", src, "-t", "0.4", "-c:v", "libx264",
                                                                       "-pix_fmt", "yuv420p"});
        const QImage first = Video::stillAt(shortClip, 1.0, QSize(144, 81));
        CHECK(first.size() == QSize(144, 81) && brightness(first) > 30);
        const QImage upright = Video::stillAt(file("rotated90.mp4"), 1.0, QSize(144, 81));
        CHECK(upright.width() < upright.height()); // turned, without borders
    }

    // Media Storage shows it in its list (in its own thread, the window stays free)
    {
        QTemporaryDir folder;
        QFile::copy(file("rotated90.mp4"), folder.filePath("phone.mp4"));
        MediaStorage storage;
        storage.setPersistent(false);
        storage.resize(500, 300);
        storage.show();
        storage.setFolder(folder.path());
        auto* list = storage.findChild<QListWidget*>();
        if (CHECK(list) && CHECK_EQ(list->count(), 1)) {
            const qint64 before = list->item(0)->icon().cacheKey();
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 20000 && list->item(0)->icon().cacheKey() == before)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            const QImage icon = list->item(0)->icon().pixmap(QSize(144, 81)).toImage().convertToFormat(QImage::Format_RGB32);
            // upright: blue on the left of the middle, red on the right (a placeholder has neither)
            const QRgb left = icon.pixel(icon.width() / 2 - 8, icon.height() / 2);
            const QRgb right = icon.pixel(icon.width() / 2 + 8, icon.height() / 2);
            CHECK(qBlue(left) > 150 && qRed(left) < 100);
            CHECK(qRed(right) > 150 && qBlue(right) < 100);
        }
    }

    // MediaCache: the key frames land in its index at their project frames, the audio file fails
    {
        MediaCache cache;
        cache.setFormat(ProjectFormat{640, 360, {25, 1}});
        cache.thumbnail(h264, 0);
        cache.thumbnail(audio, 0);
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 20000 && cache.cachedThumbnail(h264, 75).isNull()) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        for (int frame : {0, 25, 50, 75}) {
            const QImage img = cache.cachedThumbnail(h264, frame);
            Check::report(img.size() == box && brightness(img) > 30, "MediaCache key frame", __FILE__, __LINE__,
                          QString("frame %1: %2x%3").arg(frame).arg(img.width()).arg(img.height()));
        }
        CHECK(cache.cachedThumbnail(h264, 10).isNull()); // no key frame there
        // between key frames: the one before stands in when it is close enough
        CHECK(cache.thumbnail(h264, 30, 10).size() == box);
    }
    return Check::result();
}
