// SPDX-License-Identifier: AGPL-3.0-only
// Key presets for people coming from other editors: Premiere Pro and Final Cut Pro on schneidi's actions (keys schneidi's
// own actions lose when the preset takes them, second monitor reachable in every set), the preset stored in
// keybindings.json, a Premiere .kys file (in the layout and key codes Premiere saves) imported as a preset of its own ("9 taken, 3 unknown"), the key settings
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
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <clocale>
#include <functional>
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
    // undo, add edit, play, set in, zoom in, snap, ripple delete (global and timeline), step forward
    CHECK_EQ(r.taken, 9);
    CHECK_EQ(r.unknown, 3); // match frame, pen, help (find has no key: not counted)
    CHECK_EQ(r.dropped, 1); // maximize frame: program monitor only (the loop there is unknown: not counted)
    CHECK_EQ(r.keys.value("split"), QStringList({"D", "Ctrl+K"}));
    CHECK_EQ(r.keys.value("zoom_in"), QStringList({"=", "+"})); // number pad + counts as +
    CHECK_EQ(r.keys.value("snapping"), QStringList({"Shift+N"}));
    CHECK_EQ(r.keys.value("ripple_delete"), QStringList({"Alt+Del", "Alt+Backspace"})); // the timeline's after the global
    CHECK_EQ(r.keys.value("undo"), QStringList({"Ctrl+Z"}));
    CHECK_EQ(r.keys.value("play_pause"), QStringList({"Space"}));
    CHECK_EQ(r.keys.value("mark_in"), QStringList({"I"}));
    CHECK_EQ(r.keys.value("frame_next"), QStringList({"Right"}));
    CHECK(!r.keys.contains("viewer_large"));
    CHECK(!r.keys.contains("find"));

    // Premiere's key codes: characters with the top bit, the number pad with the next one, other keys by number
    const auto key = [](qint64 code, const char* mods = "") {
        const QString item = QString("<item.0 Version=\"1\"><commandname>cmd.edit.undo</commandname>%1"
                                     "<virtualkey>%2</virtualkey></item.0>")
                                 .arg(QString::fromLatin1(mods))
                                 .arg(code);
        const auto r = InputBindings::readKys(
            QString("<PremiereData Version=\"3\"><shortcuts Version=\"5\"><platform>windows</platform>"
                    "<context.global Version=\"1\"><itemcount>1</itemcount>%1</context.global></shortcuts></PremiereData>")
                .arg(item)
                .toUtf8());
        return r.keys.value("undo").value(0);
    };
    CHECK_EQ(key(0x80000047), QString("G"));
    CHECK_EQ(key(qint64(qint32(0x80000047))), QString("G")); // the same code written as a signed number
    CHECK_EQ(key(qint64(qint32(0xC000002B))), QString("+"));
    CHECK_EQ(key(0x80000035, "<modifier.shift>true</modifier.shift>"), QString("Shift+5"));
    CHECK_EQ(key(0x8000002C), QString(","));
    CHECK_EQ(key(0x80000060), QString("`"));
    CHECK_EQ(key(0xC0000037), QString("7")); // number pad 7
    CHECK_EQ(key(0xC000002A), QString("*"));
    CHECK_EQ(key(1), QString("Space"));
    CHECK_EQ(key(2), QString("Backspace"));
    CHECK_EQ(key(4), QString("Return"));
    CHECK_EQ(key(7), QString("F1"));
    CHECK_EQ(key(12), QString("F6"));
    CHECK_EQ(key(18), QString("F12"));
    CHECK_EQ(key(35), QString("Del"));
    CHECK_EQ(key(36), QString("Home"));
    CHECK_EQ(key(39), QString("PgDown"));
    CHECK_EQ(key(42), QString("Left"));
    CHECK_EQ(key(45), QString("Down"));
    CHECK_EQ(key(90), QString()); // no Premiere code (a Windows VK 'Z' is not what Premiere writes)
    CHECK_EQ(key(0x80000041, "<modifier.ctrl>true</modifier.ctrl><modifier.alt>true</modifier.alt>"), QString("Ctrl+Alt+A"));

    CHECK(!InputBindings::readKys("not xml at all").ok);
    CHECK(!InputBindings::readKys(R"({"shortcuts": {}})").ok);
    CHECK(!InputBindings::readKys("<html><body>hi</body></html>").ok);
}

// Premiere's contexts: one key may do one thing in the timeline and another one elsewhere. Only the global and the
// timeline context count, the global one wins; a key twice is kept for the first command only
const char* kContexts = R"(<?xml version="1.0" encoding="UTF-8"?>
<PremiereData Version="3"><shortcuts Version="5"><platform>windows</platform>
  <context.timeline Version="1"><itemcount>2</itemcount>
    <item.0 Version="1"><commandname>cmd.common.setin</commandname><modifier.shift>false</modifier.shift><modifier.alt>false</modifier.alt><modifier.ctrl>false</modifier.ctrl><virtualkey>2147483723</virtualkey></item.0>
    <item.1 Version="1"><commandname>cmd.sequence.snap</commandname><modifier.shift>false</modifier.shift><modifier.alt>false</modifier.alt><modifier.ctrl>false</modifier.ctrl><virtualkey>2147483726</virtualkey></item.1>
  </context.timeline>
  <context.global Version="1"><itemcount>2</itemcount>
    <item.0 Version="1"><commandname>cmd.edit.undo</commandname><modifier.shift>false</modifier.shift><modifier.alt>false</modifier.alt><modifier.ctrl>true</modifier.ctrl><virtualkey>2147483738</virtualkey></item.0>
    <item.1 Version="1"><commandname>cmd.sequence.razorateditline</commandname><modifier.shift>false</modifier.shift><modifier.alt>false</modifier.alt><modifier.ctrl>false</modifier.ctrl><virtualkey>2147483723</virtualkey></item.1>
  </context.global>
  <context.project Version="1"><itemcount>2</itemcount>
    <item.0 Version="1"><commandname>cmd.zoom.in</commandname><modifier.shift>false</modifier.shift><modifier.alt>false</modifier.alt><modifier.ctrl>false</modifier.ctrl><virtualkey>2147483726</virtualkey></item.0>
    <item.1 Version="1"><commandname>cmd.tools.09pen</commandname><modifier.shift>false</modifier.shift><modifier.alt>false</modifier.alt><modifier.ctrl>false</modifier.ctrl><virtualkey>2147483728</virtualkey></item.1>
  </context.project>
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

// A key every common keyboard layout can type without a dead key or AltGr: letters, digits, F keys and named keys
// ([ ] \ ` need AltGr on German, French, Nordic... layouts or sit elsewhere)
bool layoutNeutral(const QString& input)
{
    if (input.isEmpty() || input.startsWith("mouse:")) return false;
    const QKeyCombination k = QKeySequence::fromString(input, QKeySequence::PortableText)[0];
    const int key = k.key();
    const bool plain = (key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9)
                       || (key >= Qt::Key_F1 && key <= Qt::Key_F24) || key >= 0x01000000;
    return plain && !(k.keyboardModifiers().testFlags(Qt::ControlModifier | Qt::AltModifier));
}

// Trim start/end and the alternative split have a second key every layout can type, in every preset (Shift+[ / Shift+]
// and Ctrl+\ cannot be pressed on QWERTZ and most non-US layouts); a keybindings.json from before gets it too
void testLayoutNeutralKeys()
{
    InputBindings& b = InputBindings::instance();
    for (const char* preset : {"standard", "beginner", "premiere", "finalcut"}) {
        b.applyPreset(preset);
        for (const char* id : {"trim_start", "trim_end", "split_alt"}) {
            const QStringList keys = in(id);
            Check::report(std::any_of(keys.begin(), keys.end(), layoutNeutral), "no layout-neutral key", __FILE__,
                          __LINE__, QString("%1: %2 has %3").arg(preset, id, keys.join(", ")));
        }
        checkUniqueKeys(preset);
    }
    b.applyPreset("standard");
    CHECK_EQ(in("trim_start"), QStringList({"Shift+[", "Shift+Q"}));
    CHECK_EQ(in("trim_end"), QStringList({"Shift+]", "Shift+W"}));
    CHECK_EQ(in("split_alt"), QStringList({"Ctrl+\\", "Shift+B"}));
    // A file of version 3: the new second keys come in where the slot is empty and the key is free
    {
        QFile f(b.filePath());
        if (CHECK(f.open(QIODevice::WriteOnly)))
            f.write(R"({"version": 3, "shortcuts": {"trim_start": "Shift+[", "trim_end": ["Shift+]", "E"],
                        "split_alt": "Ctrl+\\", "add_marker": "Shift+B"}})");
    }
    b.load();
    CHECK_EQ(in("trim_start"), QStringList({"Shift+[", "Shift+Q"}));
    CHECK_EQ(in("trim_end"), QStringList({"Shift+]", "E"})); // the user's own second key stays
    CHECK_EQ(in("split_alt"), QStringList({"Ctrl+\\", ""}));  // Shift+B is the user's marker key
    CHECK(b.action("trim_start")->shortcuts().contains(QKeySequence("Shift+Q")));
    b.applyPreset("standard");
}

// The table row of an action in the key dialog
int rowOf(QTableWidget* table, const QString& id)
{
    for (int r = 0; r < table->rowCount(); ++r)
        if (table->item(r, 1)->data(Qt::UserRole).toString() == id) return r;
    return -1;
}

// Captures a key in the dialog like the user: double click on the slot, then the key goes to the capture field
void capture(KeyBindingsDialog& dlg, const QString& id, int slot, const std::function<void(QWidget*)>& press)
{
    auto* table = dlg.findChild<QTableWidget*>();
    const int row = rowOf(table, id);
    if (!CHECK(row >= 0)) return;
    emit table->cellDoubleClicked(row, 2 + slot);
    QWidget* field = table->cellWidget(row, 2 + slot);
    if (!CHECK(field)) return;
    press(field);
    QApplication::processEvents();
}

// A key already used in the same scope: the other action loses it (two actions on one key would both stop working), the
// dialog says so. Files (keybindings.json, a loaded set) with a key twice: the first keeps it, the other loses it
void testDuplicates()
{
    InputBindings& b = InputBindings::instance();
    b.applyPreset("standard");
    CHECK_EQ(in("add_marker").value(0), QString("M"));
    const auto taken = b.setInput("split", 0, "M");
    CHECK_EQ(taken.size(), 1);
    CHECK(taken.value(0).id == "add_marker" && taken.value(0).input == "M");
    CHECK(!in("add_marker").contains("M"));
    CHECK(!b.action("add_marker")->shortcuts().contains(QKeySequence("M")));
    checkUniqueKeys("setInput");
    // A panel's key is no conflict with the window's (Del in the render queue and on the timeline)
    b.setInput("delete", 1, "Del");
    CHECK(in("render_queue_delete").contains("Del"));
    CHECK(!in("delete_alt").contains("Del"));
    // The multicam angle keys share Alt+N with the target tracks on purpose
    b.applyPreset("standard");
    b.setInput("target_video_1", 0, "Alt+1");
    CHECK(in("multicam_switch_1").contains("Alt+1"));

    // In the dialog: the notice line names the key and the action it was taken from
    b.applyPreset("standard");
    {
        KeyBindingsDialog dlg;
        dlg.show();
        auto* notice = dlg.findChild<QLabel*>("key_taken_notice");
        if (!CHECK(notice)) return;
        CHECK(!notice->isVisibleTo(&dlg));
        capture(dlg, "split", 1, [](QWidget* f) { QTest::keyClick(f, Qt::Key_M); });
        CHECK_EQ(in("split").value(1), QString("M"));
        CHECK(!in("add_marker").contains("M"));
        CHECK(notice->isVisibleTo(&dlg));
        CHECK(notice->text().contains("M"));
        CHECK(notice->text().contains(QString(b.action("add_marker")->text()).remove('&')));
    }

    // A file with a key twice
    QTemporaryDir tmp;
    const QString path = QDir(tmp.path()).filePath("set.json");
    const auto write = [&](const QByteArray& json) {
        QFile f(path);
        if (CHECK(f.open(QIODevice::WriteOnly))) f.write(json);
    };
    write(R"({"version": 3, "shortcuts": {"split": "M"}})"); // add_marker keeps its default M: the file's wins
    QString error;
    CHECK(b.importSet(path, &error));
    CHECK_EQ(in("split").value(0), QString("M"));
    CHECK(!in("add_marker").contains("M"));
    checkUniqueKeys("importSet");
    write(R"({"version": 3, "shortcuts": {"split": "M", "add_marker": "M"}})");
    CHECK(b.importSet(path, &error));
    CHECK((in("split").value(0) == "M") != (in("add_marker").value(0) == "M"));
    checkUniqueKeys("importSet both");
    // keybindings.json edited by hand
    {
        QFile f(b.filePath());
        if (CHECK(f.open(QIODevice::WriteOnly)))
            f.write(R"({"version": 3, "shortcuts": {"split": "M", "add_marker": "M", "mark_in": "M"}})");
    }
    b.load();
    checkUniqueKeys("load");
    b.applyPreset("standard");
}

// A key event as the platform delivers it: Qt key, modifiers, text and the native codes (X11/Wayland: xkb key code =
// evdev code + 8 and keysym; Windows: scan code and virtual key)
void sendKey(QWidget* w, int key, Qt::KeyboardModifiers mods, const QString& text, quint32 scanCode, quint32 virtualKey)
{
    QKeyEvent press(QEvent::KeyPress, key, mods, scanCode, virtualKey, 0, text);
    QApplication::sendEvent(w, &press);
}

// Capturing in the dialog under other keyboard layouts: a Russian layout gives the Latin key of the same physical key
// (the one shortcuts match), Windows' AltGr (= Ctrl+Alt there) is not taken as Ctrl+Alt
void testCaptureLayouts()
{
    InputBindings& b = InputBindings::instance();
    b.applyPreset("standard");
    KeyBindingsDialog dlg;
    dlg.show();
    // Russian: the key that is A on a US keyboard gives "Ф" (X11 key code 38, Windows VK_A)
    capture(dlg, "add_marker", 1, [](QWidget* f) { sendKey(f, 0x0424, Qt::NoModifier, QString(QChar(0x0444)), 38, 0x41); });
    CHECK_EQ(in("add_marker").value(1), QString("A"));
    capture(dlg, "add_marker", 1,
            [](QWidget* f) { sendKey(f, 0x0421, Qt::ControlModifier, QString(), 54, 0x43); }); // Ctrl+С = Ctrl+C key
    CHECK_EQ(in("add_marker").value(1), QString("Ctrl+C"));
    // Latin letters of other layouts stay as they are (German Ü, French É)
    capture(dlg, "add_marker", 1, [](QWidget* f) { sendKey(f, Qt::Key_Udiaeresis, Qt::NoModifier, "ü", 34, 0xBA); });
    CHECK_EQ(in("add_marker").value(1), QString(QChar(0xDC)));
    // A real Ctrl+Alt combination stays one
    capture(dlg, "add_marker", 1,
            [](QWidget* f) { sendKey(f, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier, QString(), 44, 0x4A); });
    CHECK_EQ(in("add_marker").value(1), QString("Ctrl+Alt+J"));
    b.applyPreset("standard");

    // The native codes of each platform (whichever this one is)
    using N = InputBindings::NativeKeys;
    const auto key = [](N native, int qtKey, Qt::KeyboardModifiers mods, const QString& text, quint32 scan, quint32 vk,
                        quint32 nativeMods = 0) {
        QKeyEvent e(QEvent::KeyPress, qtKey, mods, scan, vk, nativeMods, text);
        return QKeySequence(InputBindings::bindingKey(&e, native)).toString(QKeySequence::PortableText);
    };
    CHECK_EQ(key(N::Windows, 0x0424, {}, QString(QChar(0x0444)), 0x1E, 0x41), QString("A")); // VK_A
    CHECK_EQ(key(N::Xkb, 0x0424, {}, QString(QChar(0x0444)), 38, 0x6c6), QString("A"));      // key code 38
    CHECK_EQ(key(N::Xkb, 0x0417, Qt::ShiftModifier, QString(QChar(0x0417)), 33, 0x6fa), QString("Shift+P"));
    CHECK_EQ(key(N::Xkb, 0x03A9, {}, QString(QChar(0x03C9)), 55, 0x7f9), QString("V")); // Greek omega on the V key
    CHECK_EQ(key(N::Xkb, Qt::Key_Udiaeresis, {}, "ü", 34, 0xfc), QString(QChar(0xDC)));
    // Windows AltGr (left Ctrl + right Alt) typing "[" (German AltGr+8): "[", not Ctrl+Alt+[
    const auto ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
    CHECK_EQ(key(N::Windows, Qt::Key_BracketLeft, ctrlAlt, "[", 0x09, 0x38, 0x42), QString("["));
    CHECK_EQ(key(N::Windows, Qt::Key_At, ctrlAlt, "@", 0x10, 0x51, 0x42), QString("@")); // AltGr+Q
    // Both Ctrl and Alt held by the user (no character): stays
    CHECK_EQ(key(N::Windows, Qt::Key_J, ctrlAlt, QString(), 0x24, 0x4A, 0x06), QString("Ctrl+Alt+J"));
    // X11/Wayland: AltGr is a level shift there, a Ctrl+Alt+digit with text stays
    CHECK_EQ(key(N::Xkb, Qt::Key_1, ctrlAlt, "1", 10, 0x31, 0x0c), QString("Ctrl+Alt+1"));
    CHECK_EQ(key(N::Xkb, Qt::Key_Backtab, Qt::ShiftModifier, QString(), 23, 0xfe20), QString("Shift+Tab"));
}

// Esc can be bound: capturing takes it like any key (cancel = click elsewhere or the small cancel button). On a window
// action it is marked as a conflict: Esc also ends the large viewer and a voice-over take
void testCaptureEscape()
{
    InputBindings& b = InputBindings::instance();
    b.applyPreset("standard");
    KeyBindingsDialog dlg;
    dlg.show();
    auto* table = dlg.findChild<QTableWidget*>();
    capture(dlg, "deselect", 1, [](QWidget* f) { QTest::keyClick(f, Qt::Key_Escape); });
    CHECK_EQ(in("deselect").value(1), QString("Esc"));
    const int row = rowOf(table, "deselect");
    if (CHECK(row >= 0)) {
        QTableWidgetItem* item = table->item(row, 3);
        CHECK(item->foreground().color() == Theme::warning);
        CHECK(!item->toolTip().isEmpty());
    }
    // The cancel button: nothing changes
    capture(dlg, "deselect", 0, [](QWidget* f) {
        auto* cancel = f->findChild<QAbstractButton*>("key_capture_cancel");
        if (CHECK(cancel)) cancel->click();
    });
    CHECK(!table->cellWidget(rowOf(table, "deselect"), 2));
    CHECK_EQ(in("deselect").value(1), QString("Esc"));
    b.applyPreset("standard");
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
    CHECK(summary->text().contains("9") && summary->text().contains("3"));
    CHECK_EQ(in("split"), QStringList({"D", "Ctrl+K"}));   // both keys of the file, no side button
    CHECK_EQ(in("snapping"), QStringList({"Shift+N", ""}));
    CHECK_EQ(in("ripple_delete"), QStringList({"Alt+Del", "Alt+Backspace"}));
    CHECK_EQ(in("play_pause").value(0), QString("Space"));
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
    // A symbol that needs Shift on the keyboard (a German "=" is Shift+0): right with Shift too
    const QString asked = t->currentId();
    const QStringList kept = b.inputs(asked);
    b.setInputs(asked, {"=", ""});
    QTest::keyClick(t, Qt::Key_Equal, Qt::ShiftModifier);
    QApplication::processEvents();
    CHECK_EQ(t->round(), 1);
    CHECK_EQ(t->hits(), 1);
    b.setInputs(asked, kept);
    // ... but a letter with Shift is another key
    b.setInputs(t->currentId(), {"K", ""});
    QTest::keyClick(t, Qt::Key_K, Qt::ShiftModifier);
    QApplication::processEvents();
    CHECK_EQ(t->round(), 1);
    // Esc closes the card
    QPointer<KeyTrainer> card = t;
    QTest::keyClick(t, Qt::Key_Escape);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    CHECK(!card);
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
        testDuplicates();
        testLayoutNeutralKeys();
        testCaptureLayouts();
        testCaptureEscape();
        testTrainer(w);
    }
    testLanguages(engine);
    return Check::result();
}
