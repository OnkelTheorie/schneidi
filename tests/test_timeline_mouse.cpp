// Test mouse operation of the timeline (offscreen): selection rectangle, auto-scroll at the edge, reorder tracks;
// plus selection from the playhead (Y / Ctrl+Y / Alt+Y) and edit points by keyboard (V, U, , / .).
#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTest>
#include <QUndoStack>
#include <cmath>

namespace {

Clip clip(int id, int start, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = "/x/a.mp4";
    c.start = start;
    c.in = 0;
    c.out = length - 1;
    return c;
}

void mouse(QWidget& w, QEvent::Type type, QPoint pt, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {})
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
    QApplication::sendEvent(&w, &e);
}

void drag(QWidget& w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = {})
{
    mouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseMove, to, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseButtonRelease, to, Qt::NoButton, mods);
}

struct Fixture {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    TimelineView tv{&editor};

    Fixture()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, false, false});
        Timeline tl;
        tl.video.resize(2);
        tl.audio.resize(1);
        tl.video[0].clips << clip(1, 0, 100) << clip(2, 150, 100); // V1
        tl.video[1].clips << clip(3, 50, 70);                       // V2
        tl.audio[0].clips << clip(4, 0, 100);                       // A1
        project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 5, 0});
        tv.resize(1200, 500);
        tv.show();
        QApplication::processEvents();
    }
    // Rows: V2 from y 30, V1 from 94 (64 high each), separator 6, A1 from 164
    int x(double frame) const { return int(std::lround(TimelineView::kHeaderW + (frame - tv.view().leftFrame) * tv.view().pxPerFrame)); }
};

void testRubber()
{
    Fixture f;
    // Rectangle from V2 (frame 130, empty) to V1 (frame 95): touches 3 and 1 - they do not have to be fully inside
    drag(f.tv, {f.x(130), 40}, {f.x(95), 120});
    CHECK(f.sel.ids() == (QSet<int>{1, 3}));
    CHECK(f.project.timeline().video[1].clips[0].start == 50); // nothing moved

    // Click on an empty spot clears the selection
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 120}, Qt::NoButton);
    CHECK(f.sel.isEmpty());

    // Rectangle without hit -> nothing; dragged up/left works the same
    drag(f.tv, {f.x(140), 120}, {f.x(110), 100});
    CHECK(f.sel.isEmpty());
    drag(f.tv, {f.x(130), 200}, {f.x(90), 170});
    CHECK(f.sel.ids() == QSet<int>{4});

    // Ctrl or Shift: add to the existing selection
    drag(f.tv, {f.x(140), 120}, {f.x(160), 110}, Qt::ControlModifier);
    CHECK(f.sel.ids() == (QSet<int>{2, 4}));
    drag(f.tv, {f.x(125), 40}, {f.x(115), 50}, Qt::ShiftModifier);
    CHECK(f.sel.ids() == (QSet<int>{2, 3, 4}));

    // Locked track: nothing on it is selected
    f.editor.toggleTrackLock({TrackKind::Video, 0});
    drag(f.tv, {f.x(125), 40}, {f.x(95), 120});
    CHECK(f.sel.ids() == QSet<int>{3});
}

void testAutoScroll()
{
    Fixture f;
    CHECK_EQ(f.tv.view().leftFrame, 0.0);
    // Rectangle from frame 120 in V1, mouse beyond the right edge: timeline scrolls along, rectangle start stays
    // at frame 120 (clip 1 ends at 100 -> not inside)
    const QPoint from(f.x(120), 120);
    mouse(f.tv, QEvent::MouseButtonPress, from, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, from + QPoint(20, 0), Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.tv.width() + 10, 120}, Qt::LeftButton);
    QTest::qWait(300);
    const double scrolled = f.tv.view().leftFrame;
    CHECK(scrolled > 20);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.tv.width() + 10, 120}, Qt::NoButton);
    CHECK(f.sel.ids() == QSet<int>{2});
    QTest::qWait(100);
    CHECK_EQ(f.tv.view().leftFrame, scrolled); // no further scrolling after release

    // back to the left edge: scrolls to frame 0 and no further
    CHECK(scrolled < 120); // frame 130 (empty in V1) still visible
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.x(120), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {TimelineView::kHeaderW - 40, 120}, Qt::LeftButton);
    QTest::qWait(1500);
    CHECK_EQ(f.tv.view().leftFrame, 0.0);
    mouse(f.tv, QEvent::MouseButtonRelease, {TimelineView::kHeaderW - 40, 120}, Qt::NoButton);
    CHECK(f.sel.ids() == QSet<int>{1}); // rectangle now spans from frame 0 to 130

    // Move clip beyond the right edge: lands further right by the scrolled amount
    const QPoint grab(f.x(200), 120);
    mouse(f.tv, QEvent::MouseButtonPress, grab, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, grab + QPoint(10, 0), Qt::LeftButton);
    const QPoint edge(f.tv.width() - 5, 120);
    mouse(f.tv, QEvent::MouseMove, edge, Qt::LeftButton);
    QTest::qWait(300);
    mouse(f.tv, QEvent::MouseButtonRelease, edge, Qt::NoButton);
    const double left = f.tv.view().leftFrame;
    CHECK(left > 0);
    const Clip* c = TimelineOps::findClip(f.project.timeline(), 2);
    const int expect = 150 + int(std::lround((edge.x() - grab.x()) / f.tv.view().pxPerFrame + left));
    CHECK(c && std::abs(c->start - expect) <= 1);
}

void testTrackMove()
{
    Fixture f;
    // Drag track header V2 onto V1: V2 becomes V1 (clip 3 on track 0)
    drag(f.tv, {100, 50}, {100, 130});
    const Timeline& tl = f.project.timeline();
    CHECK(tl.video[0].clips.size() == 1 && tl.video[0].clips[0].id == 3);
    CHECK_EQ(tl.video[1].clips.size(), 2);
    // far beyond the audio tracks: next video track (V1) is the target, without change
    drag(f.tv, {100, 120}, {100, 400});
    CHECK(f.project.timeline().video[0].clips[0].id == 3);
}

void testSelectFromPlayhead()
{
    Fixture f;
    // without selection: target tracks V1 + A1; clip under the playhead belongs to it
    f.editor.selectFromPlayhead(60, true, false);
    CHECK(f.sel.ids() == (QSet<int>{1, 2, 4}));
    f.editor.selectFromPlayhead(60, false, false);
    CHECK(f.sel.ids() == (QSet<int>{1, 4}));
    // all tracks
    f.editor.selectFromPlayhead(110, true, true);
    CHECK(f.sel.ids() == (QSet<int>{2, 3}));
    f.editor.selectFromPlayhead(110, false, true);
    CHECK(f.sel.ids() == (QSet<int>{1, 3, 4}));
    // with selection: its tracks (here V2)
    f.sel.set({3});
    f.editor.selectFromPlayhead(0, true, false);
    CHECK(f.sel.ids() == QSet<int>{3});
    // locked track is left out
    f.editor.toggleTrackLock({TrackKind::Audio, 0});
    f.editor.selectFromPlayhead(0, true, true);
    CHECK(f.sel.ids() == (QSet<int>{1, 2, 3}));
}

void testEditPoints()
{
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, false, false});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip b = clip(2, 100, 100);
    b.in = 50;
    b.out = 149;
    tl.video[0].clips << clip(1, 0, 100) << b;
    tl.audio[0].clips << clip(3, 0, 80);
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 5, 0});
    auto clipAt = [&](int id) { return *TimelineOps::findClip(project.timeline(), id); };

    CHECK(!editor.nudgeEditPoint(1, false)); // nothing chosen -> normal nudge
    // V: next cut on the target tracks (V1 + A1), at the cut of two clips both sides
    editor.selectNearestEditPoint(90);
    CHECK(sel.editPoint() == (EditPoint{1, 2, 0}));
    CHECK(sel.isEmpty());
    // . = roll by 1 frame, one undo step
    const int undo0 = project.undoStack()->count();
    CHECK(editor.nudgeEditPoint(1, false));
    CHECK_EQ(clipAt(1).end(), 101);
    CHECK(clipAt(2).start == 101 && clipAt(2).in == 51 && clipAt(2).end() == 200);
    CHECK_EQ(project.undoStack()->count(), undo0 + 1);
    CHECK(sel.editPoint() == (EditPoint{1, 2, 0})); // stays selected
    // U: left side only (end of clip 1), without ripple
    editor.cycleEditPointSide();
    CHECK_EQ(sel.editPoint().side, -1);
    editor.nudgeEditPoint(-2, false);
    CHECK(clipAt(1).end() == 99 && clipAt(2).start == 101);
    // U: right side (start of clip 2), then both again
    editor.cycleEditPointSide();
    CHECK_EQ(sel.editPoint().side, 1);
    editor.nudgeEditPoint(-1, false);
    CHECK_EQ(clipAt(2).start, 100);
    editor.cycleEditPointSide();
    CHECK_EQ(sel.editPoint().side, 0);
    // Edge at a gap: one side only, U changes nothing; trim mode = ripple
    editor.selectNearestEditPoint(260);
    CHECK(sel.editPoint() == (EditPoint{2, 0, -1}));
    editor.cycleEditPointSide();
    CHECK_EQ(sel.editPoint().side, -1);
    editor.nudgeEditPoint(5, true);
    CHECK_EQ(clipAt(2).end(), 205);
    // End of A1 is nearer to 85 than the cut at 99/100
    editor.selectNearestEditPoint(85);
    CHECK(sel.editPoint() == (EditPoint{3, 0, -1}));
    // Clip selection clears the edit point
    sel.set({1});
    CHECK(sel.editPoint().isNull());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("timeline_mouse");
    I18n::install("de");
    Theme::apply(app);
    testRubber();
    testAutoScroll();
    testTrackMove();
    testSelectFromPlayhead();
    testEditPoints();
    return Check::result();
}
