// SPDX-License-Identifier: AGPL-3.0-only
// Room for the work in the main window:
// - the bottom is one compact row (MainWindow::buildPageBar): the page buttons with icon and name side by side, the
//   status messages on its left (the QStatusBar's own row stays hidden), background work on its right; readable and
//   whole in German and Russian (long names), with the normal and a large font; nothing below it;
// - the whole program full screen (Workspace › Full-Screen App, Ctrl+F11, MainWindow::setAppFullScreen): on and off
//   by the action, back to maximized or normal as before, the menu entry follows the window state; the key is free
//   (F11 stays Replace) and differs from the viewer's full screen.
// WINDOW_SPACE_DUMP=<folder>: pictures of the window per language and font.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "core/I18n.h"
#include "engine/Engine.h"
#include "ui/BackgroundJobs.h"
#include "ui/ElidedLabel.h"
#include "ui/timeline/TimelinePanel.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QStatusBar>
#include <QTest>
#include <QToolButton>
#include <clocale>

namespace {

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
}

QAction* action(const QString& id)
{
    for (const InputBindings::Entry& e : InputBindings::instance().entries())
        if (e.id == id) return e.action;
    return nullptr;
}

void testBottomRow(Engine& engine, const QString& lang, const QString& font)
{
    const QString where = lang + ", " + font + " font";
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 900);
    w.show();
    settle();
    auto* bar = w.findChild<QWidget*>("PageBar");
    auto* text = static_cast<ElidedLabel*>(w.findChild<QLabel*>("statusText")); // ElidedLabel has no Q_OBJECT
    if (!CHECK(bar && text)) return;
    // One line high: a page button's text plus its padding and mark, not two lines and a status bar below
    const int line = bar->fontMetrics().height();
    Check::report(bar->height() <= line + 16, "page bar one compact line", __FILE__, __LINE__,
                  QString("%1: %2 px for a %3 px line").arg(where).arg(bar->height()).arg(line));
    CHECK(w.statusBar()->isHidden());
    // Nothing below it: the bar ends at the window's bottom
    const int bottom = bar->mapTo(&w, QPoint(0, bar->height())).y();
    Check::report(bottom >= w.height() - 1, "nothing below the page bar", __FILE__, __LINE__,
                  QString("%1: bar ends at %2 of %3").arg(where).arg(bottom).arg(w.height()));
    // Every page button whole (its text not cut), side by side without overlapping
    QRect last;
    for (const char* id : {"media", "edit", "comp", "color", "deliver"}) {
        auto* b = w.findChild<QToolButton*>(QString("pageButton_") + id);
        if (!CHECK(b && b->isVisible())) continue;
        Check::report(b->width() >= b->sizeHint().width() && b->height() >= b->sizeHint().height(), "page button whole",
                      __FILE__, __LINE__, QString("%1: %2 %3x%4, needs %5x%6").arg(where, id).arg(b->width())
                          .arg(b->height()).arg(b->sizeHint().width()).arg(b->sizeHint().height()));
        CHECK(!b->text().contains('\n')); // icon and name on one line
        const QRect r(b->mapTo(bar, QPoint(0, 0)), b->size());
        if (!last.isNull()) CHECK(r.left() > last.right());
        last = r;
    }
    // The buttons in the middle of the bar
    auto* first = w.findChild<QToolButton*>("pageButton_media");
    if (first && !last.isNull()) {
        const int left = first->mapTo(bar, QPoint(0, 0)).x(), right = bar->width() - 1 - last.right();
        Check::report(std::abs(left - right) <= 4, "page buttons centered", __FILE__, __LINE__,
                      QString("%1: %2 px left, %3 px right").arg(where).arg(left).arg(right));
    }
    // Status messages show in the bar, without them the name and version
    const QString name = text->fullText();
    CHECK(name.startsWith("schneidi "));
    w.statusBar()->showMessage("Test message", 5000);
    CHECK_EQ(text->fullText(), QString("Test message"));
    CHECK(text->isVisible() && text->width() > 100);
    w.statusBar()->clearMessage();
    CHECK_EQ(text->fullText(), name);
    // Background work on its right
    auto* jobs = w.findChild<BackgroundJobs*>();
    if (CHECK(jobs)) CHECK(bar->isAncestorOf(jobs));
    if (const QString dir = qEnvironmentVariable("WINDOW_SPACE_DUMP"); !dir.isEmpty())
        w.grab().save(QString("%1/window-%2-%3.png").arg(dir, lang, font));
}

void testFullScreen(Engine& engine)
{
    MainWindow w(&engine);
    w.disableAutosave();
    QAction* app = action("app_full_screen");
    QAction* viewer = action("viewer_full_screen");
    if (!CHECK(app && viewer)) return;
    CHECK(app->isCheckable());
    CHECK(app->text() != viewer->text()); // the program's and the viewer's full screen are told apart
    // Its key: free, and F11 stays Replace (source edits)
    const QKeySequence key = InputBindings::instance().current("app_full_screen");
    CHECK_EQ(key, QKeySequence("Ctrl+F11"));
    CHECK_EQ(InputBindings::instance().current("replace_clip"), QKeySequence(Qt::Key_F11));
    for (const InputBindings::Entry& e : InputBindings::instance().entries())
        if (e.id != "app_full_screen" && e.scope.isEmpty())
            Check::report(InputBindings::instance().current(e.id) != key, "full-screen key free", __FILE__, __LINE__, e.id);

    // From maximized: on, off -> maximized again
    w.showMaximized();
    settle();
    CHECK(w.isMaximized());
    app->trigger();
    settle();
    CHECK(w.isFullScreen());
    CHECK(app->isChecked());
    app->trigger();
    settle();
    CHECK(!w.isFullScreen());
    CHECK(w.isMaximized());
    CHECK(!app->isChecked());
    // From a normal window: back to normal
    w.showNormal();
    settle();
    w.setAppFullScreen(true);
    settle();
    CHECK(w.isFullScreen() && app->isChecked());
    w.setAppFullScreen(false);
    settle();
    CHECK(!w.isFullScreen() && !w.isMaximized());
    CHECK(!app->isChecked());
    // Left by the window manager (state changed from outside): the menu entry follows
    w.setAppFullScreen(true);
    settle();
    w.setWindowState(w.windowState() & ~Qt::WindowFullScreen);
    settle();
    CHECK(!app->isChecked());
    // The viewer's full screen is something else: the window stays as it is
    CHECK(!w.isFullScreen());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("window-space");
    I18n::install("de");
    std::setlocale(LC_NUMERIC, "C");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testFullScreen(engine);
    const QFont normal = QApplication::font();
    QFont big = normal;
    big.setPointSizeF(normal.pointSizeF() * 1.4); // like QT_FONT_DPI=120 or a large system font
    for (const QString lang : {"de", "ru"}) {
        if (lang != "de") I18n::install(lang); // the later translator wins
        QApplication::setFont(normal);
        testBottomRow(engine, lang, "normal");
        QApplication::setFont(big);
        testBottomRow(engine, lang, "large");
    }
    QApplication::setFont(normal);
    return Check::result();
}
