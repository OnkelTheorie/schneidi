// SPDX-License-Identifier: AGPL-3.0-only
// Opening a project with custom code nobody trusted here (core/CodeTrust.h, 0.19): the app asks once (ui/TrustDialog):
// "Ausführen" trusts and remembers it for the file, "Nicht ausführen" keeps it skipped; the inspector's "Code
// erlauben" allows it later. A project without custom code (or with remembered code) opens without asking.
#include "check.h"

#include "app/MainWindow.h"
#include "core/CodeTrust.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/TrustDialog.h"

#include <QApplication>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <clocale>

namespace {

// A project whose clip has a filter chain
ProjectData project(const QString& chain)
{
    ProjectData d;
    Track t;
    Clip c;
    c.id = 1;
    c.start = 0;
    c.in = 0;
    c.out = 49;
    c.kind = ClipKind::Title;
    if (!chain.isEmpty()) c.effects << EffectInstance{"ffmpeg_filter", {{"chain", chain}}, true};
    t.clips << c;
    d.timeline.video << t;
    d.lastClipId = 1;
    return d;
}

// Clicks a button of the dialog the next modal loop shows; *asked = it came
void answer(const char* button, bool* asked)
{
    *asked = false;
    QTimer::singleShot(0, [button, asked] {
        auto* dlg = qobject_cast<TrustDialog*>(QApplication::activeModalWidget());
        if (!dlg) {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* t = qobject_cast<TrustDialog*>(w); t && t->isVisible()) dlg = t;
        }
        if (!dlg) return;
        *asked = true;
        dlg->findChild<QPushButton*>(button)->click();
    });
}

void resetAll()
{
    CodeTrust::resetForTests();
    QSettings().remove("trust");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("trust_dialog");
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir dir;
    resetAll();

    // The dialog itself: both buttons, what it shows
    {
        const ProjectData d = project("negate\nhue=s=0");
        const QString path = dir.filePath("a.schneidi");
        bool asked = false;
        answer("trustSkip", &asked);
        CHECK(!TrustDialog::askForProject(nullptr, d, path));
        CHECK(asked);
        CHECK(!CodeTrust::isTrusted("negate\nhue=s=0"));
        CHECK(!CodeTrust::remembered(path, CodeTrust::codes(d)));
        answer("trustRun", &asked);
        CHECK(TrustDialog::askForProject(nullptr, d, path));
        CHECK(asked);
        CHECK(CodeTrust::isTrusted("negate\nhue=s=0"));
        CHECK(CodeTrust::remembered(path, CodeTrust::codes(d)));
        // Trusted now: no question
        answer("trustSkip", &asked);
        CHECK(TrustDialog::askForProject(nullptr, d, path));
        QApplication::processEvents();
        CHECK(!asked);
        // Nothing to ask about
        CHECK(TrustDialog::askForProject(nullptr, project({}), path));
        // The Skip button is the default (Enter never runs foreign code)
        TrustDialog dlg(CodeTrust::untrusted(project("negate,vflip")));
        CHECK(dlg.skipButton()->isDefault() && !dlg.runButton()->isDefault());
    }

    // In the app: opening a shared project asks; "Nicht ausführen" leaves the code untrusted and the inspector
    // offers "Code erlauben"; "Ausführen" remembers it, so the next opening asks no more
    resetAll();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    MainWindow w(&engine);
    w.disableAutosave();
    const QString shared = dir.filePath("shared.schneidi");
    CHECK(ProjectFile::save(project("negate"), shared, nullptr));
    CHECK(!CodeTrust::remembered(shared, {"negate"})); // nobody trusted it: no record written
    {
        bool asked = false;
        answer("trustSkip", &asked);
        CHECK(w.openProject(shared));
        CHECK(asked);
        CHECK(!CodeTrust::isTrusted("negate"));
        // The inspector of the clip: "Code erlauben" shown, allowing trusts the code
        w.liveSelection()->set({1});
        QApplication::processEvents();
        auto* allow = w.findChild<Inspector*>()->findChild<QPushButton*>("allowCode");
        if (CHECK(allow)) {
            CHECK(!allow->isHidden());
            allow->click();
            CHECK(CodeTrust::isTrusted("negate"));
            CHECK(allow->isHidden());
        }
    }
    CodeTrust::resetForTests();
    {
        bool asked = false;
        answer("trustRun", &asked);
        CHECK(w.openProject(shared));
        CHECK(asked);
        CHECK(CodeTrust::isTrusted("negate"));
        CodeTrust::resetForTests(); // the next run: remembered
        answer("trustSkip", &asked);
        CHECK(w.openProject(shared));
        QApplication::processEvents();
        CHECK(!asked);
        CHECK(CodeTrust::isTrusted("negate"));
    }
    resetAll();
    return Check::result();
}
