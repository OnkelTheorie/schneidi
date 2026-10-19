// Test workspace presets: whole key sets saved to a file and loaded again (InputBindings::exportSet/importSet),
// built-in sets (InputBindings::applyPreset).
#include "check.h"

#include "app/InputBindings.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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

    b.setShortcut("split", QKeySequence("S"));
    b.setShortcut("extra", QKeySequence("Ctrl+E"));
    b.setMouseButton("play", Qt::ForwardButton);
    const QString set = tmp.filePath("mine.json");
    CHECK(b.exportSet(set));

    // Back to the defaults, then load the set: everything as saved, the actions follow immediately
    b.resetAll();
    CHECK_EQ(split.shortcut(), QKeySequence("Ctrl+B"));
    CHECK_EQ(b.mouseButtonFor("play"), Qt::NoButton);
    QString error;
    CHECK(b.importSet(set, &error));
    CHECK_EQ(split.shortcut(), QKeySequence("S"));
    CHECK_EQ(extra.shortcut(), QKeySequence("Ctrl+E"));
    CHECK_EQ(play.shortcut(), QKeySequence("Space"));
    CHECK_EQ(b.mouseButtonFor("play"), Qt::ForwardButton);
    CHECK_EQ(b.mouseAction(Qt::BackButton), QString("split")); // the default side button was saved with the set
    // The loaded set is the new keybindings.json
    {
        QFile f(b.filePath());
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK_EQ(QJsonDocument::fromJson(f.readAll()).object()["shortcuts"].toObject()["split"].toString(), QString("S"));
    }

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
    b.setMouseButton("play", Qt::ForwardButton);
    CHECK_EQ(InputBindings::presetIds(), QStringList({"standard", "beginner"}));
    b.applyPreset("beginner");
    CHECK_EQ(split.shortcut(), QKeySequence("S"));
    CHECK_EQ(trim.shortcut(), QKeySequence());
    CHECK_EQ(play.shortcut(), QKeySequence("Space"));
    CHECK_EQ(b.mouseButtonFor("play"), Qt::NoButton);
    CHECK_EQ(b.mouseAction(Qt::BackButton), QString("split"));
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
    return Check::result();
}
