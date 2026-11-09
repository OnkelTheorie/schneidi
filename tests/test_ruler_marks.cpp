// SPDX-License-Identifier: AGPL-3.0-only
// The In/Out range in the timeline ruler (offscreen): handles at both edges drag the points (snapping onto clip
// edges and markers, Shift drags freely, In never past Out), the range's lower half moves the whole range (its
// length stays), Alt-drag draws a new range, a click still moves the playhead, one undo step per drag, the hover
// cursor and tooltip, the right-click actions. RULER_SHOT_DIR=<dir> saves a picture of the ruler (RULER_LANG=de/ru).
#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Timecode.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTest>
#include <QTimer>
#include <QToolTip>
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
    mouse(w, QEvent::MouseMove, (from * 2 + to) / 3, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseMove, (from + to * 2) / 3, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseMove, to, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseButtonRelease, to, Qt::NoButton, mods);
}

constexpr int kBarY = TimelineView::kRulerH - 4;   // lower half of the In/Out bar (handles and the range's middle)
constexpr int kUpperY = TimelineView::kRulerH - 13; // upper half of the bar: scrubs inside the range

struct Fixture {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    TimelineView tv{&editor};
    QVector<int> seeks;

    Fixture()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, false, false});
        Timeline tl;
        tl.video.resize(2);
        tl.audio.resize(1);
        tl.video[0].clips << clip(1, 0, 100) << clip(2, 150, 100); // V1: edges 0, 100, 150, 250
        tl.video[1].clips << clip(3, 50, 70);                       // V2: edges 50, 120
        project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 5, 0});
        tv.resize(1200, 300);
        tv.show();
        QObject::connect(&tv, &TimelineView::seekRequested, [this](int f) { seeks << f; });
        QApplication::processEvents();
    }
    int x(double frame) const { return int(std::lround(TimelineView::kHeaderW + (frame - tv.view().leftFrame) * tv.view().pxPerFrame)); }
    int in() const { return project.timeline().markIn; }
    int out() const { return project.timeline().markOut; }
};

// Drag the In handle: snaps onto a clip edge, Shift drags freely, one undo step per drag, never past Out
void testInHandle()
{
    Fixture f;
    f.editor.setMarks(20, 79);
    QUndoStack* undo = f.project.undoStack();
    const int steps = undo->index();
    drag(f.tv, {f.x(20) + 2, kBarY}, {f.x(51) + 2, kBarY}); // V2's start (50) is one frame away: snaps
    CHECK_EQ(f.in(), 50);
    CHECK_EQ(f.out(), 79);
    CHECK_EQ(undo->index(), steps + 1);
    CHECK(f.seeks.isEmpty()); // a drag does not move the playhead
    drag(f.tv, {f.x(50) + 2, kBarY}, {f.x(49) + 2 - 4 * 4, kBarY}, Qt::ShiftModifier); // 45, next to 50: no snap
    CHECK_EQ(f.in(), 45);
    CHECK_EQ(undo->index(), steps + 2);
    undo->undo();
    CHECK_EQ(f.in(), 50);
    undo->undo();
    CHECK_EQ(f.in(), 20);
    // Past Out: clamped onto it (a one-frame range), Out stays
    drag(f.tv, {f.x(20) + 2, kBarY}, {f.x(200), kBarY}, Qt::ShiftModifier);
    CHECK_EQ(f.in(), 79);
    CHECK_EQ(f.out(), 79);
}

// Drag the Out handle: its edge (after the last frame) snaps onto clip ends and markers; never before In
void testOutHandle()
{
    Fixture f;
    f.editor.setMarks(20, 79);
    drag(f.tv, {f.x(80) - 2, kBarY}, {f.x(101) - 2, kBarY}); // the edge at 100 = V1's first clip end
    CHECK_EQ(f.out(), 99);
    f.editor.toggleMarker(75);
    drag(f.tv, {f.x(100) - 2, kBarY}, {f.x(76) - 2, kBarY}); // a marker snaps too
    CHECK_EQ(f.out(), 74);
    drag(f.tv, {f.x(75) - 2, kBarY}, {f.x(76) - 2, kBarY}, Qt::ShiftModifier); // Shift: no marker snap
    CHECK_EQ(f.out(), 75);
    drag(f.tv, {f.x(76) - 2, kBarY}, {f.x(0) + 2, kBarY}, Qt::ShiftModifier); // before In: clamped
    CHECK_EQ(f.out(), 20);
    CHECK_EQ(f.in(), 20);
}

// The range's lower half moves the whole range (length kept); its upper half and a click move the playhead
void testRange()
{
    Fixture f;
    f.editor.setMarks(20, 59);
    QUndoStack* undo = f.project.undoStack();
    const int steps = undo->index();
    drag(f.tv, {f.x(40), kBarY}, {f.x(71), kBarY}, Qt::ShiftModifier);
    CHECK_EQ(f.in(), 51);
    CHECK_EQ(f.out(), 90);
    CHECK_EQ(undo->index(), steps + 1);
    drag(f.tv, {f.x(70), kBarY}, {f.x(70) - 4, kBarY}); // in 50 = V2's start: snaps
    CHECK_EQ(f.in(), 50);
    CHECK_EQ(f.out(), 89);
    drag(f.tv, {f.x(70), kBarY}, {f.x(-300), kBarY}, Qt::ShiftModifier); // not before frame 0
    CHECK_EQ(f.in(), 0);
    CHECK_EQ(f.out(), 39);
    // A click inside the range: the playhead jumps, the range stays
    f.seeks.clear();
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(30), kBarY}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(30), kBarY}, Qt::NoButton);
    CHECK(f.seeks == QVector<int>{30});
    CHECK_EQ(f.in(), 0);
    // The upper half of the ruler scrubs as before, also inside the range
    f.seeks.clear();
    drag(f.tv, {f.x(10), kUpperY}, {f.x(25), kUpperY});
    CHECK(!f.seeks.isEmpty() && f.seeks.first() == 10 && f.seeks.last() == 25);
    CHECK_EQ(f.in(), 0);
    CHECK_EQ(f.out(), 39);
    // Outside the range: a click and a drag scrub
    f.seeks.clear();
    drag(f.tv, {f.x(300), kBarY}, {f.x(320), kBarY});
    CHECK(!f.seeks.isEmpty() && f.seeks.first() == 300 && f.seeks.last() == 320);
    CHECK_EQ(f.out(), 39);
}

// Alt-drag on the ruler draws a new range (either direction); a plain drag there only scrubs
void testCreate()
{
    Fixture f;
    drag(f.tv, {f.x(160), kUpperY}, {f.x(200), kUpperY});
    CHECK_EQ(f.in(), -1);
    QUndoStack* undo = f.project.undoStack();
    const int steps = undo->index();
    drag(f.tv, {f.x(160), kUpperY}, {f.x(200), kUpperY}, Qt::AltModifier);
    CHECK_EQ(f.in(), 160);
    CHECK_EQ(f.out(), 199);
    CHECK_EQ(undo->index(), steps + 1);
    drag(f.tv, {f.x(300), kUpperY}, {f.x(251), kUpperY}, Qt::AltModifier); // backwards; 251 snaps onto 250
    CHECK_EQ(f.in(), 250);
    CHECK_EQ(f.out(), 299);
    CHECK_EQ(undo->index(), steps + 2);
    // An Alt-click without dragging changes nothing but the playhead
    f.seeks.clear();
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(400), kUpperY}, Qt::LeftButton, Qt::AltModifier);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(400), kUpperY}, Qt::NoButton, Qt::AltModifier);
    CHECK(f.seeks == QVector<int>{400});
    CHECK_EQ(f.in(), 250);
}

// Hover: the resize cursor over a handle, the hand over the range; the tooltip names timecode and length
void testHoverAndTip()
{
    Fixture f;
    f.editor.setMarks(25, 74); // 25 frames = 00:00:01:00, 50 frames long
    mouse(f.tv, QEvent::MouseMove, {f.x(25) + 2, kBarY}, Qt::NoButton);
    CHECK_EQ(int(f.tv.cursor().shape()), int(Qt::SizeHorCursor));
    mouse(f.tv, QEvent::MouseMove, {f.x(50), kBarY}, Qt::NoButton);
    CHECK_EQ(int(f.tv.cursor().shape()), int(Qt::OpenHandCursor));
    const int fps = f.project.fps();
    const QPoint at(f.x(25) + 2, kBarY);
    QHelpEvent help(QEvent::ToolTip, at, f.tv.mapToGlobal(at));
    QApplication::sendEvent(&f.tv, &help);
    const QString tip = QToolTip::text();
    CHECK(tip.contains(Timecode::format(25, fps)));
    CHECK(tip.contains(Timecode::format(50, fps)));
    QToolTip::hideText();
}

// Right-click on the ruler: In here, Out here, remove
void testMenu()
{
    Fixture f;
    auto pick = [&](int frame, const QString& text) {
        bool found = false;
        QTimer::singleShot(100, [&] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* a : menu->actions())
                    if (a->text() == text && a->isEnabled()) {
                        found = true;
                        a->trigger();
                    }
                menu->close();
            }
        });
        const QPoint at(f.x(frame) + 1, kUpperY);
        QContextMenuEvent e(QContextMenuEvent::Mouse, at, f.tv.mapToGlobal(at));
        QApplication::sendEvent(&f.tv, &e);
        QApplication::processEvents();
        return found;
    };
    CHECK(pick(30, T("In-Punkt hier setzen")));
    CHECK_EQ(f.in(), 30);
    CHECK(pick(90, T("Out-Punkt hier setzen")));
    CHECK_EQ(f.out(), 90);
    CHECK(pick(60, T("In/Out entfernen")));
    CHECK_EQ(f.in(), -1);
    CHECK_EQ(f.out(), -1);
    CHECK(!pick(60, T("In/Out entfernen"))); // nothing to remove: disabled
}

// The handles are drawn (and a picture for a look by eye)
void testPicture()
{
    Fixture f;
    f.editor.setMarks(40, 139);
    f.editor.toggleMarker(170);
    mouse(f.tv, QEvent::MouseMove, {f.x(140) - 2, kBarY}, Qt::NoButton); // the Out handle lit
    QApplication::processEvents();
    const QImage shot = f.tv.grab().toImage();
    const QColor handle = shot.pixelColor(f.x(40) + 1, TimelineView::kRulerH - 9);
    const QColor inside = shot.pixelColor(f.x(90) + 10, TimelineView::kRulerH - 9);
    CHECK(handle != inside);
    CHECK(handle.blue() > 150); // Theme::secondary
    const QString dir = qEnvironmentVariable("RULER_SHOT_DIR");
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
        shot.copy(0, 0, f.x(260), 70).scaled(f.x(260) * 2, 140, Qt::IgnoreAspectRatio, Qt::FastTransformation)
            .save(dir + "/ruler-marks-" + qEnvironmentVariable("RULER_LANG", "de") + ".png");
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("ruler_marks");
    I18n::install(qEnvironmentVariable("RULER_LANG", "de"));
    Theme::apply(app);
    testInHandle();
    testOutHandle();
    testRange();
    testCreate();
    testHoverAndTip();
    testMenu();
    testPicture();
    return Check::result();
}
