// SPDX-License-Identifier: AGPL-3.0-only
// Restore after a crash (MainWindow::offerAutosaveRestore): a broken automatic backup says why it cannot be restored
// and is moved aside, so the next start does not offer it again; a good one is restored.
#include "check.h"

#include "app/MainWindow.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "engine/Engine.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <clocale>

namespace {

// Answers every message box that comes up: the restore question with `answer`, the others with OK; what they said
struct Answerer {
    QStringList texts;
    QTimer timer;
    explicit Answerer(QMessageBox::StandardButton answer)
    {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [this, answer] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) return;
            texts << box->text();
            if (QAbstractButton* b = box->button(answer)) b->click();
            else box->accept();
        });
        timer.start();
    }
};

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QTemporaryDir tmp;
    qputenv("HOME", tmp.path().toUtf8()); // test mode puts the app data under ~/.qttest: never the user's
    QApplication app(argc, argv);
    Check::initApp("autosave-restore");
    I18n::install("de");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString backup = dir + "/autosave." + ProjectFile::Extension; // the first slot
    const auto broken = [&] {
        return QDir(dir).entryList({QString("autosave-broken-*.%1").arg(ProjectFile::Extension)}, QDir::Files);
    };

    // Broken (cut off while writing): "Yes" says why it failed and moves it aside
    {
        QFile f(backup);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("{\"app\":\"schneidi\",\"timeline\":{\"video\":[{\"cl");
        f.close();
        Answerer answer(QMessageBox::Yes);
        CHECK(!w.offerAutosaveRestore());
        if (CHECK_EQ(answer.texts.size(), qsizetype(2))) {
            CHECK(answer.texts[1].startsWith(T("Die automatische Sicherung konnte nicht geladen werden:\n%1").arg("").trimmed()));
            CHECK(answer.texts[1].contains("autosave-broken-"));
        }
        CHECK(!QFileInfo::exists(backup));
        CHECK_EQ(broken().size(), qsizetype(1));
    }
    // ... and is not offered again at the next start
    {
        Answerer answer(QMessageBox::Yes);
        CHECK(!w.offerAutosaveRestore());
        QApplication::processEvents();
        CHECK(answer.texts.isEmpty());
    }
    // A good backup is restored (unsaved: modified)
    {
        ProjectData data;
        data.timeline.markers << 42;
        CHECK(ProjectFile::save(data, backup, &err));
        Answerer answer(QMessageBox::Yes);
        CHECK(w.offerAutosaveRestore());
        CHECK_EQ(answer.texts.size(), qsizetype(1));
        CHECK(w.liveProject()->isModified());
        CHECK(w.liveProject()->timeline().markers.contains(42));
    }
    return Check::result();
}
