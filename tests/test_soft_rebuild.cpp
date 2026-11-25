// SPDX-License-Identifier: AGPL-3.0-only
// Another design or language while schneidi runs (MainWindow::rebuildUi): switched twice (German/Dark Orange ->
// English/Hell -> back), every time the window's texts and colors are the new ones while the project stays the same
// object with its undo history, clips and selection, and the playhead, the timeline's zoom/scroll, the page, a panel
// toggle, splitter sizes and the running render queue's object survive. No widget, action or connection is left
// over (the window's direct children stay the same in number; an engine signal reaches the new widgets once).
// The restart for a new interface size: SessionResume writes the project with its state, a new window takes it back
// (resumeSession) - unsaved changes, file name, page, selection, playhead, zoom - and the files are gone after it.
// SOFT_REBUILD_DUMP=<folder>: pictures of the window before and after each switch.

#include "check.h"

#include "app/InputBindings.h"
#include "app/LaunchArgs.h"
#include "app/MainWindow.h"
#include "app/SessionResume.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/NestedLoop.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/RenderQueue.h"
#include "engine/picture/CompNodes.h"
#include "ui/BackgroundJobs.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"
#include "ui/phone/PhoneShell.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>
#include <QSettings>
#include <QSplitter>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <clocale>

namespace {

using Page = MainWindow::Page;

void dump(QWidget* w, const QString& name)
{
    if (const QString dir = qEnvironmentVariable("SOFT_REBUILD_DUMP"); !dir.isEmpty()) w->grab().save(dir + "/" + name + ".png");
}

TimelineView* timeline(MainWindow& w)
{
    auto* panel = w.findChild<TimelinePanel*>();
    return panel ? panel->view() : nullptr;
}

// The viewer's time field(s): what it shows of the playhead
QStringList viewerTimes(MainWindow& w)
{
    QStringList out;
    if (auto* viewer = w.findChild<Viewer*>())
        for (const QLabel* l : viewer->findChildren<QLabel*>())
            if (l->text().startsWith("00:")) out << l->text();
    return out;
}

QString firstMenu(MainWindow& w)
{
    const QList<QAction*> menus = w.menuBar()->actions();
    return menus.isEmpty() ? QString() : menus.first()->text();
}

QString pageButtonText(MainWindow& w, const char* id)
{
    auto* b = w.findChild<QToolButton*>(QString("pageButton_") + id);
    return b ? b->text() + "|" + b->toolTip() : QString();
}

void chooseLook(const QString& lang, const QString& design)
{
    I18n::setLanguage(lang);
    Theme::save(design, Theme::defaults(design));
}

struct Seen {
    QString menu, pageButton, updateBarStyle;
    QColor window;
};

Seen look(MainWindow& w)
{
    Seen s;
    s.menu = firstMenu(w);
    s.pageButton = pageButtonText(w, "media");
    if (auto* bar = w.findChild<QWidget*>("update_bar")) s.updateBarStyle = bar->styleSheet();
    s.window = qApp->palette().color(QPalette::Window);
    return s;
}

void testSwitch(Engine& engine)
{
    chooseLook("de", "dark-orange");
    // A language or design given for this run only (--lang, --design) is no change of the setting: nothing to apply
    I18n::install("en");
    CHECK(!I18n::languageChanged());
    Theme::load("hell");
    CHECK(!Theme::changed());
    CHECK(I18n::switchTo("de"));
    Theme::load();
    Theme::apply(*qApp);
    I18n::setLanguage("fr");
    CHECK(I18n::languageChanged());
    I18n::setLanguage("de");
    CHECK(!I18n::languageChanged());

    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    QTest::qWait(50);

    Project* project = w.liveProject();
    Editor* editor = w.editor();
    // Something to keep: three titles (three undo steps), the second selected
    editor->addTitle(0, -1, 0, TitleKind::Text);
    editor->addTitle(200, -1, 0, TitleKind::Text);
    editor->addTitle(400, -1, 0, TitleKind::Text);
    const int steps = project->undoStack()->index();
    CHECK(steps >= 3);
    QVector<int> ids;
    for (const Track& t : project->timeline().video)
        for (const Clip& c : t.clips) ids << c.id;
    CHECK_EQ(ids.size(), 3);
    w.liveSelection()->set({ids.value(1)});
    const QString clipsBefore = Check::dump(project->timeline());

    // The window as the user arranged it: the Media page, the Effects panel on, a splitter dragged, zoom and playhead
    w.showPage(Page::Media);
    auto* effects = w.findChild<QToolButton*>("panelToggle_effects");
    CHECK(effects);
    if (effects) effects->setChecked(!effects->isChecked());
    const bool effectsOn = effects && effects->isChecked();
    auto* mediaPage = w.findChild<QSplitter*>("mediaPage");
    CHECK(mediaPage);
    if (mediaPage) mediaPage->setSizes({300, 560});
    QTest::qWait(20);
    const QList<int> mediaSizes = mediaPage ? mediaPage->sizes() : QList<int>();
    TimelineView* tv = timeline(w);
    CHECK(tv);
    tv->setZoomAndScroll(2.5, 37, 0);
    engine.seek(123);
    QTest::qWait(300);
    CHECK_EQ(tv->playhead(), 123);
    const QStringList times = viewerTimes(w);
    CHECK(!times.isEmpty() && !times.contains("00:00:00:00"));
    const ViewState view = tv->view();
    RenderQueue* queue = w.findChild<DeliverPanel*>()->renderQueue();
    int directChildren = int(w.children().size());
    const int actions = int(w.findChildren<QAction*>().size());

    const Seen german = look(w);
    CHECK_EQ(german.menu, QString("&Datei"));
    CHECK_EQ(german.window, QColor(Theme::defaults("dark-orange").value("window")));
    dump(&w, "1-de-dark-orange");

    for (int round = 0; round < 2; ++round) {
        const bool english = round == 0;
        chooseLook(english ? "en" : "de", english ? "hell" : "dark-orange");
        const int generation = w.uiGeneration();
        CHECK(w.rebuildUi(false));
        QTest::qWait(50); // the queued view restore and the curtain's fade
        CHECK_EQ(w.uiGeneration(), generation + 1);
        CHECK_EQ(I18n::installedLanguage(), QString(english ? "en" : "de"));

        // Texts and colors: the new ones everywhere
        const Seen now = look(w);
        CHECK_EQ(now.menu, QString(english ? "&File" : "&Datei"));
        CHECK(english ? now.pageButton != german.pageButton : now.pageButton == german.pageButton);
        CHECK_EQ(now.window, QColor(Theme::defaults(english ? "hell" : "dark-orange").value("window")));
        CHECK_EQ(Theme::window, now.window);
        CHECK(english ? now.updateBarStyle != german.updateBarStyle : now.updateBarStyle == german.updateBarStyle);
        CHECK(qApp->styleSheet().contains(Theme::window.name()));
        // Tables of translated names made once (effects, compositing nodes): in the new language too
        if (const EffectDescriptor* fx = EffectRegistry::find("color"); CHECK(fx))
            CHECK_EQ(fx->name, QString(english ? "Color Corrector" : "Farbkorrektur"));
        if (const Picture::Comp::NodeType* node = Picture::Comp::find("merge"); CHECK(node))
            CHECK_EQ(node->name, T("Mischen"));
        CHECK(english == (T("Mischen") != QString("Mischen")));
        dump(&w, english ? "2-en-hell" : "3-de-dark-orange");

        // The model: the same objects, the same history and clips, the same selection
        CHECK(w.liveProject() == project);
        CHECK(w.editor() == editor);
        CHECK_EQ(project->undoStack()->index(), steps);
        CHECK_EQ(Check::dump(project->timeline()), clipsBefore);
        CHECK_EQ(w.liveSelection()->ids(), QSet<int>({ids.value(1)}));
        // The menu's Undo follows the kept history at once (it was off until the next edit)
        if (QAction* undo = InputBindings::instance().action("undo"); CHECK(undo)) CHECK(undo->isEnabled());

        // The window: page, panel, splitter, zoom/scroll exactly, playhead, the render queue still the same object
        CHECK(w.currentPage() == Page::Media);
        auto* effectsNow = w.findChild<QToolButton*>("panelToggle_effects");
        CHECK(effectsNow && effectsNow != effects);
        CHECK(effectsNow && effectsNow->isChecked() == effectsOn);
        auto* mediaNow = w.findChild<QSplitter*>("mediaPage");
        CHECK(mediaNow && mediaNow->sizes() == mediaSizes);
        TimelineView* tvNow = timeline(w);
        CHECK(tvNow && tvNow != tv);
        if (tvNow) {
            CHECK_EQ(tvNow->view().pxPerFrame, view.pxPerFrame);
            CHECK_EQ(tvNow->view().leftFrame, view.leftFrame);
            CHECK_EQ(tvNow->view().scrollY, view.scrollY);
            CHECK_EQ(tvNow->playhead(), 123);
        }
        CHECK_EQ(viewerTimes(w), times); // the viewer's time too, not 0 until the playhead moves
        CHECK(w.findChild<DeliverPanel*>()->renderQueue() == queue);
        // Nothing left over or made twice (the first switch may have fewer: the Edit page's overlays, made when that
        // page first shows, come only when it shows again)
        const int children = int(w.children().size());
        CHECK(children <= directChildren);
        if (round == 0) directChildren = children;
        else CHECK_EQ(children, directChildren);
        CHECK_EQ(int(w.findChildren<QAction*>().size()), actions);
        effects = effectsNow;
        tv = tvNow;
    }

    // The new widgets work on the same history: undo removes the last title, redo brings it back
    project->undoStack()->undo();
    CHECK_EQ(project->undoStack()->index(), steps - 1);
    project->undoStack()->redo();
    CHECK_EQ(Check::dump(project->timeline()), clipsBefore);
    // The engine's playhead reaches the new timeline view (and only it: the old connections are gone)
    engine.seek(42);
    QTest::qWait(300);
    CHECK_EQ(tv->playhead(), 42);

    // Every page built once (Color, Comp, Studio, Regie are made on their first showing), back on Edit: all of them
    // come again, the page stays, and two switches leave the window with the same children
    for (Page p : {Page::Color, Page::Comp, Page::Studio, Page::Direct, Page::Edit}) w.showPage(p);
    QTest::qWait(20);
    // ... with the In/Out loop on and the grades bypassed: both stay so, their menu entries say it
    project->edit("marks", [](Timeline& tl) {
        tl.markIn = 10;
        tl.markOut = 60;
    });
    if (QAction* loop = InputBindings::instance().action("loop_inout"); CHECK(loop)) loop->setChecked(true);
    CHECK(engine.looping());
    engine.setColorBypass(true);
    // ... jobs in the corner (one running, one just done) and the key trainer open: all of it again afterwards
    if (auto* jobs = w.findChild<BackgroundJobs*>(); CHECK(jobs)) {
        jobs->setHoldMs(60000);
        BackgroundJobs::Job job;
        job.text = "läuft";
        job.fraction = 0.5;
        jobs->setJob("test_running", job);
        jobs->finishJob("test_done", "fertig");
    }
    if (QAction* trainer = InputBindings::instance().action("key_trainer"); CHECK(trainer)) trainer->trigger();
    QTest::qWait(20);
    CHECK(w.findChild<QWidget*>("key_trainer") && w.findChild<QWidget*>("key_trainer")->isVisible());
    int children = 0;
    for (const char* design : {"midnight", "dark-orange"}) {
        chooseLook(children ? "de" : "en", design);
        CHECK(w.rebuildUi(false));
        QTest::qWait(50);
        CHECK(w.currentPage() == Page::Edit);
        CHECK(w.compPage() && w.studioPage() && w.directPage());
        CHECK_EQ(Check::dump(project->timeline()), clipsBefore);
        if (auto* jobs = w.findChild<BackgroundJobs*>(); CHECK(jobs)) {
            CHECK(jobs->hasJob("test_running"));
            CHECK(jobs->hasJob("test_done"));
        }
        CHECK(w.findChild<QWidget*>("key_trainer") && w.findChild<QWidget*>("key_trainer")->isVisible());
        CHECK(engine.looping());
        CHECK(InputBindings::instance().action("loop_inout")->isChecked());
        CHECK(engine.colorBypass());
        CHECK(InputBindings::instance().action("color_bypass")->isChecked());
        if (children) CHECK_EQ(int(w.children().size()), children);
        children = int(w.children().size());
    }
    dump(&w, "4-edit-all-pages");
    engine.setColorBypass(false);

    // Never from a nested loop: a context menu's exec (inside the main loop), a progress loop or a drag (Guard). A
    // change asked for meanwhile comes once it is over.
    {
        bool rebuilt = true;
        QEventLoop main; // (the test has no app.exec: this stands for the main loop)
        QTimer::singleShot(0, &main, [&] {
            QMenu menu(&w);
            menu.addAction("x");
            QTimer::singleShot(50, &menu, [&] {
                rebuilt = w.rebuildUi(false);
                menu.close();
            });
            menu.exec(QPoint(20, 20));
            main.quit();
        });
        main.exec();
        CHECK(!rebuilt);
    }
    {
        const int before = w.uiGeneration();
        {
            const NestedLoop::Guard progressLoop;
            CHECK(!w.rebuildUi(false));
            chooseLook("de", "graphit");
            w.applyLookChange();
            QTest::qWait(150);
            CHECK_EQ(w.uiGeneration(), before); // waits
        }
        QTest::qWait(800);
        CHECK_EQ(w.uiGeneration(), before + 1); // then comes
        CHECK_EQ(Theme::window, QColor(Theme::defaults("graphit").value("window")));
    }

    // Neither changed: the queued apply does nothing
    const int generation = w.uiGeneration();
    w.applyLookChange();
    QTest::qWait(50);
    CHECK_EQ(w.uiGeneration(), generation);
}

void testPhone(Engine& engine)
{
    // The phone form (setLayoutForm, as the size rule does on Android): a language and design switch keeps the form - a
    // shell of the new widgets - and the state; the desktop form afterwards is in the new language too
    qputenv("SCHNEIDI_LAYOUT", "phone"); // (check.h pins the desktop form)
    chooseLook("de", "dark-orange");
    I18n::switchTo("de");
    Theme::switchToSaved(*qApp);
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(411, 760);
    w.show();
    QTest::qWait(50);
    w.setLayoutForm(MainWindow::LayoutForm::Phone);
    QTest::qWait(50);
    CHECK(w.layoutForm() == MainWindow::LayoutForm::Phone);
    Project* project = w.liveProject();
    w.editor()->addTitle(0, -1, 0, TitleKind::Text);
    w.editor()->addTitle(150, -1, 0, TitleKind::Text);
    const int steps = project->undoStack()->index();
    const QString clips = Check::dump(project->timeline());
    QSet<int> selected;
    for (const Clip& c : project->timeline().video.value(0).clips) selected = {c.id};
    w.liveSelection()->set(selected);
    engine.seek(60);
    QTest::qWait(300);
    TimelineView* tv = timeline(w);
    CHECK(tv && tv->playhead() == 60);
    const double zoom = tv ? tv->view().pxPerFrame : 0;
    PhoneShell* shell = w.phoneShell();
    CHECK(shell);
    dump(&w, "5-phone-de");

    chooseLook("en", "hell");
    CHECK(w.rebuildUi(false));
    QTest::qWait(400);
    CHECK(w.layoutForm() == MainWindow::LayoutForm::Phone);
    CHECK(w.phoneShell() && w.phoneShell() != shell);
    CHECK_EQ(I18n::installedLanguage(), QString("en"));
    CHECK_EQ(Theme::window, QColor(Theme::defaults("hell").value("window")));
    CHECK_EQ(project->undoStack()->index(), steps);
    CHECK_EQ(Check::dump(project->timeline()), clips);
    CHECK_EQ(w.liveSelection()->ids(), selected);
    tv = timeline(w);
    if (CHECK(tv)) {
        CHECK_EQ(tv->playhead(), 60);
        CHECK_EQ(tv->view().pxPerFrame, zoom);
    }
    if (auto* add = w.addMediaButton(); CHECK(add)) CHECK_EQ(add->text(), QString("Add videos")); // the shell's own, new
    dump(&w, "6-phone-en");

    // The desktop form again: its menus in the new language
    qputenv("SCHNEIDI_LAYOUT", "desktop");
    w.resize(1600, 950);
    w.setLayoutForm(MainWindow::LayoutForm::Desktop);
    QTest::qWait(50);
    CHECK(w.layoutForm() == MainWindow::LayoutForm::Desktop);
    CHECK_EQ(firstMenu(w), QString("&File"));
    CHECK_EQ(Check::dump(project->timeline()), clips);
    dump(&w, "7-desktop-en");

    chooseLook("de", "dark-orange");
    CHECK(w.rebuildUi(false));
}

void testResume(Engine& engine)
{
    // A restart's resume file, written from one window and read by a new one (as main.cpp does with --resume)
    QString stateFile;
    QSet<int> selected;
    QString timelineBefore;
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.editor()->addTitle(0, -1, 0, TitleKind::Text);
        w.editor()->addTitle(100, -1, 0, TitleKind::Text);
        for (const Clip& c : w.liveProject()->timeline().video.value(0).clips) selected = {c.id};
        timelineBefore = Check::dump(w.liveProject()->timeline());
        SessionResume::State state;
        state.projectPath = QDir(SessionResume::folder()).filePath("../elsewhere/film.schneidi");
        state.modified = true;
        state.page = MainWindow::pageId(Page::Deliver);
        state.selection = selected;
        ProjectData data = w.liveProject()->data();
        data.playhead = 77;
        data.views = {SavedView{w.liveProject()->currentSequence(), 3.0, 12.0, 0, 77}};
        stateFile = SessionResume::write(state, data);
        CHECK(!stateFile.isEmpty());
        CHECK(QFileInfo::exists(stateFile));
    }
    // Files of its own per restart (two schneidi at once), listed as left over until a window took them
    CHECK(QFileInfo(stateFile).fileName().startsWith("resume-"));
    CHECK(SessionResume::leftovers(0).contains(stateFile));
    CHECK(!SessionResume::leftovers(3600).contains(stateFile)); // just written: no leftover yet
    // `--resume` comes from the command line: a file outside the resume folder is neither read nor removed
    {
        QTemporaryDir dir;
        const QString decoy = dir.filePath("resume-1-1.json");
        QFile f(decoy);
        CHECK(f.open(QIODevice::WriteOnly) && f.write("{\"page\":\"edit\"}") > 0);
        f.close();
        MainWindow other(&engine);
        other.disableAutosave();
        CHECK(!other.resumeSession(decoy));
        SessionResume::remove(decoy);
        CHECK(QFileInfo::exists(decoy));
    }
    // The restart's arguments carry it, never as a file to open
    const QStringList args = LaunchArgs::restart("/p/film.schneidi", stateFile);
    CHECK_EQ(LaunchArgs::resumeFile(args), stateFile);
    CHECK(LaunchArgs::files(args).isEmpty());

    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    CHECK(w.resumeSession(stateFile));
    QTest::qWait(50);
    CHECK_EQ(Check::dump(w.liveProject()->timeline()), timelineBefore);
    CHECK(w.liveProject()->isModified());
    CHECK(w.liveProjectPath().endsWith("film.schneidi"));
    CHECK(w.currentPage() == Page::Deliver);
    CHECK_EQ(w.liveSelection()->ids(), selected);
    if (TimelineView* tv = timeline(w)) {
        CHECK_EQ(tv->playhead(), 77);
        CHECK_EQ(tv->view().pxPerFrame, 3.0);
        CHECK_EQ(tv->view().leftFrame, 12.0);
    }
    CHECK(!QFileInfo::exists(stateFile)); // read once, then gone
    CHECK(!SessionResume::leftovers(0).contains(stateFile));
    CHECK(!w.resumeSession(stateFile));    // a second time: nothing (and nothing changed)
    CHECK_EQ(Check::dump(w.liveProject()->timeline()), timelineBefore);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SCHNEIDI_REDUCE_MOTION", "1"); // the still goes at once (no fade to wait for)
    QApplication app(argc, argv);
    Check::initApp("soft-rebuild");
    std::setlocale(LC_NUMERIC, "C");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    QSettings().remove("panels");
    QSettings().remove("layout");
    testSwitch(engine);
    testPhone(engine);
    testResume(engine);
    return Check::result();
}
