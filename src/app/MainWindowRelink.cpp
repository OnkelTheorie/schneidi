// SPDX-License-Identifier: AGPL-3.0-only
// Find Missing Media: moved files looked for by themselves near their old places (core/Relink.h), in the background,
// at any time (File menu, Media Pool menu) and after opening a project; what is not found clearly is asked about.
#include "app/MainWindow.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Relink.h"
#include "engine/Engine.h"
#include "ui/Viewer.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

void MainWindow::updateFindMissingAction()
{
    if (m_findMissingAction) m_findMissingAction->setEnabled(!m_relinkJob && !m_project->missingMedia().isEmpty());
}

void MainWindow::findMissingMedia(bool atOpen)
{
    if (m_relinkJob) return; // one search at a time (its line in the status bar says so)
    const QStringList missing = m_project->missingMedia();
    if (missing.isEmpty()) {
        if (!atOpen) statusBar()->showMessage(T("Keine Medien fehlen."), 5000);
        updateFindMissingAction();
        return;
    }
    startRelinkSearch(Relink::nearRoots(missing), true, [this](int found) {
        if (found > 0) {
            const QString text = found == 1 ? T("1 verschobene Datei automatisch wiedergefunden")
                                            : T("%1 verschobene Dateien automatisch wiedergefunden").arg(found);
            statusBar()->showMessage(text, 10000);
            m_viewer->showNotice(text, 4000);
        }
        if (!m_project->missingMedia().isEmpty()) askForMissingMedia();
    });
}

void MainWindow::askForMissingMedia()
{
    // Like "Media Offline" elsewhere: have a folder searched or leave offline
    const QStringList missing = m_project->missingMedia();
    if (missing.isEmpty()) return;
    // Files only the Media Pool still lists (no timeline reads them) cost nothing: said, so nobody hunts for them
    const QStringList unused = ProjectFile::unusedMedia(m_project->data());
    const bool allUnused = std::all_of(missing.cbegin(), missing.cend(), [&](const QString& p) { return unused.contains(p); });
    QString text;
    if (allUnused)
        text = missing.size() == 1
                   ? T("1 Datei im Medienpool nicht gefunden (verschoben oder gelöscht?). Keine Timeline benutzt "
                       "sie: das Projekt spielt und exportiert auch ohne sie.")
                   : T("%1 Dateien im Medienpool nicht gefunden (verschoben oder gelöscht?). Keine Timeline benutzt "
                       "sie: das Projekt spielt und exportiert auch ohne sie.").arg(missing.size());
    else
        text = missing.size() == 1 ? T("1 Datei nicht gefunden (verschoben oder umbenannt?):")
                                   : T("%1 Dateien nicht gefunden (verschoben oder umbenannt?):").arg(missing.size());
    QMessageBox box(QMessageBox::Warning, T("Medien fehlen"), text, QMessageBox::NoButton, this);
    box.setObjectName("missingMediaBox");
    QStringList names;
    for (const QString& p : missing.mid(0, 15))
        names << (!allUnused && unused.contains(p) ? T("%1 (in keiner Timeline benutzt)").arg(p) : p);
    if (missing.size() > 15) names << "…";
    box.setInformativeText(names.join('\n'));
    QPushButton* search = box.addButton(T("Ordner durchsuchen…"), QMessageBox::AcceptRole);
    search->setObjectName("relinkSearchFolder");
    box.addButton(T("Offline lassen"), QMessageBox::RejectRole)->setObjectName("relinkLeaveOffline");
    box.exec();
    if (box.clickedButton() != search) return;
    const QString start = m_projectPath.isEmpty() ? QDir::homePath() : QFileInfo(m_projectPath).absolutePath();
    const QString dir = QFileDialog::getExistingDirectory(this, T("Ordner mit den Medien wählen"), start);
    if (dir.isEmpty()) return;
    startRelinkSearch({Relink::Root{dir, Relink::kFolderDepth}}, false, [this](int found) {
        if (found == 0) {
            QMessageBox::information(this, T("Medien fehlen"), T("In diesem Ordner wurden keine der Dateien gefunden."));
            askForMissingMedia(); // another folder, or leave them offline
            return;
        }
        statusBar()->showMessage(found == 1 ? T("1 Datei wiedergefunden") : T("%1 Dateien wiedergefunden").arg(found), 10000);
        if (!m_project->missingMedia().isEmpty()) askForMissingMedia(); // some still missing: another folder?
    });
}

void MainWindow::startRelinkSearch(const QVector<Relink::Root>& roots, bool near, const std::function<void(int)>& then)
{
    if (m_relinkJob) return;
    QVector<MediaInfo> missing;
    QSet<QString> taken;
    for (const MediaInfo& m : m_project->media()) {
        taken.insert(m.path);
        if (!QFileInfo::exists(m.path)) missing << m;
    }
    if (missing.isEmpty()) {
        then(0);
        return;
    }
    auto progress = std::make_shared<Relink::Progress>();
    m_relinkJob = progress;
    updateFindMissingAction();

    // A line in the status bar while it runs (never a window in the way): what it does, how far, Cancel
    if (!m_relinkStatus) {
        m_relinkStatus = new QWidget(this);
        m_relinkStatus->setObjectName("relinkStatus");
        auto* row = new QHBoxLayout(m_relinkStatus);
        row->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(m_relinkStatus);
        label->setObjectName("relinkStatusText");
        auto* bar = new QProgressBar(m_relinkStatus);
        bar->setRange(0, 0); // how many folders there are is not known
        bar->setMaximumWidth(80);
        bar->setMaximumHeight(12);
        bar->setTextVisible(false);
        auto* cancel = new QToolButton(m_relinkStatus);
        cancel->setObjectName("relinkCancel");
        cancel->setText(T("Abbrechen"));
        connect(cancel, &QToolButton::clicked, this, [this] {
            if (m_relinkJob) m_relinkJob->cancel = true;
        });
        row->addWidget(label);
        row->addWidget(bar);
        row->addWidget(cancel);
        statusBar()->addPermanentWidget(m_relinkStatus);
    }
    auto* label = m_relinkStatus->findChild<QLabel*>("relinkStatusText");
    const QString what = near ? T("Suche verschobene Dateien…") : T("Durchsuche den Ordner…");
    label->setText(what);
    m_relinkStatus->show();
    // The count of a search cancelled by another project opening stops counting here
    qDeleteAll(m_relinkStatus->findChildren<QTimer*>(Qt::FindDirectChildrenOnly));
    auto* tick = new QTimer(m_relinkStatus);
    tick->setInterval(150);
    connect(tick, &QTimer::timeout, label, [label, progress, what] {
        const int dirs = progress->dirs.load();
        label->setText(dirs == 1 ? what + ' ' + T("1 Ordner") : what + ' ' + T("%1 Ordner").arg(dirs));
    });
    tick->start();

    const quint64 generation = m_project->generation();
    const ProjectFormat format = m_project->format();
    QThreadPool::globalInstance()->start([guard = QPointer<MainWindow>(this), progress, missing, taken, roots, near,
                                          format, generation, then, tick = QPointer<QTimer>(tick)] {
        const Relink::Result found = Relink::search(
            missing, taken, roots, near ? Relink::Mode::Near : Relink::Mode::Folder,
            near ? Relink::kNearLimits : Relink::kFolderLimits,
            [format](const QString& file) { return Engine::probe(format, file); }, progress.get());
        QMetaObject::invokeMethod(qApp, [guard, progress, found, generation, then, tick] {
            if (tick) tick->deleteLater();
            if (!guard || guard->m_relinkJob != progress) return; // another project meanwhile (its own search)
            guard->m_relinkJob.reset();
            guard->m_relinkStatus->hide();
            guard->updateFindMissingAction();
            if (progress->cancel.load() || guard->m_project->generation() != generation) return;
            const int n = guard->m_project->relinkMedia(found.moved, found.probed, T("Fehlende Medien wiedergefunden"));
            guard->updateFindMissingAction();
            then(n);
        }, Qt::QueuedConnection);
    });
}
