// MainWindow: project file (new, open, save, autosave, backups, recent files, presets).
#include "app/MainWindow.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "app/KeyBindingsDialog.h"
#include "app/ClipSpeedDialog.h"
#include "app/DesignDialog.h"
#include "app/Log.h"
#include "app/NormalizeDialog.h"
#include "app/PasteAttributesDialog.h"
#include "core/Loudness.h"
#include "app/ProjectSettingsDialog.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/I18n.h"
#include "core/Timecode.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/MediaCache.h"
#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaStorage.h"
#include "ui/ColorPanel.h"
#include "ui/ScopesPanel.h"
#include "ui/Inspector.h"
#include "ui/Mixer.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QFileSystemWatcher>
#include <QMouseEvent>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QMenuBar>
#include <QSplitter>
#include <QDesktopServices>
#include <QUrl>
#include <QUndoStack>
#include <QMenu>
#include <QProgressDialog>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QLockFile>
#include <QSaveFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QInputDialog>
#include <QStandardPaths>
#include <QTimer>

// ---- Projektdatei ---------------------------------------------------------------

namespace {
const QString kFileFilter = T("schneidi-Projekt (*.%1)").arg(ProjectFile::Extension);
}

QString MainWindow::autosavePath()
{
    const QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (m_autosaveSlot == 0) {
        dir.mkpath(".");
        // ersten freien Platz belegen; eine abgestürzte Instanz hinterlässt eine verwaiste Sperre, die QLockFile erkennt
        for (int slot = 1; slot <= 9 && m_autosaveSlot == 0; ++slot) {
            auto lock = std::make_unique<QLockFile>(dir.filePath(QString("autosave-%1.lock").arg(slot)));
            if (lock->tryLock(0)) {
                m_autosaveSlot = slot;
                m_autosaveLock = std::move(lock);
            }
        }
        if (m_autosaveSlot == 0) m_autosaveSlot = 10; // alles belegt: eigener Platz ohne Sperre
    }
    const QString name = m_autosaveSlot == 1 ? QString("autosave") : QString("autosave-%1").arg(m_autosaveSlot);
    return dir.filePath(QString("%1.%2").arg(name, ProjectFile::Extension));
}

QString MainWindow::autosaveKey() const
{
    return m_autosaveSlot <= 1 ? QString("autosave/projectPath") : QString("autosave/projectPath-%1").arg(m_autosaveSlot);
}

void MainWindow::updateTitle()
{
    const QString name = m_projectPath.isEmpty() ? T("Unbenannt") : QFileInfo(m_projectPath).completeBaseName();
    const QString shown = name + (m_project->isModified() ? " *" : "");
    setWindowTitle(shown + " – schneidi");
    if (m_titleLabel) m_titleLabel->setText(shown);
}

void MainWindow::setProjectPath(const QString& path)
{
    m_projectPath = path;
    if (!path.isEmpty() && !m_autosaveDisabled) addRecent(path); // Testlauf: Liste des Nutzers nicht anfassen
    updateTitle();
    // Called right after loading/saving: the file now holds our state
    m_projectFingerprint = path.isEmpty() ? QByteArray() : ProjectFile::fingerprint(path);
    if (const QStringList watched = m_projectWatcher->files(); !watched.isEmpty()) m_projectWatcher->removePaths(watched);
    if (!path.isEmpty()) m_projectWatcher->addPath(path);
}

void MainWindow::checkProjectFile()
{
    if (m_projectPath.isEmpty()) return;
    if (!m_projectWatcher->files().contains(m_projectPath) && QFileInfo::exists(m_projectPath))
        m_projectWatcher->addPath(m_projectPath); // replaced by a QSaveFile rename
    const QByteArray now = ProjectFile::fingerprint(m_projectPath);
    if (now.isEmpty() || now == m_projectFingerprint) return; // gone (being replaced) or our own save
    // Not now: a dialog is open or a render job writes its status into this project -> try again shortly
    if (QApplication::activeModalWidget() || m_deliver->renderQueue()->isRunning()) {
        m_projectCheck.start(2000);
        return;
    }
    if (m_project->isModified()) {
        QMessageBox box(QMessageBox::Question, "schneidi",
                        T("Das Projekt wurde außerhalb von schneidi geändert (z. B. mit schneidi-cli)."),
                        QMessageBox::NoButton, this);
        box.setInformativeText(T("Neu laden verwirft die ungespeicherten Änderungen hier. Behalten überschreibt die "
                                 "Datei beim nächsten Speichern (der andere Stand bleibt als Sicherungskopie)."));
        QPushButton* reload = box.addButton(T("Neu laden"), QMessageBox::AcceptRole);
        box.setDefaultButton(box.addButton(T("Behalten"), QMessageBox::RejectRole));
        box.exec();
        if (box.clickedButton() != reload) {
            m_projectFingerprint = now; // do not ask again for this change
            return;
        }
    }
    ProjectData data;
    QString error;
    if (!ProjectFile::load(m_projectPath, &data, &error)) return; // half written by a foreign program: wait for the next change
    data.playhead = m_timeline->view()->playhead(); // stay where the person is
    applyLoaded(std::move(data), m_projectPath, false);
}

// Neustart jetzt oder später; „jetzt“ fragt wie beim Beenden nach dem Speichern und öffnet das Projekt danach wieder
void MainWindow::offerRestart(const QString& message, const QString& now, const QString& later)
{
    QMessageBox box(QMessageBox::Question, "schneidi", message, QMessageBox::NoButton, this);
    QPushButton* nowButton = box.addButton(now, QMessageBox::AcceptRole);
    box.setDefaultButton(box.addButton(later, QMessageBox::RejectRole));
    box.exec();
    if (box.clickedButton() != nowButton) return;
    m_restartRequested = true;
    if (!close()) m_restartRequested = false; // Speichern abgebrochen -> weiterarbeiten
}

QStringList MainWindow::restartArguments() const
{
    return m_projectPath.isEmpty() ? QStringList{} : QStringList{m_projectPath};
}

bool MainWindow::maybeSave()
{
    if (!m_project->isModified() || m_autosaveDisabled) return true; // Testlauf: nie nachfragen
    const auto answer = QMessageBox::question(this, "schneidi", T("Änderungen am Projekt speichern?"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Save);
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

bool MainWindow::maybeLeaveProject()
{
    RenderQueue* queue = m_deliver->renderQueue();
    if (queue->isRunning() && !m_autosaveDisabled) {
        if (QMessageBox::question(this, "schneidi",
                                  T("Es wird gerade gerendert. Ein anderes Projekt bricht das Rendern ab. Trotzdem fortfahren?"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return false;
        queue->cancel(); // synchron: Status landet noch in diesem Projekt
    }
    return maybeSave();
}

void MainWindow::newProject()
{
    if (!maybeLeaveProject()) return;
    m_engine->pause();
    m_selection->clear();
    m_engine->setFormat(ProjectFormat{}); // Medienlängen passen schon (leer) -> nicht neu einlesen
    m_project->reset();
    m_sourcePos.clear();
    m_engine->showTimeline(0);
    setProjectPath({});
    removeAutosave();
}

void MainWindow::openProjectDialog()
{
    if (!maybeLeaveProject()) return;
    const QString dir = m_projectPath.isEmpty() ? QDir::homePath() : QFileInfo(m_projectPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, T("Projekt öffnen"), dir, kFileFilter);
    if (!path.isEmpty()) openProject(path);
}

const Clip* MainWindow::presetClip() const
{
    // Like adding effects: selected video clips (else the one under the playhead), the earliest with effects
    const Clip* best = nullptr;
    for (int id : m_editor->effectTargets(m_timeline->view()->playhead())) {
        const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
        const bool has = c && std::any_of(c->effects.begin(), c->effects.end(), [](const EffectInstance& e) {
            return e.effectId != QLatin1String("grade"); // Color page grade is not part of presets
        });
        if (has && (!best || c->start < best->start)) best = c;
    }
    return best;
}

void MainWindow::saveEffectPreset()
{
    const Clip* found = presetClip();
    if (!found) {
        QMessageBox::information(this, T("Effekte als Preset speichern"), T("Der Clip hat keine Effekte."));
        return;
    }
    const Clip clip = *found; // pointers into the timeline do not survive dialogs
    QString def;
    for (const EffectInstance& e : clip.effects)
        if (const EffectDescriptor* d = EffectRegistry::find(e.effectId); d && e.effectId != QLatin1String("grade")) {
            def = d->name;
            break;
        }
    bool ok = false;
    const QString name = QInputDialog::getText(this, T("Effekte als Preset speichern"), T("Name des Presets:"),
                                               QLineEdit::Normal, def, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    const QString path = Presets::userPresetPath(name);
    if (QFileInfo::exists(path)
        && QMessageBox::question(this, T("Effekte als Preset speichern"),
                                 T("Ein Preset „%1“ gibt es schon. Ersetzen?").arg(name)) != QMessageBox::Yes)
        return;
    QString error;
    QStringList skipped;
    if (!Presets::save(clip, name, path, &error, &skipped)) {
        QMessageBox::warning(this, T("Effekte als Preset speichern"),
                             T("%1 konnte nicht gespeichert werden:\n%2").arg(QDir::toNativeSeparators(path), error));
        return;
    }
    if (!skipped.isEmpty())
        QMessageBox::information(this, T("Effekte als Preset speichern"),
                                 T("Nicht im Preset (Color-Seite bzw. Plugin fehlt): %1").arg(skipped.join(", ")));
}

bool MainWindow::openProject(const QString& path)
{
    ProjectData data;
    QString error;
    if (!ProjectFile::load(path, &data, &error)) {
        QMessageBox::warning(this, T("Projekt öffnen"), T("%1 konnte nicht geöffnet werden:\n%2").arg(path, error));
        return false;
    }
    if (!applyLoaded(std::move(data), path)) return false;
    removeAutosave();
    return true;
}

bool MainWindow::applyLoaded(ProjectData data, const QString& path, bool ask)
{
    // Fehlende Medien wie DaVinci "Media Offline": Ordner durchsuchen lassen oder offline lassen
    int relinked = 0;
    for (QStringList missing = ask ? ProjectFile::missingMedia(data) : QStringList{}; !missing.isEmpty();
         missing = ProjectFile::missingMedia(data)) {
        QMessageBox box(QMessageBox::Warning, T("Medien fehlen"),
                        T("%1 Datei(en) nicht gefunden (verschoben oder umbenannt?):").arg(missing.size()),
                        QMessageBox::NoButton, this);
        QStringList names;
        for (const QString& p : missing.mid(0, 15)) names << p;
        if (missing.size() > 15) names << "…";
        box.setInformativeText(names.join('\n'));
        QPushButton* search = box.addButton(T("Ordner durchsuchen…"), QMessageBox::AcceptRole);
        box.addButton(T("Offline lassen"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != search) break;
        const QString dir = QFileDialog::getExistingDirectory(this, T("Ordner mit den Medien wählen"),
                                                              QFileInfo(path).absolutePath());
        if (dir.isEmpty()) break;
        const int found = ProjectFile::relink(&data, dir);
        relinked += found;
        if (found == 0)
            QMessageBox::information(this, T("Medien fehlen"), T("In diesem Ordner wurden keine der Dateien gefunden."));
    }

    // Effects whose plugin is missing here (project from another computer): stay in the project, but do nothing
    if (const QStringList fx = ask ? ProjectFile::missingEffects(data) : QStringList{}; !fx.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, T("Effekte fehlen"),
                        T("%1 Effekt(e) sind auf diesem Rechner nicht installiert und wirken nicht:").arg(fx.size()),
                        QMessageBox::Ok, this);
        box.setInformativeText(fx.mid(0, 15).join('\n') + (fx.size() > 15 ? "\n…" : "") + "\n\n"
                               + T("Sie bleiben mit ihren Werten im Projekt; der Inspector zeigt sie an."));
        box.exec();
    }

    m_engine->pause();
    m_selection->clear();
    m_probeCache.clear();
    m_engine->setFormat(data.format); // Medienlängen in der Datei zählen schon in dieser Framerate
    m_project->load(data);
    if (relinked > 0) m_project->markModified(); // neue Pfade stehen erst nach dem Speichern in der Datei
    m_sourcePos.clear();
    m_engine->showTimeline(data.playhead);
    m_timeline->view()->setPlayhead(data.playhead);
    setProjectPath(path);
    return true;
}

// Sicherungskopie laden (wie DaVinci „Project Backups“): öffnet als unbenanntes Projekt, damit sie nie
// versehentlich in den Backup-Ordner gespeichert wird; Speichern fragt nach dem Ziel
void MainWindow::openBackupDialog()
{
    if (!maybeLeaveProject()) return;
    QString dir = ProjectFile::backupDir(m_projectPath);
    if (!QFileInfo::exists(dir)) dir = ProjectFile::backupDir({});
    QDir().mkpath(dir);
    const QString path = QFileDialog::getOpenFileName(this, T("Sicherungskopie öffnen"), dir, kFileFilter);
    if (path.isEmpty()) return;
    ProjectData data;
    QString error;
    if (!ProjectFile::load(path, &data, &error)) {
        QMessageBox::warning(this, T("Projekt öffnen"), T("%1 konnte nicht geöffnet werden:\n%2").arg(path, error));
        return;
    }
    if (!applyLoaded(std::move(data), {})) return;
    m_project->markModified();
}

bool MainWindow::save()
{
    return m_projectPath.isEmpty() ? saveAs() : saveTo(m_projectPath);
}

bool MainWindow::saveAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, T("Projekt speichern unter"),
        m_projectPath.isEmpty() ? QDir::home().filePath(T("Unbenannt.%1").arg(ProjectFile::Extension)) : m_projectPath,
        kFileFilter);
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix() != ProjectFile::Extension) path += QString(".%1").arg(ProjectFile::Extension);
    return saveTo(path);
}

bool MainWindow::saveTo(const QString& path)
{
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    QString error;
    // schneidi-cli may be writing the same file right now: wait for it (it takes milliseconds)
    const std::unique_ptr<QLockFile> lock = ProjectFile::lock(path);
    if (!lock) {
        QMessageBox::warning(this, T("Projekt speichern"),
                             T("Die Projektdatei wird gerade von einem anderen Programm gespeichert. Bitte noch einmal versuchen."));
        return false;
    }
    if (!m_autosaveDisabled) ProjectFile::backup(path); // bisherigen Stand sichern (Testlauf: nichts ablegen)
    if (!ProjectFile::save(data, path, &error)) {
        QMessageBox::warning(this, T("Projekt speichern"), T("Speichern fehlgeschlagen:\n%1").arg(error));
        return false;
    }
    m_project->markSaved();
    setProjectPath(path);
    removeAutosave();
    return true;
}

void MainWindow::autosave()
{
    if (!m_project->isModified()) return;
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    QDir().mkpath(QFileInfo(autosavePath()).absolutePath());
    if (const QString path = autosavePath(); ProjectFile::save(data, path, nullptr))
        QSettings().setValue(autosaveKey(), m_projectPath); // wohin die Sicherung gehört
}

void MainWindow::disableAutosave()
{
    m_autosaveTimer->stop();
    m_autosaveDisabled = true;
    m_mediaPool->setSaveSettings(false); // Testläufe: Media-Pool-Ansicht nicht speichern
    m_storage->setPersistent(false);
}

void MainWindow::removeAutosave()
{
    if (m_autosaveDisabled) return; // Testlauf: echte Sicherung des Nutzers nicht anfassen

    QFile::remove(autosavePath());
    QSettings().remove(autosaveKey());
}

bool MainWindow::offerAutosaveRestore()
{
    if (m_autosaveDisabled || !QFileInfo::exists(autosavePath())) return false;
    const QString original = QSettings().value(autosaveKey()).toString();
    const QString name = original.isEmpty() ? T("Unbenannt") : QFileInfo(original).fileName();
    const auto answer = QMessageBox::question(
        this, T("Wiederherstellen"),
        T("schneidi wurde nicht normal beendet.\nNicht gespeicherte Änderungen von „%1“ wiederherstellen?")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        removeAutosave();
        return false;
    }
    ProjectData data;
    if (!ProjectFile::load(autosavePath(), &data, nullptr) || !applyLoaded(std::move(data), original)) return false;
    m_project->markModified(); // wiederhergestellt, aber noch nicht in die Projektdatei gespeichert
    return true;
}

void MainWindow::addRecent(const QString& path)
{
    QSettings settings;
    QStringList recent = settings.value("recentProjects").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    settings.setValue("recentProjects", recent.mid(0, 10));
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu) return;
    m_recentMenu->clear();
    const QStringList recent = QSettings().value("recentProjects").toStringList();
    for (const QString& path : recent) {
        QAction* a = m_recentMenu->addAction(QFileInfo(path).fileName(), this, [this, path] {
            if (maybeLeaveProject()) openProject(path);
        });
        a->setToolTip(path);
        a->setEnabled(QFileInfo::exists(path));
    }
    m_recentMenu->setEnabled(!recent.isEmpty());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Laufender Export würde beim Beenden abgebrochen und die halbe Datei gelöscht -> erst fragen
    if (Exporter::anyRunning() && !m_autosaveDisabled
        && QMessageBox::question(this, "schneidi", T("Es wird gerade gerendert. Beenden bricht das Rendern ab. Trotzdem beenden?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    removeAutosave(); // normal beendet -> nichts wiederherzustellen
    event->accept();
}
