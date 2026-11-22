// SPDX-License-Identifier: AGPL-3.0-only
// The phone layout (ui/phone/PhoneShell, app/MainWindowPhone.cpp) on a phone-sized window, portrait (411x914, a
// Galaxy A51's logical size) and landscape (914x411): nothing is wider or taller than the window (the panels' minimum
// sizes never force it larger: the Mixer tab made the Android window slide off to the right), one panel at a time as
// the sheet, the back key closes the sheet first, then the clip tools (a selected clip shows them). Which screens get
// it (shorter side below 600 px); the desktop layout stays as it was without it.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Types.h"
#include "engine/Engine.h"
#include "ui/BackgroundJobs.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/RenderQueuePanel.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"
#include "ui/phone/PhoneShell.h"
#include "ui/touch/Touch.h"
#include "app/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QMessageBox>
#include <QTimer>
#include <QLabel>
#include <QLayout>
#include <QMenuBar>
#include <QScrollArea>
#include <QTest>
#include <QToolButton>
#include <functional>

namespace {

void settle()
{
    for (int i = 0; i < 6; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

QString rect(const QRect& r) { return QString("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()); }

// `w` lies inside `outer` (both in the window's coordinates)
bool inside(QWidget* w, QWidget* window)
{
    const QRect r(w->mapTo(window, QPoint(0, 0)), w->size());
    return window->rect().contains(r);
}

void testSizes()
{
    CHECK(PhoneShell::isPhoneSize({411, 914}));  // Galaxy A51 portrait
    CHECK(PhoneShell::isPhoneSize({914, 411}));  // ... landscape
    CHECK(PhoneShell::isPhoneSize({360, 780}));
    CHECK(!PhoneShell::isPhoneSize({800, 1280})); // a tablet
    CHECK(!PhoneShell::isPhoneSize({1024, 768}));
    CHECK(!PhoneShell::isPhoneSize({1920, 1080}));
    CHECK(!PhoneShell::isPhoneSize({}));
}

// The shell by itself: a panel much wider than the phone scrolls inside the sheet, the shell keeps its size
void testShell()
{
    PhoneShell shell;
    auto* top = new QLabel("top");
    auto* viewer = new QLabel("viewer");
    auto* timeline = new QLabel("timeline");
    shell.setTopBar(top);
    shell.setViewer(viewer);
    shell.setTimeline(timeline);
    auto* wide = new QLabel("wide panel");
    wide->setMinimumSize(900, 700);
    auto* small = new QLabel("small panel");
    shell.addPanelTool("wide", "▦", "Wide", [wide] { return wide; });
    shell.addPanelTool("small", "✦", "Small", [small] { return small; });
    int left = 0;
    QObject::connect(&shell, &PhoneShell::clipToolsLeft, &shell, [&left] { ++left; });
    shell.resize(411, 914);
    shell.show();
    settle();
    CHECK(!shell.landscape());
    CHECK(shell.size() == QSize(411, 914));
    CHECK(shell.minimumSizeHint().width() <= 411 && shell.minimumSizeHint().height() <= 914);
    shell.openSheet("wide");
    settle();
    CHECK(shell.sheet() == "wide");
    CHECK(shell.sheetFrame()->isVisible());
    CHECK(inside(shell.sheetFrame(), &shell));
    CHECK(shell.minimumSizeHint().width() <= 411);
    std::printf("portrait: sheet %s, viewer %s, timeline %s\n", qPrintable(rect(shell.sheetFrame()->geometry())),
                qPrintable(rect(viewer->geometry())), qPrintable(rect(timeline->geometry())));
    // Over the timeline, the viewer stays in sight
    CHECK(shell.sheetFrame()->geometry().top() >= viewer->geometry().center().y());
    CHECK(shell.toolButton("wide")->isChecked() && !shell.toolButton("small")->isChecked());
    // Another tool: the one before closes (one sheet at a time)
    QTest::mouseClick(shell.toolButton("small"), Qt::LeftButton);
    settle();
    CHECK(shell.sheet() == "small");
    CHECK(!wide->isVisibleTo(&shell) && small->isVisibleTo(&shell));
    CHECK(!shell.toolButton("wide")->isChecked() && shell.toolButton("small")->isChecked());
    // The same tool again closes it
    QTest::mouseClick(shell.toolButton("small"), Qt::LeftButton);
    settle();
    CHECK(shell.sheet().isEmpty() && !shell.sheetFrame()->isVisible());
    // Back: the sheet, then the clip tools, then nothing
    shell.openSheet("wide");
    shell.setClipToolsShown(true);
    CHECK(shell.back() && shell.sheet().isEmpty() && shell.clipToolsShown());
    CHECK(shell.back() && left == 1 && !shell.clipToolsShown());
    CHECK(!shell.back());
    // Dragging the sheet's header down far closes it
    shell.openSheet("small");
    settle();
    QWidget* header = shell.sheetFrame()->findChild<QWidget*>("PhoneSheetHeader");
    if (CHECK(header)) {
        const QPoint at = header->rect().center();
        QTest::mousePress(header, Qt::LeftButton, {}, at);
        QTest::mouseMove(header, at + QPoint(0, 200));
        QTest::mouseMove(header, at + QPoint(0, 380));
        QTest::mouseRelease(header, Qt::LeftButton, {}, at + QPoint(0, 380));
        settle();
        CHECK(shell.sheet().isEmpty());
    }
    // Landscape: the tools at the left, the panel beside the viewer
    shell.resize(914, 411);
    shell.openSheet("wide");
    settle();
    CHECK(shell.landscape());
    CHECK(shell.size() == QSize(914, 411));
    CHECK(inside(shell.sheetFrame(), &shell));
    std::printf("landscape: sheet %s, viewer %s, timeline %s\n", qPrintable(rect(shell.sheetFrame()->geometry())),
                qPrintable(rect(viewer->geometry())), qPrintable(rect(timeline->geometry())));
    CHECK(shell.sheetFrame()->geometry().left() >= viewer->geometry().right());
    CHECK(timeline->geometry().top() > viewer->geometry().bottom());
    CHECK(shell.minimumSizeHint().width() <= 914 && shell.minimumSizeHint().height() <= 411);
}

// Every visible widget of the window's layout inside the window (scroll areas' contents only inside their viewport)
int outside(QWidget* window)
{
    int count = 0;
    for (QWidget* w : window->findChildren<QWidget*>()) {
        if (!w->isVisible() || w->isWindow()) continue;
        bool scrolled = false;
        for (QWidget* p = w->parentWidget(); p && p != window; p = p->parentWidget())
            if (qobject_cast<QAbstractScrollArea*>(p) && w != p) scrolled = true;
        if (scrolled) continue;
        if (!inside(w, window)) {
            if (count < 8)
                std::printf("  outside: %s %s at %s\n", w->metaObject()->className(), qPrintable(w->objectName()),
                            qPrintable(rect(QRect(w->mapTo(window, QPoint(0, 0)), w->size()))));
            ++count;
        }
    }
    return count;
}

void testWindow(Engine& engine, const QSize& size)
{
    std::printf("--- window %dx%d\n", size.width(), size.height());
    MainWindow w(&engine);
    w.disableAutosave();
    CHECK(!w.phoneShell()); // the offscreen screen (800x800) is no phone
    w.enablePhoneLayout();
    PhoneShell* shell = w.phoneShell();
    if (!CHECK(shell)) return;
    w.resize(size);
    w.show();
    settle();
    CHECK(!w.menuBar()->isVisible());
    const auto fits = [&](const char* what) {
        // The layout's own minimum (the shell caps what it reports at the window's shorter side)
        const QSize least = w.minimumSizeHint().expandedTo(shell->layout()->minimumSize());
        const bool ok = w.size() == size && least.width() <= size.width() && least.height() <= size.height()
                        && outside(&w) == 0;
        if (!ok) { // what holds it: the widest/tallest minimums under the shell
            std::function<void(QWidget*, int)> dump = [&](QWidget* p, int depth) {
                for (QWidget* c : p->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                    if (!c->isVisible() || qobject_cast<QAbstractScrollArea*>(p)) continue;
                    const QSize m = c->minimumSizeHint().expandedTo(c->minimumSize());
                    if (m.width() > size.width() * 3 / 4 || m.height() > size.height() / 2) {
                        std::printf("%*s%s %s min %dx%d\n", depth * 2, "", c->metaObject()->className(),
                                    qPrintable(c->objectName()), m.width(), m.height());
                        if (depth < 8) dump(c, depth + 1);
                    }
                }
            };
            dump(shell, 1);
        }
        Check::report(ok, what, __FILE__, __LINE__,
                      QString("window %1x%2, minimum %3x%4").arg(w.width()).arg(w.height()).arg(least.width())
                          .arg(least.height()));
    };
    fits("phone layout fits the window");
    CHECK(shell->landscape() == (size.width() > size.height()));
    // The top bar: a row above the viewer in portrait; in landscape the head of the rail (no bar across the top)
    {
        QWidget* menu = shell->findChild<QWidget*>("phoneMenu");
        QWidget* undo = shell->findChild<QWidget*>("phoneUndo");
        QWidget* redo = shell->findChild<QWidget*>("phoneRedo");
        QWidget* viewer = shell->viewer();
        if (CHECK(menu && undo && redo)) {
            const auto at = [&](QWidget* x) { return QRect(x->mapTo(&w, QPoint(0, 0)), x->size()); };
            CHECK(inside(menu, &w) && inside(undo, &w) && inside(redo, &w));
            if (shell->landscape()) {
                Check::report(at(undo).right() < at(viewer).left() && at(viewer).top() < 4, "landscape: bar in the rail",
                              __FILE__, __LINE__, QString("undo %1, viewer %2").arg(rect(at(undo)), rect(at(viewer))));
                QWidget* rail = shell->toolButton("media");
                CHECK(at(undo).bottom() < at(rail).top()); // above the tools
                // One surface with the rail (and the cutout's strip at the screen's edge): the window color
                CHECK(menu->parentWidget()->styleSheet().contains(Theme::window.name()));
            } else {
                CHECK(at(menu).center().y() == at(undo).center().y()); // one row
                CHECK(at(undo).bottom() < at(viewer).top());
            }
        }
    }
    // Landscape on a phone's full height (status bar hidden): the timeline shows its first tracks whole (V2, V1, A1)
    if (shell->landscape() && size.height() >= 411) {
        TimelineView* view = w.findChild<TimelineView*>();
        if (CHECK(view)) {
            const QRect v1 = view->trackArea({TrackKind::Video, 0});
            const QRect a1 = view->trackArea({TrackKind::Audio, 0});
            Check::report(v1.height() == view->view().videoTrackHeight && a1.height() == view->view().audioTrackHeight,
                          "landscape: V1 and A1 in sight", __FILE__, __LINE__,
                          QString("view %1x%2, V1 %3, A1 %4").arg(view->width()).arg(view->height()).arg(rect(v1), rect(a1)));
        }
    }
    // The Media Pool in portrait: two tiles side by side (smaller tiles beside the bin list)
    if (!shell->landscape()) {
        shell->openSheet("media");
        settle();
        auto* list = shell->panel("media")->findChild<QListWidget*>("pool_list");
        if (CHECK(list) && CHECK(list->count() >= 2)) {
            const QRect a = list->visualItemRect(list->item(0)), b = list->visualItemRect(list->item(1));
            Check::report(a.top() == b.top() && b.left() > a.left(), "media: two columns", __FILE__, __LINE__,
                          QString("%1 / %2 in %3").arg(rect(a), rect(b), rect(list->viewport()->rect())));
            // The generators (text, shape) after the material: the last two tiles
            const auto data = [&](int i) { return list->item(i)->data(Qt::UserRole).toString(); };
            const int n = list->count();
            Check::report(n >= 3 && data(0) != MediaPool::TitleItem && data(n - 2) == MediaPool::TitleItem
                              && data(n - 1) == MediaPool::ShapeItem,
                          "media: generators last", __FILE__, __LINE__,
                          QString("first %1, last %2").arg(data(0), data(n - 1)));
        }
        shell->closeSheet();
    }
    if (!shell->landscape()) { // more tools than room: the last one in sight is cut about in half (a swipe hint)
        QToolButton* media = shell->toolButton("media");
        auto* scroll = qobject_cast<QAbstractScrollArea*>(media->parentWidget()->parentWidget()->parentWidget()->parentWidget());
        if (CHECK(scroll)) {
            const int edge = scroll->viewport()->width();
            double cut = -1;
            for (const char* id : {"media", "effects", "text", "mixer", "color", "inspector", "transcript", "ai", "export"}) {
                QToolButton* b = shell->toolButton(id);
                const int x = b->mapTo(scroll->viewport(), QPoint(0, 0)).x();
                if (x < edge && x + b->width() > edge) cut = double(edge - x) / b->width();
            }
            Check::report(cut > 0.25 && cut < 0.75, "last tool in sight cut in half", __FILE__, __LINE__,
                          QString("visible share %1").arg(cut));
        }
    }
    const char* tools[] = {"media", "effects", "mixer", "color", "inspector", "transcript", "ai", "export"};
    for (const char* id : tools) {
        shell->openSheet(id);
        settle();
        CHECK(shell->sheet() == id);
        int shown = 0;
        for (QScrollArea* s : shell->sheetFrame()->findChildren<QScrollArea*>(QString(), Qt::FindDirectChildrenOnly))
            if (s->isVisible()) ++shown;
        Check::report(shown == 1, "one panel in the sheet", __FILE__, __LINE__, QString("%1: %2").arg(id).arg(shown));
        fits(id);
        if (QString(id) == "inspector") // its tabs' two lines whole (cut at the bottom with the keyboard up)
            for (QToolButton* b : shell->panel("inspector")->findChildren<QToolButton*>())
                if (b->isVisibleTo(shell) && b->text().contains('\n'))
                    Check::report(b->minimumHeight() >= b->sizeHint().height(), "inspector tab not cut", __FILE__,
                                  __LINE__, QString("%1: min %2 < %3").arg(b->text()).arg(b->minimumHeight())
                                                .arg(b->sizeHint().height()));
        if (QString(id) == "export") { // the queue with its "Render all" in the same sheet (else nothing renders)
            auto* queue = shell->panel("export")->findChild<RenderQueuePanel*>();
            CHECK(queue && queue->isVisibleTo(shell));
            // "Render all" in sight without dragging the sheet up (the sheet opens tall for it)
            if (queue && !shell->landscape()) {
                QPushButton* render = nullptr;
                for (QPushButton* b : queue->findChildren<QPushButton*>())
                    if (b->isVisibleTo(queue) && (!render || b->y() > render->y())) render = b; // the bottom one
                auto* scroll = shell->sheetFrame()->findChild<QScrollArea*>("phonePanel_export");
                if (CHECK(render && scroll)) {
                    const QRect r(render->mapTo(scroll->viewport(), QPoint(0, 0)), render->size());
                    Check::report(r.top() >= 0 && r.bottom() < scroll->viewport()->height(), "export: Render all in sight", __FILE__,
                                  __LINE__, QString("%1 in %2").arg(rect(r), rect(scroll->viewport()->rect())));
                }
            }
        }
        if (QString(id) == "color") // the palette tabs at their full width ("W…s" in touch mode on Android)
            for (int i = 0; i < 4; ++i)
                if (auto* b = shell->panel("color")->findChild<QToolButton*>(QString("color_palette_%1").arg(i)); CHECK(b))
                    Check::report(b->width() >= b->sizeHint().width(), "palette tab not cut", __FILE__, __LINE__,
                                  QString("%1: %2 < %3").arg(b->text()).arg(b->width()).arg(b->sizeHint().width()));
    }
    // A desktop toggle (menu, key) opens its panel as the sheet, the one before closes
    shell->closeSheet();
    if (QAction* mixer = InputBindings::instance().action("toggle_mixer"); CHECK(mixer)) {
        shell->openSheet("media");
        mixer->trigger();
        settle();
        CHECK(shell->sheet() == "mixer");
        mixer->trigger();
        settle();
        CHECK(shell->sheet().isEmpty());
    }
    // Pages: Media, Color and Export are sheets, Edit closes it; the window stays the phone layout
    w.showPage(MainWindow::Page::Color);
    settle();
    CHECK(shell->sheet() == "color");
    w.showPage(MainWindow::Page::Deliver);
    CHECK(shell->sheet() == "export");
    w.showPage(MainWindow::Page::Edit);
    CHECK(shell->sheet().isEmpty());
    CHECK(w.currentPage() == MainWindow::Page::Edit);
    // A selected clip: the clip tools; the back key closes the sheet, then leaves the clip tools (selection cleared)
    w.editor()->addTitle(0, -1, 50, TitleKind::Text);
    settle();
    CHECK(!w.liveSelection()->isEmpty());
    CHECK(shell->clipToolsShown());
    CHECK(shell->toolButton("split")->isVisible());
    // Every clip tool can be reached: inside the bar's scrolled box (the bar scrolls, nothing lies past its end)
    for (const char* id : {"split", "volume", "speed", "clipEffects", "clipColor", "delete"}) {
        QToolButton* b = shell->toolButton(id);
        QWidget* box = b ? b->parentWidget()->parentWidget() : nullptr; // clip tools -> the bar's box
        const bool ok = b && box && box->rect().contains(QRect(b->mapTo(box, QPoint(0, 0)), b->size()));
        if (b) // the label whole ("Lauts…" on the phone)
            Check::report(b->width() >= b->sizeHint().width(), "clip tool label not cut", __FILE__, __LINE__,
                          QString("%1: %2 < %3").arg(b->text()).arg(b->width()).arg(b->sizeHint().width()));
        Check::report(ok, "clip tool inside the bar's box", __FILE__, __LINE__,
                      QString("%1 at %2 in %3").arg(id).arg(b ? rect(QRect(b->mapTo(box, QPoint(0, 0)), b->size())) : "-")
                          .arg(box ? rect(box->rect()) : "-"));
    }
    QTest::mouseClick(shell->toolButton("volume"), Qt::LeftButton);
    settle();
    CHECK(shell->sheet() == "inspector");
    auto* inspector = qobject_cast<Inspector*>(shell->panel("inspector"));
    CHECK(inspector && inspector->currentPage() != 1); // a title has no sound: no Audio tab
    fits("clip selected, Inspector open");
    CHECK(w.backStep());
    CHECK(shell->sheet().isEmpty() && shell->clipToolsShown());
    CHECK(w.backStep());
    CHECK(!shell->clipToolsShown() && w.liveSelection()->isEmpty());
    // Undo: the title goes again
    if (auto* undo = shell->findChild<QToolButton*>("phoneUndo"); CHECK(undo && undo->isEnabled())) {
        const int before = w.editor()->project()->timeline().video.isEmpty()
                               ? 0
                               : int(w.editor()->project()->timeline().video.first().clips.size());
        QTest::mouseClick(undo, Qt::LeftButton);
        settle();
        const int after = w.editor()->project()->timeline().video.isEmpty()
                              ? 0
                              : int(w.editor()->project()->timeline().video.first().clips.size());
        CHECK(after < before || before == 0);
    }
    // Volume on a clip with sound: the Inspector's Audio tab, at its top (the Volume section first)
    shell->closeSheet();
    if (inspector) {
        int id = 0;
        w.editor()->project()->edit("audio clip", [&](Timeline& tl) {
            if (tl.audio.isEmpty()) tl.audio.resize(1);
            Clip c;
            c.id = id = w.editor()->project()->newClipId();
            c.mediaPath = QStringLiteral("phone-test-missing.wav"); // offline media: the Inspector needs only the clip
            c.start = 0;
            c.in = 0;
            c.out = 49;
            tl.audio[0].clips << c;
        });
        w.editor()->selection()->set({id});
        settle();
        CHECK(shell->clipToolsShown());
        QTest::mouseClick(shell->toolButton("volume"), Qt::LeftButton);
        settle();
        CHECK(shell->sheet() == "inspector");
        CHECK(inspector->currentPage() == 1);
        CHECK(shell->toolButton("volume")->isChecked());
        fits("volume: Audio tab");
        shell->openSheet("inspector"); // the same panel opened another way (the Text tool): Volume is not the one
        CHECK(!shell->toolButton("volume")->isChecked());
        QTest::mouseClick(shell->toolButton("volume"), Qt::LeftButton); // and a tap makes it the one, not closes
        CHECK(shell->sheet() == "inspector" && shell->toolButton("volume")->isChecked());
        QTest::mouseClick(shell->toolButton("volume"), Qt::LeftButton); // again: closes
        settle();
        CHECK(shell->sheet().isEmpty());
        w.editor()->selection()->set({});
        settle();
    }
    // Background work: in the phone's top bar, compact (percent), inside the window; a tap shows the details
    if (auto* jobs = w.findChild<BackgroundJobs*>("background_jobs"); CHECK(jobs)) {
        CHECK(jobs->compact());
        QWidget* top = jobs->parentWidget();
        CHECK(top && top->objectName() == "TopBar" && shell->isAncestorOf(top));
        BackgroundJobs::Job job;
        job.text = "Proxy 2/5 – a_rather_long_file_name_from_a_phone_camera.mp4 – 34 %";
        job.fraction = 0.34;
        bool cancelled = false;
        job.cancel = [&cancelled] { cancelled = true; };
        jobs->setJob("proxies", job);
        settle();
        CHECK(jobs->isVisible());
        CHECK(jobs->text() == "34 %");
        CHECK(inside(jobs, &w));
        fits("background job shown");
        QString details;
        QObject::connect(jobs, &BackgroundJobs::detailsRequested, jobs, [&details](const QString& t) { details = t; });
        QTest::mouseClick(jobs, Qt::LeftButton, {}, QPoint(2, jobs->height() / 2));
        CHECK(details.contains("a_rather_long_file_name"));
        if (auto* cancel = jobs->findChild<QToolButton*>("background_jobs_cancel"); CHECK(cancel && cancel->isVisible())) {
            QTest::mouseClick(cancel, Qt::LeftButton);
            CHECK(cancelled);
        }
        jobs->removeJob("proxies");
        settle();
        CHECK(!jobs->isVisible());
    }
    // Turned while running (Android follows the device): the other orientation, still inside
    const QSize turned(size.height(), size.width());
    w.resize(turned);
    shell->openSheet("inspector");
    settle();
    CHECK(shell->landscape() == (turned.width() > turned.height()));
    Check::report(w.size() == turned && outside(&w) == 0, "turned: inside", __FILE__, __LINE__,
                  QString("window %1x%2, minimum %3x%4").arg(w.width()).arg(w.height())
                      .arg(w.minimumSizeHint().width()).arg(w.minimumSizeHint().height()));
    const QSize turnedLeast = shell->layout()->minimumSize();
    Check::report(turnedLeast.width() <= turned.width() && turnedLeast.height() <= turned.height(), "turned: minimum",
                  __FILE__, __LINE__, QString("%1x%2").arg(turnedLeast.width()).arg(turnedLeast.height()));
}

// The setting ui/phoneLayout (Settings › Oberfläche › Handy-Layout): on/off win over the screen's size
void testSetting()
{
    SettingsDialog::setSavedPhoneLayout({}); // (a run stopped before its end may have left one)
    CHECK(SettingsDialog::savedPhoneLayout().isEmpty());
    CHECK(!MainWindow::phoneLayoutWanted()); // automatic: the offscreen screen is no phone
    SettingsDialog::setSavedPhoneLayout("phone");
    CHECK(MainWindow::phoneLayoutWanted());
    {
        SettingsDialog dlg{AiSettings{}};
        auto* combo = dlg.findChild<QComboBox*>("settings_phone_layout");
        if (CHECK(combo)) {
            CHECK(combo->count() == 4);
            CHECK(dlg.phoneLayout() == "phone");
            combo->setCurrentIndex(combo->findData(QStringLiteral("desktop")));
            CHECK(dlg.phoneLayout() == "desktop");
            combo->setCurrentIndex(0);
            CHECK(dlg.phoneLayout().isEmpty());
        }
    }
    SettingsDialog::setSavedPhoneLayout("desktop");
    CHECK(!MainWindow::phoneLayoutWanted());
    CHECK(SettingsDialog::savedPhoneLayout() == "desktop");
    SettingsDialog::setSavedPhoneLayout({});
    QSettings().setValue("ui/phoneLayout", "on"); // the prototype's setting still counts
    CHECK(SettingsDialog::savedPhoneLayout() == "phone");
    SettingsDialog::setSavedPhoneLayout({});
    CHECK(SettingsDialog::savedPhoneLayout().isEmpty());
    CHECK(!MainWindow::phoneLayoutWanted());
}

// Android ends apps in the background: the backup comes back without the question (SCHNEIDI_SILENT_RESTORE=1 here)
void testSilentRestore(Engine& engine)
{
    qputenv("SCHNEIDI_SILENT_RESTORE", "1");
    CHECK(MainWindow::restoresWithoutAsking());
    {
        MainWindow w(&engine); // autosave on (test settings and paths: never the user's backup)
        w.editor()->addTitle(0, -1, 50, TitleKind::Text);
        CHECK(w.editor()->project()->isModified());
        w.autosave();
    } // gone without closing (as if killed): the backup stays
    MainWindow w2(&engine);
    bool asked = false;
    QTimer guard; // a question would block: answer it with No and note it
    QObject::connect(&guard, &QTimer::timeout, &guard, [&asked] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            asked = true;
            box->reject();
        }
    });
    guard.start(50);
    const bool restored = w2.offerAutosaveRestore();
    guard.stop();
    CHECK(restored && !asked);
    int titles = 0;
    for (const Track& t : w2.editor()->project()->timeline().video) titles += int(t.clips.size());
    CHECK(titles >= 1);
    w2.disableAutosave();
    qputenv("SCHNEIDI_SILENT_RESTORE", "0");
    CHECK(!MainWindow::restoresWithoutAsking());
    qunsetenv("SCHNEIDI_SILENT_RESTORE");
}

// The tool bar under a finger (found on the A51): past its left end it stays at 0, a slanted swipe only scrolls
// sideways, and no swipe opens a tool
void testToolBarSwipe(Engine& engine)
{
    QPointingDevice* finger = QTest::createTouchDevice();
    MainWindow w(&engine);
    w.disableAutosave();
    w.enablePhoneLayout();
    PhoneShell* shell = w.phoneShell();
    w.resize(411, 590); // (inside the offscreen screen: touch points outside it get lost)
    w.show();
    settle();
    QWidget* vp = shell->toolButton("media")->parentWidget()->parentWidget()->parentWidget();
    auto* area = qobject_cast<QAbstractScrollArea*>(vp->parentWidget());
    if (!CHECK(area)) return;
    QScrollBar* h = area->horizontalScrollBar();
    QScrollBar* v = area->verticalScrollBar();
    CHECK(h->maximum() > 0);
    const int y = vp->height() / 2;
    const auto swipe = [&](QPoint a, QPoint b, int steps) {
        QTest::touchEvent(vp, finger).press(0, a, vp);
        for (int i = 1; i <= steps; ++i) {
            QTest::touchEvent(vp, finger).move(0, a + (b - a) * i / steps, vp);
            QTest::qWait(8);
        }
        QTest::touchEvent(vp, finger).release(0, b, vp);
    };
    // Past the left end: stays at 0 (no jump to the right), nothing opens
    int most = 0;
    QObject::connect(h, &QScrollBar::valueChanged, h, [&](int value) { most = std::max(most, value); });
    swipe({100, y}, {300, y}, 10);
    QTest::qWait(400);
    Check::report(h->value() == 0 && most == 0, "tool bar: stays at its left end", __FILE__, __LINE__,
                  QString("value %1, at most %2").arg(h->value()).arg(most));
    CHECK(shell->sheet().isEmpty());
    // Slanted: only sideways, the way the finger went (about 150 px), nothing opens
    QTest::qWait(500);
    swipe({300, y}, {150, y - 40}, 10);
    QTest::qWait(600);
    Check::report(h->value() >= 100 && v->value() == 0, "tool bar: slanted swipe scrolls sideways", __FILE__, __LINE__,
                  QString("h %1, v %2").arg(h->value()).arg(v->value()));
    CHECK(shell->sheet().isEmpty());
    // Back to the start and further: 0 again
    QTest::qWait(500);
    swipe({100, y}, {400, y + 30}, 12);
    QTest::qWait(800);
    Check::report(h->value() == 0, "tool bar: back at 0", __FILE__, __LINE__, QString("value %1").arg(h->value()));
    CHECK(shell->sheet().isEmpty());
}

// Android: a dialog shown narrower than its contents grows about its middle (Touch::installDialogFit)
void testDialogFit()
{
    QDialog d;
    auto* lay = new QVBoxLayout(&d);
    auto* box = new QCheckBox("A check box with a long text that needs more room than the dialog was given at first");
    lay->addWidget(box);
    d.setGeometry(100, 100, 200, 120); // narrower than the text
    d.setMinimumWidth(0);
    d.show();
    const int middle = d.geometry().center().x();
    settle();
    Check::report(d.width() >= box->sizeHint().width(), "dialog grew to its contents", __FILE__, __LINE__,
                  QString("%1 < %2").arg(d.width()).arg(box->sizeHint().width()));
    Check::report(std::abs(d.geometry().center().x() - middle) <= 2 || d.geometry().left() == 0, "grown about its middle",
                  __FILE__, __LINE__, QString("center %1 -> %2").arg(middle).arg(d.geometry().center().x()));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("phone-layout");
    qunsetenv("SCHNEIDI_LAYOUT"); // (check.h pins the desktop layout; this test is about the others)
    testSizes();
    testShell();
    if (qEnvironmentVariableIsEmpty("SCHNEIDI_PHONE_LAYOUT")) testSetting();
    SettingsDialog::setSavedPhoneLayout({});
    Touch::installDialogFit(&app); // (main.cpp: Android only)
    testDialogFit();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testSilentRestore(engine);
    Touch::installKineticScrolling(&app); // (main.cpp: every scroll area takes a finger)
    testToolBarSwipe(engine);
    testWindow(engine, {411, 914});
    testWindow(engine, {914, 411});
    // As on a phone: touch mode (larger controls) and the app's style sheet with it
    Touch::setModeActive(true);
    Theme::apply(app);
    testWindow(engine, {411, 914});
    testWindow(engine, {914, 411});
    testWindow(engine, {360, 740}); // a small phone (turned: 740x360)
    testWindow(engine, {914, 380}); // landscape below the status bar
    return Check::result();
}
