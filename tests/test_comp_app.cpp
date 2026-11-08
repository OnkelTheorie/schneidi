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
#include "engine/PersonMatte.h"
#include "engine/picture/CompCutout.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CutoutCleanup.h"
#include "engine/picture/CustomEffects.h"
#include "ui/EffectsLibrary.h"
#include "ui/Inspector.h"
#include "ui/ScrubField.h"
#include "ui/comp/CompCanvas.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompOverlay.h"
#include "ui/comp/CompPage.h"
#include "ui/comp/CompShared.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QDialog>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTimer>
#include <QScrollBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>
#include <clocale>

namespace {

// Stand-in model: "person" = pixels redder than green, as two-channel logits (like test_comp_cutout)
PersonMatte::Segmenter standIn()
{
    PersonMatte::Segmenter s;
    s.spec = PersonMatte::Spec{};
    s.way = "stand-in";
    s.run = [spec = s.spec](const Inference::Tensor& in, QString*) -> std::optional<Inference::Tensor> {
        const size_t n = size_t(spec.width) * spec.height;
        Inference::Tensor out;
        out.shape = {1, 2, spec.height, spec.width};
        out.data.assign(2 * n, 0.0f);
        for (size_t i = 0; i < n; ++i) out.data[n + i] = (in.data[i] - in.data[n + i]) * 8.0f;
        return out;
    };
    return s;
}

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
        // The graph area is a big part of the page (no timeline on the Comp page)
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

    // ---- The Comp page follows the playhead: the clip under it; opening a clip moves the playhead into it ----
    {
        const Clip* a = TimelineOps::findClip(project->timeline(), clipId);
        const Clip* b = TimelineOps::findClip(project->timeline(), clip2);
        if (CHECK(a && b && b->start >= a->end())) {
            const int startB = b->start, startA = a->start;
            w.openComp(clipId);
            settle();
            CHECK_EQ(w.compPage()->clip(), clipId);
            engine.seek(startB + 1);
            QTest::qWait(50);
            settle();
            CHECK_EQ(w.compPage()->clip(), clip2);
            CHECK_EQ(w.compClipAt(startA), clipId);
            w.openComp(clipId); // the playhead is in clip 2: it goes to clip 1's start
            QTest::qWait(50);
            settle();
            CHECK_EQ(w.compPage()->clip(), clipId);
            CHECK(engine.position() >= startA && engine.position() < startB);
            // Selecting another clip on the page: the playhead goes into it, the page shows it
            w.liveSelection()->set({clip2});
            QTest::qWait(50);
            settle();
            CHECK(engine.position() >= startB);
            CHECK_EQ(w.compPage()->clip(), clip2);
        }
    }

    // ---- key node: only the values of the chosen kind (chroma or luma), no two "soft edge" rows ----
    {
        CompGraph g;
        g.nodes << CompNode{1, "clip", {}} << CompNode{2, "key", {}};
        g.links << CompLink{1, 2, "src"};
        g.out = 2;
        QString why;
        CHECK(w.editor()->setCompGraph({clip2}, g, {}, CompUi::kinds(), &why));
        w.openComp(clip2);
        CompPage* page = w.compPage();
        page->selectNode(2);
        settle();
        CompInspector* ins = page->inspector();
        CHECK(ins->findChild<QWidget*>("comp_param_softness") && ins->findChild<QWidget*>("comp_param_tolerance"));
        CHECK(!ins->findChild<QWidget*>("comp_param_soft") && !ins->findChild<QWidget*>("comp_param_low"));
        CHECK(w.editor()->setCompNodeValues(clip2, 2, {{"mode", "luma"}}, &why));
        settle();
        CHECK(ins->findChild<QWidget*>("comp_param_soft") && ins->findChild<QWidget*>("comp_param_low"));
        CHECK(!ins->findChild<QWidget*>("comp_param_softness") && !ins->findChild<QWidget*>("comp_param_tolerance"));
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
    // ---- Analysis buttons: cutout (with a stand-in model) and tracker, in the background, one undo step ----
    {
        CompGraph g;
        g.nodes << CompNode{1, "clip", {}} << CompNode{2, "cutout", {}} << CompNode{3, "tracker", {}};
        g.links << CompLink{1, 2, "src"} << CompLink{1, 3, "src"};
        g.out = 2;
        QString why;
        CHECK(w.editor()->setCompGraph({clip2}, g, {}, CompUi::kinds(), &why));
        w.openComp(clip2);
        CompPage* page = w.compPage();
        CompInspector* ins = page->inspector();

        // Without the model: a clear hint and a button that opens the extensions dialog
        PersonMatte::setStandIn(std::nullopt);
        page->selectNode(2);
        settle();
        if (!PersonMatte::available()) {
            auto* analyze = ins->findChild<QPushButton*>("comp_analyze");
            auto* open = ins->findChild<QPushButton*>("comp_open_extensions");
            if (!CHECK(analyze && open)) return Check::result();
            CHECK(!analyze->isEnabled());
            CHECK(open->isVisibleTo(ins));
            CHECK(ins->findChild<QLabel*>("comp_analyze_status")->text().contains("Erweiterung"));
            QSignalSpy spy(ins, &CompInspector::extensionsRequested);
            QTimer::singleShot(200, [] { // the dialog is modal: close it
                if (auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget())) d->reject();
            });
            open->click();
            CHECK_EQ(int(spy.count()), 1);
        } else {
            std::puts("note: a cutout model is installed here, the hint without it is not checked");
        }

        // With the stand-in: analysis in the background, then the node's cache value (one undo step)
        PersonMatte::setStandIn(standIn());
        page->refresh();
        settle();
        auto* analyze = ins->findChild<QPushButton*>("comp_analyze");
        if (!CHECK(analyze && analyze->isEnabled())) return Check::result();
        int idx = undo->index();
        analyze->click();
        CHECK(ins->analyzing());
        CHECK(QTest::qWaitFor([&] { return !ins->analyzing(); }, 60000));
        settle();
        CHECK_EQ(undo->index(), idx + 1);
        const Clip* c = TimelineOps::findClip(project->timeline(), clip2);
        const QString cache = CompUi::nodeValues(*c, 2).value("cache").toString();
        CHECK(!cache.isEmpty());
        CHECK(QFileInfo::exists(Picture::Comp::cutoutCachePath(cache)));
        CHECK_EQ(ins->findChild<QLabel*>("comp_analyze_status")->text(), QString("Fertig"));
        PersonMatte::setStandIn(std::nullopt);

        // Cache cleanup: the analysis stays while a node names it, goes once none does
        CHECK(Picture::Comp::cutoutCachesOf(project->data()).contains(cache));
        bool listed = false;
        for (const auto& f : Picture::Comp::unusedCutoutCaches(Picture::Comp::cutoutCachesOf(project->data())))
            listed |= f.id == cache;
        CHECK(!listed);
        undo->undo(); // the node without its analysis
        listed = false;
        for (const auto& f : Picture::Comp::unusedCutoutCaches(Picture::Comp::cutoutCachesOf(project->data())))
            listed |= f.id == cache;
        CHECK(listed);
        undo->redo();

        // Tracker: "Tracken" from the playhead to the clip's end, keyframes of x/y
        page->selectNode(3);
        settle();
        analyze = ins->findChild<QPushButton*>("comp_analyze");
        if (!CHECK(analyze && analyze->isEnabled() && analyze->text() == "Tracken")) return Check::result();
        idx = undo->index();
        analyze->click();
        CHECK(QTest::qWaitFor([&] { return !ins->analyzing(); }, 60000));
        settle();
        CHECK_EQ(undo->index(), idx + 1);
        c = TimelineOps::findClip(project->timeline(), clip2);
        CHECK(CompUi::animated(*c, 3, "x"));
        CHECK(ins->findChild<QLabel*>("comp_analyze_status")->text().startsWith("Fertig"));

        // The clip changed while it was analysed: the result is dropped, the status says so
        PersonMatte::setStandIn(standIn());
        page->selectNode(2);
        page->refresh();
        settle();
        analyze = ins->findChild<QPushButton*>("comp_analyze");
        if (CHECK(analyze && analyze->isEnabled())) {
            const QString before = CompUi::nodeValues(*TimelineOps::findClip(project->timeline(), clip2), 2).value("cache").toString();
            idx = undo->index();
            analyze->click();
            w.editor()->modifyClips({clip2}, "trim", [](Clip& cl) { cl.out -= 2; });
            CHECK(QTest::qWaitFor([&] { return !ins->analyzing(); }, 60000));
            settle();
            CHECK_EQ(undo->index(), idx + 1); // the trim only
            CHECK_EQ(ins->findChild<QLabel*>("comp_analyze_status")->text(), QString("Analyse verworfen – Clip geändert"));
            CHECK_EQ(CompUi::nodeValues(*TimelineOps::findClip(project->timeline(), clip2), 2).value("cache").toString(), before);
            undo->undo();

            // Another project replaced it meanwhile: the analysis stops, nothing lands in the new one
            settle();
            analyze = ins->findChild<QPushButton*>("comp_analyze");
            analyze->click();
            CHECK(ins->analyzing());
            project->load(project->data());
            CHECK(!ins->analyzing());
            QThreadPool::globalInstance()->waitForDone(60000);
            settle();
            CHECK_EQ(undo->index(), 0);
            CHECK(!undo->canUndo());
        }
        PersonMatte::setStandIn(std::nullopt);
    }

    // ---- Tracker handles on a 9:16 clip in the 16:9 frame: A/B are 0..1 of the clip's own picture (between the bars) ----
    {
        const QString vert = Check::makeMedia(dir.path() + "/vert.mp4",
            {"-f", "lavfi", "-i", "testsrc2=size=180x320:rate=25:duration=1", "-c:v", "libx264", "-preset", "ultrafast",
             "-pix_fmt", "yuv420p"});
        if (!CHECK(!vert.isEmpty())) return Check::result();
        w.importFiles({vert}, true, false);
        settle();
        int vertId = 0;
        for (const Track& t : project->timeline().video)
            for (const Clip& c : t.clips)
                if (c.mediaPath == vert) vertId = c.id;
        if (!CHECK(vertId)) return Check::result();
        const ProjectFormat f = project->timelineFormat();
        CHECK_EQ(f.width * 9, f.height * 16); // the frame stayed 16:9
        CompGraph g;
        g.nodes << CompNode{1, "clip", {}} << CompNode{2, "tracker", {}};
        g.links << CompLink{1, 2, "src"};
        g.out = 2;
        QHash<int, QVariantMap> values;
        values[2] = {{"mode", "two"}, {"ax", 0.25}, {"ay", 0.5}, {"bx", 0.75}, {"by", 0.1}};
        QString why;
        CHECK(w.editor()->setCompGraph({vertId}, g, values, CompUi::kinds(), &why));
        w.openComp(vertId);
        CompPage* page = w.compPage();
        page->selectNode(2);
        settle();
        CompOverlay* o = page->overlay();
        const QVector<CompOverlay::Handle> hs = o->handles();
        if (!CHECK_EQ(int(hs.size()), 2)) return Check::result();
        // The picture: 9:16 at the frame's height, centred
        const double pw = f.height * 9.0 / 16.0, left = (f.width - pw) / 2;
        CHECK(std::abs(hs[0].at.x() - (left + 0.25 * pw)) < 0.6);
        CHECK(std::abs(hs[0].at.y() - 0.5 * f.height) < 0.6);
        CHECK(std::abs(hs[1].at.x() - (left + 0.75 * pw)) < 0.6);
        CHECK(std::abs(hs[1].at.y() - 0.1 * f.height) < 0.6);
        // Dragging A to the picture's right edge gives ax = 1 (not the frame's 0.6 or so), one undo step
        const QRectF frame(0, 0, f.width, f.height); // widget = frame pixels
        const QPointF from = o->handlePos(0, frame), to(left + pw, f.height * 0.25);
        const int idx = undo->index();
        QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        CHECK(o->mousePress(&press, frame));
        for (int i = 1; i <= 4; ++i) {
            const QPointF at = from + (to - from) * double(i) / 4;
            QMouseEvent move(QEvent::MouseMove, at, at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            o->mouseMove(&move, frame);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        CHECK(o->mouseRelease(&release, frame));
        CHECK_EQ(undo->index(), idx + 1);
        const Clip* c = TimelineOps::findClip(project->timeline(), vertId);
        CHECK(std::abs(CompUi::nodeValues(*c, 2).value("ax").toDouble() - 1.0) < 1e-3);
        CHECK(std::abs(CompUi::nodeValues(*c, 2).value("ay").toDouble() - 0.25) < 1e-3);
    }

    // ---- Title 3D in the Edit inspector: each change one undo step, a value switches 3D on ----
    {
        w.showPage(MainWindow::Page::Edit);
        settle();
        const int before = int(project->timeline().video.size());
        w.editor()->addTitle(0, -1, 50);
        settle();
        int titleId = 0;
        for (const Track& t : project->timeline().video)
            for (const Clip& c : t.clips)
                if (c.kind == ClipKind::Title) titleId = c.id;
        if (!CHECK(titleId)) return Check::result();
        Q_UNUSED(before);
        w.liveSelection()->set({titleId});
        settle();
        auto* ins = w.findChild<Inspector*>();
        const auto field = [&](const char* name) {
            return ins ? dynamic_cast<ScrubField*>(ins->findChild<QWidget*>(name)) : nullptr;
        };
        ScrubField* depth = field("inspector_titleSolidDepth");
        ScrubField* rotY = field("inspector_titleSolidRotY");
        if (!CHECK(depth && rotY)) return Check::result();
        const auto look = [&] { return TimelineOps::findClip(project->timeline(), titleId)->title.look; };
        CHECK(!look().solidOn);
        int idx = undo->index();
        depth->onChange(60); // a drag: several values, one step
        depth->onChange(80);
        depth->onFinish();
        settle();
        CHECK_EQ(undo->index(), idx + 1);
        CHECK(look().solidOn);
        CHECK_EQ(look().solidDepth, 80.0);
        rotY->onChange(-40);
        rotY->onFinish();
        settle();
        CHECK_EQ(undo->index(), idx + 2);
        CHECK_EQ(look().solidRotY, -40.0);
        undo->undo();
        CHECK_EQ(look().solidRotY, TitleLook().solidRotY);
        undo->redo();
        // The same ranges as the text3d node and the edit op: turns and the light direction -360..360
        for (const char* name : {"inspector_titleSolidRotX", "inspector_titleSolidRotY", "inspector_titleSolidRotZ",
                                 "inspector_titleSolidLightAngle"})
            if (ScrubField* f = field(name); CHECK(f)) {
                const double was = f->value();
                f->setValue(-300); // shown only, no change
                CHECK_EQ(f->value(), -300.0);
                f->setValue(was);
            }
        // The section in view for the picture
        for (QWidget* h : ins->findChildren<QWidget*>("InspectorSection"))
            for (QToolButton* b : h->findChildren<QToolButton*>())
                if (b->text() == "3D" && h->isVisible())
                    for (QScrollArea* area : ins->findChildren<QScrollArea*>())
                        if (area->isAncestorOf(h)) {
                            area->verticalScrollBar()->setValue(h->mapTo(area->widget(), QPoint(0, 0)).y());
                            settle();
                        }
        dump(ins, "title-3d-section");
    }
    // A text node's box quotes its text the way the UI language does (last: the language stays English)
    {
        CompGraph g;
        g.nodes << CompNode{1, "text", {}};
        g.out = 1;
        QString why;
        CHECK(w.editor()->setCompGraph({clipId}, g, {{1, {{"text", "Hello"}}}}, CompUi::kinds(), &why));
        const Clip* c = TimelineOps::findClip(project->timeline(), clipId);
        I18n::install("en");
        CHECK_EQ(CompUi::nodeSummary(*c, *g.node(1)), QString("“Hello”"));
        // The 3D title's camera distance is no "Padding" (the box's spacing)
        const Picture::Comp::NodeType* t3 = Picture::Comp::find("text3d");
        CHECK(t3 && t3->param("distance"));
        if (t3 && t3->param("distance")) CHECK_EQ(T(t3->param("distance")->label.toUtf8().constData()), QString("Distance"));
    }
    return Check::result();
}
