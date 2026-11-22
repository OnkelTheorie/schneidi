// SPDX-License-Identifier: AGPL-3.0-only
// Test mouse operation of the timeline (offscreen): selection rectangle, auto-scroll at the edge, reorder tracks;
// plus selection from the playhead (Y / Ctrl+Y / Alt+Y), edit points by keyboard (V, U, , / .) and the Slip/Slide
// tools (drag anywhere on a clip = trim mode's slip/slide, toolbar buttons, , / . with them); where wheel zoom stays
// put (ZoomAnchor: the mouse by default, the visible playhead by choice); a playhead move in the ruler is framed by
// playheadMoveStarted/-Finished (press ... release) for the "after moving the playhead" rule (PlayheadMove), whose
// table is checked here too (with an engine: test_viewer_large); a sideways wheel delta (touchpad) pans sideways;
// the view follows the playing playhead only by choice (page/centre) and stops when the user scrolls or zooms.
#include "check.h"
#include "timeline_guard.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/PlayheadMove.h"
#include "ui/timeline/TimelineOptions.h"
#include "ui/timeline/TimelineView.h"
#include "ui/timeline/ZoomAnchor.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSettings>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <QWheelEvent>
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
    TimelineGuard guard_project{project};
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

// A click into a gap picks it (highlight); Delete closes it on all unlocked tracks - refused while another
// unlocked track has something inside it (lock that track); a clip click drops the gap
void testGap()
{
    Fixture f;
    QString said;
    QObject::connect(&f.tv, &TimelineView::message, [&](const QString& m) { said = m; });
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 120}, Qt::NoButton);
    CHECK(f.tv.hasGap());
    // V2's clip (50..120) reaches into V1's gap 100..150: not closed, a note why, the gap stays picked
    CHECK(f.tv.closeSelectedGap());
    CHECK(said.contains("V2"));
    CHECK_EQ(f.project.timeline().video[0].clips[1].start, 150);
    CHECK(f.tv.hasGap());
    // V2 locked: closed in one undo step, V1's later clip moves up, the locked V2 stays
    f.editor.toggleTrackLock({TrackKind::Video, 1});
    const int steps = f.project.undoStack()->index();
    CHECK(f.tv.closeSelectedGap());
    CHECK_EQ(f.project.timeline().video[0].clips[1].start, 100);
    CHECK_EQ(f.project.timeline().video[1].clips[0].start, 50);
    CHECK_EQ(f.project.undoStack()->index(), steps + 1);
    CHECK(!f.tv.hasGap());
    f.project.undoStack()->undo();
    CHECK_EQ(f.project.timeline().video[0].clips[1].start, 150);
    // No gap behind the last clip; a clip click drops a picked gap; nothing picked = Delete is not taken
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(400), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(400), 120}, Qt::NoButton);
    CHECK(!f.tv.hasGap());
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 120}, Qt::NoButton);
    CHECK(f.tv.hasGap());
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(50), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(50), 120}, Qt::NoButton);
    CHECK(!f.tv.hasGap());
    CHECK(!f.tv.closeSelectedGap());
    // Picking a gap drops the clip selection (Delete then means the gap); selecting a clip drops the gap
    f.sel.set({1});
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton, Qt::ControlModifier);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 120}, Qt::NoButton, Qt::ControlModifier);
    CHECK(f.tv.hasGap() && f.sel.isEmpty());
    f.sel.set({1});
    CHECK(!f.tv.hasGap());
    // The gap filled by an edit (a clip moved into it): no longer picked, Delete is not taken
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 120}, Qt::NoButton);
    CHECK(f.tv.hasGap());
    f.project.edit("fill", [](Timeline& tl) { tl.video[0].clips[1].start = 100; });
    CHECK(!f.tv.hasGap());
    f.project.undoStack()->undo();
    // The gap's own track locked: refused with a note (the others would move, it not)
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 120}, Qt::NoButton);
    CHECK(f.tv.hasGap());
    f.editor.toggleTrackLock({TrackKind::Video, 0});
    said.clear();
    CHECK(f.tv.hasGap()); // locking keeps the pick (the gap is still there)
    CHECK(f.tv.closeSelectedGap());
    CHECK(said.contains("V1"));
    CHECK_EQ(f.project.timeline().video[0].clips[1].start, 150);
    f.editor.toggleTrackLock({TrackKind::Video, 0});
    // A clip behind the gap whose linked sound lies on a locked track: refused (they would come apart)
    f.project.edit("link", [](Timeline& tl) {
        tl.video[0].clips[1].linkId = 7;
        Clip a = tl.audio[0].clips[0];
        a.id = 20, a.start = 150, a.linkId = 7;
        tl.audio[0].clips << a;
    });
    f.editor.toggleTrackLock({TrackKind::Audio, 0});
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 120}, Qt::NoButton);
    said.clear();
    CHECK(f.tv.closeSelectedGap());
    CHECK(said.contains("gesperrten Spur"));
    CHECK_EQ(f.project.timeline().video[0].clips[1].start, 150);
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
    TimelineGuard guard_project{project};
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
    TimelineGuard guard_project{project};
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
        f.guard_project.allowSlip({1}); // its sound is locked: slips alone
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
        TimelineGuard guard_project{project};
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

// Alt-drag at a cut rolls only that track's edge (J/L cut): picture and sound stay linked, the other track keeps its
// cut; with the select tool on the clip edge and with the trim tool on the cut
void testSplitEdit()
{
    struct Split : ToolFixture {
        Split()
        {
            Timeline tl;
            tl.video.resize(1);
            tl.audio.resize(1);
            auto at = [](int id, int start, int in, int link) {
                Clip c = clip(id, start, 100);
                c.in = in;
                c.out = in + 99;
                c.linkId = link;
                return c;
            };
            tl.video[0].clips << at(1, 0, 100, 1) << at(2, 100, 500, 2);
            tl.audio[0].clips << at(3, 0, 100, 1) << at(4, 100, 500, 2);
            project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 4, 2});
            project.undoStack()->clear();
            QApplication::processEvents();
        }
        const Clip& c(int id) const { return *TimelineOps::findClip(project.timeline(), id); }
    };
    {
        // Select tool, Alt on the sound edge: J cut (the next sound starts 10 frames early)
        Split f;
        const QString original = f.dump();
        drag(f.tv, {f.x(100) + 2, 125}, {f.x(100) + 2 - f.frames(10), 125}, Qt::AltModifier);
        CHECK_EQ(f.c(3).end(), 90);
        CHECK_EQ(f.c(4).start, 90);
        CHECK_EQ(f.c(4).in, 490);
        CHECK_EQ(f.c(1).end(), 100);
        CHECK_EQ(f.c(2).start, 100);
        CHECK_EQ(f.c(4).linkId, 2);
        CHECK(f.sel.ids() == QSet<int>{4}); // only the sound part is selected
        CHECK_EQ(f.project.undoStack()->count(), 1);
        f.project.undoStack()->undo();
        CHECK_EQ(f.dump(), original);
        // Without Alt the linked edges trim together and overwrite the neighbours (their ends go)
        drag(f.tv, {f.x(100) + 2, 125}, {f.x(100) + 2 - f.frames(10), 125});
        CHECK_EQ(f.c(4).start, 90);
        CHECK_EQ(f.c(2).start, 90);
        CHECK_EQ(f.c(2).in, 490);
        CHECK_EQ(f.c(1).end(), 90);
        CHECK_EQ(f.c(3).end(), 90);
    }
    {
        // Trim tool, Alt on the picture cut: only the picture rolls (the sound runs on: an L cut seen from the picture)
        Split f;
        f.tv.setTool(TimelineView::Tool::Trim);
        drag(f.tv, {f.x(100) + 1, 60}, {f.x(100) + 1 + f.frames(10), 60}, Qt::AltModifier);
        CHECK_EQ(f.c(1).end(), 110);
        CHECK_EQ(f.c(2).start, 110);
        CHECK_EQ(f.c(3).end(), 100);
        CHECK_EQ(f.c(4).start, 100);
        f.project.undoStack()->undo();
        // Without Alt the roll takes the sound along
        drag(f.tv, {f.x(100) + 1, 60}, {f.x(100) + 1 + f.frames(10), 60});
        CHECK_EQ(f.c(2).start, 110);
        CHECK_EQ(f.c(4).start, 110);
    }
}

// Select tool: an edge dragged into the neighbour overwrites it (shortened, or removed where all of it is covered) -
// one undo step, the linked sound alike; the neighbour's edge snaps but does not stop the drag. The trim tool keeps
// its ripple.
void testTrimOverwrite()
{
    struct Over : ToolFixture {
        Over()
        {
            Timeline tl;
            tl.video.resize(1);
            tl.audio.resize(1);
            auto at = [](int id, int start, int len, int in, int link) {
                Clip c = clip(id, start, len);
                c.in = in;
                c.out = in + len - 1;
                c.linkId = link;
                return c;
            };
            // V1: 1 [0,100) 2 [100,160) 3 [160,260); A1: their sounds 4, 5, 6
            tl.video[0].clips << at(1, 0, 100, 0, 1) << at(2, 100, 60, 500, 2) << at(3, 160, 100, 1000, 3);
            tl.audio[0].clips << at(4, 0, 100, 0, 1) << at(5, 100, 60, 500, 2) << at(6, 160, 100, 1000, 3);
            project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 6, 3});
            project.undoStack()->clear();
            QApplication::processEvents();
        }
        const Clip* c(int id) const { return TimelineOps::findClip(project.timeline(), id); }
    };
    {
        // End of 1 dragged 30 frames into 2: 2 (and its sound) starts later, its content stays in place
        Over f;
        const QString original = f.dump();
        drag(f.tv, {f.x(100) - 2, 60}, {f.x(100) - 2 + f.frames(30), 60});
        CHECK_EQ(f.c(1)->end(), 130);
        CHECK_EQ(f.c(4)->end(), 130);
        CHECK(f.c(2) && f.c(2)->start == 130 && f.c(2)->in == 530 && f.c(2)->end() == 160);
        CHECK(f.c(5) && f.c(5)->start == 130 && f.c(5)->in == 530);
        CHECK_EQ(f.c(3)->start, 160);
        CHECK_EQ(f.project.undoStack()->count(), 1);
        f.project.undoStack()->undo();
        CHECK_EQ(f.dump(), original);
    }
    {
        // Over all of 2 and into 3: 2 and its sound are gone, 3 is shortened at its start
        Over f;
        drag(f.tv, {f.x(100) - 2, 60}, {f.x(100) - 2 + f.frames(80), 60});
        CHECK(!f.c(2) && !f.c(5));
        CHECK_EQ(f.c(1)->end(), 180);
        CHECK(f.c(3)->start == 180 && f.c(3)->in == 1020);
        CHECK(f.c(6)->start == 180 && f.c(6)->in == 1020);
        CHECK_EQ(f.project.undoStack()->count(), 1);
    }
    {
        // Start of 3 dragged left over 2 into 1: the neighbours' ends go
        Over f;
        drag(f.tv, {f.x(160) + 2, 60}, {f.x(160) + 2 - f.frames(70), 60});
        CHECK(f.c(3)->start == 90 && f.c(3)->in == 930);
        CHECK(!f.c(2) && !f.c(5));
        CHECK_EQ(f.c(1)->end(), 90);
        CHECK_EQ(f.c(4)->end(), 90);
    }
    {
        // Close to the neighbour's end the edge snaps onto it (exactly covered: gone, nothing of it left)
        Over f;
        drag(f.tv, {f.x(100) - 2, 60}, {f.x(160) - 2 + 3, 60});
        CHECK(!f.c(2));
        CHECK_EQ(f.c(1)->end(), 160);
        CHECK_EQ(f.c(3)->start, 160);
    }
    {
        // The trim tool still ripples: 2 and 3 move right, nothing is overwritten
        Over f;
        f.tv.setTool(TimelineView::Tool::Trim);
        drag(f.tv, {f.x(100) - 6, 60}, {f.x(100) - 6 + f.frames(30), 60});
        CHECK_EQ(f.c(1)->end(), 130);
        CHECK(f.c(2) && f.c(2)->start == 130 && f.c(2)->in == 500);
        CHECK_EQ(f.c(3)->start, 190);
    }
}

// Story sections as a band in the ruler (color of the kind), notes as marks below it; a section's edge drags (one
// undo step, not past the neighbor) instead of moving the playhead
void testStoryBand()
{
    Fixture f;
    f.editor.addSection({StoryKind::Hook, "Kill", 0, 100});
    f.editor.addSection({StoryKind::Build, {}, 140, 200});
    const int note = f.editor.addNote(120, "schneller");
    QApplication::processEvents();
    const QImage shot = f.tv.grab().toImage();
    const QColor inHook = shot.pixelColor(f.x(50), 8), outside = shot.pixelColor(f.x(120), 8);
    CHECK(inHook != outside);
    CHECK(inHook.red() > inHook.blue()); // the hook's orange tint (Theme::primary)
    CHECK(shot.pixelColor(f.x(120), TimelineView::kRulerH - 10) != shot.pixelColor(f.x(125) + 4, TimelineView::kRulerH - 10));
    // Drag the hook's right edge from 100 to 130 frames
    QUndoStack* undo = f.project.undoStack();
    const int steps = undo->index();
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(100), 8}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.x(115), 8}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.x(130), 8}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 8}, Qt::NoButton);
    const QVector<StorySection>& s = f.project.timeline().sections;
    if (CHECK_EQ(s.size(), 2)) CHECK(std::abs(s[0].to - 130) <= 1 && s[0].from == 0);
    CHECK_EQ(undo->index(), steps + 1);
    // A second drag is its own undo step; it stops before the next section (from 140)
    const int edge = f.project.timeline().sections[0].to;
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(edge), 8}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.x(135), 8}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.x(170), 8}, Qt::LeftButton); // would overlap: refused, 135 stays
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(170), 8}, Qt::NoButton);
    CHECK(std::abs(f.project.timeline().sections[0].to - 135) <= 1);
    CHECK_EQ(undo->index(), steps + 2);
    undo->undo();
    CHECK(std::abs(f.project.timeline().sections[0].to - 130) <= 1);
    undo->undo();
    CHECK_EQ(f.project.timeline().sections[0].to, 100);
    CHECK(f.editor.setNoteDone(note, true)); // the note is still there
    // Touching sections: the one the mouse is over is grabbed (the later one's left edge could never be)
    f.editor.removeSectionAt(150);
    f.editor.addSection({StoryKind::Build, {}, 100, 200});
    drag(f.tv, {f.x(100) + 2, 8}, {f.x(110) + 2, 8});
    const QVector<StorySection>& t = f.project.timeline().sections;
    if (CHECK_EQ(t.size(), 2)) {
        CHECK_EQ(t[0].to, 100); // the hook stays
        CHECK(std::abs(t[1].from - 110) <= 1);
    }
}

// Alt+wheel (the default zoom binding) at x, one notch in (or out with in = false)
static void wheelZoom(TimelineView& tv, double x, bool in = true)
{
    const QPointF at(x, 60);
    QWheelEvent e(at, tv.mapToGlobal(at), QPoint(), QPoint(0, in ? 120 : -120), Qt::NoButton, Qt::AltModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(&tv, &e);
}

void testWheelZoomAnchor()
{
    Fixture f;
    const auto frameAt = [&](double x) {
        return f.tv.view().leftFrame + (x - TimelineView::kHeaderW) / f.tv.view().pxPerFrame;
    };
    const auto xOf = [&](double frame) {
        return TimelineView::kHeaderW + (frame - f.tv.view().leftFrame) * f.tv.view().pxPerFrame;
    };
    const double mouseX = TimelineView::kHeaderW + 400;
    CHECK(ZoomAnchor::mode() == ZoomAnchor::Mode::Mouse); // nothing stored: the mouse, as before the setting

    // Default: the frame under the mouse stays under it, the playhead moves on screen
    f.tv.setZoomAndScroll(2.0, 20, 0);
    f.tv.setPlayhead(150);
    const double underMouse = frameAt(mouseX), playheadX = xOf(150);
    wheelZoom(f.tv, mouseX);
    CHECK(f.tv.view().pxPerFrame > 2.0 * 1.1);
    CHECK(std::abs(frameAt(mouseX) - underMouse) < 1e-6);
    CHECK(std::abs(xOf(150) - playheadX) > 10);

    // Playhead: the red line keeps its x, the mouse's frame moves away
    ZoomAnchor::setMode(ZoomAnchor::Mode::Playhead);
    CHECK(ZoomAnchor::mode() == ZoomAnchor::Mode::Playhead);
    f.tv.setZoomAndScroll(2.0, 20, 0);
    wheelZoom(f.tv, mouseX);
    CHECK(f.tv.view().pxPerFrame > 2.0 * 1.1);
    CHECK(std::abs(xOf(150) - playheadX) < 1e-6);
    CHECK(std::abs(frameAt(mouseX) - underMouse) > 1);
    // Zooming out keeps it too (while the left edge does not reach frame 0)
    f.tv.setZoomAndScroll(2.0, 100, 0);
    const double playheadX2 = xOf(150);
    wheelZoom(f.tv, mouseX, false);
    CHECK(f.tv.view().pxPerFrame < 2.0 / 1.1);
    CHECK(std::abs(xOf(150) - playheadX2) < 1e-6);

    // Playhead scrolled out of view, left of the visible part: back to the mouse
    f.tv.setZoomAndScroll(2.0, 200, 0);
    CHECK(xOf(150) < TimelineView::kHeaderW);
    const double underMouse2 = frameAt(mouseX);
    wheelZoom(f.tv, mouseX);
    CHECK(f.tv.view().pxPerFrame > 2.0 * 1.1);
    CHECK(std::abs(frameAt(mouseX) - underMouse2) < 1e-6);
    // ... and right of it
    f.tv.setZoomAndScroll(2.0, 0, 0);
    f.tv.setPlayhead(2000);
    CHECK(xOf(2000) > f.tv.width());
    const double underMouse3 = frameAt(mouseX);
    wheelZoom(f.tv, mouseX);
    CHECK(f.tv.view().pxPerFrame > 2.0 * 1.1);
    CHECK(std::abs(frameAt(mouseX) - underMouse3) < 1e-6);

    ZoomAnchor::setMode(ZoomAnchor::Mode::Mouse);
    CHECK(ZoomAnchor::mode() == ZoomAnchor::Mode::Mouse);
}

// A wheel event with angle delta (dx, dy) and modifiers
static void wheel(TimelineView& tv, int dx, int dy, Qt::KeyboardModifiers mods = {})
{
    const QPointF at(TimelineView::kHeaderW + 300, 60);
    QWheelEvent e(at, tv.mapToGlobal(at), QPoint(), QPoint(dx, dy), Qt::NoButton, mods, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&tv, &e);
}

// A sideways touchpad swipe (or tilt wheel) pans the timeline sideways, not up/down (bug: x was fed into the
// upright wheel's binding = vertical scroll). Alt/Shift keep the binding (Qt turns an upright wheel sideways there)
void testWheelSideways()
{
    Fixture f;
    f.tv.resize(1200, 120); // short: room to scroll vertically
    f.tv.setZoomAndScroll(2.0, 200, 0);
    QSettings().remove("timeline");
    // Swipe: content follows to the right (left frame smaller), the vertical scroll stays
    wheel(f.tv, 120, 0);
    CHECK(f.tv.view().leftFrame < 200);
    CHECK_EQ(f.tv.view().scrollY, 0);
    const double left = f.tv.view().leftFrame;
    wheel(f.tv, -240, 0);
    CHECK(f.tv.view().leftFrame > left);
    CHECK_EQ(f.tv.view().scrollY, 0);
    // Mostly sideways with a little up/down (touchpad diagonal): still sideways
    const double left2 = f.tv.view().leftFrame;
    wheel(f.tv, -120, 20);
    CHECK(f.tv.view().leftFrame > left2);
    CHECK_EQ(f.tv.view().scrollY, 0);
    // An upright wheel without modifiers: vertical scroll (the default binding), left frame stays
    const double left3 = f.tv.view().leftFrame;
    wheel(f.tv, 0, -120);
    CHECK_EQ(f.tv.view().leftFrame, left3);
    CHECK(f.tv.view().scrollY > 0);
    // Alt with an x delta (Qt's swapped axes): still the Alt binding = zoom
    const double px = f.tv.view().pxPerFrame;
    wheel(f.tv, 120, 0, Qt::AltModifier);
    CHECK(f.tv.view().pxPerFrame > px * 1.1);
    // Shift with an x delta: the Shift binding = track height
    const int h = f.tv.view().videoTrackHeight;
    wheel(f.tv, 120, 0, Qt::ShiftModifier);
    CHECK(f.tv.view().videoTrackHeight > h);
    CHECK(TimelineView::wheelPansSideways({120, 0}, Qt::ControlModifier));
    CHECK(!TimelineView::wheelPansSideways({120, 0}, Qt::AltModifier));
    CHECK(!TimelineView::wheelPansSideways({60, 120}, {}));
}

// Following the playhead while playing (TimelineOptions::follow): off by default (core rule); page turns and centre
// only while it plays and only for the playback; scrolling or zooming sideways ends it until the next start
void testFollowPlayback()
{
    using F = TimelineOptions::Follow;
    QSettings().remove("timeline");
    CHECK(TimelineOptions::follow() == F::Off);
    Fixture f;
    f.tv.setZoomAndScroll(2.0, 0, 0);
    const double vis = (f.tv.width() - TimelineView::kHeaderW) / 2.0; // 510 frames
    // Off: the playhead runs out of view, the view stays
    f.tv.setPlaying(1.0);
    for (int fr = 0; fr <= 1200; fr += 10) f.tv.setPlayhead(fr);
    CHECK_EQ(f.tv.view().leftFrame, 0.0);
    f.tv.setPlaying(0);

    // Page: nothing until it leaves the view, then the playhead stands at the left edge
    TimelineOptions::setFollow(F::Page);
    CHECK(TimelineOptions::follow() == F::Page);
    f.tv.setPlayhead(0);
    f.tv.setPlaying(1.0);
    for (int fr = 0; fr < int(vis); fr += 10) f.tv.setPlayhead(fr);
    CHECK_EQ(f.tv.view().leftFrame, 0.0);
    f.tv.setPlayhead(int(vis) + 2);
    CHECK_EQ(f.tv.view().leftFrame, double(int(vis) + 2));
    // A model change while it plays moves nothing (core rule)
    const double left = f.tv.view().leftFrame;
    f.editor.trimClip(2, TimelineOps::Edge::End, -20, true);
    QApplication::processEvents();
    CHECK_EQ(f.tv.view().leftFrame, left);
    // Not while dragging in the ruler (scrubbing): the seeks of the drag do not turn the page
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(left + 50), TimelineView::kRulerH / 2}, Qt::LeftButton);
    f.tv.setPlayhead(int(left + 2 * vis));
    CHECK_EQ(f.tv.view().leftFrame, left);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(left + 50), TimelineView::kRulerH / 2}, Qt::NoButton);
    // The user scrolls sideways while it plays: following stops until the next start
    f.tv.setLeftFrame(100);
    f.tv.setPlayhead(2000);
    CHECK_EQ(f.tv.view().leftFrame, 100.0);
    f.tv.setPlaying(0);
    f.tv.setPlayhead(100);
    f.tv.setPlaying(1.0);
    f.tv.setPlayhead(2000);
    CHECK_EQ(f.tv.view().leftFrame, 2000.0);
    // Zooming (wheel or keys) ends it too
    wheelZoom(f.tv, TimelineView::kHeaderW + 100);
    const double zoomedLeft = f.tv.view().leftFrame;
    f.tv.setPlayhead(9000);
    CHECK_EQ(f.tv.view().leftFrame, zoomedLeft);
    f.tv.setPlaying(0);
    // Reverse (J): leaving at the left edge puts the playhead at the right edge
    f.tv.setZoomAndScroll(2.0, 1000, 0);
    f.tv.setPlayhead(1100);
    f.tv.setPlaying(-1.0);
    f.tv.setPlayhead(990);
    CHECK(std::abs(f.tv.view().leftFrame - (990 - vis + 1)) < 1e-6);
    f.tv.setPlaying(0);

    // Centre: from the middle on the view runs under the playhead; the start stays at the left edge
    TimelineOptions::setFollow(F::Center);
    f.tv.setZoomAndScroll(2.0, 0, 0);
    f.tv.setPlayhead(0);
    f.tv.setPlaying(1.0);
    f.tv.setPlayhead(100);
    CHECK_EQ(f.tv.view().leftFrame, 0.0);
    f.tv.setPlayhead(1000);
    CHECK(std::abs(f.tv.view().leftFrame - (1000 - vis / 2)) < 1e-6);
    f.tv.setPlayhead(1010);
    CHECK(std::abs(f.tv.view().leftFrame - (1010 - vis / 2)) < 1e-6);
    // Paused: no following (a seek while paused moves nothing)
    f.tv.setPlaying(0);
    const double paused = f.tv.view().leftFrame;
    f.tv.setPlayhead(5000);
    CHECK_EQ(f.tv.view().leftFrame, paused);
    // The choice is read at the start: switched off while paused, the next start does not follow
    TimelineOptions::setFollow(F::Off);
    CHECK(!QSettings().contains("timeline/followPlayback"));
    f.tv.setPlaying(1.0);
    f.tv.setPlayhead(9000);
    CHECK_EQ(f.tv.view().leftFrame, paused);
    f.tv.setPlaying(0);
}

void testPlayheadMoveRule()
{
    using PlayheadMove::Mode, PlayheadMove::Source, PlayheadMove::playsAfter;
    for (Source s : {Source::Timeline, Source::ViewerBar, Source::Key})
        for (bool was : {false, true}) {
            CHECK_EQ(playsAfter(Mode::Continue, s, was), was);
            CHECK(!playsAfter(Mode::Stop, s, was));
            CHECK(playsAfter(Mode::Play, s, was));
        }
    // Standard = the behaviour before the setting: the timeline plays on, the viewer's bar and keys stop
    CHECK(playsAfter(Mode::Standard, Source::Timeline, true));
    CHECK(!playsAfter(Mode::Standard, Source::Timeline, false));
    CHECK(!playsAfter(Mode::Standard, Source::ViewerBar, true));
    CHECK(!playsAfter(Mode::Standard, Source::Key, true));
    // Stored: nothing = Standard, each choice comes back; Standard removes the key again
    QSettings().remove("timeline/afterPlayheadMove");
    CHECK(PlayheadMove::mode() == Mode::Standard);
    for (Mode m : {Mode::Continue, Mode::Stop, Mode::Play, Mode::Standard}) {
        PlayheadMove::setMode(m);
        CHECK(PlayheadMove::mode() == m);
    }
    CHECK(!QSettings().contains("timeline/afterPlayheadMove"));
}

void testPlayheadMoveSignals()
{
    Fixture f;
    QStringList log;
    QObject::connect(&f.tv, &TimelineView::playheadMoveStarted, [&] { log << "start"; });
    QObject::connect(&f.tv, &TimelineView::seekRequested, [&](int frame) {
        log << QString("seek %1").arg(frame);
        f.tv.setPlayhead(frame);
    });
    QObject::connect(&f.tv, &TimelineView::playheadMoveFinished, [&] { log << "finish"; });
    f.tv.setZoomAndScroll(2.0, 0, 0);
    const ViewState before = f.tv.view();
    const int y = TimelineView::kRulerH / 2;

    // A drag in the ruler: started before the first seek, finished once on release (not during the drag)
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(20), y}, Qt::LeftButton);
    CHECK(log == QStringList({"start", "seek 20"}));
    mouse(f.tv, QEvent::MouseMove, {f.x(40), y}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.x(60), y}, Qt::LeftButton);
    CHECK(log == QStringList({"start", "seek 20", "seek 40", "seek 60"}));
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(60), y}, Qt::NoButton);
    CHECK(log == QStringList({"start", "seek 20", "seek 40", "seek 60", "finish"}));
    CHECK_EQ(f.tv.playhead(), 60);
    // A click: the same three steps
    log.clear();
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(90), y}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(90), y}, Qt::NoButton);
    CHECK(log == QStringList({"start", "seek 90", "finish"}));
    // With an In/Out range: a click on its grip (no drag) still moves the playhead, framed the same way
    f.editor.setMarks(100, 199);
    QApplication::processEvents();
    log.clear();
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(150), TimelineView::kRulerH - 2}, Qt::LeftButton);
    CHECK(log.isEmpty()); // the grip took the press: the playhead moves on release (a click, no drag)
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(150), TimelineView::kRulerH - 2}, Qt::NoButton);
    CHECK(log == QStringList({"start", "seek 150", "finish"}));
    log.clear();
    // A click into a track (not the ruler) is no playhead move
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(30), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(30), 120}, Qt::NoButton);
    CHECK(log.isEmpty());
    // Core rule: none of it changed zoom or scroll
    CHECK_EQ(f.tv.view().pxPerFrame, before.pxPerFrame);
    CHECK_EQ(f.tv.view().leftFrame, before.leftFrame);
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("timeline_mouse");
    I18n::install("de");
    Theme::apply(app);
    testRubber();
    testGap();
    testAutoScroll();
    testTrackMove();
    testSelectFromPlayhead();
    testEditPoints();
    testSlipSlideTools();
    testSplitEdit();
    testTrimOverwrite();
    testStoryBand();
    testWheelZoomAnchor();
    testWheelSideways();
    testFollowPlayback();
    testPlayheadMoveRule();
    testPlayheadMoveSignals();
    return Check::result();
}
