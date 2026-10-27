// Test workspace presets: whole key sets saved to a file and loaded again (InputBindings::exportSet/importSet),
// built-in sets (InputBindings::applyPreset), two slots per action that take keys or mouse buttons, old files.
#include "check.h"

#include "app/InputBindings.h"
#include "app/KeyBindingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QTableWidget>
#include <QTemporaryDir>

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("workspace");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    QFile::remove(InputBindings::instance().filePath()); // test config folder: start without a file

    InputBindings& b = InputBindings::instance();
    b.load();
    QAction split, play, extra;
    b.registerAction(&split, "split", "Edit", QKeySequence("Ctrl+B"));
    b.registerAction(&play, "play", "Playback", QKeySequence("Space"));
    b.registerAction(&extra, "extra", "Edit", QKeySequence());
    b.saveIfIncomplete();

    // Defaults: the key in slot 1, split also has the rear side button
    CHECK_EQ(b.inputs("split"), QStringList({"Ctrl+B", "mouse:back"}));
    CHECK_EQ(b.inputs("extra"), QStringList({"", ""}));
    CHECK_EQ(b.mouseAction(Qt::BackButton), QString("split"));

    // Both slots take keys or mouse buttons, in any order
    b.setInput("play", 1, "K");
    CHECK_EQ(play.shortcuts(), QList<QKeySequence>({QKeySequence("Space"), QKeySequence("K")}));
    b.setInput("extra", 0, "mouse:task");
    b.setInput("extra", 1, "Ctrl+E");
    CHECK_EQ(b.inputs("extra"), QStringList({"mouse:task", "Ctrl+E"}));
    CHECK_EQ(extra.shortcut(), QKeySequence("Ctrl+E"));
    CHECK_EQ(b.current("extra"), QKeySequence("Ctrl+E"));
    CHECK_EQ(b.mouseAction(Qt::TaskButton), QString("extra"));
    b.setInput("extra", 0, "Ctrl+E"); // the same input in the other slot moves over
    CHECK_EQ(b.inputs("extra"), QStringList({"Ctrl+E", ""}));
    CHECK_EQ(b.mouseAction(Qt::TaskButton), QString());
    // One action per mouse button: assigning it elsewhere takes it away
    b.setInput("play", 1, "mouse:back");
    CHECK_EQ(b.mouseAction(Qt::BackButton), QString("play"));
    CHECK_EQ(b.inputs("split"), QStringList({"Ctrl+B", ""}));
    CHECK_EQ(play.shortcuts(), QList<QKeySequence>({QKeySequence("Space")}));
    b.resetAll();
    CHECK_EQ(b.mouseAction(Qt::BackButton), QString("split"));
    CHECK_EQ(b.inputs("play"), QStringList({"Space", ""}));

    b.setInput("split", 0, "S");
    b.setInput("extra", 0, "Ctrl+E");
    b.setInput("play", 1, "mouse:forward");
    const QString set = tmp.filePath("mine.json");
    CHECK(b.exportSet(set));

    // Back to the defaults, then load the set: everything as saved, the actions follow immediately
    b.resetAll();
    CHECK_EQ(split.shortcut(), QKeySequence("Ctrl+B"));
    CHECK_EQ(b.mouseAction(Qt::ForwardButton), QString());
    QString error;
    CHECK(b.importSet(set, &error));
    CHECK_EQ(split.shortcut(), QKeySequence("S"));
    CHECK_EQ(extra.shortcut(), QKeySequence("Ctrl+E"));
    CHECK_EQ(play.shortcut(), QKeySequence("Space"));
    CHECK_EQ(b.mouseAction(Qt::ForwardButton), QString("play"));
    CHECK_EQ(b.mouseAction(Qt::BackButton), QString("split")); // the default side button was saved with the set
    // The loaded set is the new keybindings.json: one slot as a plain string, two as a list
    {
        QFile f(b.filePath());
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject keys = QJsonDocument::fromJson(f.readAll()).object()["shortcuts"].toObject();
        CHECK_EQ(keys["extra"].toString(), QString("Ctrl+E"));
        CHECK_EQ(keys["split"].toArray().toVariantList(), QVariantList({"S", "mouse:back"}));
    }

    // Old file (version 2): mouse buttons in their own section, they end up in a free slot
    {
        QFile f(tmp.filePath("old.json"));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(R"({"version": 2, "mouse": {"back": "", "forward": "extra"},
                    "shortcuts": {"split": "X", "play": "Space", "extra": ""}})");
    }
    CHECK(b.importSet(tmp.filePath("old.json"), &error));
    CHECK_EQ(b.inputs("split"), QStringList({"X", ""}));
    CHECK_EQ(b.inputs("extra"), QStringList({"mouse:forward", ""}));
    CHECK_EQ(b.mouseAction(Qt::ForwardButton), QString("extra"));
    CHECK_EQ(b.mouseAction(Qt::BackButton), QString());

    // A set that leaves an action out: that one gets its default (a whole set, not a merge)
    {
        QFile f(tmp.filePath("partial.json"));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(R"({"version": 2, "shortcuts": {"play": "K"}})");
    }
    CHECK(b.importSet(tmp.filePath("partial.json"), &error));
    CHECK_EQ(play.shortcut(), QKeySequence("K"));
    CHECK_EQ(split.shortcut(), QKeySequence("Ctrl+B"));
    CHECK_EQ(extra.shortcut(), QKeySequence());
    CHECK_EQ(b.mouseAction(Qt::BackButton), QString("split")); // old file without mouse section: the default

    // Not a key set: nothing changes
    {
        QFile f(tmp.filePath("other.json"));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(R"({"effects": []})");
    }
    CHECK(!b.importSet(tmp.filePath("other.json"), &error));
    CHECK(!error.isEmpty());
    CHECK(!b.importSet(tmp.filePath("missing.json"), &error));
    CHECK_EQ(play.shortcut(), QKeySequence("K"));

    // Built-in sets: beginner changes keys, wheel and keeps the side button; standard brings back every default
    QAction trim;
    b.registerAction(&trim, "tool_trim", "Edit", QKeySequence("T"));
    b.setInput("play", 1, "mouse:forward");
    CHECK_EQ(InputBindings::presetIds(), QStringList({"standard", "beginner"}));
    b.applyPreset("beginner");
    CHECK_EQ(split.shortcut(), QKeySequence("S"));
    CHECK_EQ(trim.shortcut(), QKeySequence());
    CHECK_EQ(play.shortcut(), QKeySequence("Space"));
    CHECK_EQ(b.mouseAction(Qt::ForwardButton), QString());
    CHECK_EQ(b.inputs("split"), QStringList({"S", "mouse:back"}));
    CHECK(b.wheelAction(Qt::ControlModifier) == WheelAction::Zoom);
    CHECK(b.wheelAction(Qt::ShiftModifier) == WheelAction::ScrollHorizontal);
    {
        QFile f(b.filePath());
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        CHECK_EQ(root["shortcuts"].toObject()["tool_trim"].toString(), QString());
        CHECK_EQ(root["wheel"].toObject()["ctrl"].toString(), QString("zoom"));
    }
    // The beginner set survives a restart (load reads the file again)
    b.load();
    CHECK(b.wheelAction(Qt::ControlModifier) == WheelAction::Zoom);
    CHECK_EQ(b.current("tool_trim"), QKeySequence());
    b.applyPreset("standard");
    CHECK_EQ(split.shortcut(), QKeySequence("Ctrl+B"));
    CHECK_EQ(trim.shortcut(), QKeySequence("T"));
    CHECK(b.wheelAction(Qt::ControlModifier) == WheelAction::ScrollHorizontal);
    CHECK(b.wheelAction(Qt::AltModifier) == WheelAction::Zoom);

    // Dialog: double-click Key 1/Key 2, then press a key combination or a side button
    {
        KeyBindingsDialog dlg;
        dlg.show();
        auto* table = dlg.findChild<QTableWidget*>();
        if (!CHECK(table && table->columnCount() == 4)) return Check::result();
        int row = -1;
        for (int r = 0; r < table->rowCount(); ++r)
            if (table->item(r, 1)->data(Qt::UserRole).toString() == "extra") row = r;
        if (!CHECK(row >= 0)) return Check::result();
        auto capture = [&](int col) {
            emit table->cellDoubleClicked(row, col);
            return qobject_cast<QLabel*>(table->cellWidget(row, col));
        };
        if (QLabel* c = capture(3); CHECK(c)) {
            QKeyEvent ctrl(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier); // modifiers alone wait
            QApplication::sendEvent(c, &ctrl);
            CHECK_EQ(b.inputs("extra"), QStringList({"", ""}));
            QKeyEvent key(QEvent::KeyPress, Qt::Key_J, Qt::ControlModifier | Qt::ShiftModifier);
            QApplication::sendEvent(c, &key);
        }
        QApplication::processEvents();
        CHECK_EQ(b.inputs("extra"), QStringList({"", "Ctrl+Shift+J"}));
        CHECK_EQ(table->item(row, 3)->text(), QKeySequence("Ctrl+Shift+J").toString(QKeySequence::NativeText));
        if (QLabel* c = capture(2); CHECK(c)) {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::ForwardButton,
                              Qt::ForwardButton, Qt::NoModifier);
            QApplication::sendEvent(c, &press);
        }
        QApplication::processEvents();
        CHECK_EQ(b.inputs("extra"), QStringList({"mouse:forward", "Ctrl+Shift+J"}));
        CHECK_EQ(table->item(row, 2)->text(), InputBindings::mouseButtonText(Qt::ForwardButton));
        if (QLabel* c = capture(2); CHECK(c)) { // Esc cancels
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(c, &esc);
        }
        QApplication::processEvents();
        CHECK_EQ(b.inputs("extra"), QStringList({"mouse:forward", "Ctrl+Shift+J"}));
        CHECK(!table->cellWidget(row, 2));
        if (const QString shot = qEnvironmentVariable("KEYBINDINGS_SHOT"); !shot.isEmpty()) {
            table->scrollToItem(table->item(row, 1));
            table->selectRow(row);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); // the removed capture widgets
            dlg.grab().save(shot);
        }
    }

    // A panel's key (render queue: Del deletes jobs): two default slots, its own scope. The panel checks its keys
    // itself (matches), Qt never fires them; the same key as a window action is no conflict, within a scope it is.
    {
        QAction windowDel, panelDel, panelOther;
        b.registerAction(&windowDel, "delete_alt", "Edit", QKeySequence(Qt::Key_Delete));
        b.registerPanelAction(&panelDel, "render_queue_delete", "Render", "render_queue", QKeySequence(Qt::Key_Delete),
                              QKeySequence(Qt::Key_Backspace));
        b.registerPanelAction(&panelOther, "render_queue_other", "Render", "render_queue", QKeySequence());
        CHECK_EQ(b.inputs("render_queue_delete"), QStringList({"Del", "Backspace"}));
        CHECK_EQ(b.defaultInputs("render_queue_delete"), QStringList({"Del", "Backspace"}));
        CHECK_EQ(panelDel.shortcuts(), QList<QKeySequence>({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}));
        CHECK(panelDel.shortcutContext() == Qt::WidgetShortcut);
        CHECK_EQ(b.scope("render_queue_delete"), QString("render_queue"));
        CHECK(b.scope("delete_alt").isEmpty());
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QKeyEvent keypadDel(QEvent::KeyPress, Qt::Key_Delete, Qt::KeypadModifier);
        QKeyEvent ctrlDel(QEvent::KeyPress, Qt::Key_Delete, Qt::ControlModifier);
        QKeyEvent ctrlD(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier);
        CHECK(InputBindings::matches(&panelDel, &del));
        CHECK(InputBindings::matches(&panelDel, &keypadDel));
        CHECK(!InputBindings::matches(&panelDel, &ctrlDel));
        b.setInput("render_queue_delete", 0, "Ctrl+D");
        CHECK(!InputBindings::matches(&panelDel, &del));
        CHECK(InputBindings::matches(&panelDel, &ctrlD));
        panelDel.setEnabled(false);
        CHECK(!InputBindings::matches(&panelDel, &ctrlD));
        panelDel.setEnabled(true);
        b.setInputs("render_queue_delete", b.defaultInputs("render_queue_delete"));

        KeyBindingsDialog dlg;
        auto* table = dlg.findChild<QTableWidget*>();
        if (!CHECK(table)) return Check::result();
        const auto conflict = [&](const QString& id) {
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 1)->data(Qt::UserRole).toString() == id) return !table->item(r, 2)->toolTip().isEmpty();
            return false;
        };
        CHECK(!conflict("delete_alt"));
        CHECK(!conflict("render_queue_delete"));
        b.setInput("render_queue_other", 0, "Del"); // the same key twice in the panel: marked
        dlg.close();
        KeyBindingsDialog again;
        table = again.findChild<QTableWidget*>();
        CHECK(conflict("render_queue_other"));
        CHECK(!conflict("delete_alt"));
        b.setInputs("render_queue_other", {});
    }
    return Check::result();
}
