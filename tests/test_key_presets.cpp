// SPDX-License-Identifier: AGPL-3.0-only
// Key presets for people coming from other editors: Premiere Pro and Final Cut Pro on schneidi's actions (keys schneidi's
// own actions lose when the preset takes them, second monitor reachable in every set), the preset stored in
// keybindings.json, a Premiere .kys file imported as a preset of its own ("7 taken, 3 unknown"), the key settings
// dialog (preset combo, import) and the key trainer (rounds, hits, skip, its keys never fire window actions). The dialog
// and the trainer in English and Russian with a 1.25x font: every button's text fits.
// KEY_PRESETS_DUMP=<dir> saves screenshots.
#include "check.h"

#include "app/InputBindings.h"
#include "app/KeyBindingsDialog.h"
#include "app/KeyTrainer.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "engine/Engine.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <clocale>
#include <memory>

namespace {

QString dumpDir() { return qEnvironmentVariable("KEY_PRESETS_DUMP"); }

QString fixture() { return QDir(Check::testData()).filePath("premiere-shortcuts.kys"); }

QStringList in(const QString& id) { return InputBindings::instance().inputs(id); }

void testReadKys()
{
    QFile f(fixture());
    if (!CHECK(f.open(QIODevice::ReadOnly))) return;
    const auto r = InputBindings::readKys(f.readAll());
    CHECK(r.ok);
    CHECK_EQ(r.taken, 7);   // undo, add edit, play, in, zoom in, snap, ripple delete
    CHECK_EQ(r.unknown, 3); // match frame, pen, lift (find has no key: not counted)
    CHECK_EQ(r.keys.value("split"), QStringList({"D", "Ctrl+K"}));
    CHECK_EQ(r.keys.value("zoom_in"), QStringList({"=", "+"})); // number pad + counts as +
    CHECK_EQ(r.keys.value("snapping"), QStringList({"Shift+N"}));
    CHECK_EQ(r.keys.value("ripple_delete"), QStringList({"Alt+Del"}));
    CHECK_EQ(r.keys.value("undo"), QStringList({"Ctrl+Z"}));
    CHECK(!r.keys.contains("find"));

    CHECK(!InputBindings::readKys("not xml at all").ok);
    CHECK(!InputBindings::readKys(R"({"shortcuts": {}})").ok);
    CHECK(!InputBindings::readKys("<html><body>hi</body></html>").ok);
}

// Premiere's contexts: one key may do one thing in the timeline and another one elsewhere. Only the global and the
// timeline context count, the global one wins; a key twice is kept for the first command only
const char* kContexts = R"(<?xml version="1.0" encoding="UTF-8" ?>
<PremiereData Version="3"><shortcuts>
  <context.timeline>
    <item><commandname>cmd.marker.setin</commandname><virtualkey>75</virtualkey></item>
    <item><commandname>cmd.sequence.snap</commandname><virtualkey>78</virtualkey></item>
  </context.timeline>
  <context.global>
    <item><commandname>cmd.edit.undo</commandname><virtualkey>90</virtualkey><modifier.ctrl>true</modifier.ctrl></item>
    <item><commandname>cmd.sequence.addedit</commandname><virtualkey>75</virtualkey></item>
  </context.global>
  <context.panel.project>
    <item><commandname>cmd.sequence.zoomin</commandname><virtualkey>78</virtualkey></item>
    <item><commandname>cmd.tool.pen</commandname><virtualkey>80</virtualkey></item>
  </context.panel.project>
</shortcuts></PremiereData>
)";

// No two actions of one scope on the same key (the multicam angle keys share Alt+1..9 with the video target tracks
// on purpose: only one of them is enabled at a time)
void checkUniqueKeys(const char* where)
{
    const InputBindings& b = InputBindings::instance();
    QHash<QString, QString> owner;
    for (const InputBindings::Entry& e : b.entries())
        for (const QString& input : b.inputs(e.id)) {
            if (input.isEmpty()) continue;
            const QString key = e.scope + '\n' + input;
            const bool angle = e.id.startsWith("multicam_switch_")
                               && owner.value(key) == "target_video_" + e.id.mid(int(strlen("multicam_switch_")));
            if (owner.contains(key) && !angle)
                Check::report(false, "a key on two actions", __FILE__, __LINE__,
                              QString("%1: %2 on %3 and %4").arg(where, input, owner[key], e.id));
            owner.insert(key, e.id);
        }
}

void testKysContexts()
{
    const auto r = InputBindings::readKys(kContexts);
    CHECK(r.ok);
    CHECK_EQ(r.keys.value("split"), QStringList({"K"})); // global before the timeline's K
    CHECK(!r.keys.contains("mark_in"));
    CHECK_EQ(r.keys.value("snapping"), QStringList({"N"}));
    CHECK(!r.keys.contains("zoom_in")); // only in the project panel there
    CHECK_EQ(r.taken, 3);
    CHECK_EQ(r.unknown, 0); // the pen in another panel is not counted at all
    CHECK_EQ(r.dropped, 2);

    // Imported: one action per key, the summary says what was left out
    InputBindings& b = InputBindings::instance();
    b.applyPreset("standard");
    QTemporaryDir tmp;
    const QString path = QDir(tmp.path()).filePath("contexts.kys");
    QFile f(path);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write(kContexts);
    f.close();
    KeyBindingsDialog dlg;
    auto* summary = dlg.findChild<QLabel*>("key_import_summary");
    if (!CHECK(summary && dlg.importFile(path))) return;
    CHECK(summary->text().contains(T("%1 Tasten weggelassen (nur für ein anderes Premiere-Fenster oder schon "
                                     "von einem anderen Befehl belegt).").arg(2)));
    CHECK_EQ(in("split"), QStringList({"K", ""}));
    CHECK(!in("mark_in").contains("K"));
    checkUniqueKeys("contexts.kys");
    for (const char* preset : {"standard", "beginner", "premiere", "finalcut"}) {
        b.applyPreset(preset);
        checkUniqueKeys(preset);
    }
    b.applyPreset("standard");
}

void testPresets()
{
    InputBindings& b = InputBindings::instance();
    const QStringList ids = InputBindings::presetIds();
    for (const char* id : {"standard", "beginner", "premiere", "finalcut"}) CHECK(ids.contains(id));
    CHECK_EQ(InputBindings::presetName("premiere"), QString("Premiere Pro"));
    CHECK_EQ(InputBindings::presetName("finalcut"), QString("Final Cut Pro"));

    // The second monitor stays reachable in every set
    for (const QString& id : ids) {
        b.applyPreset(id);
        CHECK(!b.current("viewer_second_screen").isEmpty());
        CHECK_EQ(b.currentPreset(), id);
    }

    b.applyPreset("premiere");
    CHECK_EQ(in("split").value(0), QString("Ctrl+K"));
    CHECK_EQ(in("split").value(1), QString("mouse:back")); // the default side button stays
    CHECK_EQ(in("tool_blade").value(0), QString("C"));
    CHECK_EQ(in("tool_select").value(0), QString("V"));
    CHECK_EQ(in("mark_in").value(0), QString("I"));
    CHECK_EQ(in("insert_clip").value(0), QString(","));
    CHECK_EQ(in("snapping").value(0), QString("S"));
    CHECK_EQ(in("zoom_in").value(0), QString("="));
    CHECK_EQ(in("viewer_second_screen").value(0), QString("Ctrl+Shift+F"));
    // schneidi's own actions keep their key while it is free, else they have none
    CHECK_EQ(in("select_edit_point"), QStringList({"", ""}));    // V = Selection Tool
    CHECK_EQ(in("grade_prev"), QStringList({"", ""}));           // = zooms
    CHECK_EQ(in("show_timeline"), QStringList({"", ""}));        // Q = Ripple Trim Previous
    CHECK_EQ(in("import_timeline"), QStringList({"", ""}));      // Ctrl+Shift+I clears the in point
    CHECK_EQ(in("curve_editor").value(0), QString("Shift+C"));   // free: stays
    CHECK_EQ(in("color_bypass").value(0), QString("Shift+D"));
    // Takes effect on the actions right away
    CHECK(b.action("split")->shortcuts().contains(QKeySequence("Ctrl+K")));
    CHECK(b.action("select_edit_point")->shortcuts().isEmpty());

    b.applyPreset("finalcut");
    CHECK_EQ(in("split").value(0), QString("Ctrl+B"));
    CHECK_EQ(in("ripple_delete").value(0), QString("Backspace"));
    CHECK_EQ(in("delete").value(0), QString("Shift+Backspace"));
    CHECK_EQ(in("insert_clip").value(0), QString("W"));
    CHECK_EQ(in("overwrite_clip").value(0), QString("D"));
    CHECK_EQ(in("toggle_enabled").value(0), QString("V"));
    CHECK_EQ(in("viewer_full_screen").value(0), QString("Ctrl+Shift+F"));
    CHECK_EQ(in("viewer_second_screen").value(0), QString("Ctrl+F12"));
    CHECK_EQ(in("select_edit_point"), QStringList({"", ""}));

    // The preset is remembered in the file (optional field) and comes back with load()
    QFile f(b.filePath());
    if (CHECK(f.open(QIODevice::ReadOnly)))
        CHECK_EQ(QJsonDocument::fromJson(f.readAll()).object().value("preset").toString(), QString("finalcut"));
    f.close();
    b.load();
    CHECK_EQ(b.currentPreset(), QString("finalcut"));

    // presetInputs is a dry run: neither the actions nor the current set change
    const auto premiere = b.presetInputs("premiere");
    CHECK_EQ(premiere.value("split").value(0), QString("Ctrl+K"));
    CHECK_EQ(in("split").value(0), QString("Ctrl+B"));
    CHECK(b.action("split")->shortcuts().contains(QKeySequence("Ctrl+B")));
    CHECK_EQ(b.currentPreset(), QString("finalcut"));

    b.applyPreset("standard");
    CHECK_EQ(in("split").value(0), QString("Ctrl+B"));
    CHECK_EQ(in("select_edit_point").value(0), QString("V"));

    // KEY_PRESETS_LIST=1: which keys schneidi's actions lose per preset (for the docs)
    if (qEnvironmentVariableIsSet("KEY_PRESETS_LIST"))
        for (const char* preset : {"premiere", "finalcut"}) {
            const auto set = b.presetInputs(preset);
            for (const auto& e : b.entries())
                for (int s = 0; s < InputBindings::kSlots; ++s) {
                    const QString was = b.defaultInputs(e.id)[s], now = set.value(e.id).value(s);
                    if (!was.isEmpty() && now.isEmpty())
                        std::printf("%s: %s loses %s\n", preset, qPrintable(e.id), qPrintable(was));
                }
        }
}

void testImport()
{
    InputBindings& b = InputBindings::instance();
    b.applyPreset("standard");
    KeyBindingsDialog dlg;
    auto* combo = dlg.findChild<QComboBox*>("key_preset");
    auto* summary = dlg.findChild<QLabel*>("key_import_summary");
    if (!CHECK(combo && summary)) return;
    CHECK_EQ(combo->currentData().toString(), QString("standard"));
    CHECK(dlg.importFile(fixture()));
    const QString id = "custom:premiere-shortcuts";
    CHECK_EQ(b.currentPreset(), id);
    CHECK(InputBindings::presetIds().contains(id));
    CHECK_EQ(combo->currentData().toString(), id);
    CHECK(combo->findData(id) >= 0);
    CHECK(summary->text().contains("7") && summary->text().contains("3"));
    CHECK_EQ(in("split"), QStringList({"D", "Ctrl+K"}));   // both keys of the file, no side button
    CHECK_EQ(in("snapping"), QStringList({"Shift+N", ""}));
    CHECK_EQ(in("ripple_delete"), QStringList({"Alt+Del", ""}));
    CHECK_EQ(in("tool_blade").value(0), QString("C")); // not in the file: Premiere's
    CHECK_EQ(in("toggle_enabled").value(0), QString("Shift+E"));

    // A preset of its own: another set and back
    b.applyPreset("standard");
    CHECK_EQ(in("split").value(0), QString("Ctrl+B"));
    b.applyPreset(id);
    CHECK_EQ(in("split"), QStringList({"D", "Ctrl+K"}));
    CHECK_EQ(b.currentPreset(), id);

    // Not a shortcut file: an error, the set stays
    QTemporaryDir tmp;
    const QString junk = QDir(tmp.path()).filePath("junk.kys");
    QFile j(junk);
    j.open(QIODevice::WriteOnly);
    j.write("<html/>");
    j.close();
    QString error;
    CHECK(b.importKys(junk, nullptr, &error).isEmpty());
    CHECK(!error.isEmpty());
    CHECK_EQ(b.currentPreset(), id);
    // The preset file cannot be written (a folder in its place): an error, the set in memory stays as it was
    {
        const QStringList split = in("split");
        QDir(InputBindings::customPresetDir()).mkpath("blocked.json");
        const QString blocked = QDir(tmp.path()).filePath("blocked.kys");
        QFile::copy(fixture(), blocked);
        error.clear();
        CHECK(b.importKys(blocked, nullptr, &error).isEmpty());
        CHECK(!error.isEmpty());
        CHECK_EQ(b.currentPreset(), id);
        CHECK_EQ(in("split"), split);
        CHECK_EQ(in("tool_blade").value(0), QString("C"));
        QFile set(b.filePath());
        if (CHECK(set.open(QIODevice::ReadOnly)))
            CHECK_EQ(QJsonDocument::fromJson(set.readAll()).object().value("preset").toString(), id);
        b.load(); // what the file has, which must be the same
        CHECK_EQ(b.currentPreset(), id);
        CHECK_EQ(in("split"), split);
        QDir(InputBindings::customPresetDir()).rmdir("blocked.json");
    }
    // Names Windows cannot have as a file: made safe
    for (const auto& [file, want] : {std::pair<QString, QString>{"con.kys", "con_"}, {"LPT1.old.kys", "LPT1_.old"},
                                     {"set. .kys", "set"}, {"console.kys", "console"}}) {
        const QString p = QDir(tmp.path()).filePath(file);
        QFile::copy(fixture(), p);
        error.clear();
        CHECK_EQ(b.importKys(p, nullptr, &error), "custom:" + want);
        CHECK(QFileInfo::exists(QDir(InputBindings::customPresetDir()).filePath(want + ".json")));
    }
    b.applyPreset(id);
    // Larger than any shortcut file (5 MB): refused before reading
    const QString huge = QDir(tmp.path()).filePath("huge.kys");
    QFile h(huge);
    if (CHECK(h.open(QIODevice::WriteOnly))) {
        h.write("<PremiereData><shortcuts>");
        h.resize(6 * 1024 * 1024);
        h.close();
    }
    error.clear();
    CHECK(b.importKys(huge, nullptr, &error).isEmpty());
    CHECK_EQ(error, T("Die Datei ist zu groß für eine Tastenbelegung."));
    CHECK_EQ(b.currentPreset(), id);
    b.applyPreset("standard");
}

QKeyCombination comboOf(const QString& input)
{
    return QKeySequence::fromString(input, QKeySequence::PortableText)[0];
}

void press(QWidget* w, const QString& input)
{
    const QKeyCombination k = comboOf(input);
    QTest::keyClick(w, k.key(), k.keyboardModifiers());
    QApplication::processEvents();
}

void testTrainer(MainWindow& w)
{
    InputBindings& b = InputBindings::instance();
    for (const QString& preset : {QString("standard"), QString("premiere"), QString("finalcut")}) {
        b.applyPreset(preset);
        CHECK(KeyTrainer::candidates().size() >= KeyTrainer::kRounds);
    }
    b.applyPreset("premiere");
    auto* t = new KeyTrainer(&w, 42);
    t->show();
    t->activateWindow();
    t->setFocus();
    QApplication::processEvents();
    CHECK_EQ(t->round(), 0);
    CHECK(!t->currentId().isEmpty());

    // Window actions stay quiet while the card has the focus (Space does not play)
    QSignalSpy played(b.action("play_pause"), &QAction::triggered);
    QSignalSpy split(b.action("split"), &QAction::triggered);

    // Round 1: a wrong key first (no hit), then the right one
    const QString first = t->currentId();
    press(t, "F7");
    CHECK_EQ(t->round(), 0);
    press(t, b.inputs(first).value(0));
    CHECK_EQ(t->round(), 1);
    CHECK_EQ(t->hits(), 0);
    // Round 2 skipped
    t->findChild<QPushButton*>("trainer_skip")->click();
    CHECK_EQ(t->round(), 2);
    // The rest right on the first try
    while (!t->finished()) {
        const QString id = t->currentId();
        QString key;
        for (const QString& k : b.inputs(id))
            if (!k.isEmpty() && InputBindings::inputMouseButton(k) == Qt::NoButton) {
                key = k;
                break;
            }
        if (!CHECK(!key.isEmpty())) break;
        const int round = t->round();
        press(t, key);
        if (!CHECK_EQ(t->round(), round + 1)) break;
    }
    CHECK(t->finished());
    CHECK_EQ(t->hits(), KeyTrainer::kRounds - 2);
    CHECK_EQ(played.count(), 0);
    CHECK_EQ(split.count(), 0);
    CHECK(!t->findChild<QPushButton*>("trainer_skip")->isVisible());
    CHECK(t->findChild<QPushButton*>("trainer_again")->isVisible());
    t->findChild<QPushButton*>("trainer_again")->click();
    CHECK_EQ(t->round(), 0);
    CHECK_EQ(t->hits(), 0);
    t->close();
    QApplication::processEvents();
    b.applyPreset("standard");
}

// Every button's text fits into the button (font metrics against its width), every label is wide enough for one
// word at least
void checkFits(QWidget* root, const QString& where)
{
    for (QPushButton* p : root->findChildren<QPushButton*>()) {
        if (!p->isVisible() || p->text().isEmpty()) continue;
        const int need = p->fontMetrics().horizontalAdvance(QString(p->text()).remove('&')) + 8;
        Check::report(need <= p->width(), "button text cut", __FILE__, __LINE__,
                      QString("%1: \"%2\" needs %3 px, has %4").arg(where, p->text()).arg(need).arg(p->width()));
    }
    for (QLabel* l : root->findChildren<QLabel*>()) {
        if (!l->isVisible() || l->text().isEmpty() || l->wordWrap()) continue;
        const int need = l->fontMetrics().horizontalAdvance(l->text());
        Check::report(need <= l->contentsRect().width() + 1, "label text cut", __FILE__, __LINE__,
                      QString("%1: \"%2\" needs %3 px, has %4").arg(where, l->text()).arg(need).arg(l->width()));
    }
    for (QComboBox* c : root->findChildren<QComboBox*>()) {
        if (!c->isVisible()) continue;
        const int need = c->fontMetrics().horizontalAdvance(c->currentText()) + 30;
        Check::report(need <= c->width(), "combo text cut", __FILE__, __LINE__,
                      QString("%1: \"%2\" needs %3 px, has %4").arg(where, c->currentText()).arg(need).arg(c->width()));
    }
}

void testLanguages(Engine& engine)
{
    const QFont normal = QApplication::font();
    QFont big = normal;
    big.setPointSizeF(normal.pointSizeF() * 1.25);
    QApplication::setFont(big);
    for (const char* lang : {"en", "ru"}) {
        I18n::install(lang);
        auto w = std::make_unique<MainWindow>(&engine);
        w->disableAutosave();
        w->resize(1600, 950);
        w->show();
        QApplication::processEvents();
        InputBindings::instance().applyPreset("premiere");

        {
            KeyBindingsDialog dlg(w.get());
            dlg.show();
            QApplication::processEvents();
            dlg.importFile(fixture()); // the summary line shows
            QApplication::processEvents();
            checkFits(&dlg, QString("dialog %1").arg(lang));
            if (!dumpDir().isEmpty()) dlg.grab().save(dumpDir() + QString("/keys-dialog-%1.png").arg(lang));
        }

        auto* t = new KeyTrainer(w.get(), 7);
        t->show();
        QApplication::processEvents();
        press(t, "F7"); // the "that was F7" line
        QApplication::processEvents();
        checkFits(t, QString("trainer %1").arg(lang));
        if (!dumpDir().isEmpty()) t->grab().save(dumpDir() + QString("/trainer-%1.png").arg(lang));
        while (!t->finished()) t->skip();
        QApplication::processEvents();
        checkFits(t, QString("trainer end %1").arg(lang));
        if (!dumpDir().isEmpty()) t->grab().save(dumpDir() + QString("/trainer-end-%1.png").arg(lang));
        t->close();
        InputBindings::instance().applyPreset("standard");
        w->close();
        w.reset();
        QApplication::processEvents();
    }
    QApplication::setFont(normal);
    I18n::install("de");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("key_presets");
    I18n::install("de");
    Theme::apply(app);
    InputBindings::instance().load();
    testReadKys();

    Check::initEngine();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.resize(1600, 950);
        w.show();
        w.activateWindow();
        QApplication::processEvents();
        testPresets();
        testImport();
        testKysContexts();
        testTrainer(w);
    }
    testLanguages(engine);
    return Check::result();
}
