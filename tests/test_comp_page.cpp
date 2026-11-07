// SPDX-License-Identifier: AGPL-3.0-only
// Comp page (0.19, ui/comp, MainWindowComp.cpp): built on its first showing; "Open in Comp" gives a clip an empty
// graph and shows it; the layer view shows a merge chain; nodes are added (Tab search), linked (wire drag), removed
// (Del) and duplicated (Ctrl+D) on the canvas, each one undo step; a viewer handle drag changes a mask's position as
// one undo step; the keyframe diamond sets a keyframe; the Edit Inspector shows "Comp: N Knoten" and opens the page;
// outside edits (CLI @app) show at once; the page survives undoing the whole graph.
// COMP_PAGE_DUMP=<folder>: pictures of the page (layer view, node canvas, inspector of a mask).

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/ScrubField.h"
#include "ui/comp/CompCanvas.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompLayers.h"
#include "ui/comp/CompOverlay.h"
#include "ui/comp/CompPage.h"
#include "ui/comp/CompShared.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <clocale>

namespace {

void dump(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("COMP_PAGE_DUMP");
    if (!dir.isEmpty()) w->grab().save(dir + "/" + name + ".png");
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

// Adds a node of `type` through the Tab search (typed text, Enter); returns the new id (0 = none)
int addBySearch(CompCanvas* canvas, Project* p, int clipId, const QString& text)
{
    const int before = graphOf(p, clipId).nextId();
    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_Tab);
    settle();
    CompNodeSearch* search = canvas->search();
    if (!CHECK(search && search->isVisible())) return 0;
    auto* field = search->findChild<QLineEdit*>("comp_node_search_text");
    if (!CHECK(field)) return 0;
    QTest::keyClicks(field, text);
    QTest::keyClick(field, Qt::Key_Return);
    settle();
    return graphOf(p, clipId).node(before) ? before : 0;
}

void drag(QWidget* w, const QPointF& from, const QPointF& to)
{
    QTest::mousePress(w, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    QTest::mouseMove(w, ((from + to) / 2).toPoint());
    QTest::mouseMove(w, to.toPoint());
    QTest::mouseRelease(w, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    settle();
}

void overlayDrag(CompOverlay* o, const QRectF& frame, const QPointF& from, const QPointF& to, int steps)
{
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    CHECK(o->mousePress(&press, frame));
    for (int i = 1; i <= steps; ++i) {
        const QPointF at = from + (to - from) * double(i) / steps;
        QMouseEvent move(QEvent::MouseMove, at, at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        o->mouseMove(&move, frame);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    CHECK(o->mouseRelease(&release, frame));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("comp-page");
    if (!Check::haveFfmpeg()) {
        std::puts("SKIP: no ffmpeg for the test clip");
        return 77;
    }
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
    w.importFiles({file}, true);
    settle();
    Project* project = w.liveProject();
    QUndoStack* undo = project->undoStack();
    if (!CHECK(!project->timeline().video.isEmpty() && !project->timeline().video[0].clips.isEmpty()))
        return Check::result();
    const int clipId = project->timeline().video[0].clips[0].id;

    // Lazy: no page before it is shown
    CHECK(w.compPage() == nullptr);

    // "Open in Comp" on the selected clip: empty graph clip -> output, the page shows it (one undo step)
    w.liveSelection()->set({clipId});
    const int start = undo->index();
    InputBindings::instance().action("comp_open")->trigger();
    settle();
    CompPage* page = w.compPage();
    if (!CHECK(page)) return Check::result();
    CHECK(page->isVisible());
    CHECK_EQ(page->clip(), clipId);
    CHECK_EQ(undo->index(), start + 1);
    CompGraph g = graphOf(project, clipId);
    CHECK_EQ(int(g.nodes.size()), 1);
    CHECK_EQ(g.out, 1);
    CHECK(page->view() == CompPage::View::Layers); // the layer view first
    CHECK_EQ(int(page->layers()->layers().size()), 1);

    // Node view: Tab search adds a merge behind the clip (bg <- clip, output = merge)
    page->setView(CompPage::View::Nodes);
    settle();
    CompCanvas* canvas = page->canvas();
    CHECK(canvas->isVisible());
    canvas->setSelected(1);
    int idx = undo->index();
    const int merge = addBySearch(canvas, project, clipId, "merge");
    CHECK(merge > 0);
    CHECK_EQ(undo->index(), idx + 1);
    g = graphOf(project, clipId);
    CHECK(g.linkInto(merge, "bg") && g.linkInto(merge, "bg")->from == 1);
    CHECK_EQ(g.out, merge);
    CHECK_EQ(canvas->selected(), merge);

    // A color generator, only added; wired into the merge's fg by dragging from its output (one undo step)
    idx = undo->index();
    const int color = addBySearch(canvas, project, clipId, "color");
    CHECK(color > 0);
    CHECK(graphOf(project, clipId).linkInto(merge, "fg") == nullptr);
    canvas->fit();
    settle();
    idx = undo->index();
    drag(canvas, canvas->outputPos(color), canvas->inputPos(merge, "fg"));
    g = graphOf(project, clipId);
    CHECK(g.linkInto(merge, "fg") && g.linkInto(merge, "fg")->from == color);
    CHECK_EQ(undo->index(), idx + 1);

    // A mask (added unlinked behind a picture) into the merge's mask input
    canvas->setSelected(color);
    const int mask = addBySearch(canvas, project, clipId, "mask");
    CHECK(mask > 0);
    CHECK(graphOf(project, clipId).out == merge); // the output stayed
    canvas->fit();
    settle();
    drag(canvas, canvas->outputPos(mask), canvas->inputPos(merge, "mask"));
    g = graphOf(project, clipId);
    CHECK(g.linkInto(merge, "mask") && g.linkInto(merge, "mask")->from == mask);

    // Layer view: color over the clip with the mask
    page->setView(CompPage::View::Layers);
    settle();
    const QVector<CompLayers::Layer> layers = page->layers()->layers();
    CHECK_EQ(int(layers.size()), 2);
    if (layers.size() == 2) {
        CHECK_EQ(layers[0].merge, merge);
        CHECK_EQ(layers[0].node, color);
        CHECK_EQ(layers[0].mask, mask);
        CHECK_EQ(layers[1].node, 1);
    }
    CHECK(page->layers()->findChild<QWidget*>("comp_layer_0") != nullptr);
    dump(&w, "comp-layers");

    // Outside edit (op 'node' of the CLI's @app edits calls Editor::setCompNodeValues): shown at once
    {
        Editor* editor = w.findChild<Editor*>();
        auto* opacity = dynamic_cast<ScrubField*>(page->layers()->findChild<QLineEdit*>("comp_layer_opacity_0"));
        CHECK(opacity != nullptr);
        page->selectNode(merge);
        CHECK_EQ(page->inspector()->node(), merge);
        QString e;
        if (CHECK(editor)) {
            CHECK(editor->setCompNodeValues(clipId, merge, {{"opacity", 0.25}}, &e));
            settle();
            if (opacity) CHECK(std::abs(opacity->value() - 25) < 0.01);
            auto* field = dynamic_cast<ScrubField*>(page->inspector()->findChild<QLineEdit*>("comp_param_opacity"));
            CHECK(field && std::abs(field->value() - 0.25) < 1e-6);
        }
    }

    // Handle drag on the mask's middle: one undo step for the whole drag, the position follows
    page->selectNode(mask);
    settle();
    CompOverlay* overlay = page->overlay();
    const QRectF frame(0, 0, 640, 360);
    QVector<CompOverlay::Handle> hs = overlay->handles();
    CHECK(hs.size() == 2); // middle + size corner (rect)
    idx = undo->index();
    overlayDrag(overlay, frame, overlay->handlePos(0, frame), overlay->handlePos(0, frame) + QPointF(64, 36), 6);
    CHECK_EQ(undo->index(), idx + 1);
    {
        const QVariantMap v = CompUi::nodeValues(*TimelineOps::findClip(project->timeline(), clipId), mask);
        CHECK(std::abs(v.value("x").toDouble() - 0.6) < 0.01);
        CHECK(std::abs(v.value("y").toDouble() - 0.6) < 0.01);
    }
    // The size corner: w/h around the middle
    idx = undo->index();
    const QPointF corner = overlay->handlePos(1, frame);
    overlayDrag(overlay, frame, corner, corner + QPointF(32, 0), 3);
    CHECK_EQ(undo->index(), idx + 1);
    CHECK(CompUi::nodeValues(*TimelineOps::findClip(project->timeline(), clipId), mask).value("w").toDouble() > 0.4);

    // Keyframe diamond of x: a keyframe at the playhead with the value shown (one undo step)
    settle();
    auto* diamond = page->inspector()->findChild<QToolButton*>("comp_key_x");
    if (CHECK(diamond)) {
        idx = undo->index();
        diamond->click();
        settle();
        CHECK_EQ(undo->index(), idx + 1);
        const Clip* c = TimelineOps::findClip(project->timeline(), clipId);
        const AnimParam p = Keys::effectParam(Comp::kEffectId, CompGraph::paramKey(mask, "x"));
        CHECK(Keys::animated(*c, p));
        CHECK(std::abs(Keys::valueAt(*c, p, 0) - 0.6) < 0.01);
        CHECK_EQ(diamond->text(), QString("◆"));
        // A drag of the handle now moves the keyframe's value, still one undo step
        idx = undo->index();
        overlayDrag(overlay, frame, overlay->handlePos(0, frame), overlay->handlePos(0, frame) - QPointF(64, 0), 4);
        CHECK_EQ(undo->index(), idx + 1);
        c = TimelineOps::findClip(project->timeline(), clipId);
        CHECK(std::abs(Keys::valueAt(*c, p, 0) - 0.5) < 0.01);
    }
    dump(&w, "comp-inspector-mask");

    // Ctrl+D: a copy of the mask with its values and keyframes
    page->setView(CompPage::View::Nodes);
    settle();
    canvas->setFocus();
    idx = undo->index();
    const int nextId = graphOf(project, clipId).nextId();
    QTest::keyClick(canvas, Qt::Key_D, Qt::ControlModifier);
    settle();
    CHECK_EQ(undo->index(), idx + 1);
    g = graphOf(project, clipId);
    CHECK(g.node(nextId) && g.node(nextId)->type == "mask");
    {
        const Clip* c = TimelineOps::findClip(project->timeline(), clipId);
        CHECK(Keys::animated(*c, Keys::effectParam(Comp::kEffectId, CompGraph::paramKey(nextId, "x"))));
    }
    CHECK_EQ(canvas->selected(), nextId);
    dump(&w, "comp-nodes");

    // Del removes the selected node (one undo step)
    idx = undo->index();
    QTest::keyClick(canvas, Qt::Key_Delete);
    settle();
    CHECK_EQ(undo->index(), idx + 1);
    CHECK(graphOf(project, clipId).node(nextId) == nullptr);
    CHECK_EQ(canvas->selected(), 0);

    // Moving a node stores its place (one undo step)
    canvas->fit();
    settle();
    idx = undo->index();
    const QRectF r = canvas->nodeRect(color);
    drag(canvas, r.center() - QPointF(0, r.height() / 2 - 6), r.center() - QPointF(0, r.height() / 2 - 6) + QPointF(40, 30));
    CHECK_EQ(undo->index(), idx + 1);

    // The viewer's picture of the selected node: the graph ends there
    {
        const Timeline view = CompUi::viewTimeline(project->renderTimeline(), clipId, color, false);
        CompGraph vg;
        for (const Track& t : view.video)
            for (const Clip& c : t.clips)
                if (c.id == clipId) CompUi::graphOf(c, &vg);
        CHECK_EQ(vg.out, color);
        const Timeline matte = CompUi::viewTimeline(project->renderTimeline(), clipId, 0, true);
        for (const Track& t : matte.video)
            for (const Clip& c : t.clips)
                if (c.id == clipId) {
                    CompUi::graphOf(c, &vg);
                    CHECK(vg.node(vg.out) && vg.node(vg.out)->type == "merge" && vg.out > merge);
                }
    }

    // Edit page: the Inspector says how big the graph is and opens the page
    w.showPage(MainWindow::Page::Edit);
    w.liveSelection()->set({clipId});
    settle();
    auto* row = w.findChild<QWidget*>("inspector_comp_row");
    CHECK(row && !row->isHidden());
    if (row)
        for (QLabel* l : row->findChildren<QLabel*>())
            if (!l->text().isEmpty()) CHECK(l->text().contains(QString::number(graphOf(project, clipId).nodes.size())));
    if (auto* open = w.findChild<QPushButton*>("inspector_comp_open")) {
        open->click();
        settle();
        CHECK(page->isVisible());
        CHECK_EQ(page->clip(), clipId);
    } else {
        CHECK(false);
    }

    // Undo the whole graph: the page stays, empty; redo brings it back with the node view
    page->selectNode(mask);
    while (undo->index() > start) undo->undo();
    settle();
    CHECK(graphOf(project, clipId).isEmpty());
    CHECK_EQ(page->selectedNode(), 0);
    CHECK(page->findChild<QPushButton*>("comp_create")->isVisibleTo(page));
    while (undo->canRedo()) undo->redo();
    settle();
    CHECK(graphOf(project, clipId).node(mask) != nullptr);
    page->selectNode(mask);
    settle();
    CHECK_EQ(page->inspector()->node(), mask);
    return Check::result();
}
