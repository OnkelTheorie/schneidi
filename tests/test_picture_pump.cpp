// SPDX-License-Identifier: AGPL-3.0-only
// Frame pump of the preview (engine/picture/Pump) with a synthetic clock and content whose cost is set by the test
// (it sleeps, so parallel load does not matter): picture follows the clock under load, catches up on a machine too
// slow for the material (a slideshow in sync), seek latency when stopped and no frame from before a seek afterwards,
// live content changes during playback without stopping, backward playback. Then the decoder's backward read
// (Video::Reader::setBehind): playing a long-GOP file backwards decodes each GOP once instead of once per frame.
#include "check.h"

#include "engine/Engine.h"
#include "engine/picture/MediaStills.h"
#include "engine/picture/Pump.h"
#include "engine/video/Reader.h"

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr double kFps = 25;

// A clock running at `speed` from `start` (content frames) once started; NaN while stopped
struct TestClock {
    std::mutex m;
    bool running = false;
    double start = 0, speed = 1;
    QElapsedTimer t;
    void play(double from, double s)
    {
        std::lock_guard lock(m);
        start = from;
        speed = s;
        running = true;
        t.start();
    }
    void stop()
    {
        std::lock_guard lock(m);
        running = false;
    }
    double now()
    {
        std::lock_guard lock(m);
        return running ? start + speed * t.nsecsElapsed() / 1e9 * kFps : NAN;
    }
};

// What the Show callback saw
struct Seen {
    std::mutex m;
    struct Entry {
        int64_t position;
        double clock;
        int color; // red channel of the image = content generation
    };
    std::vector<Entry> shown;
    void add(const QImage& img, int64_t pos, double clock)
    {
        std::lock_guard lock(m);
        shown.push_back({pos, clock, img.isNull() ? -1 : qRed(img.pixel(0, 0))});
    }
    std::vector<Entry> take()
    {
        std::lock_guard lock(m);
        return std::exchange(shown, {});
    }
    size_t count()
    {
        std::lock_guard lock(m);
        return shown.size();
    }
};

std::shared_ptr<Picture::Pump::Content> content(std::atomic<int>* costMs, std::atomic<int>* color, int64_t last = 10000)
{
    auto c = std::make_shared<Picture::Pump::Content>();
    c->render = [costMs, color](int64_t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(costMs->load()));
        QImage img(8, 8, QImage::Format_RGBA8888);
        img.fill(QColor(color->load(), 0, 0));
        return img;
    };
    c->last = [last] { return last; };
    c->fps = kFps;
    return c;
}

// Seconds the shown frames were behind the clock at the moment they were shown (forward: f due at clock f)
struct Lag {
    double worst = 0, mean = 0;
    int backwards = 0, frames = 0;
};

Lag lagOf(const std::vector<Seen::Entry>& shown, int direction)
{
    Lag l;
    int64_t prev = -1;
    double sum = 0;
    for (const Seen::Entry& e : shown) {
        const double behind = (direction > 0 ? e.clock - e.position : e.position + 1 - e.clock) / kFps;
        l.worst = std::max(l.worst, behind);
        sum += behind;
        if (prev >= 0 && direction * (e.position - prev) <= 0) ++l.backwards;
        prev = e.position;
        ++l.frames;
    }
    l.mean = l.frames ? sum / l.frames : 0;
    return l;
}

void print(const char* what, const Lag& l)
{
    std::printf("       %-34s %3d Bilder, hinter der Uhr max %.3f s, Mittel %.3f s\n", what, l.frames, l.worst, l.mean);
}

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

void testPump()
{
    TestClock clock;
    Seen seen;
    std::atomic<int> cost{15}, color{10};
    Picture::Pump pump([&] { return clock.now(); },
                       [&](const QImage& img, int64_t pos, double) { seen.add(img, pos, clock.now()); }, 2);

    // Stopped: the frame at the position is shown once
    pump.setContent(content(&cost, &color), 40, 0);
    CHECK(waitFor([&] { return seen.count() == 1; }, 1000));
    std::this_thread::sleep_for(100ms);
    auto s = seen.take();
    CHECK_EQ(int(s.size()), 1);
    CHECK(!s.empty() && s[0].position == 40);

    // Seek latency (stopped): render cost + a little; the newest of quick seeks wins
    QElapsedTimer t;
    t.start();
    pump.seek(100);
    CHECK(waitFor([&] { return seen.count() >= 1; }, 1000));
    const double latency = t.nsecsElapsed() / 1e6;
    std::printf("       Sprung angehalten: %.1f ms (Bild kostet %d ms)\n", latency, cost.load());
    CHECK(latency < cost + 40);
    seen.take();
    for (int p = 200; p < 260; p += 5) {
        pump.seek(p);
        std::this_thread::sleep_for(3ms);
    }
    std::this_thread::sleep_for(150ms);
    s = seen.take();
    CHECK(!s.empty() && s.back().position == 255);

    // Shown frames are kept for stepping back; memory given back while idle drops them
    CHECK(pump.stats().recentBytes > 0);
    pump.dropRecent();
    CHECK_EQ(pump.stats().recentBytes, int64_t(0));

    // Playing under load (cost 15 ms, 2 workers, 25 fps): every frame, in time
    clock.play(255, 1);
    pump.setSpeed(1, 255);
    std::this_thread::sleep_for(2000ms);
    Lag l = lagOf(seen.take(), 1);
    print("vorwärts unter Last:", l);
    CHECK(l.frames >= 45);
    CHECK(l.worst < 0.06);
    CHECK_EQ(l.backwards, 0);

    // Live change while playing: new picture within a few frames, playback goes on
    color = 200;
    pump.refresh();
    std::this_thread::sleep_for(600ms);
    s = seen.take();
    l = lagOf(s, 1);
    print("Änderung beim Abspielen:", l);
    CHECK(l.frames >= 12);
    CHECK(!s.empty() && s.back().color == 200);
    int stale = 0;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i].color != 200) stale = int(i) + 1;
    std::printf("       alte Bilder nach der Änderung: %d\n", stale);
    CHECK(stale <= 3);

    // Seek while playing: nothing from before the seek afterwards
    pump.seek(1000);
    clock.play(1000, 1);
    std::this_thread::sleep_for(500ms);
    s = seen.take();
    bool before = false;
    for (const auto& e : s) before = before || e.position < 1000;
    CHECK(!before);
    CHECK(s.size() >= 8);

    // Too slow for the material (120 ms per frame, 2 workers = ~17 fps for 25): a slideshow in sync, not behind
    cost = 120;
    std::this_thread::sleep_for(3000ms);
    l = lagOf(seen.take(), 1);
    print("zu langsam (120 ms/Bild):", l);
    CHECK(l.worst < 1.0);
    CHECK(l.mean < 0.6);
    CHECK_EQ(l.backwards, 0);
    CHECK(l.frames >= 10);
    std::printf("       Sprünge: %d, geschätzte Kosten %.3f s\n", pump.stats().jumps, pump.stats().jumpCost);

    // Backwards
    cost = 15;
    const int64_t from = 2000;
    pump.setSpeed(0, from);
    clock.stop();
    std::this_thread::sleep_for(100ms);
    seen.take();
    clock.play(from + 1, -1);
    pump.setSpeed(-1, from);
    std::this_thread::sleep_for(1500ms);
    l = lagOf(seen.take(), -1);
    print("rückwärts:", l);
    CHECK(l.frames >= 30);
    CHECK(l.worst < 0.06);
    CHECK_EQ(l.backwards, 0);

    // Shuttle 4x: every 4th frame
    clock.play(500, 4);
    pump.setSpeed(0, 500);
    pump.setSpeed(4, 500);
    std::this_thread::sleep_for(1000ms);
    s = seen.take();
    int steps = 0;
    for (size_t i = 1; i < s.size(); ++i) steps += s[i].position - s[i - 1].position == 4;
    std::printf("       4x: %zu Bilder, %d Schritte zu 4\n", s.size(), steps);
    CHECK(s.size() >= 15);
    CHECK(steps >= int(s.size()) - 3);

    // Stopping stays on the frame given
    clock.stop();
    pump.setSpeed(0, 777);
    CHECK(waitFor([&] {
        std::lock_guard lock(seen.m);
        return !seen.shown.empty() && seen.shown.back().position == 777;
    }, 1000));
}

// Playing a long-GOP file backwards: with the decoder keeping the GOP (setBehind), each GOP is decoded once
// A scrub's close picture (the key frame before the position) that finishes only after playback started: it was
// shown as the played frame and kept among the recent ones as exact, so a later still there showed it for good
void testScrubThenPlay()
{
    std::mutex m;
    std::vector<std::pair<int64_t, int>> shown; // position, red
    std::atomic<bool> release{false};
    Picture::Pump pump([] { return NAN; }, [&](const QImage& img, int64_t pos, double) {
        std::lock_guard l(m);
        shown.push_back({pos, img.isNull() ? -1 : qRed(img.pixel(0, 0))});
    }, 2);
    auto make = [](int red) {
        QImage i(4, 4, QImage::Format_RGBA8888);
        i.fill(QColor(red, 0, 0));
        return i;
    };
    auto c = std::make_shared<Picture::Pump::Content>();
    c->render = [&](int64_t) { return make(100); };
    c->renderWant = [&](int64_t, Picture::Pump::Want& w) {
        if (w.nearest) { // close picture = red 200, held back until playback started
            while (!release.load()) std::this_thread::sleep_for(1ms);
            w.exact = false;
            return make(200);
        }
        w.exact = true;
        return make(100);
    };
    c->last = [] { return int64_t(1000); };
    c->fps = 25;
    pump.setContent(c, 0, 0);
    std::this_thread::sleep_for(100ms);
    pump.seek(10, false);
    std::this_thread::sleep_for(50ms);
    pump.seek(20, true);
    std::this_thread::sleep_for(50ms);
    pump.setSpeed(1, 20);
    release = true;
    std::this_thread::sleep_for(300ms);
    pump.setSpeed(0, 25);
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard l(m);
        shown.clear();
    }
    pump.seek(20, false);
    CHECK(waitFor([&] {
        std::lock_guard l(m);
        return !shown.empty();
    }, 1000));
    std::lock_guard l(m);
    CHECK(!shown.empty() && shown.back().first == 20 && shown.back().second == 100); // the exact picture
}

void testReaderBackward(const QString& file)
{
    for (const bool keep : {false, true}) {
        auto r = Video::Reader::open(file);
        if (!CHECK(r != nullptr)) return;
        if (keep) r->setBehind(120);
        QElapsedTimer t;
        t.start();
        int missing = 0;
        for (int p = 149; p >= 0; --p) {
            auto pic = r->picture(p);
            if (!pic || pic->position != p) ++missing;
        }
        const double ms = t.nsecsElapsed() / 1e6;
        const Video::Reader::Stats st = r->stats();
        std::printf("       rückwärts 150 Bilder (GOP 50), %s: %.0f ms, %d Sprünge, %d dekodiert\n",
                    keep ? "GOP behalten" : "ohne", ms, st.seeks, st.decoded);
        CHECK_EQ(missing, 0);
        if (keep) {
            CHECK(st.seeks <= 4);
            CHECK(st.decoded <= 2 * 150);
        }
    }
}

// Stills of media files through the own engine (thumbnails, trim view, Media Pool tiles)
void testStills(const QString& wide, const QString& square, const QString& notVideo)
{
    const ProjectFormat f{640, 360, {25, 1}};
    Picture::MediaStills stills(f);
    const QImage a = stills.frame(wide, 30, QSize(320, 180));
    CHECK(!a.isNull() && a.size() == QSize(320, 180));
    CHECK(stills.frame(notVideo, 0, QSize(320, 180)).isNull());
    // A square file in a 16:9 project: black bars left and right, the picture in the middle
    const QImage b = stills.frame(square, 10, QSize(320, 180));
    if (CHECK(!b.isNull())) {
        CHECK_EQ(qGray(b.pixel(10, 90)), 0);
        CHECK(qGray(b.pixel(160, 90)) > 0 || qRed(b.pixel(160, 90)) > 0 || qBlue(b.pixel(160, 90)) > 0);
    }
    // Engine::thumbnail: in the project's shape (bars) or in the file's own shape (fills a square tile)
    const QImage t1 = Engine::thumbnail(f, square, 10, QSize(160, 160), false);
    const QImage t2 = Engine::thumbnail(f, square, 10, QSize(160, 160), true);
    if (CHECK(!t1.isNull() && !t2.isNull())) {
        CHECK(t1.size() == QSize(160, 160) && t2.size() == QSize(160, 160));
        CHECK_EQ(qGray(t1.pixel(80, 5)), 0); // 16:9 picture in a square: bars above
        CHECK(qGray(t2.pixel(80, 5)) > 0 || qRed(t2.pixel(80, 5)) > 0 || qBlue(t2.pixel(80, 5)) > 0);
    }
}

} // namespace

// Shuttle faster than 1x towards an end that is not on its step: the last frame (resp. frame 0) is still shown, so the
// engine sees the end and stops (it stood one step before it, "playing" for good)
void testShuttleReachesEnd()
{
    TestClock clock;
    Seen seen;
    std::atomic<int> cost{2}, color{10};
    Picture::Pump pump([&] { return clock.now(); },
                       [&](const QImage& img, int64_t pos, double) { seen.add(img, pos, clock.now()); }, 2);
    pump.setContent(content(&cost, &color, 30), 0, 0);
    CHECK(waitFor([&] { return seen.count() == 1; }, 1000));
    seen.take();
    clock.play(0, 4);
    pump.setSpeed(4, 0); // 0, 4 … 28, then 30 (not on the step)
    const bool end = waitFor([&] {
        std::lock_guard lock(seen.m);
        return !seen.shown.empty() && seen.shown.back().position == 30;
    }, 2000);
    CHECK(end);
    if (!end) {
        std::lock_guard lock(seen.m);
        std::printf("       4x vorwärts endet bei %lld\n", seen.shown.empty() ? -1LL : (long long)seen.shown.back().position);
    }
    clock.stop();
    pump.setSpeed(0, 29);
    std::this_thread::sleep_for(100ms);
    seen.take();
    clock.play(30, -4);
    pump.setSpeed(-4, 29); // 29, 25 … 1, then 0
    CHECK(waitFor([&] {
        std::lock_guard lock(seen.m);
        return !seen.shown.empty() && seen.shown.back().position == 0;
    }, 2000));
}

// Stopped behind the content's end (the playhead past the timeline's last clip): what the content gives there (black),
// not the frame shown before
void testStillPastEnd()
{
    std::mutex m;
    std::vector<std::pair<int64_t, int>> shown; // position, red (-1 = null image)
    Picture::Pump pump([] { return NAN; }, [&](const QImage& img, int64_t pos, double) {
        std::lock_guard l(m);
        shown.push_back({pos, img.isNull() ? -1 : qRed(img.pixel(0, 0))});
    }, 2);
    auto c = std::make_shared<Picture::Pump::Content>();
    c->render = [](int64_t f) {
        if (f > 100) return QImage(); // nothing there
        QImage i(4, 4, QImage::Format_RGBA8888);
        i.fill(QColor(100, 0, 0));
        return i;
    };
    c->last = [] { return int64_t(100); };
    c->fps = 25;
    pump.setContent(c, 0, 0);
    pump.seek(50, false);
    CHECK(waitFor([&] {
        std::lock_guard l(m);
        return !shown.empty() && shown.back().first == 50;
    }, 1000));
    pump.seek(300, false);
    CHECK(waitFor([&] {
        std::lock_guard l(m);
        return !shown.empty() && shown.back().first == 300;
    }, 1000));
    std::lock_guard l(m);
    CHECK(!shown.empty() && shown.back().first == 300 && shown.back().second == -1);
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    std::printf("pump\n");
    testPump();
    std::printf("scrub, then play\n");
    testScrubThenPlay();
    std::printf("still past the end\n");
    testStillPastEnd();
    std::printf("shuttle to the end\n");
    testShuttleReachesEnd();
    if (Check::haveFfmpeg()) {
        QTemporaryDir dir;
        const QString file = Check::makeMedia(
            dir.filePath("gop.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=6", "-c:v", "libx264",
                                      "-preset", "ultrafast", "-g", "50", "-bf", "2", "-pix_fmt", "yuv420p"});
        std::printf("reader backward\n");
        if (CHECK(!file.isEmpty())) testReaderBackward(file);
        const QString square = Check::makeMedia(
            dir.filePath("square.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=320x320:rate=25:duration=2", "-c:v",
                                         "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
        const QString text = dir.filePath("note.txt");
        QFile n(text);
        if (n.open(QIODevice::WriteOnly)) n.write("no picture");
        n.close();
        std::printf("stills\n");
        if (CHECK(!square.isEmpty())) testStills(file, square, text);
    }
    return Check::result();
}
