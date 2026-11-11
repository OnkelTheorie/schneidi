// SPDX-License-Identifier: AGPL-3.0-only
// Media Storage thumbnails, fast: Video::thumbnailAt (the key frame near 1 s decoded alone: same picture and colors as
// stillAt there, turned phone video, short clip, VP9, 10-bit HDR, MJPEG at a lower resolution, intra refresh without key
// frames) and StorageThumbCache (hit, a changed file misses, unwritable folder, pruning old pictures); the Media
// Storage shows pictures from the cache, a broken file keeps its placeholder, a cache it cannot write still works.
#include "check.h"

#include "app/Theme.h"
#include "engine/StorageThumbCache.h"
#include "engine/video/Thumbnails.h"
#include "ui/MediaStorage.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QTemporaryDir>
#include <QThread>

#include <cmath>
#include <functional>

namespace {

const QSize kBox(144, 81);

// Mean difference per channel (0..255), 255 for different sizes
double difference(const QImage& a0, const QImage& b0)
{
    if (a0.isNull() || a0.size() != b0.size()) return 255;
    const QImage a = a0.convertToFormat(QImage::Format_RGB32), b = b0.convertToFormat(QImage::Format_RGB32);
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

bool waitFor(const std::function<bool()>& done, int ms = 30000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

bool isPlaceholder(const QImage& img)
{
    const QColor bg = Theme::thumbBg;
    return img.pixelColor(2, 2) == bg && img.pixelColor(141, 78) == bg;
}

QImage iconOf(QListWidget* list, int i) { return list->item(i)->icon().pixmap(kBox).toImage().convertToFormat(QImage::Format_RGB32); }

void testThumbnailAt(const QTemporaryDir& dir)
{
    auto file = [&](const char* name) { return dir.filePath(name); };
    const QString src = "testsrc2=size=640x360:rate=25";

    // A key frame every second: the one at 1 s, as the exact way finds it (no deblocking: tiny differences)
    const QString h264 = Check::makeMedia(file("h264.mp4"), {"-f", "lavfi", "-i", src, "-t", "4", "-c:v", "libx264", "-g", "25",
                                                             "-bf", "2", "-pix_fmt", "yuv420p"});
    if (CHECK(!h264.isEmpty())) {
        const QImage fast = Video::thumbnailAt(h264, 1.0, kBox);
        CHECK(fast.size() == kBox);
        const double exact = difference(fast, Video::stillAt(h264, 1.0, kBox));
        CHECK(exact < 3);
        CHECK(difference(fast, Video::stillAt(h264, 0, kBox)) > 3 * exact + 0.5); // not the first picture (testsrc2 moves)
    }

    // Long key frame interval: the key frame before 1 s (here the first picture), nothing decoded up to 1 s
    const QString longGop = Check::makeMedia(file("longgop.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25", "-t",
                                                                   "6", "-c:v", "libx264", "-preset", "ultrafast", "-g", "250",
                                                                   "-pix_fmt", "yuv420p"});
    if (CHECK(!longGop.isEmpty())) {
        QElapsedTimer t;
        t.start();
        const QImage fast = Video::thumbnailAt(longGop, 1.0, kBox);
        const qint64 fastMs = t.restart();
        const QImage exact = Video::stillAt(longGop, 1.0, kBox);
        const qint64 exactMs = t.elapsed();
        std::printf("1080p, key frame every 10 s: thumbnailAt %lld ms, stillAt %lld ms\n", fastMs, exactMs);
        CHECK(difference(fast, Video::stillAt(longGop, 0, kBox)) < 3);
        CHECK(!exact.isNull());
    }

    // Shorter than 1 s: the first picture
    const QString shortClip = Check::makeMedia(file("short.mp4"), {"-f", "lavfi", "-i", src, "-t", "0.4", "-c:v", "libx264",
                                                                   "-pix_fmt", "yuv420p"});
    if (CHECK(!shortClip.isEmpty())) CHECK(difference(Video::thumbnailAt(shortClip, 1.0, kBox), Video::stillAt(shortClip, 0, kBox)) < 3);

    // Phone video stored lying (top half blue), shown upright: blue on the left, red on the right, narrow
    const QString lying = Check::makeMedia(
        file("lying.mp4"), {"-f", "lavfi", "-i", "color=red:size=320x180:rate=25,drawbox=x=0:y=0:w=320:h=90:color=blue:t=fill",
                            "-t", "2", "-c:v", "libx264", "-g", "25", "-pix_fmt", "yuv420p"});
    const QString rotated = Check::makeMedia(file("rotated.mp4"), {"-display_rotation", "90", "-i", lying, "-c", "copy"});
    if (CHECK(!rotated.isEmpty())) {
        const QImage img = Video::thumbnailAt(rotated, 1.0, kBox);
        CHECK(img.width() < img.height() && img.height() == kBox.height());
        const QRgb left = img.pixel(img.width() / 2 - 8, img.height() / 2), right = img.pixel(img.width() / 2 + 8, img.height() / 2);
        CHECK(qBlue(left) > 150 && qRed(left) < 100);
        CHECK(qRed(right) > 150 && qBlue(right) < 100);
    }

    // VP9 (WebM)
    const QString vp9 = Check::makeMedia(file("vp9.webm"), {"-f", "lavfi", "-i", src, "-t", "3", "-c:v", "libvpx-vp9", "-deadline",
                                                            "realtime", "-cpu-used", "8", "-g", "25", "-b:v", "1M"});
    if (CHECK(!vp9.isEmpty())) {
        const QImage fast = Video::thumbnailAt(vp9, 1.0, kBox);
        CHECK(fast.size() == kBox);
        CHECK(difference(fast, Video::stillAt(vp9, 1.0, kBox)) < 4);
    }

    // 10-bit HEVC marked BT.2020/PQ: the same colors as the exact way (and not black or washed into gray)
    const QString hdr = Check::makeMedia(
        file("hdr.mkv"), {"-f", "lavfi", "-i", src, "-t", "3", "-c:v", "libx265", "-x265-params", "keyint=25:log-level=error",
                          "-pix_fmt", "yuv420p10le", "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace",
                          "bt2020nc"});
    if (CHECK(!hdr.isEmpty())) {
        const QImage fast = Video::thumbnailAt(hdr, 1.0, kBox);
        CHECK(fast.size() == kBox);
        CHECK(difference(fast, Video::stillAt(hdr, 1.0, kBox)) < 3);
        int colorful = 0;
        for (int x = 0; x < fast.width(); x += 4) {
            const QColor c = fast.pixelColor(x, fast.height() / 2);
            colorful += c.saturation() > 100 && c.value() > 60;
        }
        CHECK(colorful > 5);
    }

    // Full range, BT.601 marked: as stillAt (same conversion)
    const QString full = Check::makeMedia(file("full.mp4"), {"-f", "lavfi", "-i", src, "-t", "2", "-c:v", "libx264", "-g", "25",
                                                             "-pix_fmt", "yuvj420p", "-colorspace", "bt470bg"});
    if (CHECK(!full.isEmpty())) CHECK(difference(Video::thumbnailAt(full, 1.0, kBox), Video::stillAt(full, 1.0, kBox)) < 3);

    // MJPEG: decoded at a quarter of the size (lowres), still the same picture
    const QString mjpeg = Check::makeMedia(file("mjpeg.avi"), {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25", "-t", "2",
                                                               "-c:v", "mjpeg", "-q:v", "3", "-pix_fmt", "yuvj420p"});
    if (CHECK(!mjpeg.isEmpty())) {
        const QImage fast = Video::thumbnailAt(mjpeg, 1.0, kBox);
        CHECK(fast.size() == kBox);
        CHECK(difference(fast, Video::stillAt(mjpeg, 1.0, kBox)) < 6);
    }

    // Intra refresh: no key frame decodes alone -> the exact way still gives a picture
    const QString refresh = Check::makeMedia(file("refresh.mp4"), {"-f", "lavfi", "-i", src, "-t", "3", "-c:v", "libx264",
                                                                   "-x264-params", "intra-refresh=1:keyint=30", "-pix_fmt", "yuv420p"});
    if (CHECK(!refresh.isEmpty())) CHECK(Video::thumbnailAt(refresh, 1.0, kBox).size() == kBox);

    // No picture: audio only, missing or broken file
    const QString audio = Check::makeMedia(file("audio.m4a"), {"-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-c:a", "aac"});
    CHECK(Video::thumbnailAt(audio, 1.0, kBox).isNull());
    CHECK(Video::thumbnailAt(file("missing.mp4"), 1.0, kBox).isNull());
    QFile junk(file("junk.mp4"));
    if (junk.open(QIODevice::WriteOnly)) junk.write(QByteArray(4096, 'x'));
    junk.close();
    CHECK(Video::thumbnailAt(file("junk.mp4"), 1.0, kBox).isNull());
}

void testCache(const QTemporaryDir& dir)
{
    QDir(StorageThumbCache::dir()).removeRecursively();
    const QString path = dir.filePath("cached.bin");
    QFile f(path);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write("one");
    f.close();
    QImage red(144, 81, QImage::Format_RGB32);
    red.fill(Qt::red);
    CHECK(StorageThumbCache::load(QFileInfo(path)).isNull()); // nothing yet, the folder is missing
    CHECK(StorageThumbCache::store(QFileInfo(path), red));
    const QImage back = StorageThumbCache::load(QFileInfo(path));
    CHECK(back.size() == red.size() && difference(back, red) < 4);

    // The file changes (content and time): its old picture is not taken
    QThread::msleep(20);
    if (CHECK(f.open(QIODevice::WriteOnly))) {
        f.write("other content");
        f.close();
    }
    f.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime);
    CHECK(StorageThumbCache::load(QFileInfo(path)).isNull());

    // Old pictures are pruned, used ones stay
    CHECK(StorageThumbCache::store(QFileInfo(path), red));
    const QString oldFile = StorageThumbCache::dir() + "/0000000000000000000000000000000000000000.jpg";
    CHECK(QFile::copy(StorageThumbCache::fileFor(QFileInfo(path)), oldFile));
    {
        QFile old(oldFile);
        if (CHECK(old.open(QIODevice::ReadWrite)))
            old.setFileTime(QDateTime::currentDateTime().addDays(-90), QFileDevice::FileModificationTime);
    }
    CHECK_EQ(StorageThumbCache::prune(true), 1);
    CHECK(!QFile::exists(oldFile));
    CHECK(!StorageThumbCache::load(QFileInfo(path)).isNull());
    CHECK_EQ(StorageThumbCache::prune(), 0); // once a day

    // The cache folder cannot be made (a file in its place): storing fails, nothing breaks
    QDir(StorageThumbCache::dir()).removeRecursively();
    QFile blocker(StorageThumbCache::dir());
    if (CHECK(blocker.open(QIODevice::WriteOnly))) blocker.close();
    CHECK(!StorageThumbCache::store(QFileInfo(path), red));
    CHECK(StorageThumbCache::load(QFileInfo(path)).isNull());
    QFile::remove(StorageThumbCache::dir());
}

void testStorage(const QTemporaryDir& media)
{
    QTemporaryDir dir;
    QFile::copy(media.filePath("h264.mp4"), dir.filePath("a.mp4"));
    QFile::copy(media.filePath("rotated.mp4"), dir.filePath("b.mp4"));
    QFile::copy(media.filePath("junk.mp4"), dir.filePath("c.mp4"));
    QDir(StorageThumbCache::dir()).removeRecursively();
    const auto shown = [&](QListWidget* list) { // a.mp4 and b.mp4 have their pictures, c.mp4 (broken) its "?"
        return !isPlaceholder(iconOf(list, 0)) && !isPlaceholder(iconOf(list, 1));
    };
    {
        MediaStorage storage;
        storage.setPersistent(false);
        storage.resize(700, 400);
        storage.show();
        auto* list = storage.findChild<QListWidget*>();
        if (!CHECK(list)) return;
        storage.setFolder(dir.path());
        CHECK_EQ(list->count(), 3);
        const qint64 junkKey = list->item(2)->icon().cacheKey();
        CHECK(waitFor([&] { return shown(list) && list->item(2)->icon().cacheKey() != junkKey; }));
        CHECK(isPlaceholder(iconOf(list, 2)));
    }
    // Written to disk: two pictures, none for the broken file
    CHECK(QFile::exists(StorageThumbCache::fileFor(QFileInfo(dir.filePath("a.mp4")))));
    CHECK(QFile::exists(StorageThumbCache::fileFor(QFileInfo(dir.filePath("b.mp4")))));
    CHECK(!QFile::exists(StorageThumbCache::fileFor(QFileInfo(dir.filePath("c.mp4")))));

    // A new Media Storage (a restart) takes the stored picture: replaced by green, it shows green
    QImage green(144, 81, QImage::Format_RGB32);
    green.fill(Qt::green);
    CHECK(StorageThumbCache::store(QFileInfo(dir.filePath("a.mp4")), green));
    {
        MediaStorage storage;
        storage.setPersistent(false);
        storage.resize(700, 400);
        storage.show();
        auto* list = storage.findChild<QListWidget*>();
        storage.setFolder(dir.path());
        CHECK(waitFor([&] { return shown(list); }));
        const QColor c = iconOf(list, 0).pixelColor(72, 40);
        CHECK(c.green() > 200 && c.red() < 60);
    }

    // The cache folder cannot be written: the thumbnails still come
    QDir(StorageThumbCache::dir()).removeRecursively();
    QFile blocker(StorageThumbCache::dir());
    if (CHECK(blocker.open(QIODevice::WriteOnly))) blocker.close();
    {
        MediaStorage storage;
        storage.setPersistent(false);
        storage.resize(700, 400);
        storage.show();
        auto* list = storage.findChild<QListWidget*>();
        storage.setFolder(dir.path());
        CHECK(waitFor([&] { return shown(list); }));
        const QColor c = iconOf(list, 0).pixelColor(72, 40);
        CHECK(!(c.green() > 200 && c.red() < 60)); // decoded, not the old green
    }
    QFile::remove(StorageThumbCache::dir());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("storage_thumbs");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();
    QTemporaryDir dir;
    testThumbnailAt(dir);
    testCache(dir);
    testStorage(dir);
    return Check::result();
}
