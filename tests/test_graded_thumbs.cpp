// Thumbnails with the Color page grade (engine/GradedThumbs): the grade applied to a small picture (saturation, LUT,
// windows), the cache keyed by source thumbnail + evaluated grade, the settle time after a change (dragging a wheel),
// undo through the Editor, ungraded clips untouched, the cost of many graded clips on the UI thread, the timeline
// filmstrip offscreen (graded and ungraded clip of one source, undo, a wheel drag) and the Media Pool pictures of
// timelines (TimelinePosters, the MediaPool entries: timeline graded, source neutral).
// GRADED_THUMBS_DUMP=<dir> saves the timeline before/after and a project file for an app screenshot.
#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/GradeWindows.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/GradedThumbs.h"
#include "engine/MediaCache.h"
#include "ui/MediaPool.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTest>
#include <clocale>
#include <cmath>
#include <functional>

namespace {

QString g_media; // colorful test video (testsrc2)

bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

void idle(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
}

// Color blocks like a test pattern (opaque RGB32 like the MediaCache thumbnails)
QImage pattern(int w = 160, int h = 90, int shift = 0)
{
    QImage img(w, h, QImage::Format_RGB32);
    const QRgb cols[] = {qRgb(220, 30, 30), qRgb(30, 200, 40), qRgb(40, 60, 230), qRgb(230, 210, 20)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) img.setPixel(x, y, cols[(x * 4 / w + shift) % 4]);
    return img;
}

// Mean chroma (max - min channel) over the picture: 0 = gray
double chroma(const QImage& img, const QRect& area = {})
{
    const QRect r = area.isNull() ? img.rect() : area.intersected(img.rect());
    double sum = 0;
    int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgb p = img.pixel(x, y);
            const int mx = std::max({qRed(p), qGreen(p), qBlue(p)}), mn = std::min({qRed(p), qGreen(p), qBlue(p)});
            sum += mx - mn;
            ++n;
        }
    return n ? sum / n : 0;
}

Clip clip(int id, int start, int in, int length, const QString& path = "/x/a.mp4")
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    return c;
}

void setGrade(Clip& c, const QString& key, const QVariant& v)
{
    if (!EffectRegistry::has(c, ColorGrade::EffectId)) EffectRegistry::add(c, ColorGrade::EffectId);
    EffectRegistry::instance(c, ColorGrade::EffectId)->params[key] = v;
}

QString writeCube(const QString& path, bool invert)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write("LUT_3D_SIZE 2\n");
    for (int b = 0; b < 2; ++b)
        for (int g = 0; g < 2; ++g)
            for (int r = 0; r < 2; ++r)
                f.write(QString("%1 %2 %3\n").arg(invert ? 1 - r : r).arg(invert ? 1 - g : g).arg(invert ? 1 - b : b).toUtf8());
    return path;
}

// ---------- pure: what the grade does to a picture ----------

void testSpecAndApply(const QString& dir)
{
    Clip plain = clip(1, 0, 0, 100);
    CHECK(GradedThumbs::specAt(plain, 0).isNull());
    Clip neutral = plain;
    EffectRegistry::add(neutral, ColorGrade::EffectId); // grade with default values changes nothing
    CHECK(GradedThumbs::specAt(neutral, 0).isNull());

    Clip gray = plain;
    setGrade(gray, "saturation", 0.0);
    const GradedThumbs::Spec s = GradedThumbs::specAt(gray, 0);
    CHECK(!s.isNull());
    const QImage src = pattern();
    const QImage out = GradedThumbs::apply(src, s);
    CHECK_EQ(out.size(), src.size());
    CHECK_EQ(out.format(), QImage::Format_RGB32);
    CHECK(chroma(src) > 100);
    CHECK(chroma(out) < 2); // saturation 0: gray
    CHECK(src.pixel(5, 5) == qRgb(220, 30, 30)); // the source thumbnail stays untouched
    // Brightness kept (Rec. 709 luma of the red block)
    const int y = int(std::lround(0.2126 * 220 + 0.7152 * 30 + 0.0722 * 30));
    CHECK(std::abs(qRed(out.pixel(5, 5)) - y) <= 2);

    // Same grade on another clip (other id, other in point) = same key; changed or switched off = different/none
    Clip other = clip(7, 500, 40, 30);
    setGrade(other, "saturation", 0.0);
    CHECK(GradedThumbs::specAt(other, 3) == s);
    Clip warmer = gray;
    setGrade(warmer, "temperature", 1500.0);
    CHECK(!(GradedThumbs::specAt(warmer, 0) == s));
    Clip off = gray;
    EffectRegistry::instance(off, ColorGrade::EffectId)->enabled = false;
    CHECK(GradedThumbs::specAt(off, 0).isNull());

    // LUT: inverted cube turns red into cyan; the bundled black-and-white LUT makes it gray
    const QString inv = writeCube(dir + "/invert.cube", true);
    Clip lut = plain;
    setGrade(lut, "lut", inv);
    const GradedThumbs::Spec ls = GradedThumbs::specAt(lut, 0);
    CHECK(!ls.isNull());
    const QRgb p = GradedThumbs::apply(src, ls).pixel(5, 5);
    CHECK(qRed(p) < 60 && qGreen(p) > 200 && qBlue(p) > 200);
    Clip bw = plain;
    setGrade(bw, "lut", ":/luts/schwarzweiss.cube");
    CHECK(chroma(GradedThumbs::apply(src, GradedThumbs::specAt(bw, 0))) < 8);
    CHECK(!(GradedThumbs::specAt(bw, 0) == ls)); // the LUT is part of the key
    // Missing LUT file: like the preview, the picture stays as it is
    Clip missing = plain;
    setGrade(missing, "lut", dir + "/gone.cube");
    CHECK(GradedThumbs::apply(src, GradedThumbs::specAt(missing, 0)).pixel(5, 5) == src.pixel(5, 5));

    // Window with a solid fill over the middle: only there the picture changes
    Clip win = plain;
    EffectRegistry::add(win, ColorGrade::EffectId);
    GradeWindows::add(win, 1, GradeWindows::Shape::Rectangle);
    EffectRegistry::instance(win, ColorGrade::EffectId)->params[GradeWindows::key(1, "fill")] = "color";
    EffectRegistry::instance(win, ColorGrade::EffectId)->params[GradeWindows::key(1, "color")] = QColor(Qt::white);
    EffectRegistry::instance(win, ColorGrade::EffectId)->params[GradeWindows::key(1, "softness")] = 0.0;
    const GradedThumbs::Spec ws = GradedThumbs::specAt(win, 0);
    CHECK(!ws.isNull());
    const QImage wout = GradedThumbs::apply(src, ws);
    CHECK(wout.pixel(80, 45) == qRgb(255, 255, 255));
    CHECK(wout.pixel(2, 2) == src.pixel(2, 2));

    // Keyframes: values per clip frame, the key follows them
    Clip anim = plain;
    EffectRegistry::add(anim, ColorGrade::EffectId);
    CHECK(!GradedThumbs::animated(anim));
    Keys::setKey(anim, AnimParam::GradeSaturation, 0, 0.0);
    Keys::setKey(anim, AnimParam::GradeSaturation, 50, 100.0);
    CHECK(GradedThumbs::animated(anim));
    CHECK(!(GradedThumbs::specAt(anim, 0) == GradedThumbs::specAt(anim, 50)));
    CHECK(chroma(GradedThumbs::apply(src, GradedThumbs::specAt(anim, 0))) < 2);
    CHECK(chroma(GradedThumbs::apply(src, GradedThumbs::specAt(anim, 50))) > chroma(src));
    Clip winAnim = win;
    Keys::setKey(winAnim, GradeWindows::param(1, "x"), 0, 30.0);
    Keys::setKey(winAnim, GradeWindows::param(1, "x"), 20, 70.0);
    CHECK(GradedThumbs::animated(winAnim)); // window keyframes count too
    Clip volume = plain;
    Keys::setKey(volume, AnimParam::Volume, 0, -6.0);
    CHECK(!GradedThumbs::animated(volume)); // other parameters do not
}

// ---------- the cache in the paint path ----------

void testCacheAndSettle()
{
    GradedThumbs thumbs;
    QSignalSpy updated(&thumbs, &GradedThumbs::updated);
    const QImage raw = pattern();

    // Ungraded clip: false, nothing computed or cached
    const Clip plain = clip(1, 0, 0, 100);
    CHECK(!thumbs.forClip(plain));
    Clip neutral = plain;
    EffectRegistry::add(neutral, ColorGrade::EffectId);
    CHECK(!thumbs.forClip(neutral));

    Clip a = clip(2, 0, 0, 100);
    setGrade(a, "saturation", 0.0);
    auto ga = thumbs.forClip(a);
    CHECK(bool(ga));
    // Not ready: the ungraded thumbnail meanwhile, the graded one follows from the worker
    CHECK_EQ(ga.image(raw, 0).cacheKey(), raw.cacheKey());
    CHECK(waitFor([&] { return thumbs.idle() && updated.count() > 0; }, 10000));
    const QImage g1 = thumbs.forClip(a).image(raw, 0);
    CHECK(g1.cacheKey() != raw.cacheKey());
    CHECK(chroma(g1) < 2);
    CHECK_EQ(thumbs.computed(), 1);

    // Scrolling back / another clip with the same grade on the same source picture: from the cache
    CHECK_EQ(thumbs.forClip(a).image(raw, 0).cacheKey(), g1.cacheKey());
    Clip b = clip(3, 300, 0, 100);
    setGrade(b, "saturation", 0.0);
    CHECK_EQ(thumbs.forClip(b).image(raw, 0).cacheKey(), g1.cacheKey());
    idle(50);
    CHECK_EQ(thumbs.computed(), 1);

    // Changed grade (dragging a wheel): the previous look stays, nothing is computed until it has been still
    const Clip before = a;
    const int redraws = int(updated.count());
    for (int i = 1; i <= 6; ++i) { // 6 steps 40 ms apart
        setGrade(a, "saturation", 50.0 + i * 8);
        CHECK_EQ(thumbs.forClip(a).image(raw, 0).cacheKey(), g1.cacheKey()); // no flicker back to ungraded
        idle(40);
    }
    CHECK_EQ(thumbs.computed(), 1);
    CHECK(thumbs.idle());
    // Settled: the timer asks for a redraw, the redraw queues one picture for the last state
    CHECK(waitFor([&] { return updated.count() > redraws; }, 3000));
    CHECK_EQ(thumbs.forClip(a).image(raw, 0).cacheKey(), g1.cacheKey()); // still the old one until computed
    CHECK(waitFor([&] { return thumbs.idle() && thumbs.computed() == 2; }, 10000));
    idle(20);
    const QImage g2 = thumbs.forClip(a).image(raw, 0);
    CHECK(g2.cacheKey() != g1.cacheKey());
    CHECK(chroma(g2) > chroma(raw)); // saturation 98: more colorful
    CHECK_EQ(thumbs.computed(), 2);  // one picture for six changes

    // Undo: the old grade is still cached -> back at once, nothing computed
    a = before;
    CHECK_EQ(thumbs.forClip(a).image(raw, 0).cacheKey(), g1.cacheKey());
    idle(GradedThumbs::kSettleMs + 100);
    CHECK_EQ(thumbs.computed(), 2);

    // A new source picture (the exact frame replaces the keyframe one): the tile keeps its graded picture until the
    // new one is graded (here right away: the grade has not changed)
    const QImage raw2 = pattern(160, 90, 1);
    CHECK_EQ(thumbs.forClip(a).image(raw2, 0).cacheKey(), g1.cacheKey());
    CHECK(waitFor([&] { return thumbs.idle() && thumbs.computed() == 3; }, 10000));
    idle(20);
    CHECK(thumbs.forClip(a).image(raw2, 0).cacheKey() != g1.cacheKey());

    // Switched off: like ungraded clips
    GradedThumbs::setEnabled(false);
    CHECK(!thumbs.forClip(a));
    GradedThumbs::setEnabled(true);
    CHECK(bool(thumbs.forClip(a)));

    // Format change: everything anew
    thumbs.clear();
    CHECK_EQ(thumbs.cacheCount(), 0);
    CHECK_EQ(thumbs.forClip(a).image(raw, 0).cacheKey(), raw.cacheKey());
    CHECK(waitFor([&] { return thumbs.idle() && thumbs.computed() == 4; }, 10000));
}

struct Model {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
};

// Grade changes through the Editor (one undo step each, a wheel drag merged into one): the drag makes one picture,
// undo/redo show the earlier look at once from the cache
void testEditorUndo()
{
    Model m;
    m.project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, false, false});
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << clip(1, 0, 0, 100);
    m.project.load(ProjectData{ProjectFormat{}, 0, m.project.media(), {}, tl, 10, 0});
    GradedThumbs thumbs;
    const QImage raw = pattern();
    // What the filmstrip draws for the clip's first tile right now
    auto draw = [&] {
        const Clip c = *TimelineOps::findClip(m.project.timeline(), 1);
        const GradedThumbs::ClipGrade g = thumbs.forClip(c);
        return g ? g.image(raw, 0) : raw;
    };
    auto computedNow = [&](int n) { return waitFor([&] { return thumbs.idle() && thumbs.computed() == n; }, 10000); };
    CHECK_EQ(draw().cacheKey(), raw.cacheKey());

    m.editor.setGradeValues({1}, {{AnimParam::GradeSaturation, 0.0}}, 0, "saturation");
    draw(); // first sight of the grade: queued right away
    CHECK(computedNow(1));
    const QImage gray = draw();
    CHECK(chroma(gray) < 2);

    // Wheel drag: 10 steps 30 ms apart, merged into one undo step; meanwhile the previous look, then one picture
    const int steps = m.project.undoStack()->count();
    for (int i = 1; i <= 10; ++i) {
        m.editor.setGradeValues({1}, {{AnimParam::GradeSaturation, 50.0 + i * 5}}, 0, "saturation", "drag");
        CHECK_EQ(draw().cacheKey(), gray.cacheKey());
        idle(30);
    }
    CHECK_EQ(m.project.undoStack()->count(), steps + 1);
    CHECK_EQ(thumbs.computed(), 1);
    CHECK(waitFor([&] { return draw().cacheKey() != gray.cacheKey(); }, 5000)); // settled: queued, then computed
    CHECK(computedNow(2));
    const QImage vivid = draw();
    CHECK(chroma(vivid) > chroma(raw));
    idle(GradedThumbs::kSettleMs + 100);
    CHECK_EQ(thumbs.computed(), 2); // one picture for the whole drag

    // Undo: the gray look at once (cached), then ungraded; redo: back to the vivid one; nothing computed anew
    m.project.undoStack()->undo();
    CHECK_EQ(draw().cacheKey(), gray.cacheKey());
    m.project.undoStack()->undo();
    CHECK_EQ(draw().cacheKey(), raw.cacheKey());
    m.project.undoStack()->redo();
    CHECK_EQ(draw().cacheKey(), gray.cacheKey());
    m.project.undoStack()->redo();
    CHECK_EQ(draw().cacheKey(), vivid.cacheKey());
    idle(GradedThumbs::kSettleMs + 100);
    draw();
    idle(50);
    CHECK_EQ(thumbs.computed(), 2);

    // Grade switched off (bypass on the Color page): the source picture; on again: the graded one from the cache
    m.editor.setGradeEnabled({1}, false);
    CHECK_EQ(draw().cacheKey(), raw.cacheKey());
    m.project.undoStack()->undo();
    CHECK_EQ(draw().cacheKey(), vivid.cacheKey());
}

// Paste a grade onto 200 clips: the UI thread only looks up and queues
void testManyClips()
{
    GradedThumbs thumbs;
    const QString lut = ":/luts/kino.cube";
    QVector<Clip> clips;
    for (int i = 0; i < 200; ++i) {
        Clip c = clip(100 + i, i * 100, 0, 100);
        setGrade(c, "lut", lut);
        setGrade(c, "saturation", 30.0);
        clips << c;
    }
    QVector<QImage> raws;
    for (int i = 0; i < 2000; ++i) raws << pattern(160, 90, i % 4);
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < clips.size(); ++i) {
        const auto g = thumbs.forClip(clips[i]);
        for (int k = 0; k < 10; ++k) g.image(raws[i * 10 + k], k * 10);
    }
    const double firstMs = t.nsecsElapsed() / 1e6;
    t.restart();
    for (int i = 0; i < clips.size(); ++i) { // next paint: the memos are still valid
        const auto g = thumbs.forClip(clips[i]);
        for (int k = 0; k < 10; ++k) g.image(raws[i * 10 + k], k * 10);
    }
    const double againMs = t.nsecsElapsed() / 1e6;
    std::printf("200 graded clips x 10 tiles on the UI thread: first look %.2f ms, next paint %.2f ms\n", firstMs, againMs);
    CHECK(firstMs < 200);
    CHECK(againMs < 100);
    // The worker catches up (the queue keeps the newest requests)
    CHECK(waitFor([&] { return thumbs.idle(); }, 60000));
    CHECK(thumbs.computed() >= 200);
}

// ---------- offscreen: the timeline and the Media Pool picture ----------

// V1: clip 1 (ungraded) | clip 2 (saturation 0), both the same source
void loadTwoClips(Model& m)
{
    m.project.addMedia(MediaInfo{g_media, "src.mp4", 125, true, false, false});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c1 = clip(1, 0, 0, 125, g_media);
    Clip c2 = clip(2, 125, 0, 125, g_media);
    setGrade(c2, "saturation", 0.0);
    tl.video[0].clips << c1 << c2;
    m.project.load(ProjectData{ProjectFormat{}, 0, m.project.media(), {}, tl, 10, 0});
}

void testTimeline(const QString& dump)
{
    Model m;
    loadTwoClips(m);
    MediaCache cache;
    TimelineView view(&m.editor);
    view.setMediaCache(&cache);
    view.resize(1400, 260);
    view.show();
    QApplication::processEvents();
    view.zoomToFit();
    QSignalSpy updates(&cache, &MediaCache::updated);
    // Settled = no new picture for a while
    auto settle = [&](int quietMs) {
        QElapsedTimer total;
        total.start();
        int seen = -1;
        QElapsedTimer quiet;
        quiet.start();
        while (total.elapsed() < 60000) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
            if (updates.count() != seen) {
                seen = updates.count();
                quiet.restart();
            } else if (quiet.elapsed() > quietMs && cache.graded()->idle()) {
                return true;
            }
        }
        return false;
    };
    GradedThumbs::setEnabled(false);
    view.update();
    CHECK(settle(1500));
    const QImage off = view.grab().toImage().convertToFormat(QImage::Format_RGB32);
    GradedThumbs::setEnabled(true);
    view.update();
    CHECK(settle(800));
    const QImage on = view.grab().toImage().convertToFormat(QImage::Format_RGB32);
    if (!dump.isEmpty()) {
        off.save(dump + "/timeline-off.png");
        on.save(dump + "/timeline-on.png");
    }
    // Columns of the clips (zoomed to fit: clip 1 left half, clip 2 right half of the frames)
    const double ppf = view.view().pxPerFrame, left = view.view().leftFrame;
    auto x = [&](double f) { return int(TimelineView::kHeaderW + (f - left) * ppf); };
    const QRect c1(x(0) + 4, 0, x(125) - x(0) - 8, on.height());
    const QRect c2(x(125) + 4, 0, x(250) - x(125) - 8, on.height());
    // Pixels that changed with the graded filmstrip
    int changed1 = 0, changed2 = 0;
    double chromaOn = 0, chromaOff = 0;
    for (int yy = 0; yy < on.height(); ++yy)
        for (int xx = 0; xx < on.width(); ++xx) {
            if (on.pixel(xx, yy) == off.pixel(xx, yy)) continue;
            if (c1.contains(xx, yy)) ++changed1;
            if (c2.contains(xx, yy)) {
                ++changed2;
                chromaOn += chroma(on, QRect(xx, yy, 1, 1));
                chromaOff += chroma(off, QRect(xx, yy, 1, 1));
            }
        }
    std::printf("timeline: changed pixels ungraded clip %d, graded clip %d (chroma %.1f -> %.1f)\n", changed1,
                changed2, changed2 ? chromaOff / changed2 : 0, changed2 ? chromaOn / changed2 : 0);
    CHECK_EQ(changed1, 0);     // ungraded clip: exactly as before
    CHECK(changed2 > 2000);    // graded clip: its tiles changed …
    CHECK(changed2 && chromaOn / changed2 < 3);  // … to gray
    CHECK(changed2 && chromaOff / changed2 > 30); // from the colorful source

    // Pixels of clip 2 that differ between two grabs
    auto differs = [&](const QImage& a, const QImage& b) {
        int n = 0;
        for (int yy = 0; yy < a.height(); ++yy)
            for (int xx = c2.left(); xx <= c2.right(); ++xx) n += a.pixel(xx, yy) != b.pixel(xx, yy);
        return n;
    };
    auto grabNow = [&] {
        QApplication::processEvents();
        return view.grab().toImage().convertToFormat(QImage::Format_RGB32);
    };
    // Grade reset: the source pictures at once; undo: the graded ones at once (cached, nothing to wait for)
    const int computedBefore = cache.graded()->computed();
    m.editor.resetGrade({2}, {}, 0, "reset");
    CHECK_EQ(differs(grabNow(), off), 0);
    m.project.undoStack()->undo();
    CHECK_EQ(differs(grabNow(), on), 0);
    CHECK(settle(GradedThumbs::kSettleMs + 300));
    CHECK_EQ(cache.graded()->computed(), computedBefore);

    // One grade change regrades each tile of clip 2 once; a wheel drag of 10 steps (repainted after each, like the
    // app; saturation 22 … 40, 50 would be neutral = no grade) costs the same: only its final state is graded
    m.editor.setGradeValues({2}, {{AnimParam::GradeSaturation, 20.0}}, 125, "saturation");
    view.repaint();
    CHECK(settle(GradedThumbs::kSettleMs + 300));
    const int perChange = cache.graded()->computed() - computedBefore;
    CHECK(perChange > 0);
    const QImage dull = grabNow();
    CHECK(differs(dull, on) > 2000);
    const int beforeDrag = cache.graded()->computed();
    for (int i = 1; i <= 10; ++i) {
        m.editor.setGradeValues({2}, {{AnimParam::GradeSaturation, 20.0 + i * 2}}, 125, "saturation", "drag");
        view.repaint();
        CHECK_EQ(differs(view.grab().toImage().convertToFormat(QImage::Format_RGB32), dull), 0); // no flicker
        idle(30);
    }
    CHECK(settle(GradedThumbs::kSettleMs + 300));
    std::printf("timeline: one grade change graded %d tiles, a drag of 10 steps %d\n", perChange,
                cache.graded()->computed() - beforeDrag);
    CHECK_EQ(cache.graded()->computed() - beforeDrag, perChange);
    CHECK(differs(grabNow(), dull) > 2000);
    m.project.undoStack()->undo();
    m.project.undoStack()->undo();
    CHECK_EQ(differs(grabNow(), on), 0); // back to gray, from the cache

    if (!dump.isEmpty()) {
        // Project for an app screenshot (timeline + Media Pool), all clips from the one source: saturation 0 first
        // (the timeline's picture in the Media Pool), ungraded, with the sepia LUT
        Timeline tl = m.project.timeline();
        tl.video[0].clips = {clip(2, 0, 0, 125, g_media), clip(1, 125, 0, 125, g_media), clip(3, 250, 0, 125, g_media)};
        setGrade(tl.video[0].clips[0], "saturation", 0.0);
        setGrade(tl.video[0].clips[2], "lut", QString(":/luts/sepia.cube"));
        ProjectData d{m.project.format(), 0, m.project.media(), {}, tl, 10, 0};
        QString error;
        CHECK(ProjectFile::save(d, dump + "/graded.schneidi", &error));
    }
}

void testPosters()
{
    // Which frame: earliest picture, topmost track on a tie; hidden tracks, disabled clips and titles do not count
    Project p;
    p.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, true, false});
    p.addMedia(MediaInfo{"/x/b.mp4", "b.mp4", 3000, true, true, false});
    p.addMedia(MediaInfo{"/x/song.wav", "song.wav", 3000, false, true, false});
    Timeline tl;
    tl.video.resize(3);
    tl.video[0].clips << clip(1, 10, 100, 50, "/x/a.mp4");
    tl.video[1].clips << clip(2, 10, 200, 50, "/x/b.mp4");
    auto src = TimelinePosters::sourceOf(p, tl);
    CHECK_EQ(src.path, QString("/x/b.mp4"));
    CHECK_EQ(src.frame, 200);
    CHECK(src.grade.isNull());
    tl.video[1].clips[0].start = 12; // V1 starts earlier
    CHECK_EQ(TimelinePosters::sourceOf(p, tl).path, QString("/x/a.mp4"));
    Clip title;
    title.id = 3;
    title.kind = ClipKind::Title;
    title.out = 49;
    tl.video[2].clips << title;
    CHECK_EQ(TimelinePosters::sourceOf(p, tl).path, QString("/x/a.mp4"));
    tl.video[0].hidden = true;
    CHECK_EQ(TimelinePosters::sourceOf(p, tl).path, QString("/x/b.mp4"));
    tl.video[1].clips[0].enabled = false;
    CHECK(TimelinePosters::sourceOf(p, tl).path.isEmpty());
    tl.video[0].hidden = false;
    setGrade(tl.video[0].clips[0], "saturation", 0.0);
    CHECK(!TimelinePosters::sourceOf(p, tl).grade.isNull());
    tl.video[0].clips[0].reverse = true; // reversed: the first frame is the end of the range in the file
    CHECK(TimelinePosters::sourceOf(p, tl).frame > 2000);

    // The picture itself: graded first frame, again after the grade changed
    Model m;
    loadTwoClips(m);
    Timeline swapped = m.project.timeline(); // graded clip first
    std::swap(swapped.video[0].clips[0].start, swapped.video[0].clips[1].start);
    std::swap(swapped.video[0].clips[0], swapped.video[0].clips[1]);
    m.project.setTimeline(swapped);
    TimelinePosters posters(&m.project, QSize(144, 81));
    QSignalSpy changed(&posters, &TimelinePosters::changed);
    const int seq = m.project.currentSequence();
    CHECK(posters.poster(seq).isNull()); // asked for, comes from the worker
    CHECK(changed.wait(30000));
    const QImage gray = posters.poster(seq);
    CHECK_EQ(gray.size(), QSize(144, 81));
    CHECK(!gray.isNull() && chroma(gray) < 3);
    // Grade removed: picture in color after the settle time
    Timeline plain = m.project.timeline();
    plain.video[0].clips[0].effects.clear();
    changed.clear();
    m.project.setTimeline(plain);
    CHECK(changed.wait(30000));
    CHECK(chroma(posters.poster(seq)) > 15);
    // Unrelated edit (a marker): nothing computed anew
    Timeline marked = m.project.timeline();
    marked.markers << 10;
    changed.clear();
    m.project.setTimeline(marked);
    idle(GradedThumbs::kSettleMs + 300);
    CHECK_EQ(changed.count(), 0);
    // Wheel drag on the first clip (10 changes 30 ms apart): one new picture once it is still
    for (int i = 0; i < 10; ++i) {
        Timeline step = m.project.timeline();
        setGrade(step.video[0].clips[0], "saturation", 36.0 - i * 4);
        m.project.setTimeline(step);
        idle(30);
    }
    CHECK(changed.wait(30000));
    idle(GradedThumbs::kSettleMs + 300);
    CHECK_EQ(changed.count(), 1);
    CHECK(chroma(posters.poster(seq)) < 3); // the last state: saturation 0
}

// The Media Pool: the timeline entry shows its first picture graded, the entry of the source stays neutral
void testMediaPool()
{
    Model m;
    loadTwoClips(m);
    Timeline swapped = m.project.timeline(); // graded clip first
    std::swap(swapped.video[0].clips[0].start, swapped.video[0].clips[1].start);
    std::swap(swapped.video[0].clips[0], swapped.video[0].clips[1]);
    m.project.setTimeline(swapped);
    Engine engine;
    MediaPool pool(&m.project, &engine);
    pool.setSaveSettings(false);
    pool.resize(700, 400);
    pool.show();
    auto* list = pool.findChild<QListWidget*>();
    if (!CHECK(list)) return;
    auto icon = [&](const QString& item) {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole).toString() == item)
                return list->item(i)->icon().pixmap(QSize(144, 81)).toImage().convertToFormat(QImage::Format_RGB32);
        return QImage();
    };
    // Below/right of the type badge (top left): only the picture
    const QRect picture(60, 36, 80, 42);
    const QString timeline = MediaPool::sequenceItem(m.project.currentSequence());
    auto timelineChroma = [&] { const QImage i = icon(timeline); return i.isNull() ? -1.0 : chroma(i, picture); };
    auto sourceChroma = [&] { const QImage i = icon(g_media); return i.isNull() ? -1.0 : chroma(i, picture); };
    CHECK(waitFor([&] { return timelineChroma() >= 0 && timelineChroma() < 3 && sourceChroma() > 30; }, 30000));
    std::printf("Media Pool: timeline picture chroma %.1f, source %.1f\n", timelineChroma(), sourceChroma());
    // Grade undone (here: removed): the timeline picture in color, the source as before
    const double source = sourceChroma();
    m.editor.resetGrade({2}, {}, 0, "reset");
    CHECK(waitFor([&] { return timelineChroma() > 30; }, 30000));
    CHECK_EQ(sourceChroma(), source);
    m.project.undoStack()->undo();
    CHECK(waitFor([&] { return timelineChroma() >= 0 && timelineChroma() < 3; }, 30000));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("graded-thumbs");
    std::setlocale(LC_NUMERIC, "C");
    Theme::apply(app);
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    testSpecAndApply(tmp.path());
    testCacheAndSettle();
    testEditorUndo();
    testManyClips();

    if (!Check::haveFfmpeg()) {
        std::printf("Hinweis: ffmpeg fehlt, Timeline-/Media-Pool-Test übersprungen\n");
        return Check::result();
    }
    // With a dump folder the medium stays there (the saved project refers to it)
    const QString dump = qEnvironmentVariable("GRADED_THUMBS_DUMP");
    if (!dump.isEmpty()) QDir().mkpath(dump);
    g_media = Check::makeMedia(QDir(dump.isEmpty() ? tmp.path() : dump).filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=5", "-c:v", "libx264",
                                "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testTimeline(dump);
    testPosters();
    testMediaPool();
    return Check::result();
}
