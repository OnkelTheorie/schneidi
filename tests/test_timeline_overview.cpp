// SPDX-License-Identifier: AGPL-3.0-only
// Overview strip above the timeline (TimelineOverview in TimelinePanel, 0.25 package R4):
//  - core rule: model changes (trimming, deleting a clip, a marker, a story section, undo) never change the ViewState and
//    the strip never asks for a scroll/zoom by itself; it only follows (its timeline, its rectangle)
//  - input: a click outside the rectangle = playhead there + the view centres it; a click inside = playhead only;
//    dragging the rectangle scrolls (zoom stays), dragging an edge zooms (the other edge stays)
//  - the window: off by default, Timeline › Timeline view › "Überblick-Timeline" shows it on Edit only (not on
//    Deliver/Color), back on Edit it is there again; a test run does not save the switch
//  - tempo: painting a 2-hour timeline with 2000 clips (rebuild of its picture) and a playhead step (ms printed)
// OVERVIEW_DUMP=<folder>: a picture of the panel with the strip.
#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/timeline/TimelineOverview.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

Clip clip(int id, int start, int length, const QString& path = "/x/a.mp4")
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = 0;
    c.out = length - 1;
    return c;
}

void mouse(QWidget& w, QEvent::Type type, QPointF pt, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                  Qt::NoModifier);
    QApplication::sendEvent(&w, &e);
}

void drag(QWidget& w, QPointF from, QPointF to)
{
    mouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton);
    mouse(w, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
    mouse(w, QEvent::MouseMove, to, Qt::LeftButton);
    mouse(w, QEvent::MouseButtonRelease, to, Qt::NoButton);
}

void click(QWidget& w, QPointF at)
{
    mouse(w, QEvent::MouseButtonPress, at, Qt::LeftButton);
    mouse(w, QEvent::MouseButtonRelease, at, Qt::NoButton);
}

bool sameView(const ViewState& a, const ViewState& b)
{
    return a.pxPerFrame == b.pxPerFrame && a.leftFrame == b.leftFrame && a.scrollY == b.scrollY;
}

struct Fixture {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    TimelinePanel panel{&editor};
    TimelineView* tv = panel.view();
    TimelineOverview* ov = panel.overview();
    int seeked = -1;

    Fixture()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 30000, true, false, false});
        Timeline tl;
        tl.video.resize(2);
        tl.audio.resize(1);
        // 0..3000 in clips of 300 with a gap, 1 = the clip that is trimmed, 4 = the one deleted
        tl.video[0].clips << clip(1, 0, 300) << clip(2, 300, 300) << clip(3, 900, 300) << clip(4, 1500, 300)
                          << clip(5, 2700, 300);
        tl.video[1].clips << clip(6, 400, 200);
        tl.audio[0].clips << clip(7, 0, 3000);
        project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 8, 0});
        QObject::connect(tv, &TimelineView::seekRequested, [this](int f) {
            seeked = f;
            tv->setPlayhead(f); // what MainWindow does after the seek
        });
        panel.resize(1400, 420);
        panel.show();
        QApplication::processEvents();
        // A fixed start view: 1 px per frame from frame 200 (a part of the timeline in view)
        tv->setZoomAndScroll(1.0, 200, 0);
    }
};

void testHiddenByDefault()
{
    Fixture f;
    CHECK(!f.panel.overviewShown());
    CHECK(f.ov->isHidden());
    f.panel.setOverviewEnabled(true);
    CHECK(f.panel.overviewShown());
    CHECK(f.ov->isVisible());
    CHECK_EQ(f.ov->height(), TimelineOverview::kHeight);
    // Lined up with the lanes: same width as the view, frame 0 at the lanes' left edge
    CHECK_EQ(f.ov->width(), f.tv->width());
    CHECK_EQ(f.ov->leftInset(), TimelineView::kHeaderW);
    CHECK_EQ(f.ov->mapTo(&f.panel, QPoint(0, 0)).x(), f.tv->mapTo(&f.panel, QPoint(0, 0)).x());
    // The page forbids it (Deliver/Color): gone, the user's switch stays; allowed again: back
    f.panel.setOverviewOnPage(false);
    CHECK(f.ov->isHidden());
    f.panel.setOverviewOnPage(true);
    CHECK(f.ov->isVisible());
    // It shows the project's timeline and the view's range
    CHECK_EQ(f.ov->timeline().video[0].clips.size(), 5);
    CHECK(std::abs(f.ov->visibleFrom() - 200) < 1e-9);
    CHECK(std::abs(f.ov->visibleTo() - (200 + f.tv->width() - TimelineView::kHeaderW)) < 1e-6);
    if (const QString dir = qEnvironmentVariable("OVERVIEW_DUMP"); !dir.isEmpty()) f.panel.grab().save(dir + "/panel.png");
}

// Core rule: what the model does never moves the view, and the strip never asks for it on its own
void testModelChangesKeepView()
{
    Fixture f;
    f.panel.setOverviewEnabled(true);
    QSignalSpy scroll(f.ov, &TimelineOverview::scrollRequested), range(f.ov, &TimelineOverview::rangeRequested),
        seek(f.ov, &TimelineOverview::seekRequested);
    const ViewState before = f.tv->view();
    const QRectF rectBefore = f.ov->visibleRect();

    f.editor.trimClip(1, TimelineOps::Edge::End, -100); // trim
    QApplication::processEvents();
    CHECK(sameView(f.tv->view(), before));
    CHECK_EQ(f.ov->timeline().video[0].clips[0].length(), 200); // the strip follows the model

    f.sel.set({4}); // delete the clip at 1500
    f.editor.deleteSelection();
    QApplication::processEvents();
    CHECK(sameView(f.tv->view(), before));
    CHECK_EQ(f.ov->timeline().video[0].clips.size(), 4);

    f.editor.toggleMarker(1234); // a marker
    QApplication::processEvents();
    CHECK(sameView(f.tv->view(), before));
    CHECK(f.ov->timeline().markers.contains(1234));

    f.editor.setSections({StorySection{StoryKind::Hook, {}, 0, 600}}, "test"); // a story section
    QApplication::processEvents();
    CHECK(sameView(f.tv->view(), before));
    CHECK_EQ(f.ov->timeline().sections.size(), 1);

    // The end moves out (the strip's scale changes): the view still stays
    f.editor.trimClip(7, TimelineOps::Edge::End, 6000);
    QApplication::processEvents();
    CHECK(sameView(f.tv->view(), before));
    CHECK(f.ov->visibleRect().width() < rectBefore.width()); // the same range is smaller on a longer strip

    while (f.project.undoStack()->canUndo()) f.project.undoStack()->undo();
    QApplication::processEvents();
    CHECK(sameView(f.tv->view(), before));
    CHECK_EQ(f.ov->timeline().video[0].clips.size(), 5);

    // Switching the strip off and on, and the playhead moving, do not touch it either
    f.panel.setOverviewEnabled(false);
    f.tv->setPlayhead(2500);
    f.panel.setOverviewEnabled(true);
    CHECK(sameView(f.tv->view(), before));
    CHECK_EQ(f.ov->playhead(), 2500);
    CHECK(scroll.isEmpty() && range.isEmpty() && seek.isEmpty()); // nothing asked by itself
}

void testClickAndDrag()
{
    Fixture f;
    f.panel.setOverviewEnabled(true);
    const double vis = f.tv->width() - TimelineView::kHeaderW; // frames in view at 1 px/frame
    const int y = TimelineOverview::kHeight / 2;

    // Click outside the rectangle (on frame ~2600): the playhead goes there, the view centres it, zoom stays
    const double x2600 = f.ov->frameToX(2600);
    CHECK(!f.ov->visibleRect().contains(QPointF(x2600, y)));
    click(*f.ov, {x2600, double(y)});
    CHECK(std::abs(f.seeked - 2600) <= 3);
    CHECK_EQ(f.tv->playhead(), f.seeked);
    CHECK_EQ(f.tv->view().pxPerFrame, 1.0);
    CHECK(std::abs(f.tv->view().leftFrame + vis / 2 - f.seeked) <= 1);
    CHECK(f.ov->visibleRect().contains(QPointF(x2600, y))); // the rectangle followed the view

    // A click inside the rectangle: only the playhead
    const double left = f.tv->view().leftFrame;
    const QRectF r = f.ov->visibleRect();
    const double inside = r.left() + r.width() * 0.4;
    click(*f.ov, {inside, double(y)});
    CHECK(std::abs(f.seeked - f.ov->xToFrame(inside)) <= 1);
    CHECK_EQ(f.tv->view().leftFrame, left);

    // Dragging the rectangle 100 px to the left scrolls by 100 px worth of strip frames, zoom stays
    const double perPx = f.ov->scaleFrames() / (f.ov->width() - f.ov->leftInset() - 1);
    const QRectF r2 = f.ov->visibleRect();
    drag(*f.ov, r2.center(), r2.center() - QPointF(100, 0));
    CHECK(std::abs(f.tv->view().leftFrame - (left - 100 * perPx)) <= 1);
    CHECK_EQ(f.tv->view().pxPerFrame, 1.0);

    // Dragging the right edge to the right zooms out, the left edge (leftFrame) stays
    const double left2 = f.tv->view().leftFrame;
    const QRectF r3 = f.ov->visibleRect();
    const double wantTo = f.ov->xToFrame(r3.right() + 150); // at the scale of the press (frozen while dragging)
    drag(*f.ov, {r3.right() - 1, double(y)}, {r3.right() + 150, double(y)});
    CHECK(f.tv->view().pxPerFrame < 1.0);
    CHECK(std::abs(f.tv->view().leftFrame - left2) < 1e-6);
    CHECK(std::abs(f.ov->visibleTo() - wantTo) <= 2);

    // Dragging the left edge to the right zooms in, the right edge stays
    const QRectF r4 = f.ov->visibleRect();
    const double to4 = f.ov->visibleTo();
    const double px4 = f.tv->view().pxPerFrame;
    drag(*f.ov, {r4.left() + 1, double(y)}, {r4.left() + 60, double(y)});
    CHECK(f.tv->view().pxPerFrame > px4);
    CHECK(std::abs(f.ov->visibleTo() - to4) <= 1);

    // Left of the strip (the header part) does nothing
    const ViewState before = f.tv->view();
    const int seekedBefore = f.seeked;
    click(*f.ov, {40.0, double(y)});
    CHECK(sameView(f.tv->view(), before));
    CHECK_EQ(f.seeked, seekedBefore);
}

void testWindow(Engine& engine)
{
    QSettings().remove("timeline/overview");
    using Page = MainWindow::Page;
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    CHECK(QTest::qWaitForWindowExposed(&w));
    auto* panel = w.findChild<TimelinePanel*>();
    QAction* toggle = InputBindings::instance().action("show_timeline_overview");
    if (!CHECK(panel && toggle)) return;
    CHECK(toggle->isCheckable());
    CHECK(!toggle->isChecked()); // off by default
    CHECK(w.currentPage() == Page::Edit);
    CHECK(panel->overview()->isHidden());

    toggle->setChecked(true);
    CHECK(panel->overview()->isVisible());
    for (Page p : {Page::Deliver, Page::Color}) {
        w.showPage(p);
        CHECK(w.currentPage() == p);
        CHECK(panel->overview()->isHidden());
    }
    w.showPage(Page::Edit);
    CHECK(panel->overview()->isVisible());
    toggle->setChecked(false);
    CHECK(panel->overview()->isHidden());
    CHECK(!QSettings().contains("timeline/overview")); // a test run never writes the user's settings
}

// 2 hours at 25 fps, 2000 clips on 2 video + 2 audio tracks, 1920 px: rebuilding the strip's picture and a playhead step
void testTempo()
{
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 200000, true, false, false});
    project.addMedia(MediaInfo{"/x/b.mp4", "b.mp4", 200000, true, false, false});
    Timeline tl;
    tl.video.resize(2);
    tl.audio.resize(2);
    const int total = 2 * 3600 * 25;
    const int len = total / 500;
    int id = 1;
    for (int t = 0; t < 4; ++t)
        for (int i = 0; i < 500; ++i) {
            Clip c = clip(id++, i * len, len - (i % 3 ? 0 : 7), i % 2 ? "/x/a.mp4" : "/x/b.mp4");
            c.enabled = i % 17 != 0;
            (t < 2 ? tl.video[t] : tl.audio[t - 2]).clips << c;
        }
    for (int m = 0; m < total; m += 900) tl.markers << m;
    for (int s = 0; s < 40; ++s) tl.sections << StorySection{StoryKind(s % 6), {}, s * (total / 40), (s + 1) * (total / 40)};

    TimelineOverview ov;
    ov.setLeftInset(TimelineView::kHeaderW);
    ov.resize(1920, TimelineOverview::kHeight);
    ov.setTimeline(tl, 25, &project);
    ov.setVisibleRange(10000, 20000);
    ov.show();
    QApplication::processEvents();
    QImage img(ov.size(), QImage::Format_ARGB32_Premultiplied);
    ov.render(&img); // warm-up

    constexpr int kRuns = 20;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < kRuns; ++i) {
        ov.setTimeline(tl, 25, &project); // what a model change does: the picture is drawn again
        ov.render(&img);
    }
    const double rebuildMs = t.nsecsElapsed() / 1e6 / kRuns;
    t.restart();
    for (int i = 0; i < kRuns; ++i) {
        ov.setPlayhead(i * 4000);
        ov.render(&img);
    }
    const double playheadMs = t.nsecsElapsed() / 1e6 / kRuns;
    std::printf("overview 2 h / 2000 clips / 1920 px: rebuild %.2f ms, playhead step %.2f ms\n", rebuildMs, playheadMs);
    CHECK(rebuildMs < 25);  // a model change
    CHECK(playheadMs < 5);  // while playing
    CHECK(playheadMs < rebuildMs); // the clips are not drawn again for the playhead

    // Something is drawn: the clip color of V1 in the middle of the video half, the playhead's red
    const QImage pic = ov.grab().toImage();
    const int px = int(std::floor(ov.frameToX(ov.playhead())));
    CHECK(pic.pixelColor(px, 10) == Theme::playhead);
    CHECK(pic.pixelColor(int(ov.frameToX(len * 10 + len / 2)), 13) != Theme::timelineBg);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("timeline-overview");
    std::setlocale(LC_NUMERIC, "C");
    I18n::install("de");
    Theme::apply(app);
    testHiddenByDefault();
    testModelChangesKeepView();
    testClickAndDrag();
    testTempo();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testWindow(engine);
    return Check::result();
}
