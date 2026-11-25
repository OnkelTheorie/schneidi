// SPDX-License-Identifier: AGPL-3.0-only
// App settings (File › Settings…, app/SettingsDialog): the AI page shows and returns AiSettings, sections can be
// added (the AI chat adds its own), the settings survive a restart (QSettings), the main window applies them (AI
// indicator modes, AI connections on/off with the reason for the AI) and a test run never saves them. General ›
// Performance chooses the threads for background work (Background::threads). "Connect with
// Claude" runs the setup the app hands in (in a thread) and shows its answer per target. The Timeline tab
// (TimelineSettingsPage) shows each timeline choice as stored, and store() writes them.
// SETTINGS_DUMP=<folder>: pictures of both tabs (settings-general.png, settings-ai.png).

#include "check.h"

#include "app/AiSettings.h"
#include "app/AiSettingsPage.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "app/TimelineSettingsPage.h"
#include "core/EditDefaults.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "engine/Background.h"
#include "engine/Engine.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "core/Vocabulary.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/timeline/PlayheadMove.h"
#include "ui/timeline/TimelineOptions.h"
#include "ui/timeline/ZoomAnchor.h"

#include <QApplication>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonObject>
#include <QLineEdit>
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
// The word list for speech recognition (core/Vocabulary, File > Settings > Transcription): parsed from lines or
// commas, stored and read back, the dialog's field, game terms, the starting text cut at whisper's limit, the cache id
void testVocabulary()
{
    CHECK_EQ(Vocabulary::parse("Onkel Theo\n  LuciSon , headshot\n\nHeadshot;AWP"),
             (QStringList{"Onkel Theo", "LuciSon", "headshot", "AWP"}));
    CHECK_EQ(Vocabulary::combine({"AWP", "Pump"}, {"awp", "Deagle"}), (QStringList{"AWP", "Pump", "Deagle"}));
    Vocabulary::setStored({});
    CHECK(Vocabulary::stored().isEmpty());
    CHECK(!QSettings().contains("transcribe/vocabulary")); // nothing stored for an empty list
    Vocabulary::setStored({"Onkel Theo", "BIGDADDYARSCH", "Onkel Theo"});
    CHECK_EQ(Vocabulary::stored(), (QStringList{"Onkel Theo", "BIGDADDYARSCH"}));
    {
        SettingsDialog dlg(AiSettings{});
        auto* field = dlg.findChild<QPlainTextEdit*>("transcribe_vocabulary");
        if (CHECK(field)) {
            CHECK_EQ(field->toPlainText(), QString("Onkel Theo\nBIGDADDYARSCH"));
            field->setPlainText("Onkel Theo\nLuciSon, Gegge\n");
            CHECK_EQ(dlg.vocabulary(), (QStringList{"Onkel Theo", "LuciSon", "Gegge"}));
        }
    }
    // Game terms: CS2 and Valorant have a few, other games none
    CHECK(Vocabulary::gameTerms("cs2").contains("Headshot") && Vocabulary::gameTerms("CS2").contains("AWP"));
    CHECK(Vocabulary::gameTerms("valorant").contains("Spike"));
    CHECK(Vocabulary::gameTerms("minecraft").isEmpty());
    CHECK(Vocabulary::gameTerms("cs2").size() <= 25); // short: each term takes room and pulls the spelling
    // The starting text: the punctuation prompt, then the words
    const QString base = "Hallo, wie geht's? Gut, und dir? Ja, passt.";
    const QStringList list = Vocabulary::combine({"Onkel Theo"}, Vocabulary::gameTerms("cs2"));
    QStringList dropped;
    const QString prompt = Vocabulary::prompt(base, list, Vocabulary::kMaxPromptTokens, &dropped);
    CHECK(prompt.startsWith(base + " Onkel Theo, Headshot, AWP,"));
    CHECK(prompt.endsWith("T-Spawn."));
    CHECK(dropped.isEmpty());
    CHECK(Vocabulary::estimateTokens(prompt) <= Vocabulary::kMaxPromptTokens);
    CHECK_EQ(Vocabulary::prompt({}, {}), QString());
    CHECK_EQ(Vocabulary::prompt(base, {}), base);
    // Too long: cut in order with the rest named, never over the limit
    QStringList many;
    for (int i = 0; i < 200; ++i) many << QString("Spielername%1").arg(i);
    const QString cut = Vocabulary::prompt(base, many, Vocabulary::kMaxPromptTokens, &dropped);
    CHECK(Vocabulary::estimateTokens(cut) <= Vocabulary::kMaxPromptTokens);
    CHECK(cut.contains("Spielername0,") && cut.endsWith('.'));
    CHECK(!dropped.isEmpty() && dropped.first() != "Spielername0" && dropped.last() == "Spielername199");
    CHECK_EQ(cut.count(',') - base.count(',') + 1 + dropped.size(), 200);
    // Cache id: none for no words (old keys stay valid), another list another id
    CHECK(Vocabulary::cacheId({}).isEmpty());
    CHECK(Vocabulary::cacheId({"AWP"}) != Vocabulary::cacheId({"AWP", "Pump"}));
    CHECK_EQ(Vocabulary::cacheId({"AWP"}), Vocabulary::cacheId({"AWP"}));
    Vocabulary::setStored({});
}

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
    // More than this computer has (settings from a larger one): shown and kept, not stored back as automatic
    const int many = QThread::idealThreadCount() + 4;
    Background::setChosenThreads(many);
    CHECK_EQ(SettingsDialog(AiSettings{}).backgroundThreads(), many);
    Background::setChosenThreads(1);

    // Sound output device: "system default" first, the stored device chosen; a stored device that is not there is
    // still listed (unplugged), so opening and confirming the dialog never switches it away by itself
    {
        QSettings().remove("audio/output");
        SettingsDialog dlg(AiSettings{});
        auto* combo = dlg.findChild<QComboBox*>("audio_output");
        if (CHECK(combo && combo->count() >= 1)) {
            CHECK(combo->itemData(0).toString().isEmpty());
            CHECK(dlg.audioOutput().isEmpty());
        }
        QSettings().setValue("audio/output", "gone-headset-123");
        SettingsDialog again(AiSettings{});
        CHECK_EQ(again.audioOutput(), QString("gone-headset-123"));
        auto* c2 = again.findChild<QComboBox*>("audio_output");
        if (CHECK(c2)) CHECK(c2->currentText().contains("gone-headset-123"));
        QSettings().remove("audio/output");
    }
    Background::setChosenThreads(100000);
    CHECK_EQ(Background::threads(6), QThread::idealThreadCount()); // at most the logical processors
    QSettings().remove("performance");
}

// Projects folder: off by default (next to the footage); on = the chosen folder, shown again next time
void testProjectFolder()
{
    QSettings().remove("project");
    CHECK(!SettingsDialog::storedOwnFolder());
    CHECK(SettingsDialog::storedFolder().endsWith("schneidi"));
    {
        SettingsDialog dlg(AiSettings{});
        auto* own = dlg.findChild<QCheckBox*>("project_own_folder");
        auto* folder = dlg.findChild<QLineEdit*>("project_folder");
        if (CHECK(own && folder)) {
            CHECK(!own->isChecked() && !folder->isEnabled());
            own->setChecked(true);
            CHECK(folder->isEnabled());
            folder->setText("/tmp/meine-projekte");
            CHECK(dlg.ownProjectFolder());
            SettingsDialog::store(dlg.ownProjectFolder(), dlg.projectFolder());
        }
    }
    CHECK(SettingsDialog::storedOwnFolder());
    CHECK_EQ(SettingsDialog::storedFolder(), QDir::fromNativeSeparators(QDir::toNativeSeparators("/tmp/meine-projekte")));
    CHECK(SettingsDialog(AiSettings{}).findChild<QCheckBox*>("project_own_folder")->isChecked());
    QSettings().remove("project");
    // The backup copy: every minute by default (as before, not stored), each choice stored and shown again
    {
        CHECK_EQ(SettingsDialog::storedAutosaveSeconds(), 60);
        SettingsDialog dlg(AiSettings{});
        auto* box = dlg.findChild<QComboBox*>("project_autosave");
        if (CHECK(box)) {
            CHECK_EQ(box->currentData().toInt(), 60);
            CHECK_EQ(box->count(), 5);
            box->setCurrentIndex(box->findData(300));
            SettingsDialog::setStoredAutosaveSeconds(dlg.autosaveSeconds());
            CHECK_EQ(SettingsDialog::storedAutosaveSeconds(), 300);
            CHECK_EQ(SettingsDialog(AiSettings{}).findChild<QComboBox*>("project_autosave")->currentData().toInt(), 300);
            SettingsDialog::setStoredAutosaveSeconds(60);
            CHECK(!QSettings().contains("project/autosaveSeconds"));
            QSettings().setValue("project/autosaveSeconds", 2); // nonsense in the file: the default
            CHECK_EQ(SettingsDialog::storedAutosaveSeconds(), 60);
        }
    }
    QSettings().remove("project");
    // Compositing: new nodes linked at once, on by default
    QSettings().remove("comp");
    SettingsDialog dlg(AiSettings{});
    auto* link = dlg.findChild<QCheckBox*>("comp_auto_link");
    if (CHECK(link)) {
        CHECK(link->isChecked() && dlg.autoLinkNodes());
        link->setChecked(false);
        CHECK(!dlg.autoLinkNodes());
    }
    // Timeline: wheel zoom at the mouse by default; the choice shows the stored playhead mode
    QSettings().remove("timeline");
    auto* anchor = dlg.findChild<QComboBox*>("timeline_zoom_anchor");
    if (CHECK(anchor)) {
        CHECK_EQ(anchor->count(), 2);
        CHECK(dlg.zoomAnchor() == ZoomAnchor::Mode::Mouse);
        anchor->setCurrentIndex(anchor->findData(int(ZoomAnchor::Mode::Playhead)));
        CHECK(dlg.zoomAnchor() == ZoomAnchor::Mode::Playhead);
        ZoomAnchor::setMode(dlg.zoomAnchor());
        CHECK_EQ(QSettings().value("timeline/zoomAnchor").toString(), QString("playhead"));
        SettingsDialog again(AiSettings{});
        CHECK(again.zoomAnchor() == ZoomAnchor::Mode::Playhead);
        QSettings().remove("timeline");
        CHECK(ZoomAnchor::mode() == ZoomAnchor::Mode::Mouse);
    }
    // Timeline: after moving the playhead - Standard (the behaviour before) by default, four choices, stored and shown
    auto* after = dlg.findChild<QComboBox*>("timeline_after_playhead_move");
    if (CHECK(after)) {
        CHECK_EQ(after->count(), 4);
        CHECK(dlg.afterPlayheadMove() == PlayheadMove::Mode::Standard);
        CHECK_EQ(after->currentText(), T("Standard"));
        for (PlayheadMove::Mode m : {PlayheadMove::Mode::Continue, PlayheadMove::Mode::Stop, PlayheadMove::Mode::Play}) {
            after->setCurrentIndex(after->findData(int(m)));
            CHECK(dlg.afterPlayheadMove() == m);
            PlayheadMove::setMode(dlg.afterPlayheadMove());
            SettingsDialog again(AiSettings{});
            CHECK(again.afterPlayheadMove() == m);
        }
        CHECK_EQ(QSettings().value("timeline/afterPlayheadMove").toString(), QString("play"));
        QSettings().remove("timeline");
        CHECK(PlayheadMove::mode() == PlayheadMove::Mode::Standard);
    }
}

// One choice of Settings › Timeline: `count` entries, `def` shown when nothing is stored (and stored as nothing),
// each entry stored by store() as `stored()` reads it back and shown again by a new dialog
void checkTimelineChoice(const char* name, int count, int def, const std::function<int()>& stored)
{
    QSettings().remove("timeline");
    SettingsDialog dlg(AiSettings{});
    auto* combo = dlg.timelinePage()->findChild<QComboBox*>(name);
    if (!CHECK(combo)) return;
    CHECK_EQ(combo->count(), count);
    CHECK_EQ(combo->currentData().toInt(), def);
    CHECK_EQ(stored(), def);
    for (int i = 0; i < combo->count(); ++i) {
        combo->setCurrentIndex(i);
        dlg.timelinePage()->store();
        CHECK_EQ(stored(), combo->itemData(i).toInt());
        SettingsDialog again(AiSettings{});
        CHECK_EQ(again.timelinePage()->findChild<QComboBox*>(name)->currentData().toInt(), combo->itemData(i).toInt());
    }
    combo->setCurrentIndex(combo->findData(def));
    dlg.timelinePage()->store();
    CHECK(QSettings().allKeys().filter("timeline/").isEmpty()); // the default is not stored
    QSettings().remove("timeline");
}

// Settings › Timeline: its own tab with the timeline choices; store() writes what the fields show
void testTimelineTab()
{
    QSettings().remove("timeline");
    SettingsDialog dlg(AiSettings{});
    auto* tabs = dlg.findChild<QTabWidget*>();
    TimelineSettingsPage* page = dlg.timelinePage();
    if (!CHECK(tabs && page)) return;
    QStringList names;
    for (int i = 0; i < tabs->count(); ++i) names << tabs->tabText(i);
    CHECK(names.contains(T("Timeline")));
    dlg.showTab(SettingsDialog::Tab::Timeline);
    CHECK(page->isAncestorOf(tabs->currentWidget()) || tabs->currentWidget()->isAncestorOf(page));
    // The two older choices live there now (same keys); store() keeps the defaults unwritten
    CHECK(page->findChild<QComboBox*>("timeline_zoom_anchor"));
    CHECK(page->findChild<QComboBox*>("timeline_after_playhead_move"));
    page->store();
    CHECK(QSettings().allKeys().filter("timeline/").isEmpty());
    page->findChild<QComboBox*>("timeline_zoom_anchor")->setCurrentIndex(1);
    page->store();
    CHECK(ZoomAnchor::mode() == ZoomAnchor::Mode::Playhead);
    QSettings().remove("timeline");
    // Follow the playhead while playing: off, page, centre (off by default)
    checkTimelineChoice("timeline_follow_playback", 3, int(TimelineOptions::Follow::Off),
                        [] { return int(TimelineOptions::follow()); });
    // New clips: still 5 s and transition 1 s by default; stored, shown again, the core gets them on apply
    {
        QSettings().remove("timeline");
        SettingsDialog dlg(AiSettings{});
        auto* still = dlg.timelinePage()->findChild<QDoubleSpinBox*>("timeline_still_seconds");
        auto* trans = dlg.timelinePage()->findChild<QDoubleSpinBox*>("timeline_transition_seconds");
        if (CHECK(still && trans)) {
            CHECK_EQ(still->value(), 5.0);
            CHECK_EQ(trans->value(), 1.0);
            dlg.timelinePage()->store();
            CHECK(QSettings().allKeys().filter("timeline/").isEmpty());
            still->setValue(3);
            trans->setValue(0.5);
            dlg.timelinePage()->store();
            CHECK_EQ(TimelineOptions::stillSeconds(), 3.0);
            CHECK_EQ(TimelineOptions::transitionSeconds(), 0.5);
            SettingsDialog again(AiSettings{});
            CHECK_EQ(again.timelinePage()->findChild<QDoubleSpinBox*>("timeline_still_seconds")->value(), 3.0);
            TimelineOptions::applyEditDefaults();
            CHECK_EQ(EditDefaults::stillSeconds(), 3.0);
            CHECK_EQ(EditDefaults::transitionFrames(25), 13);
            QSettings().remove("timeline");
            TimelineOptions::applyEditDefaults();
            CHECK_EQ(EditDefaults::stillSeconds(), 5.0);
        }
    }
    // Step sizes: big step 1 second, big nudge 5 frames by default; stored as chosen
    {
        QSettings().remove("timeline");
        SettingsDialog dlg(AiSettings{});
        TimelineSettingsPage* page = dlg.timelinePage();
        auto* count = page->findChild<QSpinBox*>("timeline_big_step_count");
        auto* unit = page->findChild<QComboBox*>("timeline_big_step_unit");
        auto* ncount = page->findChild<QSpinBox*>("timeline_big_nudge_count");
        auto* nunit = page->findChild<QComboBox*>("timeline_big_nudge_unit");
        if (CHECK(count && unit && ncount && nunit)) {
            CHECK(page->bigStep() == TimelineOptions::kDefaultBigStep);
            CHECK(page->bigNudge() == TimelineOptions::kDefaultBigNudge);
            CHECK_EQ(count->value(), 1);
            CHECK_EQ(ncount->value(), 5);
            page->store();
            CHECK(QSettings().allKeys().filter("timeline/").isEmpty());
            count->setValue(10);
            unit->setCurrentIndex(unit->findData(0)); // frames
            ncount->setValue(2);
            nunit->setCurrentIndex(nunit->findData(1)); // seconds
            page->store();
            CHECK(TimelineOptions::bigStep() == (TimelineOptions::Step{10, false}));
            CHECK(TimelineOptions::bigNudge() == (TimelineOptions::Step{2, true}));
            CHECK_EQ(TimelineOptions::stepFrames(TimelineOptions::bigNudge(), 25), 50);
            SettingsDialog again(AiSettings{});
            CHECK(again.timelinePage()->bigStep() == (TimelineOptions::Step{10, false}));
            QSettings().remove("timeline");
        }
    }
    // Space while playing: stop where it is (default) or go back to the start position
    checkTimelineChoice("timeline_space_stops", 2, 0, [] { return int(TimelineOptions::spaceReturns()); });
    // The first clip with another format: ask (default), always adapt, never change
    checkTimelineChoice("timeline_first_clip_format", 3, int(TimelineOptions::ClipFormat::Ask),
                        [] { return int(TimelineOptions::clipFormat()); });
    // A clip's edge onto its neighbour: overwrites (default) or stops
    checkTimelineChoice("timeline_edge_meets_clip", 2, 1, [] { return int(TimelineOptions::trimOverwrites()); });
    // J/L steps: doubling (default) or gentle
    checkTimelineChoice("timeline_shuttle_steps", 2, int(TimelineOptions::ShuttleSteps::Double),
                        [] { return int(TimelineOptions::shuttleSteps()); });
    // Middle button drag: pans the view (default) or nothing
    checkTimelineChoice("timeline_middle_drag", 2, 1, [] { return int(TimelineOptions::middleDragPans()); });
    // A click into the tracks: only select (default) or set the playhead too
    checkTimelineChoice("timeline_click_sets_playhead", 2, 0, [] { return int(TimelineOptions::clickSetsPlayhead()); });
    // Times as timecode (default), seconds or frames; each entry shows an example
    // Snapping: reach small/medium (default)/large; targets clip edges + playhead by default, each box stored
    checkTimelineChoice("timeline_snap_reach", 3, int(TimelineOptions::SnapReach::Medium),
                        [] { return int(TimelineOptions::snapReach()); });
    {
        QSettings().remove("timeline");
        SettingsDialog dlg(AiSettings{});
        TimelineSettingsPage* page = dlg.timelinePage();
        auto* clips = page->findChild<QCheckBox*>("timeline_snap_clips");
        auto* playhead = page->findChild<QCheckBox*>("timeline_snap_playhead");
        auto* markers = page->findChild<QCheckBox*>("timeline_snap_markers");
        auto* inout = page->findChild<QCheckBox*>("timeline_snap_inout");
        if (CHECK(clips && playhead && markers && inout)) {
            CHECK(clips->isChecked() && playhead->isChecked() && !markers->isChecked() && !inout->isChecked());
            CHECK_EQ(page->snapTargets(), TimelineOptions::kDefaultSnapTargets);
            markers->setChecked(true);
            playhead->setChecked(false);
            page->store();
            CHECK_EQ(TimelineOptions::snapTargets(), TimelineOptions::SnapClipEdges | TimelineOptions::SnapMarkers);
            SettingsDialog again(AiSettings{});
            CHECK(again.timelinePage()->findChild<QCheckBox*>("timeline_snap_markers")->isChecked());
            CHECK(!again.timelinePage()->findChild<QCheckBox*>("timeline_snap_playhead")->isChecked());
            markers->setChecked(false);
            playhead->setChecked(true);
            page->store();
            CHECK(QSettings().allKeys().filter("timeline/").isEmpty());
        }
    }
    // Pictures on video clips: filmstrip (default), first, first and last, none
    checkTimelineChoice("timeline_thumbnails", 4, int(TimelineOptions::Thumbnails::Filmstrip),
                        [] { return int(TimelineOptions::thumbnails()); });
    // Loop without a range: not possible (default) or the whole timeline; remembered: off at every start (default)
    // or as last switched by hand - the wish is kept only while remembering, switching that off forgets it
    checkTimelineChoice("timeline_loop_without_range", 2, 0, [] { return int(TimelineOptions::loopWholeTimeline()); });
    checkTimelineChoice("timeline_remember_loop", 2, 0, [] { return int(TimelineOptions::rememberLoop()); });
    {
        QSettings().remove("timeline");
        TimelineOptions::setLoopWish(true); // not remembering: not stored
        CHECK(!TimelineOptions::loopAtStart());
        CHECK(QSettings().allKeys().filter("timeline/").isEmpty());
        TimelineOptions::setRememberLoop(true);
        CHECK(!TimelineOptions::loopAtStart());
        TimelineOptions::setLoopWish(true);
        CHECK(TimelineOptions::loopAtStart());
        TimelineOptions::setLoopWish(false);
        CHECK(!TimelineOptions::loopAtStart());
        TimelineOptions::setLoopWish(true);
        TimelineOptions::setRememberLoop(false);
        CHECK(!TimelineOptions::loopAtStart());
        CHECK(QSettings().allKeys().filter("timeline/").isEmpty());
    }
    // Ctrl+T's kind: every kind but the gradient wipe (it needs a picture), cross dissolve by default
    checkTimelineChoice("timeline_transition_type", int(std::size(kTransitionTypes)) - 1, int(TransitionType::CrossDissolve),
                        [] { return int(TimelineOptions::transitionType()); });
    {
        QSettings().setValue("timeline/transitionType", "luma"); // not offered: the cross dissolve
        CHECK(TimelineOptions::transitionType() == TransitionType::CrossDissolve);
        CHECK(!TimelineOptions::transitionStyle());
        TimelineOptions::setTransitionType(TransitionType::DipToColor);
        CHECK(TimelineOptions::transitionStyle() && TimelineOptions::transitionStyle()->type == TransitionType::DipToColor);
        QSettings().remove("timeline");
    }
    // Track height: small, medium (default: the view's 64/52 from before), large
    checkTimelineChoice("timeline_track_height", 3, int(TimelineOptions::TrackHeight::Medium),
                        [] { return int(TimelineOptions::trackHeight()); });
    {
        using TH = TimelineOptions::TrackHeight;
        CHECK_EQ(TimelineOptions::videoTrackHeight(TH::Medium), ViewState{}.videoTrackHeight);
        CHECK_EQ(TimelineOptions::audioTrackHeight(TH::Medium), ViewState{}.audioTrackHeight);
        CHECK(TimelineOptions::videoTrackHeight(TH::Small) < 64 && TimelineOptions::videoTrackHeight(TH::Large) > 64);
        CHECK(TimelineOptions::audioTrackHeight(TH::Small) >= 28 && TimelineOptions::videoTrackHeight(TH::Large) <= 220);
    }
    // Waveforms: mirrored (default) or from the bottom, dB (default) or linear
    checkTimelineChoice("timeline_wave_shape", 2, int(TimelineOptions::WaveShape::Mirrored),
                        [] { return int(TimelineOptions::waveShape()); });
    checkTimelineChoice("timeline_wave_scale", 2, int(TimelineOptions::WaveScale::Db),
                        [] { return int(TimelineOptions::waveScale()); });
    checkTimelineChoice("timeline_time_format", 3, int(TimelineOptions::TimeFormat::Timecode),
                        [] { return int(TimelineOptions::timeFormat()); });
    {
        SettingsDialog dlg(AiSettings{});
        auto* tf = dlg.timelinePage()->findChild<QComboBox*>("timeline_time_format");
        if (CHECK(tf)) {
            CHECK(tf->itemText(0).contains("00:00:04:12"));
            CHECK(tf->itemText(2).contains("112"));
        }
    }
}

void testMainWindow()
{
    QSettings().remove("ai");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    QSettings().setValue("project/autosaveSeconds", 120); // the backup's interval reaches the timer at the start
    MainWindow w(&engine);
    QSettings().remove("project/autosaveSeconds");
    CHECK_EQ(w.autosaveIntervalMs(), 120 * 1000);
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

    // The time format reaches what stands still on screen at once: Media Pool durations, Inspector subtitle fields
    QSettings().remove("timeline");
    {
        MainWindow w3(&engine);
        w3.disableAutosave();
        w3.resize(1400, 900);
        w3.show();
        auto* ed = w3.findChild<Editor*>();
        auto* pool = w3.findChild<MediaPool*>();
        auto* inspector = w3.findChild<Inspector*>();
        if (CHECK(ed && pool && inspector)) {
            ed->project()->addMedia(MediaInfo{"/x/lang.mp4", "lang.mp4", 250, true, true, false});
            ed->addSubtitle(100); // selected: the Inspector shows its start
            QApplication::processEvents();
            const auto poolHas = [&](const QString& text) {
                for (auto* list : pool->findChildren<QListWidget*>())
                    for (int i = 0; i < list->count(); ++i)
                        if (list->item(i)->text().contains("lang.mp4") && list->item(i)->text().contains(text)) return true;
                return false;
            };
            const auto inspectorHas = [&](const QString& text) {
                for (auto* edit : inspector->findChildren<QLineEdit*>())
                    if (edit->text() == text) return true;
                return false;
            };
            const int fps = ed->project()->fps();
            CHECK(poolHas(Timecode::format(250, fps)));
            CHECK(inspectorHas(Timecode::format(100, fps)));
            TimelineOptions::setTimeFormat(TimelineOptions::TimeFormat::Frames);
            w3.applyTimelineOptions();
            QApplication::processEvents();
            CHECK(poolHas("    250") || poolHas("\n250"));
            CHECK(inspectorHas("100"));
        }
    }
    QSettings().remove("timeline");
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
    testVocabulary();
    testProjectFolder();
    testTimelineTab();
    testMainWindow();
    return Check::result();
}
