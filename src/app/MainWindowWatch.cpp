// MainWindow: watch folders (app/FolderWatch.h) - the watcher of the open project, its notice, the dialogs
#include "app/FolderWatch.h"
#include "app/MainWindow.h"
#include "app/WatchFoldersDialog.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "ui/MediaPool.h"
#include "ui/MediaStorage.h"

#include <QDir>
#include <QStatusBar>

void MainWindow::setupWatchFolders()
{
    m_folderWatch = new FolderWatch(m_project, this);
    // Imports wait while an AI command runs in the app or the AI chat works on a request (its changes would no longer
    // be one step to accept or discard)
    m_folderWatch->setBusy([this] { return m_liveCommand || (m_review.active && !m_review.pending); });
    connect(m_folderWatch, &FolderWatch::imported, this, [this](const QStringList& paths, const QString& folder) {
        const MediaInfo* m = m_project->mediaInfo(paths.first());
        statusBar()->showMessage(WatchFolders::importedNote(paths, folder, m_project->binName(m ? m->bin : 0)), 8000);
        m_mediaPool->markFresh(paths);
    });
    connect(m_mediaPool, &MediaPool::watchFolderRequested, this,
            [this](int bin) { WatchFoldersDialog::addFolder(*m_project, {}, bin, this); });
    connect(m_storage, &MediaStorage::watchFolderRequested, this, [this](const QString& dir) {
        WatchFoldersDialog::addFolder(*m_project, dir, WatchFolderDialog::kNewBin, this);
    });
}

void MainWindow::watchFoldersDialog()
{
    WatchFoldersDialog dlg(*m_project, this);
    if (dlg.exec() == QDialog::Accepted) WatchFoldersDialog::apply(*m_project, dlg.folders());
}
