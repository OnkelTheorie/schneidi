// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): timeline filmstrip with and without the Color page grade (engine/GradedThumbs).
// A long timeline: 1000 clips of 2 s (~33 min) from four 1080p sources, every second clip graded with a LUT +
// saturation. Per zoom level (whole timeline, ~20 clips) and mode (graded off = the filmstrip as before, on): time
// until the filmstrip is complete, then the UI-thread time per paint; afterwards a grade pasted onto all graded clips
// (time of the first paint, until regraded); at last scrolling and zooming through the timeline (paint time while
// pictures and grades are still coming, how the tiles of graded clips were drawn meanwhile).
// Run: ./build/tests/bench_graded_thumbs [media dir (reused if the files are there)]
#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "engine/GradedThumbs.h"
#include "engine/MediaCache.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <functional>

namespace {

constexpr int kClips = 1000, kLen = 50, kSources = 4;

struct Model {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
};

void grade(Clip& c, double saturation)
{
    if (!EffectRegistry::has(c, "grade")) EffectRegistry::add(c, "grade");
    EffectRegistry::instance(c, "grade")->params["lut"] = QString(":/luts/kino.cube");
    EffectRegistry::instance(c, "grade")->params["saturation"] = saturation;
}

void load(Model& m, const QStringList& sources)
{
    QVector<MediaInfo> media;
    for (const QString& s : sources) media << MediaInfo{s, QFileInfo(s).fileName(), 250, true, false, false};
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    for (int i = 0; i < kClips; ++i) {
        Clip c;
        c.id = i + 1;
        c.mediaPath = sources[i % kSources];
        c.start = i * kLen;
        c.in = (i / kSources % 4) * kLen;
        c.out = c.in + kLen - 1;
        if (i % 2) grade(c, 20 + i % 60); // each clip its own look (pictures are not shared)
        tl.video[0].clips << c;
    }
    m.project.load(ProjectData{ProjectFormat{}, 0, media, {}, tl, kClips + 1, 0});
}

struct Run {
    double buildMs = 0;          // until the last new picture
    double paintMs = 0, paintMaxMs = 0;
    int graded = 0;              // graded pictures made
    double switchOnMs = -1;      // off run: grading switched on with all thumbnails there, until all graded
    int switchOnGraded = 0;
};

// Wait until no new picture came for quietMs (and the grading worker is idle); returns the time of the last one
double settle(MediaCache& cache, const QElapsedTimer& since, int quietMs)
{
    double last = 0;
    bool fresh = false;
    const QMetaObject::Connection conn = QObject::connect(&cache, &MediaCache::updated, &cache, [&] { fresh = true; });
    QElapsedTimer quiet;
    quiet.start();
    while (since.elapsed() < 180000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (fresh) {
            fresh = false;
            last = since.nsecsElapsed() / 1e6;
            quiet.restart();
        } else if (quiet.elapsed() > quietMs && cache.graded()->idle()) {
            break;
        }
        QThread::msleep(2);
    }
    QObject::disconnect(conn);
    return last;
}

void paints(TimelineView& view, Run& r, int n = 40)
{
    QImage target(view.size(), QImage::Format_ARGB32_Premultiplied);
    QElapsedTimer t;
    double sum = 0, mx = 0;
    for (int i = 0; i < n; ++i) {
        QPainter p(&target);
        t.start();
        view.render(&p);
        const double ms = t.nsecsElapsed() / 1e6;
        sum += ms;
        mx = std::max(mx, ms);
    }
    r.paintMs = sum / n;
    r.paintMaxMs = mx;
}

Run measure(Model& m, bool graded, double zoom)
{
    GradedThumbs::setEnabled(graded);
    MediaCache cache;
    TimelineView view(&m.editor);
    view.setMediaCache(&cache);
    view.resize(1600, 260);
    view.show();
    view.zoomToFit();
    if (zoom != 1) view.zoomBy(zoom);
    QElapsedTimer since;
    since.start();
    view.repaint(); // the first paint asks for the pictures
    Run r;
    r.buildMs = settle(cache, since, 2000);
    paints(view, r);
    r.graded = cache.graded()->computed();
    if (!graded) { // only the grading now: the thumbnails are all there
        GradedThumbs::setEnabled(true);
        since.restart();
        view.repaint();
        r.switchOnMs = settle(cache, since, 1000);
        r.switchOnGraded = cache.graded()->computed();
    }
    return r;
}

double median(QVector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.isEmpty() ? 0 : v[v.size() / 2];
}

// Moving through the timeline like a user, one paint per step (~60 wheel steps per second): UI-thread time per paint
// while pictures and grades are still coming, how the tiles of graded clips were drawn, then until everything is there.
// Scroll: zoomed in (~4 clips in view), half a view per step through the whole timeline. Zoom: from the whole timeline
// in to ~4 clips and out again, three times (around the playhead at the start, like Ctrl+wheel there).
void navigateRun(Model& m, bool graded, bool zoom)
{
    GradedThumbs::setEnabled(graded);
    MediaCache cache;
    TimelineView view(&m.editor);
    view.setMediaCache(&cache);
    view.resize(1600, 260);
    view.show();
    view.zoomToFit();
    QVector<std::function<void()>> steps;
    if (zoom) {
        const int in = int(std::ceil(std::log(kClips / 4.0) / std::log(1.25)));
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < in; ++i) steps << [&] { view.zoomBy(1.25); };
            for (int i = 0; i < in; ++i) steps << [&] { view.zoomBy(1 / 1.25); };
        }
    } else {
        view.zoomBy(kClips / 4.0);
        view.setLeftFrame(0);
        const double visible = (view.width() - TimelineView::kHeaderW) / view.view().pxPerFrame;
        for (double left = 0; left < kClips * kLen; left += visible / 2) steps << [&view, left] { view.setLeftFrame(left); };
    }
    QImage target(view.size(), QImage::Format_ARGB32_Premultiplied);
    QElapsedTimer since, t;
    since.start();
    double sum = 0, mx = 0;
    GradedThumbs* g = cache.graded();
    for (const auto& step : steps) {
        step();
        QPainter p(&target);
        t.start();
        view.render(&p);
        const double ms = t.nsecsElapsed() / 1e6;
        sum += ms;
        mx = std::max(mx, ms);
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(16);
    }
    const double moved = since.nsecsElapsed() / 1e6;
    const int ready = g->tilesGraded(), previous = g->tilesPrevious(), neighbor = g->tilesNeighbor(),
              ungraded = g->tilesUngraded();
    since.restart();
    const double complete = settle(cache, since, 2000);
    std::printf("  %s %-3s: %d steps in %.0f ms, paint %.2f ms (max %.2f), complete %.0f ms after the last step, "
                "%d graded pictures",
                zoom ? "zoom  " : "scroll", graded ? "on" : "off", int(steps.size()), moved, sum / steps.size(), mx,
                complete, g->computed());
    if (graded)
        std::printf(";\n    tiles of graded clips drawn: %d graded; while theirs was computed %d with their previous "
                    "look, %d with a graded neighbor, %d ungraded (%.1f %%)",
                    ready, previous, neighbor, ungraded,
                    100.0 * ungraded / std::max(1, ready + previous + neighbor + ungraded));
    std::printf("\n");
}

// Paste another grade onto all graded clips (one edit): first paint, time until all regraded, paint afterwards
void pasteRun(Model& m, double zoom)
{
    GradedThumbs::setEnabled(true);
    MediaCache cache;
    TimelineView view(&m.editor);
    view.setMediaCache(&cache);
    view.resize(1600, 260);
    view.show();
    view.zoomToFit();
    if (zoom != 1) view.zoomBy(zoom);
    QElapsedTimer since;
    since.start();
    view.repaint();
    settle(cache, since, 2000);
    const int before = cache.graded()->computed();
    const Timeline original = m.project.timeline();
    Timeline tl = original;
    for (Clip& c : tl.video[0].clips)
        if (EffectRegistry::has(c, "grade")) grade(c, 20);
    m.project.setTimeline(tl);
    QElapsedTimer t;
    t.start();
    view.repaint();
    const double first = t.nsecsElapsed() / 1e6;
    since.restart();
    const double regraded = settle(cache, since, 1500);
    Run r;
    paints(view, r);
    std::printf("  paste grade onto %d clips: first paint %.2f ms, regraded after %.0f ms (incl. %d ms settle), "
                "%d pictures, paint then %.2f ms\n",
                kClips / 2, first, regraded, GradedThumbs::kSettleMs, cache.graded()->computed() - before, r.paintMs);
    m.project.setTimeline(original); // the first grades for the next runs
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-graded-thumbs");
    Theme::apply(app);
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    QTemporaryDir tmp;
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : tmp.path();
    QDir().mkpath(dir);
    QStringList sources;
    for (int i = 0; i < kSources; ++i) {
        const QString path = QString("%1/src%2.mp4").arg(dir).arg(i);
        if (!QFileInfo::exists(path))
            Check::makeMedia(path, {"-f", "lavfi", "-i", QString("testsrc2=size=1920x1080:rate=25:duration=10,hue=h=%1").arg(i * 70),
                                    "-c:v", "libx264", "-preset", "ultrafast", "-g", "25", "-pix_fmt", "yuv420p"});
        if (!QFileInfo::exists(path)) return Check::skip("Testmedium fehlt");
        sources << path;
    }
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    Model m;
    load(m, sources);
    std::printf("%d clips (%d graded: LUT + saturation), view 1600x260\n", kClips, kClips / 2);
    { // What grading one tile costs (worker thread): source thumbnail size, LUT + saturation
        Clip c;
        grade(c, 70);
        const GradedThumbs::Spec spec = GradedThumbs::specAt(c, 0);
        QImage tile(160, 90, QImage::Format_RGB32);
        for (int y = 0; y < tile.height(); ++y)
            for (int x = 0; x < tile.width(); ++x) tile.setPixel(x, y, qRgb(x * 255 / 160, y * 255 / 90, 128));
        GradedThumbs::apply(tile, spec); // loads the LUT
        QElapsedTimer t;
        t.start();
        constexpr int kRuns = 500;
        for (int i = 0; i < kRuns; ++i) GradedThumbs::apply(tile, spec);
        std::printf("grading one 160x90 tile: %.3f ms\n", t.nsecsElapsed() / 1e6 / kRuns);
    }
    for (double zoom : {1.0, kClips / 20.0}) {
        std::printf("%s:\n", zoom == 1 ? "whole timeline (fit)" : "zoomed in (~20 clips)");
        // Fresh caches, off and on taking turns (the thumbnails are decoded anew each time; a busy machine varies)
        constexpr int kReps = 3;
        QVector<Run> runs[2];
        for (int rep = 0; rep < kReps; ++rep)
            for (bool graded : {false, true}) runs[graded] << measure(m, graded, zoom);
        for (bool graded : {false, true}) {
            QVector<double> build, paint;
            QString each;
            double mx = 0;
            for (const Run& r : runs[graded]) {
                build << r.buildMs;
                paint << r.paintMs;
                mx = std::max(mx, r.paintMaxMs);
                each += QString(" %1").arg(r.buildMs, 0, 'f', 0);
            }
            std::printf("  graded %-3s: filmstrip complete after %.0f ms (median; runs:%s), paint %.2f ms (max %.2f), "
                        "%d graded pictures\n",
                        graded ? "on" : "off", median(build), qPrintable(each), median(paint), mx,
                        runs[graded].last().graded);
        }
        QVector<double> switchOn;
        for (const Run& r : runs[0]) switchOn << r.switchOnMs;
        std::printf("  switched on with all thumbnails there: graded after %.0f ms (median), %d pictures\n",
                    median(switchOn), runs[0].last().switchOnGraded);
        pasteRun(m, zoom);
    }
    std::printf("moving through the timeline:\n");
    for (bool zoom : {false, true})
        for (bool graded : {false, true}) navigateRun(m, graded, zoom);
    return 0;
}
