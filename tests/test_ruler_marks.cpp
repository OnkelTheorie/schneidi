// SPDX-License-Identifier: AGPL-3.0-only
// The In/Out range in the timeline ruler (offscreen): handles at both edges drag the points (snapping onto clip
// edges and markers, Shift drags freely, In never past Out), the range's lower half moves the whole range (its
// length stays), a right-button drag draws a new range (a right-click without dragging opens the menu), a click still
// moves the playhead, no other left click or drag ever changes the range, one undo step per drag, the hover cursor
// and tooltip, the right-click actions (loop playback in the range). RULER_SHOT_DIR=<dir> saves a picture of the
// ruler (RULER_LANG=de/ru).
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
#include <cstdio>
#include <functional>

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

void mouse(QWidget& w, QEvent::Type type, QPoint pt, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {},
           Qt::MouseButton button = Qt::LeftButton)
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : button, buttons, mods);
    QApplication::sendEvent(&w, &e);
}

void drag(QWidget& w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = {}, Qt::MouseButton button = Qt::LeftButton)
{
    mouse(w, QEvent::MouseButtonPress, from, button, mods, button);
    // Like X11: the context menu event follows the right button's press (Windows: its release)
    if (button == Qt::RightButton) {
        QContextMenuEvent c(QContextMenuEvent::Mouse, from, w.mapToGlobal(from));
        QApplication::sendEvent(&w, &c);
    }
    mouse(w, QEvent::MouseMove, (from * 2 + to) / 3, button, mods);
    mouse(w, QEvent::MouseMove, (from + to * 2) / 3, button, mods);
    mouse(w, QEvent::MouseMove, to, button, mods);
    mouse(w, QEvent::MouseButtonRelease, to, Qt::NoButton, mods, button);
}

// The ruler menu that is open now (a timer while it runs): runs `use` on it and closes it; false = none opened
bool withMenu(const std::function<void()>& open, const std::function<void(QMenu*)>& use)
{
    bool found = false;
    QTimer::singleShot(100, [&] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            found = true;
            use(menu);
            menu->close();
        }
    });
    open();
    QApplication::processEvents();
    return found;
}

QAction* menuAction(QMenu* menu, const QString& text)
{
    for (QAction* a : menu->actions())
        if (a->text() == text) return a;
    return nullptr;
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

// A right-button drag on the ruler draws a new range (either direction, snapping, Shift = free); one undo step
void testCreate()
{
    Fixture f;
    QUndoStack* undo = f.project.undoStack();
    const int steps = undo->index();
    drag(f.tv, {f.x(160), kUpperY}, {f.x(200), kUpperY}, {}, Qt::RightButton);
    CHECK_EQ(f.in(), 160);
    CHECK_EQ(f.out(), 199);
    CHECK_EQ(undo->index(), steps + 1);
    CHECK(f.seeks.isEmpty()); // drawing a range does not move the playhead
    undo->undo();
    CHECK_EQ(f.in(), -1);
    CHECK_EQ(f.out(), -1);
    undo->redo();
    drag(f.tv, {f.x(300), kBarY}, {f.x(251), kBarY}, {}, Qt::RightButton); // backwards; 251 snaps onto 250
    CHECK_EQ(f.in(), 250);
    CHECK_EQ(f.out(), 299);
    CHECK_EQ(undo->index(), steps + 2);
    drag(f.tv, {f.x(300), kBarY}, {f.x(251), kBarY}, Qt::ShiftModifier, Qt::RightButton); // Shift: no snap
    CHECK_EQ(f.in(), 251);
    CHECK_EQ(undo->index(), steps + 3);
    // A right-button drag over an existing range (even from its handle) draws a new one as well
    drag(f.tv, {f.x(251) + 1, kBarY}, {f.x(271) + 1, kBarY}, Qt::ShiftModifier, Qt::RightButton);
    CHECK_EQ(f.in(), 251);
    CHECK_EQ(f.out(), 270);
    // The left button never starts a range: neither a plain nor an Alt-drag on the ruler (Alt-drag used to)
    f.editor.clearMarks();
    for (const Qt::KeyboardModifiers mods : {Qt::KeyboardModifiers{}, Qt::KeyboardModifiers{Qt::AltModifier}})
        for (const int y : {kUpperY, kBarY, 3}) {
            drag(f.tv, {f.x(160), y}, {f.x(200), y}, mods);
            CHECK_EQ(f.in(), -1);
            CHECK_EQ(f.out(), -1);
        }
    // A right-click without dragging: the menu, no range, the playhead stays
    f.seeks.clear();
    const QPoint at(f.x(400), kUpperY);
    CHECK(withMenu([&] { drag(f.tv, at, at, {}, Qt::RightButton); },
                   [&](QMenu* m) { CHECK(menuAction(m, T("In-Punkt hier setzen")) != nullptr); }));
    CHECK_EQ(f.in(), -1);
    CHECK(f.seeks.isEmpty());
    // A jitter of a pixel or two is still a click
    CHECK(withMenu([&] { drag(f.tv, at, at + QPoint(2, 0), {}, Qt::RightButton); }, [](QMenu*) {}));
    CHECK_EQ(f.in(), -1);
}

// Left clicks and drags on the ruler change the range only from its handles and the grip in its middle: inside the
// range but away from the grip they scrub, like everywhere else in the ruler
void testLeftNeverChanges()
{
    Fixture f;
    f.editor.setMarks(20, 219); // wide: the grip sits at 120
    QUndoStack* undo = f.project.undoStack();
    const int steps = undo->index();
    for (const int y : {kUpperY, kBarY, 3})
        for (const int from : {40, 80, 180, 300}) {
            f.seeks.clear();
            drag(f.tv, {f.x(from), y}, {f.x(from + 25), y});
            CHECK_EQ(f.in(), 20);
            CHECK_EQ(f.out(), 219);
            CHECK(!f.seeks.isEmpty() && f.seeks.last() == from + 25);
        }
    CHECK_EQ(undo->index(), steps);
    drag(f.tv, {f.x(120), kBarY}, {f.x(130), kBarY}, Qt::ShiftModifier); // the grip moves it
    CHECK_EQ(f.in(), 30);
    CHECK_EQ(f.out(), 229);
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

// Right-click on the ruler: In here, Out here, remove, loop playback in the range
void testMenu()
{
    Fixture f;
    auto pick = [&](int frame, const QString& text) {
        bool found = false;
        const QPoint at(f.x(frame) + 1, kUpperY);
        withMenu([&] { drag(f.tv, at, at, {}, Qt::RightButton); }, [&](QMenu* m) {
            if (QAction* a = menuAction(m, text); a && a->isEnabled()) {
                found = true;
                a->trigger();
            }
        });
        return found;
    };
    CHECK(pick(30, T("In-Punkt hier setzen")));
    CHECK_EQ(f.in(), 30);
    CHECK(pick(90, T("Out-Punkt hier setzen")));
    CHECK_EQ(f.out(), 90);
    // Loop: the entry asks for it (MainWindow switches it), checked once the view shows it on
    QVector<bool> loops;
    QObject::connect(&f.tv, &TimelineView::loopInOutRequested, [&](bool on) { loops << on; });
    CHECK(pick(60, T("Im Bereich wiederholen")));
    CHECK(loops == QVector<bool>{true});
    f.tv.setLoopInOut(true);
    bool checked = false;
    const QPoint at(f.x(60), kUpperY);
    withMenu([&] { drag(f.tv, at, at, {}, Qt::RightButton); },
             [&](QMenu* m) { checked = menuAction(m, T("Im Bereich wiederholen"))->isChecked(); });
    CHECK(checked);
    f.tv.setLoopInOut(false);
    CHECK(pick(60, T("In/Out entfernen")));
    CHECK_EQ(f.in(), -1);
    CHECK_EQ(f.out(), -1);
    CHECK(!pick(60, T("In/Out entfernen"))); // nothing to remove: disabled
    CHECK(!pick(60, T("Im Bereich wiederholen"))); // no range to loop in: disabled
    // A mouse context menu event alone opens nothing (the menu comes with the release); the menu key opens it
    CHECK(!withMenu([&] {
        QContextMenuEvent e(QContextMenuEvent::Mouse, at, f.tv.mapToGlobal(at));
        QApplication::sendEvent(&f.tv, &e);
    }, [](QMenu*) {}));
    CHECK(withMenu([&] {
        QContextMenuEvent e(QContextMenuEvent::Keyboard, at, f.tv.mapToGlobal(at));
        QApplication::sendEvent(&f.tv, &e);
    }, [](QMenu*) {}));
}

// The handles are drawn (and a picture for a look by eye)
void testPicture()
{
    Fixture f;
    f.editor.setMarks(40, 139);
    f.editor.toggleMarker(170);
    f.tv.setLoopInOut(true); // the loop symbol after the In handle
    mouse(f.tv, QEvent::MouseMove, {f.x(140) - 2, kBarY}, Qt::NoButton); // the Out handle lit
    QApplication::processEvents();
    const QImage shot = f.tv.grab().toImage();
    const QColor handle = shot.pixelColor(f.x(40) + 1, TimelineView::kRulerH - 9);
    const QColor inside = shot.pixelColor(f.x(90) + 10, TimelineView::kRulerH - 9);
    CHECK(handle != inside);
    CHECK(handle.blue() > 150); // Theme::secondary
    // The loop symbol: lighter strokes right after the In handle, none there without loop
    auto lit = [&](const QImage& img) {
        int n = 0;
        for (int x = f.x(40) + 11; x < f.x(40) + 23; ++x)
            for (int y = TimelineView::kRulerH - 14; y < TimelineView::kRulerH - 2; ++y) n += qGray(img.pixel(x, y)) > 150;
        return n;
    };
    const int withLoop = lit(shot);
    f.tv.setLoopInOut(false);
    QApplication::processEvents();
    const int without = lit(f.tv.grab().toImage());
    std::printf("       Schleifensymbol: %d helle Pixel, ohne %d\n", withLoop, without);
    CHECK(withLoop > without + 10);
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
    testLeftNeverChanges();
    testHoverAndTip();
    testMenu();
    testPicture();
    return Check::result();
}
