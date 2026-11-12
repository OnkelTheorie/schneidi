// SPDX-License-Identifier: AGPL-3.0-only
// A media file overwritten under the same name (re-rendered elsewhere) shows its new content everywhere: caches and
// open readers were keyed by path alone and kept the old picture (FileStamp: size + modification time, looked at
// again at most every kRecheckMs). Each case: write file A (red), read a frame, overwrite it with other content
// (blue, then green) and a different modification time set explicitly, read again -> the new content.
// Covered: a renderer that keeps its source open (viewer, export, comp Media node: Renderer::source), MediaStills
// (thumbnail/trim workers), StillFetcher, MediaCache thumbnails, Engine::checkFiles; stills (PNG) and video; the sound
// mix (Audio::Player::reopenFiles: a new mix reads the new samples). A file that still grows is no change until it
// has held still for the settle time (kSettleMs, shortened here); observers may add/remove observers from inside. A
// source lost and back as it was shows again on a later request (no change of its stamp).

#include "render_check.h"

#include "engine/FileStamp.h"
#include "engine/MediaCache.h"
#include "engine/StillFetcher.h"
#include "engine/audio/Player.h"
#include "engine/picture/MediaStills.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <clocale>
#include <cmath>
#include <thread>

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

constexpr int kSettle = 300; // settle time in this test (FileStamp::setSettleMsForTest)

// A change counts once it has held still: look, wait the settle time, look again
void settle(const QString& file)
{
    FileStamp::now(file);
    QTest::qWait(kSettle + 50);
    FileStamp::now(file);
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
    CHECK_COLOR(renderer.render(0).toImage(), Color::Red); // not settled yet: could still be being written
    settle(file);
    CHECK_COLOR(renderer.render(0).toImage(), Color::Blue);
    CHECK_COLOR(stills.frame(file, 0, QSize(160, 90)), Color::Blue);

    // Without anyone asking: the renderer notices by itself (no stat per frame: it looks every kRecheckMs, sees the
    // change, and takes it once it has held still at a later look)
    if (!CHECK(overwrite(green, file, 3))) return;
    QTest::qWait(FileStamp::kRecheckMs + 150);
    renderer.render(0);
    QTest::qWait(std::max(FileStamp::kRecheckMs, kSettle) + 150);
    CHECK_COLOR(renderer.render(0).toImage(), Color::Green);
    CHECK_COLOR(stills.frame(file, 0, QSize(160, 90)), Color::Green);
}

// Review 2026-11-09: the file read in two lanes (V1 and V2): a change seen in one drops the other's source of the old
// content too (mediaLength takes the length from any lane: a stale one brought the old length back)
void testLanes(const QString& file, const QString& red, const QString& blue)
{
    std::printf("-- lanes\n");
    if (!CHECK(overwrite(red, file, 41))) return;
    FileStamp::now(file);
    Timeline tl;
    tl.video.resize(2);
    for (int i = 0; i < 2; ++i) {
        Clip c;
        c.id = i + 1;
        c.mediaPath = file;
        c.start = i * 25;
        c.out = 24;
        tl.video[i].kind = TrackKind::Video;
        tl.video[i].clips << c;
    }
    Picture::Options o;
    o.format.width = 320;
    o.format.height = 180;
    o.mode = Picture::Mode::Still;
    Picture::Renderer renderer(tl, o);
    CHECK_COLOR(renderer.render(0).toImage(), Color::Red);
    CHECK_COLOR(renderer.render(30).toImage(), Color::Red);
    CHECK_EQ(renderer.stats().sourcesOpen, 2);
    if (!CHECK(overwrite(blue, file, 42))) return;
    settle(file);
    CHECK_COLOR(renderer.render(0).toImage(), Color::Blue);
    CHECK_EQ(renderer.stats().sourcesOpen, 1); // V2's source of the red file is gone as well
    CHECK_COLOR(renderer.render(30).toImage(), Color::Blue);
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

// Review 2026-11-09: thumbnails, waveforms and trim stills are asked for on the UI thread: their look at the disk
// (a stat, slow on a network share) runs on a pool thread - the change is seen there
void testLookLater(const QString& file, const QString& red, const QString& blue)
{
    std::printf("-- look later\n");
    if (!CHECK(overwrite(red, file, 61))) return;
    FileStamp::now(file);
    QThread* seenOn = nullptr;
    int seen = 0;
    const int id = FileStamp::addObserver([&](const QString& path) {
        if (path != file) return;
        ++seen;
        seenOn = QThread::currentThread();
    });
    if (CHECK(overwrite(blue, file, 62))) {
        MediaCache cache;
        QElapsedTimer t;
        t.start();
        while (seen == 0 && t.elapsed() < 5000) {
            cache.thumbnail(file, 0); // looks at most every kRecheckMs, settles after kSettle
            QTest::qWait(50);
        }
        CHECK_EQ(seen, 1);
        CHECK(seenOn && seenOn != QThread::currentThread());
    }
    FileStamp::removeObserver(id);
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
    settle(file);
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
        engine.checkFiles(); // as when the app becomes active again: seen changing, not settled yet
        CHECK_EQ(seen, 0);
        // The engine looks again by itself once it may have settled
        QElapsedTimer t;
        t.start();
        while (seen == 0 && t.elapsed() < FileStamp::kSettleMs + 3000) QTest::qWait(50);
        CHECK_EQ(seen, 1);
    }
    FileStamp::removeObserver(id);
}

// Review 2026-11-09: an audio-only file nobody stamped (the sound mix opens readers without a stamp), overwritten
// while the app was in the background: stamped when the timeline came, so the next look sees the change
void testUnstamped(Engine& engine, const QString& file, const QString& loud, const QString& quiet)
{
    std::printf("-- unstamped\n");
    if (!CHECK(overwrite(loud, file, 51))) return;
    FileStamp::forget();
    Timeline tl;
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = file;
    c.out = 50;
    tl.audio[0].kind = TrackKind::Audio;
    tl.audio[0].clips << c;
    engine.updateTimeline(tl);
    QElapsedTimer t;
    t.start();
    while (!FileStamp::known(file) && t.elapsed() < 3000) QTest::qWait(10);
    CHECK(FileStamp::known(file));
    int seen = 0;
    const int id = FileStamp::addObserver([&](const QString& path) { seen += path == file; });
    if (CHECK(overwrite(quiet, file, 52))) {
        engine.checkFiles(); // the app active again
        t.restart();
        while (seen == 0 && t.elapsed() < FileStamp::kSettleMs + 3000) QTest::qWait(50);
        CHECK_EQ(seen, 1);
    }
    FileStamp::removeObserver(id);
}

// A recording that still grows: no change while it grows (readers are not reopened every look), exactly one once it
// has stopped
void testGrowing(const QString& file)
{
    std::printf("-- growing\n");
    QFile f(file);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write(QByteArray(1000, 'x'));
    f.flush();
    FileStamp::now(file);
    int seen = 0;
    const int id = FileStamp::addObserver([&](const QString& path) { seen += path == file; });
    for (int i = 0; i < 8; ++i) { // longer than the settle time in all, each step shorter
        f.write(QByteArray(1000, 'y'));
        f.flush();
        QTest::qWait(kSettle / 3);
        FileStamp::now(file);
        CHECK(FileStamp::changing(file));
    }
    CHECK_EQ(seen, 0);
    f.close();
    settle(file);
    CHECK_EQ(seen, 1);
    CHECK(!FileStamp::changing(file));
    settle(file); // nothing new
    CHECK_EQ(seen, 1);
    FileStamp::removeObserver(id);
}

// Review 2026-11-09: a file that goes missing for longer than the settle time (a NAS dropping out) is no change -
// caches and the preview's proxy stay; back as it was: still none; back with other content: one
void testMissing(const QString& file)
{
    std::printf("-- missing\n");
    QFile f(file);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write("abc");
    f.close();
    FileStamp::now(file);
    const FileStamp::Stamp before = FileStamp::now(file);
    int seen = 0;
    const int id = FileStamp::addObserver([&](const QString& path) { seen += path == file; });
    const QString away = file + ".away";
    CHECK(QFile::rename(file, away));
    settle(file);
    settle(file);
    CHECK_EQ(seen, 0);
    CHECK(FileStamp::now(file) == before); // readers keep comparing with the stamp they opened
    CHECK(!FileStamp::changing(file));
    CHECK(QFile::rename(away, file));
    settle(file);
    CHECK_EQ(seen, 0); // back as it was
    CHECK(QFile::remove(file));
    settle(file);
    if (CHECK(f.open(QIODevice::WriteOnly))) {
        f.write("other content");
        f.close();
    }
    settle(file);
    CHECK_EQ(seen, 1);
    FileStamp::removeObserver(id);
}

// A source lost while the project is open (a USB disk pulled, a share gone, the file moved away) and back as it was:
// FileStamp sees no change then, so the broken source used to stay - the preview black until the file changed. Now a
// broken source is opened again on a later request (play, seek; at most every 1.5 s), and the preview is told the file.
void testLostAndBack(const QString& file, const QString& red)
{
    std::printf("-- lost and back\n");
    if (!CHECK(overwrite(red, file, 60))) return;
    FileStamp::now(file);
    ProjectFormat format;
    format.width = 320;
    format.height = 180;
    Picture::Options o;
    o.format = format;
    o.mode = Picture::Mode::Preview;
    QStringList lost; // Options::sourceLost (a rendering thread; here the calling one)
    o.sourceLost = [&lost](const QString& path) { lost << path; };
    Picture::Renderer renderer(Picture::mediaTimeline(file), o);
    CHECK_COLOR(renderer.render(0).toImage(), Color::Red);
    const QString away = file + ".away";
    if (!CHECK(QFile::rename(file, away))) return;
    // Its decoder closed meanwhile (here: the clip left the timeline a while, then came back by undo; a USB disk pulled
    // while it is open fails its reads - Video::Reader::failed - and counts the same)
    renderer.update(Timeline{});
    renderer.trim(std::chrono::steady_clock::now() + std::chrono::minutes(5));
    CHECK_EQ(renderer.stats().sourcesOpen, 0);
    renderer.update(Picture::mediaTimeline(file));
    CHECK(colorOf(renderer.render(0).toImage()) != Color::Red); // gone: nothing to show
    CHECK_EQ(lost, QStringList{file});
    CHECK(QFile::rename(away, file)); // back as it was: the same stamp
    FileStamp::now(file);
    CHECK(colorOf(renderer.render(0).toImage()) != Color::Red); // tried a moment ago: not again yet
    CHECK_EQ(lost.size(), 1); // and not told again
    QTest::qWait(1600);
    CHECK(colorOf(renderer.render(0).toImage()) != Color::Red); // no seek or play since: not on every request
    renderer.retryLost(); // a seek (Engine::seek) or play start
    CHECK_COLOR(renderer.render(0).toImage(), Color::Red); // the next request opens it again
    CHECK_EQ(lost.size(), 1);

    // A file that never opened (offline media, an unreadable import) is no lost one: not told, not retried
    Timeline offlineTl = Picture::mediaTimeline(file);
    offlineTl.video[0].clips[0].mediaPath = file + ".never.mp4";
    Picture::Renderer offline(offlineTl, o);
    offline.render(0);
    QTest::qWait(1600);
    offline.retryLost();
    offline.render(0);
    CHECK_EQ(lost.size(), 1);
}

// Observers are called without a FileStamp lock held: one may add another and remove itself from inside
void testObservers(const QString& file)
{
    std::printf("-- observers\n");
    QFile f(file);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write("a");
    f.close();
    FileStamp::now(file);
    int first = 0, second = 0, secondId = 0, firstId = 0;
    firstId = FileStamp::addObserver([&](const QString& path) {
        if (path != file) return;
        ++first;
        secondId = FileStamp::addObserver([&](const QString& p) { second += p == file; });
        FileStamp::removeObserver(firstId);
    });
    if (CHECK(f.open(QIODevice::Append))) {
        f.write("bb");
        f.close();
    }
    settle(file);
    CHECK_EQ(first, 1);
    if (CHECK(f.open(QIODevice::Append))) {
        f.write("ccc");
        f.close();
    }
    settle(file);
    CHECK_EQ(first, 1); // removed itself
    CHECK_EQ(second, 1);
    FileStamp::removeObserver(secondId);
}

// The sound mix reads the new samples of an overwritten WAV (it kept its readers across live updates)
void testSound(const QString& file, const QString& loud, const QString& quiet)
{
    std::printf("-- sound\n");
    if (!CHECK(overwrite(loud, file, 31))) return;
    FileStamp::now(file);
    Timeline tl;
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = file;
    c.out = 90;
    tl.audio[0].clips << c;
    Audio::Player player;
    player.setTimeline(tl, FrameRate{30, 1});
    // Master peak (the last two values) of a short stretch from the start
    auto peak = [&] {
        player.takeLevels();
        player.play(0, 1.0);
        player.release();
        QTest::qWait(400);
        const std::vector<float> l = player.takeLevels();
        player.pause();
        QTest::qWait(50);
        return l.size() >= 2 ? std::max(l[l.size() - 1], l[l.size() - 2]) : 0.f;
    };
    const float before = peak();
    CHECK(before > 0.6f);
    if (!CHECK(overwrite(quiet, file, 32))) return;
    player.setTimeline(tl, FrameRate{30, 1}); // the same timeline: a live update keeps the readers
    player.reopenFiles();                     // what Engine::mediaFileChanged does
    QTest::qWait(100);
    const float after = peak();
    if (!CHECK(after > 0.05f && after < 0.25f)) std::printf("  peak before %.3f, after %.3f\n", before, after);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("media-overwrite");
    std::setlocale(LC_NUMERIC, "C");
    FileStamp::setSettleMsForTest(kSettle);
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
    testLanes(dir.filePath("lanes.png"), png[0], png[1]);
    testLookLater(dir.filePath("later.png"), png[0], png[1]);

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testEngine(engine, dir.filePath("engine.png"), png[0], png[1]);
    testGrowing(dir.filePath("growing.mkv"));
    testObservers(dir.filePath("observed.bin"));
    testMissing(dir.filePath("missing.bin"));

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
    testLostAndBack(dir.filePath("lost.mp4"), mp4[0]);

    QStringList wav;
    for (const char* amp : {"0.8", "0.15"}) {
        wav << Check::makeMedia(dir.filePath(QString("tone%1.wav").arg(amp)),
                                {"-f", "lavfi", "-i", QString("aevalsrc=%1*sin(2*PI*440*t)|%1*sin(2*PI*440*t):s=48000:d=3")
                                                          .arg(amp),
                                 "-c:a", "pcm_s16le"});
        if (!CHECK(!wav.last().isEmpty())) return Check::result();
    }
    testSound(dir.filePath("sound.wav"), wav[0], wav[1]);
    testUnstamped(engine, dir.filePath("unstamped.wav"), wav[0], wav[1]);
    return Check::result();
}
