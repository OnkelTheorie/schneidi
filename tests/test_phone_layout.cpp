// SPDX-License-Identifier: AGPL-3.0-only
// The phone layout (ui/phone/PhoneShell, app/MainWindowPhone.cpp) on a phone-sized window, portrait (411x914, a
// Galaxy A51's logical size) and landscape (914x411): nothing is wider or taller than the window (the panels' minimum
// sizes never force it larger: the Mixer tab made the Android window slide off to the right), one panel at a time as
// the sheet, the back key closes the sheet first, then the clip tools (a selected clip shows them). Which screens get
// it (shorter side below 600 px); the desktop layout stays as it was without it.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Types.h"
#include "engine/Engine.h"
#include "ui/phone/PhoneShell.h"
#include "ui/touch/Touch.h"
#include "app/Theme.h"

#include <QApplication>
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
    QTest::mouseClick(shell->toolButton("volume"), Qt::LeftButton);
    settle();
    CHECK(shell->sheet() == "inspector");
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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("phone-layout");
    testSizes();
    testShell();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
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
