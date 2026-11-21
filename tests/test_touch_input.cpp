// SPDX-License-Identifier: AGPL-3.0-only
// Touch input (offscreen, a touchscreen device made by QTest): the timeline (tap select/deselect, drag a clip, trim
// with the finger's larger grab zone, ruler tap, long press menu, pinch zoom, two-finger scroll), the viewer (pinch
// zoom around the fingers, two-finger pan, double tap = fit), lists (kinetic scroll present and scrolling, tap
// selects, long press + drag of an item onto the timeline) and a slider inside a scrolling panel. Mouse behaviour at
// the same spots stays as it was. Phone basics: click focus and the on-screen keyboard for a finger, the focused
// field kept in view, the back key's steps (popup, dialog, the window's own step). Finger-sized handles: a fade
// grip and a transition's edge just outside the mouse's zone, a pinch on the track headers (track height). Touch
// mode: auto detection, the setting, larger zones and rows. The other views: the node canvas (pinch zoom), the
// Studio lanes (pinch zoom, pinch on the headers = lane height), the timeline overview (a finger's edge zone, the
// rectangle follows two fingers); a long press where there is no menu shows the tooltip (or a hint).
#include "check.h"
#include "timeline_guard.h"

#include "app/SettingsDialog.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/FrameView.h"
#include "ui/MediaPool.h"
#include "ui/comp/CompCanvas.h"
#include "ui/studio/StudioTracks.h"
#include "ui/timeline/TimelineOverview.h"
#include "ui/timeline/TimelineView.h"
#include "ui/touch/Touch.h"
#include "ui/touch/TouchGestures.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QInputDevice>
#include <QSettings>
#include <QSignalSpy>
#include <QToolTip>
#include <QLineEdit>
#include <QMessageBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMenu>
#include <QTimer>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QSlider>
#include <QSpinBox>
#include <QStyleHints>
#include <QTest>
#include <QVBoxLayout>
#include <cmath>

namespace {

QPointingDevice* g_touch = nullptr;

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

// Let a following tap not count as the second of a double tap
void pauseTaps() { QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50); }

void tap(QWidget* w, QPoint p)
{
    QTest::touchEvent(w, g_touch).press(0, p, w);
    QTest::touchEvent(w, g_touch).release(0, p, w);
}

// One finger from `a` to `b` in steps
void swipe(QWidget* w, QPoint a, QPoint b, int steps = 6)
{
    QTest::touchEvent(w, g_touch).press(0, a, w);
    for (int i = 1; i <= steps; ++i) QTest::touchEvent(w, g_touch).move(0, a + (b - a) * i / steps, w);
    QTest::touchEvent(w, g_touch).release(0, b, w);
}

// Two fingers: from a0/a1 to b0/b1 in steps
void twoFingers(QWidget* w, QPoint a0, QPoint a1, QPoint b0, QPoint b1, int steps = 5)
{
    QTest::touchEvent(w, g_touch).press(0, a0, w).press(1, a1, w);
    for (int i = 1; i <= steps; ++i)
        QTest::touchEvent(w, g_touch).move(0, a0 + (b0 - a0) * i / steps, w).move(1, a1 + (b1 - a1) * i / steps, w);
    QTest::touchEvent(w, g_touch).release(0, b0, w).release(1, b1, w);
}

void mouse(QWidget& w, QEvent::Type type, QPoint pt, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QApplication::sendEvent(&w, &e);
}

struct Fixture {
    Project project;
    TimelineGuard guard_project{project};
    Selection sel;
    Editor editor{&project, &sel};
    QWidget window;
    QListWidget* list = nullptr;
    TimelineView* tv = nullptr;

    Fixture()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, false, false});
        Timeline tl;
        tl.video.resize(2);
        tl.audio.resize(1);
        tl.video[0].clips << clip(1, 0, 100) << clip(2, 150, 100); // V1 (y 94..158)
        tl.video[1].clips << clip(3, 50, 70);                       // V2 (y 30..94)
        tl.audio[0].clips << clip(4, 0, 100);                       // A1
        project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 5, 0});
        auto* lay = new QHBoxLayout(&window);
        lay->setContentsMargins(0, 0, 0, 0);
        tv = new TimelineView(&editor);
        tv->setSnapping(false);
        lay->addWidget(tv, 1);
        window.resize(790, 500); // inside the offscreen screen (800 x 600): touch points outside it get lost
        window.show();
        CHECK(QTest::qWaitForWindowExposed(&window));
        tv->setZoomAndScroll(2.0, 0, 0);
    }
    int x(double frame) const
    {
        return int(std::lround(TimelineView::kHeaderW + (frame - tv->view().leftFrame) * tv->view().pxPerFrame));
    }
};

void testTimelineTapAndMove()
{
    Fixture f;
    // Tap on clip 2 selects it, a tap into the gap deselects
    tap(f.tv, {f.x(200), 120});
    CHECK(f.sel.ids() == QSet<int>{2});
    pauseTaps();
    tap(f.tv, {f.x(130), 120});
    CHECK(f.sel.isEmpty());
    pauseTaps();

    // One finger on a clip drags it (20 frames right)
    swipe(f.tv, {f.x(200), 120}, {f.x(220), 120});
    CHECK_EQ(f.project.timeline().video[0].clips[1].start, 170);
    CHECK(f.sel.ids() == QSet<int>{2});
    // No view change by any of this (core rule)
    CHECK_EQ(f.tv->view().leftFrame, 0.0);
    CHECK_EQ(f.tv->view().pxPerFrame, 2.0);
    pauseTaps();

    // Tap on the ruler: the playhead goes there
    int seek = -1;
    QObject::connect(f.tv, &TimelineView::seekRequested, [&](int frame) { seek = frame; });
    tap(f.tv, {f.x(300), 15});
    CHECK_EQ(seek, 300);
}

void testTimelineTrimZone()
{
    // 10 px inside clip 2's end: outside the mouse's grab zone (6 px), inside the finger's (14 px)
    {
        Fixture f;
        const QPoint at(f.x(250) - 10, 120);
        mouse(*f.tv, QEvent::MouseButtonPress, at, Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseMove, at + QPoint(20, 0), Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseMove, at + QPoint(40, 0), Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseButtonRelease, at + QPoint(40, 0), Qt::NoButton);
        const Clip& c = f.project.timeline().video[0].clips[1];
        CHECK_EQ(c.start, 170); // the mouse moved it (40 px = 20 frames), as before
        CHECK_EQ(c.length(), 100);
    }
    {
        Fixture f;
        const QPoint at(f.x(250) - 10, 120);
        swipe(f.tv, at, at + QPoint(40, 0));
        const Clip& c = f.project.timeline().video[0].clips[1];
        CHECK_EQ(c.start, 150); // the finger trimmed its end
        CHECK_EQ(c.length(), 120);
    }
}

void testTimelineLongPress()
{
    Fixture f;
    int menus = 0;
    QObject::connect(f.tv, &TimelineView::clipMenuRequested, [&](const QPoint&) { ++menus; });
    QTest::touchEvent(f.tv, g_touch).press(0, {f.x(50), 120}, f.tv);
    QTest::qWait(Touch::longPressMs() + 150);
    QTest::touchEvent(f.tv, g_touch).release(0, {f.x(50), 120}, f.tv);
    CHECK_EQ(menus, 1);
    CHECK(f.sel.contains(1)); // the clip pressed is the one the menu is for
    CHECK_EQ(f.project.timeline().video[0].clips[0].start, 0);

    // On the ruler (its mouse menu opens on the right button's release): the ruler menu opens
    pauseTaps();
    QString menuTitle;
    QTimer closer;
    QObject::connect(&closer, &QTimer::timeout, [&] {
        if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            if (!m->actions().isEmpty()) menuTitle = m->actions().first()->text();
            m->close();
        }
    });
    closer.start(50);
    QTest::touchEvent(f.tv, g_touch).press(0, {f.x(300), 15}, f.tv);
    QTest::qWait(Touch::longPressMs() + 150);
    QTest::touchEvent(f.tv, g_touch).release(0, {f.x(300), 15}, f.tv);
    CHECK(!menuTitle.isEmpty());
}

void testTimelinePinchAndScroll()
{
    Fixture f;
    // Spread two fingers around frame 200: twice the zoom (2 -> 4 px per frame), frame 200 stays between them
    const int cx = f.x(200);
    twoFingers(f.tv, {cx - 50, 120}, {cx + 50, 120}, {cx - 100, 120}, {cx + 100, 120});
    CHECK(std::abs(f.tv->view().pxPerFrame - 4.0) < 0.05);
    CHECK(std::abs(f.tv->view().leftFrame + (cx - TimelineView::kHeaderW) / f.tv->view().pxPerFrame - 200) < 1.0);
    CHECK(f.sel.isEmpty()); // no tap, no rubber band
    // Both fingers 160 px to the left: the view scrolls 160 px (40 frames at 4 px)
    const double left = f.tv->view().leftFrame;
    twoFingers(f.tv, {cx - 50, 120}, {cx + 50, 120}, {cx - 210, 120}, {cx - 110, 120});
    CHECK(std::abs(f.tv->view().leftFrame - (left + 40)) < 0.5);
    CHECK(std::abs(f.tv->view().pxPerFrame - 4.0) < 0.05);
    CHECK_EQ(f.project.timeline().video[0].clips[1].start, 150); // nothing moved
}

QImage card(QSize s)
{
    QImage img(s, QImage::Format_RGB32);
    img.fill(Qt::darkGray);
    QPainter p(&img);
    p.fillRect(QRect(QPoint(0, 0), s / 2), Qt::red);
    return img;
}

void testViewer()
{
    FrameView view;
    view.resize(960, 600);
    view.show();
    view.setFrame(card({1920, 1080}));
    QApplication::processEvents();
    const double fit = view.fitZoom();
    CHECK(std::abs(fit - 0.5) < 1e-6);
    // Pinch around a point off the centre: the picture point there stays under the fingers
    const QPoint c(300, 200);
    const QPointF under = view.widgetToFrame(c);
    twoFingers(&view, c - QPoint(60, 0), c + QPoint(60, 0), c - QPoint(120, 0), c + QPoint(120, 0));
    CHECK(std::abs(view.zoom() - 2 * fit) < 0.01);
    const QPointF back = view.frameToWidget(under);
    CHECK(std::abs(back.x() - c.x()) < 2 && std::abs(back.y() - c.y()) < 2);
    // Two fingers together pan the zoomed picture
    const QPointF pan = view.pan();
    twoFingers(&view, {400, 300}, {500, 300}, {440, 330}, {540, 330});
    CHECK(std::abs(view.pan().x() - (pan.x() + 40)) < 1.5 && std::abs(view.pan().y() - (pan.y() + 30)) < 1.5);
    // Double tap: fit
    pauseTaps();
    tap(&view, {480, 300});
    tap(&view, {482, 300});
    CHECK(view.zoom() == FrameView::kFit);
    CHECK(view.pan() == QPointF());
    // Pinching in below the fit size stays at fit
    pauseTaps();
    twoFingers(&view, {380, 300}, {580, 300}, {440, 300}, {520, 300});
    CHECK(view.zoom() == FrameView::kFit);
}

// A list whose items drag like the Media Pool's
class PoolList : public QListWidget {
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* d = new QMimeData;
        if (!items.isEmpty()) d->setData(MediaPool::MimeType, items.first()->data(Qt::UserRole).toString().toUtf8());
        return d;
    }
};

void testLists()
{
    Fixture f;
    auto* list = new PoolList;
    list->setDragEnabled(true);
    for (int i = 0; i < 100; ++i) {
        auto* it = new QListWidgetItem(QString("clip %1").arg(i), list);
        it->setData(Qt::UserRole, "/x/a.mp4");
    }
    static_cast<QHBoxLayout*>(f.window.layout())->insertWidget(0, list);
    list->setFixedWidth(200);
    QApplication::processEvents();
    CHECK(Touch::hasKineticScroll(list)); // every scroll area of the app has it
    QWidget* vp = list->viewport();

    // A finger swipe up scrolls the list (and keeps going a little: QScroller)
    CHECK_EQ(list->verticalScrollBar()->value(), 0);
    swipe(vp, {100, 400}, {100, 150}, 10);
    QTest::qWait(300); // QScroller moves the content on its animation timer
    CHECK(list->verticalScrollBar()->value() > 100);
    QScroller::scroller(vp)->stop();
    CHECK(list->selectedItems().isEmpty()); // scrolling selects nothing
    pauseTaps();

    // Tap selects the item under the finger
    const QRect r1 = list->visualItemRect(list->item(list->indexAt({100, 40}).row()));
    tap(vp, r1.center());
    CHECK(list->currentItem() && list->currentItem()->isSelected());
    const int picked = list->currentRow();
    pauseTaps();

    // Long press on an item, then the finger moves onto the timeline and lifts: dropped there
    QStringList dropped;
    int dropFrame = -1;
    QObject::connect(f.tv, &TimelineView::dropRequested,
                     [&](const QStringList& paths, int frame, int, PlaceParts) { dropped = paths; dropFrame = frame; });
    const QRect r2 = list->visualItemRect(list->item(picked + 2));
    QTest::touchEvent(vp, g_touch).press(0, r2.center(), vp);
    QTest::qWait(Touch::longPressMs() + 150);
    CHECK_EQ(list->currentRow(), picked + 2);
    const int scrolled = list->verticalScrollBar()->value();
    const QPoint onTimeline = f.tv->mapTo(vp->window(), QPoint(f.x(150), 120)) - vp->mapTo(vp->window(), QPoint());
    for (int i = 1; i <= 6; ++i) {
        QTest::touchEvent(vp, g_touch).move(0, r2.center() + (onTimeline - r2.center()) * i / 6, vp);
        CHECK(TouchDrag::active());
    }
    QTest::touchEvent(vp, g_touch).release(0, onTimeline, vp);
    CHECK(!TouchDrag::active());
    CHECK(dropped == QStringList{"/x/a.mp4"});
    CHECK(std::abs(dropFrame - 150) <= 1);
    CHECK_EQ(list->verticalScrollBar()->value(), scrolled); // the drag did not scroll the list
}

void testSliderInPanel()
{
    // A slider in a scrolling panel: the finger moves the slider, the panel stays
    QScrollArea area;
    auto* inner = new QWidget;
    auto* lay = new QVBoxLayout(inner);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setFixedWidth(300);
    lay->addWidget(slider);
    lay->addSpacing(2000);
    area.setWidget(inner);
    area.resize(400, 300);
    area.show();
    QApplication::processEvents();
    CHECK(Touch::hasKineticScroll(&area));
    const QPoint handle = slider->mapTo(area.viewport(), QPoint(slider->style()->sliderPositionFromValue(0, 100, 0, slider->width()) + 4, slider->height() / 2));
    swipe(area.viewport(), handle, handle + QPoint(150, 0), 8);
    CHECK(slider->value() > 20);
    CHECK_EQ(area.verticalScrollBar()->value(), 0);
    // ...and a swipe on the empty part scrolls it
    swipe(area.viewport(), {200, 250}, {200, 60}, 8);
    QTest::qWait(300);
    CHECK(area.verticalScrollBar()->value() > 100);
}

void testFocusAndKeyboard()
{
    Fixture f;
    // A tap on the timeline gives it the focus like a click (its keys work)
    auto* other = new QLineEdit;
    static_cast<QHBoxLayout*>(f.window.layout())->insertWidget(0, other);
    other->setFocus();
    QApplication::processEvents();
    CHECK(other->hasFocus());
    tap(f.tv, {f.x(130), 120});
    CHECK(f.tv->hasFocus());
    pauseTaps();

    // A text field in a scrolling panel: a finger focuses it and asks for the keyboard at once
    QScrollArea area;
    auto* inner = new QWidget;
    auto* lay = new QVBoxLayout(inner);
    auto* top = new QLineEdit;
    lay->addWidget(top);
    auto* spin = new QSpinBox;
    lay->addWidget(spin);
    lay->addSpacing(1500);
    auto* low = new QLineEdit;
    lay->addWidget(low);
    area.setWidget(inner);
    area.setWidgetResizable(true);
    area.resize(400, 300);
    area.show();
    CHECK(QTest::qWaitForWindowExposed(&area));
    area.activateWindow();
    QApplication::processEvents();
    area.setFocus(); // (the window's activation gave the first field the focus)
    CHECK(!top->hasFocus());
    const int asked = Touch::inputPanelRequests();
    const QPoint inTop = top->mapTo(area.viewport(), top->rect().center());
    QTest::touchEvent(area.viewport(), g_touch).press(0, inTop, area.viewport());
    QTest::touchEvent(area.viewport(), g_touch).release(0, inTop, area.viewport());
    QApplication::processEvents();
    CHECK(top->hasFocus());
    CHECK_EQ(Touch::inputPanelRequests(), asked + 1);
    pauseTaps();
    // Tapped again (the keyboard was put away): asked again
    QTest::touchEvent(area.viewport(), g_touch).press(0, inTop, area.viewport());
    QTest::touchEvent(area.viewport(), g_touch).release(0, inTop, area.viewport());
    CHECK_EQ(Touch::inputPanelRequests(), asked + 2);
    pauseTaps();
    // A number field too (its text part lifts, the field has the focus)
    const QPoint inSpin = spin->mapTo(area.viewport(), QPoint(10, spin->height() / 2));
    QTest::touchEvent(area.viewport(), g_touch).press(0, inSpin, area.viewport());
    QTest::touchEvent(area.viewport(), g_touch).release(0, inSpin, area.viewport());
    CHECK(spin->hasFocus());
    CHECK_EQ(Touch::inputPanelRequests(), asked + 3);
    top->setFocus();
    pauseTaps();
    // Scrolling the panel with a finger keeps the focus where it is
    swipe(area.viewport(), {300, 250}, {300, 60}, 8);
    QTest::qWait(200);
    QScroller::scroller(area.viewport())->stop();
    CHECK(top->hasFocus());
    CHECK_EQ(Touch::inputPanelRequests(), asked + 3);
    // A mouse click never asks for the keyboard
    area.verticalScrollBar()->setValue(area.verticalScrollBar()->maximum());
    QApplication::processEvents();
    QTest::mouseClick(low, Qt::LeftButton, {}, low->rect().center());
    CHECK(low->hasFocus());
    CHECK_EQ(Touch::inputPanelRequests(), asked + 3);
    // The keyboard came up: the focused field scrolls into view
    area.verticalScrollBar()->setValue(0);
    QApplication::processEvents();
    Touch::keepFocusVisible();
    CHECK(area.verticalScrollBar()->value() > 1000);
}

void testBackKey()
{
    QWidget window;
    window.resize(400, 300);
    window.show();
    CHECK(QTest::qWaitForWindowExposed(&window));
    int steps = 0;
    const std::function<bool()> step = [&] {
        ++steps;
        return true;
    };
    // An open menu closes first
    QMenu menu;
    menu.addAction("a");
    menu.popup(window.mapToGlobal(QPoint(50, 50)));
    QApplication::processEvents();
    CHECK(QApplication::activePopupWidget() == &menu);
    CHECK(Touch::back(step));
    QApplication::processEvents();
    CHECK(!menu.isVisible());
    CHECK_EQ(steps, 0);
    // Then the dialog in front: rejected like Esc
    QDialog dlg(&window);
    dlg.setModal(true);
    dlg.show();
    QApplication::processEvents();
    CHECK(Touch::back(step));
    CHECK(!dlg.isVisible());
    CHECK_EQ(dlg.result(), int(QDialog::Rejected));
    CHECK_EQ(steps, 0);
    // A question takes its Esc answer (No), never Yes
    QMessageBox box(QMessageBox::Question, "schneidi", "?", QMessageBox::Yes | QMessageBox::No, &window);
    box.setWindowModality(Qt::ApplicationModal);
    box.show();
    QApplication::processEvents();
    CHECK(Touch::back(step));
    QApplication::processEvents();
    CHECK(!box.isVisible());
    CHECK(box.clickedButton() == box.button(QMessageBox::No));
    CHECK_EQ(steps, 0);
    // Nothing in front: the window's own step
    CHECK(Touch::back(step));
    CHECK_EQ(steps, 1);

    // The key itself: taken (accepted) on press and release, one step per press
    Touch::installBackKey(qApp, step);
    window.activateWindow();
    window.setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
    QApplication::sendEvent(&window, &press);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Back, Qt::NoModifier);
    release.ignore();
    QApplication::sendEvent(&window, &release);
    CHECK(release.isAccepted());
    CHECK_EQ(steps, 2);
}

// Mouse hover over a point (no button): what the mouse sees before it presses
void hover(QWidget& w, QPoint pt) { mouse(w, QEvent::MouseMove, pt, Qt::NoButton); }

void testFadeHandle()
{
    // Clip 2 (frames 150..250, x 300..500 at 2 px per frame): 13 px right of its start, 5 px below its top - outside
    // the mouse's grab zone of the fade grip, inside the finger's
    {
        Fixture f;
        const QPoint at(f.x(150) + 13, 100);
        hover(*f.tv, at);
        mouse(*f.tv, QEvent::MouseButtonPress, at, Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseMove, at + QPoint(20, 0), Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseMove, at + QPoint(40, 0), Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseButtonRelease, at + QPoint(40, 0), Qt::NoButton);
        const Clip& c = f.project.timeline().video[0].clips[1];
        CHECK_EQ(c.fadeIn, 0);
        CHECK_EQ(c.start, 170); // the mouse moved the clip, as before
    }
    {
        Fixture f;
        // No hover for a finger: a tap selects the clip and shows its grips, the next touch grabs one
        tap(f.tv, {f.x(200), 120});
        CHECK(f.sel.contains(2));
        pauseTaps();
        const QPoint at(f.x(150) + 13, 100);
        swipe(f.tv, at, at + QPoint(40, 0));
        const Clip& c = f.project.timeline().video[0].clips[1];
        CHECK(c.fadeIn >= 15 && c.fadeIn <= 25);
        CHECK_EQ(c.start, 150);
        pauseTaps();
        // An unselected clip has no grips for a finger: its corner trims it as before
        const QPoint other(f.x(0) + 13, 100);
        swipe(f.tv, other, other + QPoint(40, 0));
        CHECK_EQ(f.project.timeline().video[0].clips[0].fadeIn, 0);
    }
}

void testTransitionEdge()
{
    // A fade-in of 20 frames at clip 2 (x 300..340): its right edge is the one that drags. 9 px inside it: the mouse
    // is in the middle (selects only), the finger has the edge
    const auto withFade = [](Fixture& f) {
        f.project.edit("fade", [](Timeline& tl) { tl.video[0].clips[1].transIn = 20; });
    };
    {
        Fixture f;
        withFade(f);
        const QPoint at(f.x(170) - 9, 120);
        hover(*f.tv, at);
        mouse(*f.tv, QEvent::MouseButtonPress, at, Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseMove, at + QPoint(20, 0), Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseMove, at + QPoint(40, 0), Qt::LeftButton);
        mouse(*f.tv, QEvent::MouseButtonRelease, at + QPoint(40, 0), Qt::NoButton);
        CHECK_EQ(f.project.timeline().video[0].clips[1].transIn, 20);
    }
    {
        Fixture f;
        withFade(f);
        const QPoint at(f.x(170) - 9, 120);
        swipe(f.tv, at, at + QPoint(40, 0));
        const int len = f.project.timeline().video[0].clips[1].transIn;
        CHECK(len >= 35 && len <= 45);
        CHECK_EQ(f.project.timeline().video[0].clips[1].start, 150);
    }
}

void testHeaderPinch()
{
    Fixture f;
    const int video = f.tv->view().videoTrackHeight, audio = f.tv->view().audioTrackHeight;
    // Two fingers on the V1 header spread apart vertically: video tracks get taller, zoom and scroll stay
    twoFingers(f.tv, {40, 110}, {40, 140}, {40, 95}, {40, 155});
    CHECK(f.tv->view().videoTrackHeight > video * 1.6);
    CHECK_EQ(f.tv->view().audioTrackHeight, audio);
    CHECK_EQ(f.tv->view().pxPerFrame, 2.0);
    CHECK_EQ(f.tv->view().leftFrame, 0.0);
    // Pinched together again: smaller, never below the minimum
    twoFingers(f.tv, {40, 100}, {40, 160}, {40, 128}, {40, 132});
    CHECK(f.tv->view().videoTrackHeight >= 28 && f.tv->view().videoTrackHeight < video);
}

void testTouchMode()
{
    using T = QInputDevice::DeviceType;
    CHECK(Touch::autoOn({int(T::TouchScreen)}));
    CHECK(!Touch::autoOn({int(T::TouchScreen), int(T::Mouse)}));
    CHECK(!Touch::autoOn({int(T::TouchScreen), int(T::TouchPad)}));
    CHECK(!Touch::autoOn({int(T::Mouse), int(T::Keyboard)}));
    // The setting: missing = auto; forced on/off win over the devices
    QSettings().remove(Touch::kModeKey);
    CHECK(Touch::savedMode() == Touch::Mode::Auto);
    Touch::setSavedMode(Touch::Mode::On);
    CHECK(Touch::decideMode());
    Touch::setSavedMode(Touch::Mode::Off);
    CHECK(!Touch::decideMode());
    // In the settings dialog (General)
    {
        SettingsDialog dlg{AiSettings{}};
        auto* combo = dlg.findChild<QComboBox*>("settings_touch_mode");
        CHECK(combo);
        if (combo) {
            CHECK(dlg.touchMode() == Touch::Mode::Off);
            combo->setCurrentIndex(combo->findData(int(Touch::Mode::On)));
            CHECK(dlg.touchMode() == Touch::Mode::On);
        }
    }
    Touch::setSavedMode(Touch::Mode::Auto);
    CHECK(!QSettings().contains(Touch::kModeKey));

    // Mode on: every pointer gets the finger's zones, the timeline taller rows, the theme larger controls
    CHECK_EQ(Touch::grabPx(false, 6), 6);
    Touch::setModeActive(true);
    CHECK_EQ(Touch::grabPx(false, 6), Touch::kGrabPx);
    CHECK(Theme::touchStyleSheet().contains("QPushButton"));
    {
        Fixture f;
        CHECK_EQ(f.tv->view().videoTrackHeight, 84);
        CHECK_EQ(f.tv->view().audioTrackHeight, 68);
    }
    Touch::setModeActive(false);
}

void testCompCanvas()
{
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    CompCanvas canvas(&editor);
    canvas.resize(600, 400);
    canvas.show();
    CHECK(QTest::qWaitForWindowExposed(&canvas));
    const double zoom = canvas.zoom();
    twoFingers(&canvas, {250, 200}, {350, 200}, {200, 200}, {400, 200});
    CHECK(std::abs(canvas.zoom() - 2 * zoom) < 0.05);
    twoFingers(&canvas, {200, 200}, {400, 200}, {250, 200}, {350, 200});
    CHECK(std::abs(canvas.zoom() - zoom) < 0.05);
}

void testStudioTracks()
{
    Project project;
    TimelineGuard guard(project);
    Selection sel;
    Editor editor(&project, &sel);
    project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, false, false});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(2);
    tl.audio[0].clips << clip(1, 0, 400);
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 5, 0});
    StudioTracks tracks(&editor, nullptr);
    tracks.resize(780, 500);
    tracks.show();
    CHECK(QTest::qWaitForWindowExposed(&tracks));
    tracks.setView(1.0, 0);
    // Two fingers spread on the lanes around frame 200: twice the zoom, frame 200 stays between them
    const int cx = int(tracks.xOf(200));
    twoFingers(&tracks, {cx - 50, 200}, {cx + 50, 200}, {cx - 100, 200}, {cx + 100, 200});
    CHECK(std::abs(tracks.view().pxPerFrame - 2.0) < 0.05);
    CHECK(std::abs(tracks.frameAt(cx) - 200) < 1.0);
    CHECK_EQ(project.timeline().audio[0].clips[0].start, 0); // nothing moved
    // Begun on the track headers: the lane height
    const int lane = tracks.laneHeight();
    const QRect a1 = tracks.laneRect(0);
    const int hx = StudioTracks::kHeaderW / 2, hy = a1.center().y();
    twoFingers(&tracks, {hx, hy - 15}, {hx, hy + 15}, {hx, hy - 30}, {hx, hy + 30});
    CHECK(tracks.laneHeight() > lane * 1.5);
    CHECK(std::abs(tracks.view().pxPerFrame - 2.0) < 0.05); // zoom untouched
}

void testOverview()
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << clip(1, 0, 3000);
    const auto make = [&](TimelineOverview& ov) {
        ov.resize(600, TimelineOverview::kHeight);
        ov.setTimeline(tl, 25);
        ov.setVisibleRange(500, 1000);
        ov.show();
        QApplication::processEvents();
    };
    const double y = TimelineOverview::kHeight / 2.0;
    // 9 px inside the rectangle's right edge: the mouse moves the rectangle, a finger drags the edge (zoom)
    {
        TimelineOverview ov;
        make(ov);
        QSignalSpy scroll(&ov, &TimelineOverview::scrollRequested), range(&ov, &TimelineOverview::rangeRequested);
        const QPoint at(int(ov.visibleRect().right()) - 9, int(y));
        mouse(ov, QEvent::MouseButtonPress, at, Qt::LeftButton);
        mouse(ov, QEvent::MouseMove, at + QPoint(20, 0), Qt::LeftButton);
        mouse(ov, QEvent::MouseButtonRelease, at + QPoint(20, 0), Qt::NoButton);
        CHECK(!scroll.isEmpty());
        CHECK(range.isEmpty());
    }
    {
        TimelineOverview ov;
        make(ov);
        QSignalSpy scroll(&ov, &TimelineOverview::scrollRequested), range(&ov, &TimelineOverview::rangeRequested);
        const QPoint at(int(ov.visibleRect().right()) - 9, int(y));
        swipe(&ov, at, at + QPoint(20, 0));
        CHECK(scroll.isEmpty());
        if (CHECK(!range.isEmpty())) CHECK_EQ(range.last().at(0).toDouble(), 500.0);
    }
    // Two fingers spread around the rectangle's middle: it grows around them (the detail view shows more)
    {
        TimelineOverview ov;
        make(ov);
        QSignalSpy range(&ov, &TimelineOverview::rangeRequested);
        // (the detail view follows each request and reports back, like TimelinePanel)
        QObject::connect(&ov, &TimelineOverview::rangeRequested, &ov, [&ov](double a, double b) { ov.setVisibleRange(a, b); });
        const int cx = int(ov.visibleRect().center().x());
        twoFingers(&ov, {cx - 20, int(y)}, {cx + 20, int(y)}, {cx - 40, int(y)}, {cx + 40, int(y)});
        if (CHECK(!range.isEmpty())) {
            const double from = range.last().at(0).toDouble(), to = range.last().at(1).toDouble();
            CHECK(std::abs((to - from) - 1000) < 20);
            CHECK(std::abs((from + to) / 2 - 750) < 20);
        }
    }
}

// A widget with gestures and no menu of its own (like the overview), and one with a menu
class Plain : public QWidget {};
class WithMenu : public QWidget {
public:
    int menus = 0;

protected:
    void contextMenuEvent(QContextMenuEvent*) override { ++menus; }
};

void testLongPressHint()
{
    QWidget window;
    window.resize(400, 200);
    auto* plain = new Plain;
    plain->setParent(&window);
    plain->setGeometry(0, 0, 200, 200);
    plain->setToolTip("drag to move");
    new TouchGestures(plain);
    auto* withMenu = new WithMenu;
    withMenu->setParent(&window);
    withMenu->setGeometry(200, 0, 200, 200);
    withMenu->setToolTip("has a menu");
    new TouchGestures(withMenu);
    window.show();
    CHECK(QTest::qWaitForWindowExposed(&window));
    QToolTip::hideText();
    const auto hold = [](QWidget* w, QPoint p) {
        QTest::touchEvent(w, g_touch).press(0, p, w);
        QTest::qWait(Touch::longPressMs() + 150);
        QTest::touchEvent(w, g_touch).release(0, p, w);
    };
    // No menu: what the mouse's tooltip says
    hold(plain, {100, 100});
    CHECK(QToolTip::isVisible());
    CHECK_EQ(QToolTip::text(), QString("drag to move"));
    QToolTip::hideText();
    pauseTaps();
    // A menu: only the menu
    hold(withMenu, {100, 100});
    CHECK_EQ(withMenu->menus, 1);
    CHECK(!QToolTip::isVisible());
    // No menu, no tooltip: the hint
    plain->setToolTip({});
    pauseTaps();
    hold(plain, {100, 100});
    CHECK(QToolTip::isVisible());
    CHECK_EQ(QToolTip::text(), T("Hier gibt es kein Menü."));
    QToolTip::hideText();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("touch_input");
    I18n::install("de");
    Theme::apply(app);
    Touch::installKineticScrolling(&app);
    g_touch = QTest::createTouchDevice();
    testTimelineTapAndMove();
    testTimelineTrimZone();
    testTimelineLongPress();
    testTimelinePinchAndScroll();
    testViewer();
    testLists();
    testSliderInPanel();
    Touch::installInputPanel(qApp);
    testFocusAndKeyboard();
    testBackKey();
    testFadeHandle();
    testTransitionEdge();
    testHeaderPinch();
    testTouchMode();
    testCompCanvas();
    testStudioTracks();
    testOverview();
    testLongPressHint();
    return Check::result();
}
