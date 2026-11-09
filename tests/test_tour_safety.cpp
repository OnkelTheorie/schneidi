// SPDX-License-Identifier: AGPL-3.0-only
// Tour mode swaps the user's project for a practice project (app/MainWindowTours.cpp). Nothing of the user's may get
// lost or changed by that swap (test_tours walks the tours themselves):
// - no tour while the user's render queue renders (its job ids are per project); a practice render ends with the tour
// - the user's file stays the open project for AIs; a change from outside during the tour is loaded afterwards,
//   never overwritten by the next save
// - Esc in an open menu or an editor is theirs, not the tour's; a menu open when the tour starts is closed
// - the clipboards, the Comp page's clip, the playhead and the timeline's zoom and scroll come back as they were
// - a window closed during a tour

#include "check.h"

#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/TourManager.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/Tours.h"
#include "core/Types.h"
#include "engine/Engine.h"
#include "engine/RenderQueue.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"
#include "ui/KeyPickWindow.h"
#include "ui/TourOverlay.h"
#include "ui/comp/CompShared.h"
#include "ui/comp/CompPage.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <clocale>
#include <memory>

namespace {

QPushButton* button(TourOverlay* o, const char* name)
{
    for (QPushButton* b : o->bubble()->findChildren<QPushButton*>(name))
        if (!b->isHidden()) return b; // the buttons of earlier steps wait hidden
    return nullptr;
}

bool startTour(MainWindow& w, const char* id = "editing")
{
    w.tours()->start(id);
    if (QPushButton* keep = button(w.tours()->overlay(), "tour_keep")) keep->click(); // unsaved changes: kept
    QTest::qWait(20);
    return w.tours()->runner()->running();
}

int clipCount(Project* p)
{
    int n = 0;
    for (const Track& t : p->timeline().video) n += t.clips.size();
    for (const Track& t : p->timeline().audio) n += t.clips.size();
    return n;
}

RenderJob job(int id, const QString& path)
{
    RenderJob j;
    j.id = id;
    j.path = path;
    j.settings.format = "h264";
    j.settings.encoder = VideoEncoder::Software;
    j.inOut = true;
    j.from = 0;
    j.to = 20;
    return j;
}

std::unique_ptr<MainWindow> window(Engine& engine)
{
    auto w = std::make_unique<MainWindow>(&engine);
    w->disableAutosave();
    w->resize(1600, 960);
    w->show();
    w->activateWindow();
    CHECK(QTest::qWaitForWindowActive(w.get()));
    QTest::qWait(30);
    return w;
}

QString sampleClip()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/tour-sample/clip1.mp4";
}

// The user's render runs: no tour (a status message says why); a practice render ends with the tour and never marks
// the user's job with the same id
void testRender(Engine& engine, const QTemporaryDir& dir)
{
    auto w = window(engine);
    Project* p = w->liveProject();
    RenderQueue* queue = w->findChild<DeliverPanel*>()->renderQueue();
    w->editor()->addTitle(0, 0, 60, TitleKind::Text);
    p->setRenderQueue({job(1, dir.filePath("user.mp4"))});
    CHECK(queue->start());
    CHECK(queue->isRunning());
    w->tours()->start("editing");
    CHECK(!w->tours()->runner()->running());
    CHECK(!w->tours()->overlay()->isVisible());
    CHECK(!w->statusBar()->currentMessage().isEmpty());
    CHECK_EQ(clipCount(p), 1); // still the user's project
    queue->cancel();
    CHECK(!queue->isRunning());
    const RenderStatus before = p->renderQueue().value(0).status;
    CHECK(before != RenderStatus::Done);

    // A render in the practice project: canceled when the tour ends, the user's job 1 stays as it was
    if (!CHECK(startTour(*w))) return;
    CHECK(clipCount(p) > 0);
    p->setRenderQueue({job(1, dir.filePath("practice.mp4"))});
    CHECK(queue->start());
    w->tours()->stop();
    CHECK(!queue->isRunning());
    QElapsedTimer t;
    t.start();
    while (queue->isRunning() && t.elapsed() < 20000) QTest::qWait(50);
    QTest::qWait(100);
    CHECK_EQ(p->renderQueue().size(), qsizetype(1));
    CHECK_EQ(p->renderQueue().value(0).path, dir.filePath("user.mp4"));
    CHECK(p->renderQueue().value(0).status == before);
}

// The user's file: the open project for AIs during the tour; changed from outside meanwhile, it is loaded afterwards
void testOutsideChange(Engine& engine, const QTemporaryDir& dir)
{
    const QString path = dir.filePath("mine.schneidi");
    {
        Project p;
        Selection sel;
        Editor e(&p, &sel);
        e.addTitle(0, 0, 60, TitleKind::Text);
        CHECK(ProjectFile::save(p.data(), path, nullptr));
    }
    auto w = window(engine);
    if (!CHECK(w->openProject(path))) return;
    CHECK_EQ(clipCount(w->liveProject()), 1);
    if (!CHECK(startTour(*w))) return;
    CHECK_EQ(w->liveProjectPath(), path); // AIs (CLI, MCP) see the file open in the app: they wait, no direct write
    { // a CLI writing the file anyway (an older one, another program)
        ProjectData d;
        CHECK(ProjectFile::load(path, &d, nullptr));
        Project p;
        p.load(d);
        Selection sel;
        Editor e(&p, &sel);
        e.addTitle(200, 0, 60, TitleKind::Text);
        QTest::qWait(1100); // a new modification time too
        CHECK(ProjectFile::save(p.data(), path, nullptr));
    }
    w->tours()->stop();
    QTest::qWait(50);
    CHECK_EQ(clipCount(w->liveProject()), 2); // the change is there (the user had nothing unsaved)
    CHECK(!w->liveProject()->isModified());
    CHECK_EQ(w->liveProjectPath(), path);
}

// Esc: the open menu's and the editor's, not the tour's; a menu open at the start is closed
void testEscape(Engine& engine)
{
    auto w = window(engine);
    w->editor()->addTitle(0, 0, 60, TitleKind::Text);
    auto* menu = new QMenu(w.get());
    menu->addAction("one");
    menu->popup(w->mapToGlobal(QPoint(300, 300)));
    QTest::qWait(30);
    CHECK(menu->isVisible());
    if (!CHECK(startTour(*w))) return;
    CHECK(!menu->isVisible()); // closed before the projects traded places
    menu->popup(w->mapToGlobal(QPoint(300, 300)));
    QTest::qWait(30);
    if (CHECK(QApplication::activePopupWidget() == menu)) {
        QTest::keyClick(menu, Qt::Key_Escape);
        QTest::qWait(20);
        CHECK(w->tours()->runner()->running());
    }
    menu->close();
    auto* edit = new QLineEdit(w->centralWidget());
    edit->show();
    edit->setFocus();
    QTest::qWait(20);
    if (CHECK(QApplication::focusWidget() == edit)) {
        QTest::keyClick(edit, Qt::Key_Escape);
        QTest::qWait(20);
        CHECK(w->tours()->runner()->running());
    }
    delete edit;
    QTest::qWait(20);
    QTest::keyClick(w.get(), Qt::Key_Escape); // nothing else wants it: the tour ends
    QTest::qWait(20);
    CHECK(!w->tours()->runner()->running());
}

// What the user had besides the project: the clipboards, the Comp page's clip, the playhead, zoom and scroll
void testUserState(Engine& engine)
{
    auto w = window(engine);
    Project* p = w->liveProject();
    w->tours()->start("editing"); // the practice clips copied in place (and the window's first tour out of the way)
    w->tours()->stop();
    for (int i = 0; i < 9; ++i) w->editor()->addTitle(300 * i, 0, 60, TitleKind::Text); // ids beyond the practice clips'
    w->importFiles({sampleClip()}, true, false);
    int video = 0, title = 0;
    for (const Track& t : p->timeline().video)
        for (const Clip& c : t.clips) {
            if (c.kind == ClipKind::Media) video = c.id;
            else if (!title) title = c.id;
        }
    if (!CHECK(video && title)) return;
    // Many tracks: the timeline scrolls down
    for (int i = 0; i < 14; ++i) w->editor()->addTrack(TrackKind::Video, 0);
    w->openComp(video);
    QTest::qWait(30);
    CHECK_EQ(w->compPage()->clip(), video);
    // The Color page with a clip selected the playhead is not in
    w->showPage(MainWindow::Page::Color);
    w->liveSelection()->set({title});
    QTest::qWait(30);
    auto* tv = w->findChild<TimelineView*>();
    const int at = 5000;
    w->liveSeek(at);
    QTest::qWait(50);
    CHECK_EQ(tv->playhead(), at);
    tv->setZoomAndScroll(tv->view().pxPerFrame * 1.7, 123, 100000); // as far down as it goes
    QTest::qWait(30);
    const ViewState view = tv->view();
    CHECK(view.scrollY > 0);
    CHECK(w->editor()->copyGrade(video));
    // Another frame rate in the user's project than in the practice project's clips
    ProjectFormat f = p->format();
    f.rate = {24, 1};
    w->setProjectFormat(f);
    QTest::qWait(30);
    CHECK(w->editor()->hasCopiedGrade());
    const ViewState view24 = tv->view();

    if (!CHECK(startTour(*w))) return;
    int practice = 0;
    for (const Track& t : p->timeline().video)
        for (const Clip& c : t.clips) practice = c.id;
    if (CHECK(practice) && practice != video) w->openComp(practice); // a step on the Comp page
    w->showPage(MainWindow::Page::Deliver);                          // the tours end on Deliver
    QTest::qWait(30);
    w->tours()->stop();
    QTest::qWait(100);

    CHECK(w->currentPage() == MainWindow::Page::Color);
    Check::report(tv->playhead() == at, "playhead stays", __FILE__, __LINE__, QString("%1 -> %2").arg(at).arg(tv->playhead()));
    CHECK_EQ(w->compPage()->clip(), video);
    CHECK(w->editor()->hasCopiedGrade());
    Check::report(tv->view().scrollY == view24.scrollY && std::abs(tv->view().pxPerFrame - view24.pxPerFrame) < 1e-9
                      && std::abs(tv->view().leftFrame - view24.leftFrame) < 1e-9,
                  "zoom and scroll back", __FILE__, __LINE__,
                  QString("%1/%2/%3 -> %4/%5/%6").arg(view24.pxPerFrame).arg(view24.leftFrame).arg(view24.scrollY)
                      .arg(tv->view().pxPerFrame).arg(tv->view().leftFrame).arg(tv->view().scrollY));
}

// The practice clips have handles (a transition on a cut needs no question); the Inspector's step selects a clip
void testPracticeProject(Engine& engine)
{
    auto w = window(engine);
    if (!CHECK(startTour(*w))) return;
    const Timeline& tl = w->liveProject()->timeline();
    if (!CHECK(!tl.video.isEmpty() && tl.video[0].clips.size() >= 2)) return;
    const Clip& a = tl.video[0].clips[0];
    const Clip& b = tl.video[0].clips[1];
    CHECK(a.in > 0 && b.in > 0);
    CHECK_EQ(b.start, a.end()); // still one after another
    CHECK(w->editor()->transitionsLackingHandles(b.start, TransitionStyle{}, TrackKind::Video).isEmpty());
    // To the Inspector's step with nothing selected: it selects a clip
    TourRunner* r = w->tours()->runner();
    while (r->running() && r->index() < 5)
        if (QPushButton* next = button(w->tours()->overlay(), "tour_next"); CHECK(next)) {
            w->liveSelection()->clear();
            next->click();
            QTest::qWait(20);
        } else {
            break;
        }
    if (CHECK(r->step()) && CHECK_EQ(r->step()->target, QString("inspector"))) CHECK(!w->liveSelection()->ids().isEmpty());
    w->tours()->stop();
}

// A recipe dropped on a clip selects that clip (the Comp page and the Inspector show it next)
void testRecipeDrop(Engine& engine)
{
    auto w = window(engine);
    w->importFiles({sampleClip(), sampleClip()}, true, true);
    const Timeline& tl = w->liveProject()->timeline();
    if (!CHECK(!tl.video.isEmpty() && tl.video[0].clips.size() == 2)) return;
    const int a = tl.video[0].clips[0].id, b = tl.video[0].clips[1].id;
    w->liveSelection()->set({a});
    emit w->findChild<TimelineView*>()->recipeDropped(b, "pip_frame");
    QTest::qWait(50);
    CHECK(w->liveSelection()->ids().contains(b));
    CHECK(!w->liveSelection()->ids().contains(a));
}

// Closed during a tour (both histories hold steps): no slot runs on the half-destroyed window
void testCloseInTour(Engine& engine)
{
    auto w = window(engine);
    w->editor()->addTitle(0, 0, 60, TitleKind::Text);
    if (!CHECK(startTour(*w))) return;
    w->editor()->addTitle(0, 1, 30, TitleKind::Text);
    w.reset();
    CHECK(true);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("tour_safety");
    Theme::load({});
    Theme::apply(app);
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir dir;
    testRender(engine, dir);
    testOutsideChange(engine, dir);
    testEscape(engine);
    testUserState(engine);
    testPracticeProject(engine);
    testRecipeDrop(engine);
    testCloseInTour(engine);
    return Check::result();
}
