// SPDX-License-Identifier: AGPL-3.0-only
// Compositing in the app (0.19, the rest of the UI): recipes in the Effects Library (shown in German, a double-click
// and a drop onto a clip apply one as one undo step), "Als Rezept speichern" on the Comp page and the ZIP round trip of
// the library's context menu functions.
// COMP_APP_DUMP=<folder>: pictures of the polished Comp page and the title 3D section.

#include "check.h"

#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/CompRecipes.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/picture/CustomEffects.h"
#include "ui/EffectsLibrary.h"
#include "ui/comp/CompCanvas.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompPage.h"
#include "ui/comp/CompShared.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>
#include <clocale>

namespace {

void dump(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("COMP_APP_DUMP");
    if (dir.isEmpty()) return;
    QTest::qWait(600); // the viewer's picture
    w->grab().save(dir + "/" + name + ".png");
}

void settle()
{
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

CompGraph graphOf(Project* p, int clipId)
{
    CompGraph g;
    if (const Clip* c = TimelineOps::findClip(p->timeline(), clipId)) CompUi::graphOf(*c, &g);
    return g;
}

// The Effects Library list entry with this data (nullptr = none)
QListWidgetItem* entry(EffectsLibrary* lib, const QString& data)
{
    for (int i = 0; i < lib->list()->count(); ++i)
        if (lib->list()->item(i)->data(Qt::UserRole).toString() == data) return lib->list()->item(i);
    return nullptr;
}

void showCategory(EffectsLibrary* lib, const QString& name)
{
    auto* tree = lib->findChild<QTreeWidget*>();
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == name) tree->setCurrentItem(*it);
    settle();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("comp-app");
    I18n::setLanguage("de");
    I18n::install("de");
    Theme::apply(app);
    if (!Check::haveFfmpeg()) {
        std::puts("SKIP: no ffmpeg for the test clip");
        return 77;
    }
    QDir(CompRecipes::dir()).removeRecursively(); // own recipes of an earlier run
    QTemporaryDir dir;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    CHECK(QTest::qWaitForWindowExposed(&w));
    const QString file = Check::makeMedia(dir.path() + "/clip.mp4",
        {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=2", "-c:v", "libx264", "-preset", "ultrafast",
         "-pix_fmt", "yuv420p"});
    if (!CHECK(!file.isEmpty())) return Check::result();
    w.importFiles({file, file}, true);
    settle();
    Project* project = w.liveProject();
    QUndoStack* undo = project->undoStack();
    if (!CHECK(!project->timeline().video.isEmpty() && project->timeline().video[0].clips.size() >= 2))
        return Check::result();
    const int clipId = project->timeline().video[0].clips[0].id;
    const int clip2 = project->timeline().video[0].clips[1].id;

    // ---- Recipes in the Effects Library: German names, double-click applies (one undo step) ----
    {
        CHECK_EQ(CompRecipes::shownName(*CompRecipes::find("greenscreen"), "de"), QString("Green Screen"));
        CHECK_EQ(CompRecipes::shownName(*CompRecipes::find("pip_frame"), "fr"), QString("Incrustation d'image"));
        CHECK_EQ(CompRecipes::find("pip_frame")->name, QString("Picture in picture")); // English for the AI
        auto* lib = w.findChild<EffectsLibrary*>();
        if (!CHECK(lib)) return Check::result();
        lib->showRecipes();
        settle();
        QListWidgetItem* pip = entry(lib, "recipe:pip_frame");
        if (!CHECK(pip)) return Check::result();
        CHECK_EQ(pip->text(), QString("Bild im Bild"));
        CHECK(pip->toolTip().contains("Ecke"));
        CHECK(entry(lib, "recipe:greenscreen") && entry(lib, "recipe:greenscreen")->text() == "Green Screen");
        // The drag data is the effect form "recipe:<id>"
        std::unique_ptr<QMimeData> mime(lib->list()->model()->mimeData({lib->list()->indexFromItem(pip)}));
        CHECK_EQ(QString::fromUtf8(mime->data(EffectsLibrary::EffectMimeType)), QString("recipe:pip_frame"));

        w.liveSelection()->set({clipId});
        const int idx = undo->index();
        emit lib->list()->itemDoubleClicked(pip);
        settle();
        CHECK_EQ(undo->index(), idx + 1);
        CompGraph g = graphOf(project, clipId);
        CHECK_EQ(int(g.nodes.size()), int(CompRecipes::find("pip_frame")->graph.nodes.size()));
        CHECK(graphOf(project, clip2).isEmpty()); // only the selected clip
        undo->undo();
        CHECK(graphOf(project, clipId).isEmpty());

        // Dropped onto the second clip in the timeline: onto that clip only, one undo step
        TimelineView* tv = w.findChild<TimelineView*>();
        if (!CHECK(tv)) return Check::result();
        w.liveSelection()->set({clipId}); // the drop goes onto the clip under the mouse, not the selection
        const int idx2 = undo->index();
        emit tv->recipeDropped(clip2, "greenscreen");
        settle();
        QTest::qWait(10);
        settle();
        CHECK_EQ(undo->index(), idx2 + 1);
        CHECK(!graphOf(project, clip2).isEmpty());
        CHECK(graphOf(project, clipId).isEmpty());
        CHECK_EQ(undo->undoText(), QString("Rezept „Green Screen“ anwenden"));
        // The recipe's slot values are normal node values afterwards (the key colour of the green screen)
        const Clip* c2 = TimelineOps::findClip(project->timeline(), clip2);
        CHECK(CompUi::nodeValues(*c2, 2).value("color").toString().compare("#ff00ff00", Qt::CaseInsensitive) == 0);

        // A required slot missing (the sticker): the core refuses it, nothing changes
        const int idx3 = undo->index();
        QString why;
        CHECK(!CompUi::applyRecipe(w.editor(), {clipId}, "nothing", {}, &why));
        CHECK(!why.isEmpty());
        why.clear();
        CHECK(!CompUi::applyRecipe(w.editor(), {clipId}, "sticker_track", {}, &why));
        CHECK(why.contains("sticker"));
        CHECK_EQ(undo->index(), idx3);
        CHECK(!undo->canRedo()); // no empty step left behind
    }

    // ---- "Als Rezept speichern" on the Comp page, listed as own recipe, ZIP export + import round trip ----
    {
        w.openComp(clip2);
        settle();
        CompPage* page = w.compPage();
        if (!CHECK(page)) return Check::result();
        QString why, code;
        const QString id = page->saveRecipe("Mein Key", false, &why, &code);
        CHECK(!id.isEmpty());
        CHECK(page->saveRecipe("Mein Key", false, &why, &code).isEmpty());
        CHECK_EQ(code, QString("EXISTS"));
        CHECK_EQ(page->saveRecipe("Mein Key", true, &why, &code), id);
        auto* lib = w.findChild<EffectsLibrary*>();
        lib->showRecipes();
        settle();
        QListWidgetItem* own = entry(lib, "recipe:" + id);
        CHECK(own && own->text() == "Mein Key");

        const QString zip = dir.path() + "/key.zip";
        CHECK(lib->exportRecipe(id, zip, &why));
        CHECK(QFileInfo(zip).size() > 0);
        CHECK(CompRecipes::remove(id));
        lib->refreshRecipes();
        CHECK(entry(lib, "recipe:" + id) == nullptr);
        const QString back = lib->importRecipe(zip, &why);
        CHECK_EQ(back, id);
        CHECK(entry(lib, "recipe:" + id) != nullptr);
        CHECK(lib->importRecipe(dir.path() + "/missing.zip", &why).isEmpty());
        CHECK(!why.isEmpty());
        // The imported recipe applies like a built-in one (one undo step), the same graph as the saved clip's
        const int idx = undo->index();
        CHECK(CompUi::applyRecipe(w.editor(), {clipId}, id, {}, &why));
        CHECK_EQ(undo->index(), idx + 1);
        CHECK_EQ(int(graphOf(project, clipId).nodes.size()), int(graphOf(project, clip2).nodes.size()));
    }
    // ---- Comp page polish: node boxes say what the node does, the graph has room, "Einpassen" shows every node ----
    {
        CompGraph g;
        g.nodes << CompNode{1, "clip", {}} << CompNode{2, "color", {}} << CompNode{3, "mask", {}}
                << CompNode{4, "merge", {}} << CompNode{5, "fx", {}} << CompNode{6, "text", {}} << CompNode{7, "merge", {}};
        g.links << CompLink{1, 4, "bg"} << CompLink{2, 4, "fg"} << CompLink{3, 4, "mask"} << CompLink{4, 5, "src"}
                << CompLink{5, 7, "bg"} << CompLink{6, 7, "fg"};
        g.out = 7;
        QHash<int, QVariantMap> values;
        values[3] = {{"shape", "ellipse"}};
        values[4] = {{"mode", "screen"}, {"opacity", 0.5}};
        values[5] = {{"effect", "frei0r.glow"}};
        values[6] = {{"text", "Hallo\nWelt"}};
        QString why;
        CHECK(w.editor()->setCompGraph({clipId}, g, values, CompUi::kinds(), &why));
        const Clip* c = TimelineOps::findClip(project->timeline(), clipId);
        CHECK_EQ(CompUi::nodeSummary(*c, *g.node(3)), QString("Ellipse"));
        CHECK_EQ(CompUi::nodeSummary(*c, *g.node(4)), QString("Negativ multiplizieren · 50 %"));
        CHECK_EQ(CompUi::nodeSummary(*c, *g.node(5)), EffectRegistry::find("frei0r.glow")->name);
        CHECK_EQ(CompUi::nodeSummary(*c, *g.node(6)), QString("„Hallo Welt“"));
        CHECK_EQ(CompUi::nodeSummary(*c, *g.node(7)), QString("Normal"));
        CHECK(!CompUi::nodeSummary(*c, *g.node(2)).isEmpty()); // the colour

        w.openComp(clipId);
        CompPage* page = w.compPage();
        page->setView(CompPage::View::Nodes);
        settle();
        QTest::qWait(20);
        settle();
        CompCanvas* canvas = page->canvas();
        // The graph area is the biggest part of the page (the timeline a slim strip)
        CHECK(canvas->height() >= 380);
        canvas->fit();
        settle();
        for (const CompNode& n : g.nodes) CHECK(canvas->rect().contains(canvas->nodeRect(n.id).toRect()));
        page->selectNode(4);
        settle();
        dump(&w, "comp-page-polished");

        // ---- fx node: the wrapped effect's own values (e_<key>) in the inspector ----
        page->selectNode(5);
        settle();
        CompInspector* ins = page->inspector();
        const EffectDescriptor* glow = EffectRegistry::find("frei0r.glow");
        for (const EffectParam& q : glow->params) CHECK(ins->findChild<QWidget*>("comp_param_e_" + q.key) != nullptr);
        CHECK(ins->findChild<QWidget*>("comp_param_effect") != nullptr);
    }

    // ---- fx node with a filter chain: a multi-line field, "Anwenden" checks like the CLI and shows the error ----
    {
        CompGraph g;
        g.nodes << CompNode{1, "clip", {}} << CompNode{2, "fx", {}};
        g.links << CompLink{1, 2, "src"};
        g.out = 2;
        QHash<int, QVariantMap> values;
        values[2] = {{"effect", "ffmpeg_filter"}};
        QString why;
        CHECK(w.editor()->setCompGraph({clip2}, g, values, CompUi::kinds(), &why));
        w.openComp(clip2);
        CompPage* page = w.compPage();
        page->selectNode(2);
        settle();
        CompInspector* ins = page->inspector();
        auto* edit = ins->findChild<QPlainTextEdit*>("comp_param_e_chain");
        auto* applyButton = ins->findChild<QPushButton*>("comp_apply_e_chain");
        auto* error = ins->findChild<QLabel*>("comp_error_e_chain");
        if (!CHECK(edit && applyButton && error)) return Check::result();
        const QString bad = "movie=/etc/passwd";
        const QString expected = Picture::Custom::checkFilter(bad);
        CHECK(!expected.isEmpty());
        int idx = undo->index();
        edit->setPlainText(bad);
        applyButton->click();
        settle();
        CHECK_EQ(undo->index(), idx); // refused: nothing set
        CHECK(error->isVisibleTo(ins));
        CHECK_EQ(error->text(), expected);
        if (Picture::Custom::filterAvailable() && Picture::Custom::checkFilter("hue=s=0").isEmpty()) {
            edit->setPlainText("hue=s=0");
            applyButton->click();
            settle();
            CHECK_EQ(undo->index(), idx + 1);
            CHECK(!error->isVisibleTo(ins));
            const Clip* c = TimelineOps::findClip(project->timeline(), clip2);
            CHECK_EQ(CompUi::nodeValues(*c, 2).value("e_chain").toString(), QString("hue=s=0"));
        } else {
            std::puts("note: no FFmpeg filters here, only the refusal checked");
        }
    }
    return Check::result();
}
