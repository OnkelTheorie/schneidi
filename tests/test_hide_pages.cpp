// SPDX-License-Identifier: AGPL-3.0-only
// Hiding pages of the page bar (PageVisibility, MainWindow::setPageHidden / fillPagesMenu): default all shown, the last
// shown page cannot be hidden, a hidden current page switches to the nearest shown one, a key to a hidden page shows
// it while it is current, saved in the settings and read again by a new window, every page button has its stable
// objectName pageButton_<id> (tours point at it).
// HIDE_PAGES_DUMP=<folder>: pictures of the page bar with all pages and with some hidden.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/PageVisibility.h"
#include "engine/Engine.h"

#include <QAction>
#include <QApplication>
#include <QImage>
#include <QMenu>
#include <QSettings>
#include <QTest>
#include <QToolButton>
#include <clocale>

namespace {

void dump(QWidget* w, const QString& name)
{
    if (const QString dir = qEnvironmentVariable("HIDE_PAGES_DUMP"); !dir.isEmpty())
        w->grab().save(dir + "/" + name + ".png");
}

void testLogic()
{
    QSettings().remove(PageVisibility::kSettingsKey);
    PageVisibility v({"media", "edit", "comp", "color", "deliver"});
    v.load();
    CHECK_EQ(v.visibleCount(), 5); // default: all shown
    CHECK(v.hidden().isEmpty());
    CHECK(v.setHidden("edit", true));
    CHECK(!v.setHidden("edit", true)); // nothing changed
    CHECK(!v.setHidden("nope", true));
    CHECK_EQ(v.nearestVisible("edit"), QString("comp")); // the right one first on a tie
    CHECK_EQ(v.nearestVisible("media"), QString("media"));
    CHECK(v.setHidden("comp", true));
    CHECK_EQ(v.nearestVisible("edit"), QString("media")); // distance 1 left before distance 2 right
    CHECK_EQ(v.nearestVisible("comp"), QString("color"));
    CHECK(v.setHidden("media", true));
    CHECK(v.setHidden("color", true));
    CHECK_EQ(v.visibleCount(), 1);
    CHECK(!v.canHide("deliver")); // the last shown page stays
    CHECK(!v.setHidden("deliver", true));
    CHECK_EQ(v.nearestVisible("media"), QString("deliver"));
    CHECK_EQ(v.hidden(), QStringList({"media", "edit", "comp", "color"})); // bar order

    // Saved and read again; ids another version does not know are dropped, all hidden = all shown
    v.save();
    PageVisibility again({"media", "edit", "comp", "color", "deliver"});
    again.load();
    CHECK_EQ(again.hidden(), QStringList({"media", "edit", "comp", "color"}));
    QSettings().setValue(PageVisibility::kSettingsKey, QStringList({"edit", "future"}));
    again.load();
    CHECK_EQ(again.hidden(), QStringList({"edit"}));
    QSettings().setValue(PageVisibility::kSettingsKey, QStringList({"media", "edit", "comp", "color", "deliver"}));
    again.load();
    CHECK_EQ(again.visibleCount(), 5);
    again.showAll();
    again.save();
    CHECK(!QSettings().contains(PageVisibility::kSettingsKey)); // nothing hidden = nothing stored
}

QToolButton* button(MainWindow& w, const char* id)
{
    return w.findChild<QToolButton*>(QString("pageButton_") + id);
}

QAction* menuEntry(QMenu& menu, const QString& name)
{
    return menu.findChild<QAction*>(name);
}

void testWindow(Engine& engine)
{
    QSettings().remove(PageVisibility::kSettingsKey);
    using Page = MainWindow::Page;
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.resize(1400, 900);
        w.show();
        CHECK(QTest::qWaitForWindowExposed(&w));
        for (const char* id : {"media", "edit", "comp", "color", "deliver"}) {
            QToolButton* b = button(w, id);
            if (CHECK(b != nullptr)) CHECK(b->isVisible()); // default: all shown, no question
        }
        CHECK(w.currentPage() == Page::Edit);
        QWidget* bar = w.findChild<QWidget*>("PageBar");
        if (bar) dump(bar, "all");

        // The menu: checkboxes, all checked and enabled, "Show all" disabled while nothing is hidden
        QMenu menu;
        w.fillPagesMenu(&menu);
        QAction* edit = menuEntry(menu, "pageVisible_edit");
        QAction* all = menuEntry(menu, "pageVisible_all");
        if (!CHECK(edit && all)) return;
        CHECK(edit->isCheckable() && edit->isChecked() && edit->isEnabled());
        CHECK(!all->isEnabled());

        // Unchecking the current page hides it and goes to the nearest shown one (Comp, to the right)
        edit->trigger();
        CHECK(w.isPageHidden(Page::Edit));
        CHECK(w.currentPage() == Page::Comp);
        CHECK(!button(w, "edit")->isVisible());
        // A test run (screenshots too) never writes the user's settings
        CHECK(!QSettings().contains(PageVisibility::kSettingsKey));

        // Hide all but Deliver: its checkbox is disabled
        w.setPageHidden(Page::Media, true);
        w.setPageHidden(Page::Color, true);
        w.setPageHidden(Page::Comp, true);
        CHECK(w.currentPage() == Page::Color || w.currentPage() == Page::Deliver);
        w.setPageHidden(Page::Color, true);
        CHECK(w.currentPage() == Page::Deliver);
        w.setPageHidden(Page::Deliver, true); // refused
        CHECK(!w.isPageHidden(Page::Deliver));
        w.fillPagesMenu(&menu);
        CHECK(!menuEntry(menu, "pageVisible_deliver")->isEnabled());
        CHECK(menuEntry(menu, "pageVisible_deliver")->isChecked());
        CHECK(menuEntry(menu, "pageVisible_media")->isEnabled()); // hidden ones can come back
        CHECK(!menuEntry(menu, "pageVisible_media")->isChecked());
        CHECK(menuEntry(menu, "pageVisible_all")->isEnabled());

        // A key to a hidden page shows it (its button too) while it is current; leaving hides the button again
        QAction* key = InputBindings::instance().action("page_media");
        if (CHECK(key != nullptr)) {
            key->trigger();
            CHECK(w.currentPage() == Page::Media);
            CHECK(button(w, "media")->isVisible() && button(w, "media")->isChecked());
            CHECK(w.isPageHidden(Page::Media)); // still hidden in the settings
            InputBindings::instance().action("page_deliver")->trigger();
            CHECK(!button(w, "media")->isVisible());
        }
        if (bar) dump(bar, "only-deliver");

        // The Workspace submenu has the same entries
        auto* sub = w.findChild<QMenu*>("visiblePagesMenu");
        if (CHECK(sub != nullptr)) {
            emit sub->aboutToShow();
            CHECK(sub->findChild<QAction*>("pageVisible_media") != nullptr);
            CHECK(!sub->findChild<QAction*>("pageVisible_deliver")->isEnabled());
        }

        // Back to Media + Deliver for the next window
        w.setPageHidden(Page::Media, false);
        if (bar) {
            // Grabbing renders the bar: only the shown buttons take room
            const QImage img = bar->grab().toImage();
            CHECK(!img.isNull() && img.width() > 0);
            CHECK(button(w, "media")->isVisible() && button(w, "deliver")->isVisible());
            CHECK(!button(w, "comp")->isVisible());
            dump(bar, "media-deliver");
        }
    }
    // A normal window saves what it hides (Edit, Comp, Color: Media and Deliver are left for the next window)
    {
        MainWindow w(&engine);
        w.show();
        CHECK(QTest::qWaitForWindowExposed(&w));
        for (Page p : {Page::Edit, Page::Comp, Page::Color}) w.setPageHidden(p, true);
        CHECK(QSettings().value(PageVisibility::kSettingsKey).toStringList() == QStringList({"edit", "comp", "color"}));
        w.disableAutosave();
    }
    // A new window reads it again and starts on the page nearest to Edit (Media)
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.show();
        CHECK(QTest::qWaitForWindowExposed(&w));
        CHECK(w.isPageHidden(Page::Edit));
        CHECK(!w.isPageHidden(Page::Media));
        CHECK(w.currentPage() == Page::Media);
        CHECK(!button(w, "edit")->isVisible());
        w.showAllPages();
        for (const char* id : {"media", "edit", "comp", "color", "deliver"}) CHECK(button(w, id)->isVisible());
        CHECK(QSettings().value(PageVisibility::kSettingsKey).toStringList().size() == 3); // a test run: unchanged
        QSettings().remove(PageVisibility::kSettingsKey);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("hide-pages");
    std::setlocale(LC_NUMERIC, "C");
    testLogic();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testWindow(engine);
    return Check::result();
}
