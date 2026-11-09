// SPDX-License-Identifier: AGPL-3.0-only
// In-app tours (core/Tours.h, ui/TourOverlay, app/TourManager):
// - the built-in tour files parse, every text has a translation key (tools/i18n_check.py covers the dictionaries)
// - TourState: asked exactly once (any answer ends it), "Don't offer tours" stops every question, the menu switch and
//   "offer again" bring it back, suppressed (test runs) never asks and never writes
// - TourRunner: next/back/finish, a wait step goes on by itself when its condition becomes true (a fake signal), one
//   whose condition already holds does not wait, Skip passes a waiting step
// - placeBubble keeps the bubble inside the window and off the target
// - in the window: every target of every built-in tour is found on its page (offscreen), the bubble stays inside the
//   window and off the holes, Esc ends the tour, the wait step "clip on the timeline" goes on when a clip is placed.
// TOURS_DUMP=<folder>: a picture of every step (and of the question) for a look.

#include "check.h"

#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/TourManager.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Tours.h"
#include "engine/Engine.h"
#include "ui/TourOverlay.h"
#include "ui/comp/CompCanvas.h"
#include "ui/comp/CompPage.h"

#include <QAbstractButton>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QToolButton>
#include "core/Selection.h"
#include <QTemporaryDir>
#include <QTest>
#include <clocale>

namespace {

QString dumpDir() { return qEnvironmentVariable("TOURS_DUMP"); }

void dump(QWidget* w, const QString& name)
{
    if (!dumpDir().isEmpty()) w->grab().save(dumpDir() + "/" + name + ".png");
}

void testParse()
{
    const QVector<Tour> tours = Tours::builtIn();
    CHECK(tours.size() >= 2);
    CHECK_EQ(tours.value(0).id, QString("first_steps")); // by order
    CHECK_EQ(tours.value(0).offerOn, QString("start"));
    for (const Tour& t : tours) {
        CHECK(!t.title.isEmpty());
        Check::report(t.steps.size() >= 5 && t.steps.size() <= 8, "5-8 steps", __FILE__, __LINE__, t.id); // short
        for (const TourStep& s : t.steps) CHECK(!s.text.isEmpty());
    }
    QString err;
    CHECK(Tours::parse("{\"id\":\"x\",\"title\":\"X\",\"steps\":[{\"target\":\"a\"}]}", &err).id.isEmpty()); // no text
    CHECK(!err.isEmpty());
    const Tour t = Tours::parse(
        R"({"id":"x","title":"X","group":"guides","steps":[{"text":"a","wait":"clip_added","offer":"clips","action":"comp_nodes"}]})");
    CHECK_EQ(t.id, QString("x"));
    CHECK_EQ(t.group, QString("guides"));
    CHECK_EQ(t.steps.value(0).wait, QString("clip_added"));
    CHECK_EQ(t.steps.value(0).offer, QString("clips"));
    CHECK_EQ(t.steps.value(0).action, QString("comp_nodes"));
    CHECK(Tours::find("nope").id.isEmpty());
}

void testState()
{
    TourState::resetAll();
    TourState::setSuppressed(false);
    CHECK(TourState::offerTours());
    CHECK(TourState::shouldAsk("first_steps"));
    TourState::answer("first_steps", TourState::Answer::Later);
    CHECK(!TourState::shouldAsk("first_steps")); // "Later" never asks again
    CHECK_EQ(TourState::state("first_steps"), QString("later"));
    CHECK(TourState::shouldAsk("color"));
    TourState::answer("first_steps", TourState::Answer::Show);
    CHECK_EQ(TourState::state("first_steps"), QString("shown"));
    // "Don't offer tours": no page asks any more
    TourState::answer("color", TourState::Answer::Never);
    CHECK(!TourState::offerTours());
    CHECK(!TourState::shouldAsk("comp"));
    TourState::setOfferTours(true); // Help › Tours › "Offer tours"
    CHECK(TourState::shouldAsk("comp"));
    CHECK(!TourState::shouldAsk("color"));
    TourState::markShown("comp"); // started from the menu: not asked later
    CHECK(!TourState::shouldAsk("comp"));
    TourState::resetAll();
    CHECK(TourState::shouldAsk("color"));
    // Test runs: never asked, nothing written
    TourState::setSuppressed(true);
    CHECK(!TourState::shouldAsk("color"));
    TourState::answer("color", TourState::Answer::Never);
    TourState::markShown("color");
    CHECK(TourState::offerTours());
    CHECK(TourState::state("color").isEmpty());
    TourState::setSuppressed(false);
}

// A fake "core signal" for the runner's wait steps
class FakeCore : public QObject {
    Q_OBJECT
public:
    bool clipThere = false;
    int begun = 0;
signals:
    void changed();
};

void testRunner()
{
    Tour t;
    t.id = "t";
    t.title = "T";
    for (const char* w : {"", "clip", "", "clip"}) {
        TourStep s;
        s.text = "x";
        s.wait = w;
        t.steps << s;
    }
    FakeCore core;
    TourRunner r;
    r.setConditions([&core](const TourStep&) { ++core.begun; },
                    [&core](const QString& c) { return c == "clip" && core.clipThere; });
    QObject::connect(&core, &FakeCore::changed, &r, &TourRunner::check);
    int finishedCompleted = -1;
    QObject::connect(&r, &TourRunner::finished, [&](bool c) { finishedCompleted = c; });
    r.start(t);
    CHECK(r.running());
    CHECK_EQ(r.index(), 0);
    CHECK(!r.waiting());
    CHECK_EQ(core.begun, 1);
    emit core.changed(); // nothing waits: stays
    CHECK_EQ(r.index(), 0);
    r.next();
    CHECK_EQ(r.index(), 1);
    CHECK(r.waiting());
    emit core.changed(); // still no clip
    CHECK_EQ(r.index(), 1);
    core.clipThere = true;
    emit core.changed(); // the user did it: on by itself
    CHECK_EQ(r.index(), 2);
    CHECK(!r.waiting());
    r.back();
    CHECK_EQ(r.index(), 1);
    CHECK(!r.waiting()); // the condition holds already: a normal step
    r.next();
    r.next();
    CHECK_EQ(r.index(), 3);
    CHECK(!r.waiting());
    core.clipThere = false;
    r.back();
    r.next(); // entered again without a clip: waits
    CHECK(r.waiting());
    r.next(); // Skip on the last (waiting) step: completed
    CHECK(!r.running());
    CHECK_EQ(finishedCompleted, 1);
    r.start(t);
    r.finish(); // Esc
    CHECK(!r.running());
    CHECK_EQ(finishedCompleted, 0);
    r.back(); // not running: nothing
    r.next();
    CHECK(!r.running());
}

void testPlace()
{
    const QRect area(0, 0, 1600, 950);
    const QSize size(360, 140);
    // No target: centered
    CHECK_EQ(TourOverlay::placeBubble(area, {}, size).center(), area.center());
    const auto ok = [&](const QRect& target) {
        const QRect r = TourOverlay::placeBubble(area, target, size);
        CHECK(area.contains(r));
        if (target.width() < 1000 || target.height() < 600) CHECK(!r.intersects(target));
        return r;
    };
    CHECK(ok(QRect(100, 100, 200, 80)).top() > 180); // below
    CHECK(ok(QRect(100, 850, 200, 80)).bottom() < 850); // at the bottom edge: above
    ok(QRect(0, 0, 400, 950));     // full height at the left: right of it
    ok(QRect(1200, 0, 400, 950));  // full height at the right: left of it
    ok(QRect(1500, 40, 100, 30));  // in a corner
    ok(QRect(0, 0, 1600, 950));    // everything: inside, still in the window
}

bool insideAndOff(TourOverlay* o)
{
    const QRect b = o->bubbleGeometry();
    bool ok = o->rect().contains(b);
    for (const QRect& h : o->holes())
        if (h.intersects(b) && h.width() < o->width() * 2 / 3 && h.height() < o->height() * 2 / 3) ok = false;
    return ok;
}

// The visible one (buttons of an earlier step are deleted later)
QPushButton* button(TourOverlay* o, const char* name)
{
    for (QPushButton* b : o->findChildren<QPushButton*>(name))
        if (!b->isHidden()) return b;
    return nullptr;
}

// First Steps on an empty project: the sample clips come from the resources, and the step waiting for media goes on
void testSample(MainWindow& w)
{
    TourManager* tours = w.tours();
    TourOverlay* o = tours->overlay();
    CHECK(w.liveProject()->media().isEmpty());
    tours->start("first_steps");
    tours->runner()->next(); // step 2: the Media Pool, waits for media, offers the sample clips
    CHECK(tours->runner()->waiting());
    QPushButton* offer = button(o, "tour_offer");
    if (!CHECK(offer)) return;
    CHECK_EQ(offer->text(), T("Beispielclips laden"));
    dump(&w, "first_steps-2-empty");
    offer->click();
    QTest::qWait(50);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/tour-sample";
    CHECK_EQ(w.liveProject()->media().size(), qsizetype(3));
    for (const MediaInfo& m : w.liveProject()->media()) {
        CHECK(m.path.startsWith(dir)); // copied from the built-in resources, nothing downloaded
        QFile copy(m.path), res(":/templates/gaming-highlight/media/" + QFileInfo(m.path).fileName());
        if (CHECK(copy.open(QIODevice::ReadOnly) && res.open(QIODevice::ReadOnly))) CHECK(copy.readAll() == res.readAll());
        CHECK(m.hasVideo);
    }
    CHECK_EQ(tours->runner()->index(), 2); // the wait step went on by itself
    CHECK(!button(o, "tour_offer"));       // not offered again once there are clips
    tours->stop();
}

void testWindow(MainWindow& w)
{
    TourManager* tours = w.tours();
    if (!CHECK(tours)) return;
    TourOverlay* o = tours->overlay();
    // Help › Tours lists every tour, the guides in their submenu
    QMenu* menu = w.menuBar()->findChild<QMenu*>("tours_menu");
    if (CHECK(menu)) {
        int n = 0, guides = 0;
        for (QAction* a : menu->findChildren<QAction*>())
            if (a->objectName().startsWith("tour_")) ++n;
        if (QMenu* g = menu->findChild<QMenu*>("tours_guides"); CHECK(g))
            for (QAction* a : g->actions()) guides += a->objectName().startsWith("tour_");
        CHECK_EQ(n, int(Tours::builtIn().size()));
        CHECK(guides >= 3);
    }

    // The question: once, with the third button at the first start
    TourState::resetAll();
    tours->offerAtStart();
    CHECK(tours->asking());
    CHECK(o->isVisible());
    CHECK(button(o, "tour_never"));
    QTest::qWait(50);
    dump(&w, "question");
    if (auto* later = button(o, "tour_later")) later->click();
    CHECK(!o->isVisible());
    CHECK_EQ(TourState::state("first_steps"), QString("later"));
    tours->offerAtStart(); // never twice
    CHECK(!tours->asking());
    // Suppressed (test runs): no question at all
    TourState::resetAll();
    TourState::setSuppressed(true);
    tours->offerAtStart();
    CHECK(!tours->asking());
    TourState::setSuppressed(false);
    // Esc on the question = Later
    tours->offerAtStart();
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(o, &esc);
    CHECK(!o->isVisible());
    CHECK_EQ(TourState::state("first_steps"), QString("later"));
    // A page opened by the user asks about its tour once (the Comp page: Compositing; no question while one runs)
    if (auto* b = w.findChild<QAbstractButton*>("pageButton_comp"); CHECK(b)) {
        b->click();
        CHECK(tours->asking());
        dump(&w, "question-comp");
        if (auto* later = button(o, "tour_later")) later->click();
        CHECK_EQ(TourState::state("compositing"), QString("later"));
        if (auto* edit = w.findChild<QAbstractButton*>("pageButton_edit")) edit->click();
        b->click();
        CHECK(!tours->asking());
        if (auto* edit = w.findChild<QAbstractButton*>("pageButton_edit")) edit->click();
    }

    // A wait step that goes on when the user does it: a clip onto the timeline (first_steps step 4)
    const Tour first = Tours::find("first_steps");
    int clipStep = -1;
    for (int i = 0; i < first.steps.size(); ++i)
        if (first.steps[i].wait == "clip_on_timeline") clipStep = i;
    if (CHECK(clipStep > 0)) {
        tours->start("first_steps");
        while (tours->runner()->index() < clipStep) tours->runner()->next();
        CHECK(tours->runner()->waiting());
        if (auto* next = button(o, "tour_next"); CHECK(next)) CHECK_EQ(next->text(), T("Überspringen"));
        // The real clip the other tours work on: a sample clip at the start of the timeline
        w.editor()->sourceEdit(Editor::SourceEditMode::AppendAtEnd, w.liveProject()->media().value(0).path, 0, 0);
        QTest::qWait(30);
        CHECK_EQ(tours->runner()->index(), clipStep + 1);
        QKeyEvent esc2(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); // Esc ends it
        QApplication::sendEvent(&w, &esc2);
        CHECK(!tours->runner()->running());
        CHECK(!o->isVisible());
    }

    // A panel the user switched off is shown for its step and hidden again afterwards (toggle and setting untouched)
    QToolButton* poolToggle = nullptr;
    for (QToolButton* b : w.findChildren<QToolButton*>())
        if (b->text().endsWith(T("Medienpool"))) poolToggle = b;
    QWidget* pool = nullptr;
    for (QWidget* c : w.findChildren<QWidget*>())
        if (c->property("tourTarget").toString() == "mediaPool") pool = c;
    if (CHECK(poolToggle && pool)) {
        poolToggle->click();
        CHECK(!pool->isVisible());
        tours->start("first_steps");
        tours->runner()->next();
        CHECK(pool->isVisible());
        CHECK(!poolToggle->isChecked());
        CHECK_EQ(tours->resolve("mediaPool").size(), qsizetype(1));
        tours->runner()->next(); // step 3 shows it too, step 4 as well
        CHECK(pool->isVisible());
        tours->stop();
        CHECK(!pool->isVisible());
        CHECK(!poolToggle->isChecked());
        poolToggle->click();
        CHECK(pool->isVisible());
    }

    // Every step of every tour: its targets are found, the bubble in the window and off the holes
    for (const Tour& t : Tours::builtIn()) {
        w.liveSelection()->clear();
        tours->start(t.id);
        TourRunner* r = tours->runner();
        for (int i = 0; i < t.steps.size(); ++i) {
            if (!CHECK_EQ(r->index(), i)) break;
            QTest::qWait(30);
            if (i == 0 && t.id == "compositing") { // the example graph: built, and the waiting step goes on
                QPushButton* ex = button(o, "tour_offer");
                if (CHECK(ex)) {
                    dump(&w, t.id + "-1");
                    ex->click();
                    QTest::qWait(50);
                    if (!CHECK_EQ(r->index(), 1)) break;
                    continue;
                }
            }
            const TourStep& s = t.steps[i];
            const QVector<QRect> holes = tours->resolve(s.target);
            if (!s.target.isEmpty())
                Check::report(holes.size() == s.target.count('|') + 1, "all targets found", __FILE__, __LINE__,
                              t.id + " step " + QString::number(i + 1) + ": " + s.target);
            CHECK(o->isVisible());
            CHECK(insideAndOff(o));
            if (!s.page.isEmpty()) // the tour shows the step's page
                if (auto* b = w.findChild<QAbstractButton*>("pageButton_" + s.page); CHECK(b)) CHECK(b->isChecked());
            if (auto* counter = o->findChild<QLabel*>("tour_counter"))
                CHECK_EQ(counter->text(), QString("%1/%2").arg(i + 1).arg(t.steps.size()));
            dump(&w, QString("%1-%2").arg(t.id).arg(i + 1));
            auto* next = button(o, "tour_next");
            if (!CHECK(next)) break;
            next->click(); // also skips a waiting step
        }
        CHECK(!r->running());
        CHECK(!o->isVisible());
    }
    // The empty graph's button starts the Compositing tour
    w.openComp(w.liveProject()->timeline().video.value(0).clips.value(0).id);
    w.liveProject()->undoStack()->undo(); // the example graph away: the clip has none
    QTest::qWait(30);
    if (CompPage* page = w.compPage(); CHECK(page)) {
        if (auto* nodes = page->findChild<QToolButton*>("comp_view_nodes")) nodes->setChecked(true);
        page->canvas()->refresh();
        QTest::qWait(30);
        QPushButton* start = page->canvas()->tourButton();
        CHECK(start->isVisible());
        dump(&w, "comp-empty-graph");
        start->click();
        CHECK(tours->runner()->running());
        CHECK_EQ(tours->runner()->tour().id, QString("compositing"));
        tours->stop();
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("tours");
    Theme::load({});
    Theme::apply(app); // the pictures look like the app
    testParse();
    testState();
    testRunner();
    testPlace();

    QTemporaryDir dir;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&w));
    testSample(w);
    testWindow(w);
    return Check::result();
}

#include "test_tours.moc"
