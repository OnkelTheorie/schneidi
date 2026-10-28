// SPDX-License-Identifier: AGPL-3.0-only
// Test mouse operation of the timeline (offscreen): selection rectangle, auto-scroll at the edge, reorder tracks;
// plus selection from the playhead (Y / Ctrl+Y / Alt+Y), edit points by keyboard (V, U, , / .) and the Slip/Slide
// tools (drag anywhere on a clip = trim mode's slip/slide, toolbar buttons, , / . with them).
#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTest>
#include <QToolButton>
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

// Slip/Slide tools: V1 a [0,100) in 100 | b [100,200) in 500 | c [200,300) in 1000, A1 a' linked to a
struct ToolFixture {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    TimelineView tv{&editor};
    QVector<TrimFrames::View> views;
    int ended = 0;

    ToolFixture()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, true, false});
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        auto at = [](int id, int start, int in, int link = 0) {
            Clip c = clip(id, start, 100);
            c.in = in;
            c.out = in + 99;
            c.linkId = link;
            return c;
        };
        tl.video[0].clips << at(1, 0, 100, 1) << at(2, 100, 500) << at(3, 200, 1000);
        tl.audio[0].clips << at(4, 0, 100, 1);
        project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 5, 1});
        tv.resize(1400, 400);
        tv.show();
        QApplication::processEvents();
        QObject::connect(&tv, &TimelineView::trimFramesChanged, [this](const TrimFrames::View& v) { views << v; });
        QObject::connect(&tv, &TimelineView::trimFramesEnded, [this] { ++ended; });
    }
    // V1 from y 30 (64 high, title bar = the top 16 px of the clip), A1 from y 100
    int x(double frame) const { return int(std::lround(TimelineView::kHeaderW + (frame - tv.view().leftFrame) * tv.view().pxPerFrame)); }
    int frames(int n) const { return int(std::lround(n * tv.view().pxPerFrame)); }
    QString dump() const { return Check::dump(project.timeline()); }
};

void testSlipSlideTools()
{
    using TimelineOps::TrimKind;
    const QString original = ToolFixture().dump();

    // Slip: what trim mode does on the clip body, the slip tool does anywhere on the clip - here on the title bar at
    // the left edge, where trim mode would slide resp. ripple
    QString slipped;
    {
        ToolFixture f;
        f.tv.setTool(TimelineView::Tool::Trim);
        drag(f.tv, {f.x(50), 75}, {f.x(50) + f.frames(10), 75});
        slipped = f.dump();
        CHECK(slipped != original);
    }
    {
        ToolFixture f;
        QVector<TimelineView::Tool> changed;
        QObject::connect(&f.tv, &TimelineView::toolChanged, [&](TimelineView::Tool t) { changed << t; });
        f.tv.setTool(TimelineView::Tool::Slip);
        CHECK(changed == QVector<TimelineView::Tool>{TimelineView::Tool::Slip});
        CHECK(f.tv.trimTool());
        mouse(f.tv, QEvent::MouseMove, {f.x(1), 33}, Qt::NoButton); // over the clip: the slip cursor
        CHECK(f.tv.cursor().shape() == Qt::BitmapCursor);
        mouse(f.tv, QEvent::MouseMove, {f.x(350), 33}, Qt::NoButton);
        CHECK(f.tv.cursor().shape() == Qt::ArrowCursor);
        const int undo0 = f.project.undoStack()->count();
        const QPoint grab(f.x(1), 33);
        mouse(f.tv, QEvent::MouseButtonPress, grab, Qt::LeftButton);
        mouse(f.tv, QEvent::MouseMove, grab + QPoint(f.frames(5), 0), Qt::LeftButton);
        mouse(f.tv, QEvent::MouseMove, grab + QPoint(f.frames(10), 0), Qt::LeftButton);
        // four-up trim view like trim mode's slip, ended on release
        if (CHECK(!f.views.isEmpty())) {
            CHECK(f.views.last().kind == TrimKind::Slip);
            CHECK_EQ(f.views.last().small.size(), 2);
            CHECK_EQ(f.views.last().main.size(), 2);
        }
        mouse(f.tv, QEvent::MouseButtonRelease, grab + QPoint(f.frames(10), 0), Qt::NoButton);
        CHECK_EQ(f.ended, 1);
        CHECK_EQ(f.dump(), slipped);
        const Clip* a = TimelineOps::findClip(f.project.timeline(), 1);
        const Clip* partner = TimelineOps::findClip(f.project.timeline(), 4);
        CHECK(a && a->start == 0 && a->length() == 100 && a->in != 100); // content moved, place and length stay
        CHECK(partner && a && partner->in == a->in);                      // linked audio slips along
        CHECK_EQ(f.project.undoStack()->count(), undo0 + 1);
        f.project.undoStack()->undo();
        CHECK_EQ(f.dump(), original);
    }

    // Slide: on b's body next to the cut with c (trim mode: roll there) like trim mode on the title bar, snapping
    // included (the playhead at 112: dragged by 11 frames, b lands on 112)
    QString slid;
    {
        ToolFixture f;
        f.tv.setPlayhead(112);
        f.tv.setTool(TimelineView::Tool::Trim);
        drag(f.tv, {f.x(150), 33}, {f.x(150) + f.frames(11), 33});
        slid = f.dump();
        CHECK_EQ(TimelineOps::findClip(f.project.timeline(), 2)->start, 112);
    }
    {
        ToolFixture f;
        f.tv.setPlayhead(112);
        f.tv.setTool(TimelineView::Tool::Slide);
        const QPoint grab(f.x(200) - 1, 75);
        mouse(f.tv, QEvent::MouseButtonPress, grab, Qt::LeftButton);
        mouse(f.tv, QEvent::MouseMove, grab + QPoint(f.frames(11), 0), Qt::LeftButton);
        if (CHECK(!f.views.isEmpty())) CHECK(f.views.last().kind == TrimKind::Slide);
        mouse(f.tv, QEvent::MouseButtonRelease, grab + QPoint(f.frames(11), 0), Qt::NoButton);
        CHECK_EQ(f.dump(), slid);
        const Timeline& tl = f.project.timeline();
        CHECK(TimelineOps::findClip(tl, 1)->end() == 112 && TimelineOps::findClip(tl, 3)->start == 212);
        CHECK_EQ(TimelineOps::findClip(tl, 2)->in, 500); // slide keeps the content
        CHECK(f.sel.contains(2));
        CHECK_EQ(f.ended, 1);
    }

    // Locked tracks: nothing on V1 is grabbed; a locked partner (A1) stays where it is
    {
        ToolFixture f;
        f.tv.setTool(TimelineView::Tool::Slip);
        f.editor.toggleTrackLock({TrackKind::Video, 0});
        const int undo0 = f.project.undoStack()->count();
        drag(f.tv, {f.x(50), 75}, {f.x(50) + f.frames(10), 75});
        CHECK(f.views.isEmpty());
        CHECK_EQ(f.project.undoStack()->count(), undo0);
        f.editor.toggleTrackLock({TrackKind::Video, 0});
        f.editor.toggleTrackLock({TrackKind::Audio, 0});
        drag(f.tv, {f.x(50), 75}, {f.x(50) + f.frames(10), 75});
        CHECK(TimelineOps::findClip(f.project.timeline(), 1)->in != 100);
        CHECK_EQ(TimelineOps::findClip(f.project.timeline(), 4)->in, 100);
    }

    // Another tool leaves it: the select tool moves the clip again
    {
        ToolFixture f;
        f.tv.setTool(TimelineView::Tool::Slide);
        f.tv.setTool(TimelineView::Tool::Select);
        CHECK(!f.tv.trimTool());
        drag(f.tv, {f.x(350), 75}, {f.x(350), 75}); // click on empty space: deselect
        drag(f.tv, {f.x(250), 75}, {f.x(250) + f.frames(20), 75});
        CHECK_EQ(TimelineOps::findClip(f.project.timeline(), 3)->start, 220);
        CHECK(f.views.isEmpty());
    }

    // Toolbar: a button per tool, it follows the tool
    {
        Project project;
        Selection sel;
        Editor editor(&project, &sel);
        TimelinePanel panel(&editor);
        auto* slip = panel.findChild<QToolButton*>("tool_slip");
        auto* slide = panel.findChild<QToolButton*>("tool_slide");
        if (CHECK(slip && slide)) {
            slip->click();
            CHECK(panel.view()->tool() == TimelineView::Tool::Slip);
            CHECK(slip->isChecked());
            panel.view()->setTool(TimelineView::Tool::Slide);
            CHECK(slide->isChecked() && !slip->isChecked());
            panel.view()->setTool(TimelineView::Tool::Blade);
            CHECK(!slide->isChecked() && panel.findChild<QToolButton*>("tool_blade")->isChecked());
        }
    }

    // , and . with the slide tool: the selection slides (one undo step); slip tool: slips
    {
        ToolFixture f;
        f.sel.set({2});
        const int undo0 = f.project.undoStack()->count();
        f.editor.slideSelection(5);
        const Timeline& tl = f.project.timeline();
        CHECK(TimelineOps::findClip(tl, 1)->end() == 105 && TimelineOps::findClip(tl, 2)->start == 105);
        CHECK(TimelineOps::findClip(tl, 3)->start == 205 && TimelineOps::findClip(tl, 3)->in == 1005);
        CHECK_EQ(f.project.undoStack()->count(), undo0 + 1);
        f.editor.slipSelection(-3);
        CHECK(TimelineOps::findClip(tl, 2)->start == 105 && TimelineOps::findClip(tl, 2)->in != 500);
    }
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
    testSlipSlideTools();
    return Check::result();
}
