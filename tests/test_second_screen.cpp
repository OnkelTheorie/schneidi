// SPDX-License-Identifier: AGPL-3.0-only
// Preview on a second monitor (Workspace › Preview on Second Monitor, Ctrl+Shift+F; ui/SecondScreen,
// MainWindow::moveViewerToNextScreen/showPreviewOn).
// SecondScreenPlan with made-up screens (offscreen there is only one): the key goes through the monitors other than
// the main window's and then back into the window; an unplugged monitor clears the target, plugging it in again does
// not set one; the picture is letterboxed (a portrait monitor filled by a 9:16 timeline).
// In the app (on the only screen there is): a frameless full-screen window without the focus, fed with the engine's
// frames; "show source clips" keeps the viewer's area as a source viewer (the timeline's picture only on the monitor:
// an empty page with a button to bring it back, a source clip opened there, its frame kept while the timeline plays
// on the monitor), "hide the preview area" hides it and the panels close up - across page switches; Esc in the window, the key again or the monitor
// going away bring back exactly the layout before; the timeline's zoom/scroll never change (core rule).
// Workspace › Main Window with a Second Monitor and the viewer's right-click switch the mode live; the empty page's
// second button closes up the area.
// SECOND_SCREEN_DUMP=<folder>: pictures of the main window in both modes and of the monitor's window;
// SECOND_SCREEN_LANG=ru (another language), SECOND_SCREEN_FONT=1.25 (larger font) to check that the texts fit.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "core/I18n.h"
#include "engine/Engine.h"
#include "ui/FrameView.h"
#include "ui/FullScreenViewer.h"
#include "ui/SecondScreen.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QMenu>
#include <QScreen>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <clocale>

namespace {

using Plan = SecondScreenPlan;

QString dumpDir() { return qEnvironmentVariable("SECOND_SCREEN_DUMP"); }

void dump(QWidget* w, const QString& name)
{
    if (!dumpDir().isEmpty()) w->grab().save(dumpDir() + "/" + name + ".png");
}

void settle()
{
    QTest::qWait(30);
    QApplication::processEvents();
}

const Plan::Screen kMain{"DP-1", {0, 0, 2560, 1440}};
const Plan::Screen kPortrait{"DP-2", {2560, 0, 1080, 1920}}; // rotated: taller than wide
const Plan::Screen kTv{"HDMI-1", {3640, 0, 3840, 2160}};

void testPlanOrder()
{
    Plan plan;
    CHECK(!plan.setScreens({kMain}));
    CHECK(plan.next("DP-1").isEmpty()); // only the main window's monitor: nowhere to go
    CHECK(!plan.setScreens({kMain, kPortrait}));
    // Two monitors: on and off
    CHECK_EQ(plan.next("DP-1"), QString("DP-2"));
    plan.setTarget("DP-2");
    CHECK(plan.next("DP-1").isEmpty());
    // Three: through the others in order, then back into the window
    plan.setTarget({});
    CHECK(!plan.setScreens({kMain, kPortrait, kTv}));
    CHECK_EQ(plan.next("DP-1"), QString("DP-2"));
    plan.setTarget("DP-2");
    CHECK_EQ(plan.next("DP-1"), QString("HDMI-1"));
    plan.setTarget("HDMI-1");
    CHECK(plan.next("DP-1").isEmpty());
    // The main window on another monitor: never onto its own one
    plan.setTarget({});
    CHECK_EQ(plan.next("DP-2"), QString("DP-1"));
    plan.setTarget("DP-1");
    CHECK_EQ(plan.next("DP-2"), QString("HDMI-1"));
    // The main window was moved onto the monitor the preview is on: start over with the first other one
    plan.setTarget("DP-2");
    CHECK_EQ(plan.next("DP-2"), QString("DP-1"));
    CHECK(plan.screen("DP-2") && plan.screen("DP-2")->geometry.height() > plan.screen("DP-2")->geometry.width());
}

void testPlanRemoval()
{
    Plan plan;
    plan.setScreens({kMain, kPortrait, kTv});
    plan.setTarget("DP-2");
    // Another monitor going does not matter
    CHECK(!plan.setScreens({kMain, kPortrait}));
    CHECK_EQ(plan.target(), QString("DP-2"));
    // Its own going: back into the window
    CHECK(plan.setScreens({kMain}));
    CHECK(plan.target().isEmpty());
    // Plugged in again: no jump by itself
    CHECK(!plan.setScreens({kMain, kPortrait}));
    CHECK(plan.target().isEmpty());
    CHECK_EQ(plan.next("DP-1"), QString("DP-2"));
}

void testPlanPicture()
{
    // A 9:16 timeline fills a portrait monitor
    CHECK_EQ(Plan::fitted({1080, 1920}, {1080, 1920}), QRectF(0, 0, 1080, 1920));
    CHECK_EQ(Plan::fitted({1080, 1920}, {720, 1280}), QRectF(0, 0, 1080, 1920));
    // 16:9 on it: bars above and below, centered
    CHECK_EQ(Plan::fitted({1080, 1920}, {1920, 1080}), QRectF(0, 656.25, 1080, 607.5));
    // 9:16 on a landscape monitor: bars left and right
    CHECK_EQ(Plan::fitted({1920, 1080}, {1080, 1920}), QRectF(656.25, 0, 607.5, 1080));
    CHECK(Plan::fitted({}, {1920, 1080}).isEmpty());
    // What the main window shows
    CHECK(Plan::viewerInWindow(Plan::Mode::HideViewer, false));
    CHECK(Plan::viewerInWindow(Plan::Mode::KeepArea, true));
    CHECK(!Plan::viewerInWindow(Plan::Mode::HideViewer, true));
    CHECK(Plan::holdsTimeline(Plan::Mode::KeepArea, true));
    CHECK(!Plan::holdsTimeline(Plan::Mode::KeepArea, false));
    CHECK(!Plan::holdsTimeline(Plan::Mode::HideViewer, true));
    CHECK(Plan::programElsewhere(Plan::Mode::KeepArea, true));
    CHECK(!Plan::programElsewhere(Plan::Mode::KeepArea, false));
    CHECK(!Plan::programElsewhere(Plan::Mode::HideViewer, true));
}

// The window's picture on a portrait monitor (the window at a portrait size: offscreen has no rotated screen)
void testPortraitWindow(Engine& engine)
{
    QImage tall(540, 960, QImage::Format_RGBA8888_Premultiplied);
    tall.fill(Qt::red);
    FullScreenViewer fs(&engine, {}, false);
    fs.resize(540, 960);
    fs.show();
    fs.setPicture(tall, {1080, 1920});
    settle();
    QRectF r = fs.pictureRect();
    CHECK(r.width() >= fs.width() - 1 && r.height() >= fs.height() - 1);
    // A 16:9 timeline there: letterboxed like the plan says
    QImage wide(960, 540, QImage::Format_RGBA8888_Premultiplied);
    wide.fill(Qt::green);
    fs.setPicture(wide, {1920, 1080});
    settle();
    r = fs.pictureRect();
    const QRectF want = Plan::fitted(fs.size(), QSizeF(1920, 1080));
    CHECK(std::abs(r.width() - want.width()) <= 1 && std::abs(r.top() - want.top()) <= 1);
    // Not following the engine: its frames do not reach it
    const qint64 key = fs.picture().cacheKey();
    engine.seek(3);
    settle();
    CHECK_EQ(fs.picture().cacheKey(), key);
}

struct Sizes {
    QHash<QSplitter*, QList<int>> sizes;
    QHash<QWidget*, bool> hidden;
    static Sizes of(QWidget* w)
    {
        Sizes s;
        for (QSplitter* sp : w->findChildren<QSplitter*>())
            if (sp->isVisible()) {
                s.sizes[sp] = sp->sizes();
                for (int i = 0; i < sp->count(); ++i) s.hidden[sp->widget(i)] = sp->widget(i)->isHidden();
            }
        return s;
    }
};

bool same(const Sizes& a, const Sizes& b, const char* where)
{
    bool ok = true;
    for (auto it = a.hidden.begin(); it != a.hidden.end(); ++it)
        if (b.hidden.value(it.key(), !it.value()) != it.value()) {
            Check::report(false, "visibility changed", __FILE__, __LINE__,
                          QString("%1: %2").arg(where, it.key()->metaObject()->className()));
            ok = false;
        }
    for (auto it = a.sizes.begin(); it != a.sizes.end(); ++it) {
        const QList<int> x = it.value(), y = b.sizes.value(it.key());
        bool near = x.size() == y.size();
        for (int i = 0; near && i < x.size(); ++i) near = std::abs(x[i] - y[i]) <= 1;
        if (!near) {
            QStringList a, b;
            for (int v : x) a << QString::number(v);
            for (int v : y) b << QString::number(v);
            Check::report(false, "splitter sizes changed", __FILE__, __LINE__,
                          QString("%1: %2 %3 -> %4").arg(where, it.key()->objectName(), a.join(' '), b.join(' ')));
            ok = false;
        }
    }
    return ok;
}

void testApp(MainWindow& w, Engine& engine, const QString& clip)
{
    QAction* action = InputBindings::instance().action("viewer_second_screen");
    if (!CHECK(action)) return;
    // Its own key, rebindable, no other window key on it
    CHECK_EQ(InputBindings::instance().inputs("viewer_second_screen").value(0), QString("Ctrl+Shift+F"));
    for (const InputBindings::Entry& e : InputBindings::instance().entries())
        if (e.id != "viewer_second_screen" && e.scope.isEmpty())
            CHECK(!InputBindings::instance().inputs(e.id).contains("Ctrl+Shift+F"));
    CHECK(action->isCheckable());

    auto* viewer = w.findChild<Viewer*>();
    auto* timeline = w.findChild<TimelinePanel*>();
    if (!CHECK(viewer && timeline)) return;
    w.showPage(MainWindow::Page::Edit);
    engine.seek(10);
    settle();
    TimelineView* tv = timeline->view();
    tv->setZoomAndScroll(7.5, 12.0, 0);
    const ViewState view = tv->view();
    // Offscreen there is one screen only: the key finds no second monitor (says so in the status bar) and leaves
    // everything as it is
    if (QGuiApplication::screens().size() == 1) {
        action->trigger();
        settle();
        CHECK(!w.previewOutput());
        CHECK(!action->isChecked());
        CHECK(viewer->isVisible());
    }
    const Sizes before = Sizes::of(&w); // with the status bar the message made
    dump(&w, "window-before");

    // "Show source clips" (the default): the viewer's area stays, the monitor gets the picture
    CHECK(w.secondScreenMode() == Plan::Mode::KeepArea);
    QScreen* screen = QGuiApplication::primaryScreen();
    w.showPreviewOn(screen);
    settle();
    auto* out = qobject_cast<ScreenOutput*>(w.previewOutput());
    if (!CHECK(out && out->window())) return;
    FullScreenViewer* fs = out->window();
    CHECK(action->isChecked());
    CHECK(!w.screenPlan().target().isEmpty());
    CHECK(fs->isVisible() && fs->isFullScreen() && (fs->windowFlags() & Qt::FramelessWindowHint));
    CHECK(fs->geometry() == screen->geometry());
    CHECK(fs->testAttribute(Qt::WA_ShowWithoutActivating)); // the main window keeps the keyboard
    CHECK(viewer->isVisible());
    // ... not the timeline's picture a second time: the empty page says where it is
    CHECK(viewer->programElsewhere());
    CHECK(viewer->programAwayPageShown());
    CHECK(w.findChild<QToolButton*>("viewer_program_back"));
    CHECK(!fs->picture().isNull());
    // Sharp: preview frames with the monitor's pixels
    CHECK_EQ(engine.previewEdge(), out->wantedPreviewEdge());
    CHECK(out->wantedPreviewEdge() > 0);
    // The engine's frames reach it, the viewer keeps what it had (the frame before the monitor came)
    const qint64 viewerBefore = viewer->picture().cacheKey();
    engine.seek(20);
    settle();
    QTest::qWait(100);
    const qint64 program = fs->picture().cacheKey();
    CHECK(program != viewerBefore);
    CHECK_EQ(viewer->picture().cacheKey(), viewerBefore);
    CHECK(viewer->programAwayPageShown());
    dump(&w, "keep-area");
    dump(fs, "monitor");
    // A source clip: in the viewer, the monitor keeps the timeline's picture
    engine.showSource(clip, 5);
    settle();
    QTest::qWait(200);
    CHECK(engine.mode() == Engine::Mode::Source);
    CHECK(viewer->isVisible());
    CHECK_EQ(fs->picture().cacheKey(), program);
    CHECK(viewer->picture().cacheKey() != program);
    CHECK(!viewer->programAwayPageShown());
    const qint64 sourceFrame = viewer->picture().cacheKey();
    dump(&w, "keep-area-source");
    // Back to the timeline (played/sought): the monitor follows, the viewer keeps the source frame, paused
    engine.showTimeline(30);
    settle();
    QTest::qWait(200);
    CHECK(fs->picture().cacheKey() != program);
    CHECK_EQ(viewer->picture().cacheKey(), sourceFrame);
    CHECK(!viewer->programAwayPageShown());
    dump(&w, "keep-area-source-kept");
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);

    // "Vorschau weg": the viewer goes, the panels close up; the timeline keeps zoom and scroll
    w.setSecondScreenMode(Plan::Mode::HideViewer);
    settle();
    CHECK(viewer->isHidden());
    CHECK(!viewer->programElsewhere());
    CHECK(timeline->isVisible());
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);
    // The viewer's room went to the others: no empty row where it was
    for (QSplitter* s = qobject_cast<QSplitter*>(viewer->parentWidget()); s; s = nullptr) {
        int shown = 0;
        for (int i = 0; i < s->count(); ++i) shown += s->widget(i)->isHidden() ? 0 : 1;
        CHECK(shown > 0 || s->isHidden());
    }
    dump(&w, "viewer-gone");
    // The large viewer has nothing to enlarge
    if (QAction* large = InputBindings::instance().action("viewer_large")) {
        large->trigger();
        settle();
        CHECK(!w.viewerLarge());
        CHECK(viewer->isHidden());
    }
    // Source clips go onto the monitor now
    engine.showSource(clip, 8);
    settle();
    QTest::qWait(200);
    CHECK_EQ(fs->picture().cacheKey(), viewer->picture().cacheKey());
    engine.showTimeline(30);
    settle();
    // Across page switches: hidden on the Color page too, the Edit page closed up again on the way back
    w.showPage(MainWindow::Page::Color);
    settle();
    CHECK(viewer->isHidden());
    dump(&w, "viewer-gone-color");
    w.showPage(MainWindow::Page::Edit);
    settle();
    CHECK(viewer->isHidden());
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);

    // Esc in the monitor's window: back into the main window, exactly the layout before
    QTest::keyClick(fs, Qt::Key_Escape);
    settle();
    CHECK(!w.previewOutput() && !action->isChecked());
    CHECK(viewer->isVisible());
    CHECK_EQ(engine.previewEdge(), viewer->wantedPreviewEdge()); // the viewer's own size again
    same(before, Sizes::of(&w), "esc");
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);

    // The monitor unplugged: back as well; the key, the playback keys work in the window
    w.showPreviewOn(screen);
    settle();
    out = qobject_cast<ScreenOutput*>(w.previewOutput());
    if (!CHECK(out && out->window())) return;
    CHECK(viewer->isHidden());
    const int at = engine.position();
    QTest::keyClick(out->window(), Qt::Key_Right);
    settle();
    CHECK_EQ(engine.position(), at + 1);
    QPointer<FullScreenViewer> window = out->window();
    emit qGuiApp->screenRemoved(screen);
    settle();
    CHECK(!w.previewOutput() && !action->isChecked());
    CHECK(w.screenPlan().target().isEmpty());
    CHECK(viewer->isVisible());
    CHECK(!window || !window->isVisible());
    same(before, Sizes::of(&w), "unplugged");
    // Plugged in again: nothing moves by itself
    emit qGuiApp->screenAdded(screen);
    settle();
    CHECK(!w.previewOutput());

    // Settings: the choice is in the dialog ("Fläche bleibt" first, the default)
    {
        SettingsDialog dlg(AiSettings{});
        auto* combo = dlg.findChild<QComboBox*>("second_screen_mode");
        if (CHECK(combo)) {
            CHECK_EQ(combo->count(), 2);
            CHECK_EQ(combo->itemData(0).toInt(), int(Plan::Mode::KeepArea));
            CHECK_EQ(combo->itemText(0), T("Quellclips zeigen"));
            CHECK(dlg.secondScreenMode() == Plan::storedMode());
            combo->setCurrentIndex(1);
            CHECK(dlg.secondScreenMode() == Plan::Mode::HideViewer);
        }
    }
    w.setSecondScreenMode(Plan::Mode::KeepArea);
    CHECK(viewer->isVisible());
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);

    // The empty page's button brings the preview back: the timeline's picture in the viewer again
    w.showPreviewOn(screen);
    settle();
    CHECK(viewer->programAwayPageShown());
    engine.seek(15);
    settle();
    QTest::qWait(100);
    auto* back = w.findChild<QToolButton*>("viewer_program_back");
    if (CHECK(back && back->isVisible())) {
        QTest::mouseClick(back, Qt::LeftButton);
        settle();
        CHECK(!w.previewOutput() && !action->isChecked());
        CHECK(!viewer->programElsewhere() && !viewer->programAwayPageShown());
        engine.seek(16);
        settle();
        QTest::qWait(100);
        CHECK(!viewer->picture().isNull());
    }
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);

    // Workspace › Main Window with a Second Monitor: the settings' choice as radio entries beside the toggle, applied
    // live; the same entries on the viewer's right-click, only while the preview is on the monitor
    QAction* keep = InputBindings::instance().action("second_screen_keep_area");
    QAction* hide = InputBindings::instance().action("second_screen_hide_viewer");
    auto* viewerMenu = w.findChild<QMenu*>("viewer_second_screen_mode_menu");
    if (!CHECK(keep && hide && viewerMenu)) return;
    CHECK(keep->isCheckable() && hide->isCheckable());
    CHECK(keep->isChecked() && !hide->isChecked());
    CHECK_EQ(keep->text(), T("Quellclips zeigen"));
    CHECK_EQ(hide->text(), T("Vorschau-Fläche ausblenden"));
    CHECK(viewer->actions().contains(viewerMenu->menuAction()));
    CHECK(viewerMenu->actions().contains(keep) && viewerMenu->actions().contains(hide));
    CHECK(!viewerMenu->menuAction()->isVisible());
    w.showPreviewOn(screen);
    settle();
    CHECK(viewerMenu->menuAction()->isVisible());
    CHECK(viewer->isVisible() && viewer->programAwayPageShown());
    hide->trigger();
    settle();
    CHECK(w.secondScreenMode() == Plan::Mode::HideViewer);
    CHECK(hide->isChecked() && !keep->isChecked());
    CHECK(viewer->isHidden());
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);
    keep->trigger();
    settle();
    CHECK(w.secondScreenMode() == Plan::Mode::KeepArea);
    CHECK(keep->isChecked() && !hide->isChecked());
    CHECK(viewer->isVisible() && viewer->programAwayPageShown());
    // The settings' way (setSecondScreenMode) moves the radio mark too
    w.setSecondScreenMode(Plan::Mode::HideViewer);
    CHECK(hide->isChecked());
    w.setSecondScreenMode(Plan::Mode::KeepArea);
    settle();
    CHECK(keep->isChecked());
    // The empty page's second button: the area closes up
    auto* hideArea = w.findChild<QToolButton*>("viewer_program_hide_area");
    if (CHECK(hideArea && hideArea->isVisible())) {
        dump(&w, "away-page-buttons");
        QTest::mouseClick(hideArea, Qt::LeftButton);
        settle();
        CHECK(w.secondScreenMode() == Plan::Mode::HideViewer);
        CHECK(hide->isChecked());
        CHECK(viewer->isHidden());
        CHECK(w.previewOutput()); // the preview stays on the monitor
    }
    w.showPreviewOn(nullptr);
    settle();
    CHECK(!viewerMenu->menuAction()->isVisible());
    CHECK(viewer->isVisible());
    w.setSecondScreenMode(Plan::Mode::KeepArea);
    same(before, Sizes::of(&w), "mode-menu");
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);
}

// Closing the main window with the preview on the monitor: the monitor's window goes too (it never keeps the app
// running: no quit-on-close of its own)
void testCloseWithOutput(MainWindow& w)
{
    w.showPreviewOn(QGuiApplication::primaryScreen());
    settle();
    auto* out = qobject_cast<ScreenOutput*>(w.previewOutput());
    if (!CHECK(out && out->window())) return;
    QPointer<FullScreenViewer> fs = out->window();
    CHECK(!fs->testAttribute(Qt::WA_QuitOnClose));
    CHECK(fs->isVisible());
    CHECK(w.close());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    settle();
    CHECK(!w.previewOutput());
    CHECK(!fs || !fs->isVisible());
    CHECK(!fs); // deleted, not only hidden
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("second-screen");
    testPlanOrder();
    testPlanRemoval();
    testPlanPicture();
    QTemporaryDir dir;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    testPortraitWindow(engine);
    if (!Check::haveFfmpeg()) return Check::result();
    if (qEnvironmentVariableIsSet("SECOND_SCREEN_LANG")) I18n::install(qEnvironmentVariable("SECOND_SCREEN_LANG"));
    if (const double scale = qEnvironmentVariable("SECOND_SCREEN_FONT").toDouble(); scale > 0) {
        QFont font = QApplication::font();
        font.setPointSizeF(font.pointSizeF() * scale);
        QApplication::setFont(font);
    }
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&w));
    const QString file = Check::makeMedia(dir.path() + "/clip.mp4",
        {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=2", "-c:v", "libx264", "-preset", "ultrafast",
         "-pix_fmt", "yuv420p"});
    if (!CHECK(!file.isEmpty())) return Check::result();
    w.importFiles({file}, true);
    settle();
    testApp(w, engine, file);
    testCloseWithOutput(w);
    return Check::result();
}
