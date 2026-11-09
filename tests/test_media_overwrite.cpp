// SPDX-License-Identifier: AGPL-3.0-only
// A media file overwritten under the same name (re-rendered elsewhere) shows its new content everywhere: caches and
// open readers were keyed by path alone and kept the old picture (FileStamp: size + modification time, looked at
// again at most every kRecheckMs). Each case: write file A (red), read a frame, overwrite it with other content
// (blue, then green) and a different modification time set explicitly, read again -> the new content.
// Covered: a renderer that keeps its source open (viewer, export, comp Media node: Renderer::source), MediaStills
// (thumbnail/trim workers), StillFetcher, MediaCache thumbnails, Engine::checkFiles; stills (PNG) and video.

#include "render_check.h"

#include "engine/FileStamp.h"
#include "engine/MediaCache.h"
#include "engine/StillFetcher.h"
#include "engine/picture/MediaStills.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <clocale>

namespace {

enum class Color { Red, Blue, Green, Other };

Color colorOf(const QImage& img)
{
    if (img.isNull()) return Color::Other;
    const QRgb p = img.pixel(img.width() / 2, img.height() / 2);
    const int r = qRed(p), g = qGreen(p), b = qBlue(p);
    if (r > 180 && g < 80 && b < 80) return Color::Red;
    if (b > 180 && r < 80 && g < 80) return Color::Blue;
    if (g > 100 && r < 80 && b < 80) return Color::Green;
    return Color::Other;
}

const char* name(Color c)
{
    switch (c) {
    case Color::Red: return "red";
    case Color::Blue: return "blue";
    case Color::Green: return "green";
    case Color::Other: break;
    }
    return "other";
}

// Copies `from` over `to` and sets `to`'s modification time `step` seconds after a fixed point (always another one)
bool overwrite(const QString& from, const QString& to, int step)
{
    QFile::remove(to);
    if (!QFile::copy(from, to)) return false;
    QFile f(to);
    if (!f.open(QIODevice::ReadWrite)) return false;
    return f.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000 + step * 100), QFileDevice::FileModificationTime);
}

#define CHECK_COLOR(img, want) \
    do { \
        const Color got_ = colorOf(img); \
        if (!CHECK(got_ == (want))) std::printf("  got %s, want %s\n", name(got_), name(want)); \
    } while (0)

void testFile(const QString& file, const QString& red, const QString& blue, const QString& green)
{
    std::printf("-- %s\n", qPrintable(QFileInfo(file).fileName()));
    ProjectFormat format;
    format.width = 320;
    format.height = 180;
    if (!CHECK(overwrite(red, file, 1))) return;
    FileStamp::now(file);

    // A renderer keeps its source open across frames (viewer, export, the comp Media node all read through it)
    Picture::Options o;
    o.format = format;
    o.mode = Picture::Mode::Still;
    Picture::Renderer renderer(Picture::mediaTimeline(file), o);
    CHECK_COLOR(renderer.render(0).toImage(), Color::Red);
    Picture::MediaStills stills(format);
    CHECK_COLOR(stills.frame(file, 0, QSize(160, 90)), Color::Red);

    // Overwritten: whoever looks at the disk (FileStamp::now: a reader being opened, the app becoming active) makes
    // every reader open it again
    if (!CHECK(overwrite(blue, file, 2))) return;
    FileStamp::now(file);
    CHECK_COLOR(renderer.render(0).toImage(), Color::Blue);
    CHECK_COLOR(stills.frame(file, 0, QSize(160, 90)), Color::Blue);

    // Without anyone asking: the renderer notices by itself once kRecheckMs has passed (no stat per frame before)
    if (!CHECK(overwrite(green, file, 3))) return;
    QTest::qWait(FileStamp::kRecheckMs + 150);
    CHECK_COLOR(renderer.render(0).toImage(), Color::Green);
    CHECK_COLOR(stills.frame(file, 0, QSize(160, 90)), Color::Green);
}

// Waits until fn() gives a picture of `want` (events processed); the last picture's color is reported
bool waitFor(const std::function<QImage()>& fn, Color want, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    Color got = Color::Other;
    while (t.elapsed() < ms) {
        got = colorOf(fn());
        if (got == want) return true;
        QTest::qWait(20);
    }
    std::printf("  got %s, want %s\n", name(got), name(want));
    return false;
}

void testCaches(const QString& file, const QString& red, const QString& blue)
{
    std::printf("-- caches: %s\n", qPrintable(QFileInfo(file).fileName()));
    if (!CHECK(overwrite(red, file, 11))) return;
    FileStamp::now(file);
    ProjectFormat format;
    format.width = 320;
    format.height = 180;

    StillFetcher fetcher;
    fetcher.setFormat(format);
    auto fetched = [&] {
        const QImage img = fetcher.cached(file, 0);
        if (img.isNull()) fetcher.request({{file, 0}});
        return img;
    };
    CHECK(waitFor(fetched, Color::Red));

    MediaCache cache;
    cache.setFormat(format);
    auto thumb = [&] { return cache.thumbnail(file, 0); };
    CHECK(waitFor(thumb, Color::Red));

    if (!CHECK(overwrite(blue, file, 12))) return;
    FileStamp::now(file);
    CHECK(fetcher.cached(file, 0).isNull()); // dropped at once
    CHECK(waitFor(fetched, Color::Blue));
    CHECK(waitFor(thumb, Color::Blue));
}

void testEngine(Engine& engine, const QString& file, const QString& red, const QString& blue)
{
    std::printf("-- engine\n");
    if (!CHECK(overwrite(red, file, 21))) return;
    FileStamp::now(file);
    engine.updateTimeline(Picture::mediaTimeline(file));
    int seen = 0;
    const int id = FileStamp::addObserver([&](const QString& path) { seen += path == file; });
    engine.checkFiles();
    CHECK_EQ(seen, 0); // nothing changed
    if (CHECK(overwrite(blue, file, 22))) {
        engine.checkFiles(); // as when the app becomes active again
        CHECK_EQ(seen, 1);
    }
    FileStamp::removeObserver(id);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("media-overwrite");
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir dir;
    if (!CHECK(dir.isValid())) return Check::result();

    // Stills: the same size and format, only the color (and the modification time) differs
    QStringList png;
    for (const auto& [n, c] : {std::pair{"red", Qt::red}, {"blue", Qt::blue}, {"green", Qt::green}}) {
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(c);
        png << dir.filePath(QString("%1.png").arg(n));
        CHECK(img.save(png.last()));
    }
    testFile(dir.filePath("still.png"), png[0], png[1], png[2]);
    testCaches(dir.filePath("still-cache.png"), png[0], png[1]);

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testEngine(engine, dir.filePath("engine.png"), png[0], png[1]);

    if (!Check::haveFfmpeg()) {
        std::printf("no ffmpeg: video cases skipped\n");
        return Check::result();
    }
    QStringList mp4;
    for (const char* c : {"red", "blue", "green"}) {
        mp4 << Check::makeMedia(dir.filePath(QString("%1.mp4").arg(c)),
                                {"-f", "lavfi", "-i", QString("color=c=%1:size=320x180:rate=25:duration=1").arg(c),
                                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
        if (!CHECK(!mp4.last().isEmpty())) return Check::result();
    }
    testFile(dir.filePath("clip.mp4"), mp4[0], mp4[1], mp4[2]);
    testCaches(dir.filePath("clip-cache.mp4"), mp4[0], mp4[1]);
    return Check::result();
}
