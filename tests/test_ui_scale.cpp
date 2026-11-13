// SPDX-License-Identifier: AGPL-3.0-only
// Interface size (app/UiScale.h): the factor offered for a screen (4K at 100 % yes; scaled, ordinary or far away TV
// screens no), the saved factor as QT_SCALE_FACTOR (never over the user's own, taken back after the QApplication), the
// offer only once (either answer kept, a factor chosen in the settings counts as an answer), and the bar in the window:
// "Leave it" changes nothing, "Larger" saves the factor (the restart question answered "Later"); a saved size the
// settings' list lacks stays chosen there; the user's own QT_SCALE_FACTOR is known as such.
// UI_SCALE_DUMP=<file.png>: a picture of the window with the bar.

#include "check.h"

#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "app/UiScale.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QMessageBox>
#include <QSettings>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <cmath>

namespace {

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

void testSuggestion()
{
    using UiScale::suggestion;
    CHECK_EQ(suggestion(3840, 1.0, 163), 1.75); // 27" 4K
    CHECK_EQ(suggestion(3840, 1.0, 137), 1.5);  // 32" 4K
    CHECK_EQ(suggestion(3200, 1.0, 120), 1.25); // smallest that counts
    CHECK_EQ(suggestion(7680, 1.0, 280), 2.0);  // 8K: at most 200 %
    CHECK_EQ(suggestion(3840, 1.5, 163), 0.0);  // scaled already
    CHECK_EQ(suggestion(2560, 1.0, 109), 0.0);  // WQHD
    CHECK_EQ(suggestion(3840, 1.0, 80), 0.0);   // a 4K TV
    CHECK_EQ(suggestion(3840, 1.0, std::nan("")), 0.0); // physical size unknown
    CHECK_EQ(UiScale::suggestion(static_cast<const QScreen*>(nullptr)), 0.0);
}

void testSettingAndEnvironment()
{
    QSettings().remove("ui");
    qunsetenv("QT_SCALE_FACTOR");
    CHECK_EQ(UiScale::saved(), 0.0);
    CHECK(!UiScale::applyBeforeApp()); // nothing saved
    CHECK(UiScale::offerDue());
    UiScale::setSaved(1.0);
    CHECK(!UiScale::applyBeforeApp()); // 100 %: nothing to set
    CHECK(!UiScale::offerDue());       // a size chosen: no offer
    UiScale::setSaved(1.5);
    CHECK(UiScale::applyBeforeApp());
    CHECK_EQ(qEnvironmentVariable("QT_SCALE_FACTOR"), QString("1.5"));
    CHECK(UiScale::userFactor().isEmpty()); // ours, not the user's
    UiScale::takeBackEnvironment(); // a restart must not take it for the user's
    CHECK(qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR"));
    qputenv("QT_SCALE_FACTOR", "2"); // the user's own wins
    CHECK(!UiScale::applyBeforeApp());
    UiScale::takeBackEnvironment();
    CHECK_EQ(qEnvironmentVariable("QT_SCALE_FACTOR"), QString("2"));
    CHECK_EQ(UiScale::userFactor(), QString("2")); // Settings says the size is overridden
    UiScale::setSaved(0);
    CHECK(!UiScale::offerDue()); // the user scales by hand
    qunsetenv("QT_SCALE_FACTOR");
    CHECK(UiScale::offerDue());
    UiScale::setAsked();
    CHECK(!UiScale::offerDue());
    QSettings().remove("ui");
}

QToolButton* button(MainWindow& w, const char* name)
{
    auto* b = w.findChild<QToolButton*>(name);
    return b && b->isVisible() ? b : nullptr;
}

// A saved size the list lacks (250 %) stays chosen in the settings: OK does not reset it
void testSettingsList()
{
    QSettings().remove("ui");
    UiScale::setSaved(2.5);
    {
        SettingsDialog dlg{AiSettings{}};
        CHECK_EQ(dlg.uiScale(), 2.5);
    }
    UiScale::setSaved(1.25);
    {
        SettingsDialog dlg{AiSettings{}};
        CHECK_EQ(dlg.uiScale(), 1.25);
    }
    QSettings().remove("ui");
}

void testBar(Engine& engine)
{
    QSettings().remove("ui");
    {
        // "Leave it": nothing saved, asked
        MainWindow w(&engine);
        w.disableAutosave();
        w.resize(1000, 700);
        w.show();
        w.offerUiScale(1.75);
        settle();
        QWidget* bar = w.findChild<QWidget*>("ui_scale_bar");
        CHECK(bar && bar->isVisible());
        if (bar) CHECK(bar->geometry().right() < w.width() && w.width() == 1000); // the text wraps, never wider
        if (const QString dump = qEnvironmentVariable("UI_SCALE_DUMP"); !dump.isEmpty()) w.grab().save(dump);
        QToolButton* larger = button(w, "ui_scale_larger");
        CHECK(larger && larger->text().contains("175"));
        QToolButton* keep = button(w, "ui_scale_keep");
        if (!CHECK(keep)) return;
        keep->click();
        settle();
        CHECK(!bar->isVisible());
        CHECK_EQ(UiScale::saved(), 0.0);
        CHECK(!UiScale::offerDue());
    }
    QSettings().remove("ui");
    {
        // "Larger": the factor saved, the restart question answered "Later"
        MainWindow w(&engine);
        w.disableAutosave();
        w.show();
        w.offerUiScale(1.5);
        settle();
        QToolButton* larger = button(w, "ui_scale_larger");
        if (!CHECK(larger)) return;
        bool asked = false;
        QTimer::singleShot(100, &w, [&asked] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                asked = true;
                box->reject();
            }
        });
        larger->click();
        settle();
        CHECK(asked);
        CHECK(!w.restartRequested());
        CHECK_EQ(UiScale::saved(), 1.5);
        CHECK(!UiScale::offerDue());
    }
    QSettings().remove("ui");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("ui-scale");
    testSuggestion();
    testSettingAndEnvironment();
    testSettingsList();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testBar(engine);
    return Check::result();
}
