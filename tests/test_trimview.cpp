// Trim view (two-up/four-up in the viewer while trimming, like DaVinci): which frames are shown for each kind
// of trim (TrimFrames), the signals of the timeline while dragging, the TrimView widget, the background frame
// fetcher, and offscreen the whole main window during a ripple drag (TRIMVIEW_DUMP=<dir> saves screenshots).
#include "check.h"

#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/TrimFrames.h"
#include "engine/Engine.h"
#include "engine/StillFetcher.h"
#include "ui/TrimView.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <clocale>
#include <cmath>

using TimelineOps::Edge;
using TimelineOps::TrimKind;

namespace {

Clip clip(int id, const QString& path, int start, int in, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    return c;
}

// V1: a (10..109 of a.mp4) | b (200..299 of b.mp4) | c (50..99 of a.mp4), all adjacent; A1: audio of a
struct Model {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};

    Model()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, true, false});
        project.addMedia(MediaInfo{"/x/b.mp4", "b.mp4", 3000, true, true, false});
        project.addMedia(MediaInfo{"/x/still.png", "still.png", 1, true, false, true});
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        tl.video[0].clips << clip(1, "/x/a.mp4", 0, 10, 100) << clip(2, "/x/b.mp4", 100, 200, 100)
                          << clip(3, "/x/a.mp4", 200, 50, 50);
        tl.audio[0].clips << clip(4, "/x/a.mp4", 0, 10, 100);
        project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 10, 0});
    }
    TrimFrames::View view(TrimKind kind, int clipId, Edge edge, int delta)
    {
        const TimelineOps::TrimEdit e = editor.trimEdit(kind, clipId, edge);
        const int d = editor.clampTrimEdit(e, delta);
        const Project* p = &project;
        return TrimFrames::compute(editor.previewTrimEdit(e, d), e, d, [p](const QString& f) { return p->mediaInfo(f); });
    }
};

void checkPane(const TrimFrames::Pane& p, int clipId, bool out, int fileFrame, int timelineFrame, int line)
{
    Check::report(p.clipId == clipId && p.out == out && p.fileFrame == fileFrame && p.timelineFrame == timelineFrame,
                  "pane", __FILE__, line,
                  QString("clip %1 out %2 file %3 timeline %4 (expected %5 %6 %7 %8)")
                      .arg(p.clipId).arg(p.out).arg(p.fileFrame).arg(p.timelineFrame)
                      .arg(clipId).arg(out).arg(fileFrame).arg(timelineFrame));
}
#define PANE(p, id, out, file, tl) checkPane((p), (id), (out), (file), (tl), __LINE__)

void testFrames()
{
    Model m;
    // Roll the cut a|b by +5: left = new Out of a, right = new In of b (both move with the cut)
    auto v = m.view(TrimKind::Roll, 1, Edge::End, 5);
    CHECK(v.kind == TrimKind::Roll);
    CHECK_EQ(v.delta, 5);
    if (CHECK_EQ(v.main.size(), 2)) {
        PANE(v.main[0], 1, true, 114, 104);
        PANE(v.main[1], 2, false, 205, 105);
        CHECK_EQ(v.main[0].path, QString("/x/a.mp4"));
        CHECK_EQ(v.main[1].name, QString("b.mp4"));
        CHECK_EQ(v.main[0].sourceFrame, 114);
    }
    CHECK(v.small.isEmpty());
    // grabbed on the right clip of the cut: same edit
    CHECK(m.view(TrimKind::Roll, 2, Edge::Start, 5) == v);

    // Ripple the end of a by -10: a ends earlier, b follows -> b's In stays 200
    v = m.view(TrimKind::Ripple, 1, Edge::End, -10);
    if (CHECK_EQ(v.main.size(), 2)) {
        PANE(v.main[0], 1, true, 99, 89);
        PANE(v.main[1], 2, false, 200, 90);
    }
    // Ripple the start of b by +10: Out of a unchanged, new In of b
    v = m.view(TrimKind::Ripple, 2, Edge::Start, 10);
    if (CHECK_EQ(v.main.size(), 2)) {
        PANE(v.main[0], 1, true, 109, 99);
        PANE(v.main[1], 2, false, 210, 100);
    }
    // Ripple the end of the last clip: nothing on the right
    v = m.view(TrimKind::Ripple, 3, Edge::End, -5);
    if (CHECK_EQ(v.main.size(), 2)) {
        PANE(v.main[0], 3, true, 94, 244);
        CHECK_EQ(v.main[1].clipId, 0);
        CHECK(!v.main[1].out);
    }
    // Ripple the start of the first clip: nothing on the left
    v = m.view(TrimKind::Ripple, 1, Edge::Start, 3);
    if (CHECK_EQ(v.main.size(), 2)) {
        CHECK_EQ(v.main[0].clipId, 0);
        CHECK(v.main[0].out);
        PANE(v.main[1], 1, false, 13, 0);
    }

    // Slip b by +7: large = In/Out of b (content moves), small = neighbours (unchanged)
    v = m.view(TrimKind::Slip, 2, Edge::End, 7);
    if (CHECK_EQ(v.main.size(), 2) && CHECK_EQ(v.small.size(), 2)) {
        PANE(v.main[0], 2, false, 207, 100);
        PANE(v.main[1], 2, true, 306, 199);
        PANE(v.small[0], 1, true, 109, 99);
        PANE(v.small[1], 3, false, 50, 200);
    }
    // Slide b by -4: large = neighbours' changed Out/In, small = b itself (content unchanged)
    v = m.view(TrimKind::Slide, 2, Edge::End, -4);
    if (CHECK_EQ(v.main.size(), 2) && CHECK_EQ(v.small.size(), 2)) {
        PANE(v.main[0], 1, true, 105, 95);
        PANE(v.main[1], 3, false, 46, 196);
        PANE(v.small[0], 2, false, 200, 96);
        PANE(v.small[1], 2, true, 299, 195);
    }

    // Audio-only edit: no trim view (viewer stays normal)
    Timeline tl = m.project.timeline();
    TimelineOps::TrimEdit audioOnly{TrimKind::Ripple, {4}, {}, Edge::End};
    CHECK(TrimFrames::compute(tl, audioOnly, 0, {}).isNull());
    // Linked video+audio: the video clip is shown
    TimelineOps::TrimEdit linked{TrimKind::Ripple, {4, 1}, {}, Edge::End};
    v = TrimFrames::compute(tl, linked, 0, {});
    if (CHECK_EQ(v.main.size(), 2)) CHECK_EQ(v.main[0].clipId, 1);

    // Selection tool edge trim (no ripple): a gap opens, the nearest clip after it is shown
    TimelineOps::trimClips(tl, {1}, Edge::End, -20, [](const Clip&) { return 0; });
    v = TrimFrames::compute(tl, TimelineOps::TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, -20, {});
    if (CHECK_EQ(v.main.size(), 2)) {
        PANE(v.main[0], 1, true, 89, 79);
        PANE(v.main[1], 2, false, 200, 100);
    }
}

void testRetimedPanes()
{
    const MediaInfo media{"/x/a.mp4", "a.mp4", 1000, true, false, false};
    const MediaInfo still{"/x/s.png", "s.png", 1, true, false, true};
    auto lookup = [&](const QString& p) -> const MediaInfo* { return p == media.path ? &media : p == still.path ? &still : nullptr; };

    Clip c = clip(1, media.path, 0, 10, 50);
    c.speed = 2.0; // material frame m plays file frame 2m
    PANE(TrimFrames::pane(c, false, lookup), 1, false, 20, 0);
    PANE(TrimFrames::pane(c, true, lookup), 1, true, 118, 49);

    c = clip(1, media.path, 0, 0, 10);
    c.reverse = true; // material 0 = last frame of the file
    CHECK_EQ(TrimFrames::pane(c, false, lookup).fileFrame, 999);
    CHECK_EQ(TrimFrames::pane(c, true, lookup).fileFrame, 990);

    c = clip(1, media.path, 0, 30, 40);
    c.freeze = true;
    c.freezeFrame = 42; // every frame shows the still frame
    CHECK_EQ(TrimFrames::pane(c, false, lookup).fileFrame, 42);
    CHECK_EQ(TrimFrames::pane(c, true, lookup).fileFrame, 42);
    CHECK_EQ(TrimFrames::pane(c, true, lookup).sourceFrame, 69);

    c = clip(1, still.path, 0, 0, 100); // image: always frame 0
    CHECK_EQ(TrimFrames::pane(c, true, lookup).fileFrame, 0);

    c = clip(1, media.path, 0, 990, 30); // out of the file (missing media length change): clamped
    CHECK_EQ(TrimFrames::pane(c, true, lookup).fileFrame, 999);

    Clip title;
    title.id = 5;
    title.kind = ClipKind::Title;
    title.title.text = "Hallo";
    title.out = 49;
    const TrimFrames::Pane t = TrimFrames::pane(title, true, lookup);
    CHECK(t.path.isEmpty()); // nothing to decode
    CHECK_EQ(t.name, QString("Hallo"));
    CHECK_EQ(t.clipId, 5);
}

void mouse(QWidget& w, QEvent::Type type, QPoint pt, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QApplication::sendEvent(&w, &e);
}

// The timeline reports the frames while dragging (only changes) and ends the view on release
void testTimelineSignals()
{
    Model m;
    TimelineView tv(&m.editor);
    tv.resize(1400, 400);
    tv.setSnapping(false);
    tv.show();
    QApplication::processEvents();
    QVector<TrimFrames::View> views;
    int ended = 0;
    QObject::connect(&tv, &TimelineView::trimFramesChanged, [&](const TrimFrames::View& v) { views << v; });
    QObject::connect(&tv, &TimelineView::trimFramesEnded, [&] { ++ended; });
    const double ppf = tv.view().pxPerFrame;
    auto x = [&](double frame) { return int(std::lround(TimelineView::kHeaderW + (frame - tv.view().leftFrame) * ppf)); };
    const int y = TimelineView::kRulerH + 40; // V1 (only video track), below the title bar

    // Trim tool, exactly on the cut a|b: roll
    tv.setTool(TimelineView::Tool::Trim);
    const QPoint cut(x(100), y);
    mouse(tv, QEvent::MouseButtonPress, cut, Qt::LeftButton);
    if (CHECK_EQ(views.size(), 1)) { // shown at once on the press, delta 0
        CHECK(views[0].kind == TrimKind::Roll);
        CHECK_EQ(views[0].delta, 0);
        if (CHECK_EQ(views[0].main.size(), 2)) {
            PANE(views[0].main[0], 1, true, 109, 99);
            PANE(views[0].main[1], 2, false, 200, 100);
        }
    }
    mouse(tv, QEvent::MouseMove, cut + QPoint(int(std::lround(6 * ppf)), 0), Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, cut + QPoint(int(std::lround(6 * ppf)), 0), Qt::LeftButton); // same delta: no signal
    if (CHECK_EQ(views.size(), 2)) {
        CHECK_EQ(views[1].delta, 6);
        if (CHECK_EQ(views[1].main.size(), 2)) {
            PANE(views[1].main[0], 1, true, 115, 105);
            PANE(views[1].main[1], 2, false, 206, 106);
        }
    }
    CHECK_EQ(ended, 0);
    mouse(tv, QEvent::MouseButtonRelease, cut + QPoint(int(std::lround(6 * ppf)), 0), Qt::NoButton);
    CHECK_EQ(ended, 1);
    CHECK_EQ(TimelineOps::findClip(m.project.timeline(), 1)->out, 115); // roll applied

    // Slip in the middle of b (below the title bar): four-up
    views.clear();
    const QPoint mid(x(150), y + 10);
    mouse(tv, QEvent::MouseButtonPress, mid, Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, mid - QPoint(int(std::lround(3 * ppf)), 0), Qt::LeftButton);
    if (CHECK_EQ(views.size(), 2)) {
        CHECK(views[1].kind == TrimKind::Slip);
        CHECK_EQ(views[1].small.size(), 2);
        // In of b as the slip preview computes it (TimelineOps decides the direction)
        const Clip* b = TimelineOps::findClip(m.editor.previewTrimEdit(m.editor.trimEdit(TrimKind::Slip, 2), views[1].delta), 2);
        if (CHECK(b) && CHECK_EQ(views[1].main.size(), 2)) PANE(views[1].main[0], 2, false, b->in, 106);
    }
    mouse(tv, QEvent::MouseButtonRelease, mid, Qt::NoButton);
    CHECK_EQ(ended, 2);

    // Selection tool: dragging the end of c (last clip) is an edge trim -> two-up too
    tv.setTool(TimelineView::Tool::Select);
    views.clear();
    const QPoint edge(x(250) - 2, y + 10);
    mouse(tv, QEvent::MouseButtonPress, edge, Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, edge - QPoint(int(std::lround(8 * ppf)), 0), Qt::LeftButton);
    if (CHECK_EQ(views.size(), 2) && CHECK_EQ(views[1].main.size(), 2)) {
        PANE(views[1].main[0], 3, true, 91, 241);
        CHECK_EQ(views[1].main[1].clipId, 0);
        CHECK_EQ(views[1].delta, -8);
    }
    mouse(tv, QEvent::MouseButtonRelease, edge, Qt::NoButton);
    CHECK_EQ(ended, 3);

    // Plain click on a clip body (move, not trim): no trim view
    views.clear();
    mouse(tv, QEvent::MouseButtonPress, {x(50), y + 10}, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, {x(50), y + 10}, Qt::NoButton);
    CHECK(views.isEmpty());
    CHECK_EQ(ended, 3);
}

QImage solid(const QColor& c)
{
    QImage img(160, 90, QImage::Format_RGB32);
    img.fill(c);
    return img;
}

void testWidget()
{
    Model m;
    TrimView w;
    w.resize(900, 500);
    w.setFps(25);
    w.setAspect({1920, 1080});
    w.setView(m.view(TrimKind::Roll, 1, Edge::End, 5));
    CHECK(!w.isExact(false, 0));
    CHECK(w.setImage("/x/a.mp4", 114, solid(Qt::red)));
    CHECK(w.setImage("/x/b.mp4", 205, solid(Qt::blue)));
    CHECK(!w.setImage("/x/b.mp4", 206, solid(Qt::green))); // nobody waits for that frame
    CHECK(w.isExact(false, 0) && w.isExact(false, 1));

    const QVector<QRect> r = w.mainRects();
    if (CHECK_EQ(r.size(), 2)) {
        CHECK(r[0].right() < r[1].left()); // side by side, left = Out
        CHECK(std::abs(r[0].width() * 9 - r[0].height() * 16) < 40); // project aspect
        CHECK(QRect(QPoint(), w.size()).contains(r[0].united(r[1])));
        const QImage g = w.grab().toImage();
        CHECK_EQ(QColor(g.pixel(r[0].center())), QColor(Qt::red));
        CHECK_EQ(QColor(g.pixel(r[1].center())), QColor(Qt::blue));
        if (const QString dump = qEnvironmentVariable("TRIMVIEW_DUMP"); !dump.isEmpty()) g.save(QDir(dump).filePath("two-up.png"));
    }
    CHECK(w.smallRects().isEmpty());

    // Next delta: same clips -> old images stay as stand-ins (not exact) until the new frames arrive
    w.setView(m.view(TrimKind::Roll, 1, Edge::End, 6));
    CHECK(!w.isExact(false, 0));
    QImage g = w.grab().toImage();
    CHECK_EQ(QColor(g.pixel(w.mainRects()[0].center())), QColor(Qt::red));
    CHECK(w.setImage("/x/a.mp4", 115, solid(Qt::yellow)));
    CHECK(w.isExact(false, 0));

    // Slip: four-up, small panes above the large ones and half as large
    w.setView(m.view(TrimKind::Slip, 2, Edge::End, 7));
    const QVector<QRect> big = w.mainRects(), small = w.smallRects();
    if (CHECK_EQ(big.size(), 2) && CHECK_EQ(small.size(), 2)) {
        CHECK(small[0].bottom() < big[0].top());
        CHECK(std::abs(small[0].width() * 2 - big[0].width()) <= 2);
        w.setImage("/x/b.mp4", 207, solid(Qt::green));
        w.setImage("/x/a.mp4", 109, solid(Qt::magenta));
        g = w.grab().toImage();
        CHECK_EQ(QColor(g.pixel(big[0].center())), QColor(Qt::green));
        CHECK_EQ(QColor(g.pixel(small[0].center())), QColor(Qt::magenta));
        CHECK(QColor(g.pixel(big[1].center())) != QColor(Qt::green)); // Out of b not decoded yet
        if (const QString dump = qEnvironmentVariable("TRIMVIEW_DUMP"); !dump.isEmpty()) g.save(QDir(dump).filePath("four-up.png"));
    }
}

// Luma ramp: frame n has Y = 16 + 2n (gray in RGB ~ 2.33 n) -> the frame number can be read back from the image
QString makeRamp(const QString& dir)
{
    return Check::makeMedia(QDir(dir).filePath("ramp.mp4"),
                            {"-f", "lavfi", "-i", "color=black:size=320x180:rate=25:duration=4", "-vf",
                             "geq=lum='16+2*N':cb=128:cr=128", "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p"});
}

int frameOf(const QImage& img, const QRect& r)
{
    double sum = 0;
    int n = 0;
    for (int y = r.top() + r.height() / 4; y < r.bottom() - r.height() / 4; y += 3)
        for (int x = r.left() + r.width() / 4; x < r.right() - r.width() / 4; x += 3) {
            sum += qGray(img.pixel(x, y));
            ++n;
        }
    return n ? int(std::lround(sum / n * 219.0 / 255.0 / 2.0)) : -1;
}

bool waitUntil(const std::function<bool()>& ok, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!ok() && t.elapsed() < ms) QTest::qWait(20);
    return ok();
}

void testFetcher(const QString& ramp)
{
    StillFetcher f;
    ProjectFormat fmt;
    fmt.width = 320;
    fmt.height = 180;
    f.setFormat(fmt);
    QVector<QPair<int, QImage>> got;
    QObject::connect(&f, &StillFetcher::ready, [&](const QString&, int frame, const QImage& img) { got << qMakePair(frame, img); });
    f.request({{ramp, 10}, {ramp, 60}});
    CHECK(waitUntil([&] { return got.size() == 2; }));
    for (const auto& [frame, img] : got) {
        CHECK(!img.isNull());
        CHECK(std::abs(frameOf(img, img.rect()) - frame) <= 1);
    }
    CHECK(!f.cached(ramp, 60).isNull());
    CHECK(f.cached(ramp, 61).isNull());
    // cached frames are not decoded again
    const int decoded = f.decodedCount();
    f.request({{ramp, 10}});
    QTest::qWait(100);
    CHECK_EQ(f.decodedCount(), decoded);
    // latest request wins: a burst of requests (fast drag) only decodes what is current when the worker is free
    got.clear();
    for (int i = 0; i < 40; ++i) f.request({{ramp, 20 + i}, {ramp, 90 - i}});
    CHECK(waitUntil([&] { return f.pending() == 0 && !f.cached(ramp, 59).isNull() && !f.cached(ramp, 51).isNull(); }));
    QTest::qWait(100);
    CHECK(f.decodedCount() - decoded < 20);
}

// Whole window: trim tool ripple drag on the end of the first clip -> the viewer shows two-up with the real
// frames (Out of clip 1 left, In of clip 2 right) and goes back to the normal picture on release
void testMainWindow(const QString& ramp)
{
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({ramp, ramp}, true); // V1: ramp 0..99 | ramp 0..99
    QApplication::processEvents();
    auto* tv = w.findChild<TimelineView*>();
    auto* viewer = w.findChild<Viewer*>();
    if (!CHECK(tv && viewer)) return;
    tv->setSnapping(false);
    tv->zoomToFit();
    tv->setTool(TimelineView::Tool::Trim);
    QApplication::processEvents();
    const double ppf = tv->view().pxPerFrame;
    auto x = [&](double frame) { return TimelineView::kHeaderW + (frame - tv->view().leftFrame) * ppf; };
    QVector<TrimFrames::View> views;
    QObject::connect(tv, &TimelineView::trimFramesChanged, [&](const TrimFrames::View& v) { views << v; });
    // V1 row: depends on the default tracks/scroll -> find it with clicks (press + release without moving changes
    // nothing) in the ripple zone of the end of clip 1, left of the roll zone at the cut
    QPoint grab;
    for (int y = TimelineView::kRulerH + 4; y < tv->height() && views.isEmpty(); y += 6) {
        grab = QPoint(int(x(100)) - 6, y);
        mouse(*tv, QEvent::MouseButtonPress, grab, Qt::LeftButton);
        if (views.isEmpty()) mouse(*tv, QEvent::MouseButtonRelease, grab, Qt::NoButton);
    }
    if (!CHECK(!views.isEmpty()) || !CHECK(views.last().kind == TrimKind::Ripple)) {
        mouse(*tv, QEvent::MouseButtonRelease, grab, Qt::NoButton);
        return;
    }
    CHECK(viewer->trimShown());
    const QPoint to = grab - QPoint(int(std::lround(10 * ppf)), 0);
    mouse(*tv, QEvent::MouseMove, to, Qt::LeftButton);
    const TrimFrames::View v = viewer->trimView()->view();
    if (CHECK_EQ(v.main.size(), 2)) {
        const int outFrame = v.main[0].fileFrame;
        CHECK(outFrame < 99 && outFrame > 80); // shortened by ~10 frames
        CHECK_EQ(v.main[1].fileFrame, 0);
        CHECK(waitUntil([&] { return viewer->trimView()->isExact(false, 0) && viewer->trimView()->isExact(false, 1); }));
        const QImage shot = viewer->trimView()->grab().toImage();
        const QVector<QRect> r = viewer->trimView()->mainRects();
        CHECK(std::abs(frameOf(shot, r[0]) - outFrame) <= 1);
        CHECK(frameOf(shot, r[1]) <= 1);
        if (const QString dump = qEnvironmentVariable("TRIMVIEW_DUMP"); !dump.isEmpty())
            w.grab().save(QDir(dump).filePath("window-ripple.png"));
    }
    mouse(*tv, QEvent::MouseButtonRelease, to, Qt::NoButton);
    CHECK(!viewer->trimShown());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("trimview");
    I18n::install("de");
    Theme::apply(app);
    testFrames();
    testRetimedPanes();
    testTimelineSignals();
    testWidget();
    if (!Check::haveFfmpeg()) {
        std::printf("Hinweis: ffmpeg fehlt, Bilder/Hauptfenster übersprungen\n");
        return Check::result();
    }
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    Check::initMlt();
    const QString ramp = makeRamp(tmp.path());
    if (!CHECK(!ramp.isEmpty())) return Check::result();
    testFetcher(ramp);
    testMainWindow(ramp);
    return Check::result();
}
