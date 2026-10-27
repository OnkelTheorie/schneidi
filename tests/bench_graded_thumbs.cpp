// Benchmark (not a CTest test): timeline filmstrip with and without the Color page grade (engine/GradedThumbs).
// 200 clips of four 1080p sources, every second clip graded with a LUT + saturation. Per zoom level (whole timeline,
// ~20 clips) and mode (graded off = the filmstrip as before, on): time until the filmstrip is complete, then the
// UI-thread time per paint; afterwards a grade pasted onto all graded clips (time of the first paint, until regraded).
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
#include <cstdio>

namespace {

constexpr int kClips = 200, kLen = 50, kSources = 4;

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
        if (i % 2) grade(c, 70);
        tl.video[0].clips << c;
    }
    m.project.load(ProjectData{ProjectFormat{}, 0, media, {}, tl, kClips + 1, 0});
}

struct Run {
    double buildMs = 0;          // until the last new picture
    double paintMs = 0, paintMaxMs = 0;
    int graded = 0;              // graded pictures made
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
    return r;
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
    Timeline tl = m.project.timeline();
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
    // back to the first grade for the next runs
    Timeline back = m.project.timeline();
    for (Clip& c : back.video[0].clips)
        if (EffectRegistry::has(c, "grade")) grade(c, 70);
    m.project.setTimeline(back);
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
    for (double zoom : {1.0, 10.0}) {
        std::printf("%s:\n", zoom == 1 ? "whole timeline (fit)" : "zoomed in (~20 clips)");
        for (bool graded : {false, true}) {
            const Run r = measure(m, graded, zoom);
            std::printf("  graded %-3s: filmstrip complete after %.0f ms, paint %.2f ms (max %.2f), %d graded pictures\n",
                        graded ? "on" : "off", r.buildMs, r.paintMs, r.paintMaxMs, r.graded);
        }
        pasteRun(m, zoom);
    }
    return 0;
}
