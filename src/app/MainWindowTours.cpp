// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the in-app tours (app/TourManager.h): names for their targets, the pages by name, the sample clips.
#include "app/MainWindow.h"

#include "app/TourManager.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "core/I18n.h"
#include "core/Selection.h"
#include "core/StartTemplates.h"
#include "ui/comp/CompCanvas.h"
#include "ui/comp/CompPage.h"
#include "ui/comp/CompShared.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/ScopesPanel.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSplitter>
#include <QStatusBar>
#include <QPointer>
#include <algorithm>
#include <utility>
#include <QToolButton>

namespace {
const QPair<MainWindow::Page, const char*> kPageNames[] = {{MainWindow::Page::Media, "media"},
                                                           {MainWindow::Page::Edit, "edit"},
                                                           {MainWindow::Page::Comp, "comp"},
                                                           {MainWindow::Page::Color, "color"},
                                                           {MainWindow::Page::Deliver, "deliver"}};
} // namespace

QString MainWindow::pageName(Page page)
{
    for (const auto& [p, name] : kPageNames)
        if (p == page) return QString::fromLatin1(name);
    return {};
}

void MainWindow::setupTours(QMenu* help)
{
    // Targets of the tours: panels whose objectName is shared for the styles ("Panel") get a name of their own
    m_mediaPool->setProperty("tourTarget", "mediaPool");
    m_viewer->setProperty("tourTarget", "viewer");
    m_timeline->setProperty("tourTarget", "timeline");
    m_inspector->setProperty("tourTarget", "inspector");
    m_effectsToggle->setProperty("tourTarget", "effectsToggle");
    m_scopes->setProperty("tourTarget", "scopes");
    for (QAbstractButton* b : m_pageButtons->buttons()) // page buttons: the same names the page bar uses elsewhere
        if (b->objectName().isEmpty()) b->setObjectName("pageButton_" + pageName(Page(m_pageButtons->id(b))));

    TourManager::Host host;
    host.page = [this] { return pageName(m_page); };
    host.showPage = [this](const QString& name) {
        for (const auto& [p, n] : kPageNames)
            if (name == QLatin1String(n)) showPage(p);
    };
    host.playhead = [this] { return m_timeline->view()->playhead(); };
    // Buttons a step offers: the sample clips in an empty project, an example graph on the Comp page
    host.offers["clips"] = {N_("Beispielclips laden"), [this] { return m_project->media().isEmpty(); },
                            [this] { loadTourSample(); }};
    host.offers["comp_example"] = {N_("Beispiel anlegen"), [this] { return !compHasMerge(); },
                                   [this] { tourCompExample("pip_frame"); }};
    // The guides' "do it for me": the recipe the step asks to drag onto the clip
    host.offers["recipe_pip"] = {N_("Für mich erledigen"), [this] { return !timelineHas("merge"); },
                                 [this] { tourCompExample("pip_frame"); }};
    host.offers["recipe_greenscreen"] = {N_("Für mich erledigen"), [this] { return !timelineHas("key"); },
                                         [this] { tourCompExample("greenscreen"); }};
    host.conditions["comp_has_merge"] = [this] { return compHasMerge(); };
    // The Comp page's node view (the graph the tour explains)
    host.actions["comp_nodes"] = [this] {
        ensureCompPage();
        if (auto* b = m_compPage->findChild<QToolButton*>("comp_view_nodes")) b->setChecked(true);
    };
    host.conditions["timeline_has_merge"] = [this] { return timelineHas("merge"); };
    host.conditions["timeline_has_key"] = [this] { return timelineHas("key"); };
    // Tour mode: a practice project and a fixed layout; the user's project and layout come back at the end
    host.enter = [this](const Tour& t) { enterTourMode(t); };
    host.leave = [this] { leaveTourMode(); };
    host.unsaved = [this] { return m_project->isModified() && !m_autosaveDisabled; }; // test runs never ask
    host.save = [this] { return save(); };
    // The timeline's ruler (click there = playhead) and the playhead line, in window coordinates
    using Area = TourManager::Host::Area;
    host.areas["ruler"] = [this]() -> Area {
        TimelineView* tv = m_timeline->view();
        return {tv, QRect(TimelineView::kHeaderW, 0, tv->width() - TimelineView::kHeaderW, TimelineView::kRulerH)};
    };
    host.areas["playhead"] = [this]() -> Area {
        TimelineView* tv = m_timeline->view();
        const int x = tv->playheadX();
        if (x < TimelineView::kHeaderW || x >= tv->width()) return {};
        return {tv, QRect(x - 5, 0, 11, tv->height())};
    };
    // Parts of the Comp page's graph (CompCanvas::tourArea): the output node, the ports
    for (const QString& part : {QStringLiteral("output"), QStringLiteral("node:merge"), QStringLiteral("node:transform"), QStringLiteral("port:src"),
                                QStringLiteral("port:bg"), QStringLiteral("port:fg"), QStringLiteral("port:mask")})
        host.areas["comp_" + QString(part).replace(':', '_')] = [this, part]() -> Area {
            if (!m_compPage) return {};
            CompCanvas* c = m_compPage->canvas();
            return {c, c->tourArea(part).intersected(c->rect())};
        };
    m_tours = new TourManager(this, m_project, host);
    m_tours->addMenu(help);
    // The empty graph's "Start the Compositing tour" stays away while a tour runs (it would sit in the spotlight)
    connect(m_tours, &TourManager::runningChanged, this, [this](bool on) {
        if (m_compPage) m_compPage->canvas()->setTourButtonAllowed(!on);
    });
    // Queued: the timeline takes the new playhead in a slot connected later
    connect(m_engine, &Engine::positionChanged, m_tours, &TourManager::poke, Qt::QueuedConnection);
}

void MainWindow::offerTours()
{
    // An AI driving the app right now (live connection): no question in its way; asked at a later start
    if (m_tours && !m_liveConnected) m_tours->offerAtStart();
}

bool MainWindow::timelineHas(const QString& nodeType) const
{
    for (const Track& t : m_project->timeline().video)
        for (const Clip& c : t.clips) {
            CompGraph g;
            if (CompUi::graphOf(c, &g)
                && std::any_of(g.nodes.cbegin(), g.nodes.cend(), [&](const CompNode& n) { return n.type == nodeType; }))
                return true;
        }
    return false;
}

bool MainWindow::compHasMerge() const
{
    if (!m_compPage || !m_compPage->clip()) return false;
    const Clip* c = TimelineOps::findClip(m_project->timeline(), m_compPage->clip());
    CompGraph g;
    if (!c || !CompUi::graphOf(*c, &g)) return false;
    return std::any_of(g.nodes.cbegin(), g.nodes.cend(), [](const CompNode& n) { return n.type == QLatin1String("merge"); });
}

void MainWindow::tourCompExample(const QString& recipe)
{
    // A video clip to show it on: the selected one / the one at the playhead, else the first on the timeline, else
    // a sample clip placed at the end
    int id = selectedVideoClip();
    if (!id) id = compClipAt(m_timeline->view()->playhead());
    if (!id)
        for (const Track& t : m_project->timeline().video)
            for (const Clip& c : t.clips)
                if (!id && c.kind == ClipKind::Media) id = c.id;
    if (!id) {
        if (m_project->media().isEmpty()) loadTourSample();
        QString video;
        for (const MediaInfo& m : m_project->media())
            if (video.isEmpty() && m.hasVideo) video = m.path;
        if (video.isEmpty()) return;
        m_editor->sourceEdit(Editor::SourceEditMode::AppendAtEnd, video, 0, m_timeline->view()->playhead());
        for (const Track& t : m_project->timeline().video)
            for (const Clip& c : t.clips)
                if (c.mediaPath == video) id = c.id;
        if (!id) return;
    }
    // A built-in recipe ("picture in picture": a merge with its background and foreground), one undo step
    QString err;
    if (!CompUi::applyRecipe(m_editor, {id}, recipe, {}, &err)) {
        statusBar()->showMessage(T("Geht nicht: %1").arg(err), 5000);
        return;
    }
    openComp(id);
    if (auto* b = m_compPage->findChild<QToolButton*>("comp_view_nodes")) b->setChecked(true);
}

QStringList MainWindow::tourSampleFiles()
{
    // Built in, nothing is downloaded: the short game clips of the built-in highlight template, copied to the app's data
    // folder (the engine reads files, not resources)
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/tour-sample";
    QDir().mkpath(dir);
    QStringList files;
    for (const QString& name : {QStringLiteral("clip1.mp4"), QStringLiteral("clip2.mp4"), QStringLiteral("clip3.mp4")}) {
        const QString target = dir + "/" + name;
        if (!QFile::exists(target)) {
            QFile::copy(":/templates/gaming-highlight/media/" + name, target);
            QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
        }
        if (QFile::exists(target)) files << target;
    }
    return files;
}

void MainWindow::loadTourSample()
{
    if (const QStringList files = tourSampleFiles(); !files.isEmpty())
        importFiles(files, false, m_project->media().isEmpty());
}

QVector<QToolButton*> MainWindow::tourToggles() const
{
    return {m_poolToggle, m_effectsToggle, m_storageToggle, m_inspectorToggle, m_mixerToggle, m_transcriptToggle, m_chatToggle};
}

void MainWindow::enterTourMode(const Tour& tour)
{
    if (m_tourSaved.active) leaveTourMode();
    // The user's project as it is (unsaved changes included) and how the window looks, kept aside
    TourSaved& s = m_tourSaved;
    s = {};
    s.active = true;
    m_engine->pause();
    s.data = m_project->data();
    s.data.playhead = m_timeline->view()->playhead();
    s.data.views = savedViews();
    s.path = m_projectPath;
    s.saveSuggestion = m_saveSuggestion;
    s.templateDir = m_templateDir;
    s.templateCopies = m_templateCopies;
    s.modified = m_project->isModified();
    s.selection = m_selection->ids();
    s.page = m_page;
    s.large = m_large.active;
    for (QToolButton* b : tourToggles()) s.toggles << b->isChecked();
    s.scopes = !m_scopes->isHidden();
    for (QSplitter* sp : findChildren<QSplitter*>()) s.sizes.push_back({sp, sp->sizes()});

    // The tour's layout: the default panels at their default sizes, every page reachable (the settings stay as they are)
    setViewerLarge(false);
    const bool defaults[] = {true, false, false, true, false, false, false}; // tourToggles() order
    const QVector<QToolButton*> toggles = tourToggles();
    for (int i = 0; i < toggles.size(); ++i) toggles[i]->setChecked(defaults[i]);
    m_scopes->setVisible(true);
    applySizes(m_colorBottom, {1100, 420});
    updatePageButtons();

    buildPracticeProject(tour.practice);
    showPage(Page::Edit); // the defaults of the Edit page; each step shows its own page
}

void MainWindow::buildPracticeProject(const QString& practice)
{
    // An empty project like a new one (Neutral, never the user's default template), then the sample clips
    StartTemplate neutral;
    for (const StartTemplate& t : StartTemplates::builtIn())
        if (t.id == QLatin1String(StartTemplates::NeutralId)) neutral = t;
    applyLoaded(StartTemplates::apply(neutral), {}, false);
    const QStringList files = tourSampleFiles();
    if (files.isEmpty()) return;
    const bool onTimeline = practice != QLatin1String("pool");
    importFiles(files, onTimeline, true); // the project takes the clips' format
    if (!onTimeline) {
        m_project->markSaved(); // a fresh practice project: nothing to save
        return;
    }
    if (practice == QLatin1String("stack") && files.size() > 1) m_editor->addMediaAt({files[1]}, 0, 1); // V2 over V1
    // Loaded once more: no undo steps of its own making, the playhead inside the first clip
    ProjectData d = m_project->data();
    d.playhead = m_project->timelineFormat().rate.timebase() / 2;
    applyLoaded(std::move(d), {}, false);
    // The clip the tour works on selected: the upper one of a stack, else the first one
    int clip = 0;
    for (const Track& t : m_project->timeline().video)
        if (!t.clips.isEmpty()) clip = t.clips.first().id;
    if (clip) m_selection->set({clip});
}

void MainWindow::leaveTourMode()
{
    if (!m_tourSaved.active) return;
    TourSaved s = std::exchange(m_tourSaved, {});
    m_engine->pause();
    // The user's project back as it was: unsaved changes still unsaved, the same file, playhead, zoom and selection
    applyLoaded(std::move(s.data), s.path, false);
    m_saveSuggestion = s.saveSuggestion;
    m_templateDir = s.templateDir;
    m_templateCopies = s.templateCopies;
    if (s.modified) m_project->markModified();
    updateTitle();
    m_selection->set(s.selection);
    // The panels and sizes as they were (still in tour mode while the toggles go back: their settings stay untouched)
    m_tourSaved.active = true;
    const QVector<QToolButton*> toggles = tourToggles();
    for (int i = 0; i < toggles.size() && i < s.toggles.size(); ++i) toggles[i]->setChecked(s.toggles[i]);
    m_scopes->setVisible(s.scopes);
    m_tourSaved.active = false;
    applySizes(m_colorBottom, {1100, 420});
    showPage(s.page);
    updatePageButtons();
    for (const auto& [sp, sizes] : s.sizes)
        if (sp && sp->count() == sizes.size()) sp->setSizes(sizes);
    if (s.large) setViewerLarge(true);
}
