// SPDX-License-Identifier: AGPL-3.0-only
// Fast seeking and scrubbing on the own picture engine:
// - Video::Reader's seek window (Options::keepBytes): after a jump the pictures decoded on the way from the key frame
//   are kept, so steps back are answered without decoding again - the same pixels as a fresh decode, within the
//   memory budget (more steps back than fit = a new seek).
// - Reader::Fetch::nearest (scrubbing): answered by a close picture at once, the exact one follows; Fetch::cancel
//   gives up a request.
// - Renderer::render with RenderHints::nearest: Frame::exact() false for a stand-in, the exact render equals a render
//   without hints.
// - Pump: scrubbing shows a close picture first and always ends on the exact one (never a stand-in left on screen),
//   stale still renders are cancelled; recently shown frames are reused within their byte budget and dropped on
//   refresh().
#include "check.h"

#include "core/Types.h"
#include "engine/picture/Pump.h"
#include "engine/picture/Renderer.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

Video::Reader::Options software(int64_t keepBytes)
{
    Video::Reader::Options o;
    o.hardware = false;
    o.keepBytes = keepBytes;
    return o;
}

// Luma plane equal?
bool sameLuma(const Video::Picture& a, const Video::Picture& b)
{
    if (a.frame->width != b.frame->width || a.frame->height != b.frame->height) return false;
    for (int y = 0; y < a.frame->height; ++y)
        if (std::memcmp(a.frame->data[0] + size_t(y) * a.frame->linesize[0],
                        b.frame->data[0] + size_t(y) * b.frame->linesize[0], size_t(a.frame->width)))
            return false;
    return true;
}

// 640x360 4:2:0 = 345600 bytes per picture
constexpr int64_t kPicture = 640 * 360 * 3 / 2;

void testSeekWindow(const QString& file)
{
    // Jump to 120 (key frame 100), then 16 steps back: answered from the window, no seek, the same pixels
    auto r = Video::Reader::open(file, software(64 * kPicture));
    auto fresh = Video::Reader::open(file, software(0));
    if (!CHECK(r && fresh)) return;
    auto p = r->picture(120);
    CHECK(p && p->position == 120);
    const int decoded = r->stats().decoded;
    int wrong = 0, differ = 0;
    for (int k = 119; k >= 104; --k) {
        auto q = r->picture(k);
        if (!q || q->position != k) ++wrong;
        else if (auto f = Video::Reader::open(file, software(0))) { // a fresh decode of the same frame
            auto e = f->picture(k);
            if (!e || !sameLuma(*q, *e)) ++differ;
        }
    }
    const Video::Reader::Stats st = r->stats();
    std::printf("       window: 16 steps back after a jump: %d seek(s), %d decoded after the jump\n", st.seeks,
                st.decoded - decoded);
    CHECK_EQ(wrong, 0);
    CHECK_EQ(differ, 0);
    CHECK_EQ(st.seeks, 1);
    CHECK(st.decoded - decoded <= 8); // read-ahead only
    // Without the window the first steps back seek, then the reader keeps a window behind (going backwards)
    fresh->picture(120);
    for (int k = 119; k >= 110; --k) fresh->picture(k);
    CHECK(fresh->stats().seeks >= 3 && fresh->stats().seeks <= 4);
    CHECK_EQ(fresh->stats().backwardWindows, 1);
    // Forwards with a late worker now and then (a step back of a few frames, far apart): no backward window (two
    // such stragglers anywhere opened one, ~400 MB per file at 1080p); single pictures newest first neither
    auto forward = Video::Reader::open(file, software(0));
    if (CHECK(forward != nullptr)) {
        for (int k = 0; k <= 60; ++k) forward->picture(k);
        forward->picture(50);
        for (int k = 61; k <= 120; ++k) forward->picture(k);
        forward->picture(110);
        for (int k = 121; k <= 130; ++k) forward->picture(k);
        CHECK_EQ(forward->stats().backwardWindows, 0);
    }
    // The window behind stays within its byte budget: room for 6 pictures (a 4K 4:4:4 picture in 384 MB) -> a window
    // of 5, so 30 steps back seek about every 5 (it was at least 17 pictures, whatever their size)
    Video::Reader::Options budget = software(0);
    budget.backwardBytes = 6 * kPicture;
    if (auto tight = Video::Reader::open(file, budget); CHECK(tight != nullptr)) {
        tight->picture(120);
        int wrong = 0;
        for (int k = 119; k >= 90; --k) wrong += tight->picture(k)->position != k;
        CHECK_EQ(wrong, 0);
        CHECK_EQ(tight->stats().backwardWindows, 1);
        std::printf("       window behind of 6 pictures: 30 steps back, %d seeks\n", tight->stats().seeks);
        CHECK(tight->stats().seeks >= 6 && tight->stats().seeks <= 10);
    }
    Video::Reader::Options stills = software(0);
    stills.autoBackward = false;
    if (auto thumbs = Video::Reader::open(file, stills); CHECK(thumbs != nullptr)) {
        for (int k = 140; k >= 20; k -= 20) thumbs->picture(k);
        CHECK_EQ(thumbs->stats().backwardWindows, 0);
    }

    // Memory bound: a window of 5 pictures holds the 5 before the last request, the 10th step back seeks again
    auto small = Video::Reader::open(file, software(5 * kPicture + 1000));
    if (!CHECK(small != nullptr)) return;
    small->picture(120);
    for (int k = 119; k >= 115; --k) CHECK_EQ(small->picture(k)->position, int64_t(k));
    CHECK_EQ(small->stats().seeks, 1);
    small->picture(105);
    CHECK_EQ(small->stats().seeks, 2);
}

void testNearest(const QString& file)
{
    auto r = Video::Reader::open(file, software(0));
    if (!CHECK(r != nullptr)) return;
    Video::Reader::Fetch near;
    near.nearest = true;
    bool exact = true;
    auto a = r->picture(140, near, &exact);
    // A close picture: at or before the position, from the key frame (100) on
    if (CHECK(a != nullptr)) {
        std::printf("       nearest for 140: %lld (%s)\n", (long long)a->position, exact ? "exact" : "stand-in");
        CHECK(a->position >= 100 && a->position <= 140);
        CHECK_EQ(exact, a->position == 140);
    }
    // The exact picture follows; asked again (still decoded), the nearest fetch is exact
    auto b = r->picture(140);
    CHECK(b && b->position == 140);
    exact = false;
    auto c = r->picture(140, near, &exact);
    CHECK(c && c->position == 140 && exact);

    // Cancelled before: nothing, nothing decoded for it; cancelled while waiting: returns soon
    std::atomic<bool> cancel{true};
    Video::Reader::Fetch gone;
    gone.cancel = &cancel;
    CHECK(r->picture(20, gone) == nullptr);
    cancel = false;
    std::thread later([&] {
        std::this_thread::sleep_for(1ms);
        cancel = true;
    });
    QElapsedTimer t;
    t.start();
    auto d = r->picture(30, gone);
    later.join();
    CHECK(t.elapsed() < 2000);
    if (!d) CHECK_EQ(r->stats().cancelled, 1);
    // The reader goes on normally afterwards
    auto e = r->picture(75);
    CHECK(e && e->position == 75);
}

void testRendererHints(const QString& file)
{
    Timeline tl;
    Track v;
    Clip c;
    c.id = 1;
    c.mediaPath = file;
    c.start = 0;
    c.in = 0;
    c.out = 149;
    v.clips.push_back(c);
    tl.video.push_back(v);
    Picture::Options o;
    o.format = ProjectFormat{640, 360, {25, 1}};
    o.mode = Picture::Mode::Preview;
    Picture::Renderer r(tl, o);
    Picture::RenderHints near;
    near.nearest = true;
    const Picture::Frame a = r.render(140, near);
    CHECK(!a.isNull());
    const Picture::Frame b = r.render(140);
    CHECK(!b.isNull() && b.exact());
    Picture::Renderer ref(tl, o);
    const Picture::Frame want = ref.render(140);
    CHECK(b.toImage() == want.toImage());
    if (a.exact()) CHECK(a.toImage() == want.toImage());
    // Cancelled: null
    std::atomic<bool> cancel{true};
    Picture::RenderHints gone;
    gone.cancel = &cancel;
    CHECK(r.render(60, gone).isNull());
}

// Pump with synthetic content: a close picture costs 2 ms (green = 1), the exact one 30 ms (red channel = 200),
// checking the cancel flag while it works
struct Synthetic {
    std::atomic<int> exactRenders{0}, nearRenders{0}, cancelled{0};
    std::shared_ptr<Picture::Pump::Content> content()
    {
        auto c = std::make_shared<Picture::Pump::Content>();
        auto make = [](int red, int64_t f) {
            QImage img(8, 8, QImage::Format_RGBA8888);
            img.fill(QColor(red, int(f % 256), int(f / 256)));
            return img;
        };
        c->render = [this, make](int64_t f) {
            ++exactRenders;
            std::this_thread::sleep_for(30ms);
            return make(200, f);
        };
        c->renderWant = [this, make](int64_t f, Picture::Pump::Want& w) {
            if (w.nearest) {
                ++nearRenders;
                std::this_thread::sleep_for(2ms);
                w.exact = false;
                return make(1, f);
            }
            ++exactRenders;
            for (int i = 0; i < 30; ++i) {
                if (w.cancel && w.cancel->load()) {
                    ++cancelled;
                    return QImage();
                }
                std::this_thread::sleep_for(1ms);
            }
            return make(200, f);
        };
        c->last = [] { return int64_t(10000); };
        c->fps = 60;
        return c;
    }
};

struct Shown {
    std::mutex m;
    struct Entry {
        int64_t position;
        int red;
        int64_t frame; // frame encoded in the image
    };
    std::vector<Entry> list;
    void add(const QImage& img, int64_t pos)
    {
        std::lock_guard lock(m);
        const QRgb px = img.isNull() ? 0 : img.pixel(0, 0);
        list.push_back({pos, qRed(px), qGreen(px) + 256 * qBlue(px)});
    }
    Entry last()
    {
        std::lock_guard lock(m);
        return list.empty() ? Entry{-1, -1, -1} : list.back();
    }
    size_t count()
    {
        std::lock_guard lock(m);
        return list.size();
    }
};

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void testPumpScrub()
{
    Synthetic syn;
    Shown shown;
    Picture::Pump pump([] { return NAN; }, [&](const QImage& img, int64_t pos, double) { shown.add(img, pos); }, 3);
    pump.setContent(syn.content(), 100, 0);
    CHECK(waitFor([&] { return shown.last().position == 100 && shown.last().red == 200; }, 2000));
    // 60 scrub seeks in about a second
    for (int k = 1; k <= 60; ++k) {
        pump.seek(100 + 3 * k, true);
        std::this_thread::sleep_for(16ms);
    }
    // The mouse stops: the exact picture of the last position, and it stays
    CHECK(waitFor([&] { return shown.last().position == 280 && shown.last().red == 200; }, 2000));
    std::this_thread::sleep_for(100ms);
    const Shown::Entry end = shown.last();
    CHECK_EQ(end.position, int64_t(280));
    CHECK_EQ(end.red, 200);
    CHECK_EQ(end.frame, int64_t(280));
    const Picture::Pump::Stats st = pump.stats();
    std::printf("       scrub: %zu pictures shown, %d close ones, %d exact renders, %d cancelled\n", shown.count(),
                st.nearest, syn.exactRenders.load(), st.cancelled);
    CHECK(st.nearest >= 20);        // the scrub showed close pictures while moving
    CHECK(syn.cancelled.load() > 0); // exact renders for positions passed by were given up
    // Every shown picture is the one of its position
    {
        std::lock_guard lock(shown.m);
        int wrong = 0;
        for (const Shown::Entry& e : shown.list) wrong += e.frame != e.position;
        CHECK_EQ(wrong, 0);
    }

    // A single seek (no scrub): the exact picture at once, no stand-in
    const int nearBefore = pump.stats().nearest;
    pump.seek(500);
    CHECK(waitFor([&] { return shown.last().position == 500; }, 2000));
    CHECK_EQ(shown.last().red, 200);
    CHECK_EQ(pump.stats().nearest, nearBefore);
}

void testPumpRecent()
{
    Synthetic syn;
    Shown shown;
    Picture::Pump pump([] { return NAN; }, [&](const QImage& img, int64_t pos, double) { shown.add(img, pos); }, 2);
    auto c = syn.content();
    c->renderWant = nullptr; // plain renders: every render is exact
    pump.setContent(c, 10, 0);
    CHECK(waitFor([&] { return shown.last().position == 10; }, 2000));
    pump.seek(11);
    CHECK(waitFor([&] { return shown.last().position == 11; }, 2000));
    const int renders = syn.exactRenders.load();
    // Back to 10: shown recently, not rendered again
    pump.seek(10);
    CHECK(waitFor([&] { return shown.last().position == 10; }, 2000));
    CHECK_EQ(syn.exactRenders.load(), renders);
    CHECK(pump.stats().recent >= 1);
    // A content change drops them
    pump.refresh();
    CHECK(waitFor([&] { return syn.exactRenders.load() > renders; }, 2000));
    CHECK(waitFor([&] { return pump.stats().recentBytes > 0; }, 2000));
    // Memory bound: 3 pictures of 8x8 RGBA
    pump.setRecentBudget(3 * 8 * 8 * 4);
    for (int f = 20; f < 30; ++f) {
        pump.seek(f);
        CHECK(waitFor([&] { return shown.last().position == f; }, 2000));
    }
    const Picture::Pump::Stats st = pump.stats();
    CHECK(st.recentBytes <= 3 * 8 * 8 * 4);
    CHECK(st.recentBytes > 0);
    // The newest one is kept, an old one is not
    const int before = syn.exactRenders.load();
    pump.seek(29);
    CHECK(waitFor([&] { return shown.last().position == 29; }, 2000));
    CHECK_EQ(syn.exactRenders.load(), before);
    pump.seek(20);
    CHECK(waitFor([&] { return shown.last().position == 20; }, 2000));
    CHECK_EQ(syn.exactRenders.load(), before + 1);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    std::printf("pump scrub\n");
    testPumpScrub();
    std::printf("pump recent frames\n");
    testPumpRecent();
    if (Check::haveFfmpeg()) {
        QTemporaryDir dir;
        const QString file = Check::makeMedia(
            dir.filePath("gop.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=6", "-c:v", "libx264",
                                      "-preset", "ultrafast", "-g", "50", "-bf", "2", "-pix_fmt", "yuv420p"});
        if (CHECK(!file.isEmpty())) {
            std::printf("reader seek window\n");
            testSeekWindow(file);
            std::printf("reader nearest\n");
            testNearest(file);
            std::printf("renderer hints\n");
            testRendererHints(file);
        }
    }
    return Check::result();
}
