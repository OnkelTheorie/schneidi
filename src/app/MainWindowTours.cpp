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
#include "ui/EffectsLibrary.h"
#include "ui/Inspector.h"
#include "ui/ColorPanel.h"
#include "core/GradeWindows.h"
#include "ui/ScopesPanel.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAbstractButton>
#include <QListWidget>
#include <QMenuBar>
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
constexpr double kTourPxPerSecond = 60; // the practice timeline's zoom: ~20 s in view on a laptop
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
    m_effects->setProperty("tourTarget", "effects");
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
    host.conditions["effects_open"] = [this] { return m_effectsToggle->isChecked() && m_effects->isVisible(); };
    // The viewer shows a picture: the playhead into the first video clip if it is not on one (the user dropped the
    // clip further right)
    host.actions["playhead_in_clip"] = [this] {
        const int at = m_timeline->view()->playhead();
        int first = 0;
        for (const Track& t : m_project->timeline().video)
            for (const Clip& c : t.clips) {
                if (at >= c.start && at < c.end()) return;
                if (!first && c.kind == ClipKind::Media) first = c.id;
            }
        if (const Clip* c = first ? TimelineOps::findClip(m_project->timeline(), first) : nullptr) {
            m_engine->seek(c->start + std::min(c->length() / 2, 15));
            m_timeline->view()->setPlayhead(c->start + std::min(c->length() / 2, 15));
        }
    };
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
    // The page buttons (not the whole bar: the hole of a bar as wide as the window was a thin line at its top)
    host.areas["page_buttons"] = [this]() -> Area {
        QRect r;
        QWidget* bar = nullptr;
        for (QAbstractButton* b : m_pageButtons->buttons())
            if (b->isVisible()) {
                bar = b->parentWidget();
                r = r.united(b->geometry());
            }
        return {bar, r};
    };
    // One clip of the practice project: its entry in the Media Pool, the first and second clip on V1 (and the right
    // edge of the first one: trimming), the clip on V2, a track's lane
    const auto mediaPath = [this](int i) { return i < m_project->media().size() ? m_project->media()[i].path : QString(); };
    host.areas["pool_clip"] = [this, mediaPath]() -> Area { return {m_mediaPool, m_mediaPool->itemArea(mediaPath(0))}; };
    const auto clipOn = [this](TrackRef track, int i) {
        const Timeline& tl = m_project->timeline();
        if (track.index >= tl.tracks(track.kind).size() || i >= tl.track(track).clips.size()) return 0;
        return tl.track(track).clips[i].id;
    };
    host.areas["clip_v1"] = [this, clipOn]() -> Area {
        return {m_timeline->view(), m_timeline->view()->clipArea(clipOn({TrackKind::Video, 0}, 0))};
    };
    host.areas["clip_v1_second"] = [this, clipOn]() -> Area {
        return {m_timeline->view(), m_timeline->view()->clipArea(clipOn({TrackKind::Video, 0}, 1))};
    };
    host.areas["clip_v2"] = [this, clipOn]() -> Area {
        return {m_timeline->view(), m_timeline->view()->clipArea(clipOn({TrackKind::Video, 1}, 0))};
    };
    host.areas["clip_v1_edge"] = [this, clipOn]() -> Area {
        const QRect c = m_timeline->view()->clipArea(clipOn({TrackKind::Video, 0}, 0));
        if (c.isEmpty()) return {};
        return {m_timeline->view(), QRect(c.right() - 14, c.top(), 28, c.height())};
    };
    // The first gap between two clips on V1 (a trim leaves one), else the second clip
    host.areas["gap_v1"] = [this, clipOn]() -> Area {
        TimelineView* tv = m_timeline->view();
        const Timeline& tl = m_project->timeline();
        if (tl.video.isEmpty()) return {};
        const QVector<Clip>& clips = tl.video[0].clips;
        for (int i = 1; i < clips.size(); ++i) {
            const QRect a = tv->clipArea(clips[i - 1].id), b = tv->clipArea(clips[i].id);
            if (clips[i].start > clips[i - 1].end() && a.isValid() && b.isValid() && b.left() - a.right() >= 6)
                return {tv, QRect(QPoint(a.right() + 2, a.top()), QPoint(b.left() - 2, a.bottom()))};
        }
        return {tv, tv->clipArea(clipOn({TrackKind::Video, 0}, 1))};
    };
    host.areas["track_v1"] = [this]() -> Area { return {m_timeline->view(), m_timeline->view()->trackArea({TrackKind::Video, 0})}; };
    host.areas["track_a1"] = [this]() -> Area { return {m_timeline->view(), m_timeline->view()->trackArea({TrackKind::Audio, 0})}; };
    // A recipe in the Effects Library (its "Compositing recipes" category shown by the action "effects_recipes")
    for (const QString& recipe : {QStringLiteral("greenscreen"), QStringLiteral("pip_frame")})
        host.areas["recipe_" + recipe] = [this, recipe]() -> Area {
            QListWidget* list = m_effects->list();
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole).toString() == CompUi::kRecipePrefix + recipe && !list->item(i)->isHidden()) {
                    const QRect r = list->visualItemRect(list->item(i)).intersected(list->viewport()->rect());
                    return {list->viewport(), r};
                }
            return {};
        };
    // The blur guide: a window on the clip (the step's "do it for me" adds a rectangle)
    const auto hasWindow = [this] {
        const Clip* c = TimelineOps::findClip(m_project->timeline(), selectedVideoClip());
        return c && !GradeWindows::list(*c).isEmpty();
    };
    host.conditions["color_has_window"] = hasWindow;
    host.offers["color_window"] = {N_("Für mich erledigen"), [hasWindow] { return !hasWindow(); },
                                   [this] { m_colorPanel->addWindow("rectangle"); }};
    host.actions["effects_recipes"] = [this] { m_effects->showRecipes(); };
    // The Comp page: a node selected (the Inspector shows its settings), the key's pipette there
    for (const QString& type : {QStringLiteral("key"), QStringLiteral("merge"), QStringLiteral("transform")})
        host.actions["comp_select_" + type] = [this, type] {
            ensureCompPage();
            if (auto* b = m_compPage->findChild<QToolButton*>("comp_view_nodes")) b->setChecked(true);
            CompGraph g;
            const Clip* c = m_compPage->clip() ? TimelineOps::findClip(m_project->timeline(), m_compPage->clip()) : nullptr;
            if (!c || !CompUi::graphOf(*c, &g)) return;
            for (const CompNode& n : g.nodes)
                if (n.type == type) {
                    m_compPage->selectNode(n.id);
                    return;
                }
        };
    host.areas["comp_pipette"] = [this]() -> Area {
        if (!m_compPage) return {};
        for (QToolButton* b : m_compPage->findChildren<QToolButton*>())
            if (b->objectName().startsWith("comp_pipette_") && b->isVisible()) return {b, b->rect()};
        return {};
    };
    // Where the bubble had better not sit: the bars with the controls the steps name (menus, panel tabs, page bar,
    // the timeline's tools, ruler and track headers, the viewer's transport) and the Comp graph's nodes
    host.avoid = [this] {
        QVector<QRect> out;
        const auto add = [&](QWidget* w, const QRect& local) {
            if (w && w->isVisible() && !local.isEmpty()) out << QRect(w->mapTo(this, local.topLeft()), local.size());
        };
        add(menuBar(), menuBar()->rect());
        for (const char* name : {"TopBar", "PageBar"}) add(findChild<QWidget*>(name), findChild<QWidget*>(name) ? findChild<QWidget*>(name)->rect() : QRect());
        TimelineView* tv = m_timeline->view();
        if (tv->isVisible()) {
            const QPoint top = tv->mapTo(m_timeline, QPoint(0, 0));
            add(m_timeline, QRect(0, 0, m_timeline->width(), top.y() + TimelineView::kRulerH)); // tools and ruler
            add(tv, QRect(0, 0, TimelineView::kHeaderW, tv->height()));                     // track headers
        }
        if (auto* screen = m_viewer->findChild<QWidget*>("viewer_screen"); screen && screen->isVisible()) {
            const int below = screen->mapTo(m_viewer, QPoint(0, screen->height())).y();
            add(m_viewer, QRect(0, below, m_viewer->width(), m_viewer->height() - below));
        }
        if (m_compPage && m_compPage->isVisible()) {
            if (auto* create = m_compPage->findChild<QWidget*>("comp_save_recipe"); create && create->parentWidget())
                add(create->parentWidget(), create->parentWidget()->rect()); // the Comp page's bar
            for (const QRect& n : m_compPage->canvas()->nodeRects()) add(m_compPage->canvas(), n);
        }
        return out;
    };
    m_tours = new TourManager(this, m_project, host);
    m_tours->addMenu(help);
    // The empty graph's "Start the Compositing tour" stays away while a tour runs (it would sit in the spotlight)
    // The page's own "Create Compositing" too: it builds an empty graph, not the tour's example
    connect(m_tours, &TourManager::runningChanged, this, [this](bool on) {
        if (m_compPage) m_compPage->setTourRunning(on);
        m_colorPanel->setTourButtonAllowed(!on);
    });
    connect(m_colorPanel, &ColorPanel::tourRequested, m_tours, [this] { m_tours->start("color"); });
    // A page or a panel the user opens may be what a step waits for
    connect(m_effectsToggle, &QToolButton::toggled, m_tours, &TourManager::poke, Qt::QueuedConnection);
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
    if (!m_autosaveDisabled) autosave(); // the backup up to date: a crash during the tour still restores the user's work
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
    s.window = size();

    // The tour's layout: the default panels at their default sizes, every page reachable (the settings stay as they are)
    setViewerLarge(false);
    const bool defaults[] = {true, false, false, true, false, false, false}; // tourToggles() order
    const QVector<QToolButton*> toggles = tourToggles();
    for (int i = 0; i < toggles.size(); ++i) toggles[i]->setChecked(defaults[i]);
    m_scopes->setVisible(true);
    applySizes(m_colorBottom, {1100, 420});
    updatePageButtons();

    // AIs wait meanwhile (APP_WAITING with this text): their edits would land in the practice project
    if (m_liveWaiting)
        m_liveWaiting("a tour runs in the schneidi app (the user is learning on a practice project, the real project is "
                      "set aside); try again once the user ended it");
    m_project->setHistoryAside(true); // the user's undo history waits untouched; the practice project gets its own
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
    if (!onTimeline) offerClipFormat(files, false); // also with the clips only in the pool: no format question on the drop
    if (practice == QLatin1String("stack") && files.size() > 1) m_editor->addMediaAt({files[1]}, 0, 1); // V2 over V1
    // Loaded once more: no undo steps of its own making, the playhead inside the first clip, a fixed timeline view
    // (set once here, as the tour begins: no step and no drop changes it, core rule)
    ProjectData d = m_project->data();
    const double fps = m_project->timelineFormat().rate.fps();
    d.playhead = onTimeline ? m_project->timelineFormat().rate.timebase() / 2 : 0;
    SavedView view;
    view.sequence = m_project->currentSequence();
    view.pxPerFrame = kTourPxPerSecond / fps;
    view.playhead = d.playhead;
    d.views = {view};
    applyLoaded(std::move(d), {}, false);
    m_timeline->view()->setZoomAndScroll(view.pxPerFrame, 0, 0);
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
    applyLoaded(std::move(s.data), s.path, false); // clears only the practice history
    m_project->setHistoryAside(false);             // the user's undo and redo steps back, its clean state too
    m_saveSuggestion = s.saveSuggestion;
    m_templateDir = s.templateDir;
    m_templateCopies = s.templateCopies;
    if (s.modified && !m_project->isModified()) m_project->markModified();
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
    if (!isMaximized() && !isFullScreen() && s.window.isValid() && size() != s.window) resize(s.window);
    if (m_liveWaiting) m_liveWaiting({}); // AIs may work on the user's project again
}
