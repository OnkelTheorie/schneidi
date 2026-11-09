// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the in-app tours (app/TourManager.h): names for their targets, the pages by name, the sample clips.
#include "app/MainWindow.h"

#include "app/TourManager.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "core/I18n.h"
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
    host.offers["comp_example"] = {N_("Beispiel anlegen"), [this] { return !compHasMerge(); }, [this] { tourCompExample(); }};
    host.conditions["comp_has_merge"] = [this] { return compHasMerge(); };
    // The Comp page's node view (the graph the tour explains)
    host.actions["comp_nodes"] = [this] {
        ensureCompPage();
        if (auto* b = m_compPage->findChild<QToolButton*>("comp_view_nodes")) b->setChecked(true);
    };
    // A panel the user switched off: shown for the step, hidden again after it (the toggle and its setting stay)
    host.reveal = [this](const QString& target) -> std::function<void()> {
        QWidget* panel = target == QLatin1String("mediaPool") ? static_cast<QWidget*>(m_mediaPool)
                         : target == QLatin1String("inspector") ? static_cast<QWidget*>(m_inspector)
                         : target == QLatin1String("scopes")    ? static_cast<QWidget*>(m_scopes)
                                                                : nullptr;
        if (!panel || !panel->isHidden() || !panel->parentWidget()) return {};
        const bool leftHidden = panel == m_mediaPool && m_editLeft->isHidden();
        panel->show();
        if (leftHidden) m_editLeft->show();
        return [this, panel = QPointer<QWidget>(panel), leftHidden] {
            if (panel) panel->hide();
            if (leftHidden) updateLeftColumn();
        };
    };
    // The timeline's ruler (click there = playhead) and the playhead line, in window coordinates
    host.areas["ruler"] = [this]() -> QRect {
        TimelineView* tv = m_timeline->view();
        if (!tv->isVisible()) return {};
        return QRect(tv->mapTo(this, QPoint(TimelineView::kHeaderW, 0)),
                     QSize(tv->width() - TimelineView::kHeaderW, TimelineView::kRulerH));
    };
    host.areas["playhead"] = [this]() -> QRect {
        TimelineView* tv = m_timeline->view();
        const int x = tv->playheadX();
        if (!tv->isVisible() || x < TimelineView::kHeaderW || x >= tv->width()) return {};
        return QRect(tv->mapTo(this, QPoint(x - 5, 0)), QSize(11, tv->height()));
    };
    // Parts of the Comp page's graph (CompCanvas::tourArea): the output node, the ports
    for (const QString& part : {QStringLiteral("output"), QStringLiteral("node:merge"), QStringLiteral("node:transform"), QStringLiteral("port:src"),
                                QStringLiteral("port:bg"), QStringLiteral("port:fg"), QStringLiteral("port:mask")})
        host.areas["comp_" + QString(part).replace(':', '_')] = [this, part]() -> QRect {
            if (!m_compPage || !m_compPage->canvas()->isVisible()) return {};
            CompCanvas* c = m_compPage->canvas();
            const QRect r = c->tourArea(part).intersected(c->rect());
            return r.isValid() ? QRect(c->mapTo(this, r.topLeft()), r.size()) : QRect();
        };
    m_tours = new TourManager(this, m_project, host);
    m_tours->addMenu(help);
    // Queued: the timeline takes the new playhead in a slot connected later
    connect(m_engine, &Engine::positionChanged, m_tours, &TourManager::poke, Qt::QueuedConnection);
}

void MainWindow::offerTours()
{
    // An AI driving the app right now (live connection): no question in its way; asked at a later start
    if (m_tours && !m_liveConnected) m_tours->offerAtStart();
}

bool MainWindow::compHasMerge() const
{
    if (!m_compPage || !m_compPage->clip()) return false;
    const Clip* c = TimelineOps::findClip(m_project->timeline(), m_compPage->clip());
    CompGraph g;
    if (!c || !CompUi::graphOf(*c, &g)) return false;
    return std::any_of(g.nodes.cbegin(), g.nodes.cend(), [](const CompNode& n) { return n.type == QLatin1String("merge"); });
}

void MainWindow::tourCompExample()
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
    // The built-in "picture in picture" recipe: a merge with its background and foreground, one undo step
    QString err;
    if (!CompUi::applyRecipe(m_editor, {id}, "pip_frame", {}, &err)) {
        statusBar()->showMessage(T("Geht nicht: %1").arg(err), 5000);
        return;
    }
    openComp(id);
    if (auto* b = m_compPage->findChild<QToolButton*>("comp_view_nodes")) b->setChecked(true);
}

void MainWindow::loadTourSample()
{
    // Built in, nothing is downloaded: the short game clips of the built-in highlight template, copied to the app's data
    // folder (the engine reads files, not resources) and imported
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
    if (!files.isEmpty()) importFiles(files, false, m_project->media().isEmpty());
}
