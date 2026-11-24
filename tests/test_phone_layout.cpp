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
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Types.h"
#include "engine/Engine.h"
#include "ui/BackgroundJobs.h"
#include "ui/Inspector.h"
#include "ui/ShapeOverlay.h"
#include "ui/FrameView.h"
#include "ui/Viewer.h"
#include "ui/MediaGallery.h"
#include "ui/MediaPool.h"
#include "ui/RenderQueuePanel.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"
#include "ui/phone/PhoneShell.h"
#include "ui/touch/Touch.h"
#include "ui/touch/ScrollIndicators.h"
#include "app/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QFontInfo>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QScroller>
#include <QSettings>
#include <QMessageBox>
#include <QTimer>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMenuBar>
#include <QScrollArea>
#include <QTest>
#include <QUndoStack>
#include <QToolButton>
#include <functional>
#include <memory>

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
    CHECK(w.layoutForm() != MainWindow::LayoutForm::Phone); // the offscreen screen (800 px) is no phone
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
    if (!shell->landscape()) // the A51 finding: the play button in the window's middle (also in touch mode)
        if (auto* play = w.findChild<QToolButton*>("viewer_play"); CHECK(play)) {
            const int mid = play->mapTo(&w, play->rect().center()).x();
            Check::report(std::abs(mid - w.width() / 2) <= 3, "play button in the middle (portrait)", __FILE__, __LINE__,
                          QString("centre %1 of %2%3").arg(mid).arg(w.width()).arg(Touch::modeActive() ? ", touch" : ""));
        }
    if (shell->landscape()) { // item 10: a picture worth the name, the rail's tools with their names
        if (auto* screen = w.findChild<QWidget*>("viewer_screen"); CHECK(screen))
            Check::report(screen->height() >= size.height() * 3 / 10, "landscape picture not tiny", __FILE__, __LINE__,
                          QString("%1 of %2").arg(screen->height()).arg(size.height()));
        if (QToolButton* media = shell->toolButton("media"); CHECK(media))
            CHECK(media->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && media->width() >= media->sizeHint().width());
        CHECK(!w.findChild<TimelinePanel*>()->findChild<QScrollArea*>("timelineBarScroll")->isVisible());
    }
    if (const QString dir = qEnvironmentVariable("SCHNEIDI_TEST_SHOTS"); !dir.isEmpty())
        w.grab().save(QString("%1/window-%2x%3%4.png").arg(dir).arg(size.width()).arg(size.height())
                          .arg(Touch::modeActive() ? "-touch" : ""));
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
        if (!shell->landscape() && QString(id) != "export") { // item 9: V1 stays in sight above a tool's sheet
            TimelineView* tv = w.findChild<TimelineView*>();
            const int v1 = tv->mapTo(&w, QPoint(0, tv->mainTrackBottom())).y();
            const int top = shell->sheetFrame()->mapTo(&w, QPoint(0, 0)).y();
            Check::report(top >= v1 || shell->sheetShare() <= 0.31, "timeline's V1 above the sheet", __FILE__, __LINE__,
                          QString("%1: sheet top %2, V1 bottom %3").arg(id).arg(top).arg(v1));
        }
        if (QString(id) == "ai") { // item 11: no key field at once; a button first (or why it cannot work here)
            auto* chat = shell->panel("ai");
            auto* key = chat->findChild<QWidget*>("ai_setup_key");
            auto* open = chat->findChild<QPushButton*>("ai_setup_open");
            auto* intro = chat->findChild<QWidget*>("ai_setup_intro");
            if (CHECK(key && open && intro) && intro->isVisibleTo(chat)) {
                CHECK(!key->isVisibleTo(chat));
                if (auto* header = chat->findChild<QWidget*>("ai_chat_header")) CHECK(!header->isVisibleTo(chat));
                if (open->isVisibleTo(chat)) {
                    QTest::mouseClick(open, Qt::LeftButton);
                    CHECK(key->isVisibleTo(chat) && !intro->isVisibleTo(chat));
                }
            }
            for (QLabel* l : chat->findChildren<QLabel*>("PanelTitle"))
                Check::report(!l->isVisibleTo(chat), "AI sheet without a second title", __FILE__, __LINE__, l->text());
        }
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
    // Item 7: the selected text title has its box on the picture (move, size)
    if (auto* ov = w.findChild<ShapeOverlay*>(); CHECK(ov)) CHECK(ov->textTitles());
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
    if (Touch::modeActive() && inspector) { // item 8: no finger-wide keyframe arrows beside every row, a "B" with room
        int arrows = 0;
        for (QToolButton* b : inspector->findChildren<QToolButton*>())
            if ((b->text() == QString("◀") || b->text() == QString("▶")) && b->isVisibleTo(inspector)) ++arrows;
        Check::report(arrows == 0, "title inspector without keyframe arrows (touch)", __FILE__, __LINE__,
                      QString::number(arrows));
        for (QToolButton* b : inspector->findChildren<QToolButton*>())
            if (b->text() == "B" && b->isVisibleTo(inspector))
                Check::report(b->height() >= 28, "brand button finger-high", __FILE__, __LINE__, QString::number(b->height()));
    }
    if (const QString dir = qEnvironmentVariable("SCHNEIDI_TEST_SHOTS"); !dir.isEmpty())
        w.grab().save(QString("%1/title-inspector-%2x%3%4.png").arg(dir).arg(size.width()).arg(size.height())
                          .arg(Touch::modeActive() ? "-touch" : ""));
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
    auto w2p = std::make_unique<MainWindow>(&engine);
    MainWindow& w2 = *w2p;
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
    // Review B4: that restore never reached a running window (as if the restored project crashed the start): the
    // next start asks instead of restoring again
    w2.editor()->addTitle(100, -1, 50, TitleKind::Text);
    w2.autosave();
    w2p.reset(); // (killed: its backup is an orphan the next window finds)
    {
        MainWindow w3(&engine);
        asked = false;
        guard.start(50);
        w3.offerAutosaveRestore();
        guard.stop();
        CHECK(asked);
        w3.disableAutosave();
    }
    QSettings().remove(MainWindow::kRestoreStartKey);
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
    // Every label whole (items as wide as their labels), no frames
    for (QToolButton* b : vp->findChildren<QToolButton*>())
        if (b->isVisibleTo(vp))
            Check::report(b->width() >= b->sizeHint().width(), "tool bar: label whole", __FILE__, __LINE__,
                          QString("%1: %2 < %3").arg(b->text()).arg(b->width()).arg(b->sizeHint().width()));
    // A flick, then a finger on the gliding bar: it stops, no tool opens
    QTest::qWait(500);
    h->setValue(0);
    QTest::touchEvent(vp, finger).press(0, {300, y}, vp);
    for (int i = 1; i <= 4; ++i) {
        QTest::touchEvent(vp, finger).move(0, QPoint(300 - 20 * i, y), vp);
        QTest::qWait(10);
    }
    QTest::touchEvent(vp, finger).release(0, {220, y}, vp);
    QTest::qWait(30);
    QTest::touchEvent(vp, finger).press(0, {200, y}, vp);
    QTest::touchEvent(vp, finger).release(0, {200, y}, vp);
    QTest::qWait(300);
    CHECK(shell->sheet().isEmpty());
    // The clip bar's back arrow is outside the scrolling part: it stays where it is
    w.editor()->addTitle(0, -1, 50, TitleKind::Text);
    settle();
    QToolButton* back = shell->toolButton("back");
    if (CHECK(back && back->isVisible())) {
        CHECK(!vp->isAncestorOf(back));
        const QPoint before = back->mapTo(&w, QPoint(0, 0));
        swipe({300, y}, {60, y}, 8);
        QTest::qWait(600);
        CHECK(back->mapTo(&w, QPoint(0, 0)) == before);
    }
}

// A second-long WAV of `seconds` (silence) for the gallery's files
QString writeWav(const QString& path, int seconds)
{
    const quint32 rate = 8000, bytes = rate * 2 * quint32(seconds);
    QByteArray d;
    auto u32 = [&d](quint32 v) { d.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&d](quint16 v) { d.append(reinterpret_cast<const char*>(&v), 2); };
    d += "RIFF";
    u32(36 + bytes);
    d += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    d += "data";
    u32(bytes);
    d.append(QByteArray(int(bytes), '\0'));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(d);
    return path;
}

// The phone's way to add material: the empty timeline's big button, schneidi's gallery (newest first, by day, picked
// in order with numbers), the picks in that order into the pool and onto the timeline
void testGallery(Engine& engine)
{
    QTemporaryDir dir;
    const QDateTime now = QDateTime::currentDateTime();
    QVector<DeviceMediaItem> items;
    const auto item = [&](const char* name, int seconds, QDateTime date, bool video) {
        DeviceMediaItem m;
        m.path = writeWav(dir.filePath(name), seconds);
        m.name = name;
        m.date = date;
        m.durationMs = seconds * 1000;
        m.video = video;
        items << m;
    };
    item("one.wav", 1, now, true);
    item("two.wav", 2, now.addSecs(-60), true);
    item("three.wav", 3, now.addDays(-1), true);
    item("photo.wav", 1, now.addDays(-3), false); // (shown as a photo)
    DeviceMedia::setTestItems(items);
    {
        MediaGallery g;
        g.resize(411, 700);
        g.show();
        settle();
        CHECK(g.tiles().size() == 4);
        CHECK(!g.addButton()->isEnabled());
        // Picked: three, then one; the numbers follow the order, unpicking renumbers
        const auto tileOf = [&](int i) -> GalleryTile* {
            for (GalleryTile* t : g.tiles())
                if (t->item() == i) return t;
            return nullptr;
        };
        QTest::mouseClick(tileOf(2), Qt::LeftButton);
        QTest::mouseClick(tileOf(0), Qt::LeftButton);
        QTest::mouseClick(tileOf(1), Qt::LeftButton);
        CHECK(tileOf(2)->number() == 1 && tileOf(0)->number() == 2 && tileOf(1)->number() == 3);
        QTest::mouseClick(tileOf(0), Qt::LeftButton); // unpicked: two moves up
        CHECK(tileOf(0)->number() == 0 && tileOf(1)->number() == 2);
        CHECK(g.addButton()->isEnabled() && g.addButton()->text().contains("2"));
        CHECK(g.chosenPaths() == QStringList({items[2].path, items[1].path}));
        g.setFilter(MediaGallery::Filter::Photos);
        CHECK(g.tiles().size() == 1);
        g.setFilter(MediaGallery::Filter::Videos);
        CHECK(g.tiles().size() == 3);
        CHECK(tileOf(2)->number() == 1); // (the picks stay through the filter)
    }
    {
        // Review B3: a phone full of videos - only the tiles in sight (and a screen below) ask for their picture,
        // scrolling asks for more
        QVector<DeviceMediaItem> many;
        for (int i = 0; i < 400; ++i) {
            DeviceMediaItem m = items[0];
            m.path = QString("%1/many-%2.wav").arg(dir.path()).arg(i);
            m.date = now.addSecs(-i);
            many << m;
        }
        DeviceMedia::setTestItems(many);
        MediaGallery g;
        g.resize(411, 700);
        g.show();
        settle();
        const int first = g.thumbnailsRequested();
        Check::report(first > 0 && first < 100, "gallery: only tiles in sight load", __FILE__, __LINE__,
                      QString::number(first));
        // Tiles a page at a time (a widget per item of a large library took seconds); scrolling to the end brings
        // the next page, a pick on the first page stays
        Check::report(g.tiles().size() == MediaGallery::kPage, "gallery: first page of tiles only", __FILE__, __LINE__,
                      QString::number(g.tiles().size()));
        g.tiles().first()->tapped(g.tiles().first()->item());
        auto* scroll = g.findChild<QScrollArea*>();
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        settle();
        CHECK(g.thumbnailsRequested() > first && g.thumbnailsRequested() < 400);
        Check::report(g.tiles().size() > MediaGallery::kPage, "gallery: scrolled to the end, the next page", __FILE__,
                      __LINE__, QString::number(g.tiles().size()));
        for (int i = 0; i < 10 && g.tiles().size() < 400; ++i) {
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            settle();
        }
        CHECK(g.tiles().size() == 400);
        CHECK(g.chosenPaths() == QStringList{many[0].path} && g.tiles().first()->number() == 1);
        DeviceMedia::setTestItems(items);
    }
    // The flow: the big button over the empty timeline, the gallery, two picked, added in that order
    MainWindow w(&engine);
    w.disableAutosave();
    w.enablePhoneLayout();
    w.resize(411, 590);
    w.show();
    settle();
    QAbstractButton* add = w.addMediaButton();
    if (!CHECK(add && add->isVisible())) return;
    CHECK(inside(add, &w));
    CHECK(add->width() > 2 * add->height()); // clip-shaped on V1 while the timeline is empty
    bool shown = false;
    QTimer::singleShot(300, [&] {
        auto* g = qobject_cast<MediaGallery*>(QApplication::activeModalWidget());
        if (!g) return;
        shown = true;
        for (int want : {2, 0})
            for (GalleryTile* t : g->tiles())
                if (t->item() == want) QTest::mouseClick(t, Qt::LeftButton);
        QTest::mouseClick(g->addButton(), Qt::LeftButton);
    });
    QTest::mouseClick(add, Qt::LeftButton);
    QTest::qWait(3000); // (probing)
    settle();
    CHECK(shown);
    const Timeline& tl = w.editor()->project()->timeline();
    QStringList order;
    for (const Track& t : tl.audio)
        for (const Clip& c : t.clips) order << QFileInfo(c.mediaPath).fileName();
    Check::report(order == QStringList({"three.wav", "one.wav"}), "gallery: picks on the timeline in order", __FILE__,
                  __LINE__, order.join(", "));
    // The timeline has clips now: the small "+" square at the lanes' right edge (same gallery)
    CHECK(add->isVisible() && add->width() == add->height() && add->width() <= 48);
    CHECK(add->geometry().right() > w.findChild<TimelineView*>()->width() - 60);
    DeviceMedia::setTestItems({});
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

// The viewer's timecode and play button never change width while the time runs (item 1 of the phone review: on
// Android "monospace" fell back to a proportional font and the transport buttons jittered with every frame)
void testSteadyTransport(Engine& engine)
{
    MainWindow w(&engine);
    w.disableAutosave();
    w.enablePhoneLayout();
    w.resize(411, 914);
    w.show();
    settle();
    auto* tc = w.findChild<QLabel*>("viewer_timecode");
    auto* play = w.findChild<QToolButton*>("viewer_play");
    if (!CHECK(tc && play)) return;
    emit engine.positionChanged(0);
    emit engine.speedChanged(1.0);
    emit engine.speedChanged(0.0);
    settle();
    const int x = play->mapTo(&w, QPoint()).x(), width = tc->width();
    bool steady = true;
    QString moved;
    for (int f : {1, 11, 111, 4444, 18181, 89898, 7, 0}) {
        emit engine.positionChanged(f);
        emit engine.speedChanged(f % 2 ? 1.0 : 0.0);
        QApplication::processEvents();
        tc->parentWidget()->layout()->activate();
        if (play->mapTo(&w, QPoint()).x() != x || tc->width() != width) {
            steady = false;
            moved = QString("frame %1: play x %2 -> %3, timecode %4 -> %5").arg(f).arg(x)
                        .arg(play->mapTo(&w, QPoint()).x()).arg(width).arg(tc->width());
        }
    }
    Check::report(steady, "timecode and play button keep their place while the time runs", __FILE__, __LINE__, moved);
    // The play button in the middle of the window (the A51: the timecode left is narrower than "Fit" + full screen at
    // the right, it sat at 46 %), also with another zoom text and on a narrower phone
    const auto centred = [&](const char* what) {
        tc->parentWidget()->layout()->activate();
        const int mid = play->mapTo(&w, play->rect().center()).x();
        Check::report(std::abs(mid - w.width() / 2) <= 3, "play button in the middle of the phone window", __FILE__,
                      __LINE__, QString("%1: centre %2 of %3").arg(what).arg(mid).arg(w.width()));
    };
    centred("411 wide");
    if (auto* viewer = w.findChild<Viewer*>(); CHECK(viewer)) {
        viewer->setZoom(1.0);
        settle();
        centred("zoom 100 %");
        viewer->setZoom(FrameView::kFit);
    }
    w.resize(360, 740);
    settle();
    centred("360 wide");
    emit engine.positionChanged(89898);
    settle();
    centred("360 wide, long timecode");
    Check::report(tc->font().fixedPitch() || QFontInfo(tc->font()).fixedPitch(), "timecode in a fixed-pitch font",
                  __FILE__, __LINE__, QFontInfo(tc->font()).family());
}

// Item 3 of the phone review: thin scroll indicators that show while the content moves, instead of finger-wide bars
void testScrollIndicators()
{
    QWidget window;
    auto* lay = new QVBoxLayout(&window);
    auto* area = new QScrollArea;
    auto* tall = new QWidget;
    tall->setMinimumSize(200, 2000);
    area->setWidget(tall);
    lay->addWidget(area);
    window.resize(300, 400);
    window.show();
    settle();
    auto* indicators = new ScrollIndicators(&window);
    settle();
    CHECK(area->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff && !area->verticalScrollBar()->isVisible());
    auto* ind = area->findChild<ScrollIndicator*>("scrollIndicatorV");
    if (!CHECK(ind)) return;
    CHECK(!ind->shown() && ind->testAttribute(Qt::WA_TransparentForMouseEvents)); // (hidden: no room, no touch)
    CHECK(ind->width() <= ScrollIndicator::kHit && ind->geometry().right() >= area->viewport()->geometry().right() - 1);
    area->verticalScrollBar()->setValue(300);
    CHECK(ind->shown() && !ind->testAttribute(Qt::WA_TransparentForMouseEvents));
    QTest::qWait(ScrollIndicator::kHideMs + 400);
    CHECK(!ind->shown() && ind->testAttribute(Qt::WA_TransparentForMouseEvents));
    // The area grows (a sheet laid out after the indicator came): the indicator follows to the new right edge
    window.resize(520, 400);
    settle();
    area->verticalScrollBar()->setValue(200);
    settle();
    Check::report(std::abs(ind->geometry().right() - area->viewport()->geometry().right()) <= 1,
                  "indicator at the grown area's edge", __FILE__, __LINE__,
                  QString("%1 vs %2").arg(ind->geometry().right()).arg(area->viewport()->geometry().right()));
    QTest::qWait(ScrollIndicator::kHideMs + 400);
    // Shown, it drags like a bar (a mouse in a narrow window)
    area->verticalScrollBar()->setValue(0);
    const QPoint top(ind->width() / 2, 10);
    QTest::mousePress(ind, Qt::LeftButton, {}, top);
    QTest::mouseMove(ind, top + QPoint(0, 150));
    QTest::mouseRelease(ind, Qt::LeftButton, {}, top + QPoint(0, 150));
    Check::report(area->verticalScrollBar()->value() > 300, "indicator drags the content", __FILE__, __LINE__,
                  QString::number(area->verticalScrollBar()->value()));
    // Touch mode: a shown indicator never takes a press (the finger's next tap at the edge goes to the content)
    Touch::setModeActive(true);
    area->verticalScrollBar()->setValue(50);
    CHECK(ind->shown() && ind->testAttribute(Qt::WA_TransparentForMouseEvents));
    Touch::setModeActive(false);
    // Gone: the scroll bars as they were
    delete indicators;
    CHECK(area->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded && !area->findChild<ScrollIndicator*>());
}

// Item 5 of the phone review: the settings dialog was wider and taller than the screen (OK/Cancel out of sight, the
// page not scrolling): on a phone it fills the window, OK in sight, its pages as narrow as the screen
void testSettingsFit(const QSize& size)
{
    SettingsDialog dlg{AiSettings{}};
    const QRect room(QPoint(0, 0), size);
    dlg.fitScreen(room);
    dlg.show();
    settle();
    Check::report(dlg.size() == room.size() && room.contains(dlg.frameGeometry().topLeft()),
                  "settings dialog fills the phone window", __FILE__, __LINE__, rect(dlg.frameGeometry()));
    if (const QString dir = qEnvironmentVariable("SCHNEIDI_TEST_SHOTS"); !dir.isEmpty()) // (a look at it)
        dlg.grab().save(QString("%1/settings-%2x%3.png").arg(dir).arg(size.width()).arg(size.height()));
    QPushButton* ok = nullptr;
    for (QPushButton* b : dlg.findChildren<QPushButton*>())
        if (b->text().contains("OK", Qt::CaseInsensitive)) ok = b;
    if (CHECK(ok)) CHECK(ok->isVisible() && inside(ok, &dlg));
    for (QScrollArea* area : dlg.findChildren<QScrollArea*>()) {
        if (!area->widget() || area->horizontalScrollBarPolicy() != Qt::ScrollBarAsNeeded) continue;
        const int least = area->widget()->minimumSizeHint().width();
        if (least > size.width()) { // what holds it
            std::function<void(QWidget*, int)> dump = [&](QWidget* p, int depth) {
                for (QWidget* c : p->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                    const int m = std::max(c->minimumSizeHint().width(), c->minimumWidth());
                    if (!c->isHidden() && m > size.width() * 2 / 3) {
                        std::printf("%*s%s %s %s min %d\n", depth * 2, "", c->metaObject()->className(),
                                    qPrintable(c->objectName()), qPrintable(c->property("text").toString().left(30)), m);
                        if (depth < 9) dump(c, depth + 1);
                    }
                }
            };
            dump(area->widget(), 1);
        }
        Check::report(least <= size.width(), "settings page about as narrow as the phone", __FILE__, __LINE__,
                      QString("%1 needs %2 of %3").arg(area->widget()->metaObject()->className()).arg(least)
                          .arg(size.width()));
    }
    // A desktop-sized room leaves it as it was
    SettingsDialog big{AiSettings{}};
    const QSize before = big.size();
    big.fitScreen(QRect(0, 0, 1920, 1080));
    CHECK(big.size() == before);
}

// A mouse-driven shell keeps the desktop's timeline (no centred playhead, no magnetic V1); before any test touch
// device exists (QTest::createTouchDevice registers a touchscreen for the rest of the run)
void testMouseShell(Engine& engine)
{
    Touch::setModeActive(false);
    MainWindow m(&engine);
    m.disableAutosave();
    m.enablePhoneLayout();
    TimelineView* mv = m.findChild<TimelineView*>();
    CHECK(!MainWindow::touchInput());
    CHECK(!mv->centerPlayhead());
    CHECK(!mv->magneticMain());
}

// The desktop's magnetic main track switch: Werkzeuge › "Lücken automatisch schließen (Spur 1)", Shift+N, a toolbar
// button; off by default; switched on, V1's gaps close at once (one undo step), off again nothing moves
void testDesktopMagnetic(Engine& engine)
{
    QSettings().remove("timeline/magneticMainDesktop");
    MainWindow m(&engine);
    m.disableAutosave();
    TimelineView* view = m.findChild<TimelineView*>();
    QAction* a = InputBindings::instance().action("magnetic_main");
    if (!CHECK(a)) return;
    CHECK(a->isCheckable() && !a->isChecked());
    CHECK_EQ(a->shortcut(), QKeySequence("Shift+N"));
    CHECK(!view->magneticMain());
    Project* p = m.editor()->project();
    p->edit("clips", [p](Timeline& tl) {
        Clip c;
        c.mediaPath = "/x/a.mp4";
        c.out = 49;
        c.id = p->newClipId();
        c.start = 0;
        tl.video[0].clips << c;
        c.id = p->newClipId();
        c.start = 100;
        tl.video[0].clips << c;
    });
    const int steps = p->undoStack()->index();
    a->trigger();
    CHECK(a->isChecked() && view->magneticMain() && m.editor()->magneticMain());
    CHECK_EQ(p->timeline().video[0].clips[1].start, 50);
    CHECK_EQ(p->undoStack()->index(), steps + 1);
    a->trigger();
    CHECK(!a->isChecked() && !view->magneticMain());
    CHECK_EQ(p->undoStack()->index(), steps + 1);
    // The toolbar has its button (the action's)
    bool button = false;
    for (QToolButton* b : m.findChildren<QToolButton*>()) button |= b->defaultAction() == a;
    CHECK(button);
}

// Review B2/B6: the panels' own titles come back on the desktop, the export sheet's box goes
void testShellRoundTrip(Engine& engine)
{
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    settle();
    QHash<QLabel*, bool> hidden;
    for (QLabel* l : w.findChildren<QLabel*>("PanelTitle")) hidden.insert(l, l->isHidden());
    w.setLayoutForm(MainWindow::LayoutForm::Phone);
    w.resize(411, 914);
    settle();
    for (const char* id : {"media", "effects", "mixer", "inspector", "transcript", "ai", "export"}) {
        w.phoneShell()->openSheet(id);
        settle();
    }
    // B5: the shell outlives the switch's own call (it may come from a slot of its ⋮ menu)
    QPointer<PhoneShell> shell = w.phoneShell();
    w.setLayoutForm(MainWindow::LayoutForm::Desktop);
    CHECK(shell && !w.phoneShell());
    w.resize(1400, 900);
    settle();
    CHECK(!shell);
    int changed = 0;
    for (auto it = hidden.cbegin(); it != hidden.cend(); ++it)
        if (it.key()->isHidden() != it.value()) {
            ++changed;
            std::printf("title %s: hidden %d -> %d\n", qPrintable(it.key()->text()), it.value(), it.key()->isHidden());
        }
    CHECK_EQ(changed, 0);
    CHECK(!w.findChild<QWidget*>("phoneExport"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("phone-layout");
    qunsetenv("SCHNEIDI_LAYOUT"); // (check.h pins the desktop layout; this test is about the others)
    MainWindow::setFormBySize(true); // (the forms by the window's size, as on Android)
    testSizes();
    testShell();
    if (qEnvironmentVariableIsEmpty("SCHNEIDI_PHONE_LAYOUT")) testSetting();
    SettingsDialog::setSavedPhoneLayout({});
    Touch::installDialogFit(&app); // (main.cpp: Android only)
    testDialogFit();
    testScrollIndicators();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testMouseShell(engine);
    testDesktopMagnetic(engine);
    testShellRoundTrip(engine);
    testSilentRestore(engine);
    testSteadyTransport(engine);
    Touch::installKineticScrolling(&app); // (main.cpp: every scroll area takes a finger)
    testToolBarSwipe(engine);
    testGallery(engine);
    // The phone's timeline: centred playhead without the sideways scrollbar, hold-to-move and the magnetic main track
    // on by default; the settings switch them (the desktop never has them)
    {
        Touch::setModeActive(true); // (a finger: touch mode, as on a phone)
        MainWindow w(&engine);
        w.disableAutosave();
        w.enablePhoneLayout();
        w.resize(411, 590);
        w.show();
        settle();
        TimelineView* view = w.findChild<TimelineView*>();
        CHECK(view->centerPlayhead() && view->holdToMove() && view->magneticMain());
        // The hidden menu bar's shortcuts still work (Ctrl+Z undoes)
        w.editor()->addTitle(0, -1, 50, TitleKind::Text);
        settle();
        const int steps = w.editor()->project()->undoStack()->index();
        QApplication::setActiveWindow(&w);
        QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier);
        settle();
        Check::report(w.editor()->project()->undoStack()->index() == steps - 1, "Ctrl+Z works in the phone layout",
                      __FILE__, __LINE__, QString("%1 -> %2").arg(steps).arg(w.editor()->project()->undoStack()->index()));
        bool barShown = false;
        for (QScrollBar* b : w.findChild<TimelinePanel*>()->findChildren<QScrollBar*>())
            if (b->orientation() == Qt::Horizontal && b->isVisible() && !qobject_cast<QAbstractScrollArea*>(b->parentWidget()->parentWidget()))
                barShown = true;
        CHECK(!barShown);
        // No scroll bar anywhere in the phone window (thin indicators instead); the desktop gets its bars back
        const auto visibleBars = [&w] {
            int n = 0;
            for (QScrollBar* b : w.findChildren<QScrollBar*>())
                if (b->isVisible() && b->maximum() > b->minimum()) ++n;
            return n;
        };
        for (const char* id : {"media", "inspector", "effects"}) {
            w.phoneShell()->openSheet(id);
            settle();
            Check::report(visibleBars() == 0, "no scroll bar in the phone window", __FILE__, __LINE__, id);
        }
        w.phoneShell()->closeSheet();
        CHECK(w.findChild<TimelinePanel*>()->scrollIndicators());
        // Item 6: a recording with five sound streams (A1-A5) shares half of the lanes instead of filling them
        w.editor()->project()->edit("five streams", [&](Timeline& tl) {
            tl.audio.resize(5);
            for (int i = 0; i < 5; ++i) {
                Clip c;
                c.id = w.editor()->project()->newClipId();
                c.mediaPath = QStringLiteral("phone-test-missing.mkv");
                c.start = 0;
                c.in = 0;
                c.out = 99;
                tl.audio[i].clips << c;
            }
        });
        w.resize(411, 914); // (the A51's portrait)
        settle();
        CHECK(view->fitAudioRows());
        const int audioRow = view->audioRowHeight();
        Check::report(audioRow < 46 && audioRow >= TimelineView::kFitAudioMin && 5 * audioRow <= view->viewportHeight(),
                      "five audio tracks share half of the lanes", __FILE__, __LINE__,
                      QString("row %1, lanes %2").arg(audioRow).arg(view->viewportHeight()));
        Check::report(view->contentHeight() <= view->viewportHeight(), "V and A1-A5 all in sight", __FILE__, __LINE__,
                      QString("%1 > %2").arg(view->contentHeight()).arg(view->viewportHeight()));
        // The A51 shot: ~110 px empty below A5 - the timeline is only as tall as its rows (and a short empty lane), the
        // viewer gets the rest
        const int emptyBelow = view->viewportHeight() - view->contentHeight();
        Check::report(emptyBelow <= TimelineView::kFitAudioMin + 4, "no empty lanes below A5 in portrait", __FILE__,
                      __LINE__, QString("%1 px empty").arg(emptyBelow));
        const int viewerFive = w.findChild<Viewer*>()->height();
        // Typing in a sheet: the keyboard makes the window lower for a moment - the rows keep their height
        {
            auto* field = new QLineEdit(w.phoneShell());
            field->show();
            QApplication::setActiveWindow(&w);
            field->setFocus();
            settle();
            w.resize(411, 600);
            settle();
            Check::report(view->audioRowHeight() == audioRow, "keyboard up: the audio rows keep their height", __FILE__,
                          __LINE__, QString("%1 / %2").arg(view->audioRowHeight()).arg(audioRow));
            delete field;
            w.resize(411, 914);
            settle();
            CHECK(view->audioRowHeight() == audioRow && w.findChild<Viewer*>()->height() == viewerFive);
        }
        // Fewer tracks (V1 + A1): a lower timeline, a taller viewer; never lower than 3/10 of the room (~1/4 of the shell)
        w.editor()->project()->edit("two streams", [&](Timeline& tl) {
            tl.audio.resize(1);
            tl.video.resize(1);
        });
        settle();
        auto* panel = w.findChild<TimelinePanel*>();
        Check::report(w.findChild<Viewer*>()->height() > viewerFive && panel->height() >= w.phoneShell()->height() / 4,
                      "V1 + A1: the viewer gets the timeline's empty room", __FILE__, __LINE__,
                      QString("viewer %1 (five: %2), timeline %3").arg(w.findChild<Viewer*>()->height()).arg(viewerFive)
                          .arg(panel->height()));
        Check::report(view->contentHeight() <= view->viewportHeight(), "V1 + A1 in sight", __FILE__, __LINE__);
        // A curve editor opened under the clip (+120 px of rows, no timeline change): the timeline grows with it, up
        // to its old share; closed, it is low again
        {
            int clipId = 0;
            w.editor()->project()->edit("keys", [&](Timeline& tl) {
                if (tl.video[0].clips.isEmpty()) {
                    Clip c;
                    c.id = w.editor()->project()->newClipId();
                    c.mediaPath = QStringLiteral("phone-test-missing.mkv");
                    c.out = 99;
                    tl.video[0].clips << c;
                }
                Clip& c = tl.video[0].clips[0];
                Keys::setKey(c, AnimParam::Opacity, 10, 20);
                Keys::setKey(c, AnimParam::Opacity, 80, 80);
                clipId = c.id;
            });
            settle();
            const int before = panel->height();
            view->setCurveEditor({clipId}, true, AnimParam::Opacity);
            settle();
            Check::report(panel->height() > before + 60 && view->contentHeight() <= view->viewportHeight(),
                          "curve editor opened: the phone timeline grows with its rows", __FILE__, __LINE__,
                          QString("%1 -> %2, content %3 / lanes %4").arg(before).arg(panel->height())
                              .arg(view->contentHeight()).arg(view->viewportHeight()));
            view->setCurveEditor({clipId}, false);
            settle();
            Check::report(panel->height() == before, "curve editor closed: as low as before", __FILE__, __LINE__,
                          QString("%1 / %2").arg(panel->height()).arg(before));
            w.editor()->project()->undoStack()->undo();
            settle();
        }
        // Back to five: the rows' old height and the timeline as before
        w.editor()->project()->undoStack()->undo();
        settle();
        Check::report(view->audioRowHeight() == audioRow && w.findChild<Viewer*>()->height() == viewerFive,
                      "five tracks again: the same rows and viewer", __FILE__, __LINE__,
                      QString("row %1 / %2, viewer %3 / %4").arg(view->audioRowHeight()).arg(audioRow)
                          .arg(w.findChild<Viewer*>()->height()).arg(viewerFive));
        QSettings().setValue("timeline/magneticMain", false);
        QSettings().setValue("timeline/clipDrag", "instant");
        w.setLayoutForm(MainWindow::LayoutForm::Desktop);
        w.setLayoutForm(MainWindow::LayoutForm::Phone);
        CHECK(!view->magneticMain() && !view->holdToMove());
        w.setLayoutForm(MainWindow::LayoutForm::Desktop);
        CHECK(!view->magneticMain() && !view->centerPlayhead());
        CHECK(!w.findChild<TimelinePanel*>()->scrollIndicators() && !w.findChild<ScrollIndicator*>());
        CHECK(!view->fitAudioRows()); // (the desktop: the user's heights)
        // Review 10: the desktop gets its own row heights and its tool row back (not the phone's 56/46, no scroll box)
        CHECK(view->view().videoTrackHeight != 56 && view->view().audioTrackHeight != 46);
        CHECK(!w.findChild<QScrollArea*>("timelineBarScroll"));

        QSettings().remove("timeline/magneticMain");
        QSettings().remove("timeline/clipDrag");
        Touch::setModeActive(false);
    }
    testWindow(engine, {411, 914});
    testWindow(engine, {914, 411});
    // As on a phone: touch mode (larger controls) and the app's style sheet with it
    Touch::setModeActive(true);
    Theme::apply(app);
    testWindow(engine, {411, 914});
    testWindow(engine, {914, 411});
    testSettingsFit({411, 914});
    testSettingsFit({914, 411});
    testSettingsFit({360, 740});
    testWindow(engine, {360, 740}); // a small phone (turned: 740x360)
    testWindow(engine, {914, 380}); // landscape below the status bar
    return Check::result();
}
