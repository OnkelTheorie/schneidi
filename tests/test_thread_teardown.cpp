// Background workers stopped, reset or canceled while they are busy, at varying moments: hover scrub pictures
// (ScrubFrames), graded filmstrip tiles and timeline posters (GradedThumbs, TimelinePosters), the watch folder scan
// (FolderWatch), the render cache and the render queue preparing in the background. Nothing may crash, hang or keep
// running afterwards. Mostly a stress test for the thread sanitizer and ASan (see the sanitizer scripts of the lead);
// in the normal suite it takes a few seconds.
#include "check.h"

#include "app/FolderWatch.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Exporter.h"
#include "engine/GradedThumbs.h"
#include "engine/RenderCache.h"
#include "engine/RenderQueue.h"
#include "engine/ScrubFrames.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <clocale>
#include <functional>
#include <memory>

namespace {

QString g_clip, g_clip2; // short test videos

// Events keep running for `ms` (queued results of the workers arrive)
void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    } while (t.elapsed() < ms);
}

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.hasExpired(ms)) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return true;
}

QImage pattern(int shift)
{
    QImage img(160, 90, QImage::Format_RGB32);
    img.fill(QColor::fromHsv((shift * 37) % 360, 200, 220));
    return img;
}

Clip mediaClip(int id, const QString& path, int in = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.in = in;
    c.out = in + 39;
    return c;
}

Clip graded(Clip c)
{
    EffectRegistry::add(c, ColorGrade::EffectId);
    EffectRegistry::instance(c, ColorGrade::EffectId)->params["saturation"] = 20.0;
    return c;
}

// Destroyed before, while and after decoding; the last one finishes
void testScrubFrames()
{
    int decoded = 0;
    for (int round = 0; round < 6; ++round) {
        ScrubFrames frames(QSize(144, 81));
        for (int s = 0; s < ScrubFrames::kSlots; ++s) {
            frames.picture(g_clip, s);
            frames.picture(g_clip2, ScrubFrames::kSlots - 1 - s);
        }
        if (round < 5) spin(round * 15);
        else waitFor([&] { return frames.has(g_clip, 0) && frames.has(g_clip2, 0); }, 60000);
        decoded = frames.decoded();
    }
    CHECK(decoded > 0);
}

// Tiles requested by the hundred, reset (project format changed) and destroyed while the worker grades them
void testGradedThumbs()
{
    QVector<QImage> raws;
    for (int i = 0; i < 300; ++i) raws << pattern(i);
    for (int round = 0; round < 5; ++round) {
        GradedThumbs thumbs;
        const Clip c = graded(mediaClip(1, "/x/a.mp4"));
        for (int pass = 0; pass < 2; ++pass) {
            const GradedThumbs::ClipGrade g = thumbs.forClip(c);
            CHECK(bool(g));
            for (int i = 0; i < raws.size(); ++i) g.image(raws[i], i);
            if (pass == 0 && round % 2) thumbs.clear();
        }
        if (round == 4) CHECK(waitFor([&] { return thumbs.idle(); }, 60000));
        else spin(round * 10);
    }
}

// The worker decodes the first picture while the format changes and the posters go away
void testTimelinePosters()
{
    Project p;
    p.addMedia(MediaInfo{g_clip, "a.mp4", 50, true, false, false});
    Timeline tl = p.timeline();
    tl.video[0].clips = {graded(mediaClip(1, g_clip))};
    p.setTimeline(tl);
    const int seq = p.currentSequence();
    for (int round = 0; round < 5; ++round) {
        TimelinePosters posters(&p, QSize(144, 81));
        posters.poster(seq);
        if (round == 1 || round == 3) {
            ProjectFormat f = p.format();
            f.width = round == 1 ? 1280 : 1920;
            f.height = round == 1 ? 720 : 1080;
            p.setFormat(f);
            posters.poster(seq);
        }
        if (round == 4) CHECK(waitFor([&] { return !posters.poster(seq).isNull(); }, 60000));
        else spin(round * 20);
    }
}

// Scans (listing + probing six files in the worker) while the watch list or the format changes and the watcher goes
void testFolderWatch(const QString& dir)
{
    QDir().mkpath(dir);
    for (int i = 0; i < 6; ++i) QFile::copy(i % 2 ? g_clip : g_clip2, QString("%1/w%2.mp4").arg(dir).arg(i));
    WatchFolder w;
    w.path = dir; // no start time: every file counts as new
    for (int round = 0; round < 6; ++round) {
        Project p;
        ProjectData d;
        d.timeline = emptyTimeline();
        p.load(d);
        auto fw = std::make_unique<FolderWatch>(&p);
        fw->setSettleMs(0);
        p.setWatchFolders({w});
        spin(round * 30);
        if (round == 2) p.setWatchFolders({});
        if (round == 3) {
            ProjectFormat f = p.format();
            f.rate = FrameRate{30, 1};
            p.setFormat(f);
        }
        if (round == 5) {
            CHECK(waitFor([&] { return p.media().size() == 6; }, 60000));
            CHECK(waitFor([&] { return fw->idle(); }, 10000));
        }
        fw.reset(); // destroyed while scanning or probing (the earlier rounds)
        spin(5);
    }
}

// Render cache jobs canceled before, while and right as they end (also with their result still queued)
void testRenderCache()
{
    RenderCache rc;
    ProjectFormat fmt;
    fmt.width = 320;
    fmt.height = 180;
    rc.setFormat(fmt);
    rc.setMode(RenderCache::Mode::User);
    rc.clear();
    auto one = [](const Clip& c) {
        Timeline tl;
        tl.video.resize(1);
        tl.video[0].clips << c;
        return tl;
    };
    for (int round = 0; round < 5; ++round) {
        Clip c = mediaClip(1, g_clip, round);
        c.renderCache = true;
        EffectRegistry::add(c, "blur");
        rc.sync(one(c));
        CHECK(waitFor([&] { return rc.isRendering(); }, 10000));
        if (round % 2) QThread::msleep(round * 150); // no events: a finished render stays queued
        else spin(round * 150);
        Clip off = c;
        off.renderCache = false;
        rc.sync(one(off));
        CHECK(!rc.isRendering());
        spin(20);
        CHECK(!rc.isRendering());
    }
    rc.clear();
}

// The render queue canceled at varying moments: preparing (opening the files in the background), right as it has
// prepared (its result still queued) and rendering
void testRenderQueue(const QString& dir)
{
    Project p;
    {
        ProjectData d;
        d.format.width = 320;
        d.format.height = 180;
        Timeline& tl = d.timeline;
        TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
        TimelineOps::ensureTracks(tl, TrackKind::Audio, 1);
        Clip v = mediaClip(1, g_clip);
        v.linkId = 1;
        Clip a = v;
        a.id = 2;
        Clip v2 = mediaClip(3, g_clip2);
        v2.start = 40;
        tl.video[0].clips << v << v2;
        tl.audio[0].clips << a;
        d.lastClipId = 3;
        d.lastLinkId = 1;
        p.load(d);
    }
    RenderQueue q(&p);
    int finished = 0;
    QObject::connect(&q, &RenderQueue::finished, [&] { ++finished; });
    const int delays[] = {0, 1, 10, 40, 120, 400, 1000};
    int round = 0;
    for (const bool events : {true, false})
        for (const int ms : delays) {
            RenderJob j;
            j.id = ++round;
            j.path = QDir(dir).filePath(QString("r%1.mp4").arg(round));
            j.settings.format = "h264";
            j.settings.quality = 2;
            j.settings.encoder = VideoEncoder::Software;
            p.setRenderQueue({j});
            const int before = finished;
            CHECK(q.start());
            if (events) spin(ms);
            else QThread::msleep(ms);
            q.cancel();
            CHECK(!q.isRunning());
            spin(50);
            CHECK_EQ(finished, before + 1);
            CHECK(!Exporter::anyRunning());
        }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("thread-teardown");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_clip = Check::makeMedia(tmp.filePath("a.mp4"),
                              {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2", "-f", "lavfi", "-i",
                               "sine=frequency=440:duration=2", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                               "yuv420p", "-c:a", "aac", "-shortest"});
    g_clip2 = Check::makeMedia(tmp.filePath("b.mp4"),
                               {"-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=2", "-c:v", "libx264",
                                "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_clip.isEmpty() && !g_clip2.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");

    testScrubFrames();
    testGradedThumbs();
    testTimelinePosters();
    testFolderWatch(tmp.filePath("watch"));
    testRenderCache();
    testRenderQueue(tmp.path());
    return Check::result();
}
