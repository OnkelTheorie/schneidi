// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the in-app tours (app/TourManager.h): names for their targets, the pages by name, the sample clips.
#include "app/MainWindow.h"

#include "app/TourManager.h"
#include "core/Project.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
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
    for (QAbstractButton* b : m_pageButtons->buttons()) // page buttons: the same names the page bar uses elsewhere
        if (b->objectName().isEmpty()) b->setObjectName("pageButton_" + pageName(Page(m_pageButtons->id(b))));

    TourManager::Host host;
    host.page = [this] { return pageName(m_page); };
    host.showPage = [this](const QString& name) {
        for (const auto& [p, n] : kPageNames)
            if (name == QLatin1String(n)) showPage(p);
    };
    host.playhead = [this] { return m_timeline->view()->playhead(); };
    host.loadSample = [this] { loadTourSample(); };
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
