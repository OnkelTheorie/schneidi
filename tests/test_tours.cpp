// SPDX-License-Identifier: AGPL-3.0-only
// In-app tours (core/Tours.h, ui/TourOverlay, app/TourManager):
// - the built-in tour files parse, every text has a translation key (tools/i18n_check.py covers the dictionaries)
// - TourState: asked exactly once (any answer ends it), "Don't offer tours" stops every question, the menu switch and
//   "offer again" bring it back, suppressed (test runs) never asks and never writes
// - TourRunner: next/back/finish, a wait step goes on by itself when its condition becomes true (a fake signal), one
//   whose condition already holds does not wait, Skip passes a waiting step
// - placeBubble keeps the bubble inside the window, off every hole and beside the main target
// - the bubble in every language, also with a 1.25x font: every button shows its whole text, every label its lines
// - in the window, with the default settings and with a real user's settings (tests/data/tours/user-settings.conf:
//   Media Pool off, Effects on, own sizes), in English and Russian: every step of every tour on its practice project;
//   the center of each hole hits its target (not another panel, not the overlay), the target is big enough, the bubble
//   fits, stays in the window and off the holes. After each tour the user's project (unsaved changes too), page,
//   panels, sizes and settings are back as they were; with unsaved changes the tour asks first.
// TOURS_DUMP=<folder>: a picture of every step (the user-settings runs, the questions) for a look.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "cli/Live.h"
#include "app/Theme.h"
#include "app/TourManager.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "core/Tours.h"
#include "engine/Engine.h"
#include "ui/EffectsLibrary.h"
#include "ui/TourOverlay.h"
#include "ui/comp/CompCanvas.h"
#include "ui/comp/CompPage.h"
#include "ui/timeline/TimelineView.h"

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
#include "core/Types.h"
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <cmath>
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
        Check::report(t.steps.size() >= 5 && t.steps.size() <= 9, "5-9 steps", __FILE__, __LINE__, t.id); // short
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
    const auto ok = [&](const QVector<QRect>& holes) {
        const QRect r = TourOverlay::placeBubble(area, holes, size);
        CHECK(area.contains(r));
        for (const QRect& h : holes)
            if (h.width() < 1000 || h.height() < 600) CHECK(!r.intersects(h));
        return r;
    };
    CHECK(ok({QRect(100, 100, 200, 80)}).top() > 180);    // below
    CHECK(ok({QRect(100, 850, 200, 80)}).bottom() < 850); // at the bottom edge: above
    ok({QRect(0, 0, 400, 950)});    // full height at the left: right of it
    ok({QRect(1200, 0, 400, 950)}); // full height at the right: left of it
    ok({QRect(1500, 40, 100, 30)}); // in a corner
    ok({QRect(0, 0, 1600, 950)});   // everything: inside, still in the window
    // Far apart targets (the timeline at the bottom, a menu at the top left): beside the main one, not in between
    const QRect timeline(0, 600, 1600, 300), menu(60, 0, 50, 24);
    const QRect r = ok({timeline, menu});
    CHECK(r.bottom() < timeline.top() && r.top() > timeline.top() - 14 - size.height() - 2);
}

// The visible one (buttons of an earlier step are deleted later)
QPushButton* button(TourOverlay* o, const char* name)
{
    for (QPushButton* b : o->findChildren<QPushButton*>(name))
        if (!b->isHidden()) return b;
    return nullptr;
}

// What does not fit in the bubble: a button narrower than its text, a label cut off, anything outside the bubble
QString bubbleProblems(TourOverlay* o)
{
    QStringList problems;
    QWidget* bubble = o->bubble();
    const QRect inside = bubble->rect();
    for (QPushButton* b : bubble->findChildren<QPushButton*>()) {
        if (b->isHidden()) continue;
        const int need = b->fontMetrics().horizontalAdvance(b->text()) + 8; // the text and a little padding
        if (b->width() < need || b->width() < b->sizeHint().width())
            problems << QString("button '%1' %2 px < %3").arg(b->text()).arg(b->width()).arg(std::max(need, b->sizeHint().width()));
        if (!inside.contains(b->geometry())) problems << QString("button '%1' outside the bubble").arg(b->text());
    }
    for (QLabel* l : bubble->findChildren<QLabel*>()) {
        if (l->isHidden() || l->text().isEmpty()) continue;
        const int needH = l->wordWrap() ? l->heightForWidth(l->width()) : l->sizeHint().height();
        if (l->height() < needH) problems << QString("label '%1' %2 px high < %3").arg(l->text().left(30)).arg(l->height()).arg(needH);
        if (!l->wordWrap() && l->width() < l->sizeHint().width()) problems << QString("label '%1' too narrow").arg(l->text());
        if (!inside.contains(l->geometry())) problems << QString("label '%1' outside the bubble").arg(l->text().left(30));
    }
    return problems.join("; ");
}

// The bubble in every language and with a larger font: each step as it can show (plain, waiting with the offer and
// Back), the questions
void testBubbleSizes()
{
    QWidget host;
    host.resize(1920, 1050);
    host.show();
    TourOverlay o(&host);
    const QFont base = QApplication::font();
    int widest = 0;
    for (const char* lang : {"de", "en", "es", "fr", "pl", "ru"}) { // German first: no dictionary installed yet
        I18n::install(lang);
        for (const double scale : {1.0, 1.25}) {
            QFont f = base;
            f.setPointSizeF(base.pointSizeF() * scale);
            QApplication::setFont(f);
            const auto check = [&](const QString& what) {
                QApplication::processEvents();
                o.refresh();
                const QString p = bubbleProblems(&o);
                Check::report(p.isEmpty(), "bubble fits", __FILE__, __LINE__,
                              QString("%1 x%2 %3: %4").arg(lang).arg(scale).arg(what, p));
                Check::report(host.rect().contains(o.bubbleGeometry()), "bubble in the window", __FILE__, __LINE__, what);
                widest = std::max(widest, o.bubbleGeometry().width());
            };
            for (const Tour& t : Tours::builtIn())
                for (int i = 0; i < t.steps.size(); ++i) {
                    o.showStep(T(t.steps[i].text), i, int(t.steps.size()), false);
                    check(QString("%1 %2").arg(t.id).arg(i + 1));
                    o.showStep(T(t.steps[i].text), std::max(1, i), int(t.steps.size()), true, T("Beispielclips laden"));
                    check(QString("%1 %2 waiting").arg(t.id).arg(i + 1));
                    if (QString(lang) == "ru" && scale > 1 && t.id == "first_steps" && i == 1 && !dumpDir().isEmpty())
                        o.bubble()->grab().save(dumpDir() + "/bubble-ru-125-waiting.png");
                }
            o.showQuestion(T("Erste Schritte"), T("Neu hier? Soll ich dir schneidi kurz zeigen? (ca. 1 Minute)"),
                           T("Du findest alle Rundgänge jederzeit unter Hilfe › Rundgänge."),
                           {{"tour_never", T("Keine Rundgänge anbieten"), {}, false, true},
                            {"tour_later", T("Später"), {}, false, false, true},
                            {"tour_show", T("Zeig's mir"), {}, true}});
            check("question");
            o.showQuestion(T("Compositing"),
                           T("Der Rundgang läuft in einem Übungsprojekt. Dein Projekt hat ungespeicherte Änderungen: Es "
                             "kommt nach dem Rundgang genau so zurück, wie es jetzt ist."),
                           {},
                           {{"tour_cancel", T("Abbrechen"), {}, false, true, true},
                            {"tour_save", T("Erst speichern"), {}},
                            {"tour_keep", T("Rundgang starten"), {}, true}});
            check("save question");
            if (QString(lang) == "ru" && scale > 1 && !dumpDir().isEmpty())
                o.bubble()->grab().save(dumpDir() + "/bubble-ru-125-save-question.png");
        }
    }
    QApplication::setFont(base);
    o.end();
    CHECK(widest <= TourOverlay::kMaxWidth);
    qInfo("widest bubble: %d px", widest);
}

bool insideAndOff(TourOverlay* o)
{
    const QRect b = o->bubbleGeometry();
    bool ok = o->rect().contains(b);
    for (const QRect& h : o->holes())
        if (h.intersects(b) && h.width() < o->width() * 2 / 3 && h.height() < o->height() * 2 / 3) ok = false;
    return ok;
}

QString describe(QWidget* w)
{
    if (!w) return "nothing";
    QString s = QString::fromLatin1(w->metaObject()->className());
    if (!w->objectName().isEmpty()) s += "#" + w->objectName();
    if (const QString t = w->property("tourTarget").toString(); !t.isEmpty()) s += "[" + t + "]";
    return s;
}

// One step as the user sees it: every target found and really visible at its hole (the hole's center hits it), big
// enough to see, the bubble whole, in the window and off the holes
void checkStep(MainWindow& w, const QString& where)
{
    TourManager* tours = w.tours();
    TourOverlay* o = tours->overlay();
    const TourStep* s = tours->runner()->step();
    if (!CHECK(s)) return;
    o->refresh();
    const QVector<TourManager::Target> targets = tours->targets(s->target);
    if (!s->target.isEmpty())
        Check::report(targets.size() == s->target.count('|') + 1, "all targets found", __FILE__, __LINE__,
                      where + ": " + s->target);
    for (const TourManager::Target& t : targets) {
        QWidget* hit = w.childAt(t.rect.center());
        Check::report(hit && (hit == t.widget || t.widget->isAncestorOf(hit)), "the hole shows its target", __FILE__,
                      __LINE__, QString("%1: %2 is %3, the center hits %4").arg(where, t.part, describe(t.widget), describe(hit)));
        // What the user sees of it: the hole (the target and a margin); a thin line (the playhead) counts by its area
        const QRect hole = t.rect.adjusted(-6, -6, 6, 6);
        Check::report(hole.height() >= 20 && hole.width() * hole.height() >= 40 * 20, "target big enough", __FILE__,
                      __LINE__, QString("%1: %2 %3x%4").arg(where, t.part).arg(t.rect.width()).arg(t.rect.height()));
    }
    const QString p = bubbleProblems(o);
    Check::report(p.isEmpty(), "bubble fits", __FILE__, __LINE__, where + ": " + p);
    Check::report(insideAndOff(o), "bubble in the window and off the holes", __FILE__, __LINE__, where);
    // The controls the text tells the user to use beside the target (the step's "keep"): the bubble keeps off them
    for (const TourManager::Target& k : tours->targets(s->keep))
        Check::report(!o->bubbleGeometry().intersects(k.rect), "bubble off the named controls", __FILE__, __LINE__,
                      where + ": " + k.part);
}

// What a tour must give back: the settings it could touch, the panels, the sizes, the page, the project
struct Look {
    QVariantMap settings;
    QVector<bool> toggles;
    QMap<QString, QList<int>> sizes; // the splitters on screen
    QSize window;
    QString page;
    int clips = 0, media = 0, playhead = 0;
    bool modified = false;
    QSet<int> selection;
    QString title;
};

const char* const kToggles[] = {"panelToggle_mediaPool", "panelToggle_effects", "panelToggle_mediaStorage",
                                "panelToggle_inspector", "panelToggle_mixer",  "panelToggle_transcript"};

int clipCount(Project* p)
{
    int n = 0;
    for (const Track& t : p->timeline().video) n += t.clips.size();
    for (const Track& t : p->timeline().audio) n += t.clips.size();
    return n;
}

Look look(MainWindow& w)
{
    Look l;
    QSettings s;
    for (const QString& k : s.allKeys())
        if (k.startsWith("panels/") || k.startsWith("layout/") || k.startsWith("color/")) l.settings[k] = s.value(k);
    for (const char* name : kToggles)
        if (auto* b = w.findChild<QToolButton*>(name); CHECK(b)) l.toggles << b->isChecked();
    for (const char* name : {"editTop", "editMain", "editRow", "colorBottom"})
        if (auto* sp = w.findChild<QSplitter*>(name); CHECK(sp) && sp->isVisible()) l.sizes[name] = sp->sizes();
    l.window = w.size();
    for (QAbstractButton* b : w.findChildren<QAbstractButton*>())
        if (b->objectName().startsWith("pageButton_") && b->isChecked()) l.page = b->objectName();
    l.clips = clipCount(w.liveProject());
    l.media = int(w.liveProject()->media().size());
    l.modified = w.liveProject()->isModified();
    l.selection = w.liveSelection()->ids();
    l.playhead = w.tours()->runner()->running() ? -1 : 0;
    l.title = w.windowTitle();
    return l;
}

void sameLook(const Look& a, const Look& b, const QString& where)
{
    Check::report(a.settings == b.settings, "settings untouched", __FILE__, __LINE__, where);
    Check::report(a.toggles == b.toggles, "panels back", __FILE__, __LINE__, where);
    Check::report(a.sizes.keys() == b.sizes.keys(), "same splitters on screen", __FILE__, __LINE__, where);
    Check::report(a.window == b.window, "window size", __FILE__, __LINE__,
                  QString("%1: %2x%3 -> %4x%5").arg(where).arg(a.window.width()).arg(a.window.height()).arg(b.window.width()).arg(b.window.height()));
    for (auto it = a.sizes.cbegin(); it != a.sizes.cend(); ++it) {
        QStringList x, y;
        for (int v : it.value()) x << QString::number(v);
        for (int v : b.sizes.value(it.key())) y << QString::number(v);
        Check::report(it.value() == b.sizes.value(it.key()), "sizes back", __FILE__, __LINE__,
                      QString("%1: %2 %3 -> %4").arg(where, it.key(), x.join(','), y.join(',')));
    }
    Check::report(a.page == b.page, "page back", __FILE__, __LINE__, where + ": " + a.page + " -> " + b.page);
    Check::report(a.clips == b.clips && a.media == b.media, "project back", __FILE__, __LINE__, where);
    Check::report(a.modified == b.modified, "unsaved state back", __FILE__, __LINE__, where);
    Check::report(a.selection == b.selection, "selection back", __FILE__, __LINE__, where);
    Check::report(a.title == b.title, "title back", __FILE__, __LINE__, where + ": " + a.title + " -> " + b.title);
}

// The timeline's zoom and scroll in seconds (a frame rate change would change them on screen too)
struct Zoom {
    double pxPerSecond = 0, leftSecond = 0;
    int scrollY = 0;
    bool operator==(const Zoom& o) const
    {
        return std::abs(pxPerSecond - o.pxPerSecond) < 1e-6 && std::abs(leftSecond - o.leftSecond) < 1e-6
            && scrollY == o.scrollY;
    }
    QString text() const { return QString("%1 px/s, left %2 s, y %3").arg(pxPerSecond).arg(leftSecond).arg(scrollY); }
};

Zoom zoomOf(MainWindow& w)
{
    auto* tv = w.findChild<TimelineView*>();
    if (!tv) return {};
    const double fps = w.liveProject()->timelineFormat().rate.fps();
    return {tv->view().pxPerFrame * fps, tv->view().leftFrame / fps, tv->view().scrollY};
}

// A dialog that pops up meanwhile (the format question on the first drop): noted and closed
struct DialogWatch {
    QStringList seen;
    QTimer timer;
    DialogWatch()
    {
        timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            if (QWidget* m = QApplication::activeModalWidget()) {
                seen << (m->windowTitle() + ": " + describe(m));
                m->close();
            }
        });
        timer.start();
    }
};

// One tour from start to end, acting like the user where it waits (the clip onto the timeline, the page button, the
// panel toggle, the offered button)
void walk(MainWindow& w, const Tour& t, const QString& run, bool dumpIt)
{
    TourManager* tours = w.tours();
    TourOverlay* o = tours->overlay();
    TourRunner* r = tours->runner();
    tours->start(t.id);
    if (QPushButton* keep = button(o, "tour_keep")) { // unsaved changes: asked first, the project kept for later
        QTest::qWait(20);
        o->refresh();
        const QString p = bubbleProblems(o);
        Check::report(p.isEmpty(), "question fits", __FILE__, __LINE__, run + " " + t.id + ": " + p);
        if (dumpIt) dump(&w, run + "-" + t.id + "-0-question");
        keep->click();
    }
    if (!CHECK(r->running())) return;
    DialogWatch dialogs;
    // The practice project: the sample clips, never the user's own
    const QString sample = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/tour-sample";
    CHECK_EQ(w.liveProject()->media().size(), qsizetype(3));
    for (const MediaInfo& m : w.liveProject()->media()) CHECK(m.path.startsWith(sample));
    CHECK_EQ(clipCount(w.liveProject()) == 0, t.practice == "pool");
    QTest::qWait(30);
    const Zoom zoom = zoomOf(w); // core rule: only the user zooms or scrolls the timeline, never a tour step
    const QSize size = w.size();
    QRect lastBubble;
    int moves = 0;
    QStringList movesText;
    for (int i = 0; i < t.steps.size(); ++i) {
        const QString where = QString("%1 %2 step %3").arg(run, t.id).arg(i + 1);
        if (!Check::report(r->index() == i, "step", __FILE__, __LINE__, where + QString(" (at %1)").arg(r->index() + 1)))
            break;
        QTest::qWait(30);
        checkStep(w, where);
        Check::report(zoomOf(w) == zoom, "timeline zoom untouched", __FILE__, __LINE__,
                      where + ": " + zoom.text() + " -> " + zoomOf(w).text());
        Check::report(w.size() == size, "window size untouched", __FILE__, __LINE__,
                      QString("%1: %2x%3 -> %4x%5").arg(where).arg(size.width()).arg(size.height()).arg(w.width()).arg(w.height()));
        const TourStep& s = t.steps[i];
        if (s.target.contains("viewer") && clipCount(w.liveProject()) > 0) { // the viewer shows a picture, not black
            const int at = w.findChild<TimelineView*>()->playhead();
            bool onClip = false;
            for (const Track& tr : w.liveProject()->timeline().video)
                for (const Clip& c : tr.clips) onClip |= at >= c.start && at < c.end();
            Check::report(onClip, "playhead on a clip where the viewer is shown", __FILE__, __LINE__, where);
        }
        if (!s.page.isEmpty()) // the tour shows the step's page
            if (auto* b = w.findChild<QAbstractButton*>("pageButton_" + s.page); CHECK(b)) CHECK(b->isChecked());
        if (auto* counter = o->findChild<QLabel*>("tour_counter"))
            CHECK_EQ(counter->text(), QString("%1/%2").arg(i + 1).arg(t.steps.size()));
        if (dumpIt) dump(&w, QString("%1-%2-%3").arg(run, t.id).arg(i + 1));
        // The bubble stays put where it can (its home corner); the same step puts it at the same place on a revisit
        const QRect at = o->bubbleGeometry();
        if (qEnvironmentVariableIsSet("TOURS_TRACE"))
            qInfo("%s %d: %d,%d %dx%d", qPrintable(t.id), i + 1, at.x(), at.y(), at.width(), at.height());
        if (i > 0 && at != lastBubble) {
            ++moves;
            movesText << QString("%1:(%2,%3)").arg(i + 1).arg(at.x()).arg(at.y());
        }
        lastBubble = at;
        if (i > 0) {
            if (auto* back = button(o, "tour_back"); CHECK(back)) back->click();
            QTest::qWait(30);
            if (auto* next = button(o, "tour_next"); CHECK(next)) next->click();
            QTest::qWait(30);
            o->refresh();
            Check::report(r->index() == i && o->bubbleGeometry() == at, "same place on a revisit", __FILE__, __LINE__,
                          QString("%1: (%2,%3) -> (%4,%5)").arg(where).arg(at.x()).arg(at.y())
                              .arg(o->bubbleGeometry().x()).arg(o->bubbleGeometry().y()));
            if (r->index() != i) break;
        }
        // The empty graph's own tour button never sits in the spotlight, nor the page's own "Create Compositing"
        if (CompPage* page = w.compPage()) {
            CHECK(!page->canvas()->tourButton()->isVisible());
            if (auto* create = page->findChild<QPushButton*>("comp_create")) CHECK(!create->isVisible());
        }
        if (r->waiting()) { // what the user does here: the step goes on by itself
            QAbstractButton* click = nullptr;
            if (s.wait.startsWith("page:")) click = w.findChild<QAbstractButton*>("pageButton_" + s.wait.mid(5));
            if (s.wait == "effects_open") click = w.findChild<QAbstractButton*>("panelToggle_effects");
            bool did = true;
            if (s.wait == "clip_on_timeline") { // dragged from the Media Pool onto the timeline (the drop's own path)
                auto* tv = w.findChild<TimelineView*>();
                if (CHECK(tv)) emit tv->dropRequested({w.liveProject()->media().value(0).path}, 0, 0);
            } else if (s.wait == "clip_trimmed") { // the first clip's end dragged to the left
                const Timeline& tl = w.liveProject()->timeline();
                if (CHECK(!tl.video.isEmpty() && !tl.video[0].clips.isEmpty()))
                    w.editor()->trimClip(tl.video[0].clips[0].id, TimelineOps::Edge::End, -10);
            } else if (s.wait == "clip_added") { // the playhead into the first clip, then the split key
                const Timeline& tl = w.liveProject()->timeline();
                auto* tv = w.findChild<TimelineView*>();
                QAction* split = nullptr;
                for (const auto& e : InputBindings::instance().entries())
                    if (e.id == QLatin1String("split")) split = e.action;
                if (CHECK(tv && split && !tl.video.isEmpty() && !tl.video[0].clips.isEmpty())) {
                    const Clip& c = tl.video[0].clips[0];
                    emit tv->seekRequested(c.start + c.length() / 2);
                    QTest::qWait(30);
                    split->trigger();
                }
            } else if (s.wait == "playhead_moved") { // a click into the ruler
                auto* tv = w.findChild<TimelineView*>();
                if (CHECK(tv)) emit tv->seekRequested(tv->playhead() + 5);
            } else if (click) {
                click->click();
            } else {
                did = false;
            }
            if (did) {
                QTest::qWait(60);
                Check::report(r->index() == i + 1, "goes on by itself", __FILE__, __LINE__, where + ": " + s.wait);
                Check::report(dialogs.seen.isEmpty(), "no dialog in the tour", __FILE__, __LINE__,
                              where + ": " + dialogs.seen.join(", "));
                if (s.wait == "effects_open")
                    if (auto* lib = w.findChild<EffectsLibrary*>(); CHECK(lib))
                        Check::report(lib->isVisible() && lib->height() >= 150, "effects shown", __FILE__, __LINE__,
                                      where + QString(": %1 px").arg(lib->height()));
                if (r->index() == i + 1) {
                    Check::report(zoomOf(w) == zoom, "timeline zoom untouched by the user's step", __FILE__, __LINE__,
                                  where + ": " + zoom.text() + " -> " + zoomOf(w).text());
                    continue;
                }
            }
        }
        if (QPushButton* offer = button(o, "tour_offer")) { // the button the step offers ("Create Example")
            offer->click();
            QTest::qWait(50);
            if (r->index() == i + 1) continue;
        }
        auto* next = button(o, "tour_next");
        if (!CHECK(next)) break;
        next->click(); // also skips a waiting step
    }
    CHECK(!r->running());
    CHECK(!o->isVisible());
    // Few moves: the bubble leaves its home only for a target or a named control there
    qInfo("%s %s: bubble moved %d times in %d steps %s", qPrintable(run), qPrintable(t.id), moves, int(t.steps.size()),
          qPrintable(movesText.join(' ')));
    Check::report(moves <= std::max(2, int(t.steps.size()) / 2), "bubble moves", __FILE__, __LINE__,
                  QString("%1 %2: %3 moves: %4").arg(run, t.id).arg(moves).arg(movesText.join(' ')));
    Check::report(dialogs.seen.isEmpty(), "no dialog in the tour", __FILE__, __LINE__, run + " " + t.id + ": " + dialogs.seen.join(", "));
}

// Fresh settings: a left panel that was never shown comes up with a usable height when its toggle is switched on
// (it once kept the size 0 it had in the splitter: the toggle lit up, nothing appeared)
void testPanelToggles(MainWindow& w)
{
    auto* left = w.findChild<QSplitter*>("editLeft");
    if (!CHECK(left)) return;
    for (const char* key : {"effects", "mediaStorage"}) {
        auto* toggle = w.findChild<QToolButton*>(QString("panelToggle_") + key);
        if (!CHECK(toggle)) continue;
        CHECK(!toggle->isChecked());
        toggle->click();
        QTest::qWait(30);
        int shown = 0;
        for (int i = 0; i < left->count(); ++i)
            if (left->widget(i)->isVisible()) {
                ++shown;
                Check::report(left->widget(i)->height() >= 150, "panel usable height", __FILE__, __LINE__,
                              QString("%1: %2 is %3 px").arg(key, describe(left->widget(i))).arg(left->widget(i)->height()));
            }
        CHECK(shown >= 2);
        toggle->click();
        QTest::qWait(30);
    }
    for (int i = 0; i < left->count(); ++i) // the Media Pool alone again, the whole column
        if (left->widget(i)->isVisible()) CHECK(left->widget(i)->height() >= left->height() - 10);
}

// The Color page without a clip: "Start the Color tour" starts it (and hides while it runs)
void testColorTourButton(MainWindow& w)
{
    auto* color = w.findChild<QAbstractButton*>("pageButton_color");
    auto* start = w.findChild<QPushButton*>("color_tour_button");
    if (!CHECK(color && start)) return;
    color->click();
    if (QPushButton* later = button(w.tours()->overlay(), "tour_later")) later->click(); // the page's own question
    QTest::qWait(20);
    CHECK(start->isVisible());
    dump(&w, "color-empty");
    start->click();
    CHECK(w.tours()->runner()->running());
    CHECK_EQ(w.tours()->runner()->tour().id, QString("color"));
    CHECK(!start->isVisible());
    w.tours()->stop();
    QTest::qWait(20);
    if (auto* edit = w.findChild<QAbstractButton*>("pageButton_edit")) edit->click();
}

// "{key:split}" in a step's text: the split key of the user's current key set, in bold; its name when it has none
void testKeyPlaceholder(MainWindow& w)
{
    InputBindings& b = InputBindings::instance();
    TourStep step;
    step.text = "drück {key:split}: geteilt";
    QStringList seen;
    for (const char* preset : {"standard", "premiere"}) {
        b.applyPreset(preset);
        const QString key = b.current("split").toString(QKeySequence::NativeText);
        CHECK(!key.isEmpty());
        const QString text = w.tours()->stepText(step);
        Check::report(text.contains("<b>" + key.toHtmlEscaped() + "</b>") && !text.contains("{key"), "key in the text",
                      __FILE__, __LINE__, QString("%1: %2 / %3").arg(preset, key, text));
        seen << key;
    }
    CHECK(seen.value(0) != seen.value(1)); // Ctrl+B, Ctrl+K: the text follows the preset
    b.setInputs("split", {QString(), QString()}); // no key at all: the action's name
    const QString none = w.tours()->stepText(step);
    CHECK(none.contains("<b>") && !none.contains("{key") && !none.contains("<b></b>"));
    // The real tour text too (translated, the placeholder kept by the dictionaries)
    b.applyPreset("standard");
    for (const Tour& t : Tours::builtIn())
        for (const TourStep& s : t.steps) CHECK(!w.tours()->stepText(s).contains("{key"));
}

// The menu and the questions (the default run only)
void testMenuAndQuestions(MainWindow& w)
{
    TourManager* tours = w.tours();
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
    CHECK(bubbleProblems(o).isEmpty());
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
    // "Show me" starts it; Esc ends it
    TourState::resetAll();
    tours->offerAtStart();
    if (auto* show = button(o, "tour_show"); CHECK(show)) show->click();
    CHECK(tours->runner()->running());
    QKeyEvent esc2(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&w, &esc2);
    CHECK(!tours->runner()->running());
    CHECK(!o->isVisible());
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
}

QAction* bound(const char* id)
{
    for (const auto& e : InputBindings::instance().entries())
        if (e.id == QLatin1String(id)) return e.action;
    return nullptr;
}

// "Never lose work": the user's undo and redo steps survive a tour (the practice project has a history of its own,
// Ctrl+Z there never reaches the user's steps); an AI on the live connection waits while the tour runs
void testHistoryAndLive(MainWindow& w)
{
    Project* p = w.liveProject();
    TourManager* tours = w.tours();
    QAction* undo = bound("undo");
    QAction* redo = bound("redo");
    if (!CHECK(undo && redo)) return;
    Cli::Live::Server server(&w); // not listening: the app's own chat path (runForChat) asks the same waiting
    server.setAllowed(true);
    w.setLiveWaiting([&server](const QString& m) { server.setWaiting(m); });
    const int clips0 = clipCount(p);
    const int base = p->undoStack()->index();
    for (int f : {300, 600, 900}) w.editor()->addTitle(f, 0, 30, TitleKind::Text); // three edits
    CHECK_EQ(clipCount(p), clips0 + 3);
    undo->trigger(); // one of them undone: a redo step waits too
    CHECK_EQ(clipCount(p), clips0 + 2);
    const int count = p->undoStack()->count();
    const bool modified = p->isModified();

    tours->start("editing");
    if (QPushButton* keep = button(tours->overlay(), "tour_keep")) keep->click();
    if (!CHECK(tours->runner()->running())) return;
    QTest::qWait(20);
    // In the practice project: its own history, empty at first; Ctrl+Z there leaves the practice project alone
    const int practiceClips = clipCount(p);
    CHECK(!p->undoStack()->canUndo());
    CHECK(!undo->isEnabled());
    undo->trigger();
    CHECK_EQ(clipCount(p), practiceClips);
    w.editor()->addTitle(0, 1, 30, TitleKind::Text); // an edit in the practice project, undone and done again
    CHECK(undo->isEnabled());
    undo->trigger();
    CHECK_EQ(clipCount(p), practiceClips);
    redo->trigger();
    CHECK_EQ(clipCount(p), practiceClips + 1);
    // An AI meanwhile: told to wait, nothing lands in the practice project
    const QJsonObject r = server.runForChat("info", {});
    CHECK_EQ(r.value("error").toObject().value("code").toString(), QString("APP_WAITING"));
    CHECK(r.value("error").toObject().value("message").toString().contains("tour"));
    tours->stop();

    // The user's project with its whole history: three steps back to before, three forward again
    CHECK_EQ(clipCount(p), clips0 + 2);
    CHECK_EQ(p->undoStack()->index(), base + 2);
    CHECK_EQ(p->undoStack()->count(), count);
    CHECK_EQ(p->isModified(), modified);
    CHECK(undo->isEnabled() && redo->isEnabled());
    redo->trigger();
    CHECK_EQ(clipCount(p), clips0 + 3);
    for (int i = 0; i < 3; ++i) undo->trigger();
    CHECK_EQ(clipCount(p), clips0);
    CHECK_EQ(p->undoStack()->index(), base);
    for (int i = 0; i < 3; ++i) redo->trigger(); // Ctrl+Shift+Z
    CHECK_EQ(clipCount(p), clips0 + 3);
    for (int i = 0; i < 3; ++i) undo->trigger(); // as the other checks expect it
    CHECK(server.waiting().isEmpty()); // AIs may work again
    w.setLiveWaiting({});
}

// The user's own project: a clip of its own (a title: no media at all), unsaved, selected, the playhead moved
void ownProject(MainWindow& w)
{
    w.editor()->addTitle(0, 0, 0, TitleKind::Text);
    const Timeline& tl = w.liveProject()->timeline();
    if (CHECK(!tl.video.isEmpty() && !tl.video[0].clips.isEmpty())) w.liveSelection()->set({tl.video[0].clips[0].id});
    CHECK(w.liveProject()->isModified());
    CHECK(w.liveProject()->media().isEmpty());
}

void runWindow(Engine& engine, const QString& run, const QString& settingsFile, const QString& lang, const QSize& size,
               bool dumpIt)
{
    QSettings().clear();
    if (!settingsFile.isEmpty()) {
        QSettings from(settingsFile, QSettings::IniFormat);
        CHECK(!from.allKeys().isEmpty());
        QSettings to;
        for (const QString& k : from.allKeys()) to.setValue(k, from.value(k));
    }
    // Only this run's language (the translators of the runs before stay installed otherwise: German would show the
    // last one)
    for (QTranslator* t : qApp->findChildren<QTranslator*>()) {
        QCoreApplication::removeTranslator(t);
        delete t;
    }
    I18n::install(lang);
    const bool user = !settingsFile.isEmpty();
    MainWindow w(&engine);
    if (!user) w.disableAutosave(); // the user run keeps it: the panels' settings must stay untouched by the tour itself
    w.resize(size);
    w.show();
    w.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&w));
    QTest::qWait(50);
    TourManager* tours = w.tours();
    if (!CHECK(tours)) return;
    TourOverlay* o = tours->overlay();
    if (!user) testPanelToggles(w);
    if (!user) testColorTourButton(w);
    if (!user) testKeyPlaceholder(w);
    if (!user) testMenuAndQuestions(w);
    ownProject(w);
    if (user) { // the user's panels as in the settings file
        if (auto* pool = w.findChild<QToolButton*>("panelToggle_mediaPool")) CHECK(!pool->isChecked());
        if (auto* fx = w.findChild<QToolButton*>("panelToggle_effects")) CHECK(fx->isChecked());
        // Unsaved changes: asked first; Cancel = no tour, nothing changed
        const Look before = look(w);
        tours->start("first_steps");
        CHECK(!tours->runner()->running());
        if (auto* cancel = button(o, "tour_cancel"); CHECK(cancel)) cancel->click();
        CHECK(!o->isVisible());
        CHECK(!tours->runner()->running());
        sameLook(before, look(w), run + " cancel");
    }
    for (const Tour& t : Tours::builtIn()) {
        const Look before = look(w);
        walk(w, t, run, dumpIt);
        QTest::qWait(30);
        sameLook(before, look(w), run + " after " + t.id);
    }
    if (!user) testPanelToggles(w); // after the tours (they put every splitter's sizes back) too
    testHistoryAndLive(w);
    // Back in the user's project after the tours: the same clip, still unsaved, still selected
    CHECK(w.liveProject()->isModified());
    CHECK(w.liveProject()->media().isEmpty());
    CHECK_EQ(clipCount(w.liveProject()), 1);
    // A tour started while one runs: the first ends (its project back), then the next one begins
    tours->start("editing");
    if (QPushButton* keep = button(o, "tour_keep")) keep->click();
    tours->start("color");
    if (QPushButton* keep = button(o, "tour_keep")) keep->click();
    CHECK(tours->runner()->running());
    CHECK_EQ(tours->runner()->tour().id, QString("color"));
    tours->stop();
    CHECK(w.liveProject()->media().isEmpty());
    CHECK_EQ(clipCount(w.liveProject()), 1);
    if (!user) {
        // The empty graph's button starts the Compositing tour (in the user's project: a video clip without a graph)
        w.importFiles({QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/tour-sample/clip1.mp4"}, true, true);
        int video = 0;
        for (const Track& tr : w.liveProject()->timeline().video)
            for (const Clip& c : tr.clips)
                if (c.kind == ClipKind::Media) video = c.id;
        if (CHECK(video)) {
            w.openComp(video);
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
                CHECK(!start->isVisible()); // not in the spotlight while it runs
                tours->stop();
                QTest::qWait(30);
                CHECK(start->isVisible()); // the user's clip still has no graph
            }
        }
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
    testBubbleSizes();

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    const QString user = Check::testData() + "/tours/user-settings.conf";
    // Default settings and the user's (Media Pool off, Effects on, own sizes) on the user's screen size, English and
    // Russian (long words)
    // German with fresh settings on the screen of a real walkthrough (the bubble texts as written)
    runWindow(engine, "default-de", {}, "de", QSize(1600, 960), true);
    runWindow(engine, "default-en", {}, "en", QSize(1600, 950), false);
    runWindow(engine, "user-en", user, "en", QSize(1920, 1050), true);
    runWindow(engine, "user-ru", user, "ru", QSize(1920, 1050), true);
    // A small laptop screen (the Color page scrolls its controls: the window never grows past it)
    runWindow(engine, "default-ru", {}, "ru", QSize(1366, 768), false);
    return Check::result();
}

#include "test_tours.moc"
