// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the in-app tours (app/TourManager.h): names for their targets, the pages by name, the sample clips.
#include "app/MainWindow.h"

#include "app/TourManager.h"
#include "app/VoiceoverRecorder.h"
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
#include <QApplication>
#include <QTimer>
#include "engine/RenderQueue.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"
#include <QListWidget>
#include <QStackedWidget>
#include <QMenuBar>
#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSplitter>
#include <QStatusBar>
#include <QPointer>
#include <QUndoStack>
#include <algorithm>
#include <cmath>
#include <memory>
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
    m_effects->setProperty("tourTarget", "effects");
    // the scopes' target: ensureColorPage (built on the first use)
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
            m_engine->seek(c->start + c->length() / 2);
            m_timeline->view()->setPlayhead(c->start + c->length() / 2);
        }
    };
    // The Inspector shows a clip: the first video clip selected when none is (a selection of deleted clips counts as
    // none: the Inspector would stay empty)
    host.actions["select_clip"] = [this] {
        for (int id : m_selection->ids())
            if (TimelineOps::findClip(m_project->timeline(), id)) return;
        for (const QVector<Track>* tracks : {&m_project->timeline().video, &m_project->timeline().audio})
            for (const Track& t : *tracks)
                if (!t.clips.isEmpty()) {
                    m_selection->set({t.clips.first().id});
                    return;
                }
    };
    // The split step: the playhead in the middle of the clip it is about (the first on V1, else the first video clip),
    // that clip selected (the split key splits the selected clips only) - the key just works. Only the playhead moves,
    // never the timeline's zoom or scroll (core rule): a clip outside the visible part gets the playhead in its middle
    // and a note that it is out of view; one partly visible, in the middle of what shows
    auto note = std::make_shared<QString>();
    host.takeNote = [note] { return std::exchange(*note, QString()); };
    host.actions["playhead_mid_clip"] = [this, note] {
        const Timeline& tl = m_project->timeline();
        const Clip* clip = !tl.video.isEmpty() && !tl.video[0].clips.isEmpty() ? &tl.video[0].clips[0] : nullptr;
        for (const Track& t : tl.video)
            for (const Clip& c : t.clips)
                if (!clip && c.kind == ClipKind::Media) clip = &c;
        if (!clip || clip->length() < 2) return;
        const int id = clip->id, start = clip->start, end = clip->end();
        TimelineView* tv = m_timeline->view();
        const ViewState& v = tv->view();
        int from = start, to = end; // the frames of the clip on screen
        if (v.pxPerFrame > 0) {
            from = std::max(from, int(std::ceil(v.leftFrame)));
            to = std::min(to, int(v.leftFrame + (tv->width() - TimelineView::kHeaderW) / v.pxPerFrame));
        }
        int at = start + (end - start) / 2;
        if (tv->clipArea(id).isEmpty() || to - from < 2) *note = N_("Der Clip liegt gerade außerhalb des sichtbaren "
                                                                     "Teils der Timeline: Scroll hin, dann siehst du "
                                                                     "den Abspielkopf in seiner Mitte.");
        else if (from > start || to < end) at = std::clamp(from + (to - from) / 2, start + 1, end - 1);
        m_selection->set({id});
        m_engine->seek(at);
        tv->setPlayhead(at);
    };
    // The steps work on a video clip of the practice project. Deleted (the user selected all and pressed Delete):
    // undone, step by step until one is back (the practice project has a history of its own; never past an import);
    // nothing left to undo: a sample clip placed at the end
    const auto hasClip = [this] {
        for (const Track& t : m_project->timeline().video)
            for (const Clip& c : t.clips)
                if (c.kind == ClipKind::Media) return true;
        return false;
    };
    host.hasClip = hasClip;
    host.restoreClip = [this, hasClip] {
        using R = TourManager::Host::Restored;
        if (hasClip()) return R::No;
        QUndoStack* undo = m_project->undoStack();
        for (int guard = 0; guard < 20 && !hasClip() && undo->canUndo(); ++guard) {
            const qsizetype media = m_project->media().size();
            undo->undo();
            if (m_project->media().size() < media) { // an import: that one stays
                undo->redo();
                break;
            }
        }
        if (hasClip()) return R::Undone;
        if (m_project->media().isEmpty()) loadTourSample();
        for (const MediaInfo& m : m_project->media())
            if (m.hasVideo) {
                m_editor->addMediaAt({m.path}, TimelineOps::endFrame(m_project->timeline()), 0);
                break;
            }
        return hasClip() ? R::Placed : R::No;
    };
    // Tour mode: a practice project and a fixed layout; the user's project and layout come back at the end
    host.enter = [this](const Tour& t) { enterTourMode(t); };
    host.leave = [this] { leaveTourMode(); };
    host.unsaved = [this] { return m_project->isModified() && !m_autosaveDisabled; }; // test runs never ask
    host.save = [this] { return save(); };
    // What a swap to the practice project would break: a render of the user's queue (its job ids are per project),
    // AI changes waiting for review (Accept would act on the practice history), a window tracking on the user's clip
    host.blocked = [this]() -> QString {
        if (m_tourSaved.active) return {}; // one tour after the other: the first one's project goes away anyway
        // A voice-over take would end and land in the practice project
        if (m_voiceover && m_voiceover->recording())
            return T("Der Rundgang startet nach der Aufnahme – erst die Aufnahme beenden (Esc).");
        if (m_deliver->renderQueue()->isRunning())
            return T("Rundgang erst nach dem Rendern – oder das Rendern abbrechen.");
        if (m_review.active)
            return T("Rundgang erst, wenn die Änderungen der KI übernommen oder verworfen sind.");
        if (m_colorPanel && m_colorPanel->tracking()) return T("Rundgang erst, wenn das Verfolgen des Fensters fertig ist.");
        return {};
    };
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
    for (const QString& part : {QStringLiteral("output"), QStringLiteral("node:merge"), QStringLiteral("node:transform"), QStringLiteral("node:key"), QStringLiteral("port:src"),
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
        // The clip's last fifth and a little beyond its edge: easy to grab
        const int w = std::max(28, c.width() / 5);
        return {m_timeline->view(), QRect(c.right() - w, c.top(), w + 12, c.height())};
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
    // The timeline's tool bar (select, trim, blade ...), above its ruler
    host.areas["timeline_tools"] = [this]() -> Area {
        TimelineView* tv = m_timeline->view();
        const int top = tv->mapTo(m_timeline, QPoint(0, 0)).y();
        return {m_timeline, QRect(0, 0, m_timeline->width(), top)};
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
                                   [this] { colorPanel()->addWindow("rectangle"); }};
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
    host.setTool = [this](const QString& tool) {
        const QPair<const char*, TimelineView::Tool> tools[] = {
            {"select", TimelineView::Tool::Select}, {"trim", TimelineView::Tool::Trim}, {"blade", TimelineView::Tool::Blade}};
        for (const auto& [name, t] : tools)
            if (tool == QLatin1String(name)) m_timeline->view()->setTool(t);
    };
    host.home = [this] { return QRect(m_pages->mapTo(this, QPoint(0, 0)), m_pages->size()); };
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
        if (m_colorPanel) m_colorPanel->setTourButtonAllowed(!on);
    });
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
    closePopups(); // a context menu still open would act on practice clips with the same ids
    endVoiceover();
    m_engine->pause();
    if (!m_autosaveDisabled) autosave(); // the backup up to date: a crash during the tour still restores the user's work
    // The user's project as it is (unsaved changes included) and how the window looks, kept aside
    TourSaved& s = m_tourSaved;
    s = {};
    s.active = true;
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
    s.scopes = m_scopes ? !m_scopes->isHidden() : QSettings().value("color/scopesVisible", true).toBool();
    for (QSplitter* sp : findChildren<QSplitter*>()) s.sizes.push_back({sp, sp->sizes()});
    s.window = size();
    s.tool = int(m_timeline->view()->tool());
    s.fingerprint = m_projectFingerprint;
    s.compClip = m_compPage ? m_compPage->clip() : 0;
    s.clipboards = m_editor->clipboards();
    s.views = s.data.views;

    // The tour's layout: the default panels at their default sizes, every page reachable (the settings stay as they are)
    setViewerLarge(false);
    const bool defaults[] = {true, false, false, true, false, false, false}; // tourToggles() order
    const QVector<QToolButton*> toggles = tourToggles();
    for (int i = 0; i < toggles.size(); ++i) toggles[i]->setChecked(defaults[i]);
    if (m_scopes) { // not built yet: ensureColorPage shows them during a tour
        m_scopes->setVisible(true);
        applySizes(m_colorBottom, {1100, 420});
    }
    updatePageButtons();

    // AIs wait meanwhile (APP_WAITING with this text): their edits would land in the practice project
    if (m_liveWaiting)
        m_liveWaiting("a tour runs in the schneidi app (the user is learning on a practice project, the real project is "
                      "set aside); try again once the user ended it");
    m_project->setHistoryAside(true); // the user's undo history waits untouched; the practice project gets its own
    m_tourSwap = true;
    buildPracticeProject(tour.practice);
    m_tourSwap = false;
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
    // Handles: each clip starts a little into its file and ends a little before its end, so a transition dropped on
    // a cut finds material to overlap (no "trim the clips?" question in the tour); the clips stay one after another
    const int handle = m_project->timelineFormat().rate.timebase() / 3;
    for (QVector<Track>* tracks : {&d.timeline.video, &d.timeline.audio})
        for (Track& t : *tracks) {
            int at = t.clips.isEmpty() ? 0 : t.clips.first().start;
            for (Clip& c : t.clips) {
                if (c.kind == ClipKind::Media && c.out - c.in > 6 * handle) {
                    c.in += handle;
                    c.out -= handle;
                }
                c.start = at;
                at = c.end();
            }
        }
    d.playhead = onTimeline ? m_project->timelineFormat().rate.timebase() / 2 : 0;
    SavedView view;
    view.sequence = m_project->currentSequence();
    // The sample clips fill about 60 % of the timeline's width (wide enough to grab a clip's edge), the rest stays
    // free for the bubble
    double seconds = 0;
    for (const MediaInfo& m : m_project->media()) seconds += m.length / fps;
    const int lane = std::max(400, m_timeline->view()->width() - TimelineView::kHeaderW);
    view.pxPerFrame = std::clamp(0.6 * lane / std::max(1.0, seconds), 40.0, 240.0) / fps;
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
    closePopups(); // a menu on a practice clip must not act on the user's clip with the same id
    endVoiceover(); // before the practice project goes
    m_deliver->renderQueue()->cancel(); // a practice render: its jobs' ids would mark the user's jobs
    TourSaved s = std::exchange(m_tourSaved, {});
    m_engine->pause();
    // The user's project back as it was: unsaved changes still unsaved, the same file, playhead, zoom and selection
    m_tourSwap = true;
    applyLoaded(std::move(s.data), s.path, false); // clears only the practice history
    m_tourSwap = false;
    m_project->setHistoryAside(false);             // the user's undo and redo steps back, its clean state too
    m_editor->setClipboards(s.clipboards);
    // The file as the user last loaded or saved it (not as it is now): a change from outside during the tour is
    // noticed below instead of being overwritten by the next save
    m_projectFingerprint = s.fingerprint;
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
    if (m_scopes) m_scopes->setVisible(s.scopes);
    m_tourSaved.active = false;
    if (m_colorBottom) applySizes(m_colorBottom, {1100, 420});
    m_tourSwap = true; // the page as it was: no page entry moves the user's playhead or changes the Comp clip
    showPage(s.page);
    m_tourSwap = false;
    if (m_compPage) m_compPage->setClip(s.compClip);
    updatePageButtons();
    for (const auto& [sp, sizes] : s.sizes)
        if (sp && sp->count() == sizes.size()) sp->setSizes(sizes);
    if (s.large) setViewerLarge(true);
    if (!isMaximized() && !isFullScreen() && s.window.isValid() && size() != s.window) resize(s.window);
    m_timeline->view()->setTool(TimelineView::Tool(s.tool)); // the user's tool, whatever the steps chose
    // The zoom and scroll last, once the page's layout settled (the scroll range needs the view's real height; set
    // earlier, a scroll further down came back clamped). Core rule: as the user left them.
    const auto restoreView = [this, views = s.views, seq = m_project->currentSequence()] {
        for (const SavedView& v : views)
            if (v.sequence == seq && v.sequence == m_project->currentSequence())
                m_timeline->view()->setZoomAndScroll(v.pxPerFrame, v.leftFrame, v.scrollY);
    };
    restoreView();
    QTimer::singleShot(0, m_timeline->view(), restoreView); // after restoreViews' own (queued) one and the layout
    if (m_liveWaiting) m_liveWaiting({}); // AIs may work on the user's project again
    checkProjectFile();
}

void MainWindow::closePopups()
{
    for (int guard = 0; guard < 10; ++guard) // nested menus: the innermost first
        if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        else break;
}
