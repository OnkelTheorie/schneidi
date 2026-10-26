// App settings (File › Settings…, app/SettingsDialog): the AI page shows and returns AiSettings, sections can be
// added (the AI chat adds its own), the settings survive a restart (QSettings), the main window applies them (AI
// indicator modes, AI connections on/off with the reason for the AI) and a test run never saves them. General ›
// Performance chooses the threads for background work (Background::threads). "Connect with
// Claude" runs the setup the app hands in (in a thread) and shows its answer per target.
// SETTINGS_DUMP=<folder>: pictures of both tabs (settings-general.png, settings-ai.png).

#include "check.h"

#include "app/AiSettings.h"
#include "app/AiSettingsPage.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "engine/Background.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QSettings>
#include <QTabWidget>
#include <QThread>
#include <QToolButton>
#include <clocale>
#include <functional>

namespace {

bool waitFor(const std::function<bool()>& done, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

QCheckBox* checkBox(QWidget* w, const QString& text)
{
    for (QCheckBox* c : w->findChildren<QCheckBox*>())
        if (c->text() == text) return c;
    return nullptr;
}

void testSettings()
{
    QSettings().remove("ai");
    CHECK(AiSettings::load() == AiSettings{});
    AiSettings s;
    s.liveAllowed = false;
    s.highlight = false;
    s.color = QColor("#30c0a0");
    s.highlightMs = 3500;
    s.distinguish = false;
    s.logLength = 50;
    s.indicator = AiSettings::Indicator::Always;
    s.releaseMinutes = 0;
    s.save();
    CHECK(AiSettings::load() == s);
    QSettings().setValue("ai/highlightMs", 999999); // out of range from a hand-edited file
    CHECK_EQ(AiSettings::load().highlightMs, AiSettings::kMaxHighlightMs);
    QSettings().setValue("ai/releaseMinutes", -5);
    CHECK_EQ(AiSettings::load().releaseMinutes, 0);
    QSettings().remove("ai");
}

void testDialog()
{
    AiSettings s;
    s.highlightMs = 1500;
    s.indicator = AiSettings::Indicator::Never;
    SettingsDialog dlg(s);
    CHECK(dlg.aiSettings() == s); // what goes in comes out unchanged
    CHECK_EQ(dlg.language(), I18n::language());

    AiSettingsPage* page = dlg.aiPage();
    QCheckBox* allowed = checkBox(page, T("KIs dürfen das offene Projekt bearbeiten (schneidi-cli, MCP)"));
    QCheckBox* highlight = checkBox(page, T("Was eine KI ändert, kurz aufleuchten lassen"));
    auto* duration = page->findChild<QDoubleSpinBox*>();
    auto* indicator = page->findChild<QComboBox*>();
    if (!CHECK(allowed && highlight && duration && indicator)) return;
    allowed->setChecked(false);
    highlight->setChecked(false);
    CHECK(!duration->isEnabled()); // nothing to set while the highlight is off
    highlight->setChecked(true);
    duration->setValue(4.5);
    indicator->setCurrentIndex(indicator->findData(int(AiSettings::Indicator::Always)));
    page->setColor(QColor("#ff8800"));
    QCheckBox* distinguish = checkBox(page, T("Neue und bearbeitete Clips unterscheiden"));
    auto* logLength = page->findChild<QSpinBox*>("ai_log_length");
    if (CHECK(distinguish && logLength)) {
        distinguish->setChecked(false);
        logLength->setValue(0);
        CHECK_EQ(logLength->text(), T("Aus"));
    }
    // How long a silent AI keeps the app: nothing to set while AIs are not allowed at all
    auto* release = page->findChild<QSpinBox*>("ai_release");
    if (CHECK(release)) {
        CHECK_EQ(release->value(), 2);
        CHECK(!release->isEnabled());
        allowed->setChecked(true);
        CHECK(release->isEnabled());
        release->setValue(0);
        CHECK_EQ(release->text(), T("Nie"));
        allowed->setChecked(false);
    }
    const AiSettings chosen = dlg.aiSettings();
    CHECK(!chosen.liveAllowed);
    CHECK(chosen.highlight);
    CHECK_EQ(chosen.highlightMs, 4500);
    CHECK(chosen.indicator == AiSettings::Indicator::Always);
    CHECK_EQ(chosen.color, QColor("#ff8800"));
    CHECK(!chosen.distinguish);
    CHECK_EQ(chosen.logLength, 0);
    CHECK_EQ(chosen.releaseMinutes, 0);

    // "Connect with Claude": only with a setup handed in; answer per target, translated around the details
    CHECK(!page->findChild<QPushButton*>("claude_connect"));
    QStringList calls;
    page->setClaudeSetup([&calls](bool remove) {
        calls << (remove ? "remove" : "connect"); // runs in its own thread
        return QJsonObject{{"claude_code", QJsonObject{{"state", "done"}, {"message", "MCP server added"}}},
                           {"claude_desktop", QJsonObject{{"state", "skipped"}, {"message", "Claude Desktop not found"}}}};
    });
    auto* connectButton = page->findChild<QPushButton*>("claude_connect");
    auto* result = page->findChild<QLabel*>("claude_result");
    if (CHECK(connectButton && result)) {
        page->runClaudeSetup(false);
        CHECK(waitFor([&] { return connectButton->isEnabled(); }));
        CHECK_EQ(calls, QStringList{"connect"});
        CHECK(result->text().contains(T("%1: %2").arg("Claude Code", T("eingerichtet"))));
        CHECK(result->text().contains("MCP server added"));
        CHECK(result->text().contains(T("%1: %2").arg("Claude Desktop", T("nicht gefunden, übersprungen"))));
        page->runClaudeSetup(true);
        CHECK(waitFor([&] { return connectButton->isEnabled() && calls.size() == 2; }));
        CHECK(result->text().contains(T("%1: %2").arg("Claude Code", T("entfernt"))));
    }

    // The AI chat adds its own section, first on the page
    QGroupBox* chat = page->addSection("Chat", 0);
    AiSettingsPage::formOf(chat)->addRow(new QLabel("provider"));
    const auto boxes = page->findChildren<QGroupBox*>(Qt::FindDirectChildrenOnly);
    CHECK(!boxes.isEmpty());
    dlg.resize(560, 560);
    dlg.showTab(SettingsDialog::Tab::Ai); // laid out only while shown
    dlg.show();
    QApplication::processEvents();
    QGroupBox* first = nullptr;
    for (QGroupBox* b : boxes)
        if (!first || b->y() < first->y()) first = b;
    CHECK_EQ(first, chat);
    dlg.hide();

    if (const QString dump = qEnvironmentVariable("SETTINGS_DUMP"); !dump.isEmpty()) {
        SettingsDialog shot(AiSettings{});
        shot.aiPage()->setClaudeSetup([](bool) { return QJsonObject{}; });
        shot.resize(560, 560);
        shot.show();
        QApplication::processEvents();
        shot.grab().save(dump + "/settings-general.png");
        shot.showTab(SettingsDialog::Tab::Ai);
        QApplication::processEvents();
        shot.grab().save(dump + "/settings-ai.png");
    }
}

// General › Performance: threads for background work (whisper, analyses, proxies, import); 0 = automatic
void testBackgroundThreads()
{
    QSettings().remove("performance");
    CHECK_EQ(Background::chosenThreads(), 0);
    CHECK_EQ(Background::threads(3), 3); // automatic: the caller's own default
    {
        SettingsDialog dlg(AiSettings{});
        auto* combo = dlg.findChild<QComboBox*>("background_threads");
        if (CHECK(combo)) {
            CHECK_EQ(dlg.backgroundThreads(), 0);
            CHECK_EQ(combo->count(), QThread::idealThreadCount() + 1); // automatic + every number
            combo->setCurrentIndex(combo->findData(1));
            CHECK_EQ(dlg.backgroundThreads(), 1);
        }
    }
    Background::setChosenThreads(1);
    CHECK_EQ(Background::threads(6), 1); // the choice wins, also over a larger default
    CHECK_EQ(SettingsDialog(AiSettings{}).backgroundThreads(), 1); // shown again
    Background::setChosenThreads(100000);
    CHECK_EQ(Background::threads(6), QThread::idealThreadCount()); // at most the logical processors
    QSettings().remove("performance");
}

void testMainWindow()
{
    QSettings().remove("ai");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave(); // a test run: settings apply, but are not saved
    QStringList calls;
    QList<int> limits;
    w.setLiveControls([&calls](const QString& reason) { calls << "disconnect " + reason; }, [&calls] { calls << "allow"; },
                      [&limits](int ms) { limits << ms; });
    CHECK(calls.isEmpty()); // allowed by default: nothing refused at start
    CHECK(limits == QList<int>{2 * 60 * 1000}); // the release time from the settings reaches the server at start

    auto* box = w.findChild<QWidget*>("live_indicator");
    auto* label = w.findChild<QToolButton*>("live_label");
    auto* button = w.findChild<QToolButton*>("live_button");
    if (!CHECK(box && label && button)) return;
    // Default: only while an AI is connected
    CHECK(box->isHidden());
    w.liveConnectionChanged(true, "schneidi-cli mcp");
    CHECK(!box->isHidden());
    CHECK(label->text().contains(T("KI verbunden")));
    CHECK(!button->isHidden());
    w.liveConnectionChanged(false, "schneidi-cli mcp");
    CHECK(!box->isHidden()); // lingers a moment after a one-shot command

    AiSettings s = w.aiSettings();
    s.indicator = AiSettings::Indicator::Never;
    w.liveConnectionChanged(true, "schneidi-cli mcp");
    w.setAiSettings(s);
    CHECK(box->isHidden());
    w.liveConnectionChanged(false, "schneidi-cli mcp");
    s.indicator = AiSettings::Indicator::Always;
    w.setAiSettings(s);
    CHECK(!box->isHidden());
    QElapsedTimer t; // "connected" lingers a moment after the AI left
    t.start();
    while (t.elapsed() < 4000 && label->text() != "○ " + T("KI")) QApplication::processEvents(QEventLoop::AllEvents, 50);
    CHECK(!box->isHidden());
    CHECK_EQ(label->text(), "○ " + T("KI"));

    // AI connections off: refused with the reason for the AI, on again: allowed
    s.liveAllowed = false;
    w.setAiSettings(s);
    CHECK_EQ(calls.size(), 1);
    CHECK(calls.value(0).startsWith("disconnect ") && calls.value(0).contains("Settings"));
    CHECK_EQ(label->text(), T("KI aus"));
    CHECK(button->isHidden());
    s.liveAllowed = true;
    w.setAiSettings(s);
    CHECK_EQ(calls.value(1), QString("allow"));
    w.setAiSettings(s); // unchanged: nothing called again
    CHECK_EQ(calls.size(), 2);
    CHECK_EQ(limits.size(), qsizetype(1));
    s.releaseMinutes = 0;
    w.setAiSettings(s);
    CHECK(limits == (QList<int>{2 * 60 * 1000, 0}));

    // Started with AI connections off: refused right away
    QSettings().setValue("ai/liveAllowed", false);
    {
        MainWindow w2(&engine);
        w2.disableAutosave();
        QStringList calls2;
        w2.setLiveControls([&calls2](const QString& reason) { calls2 << reason; }, [] {}, [](int) {});
        CHECK_EQ(calls2.size(), 1);
    }
    QSettings().remove("ai");

    // The test run saved nothing
    CHECK(!QSettings().contains("ai/indicator"));
    CHECK(!QSettings().contains("ai/liveAllowed"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("settings");
    Theme::apply(app); // as in the app (pictures)
    I18n::install("de");
    testSettings();
    testDialog();
    testBackgroundThreads();
    testMainWindow();
    return Check::result();
}
