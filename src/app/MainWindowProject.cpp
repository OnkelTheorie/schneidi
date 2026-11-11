// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: project file (new, open, save, autosave, backups, recent files, presets).
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "app/KeyBindingsDialog.h"
#include "app/LaunchArgs.h"
#include "app/ClipSpeedDialog.h"
#include "app/DesignDialog.h"
#include "app/Log.h"
#include "app/NormalizeDialog.h"
#include "app/OpenTrace.h"
#include "app/PasteAttributesDialog.h"
#include "core/Loudness.h"
#include "app/ProjectSettingsDialog.h"
#include "app/StartTemplatesPage.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/I18n.h"
#include "core/Timecode.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/StartTemplates.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/MediaCache.h"
#include "engine/ClipFormat.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "ui/TrustDialog.h"
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
#include <QDateTime>
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
#include <QStatusBar>
#include "app/TourManager.h"
#include <QTimer>

// ---- Project file ---------------------------------------------------------------

namespace {
// A function, not a global: a global would be built before the dictionary is loaded (always German)
QString fileFilter() { return T("schneidi-Projekt (*.%1)").arg(ProjectFile::Extension); }

QDir autosaveDir() { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)); }
QString autosaveLockPath(int slot) { return autosaveDir().filePath(QString("autosave-%1.lock").arg(slot)); }
QString autosaveFile(int slot)
{
    const QString name = slot == 1 ? QString("autosave") : QString("autosave-%1").arg(slot);
    return autosaveDir().filePath(QString("%1.%2").arg(name, ProjectFile::Extension));
}
} // namespace

QString MainWindow::autosavePath()
{
    if (m_autosaveSlot == 0) {
        autosaveDir().mkpath(".");
        // occupy the first free slot; a crashed instance leaves an orphaned lock, which QLockFile detects
        for (int slot = 1; slot <= 9 && m_autosaveSlot == 0; ++slot) {
            auto lock = std::make_unique<QLockFile>(autosaveLockPath(slot));
            if (lock->tryLock(0)) {
                m_autosaveSlot = slot;
                m_autosaveLock = std::move(lock);
            }
        }
        if (m_autosaveSlot == 0) m_autosaveSlot = 10; // all taken: own slot without lock
    }
    return autosaveFile(m_autosaveSlot);
}

bool MainWindow::adoptOrphanedAutosave()
{
    // Each window writes into its own slot: the backup of a window that crashed while another one had slot 1 lies in
    // a later slot, which a single window never looks into. A slot whose lock is free (no running window) and that
    // holds a backup becomes this window's slot, so restoring, later backups and removing it all go there.
    autosavePath(); // own slot first: a slot other windows already hold stays theirs
    for (int slot = 1; slot <= 9; ++slot) {
        if (slot == m_autosaveSlot || !QFileInfo::exists(autosaveFile(slot))) continue;
        auto lock = std::make_unique<QLockFile>(autosaveLockPath(slot));
        if (!lock->tryLock(0)) continue; // a running window's
        m_autosaveSlot = slot;
        m_autosaveLock = std::move(lock); // the previous slot's lock is released
        return true;
    }
    return false;
}

QString MainWindow::autosaveKey() const
{
    return m_autosaveSlot <= 1 ? QString("autosave/projectPath") : QString("autosave/projectPath-%1").arg(m_autosaveSlot);
}

void MainWindow::updateTitle()
{
    const QString name = m_projectPath.isEmpty() ? T("Unbenannt") : QFileInfo(m_projectPath).completeBaseName();
    const QString shown = name + (m_project->isModified() ? " *" : "");
    setWindowTitle(shown + " – " + appName());
    if (m_titleLabel) m_titleLabel->setText(shown);
}

void MainWindow::setProjectPath(const QString& path)
{
    m_projectPath = path;
    m_saveSuggestion.clear();
    if (!path.isEmpty() && !m_autosaveDisabled) addRecent(path); // test run: do not touch the user's list
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
    data.views = savedViews(); // and see what they saw: a change from outside never moves the zoom (core rule)
    applyLoaded(std::move(data), m_projectPath, false);
}

// Restart now or later; "now" asks about saving like on quit and reopens the project afterwards
void MainWindow::offerRestart(const QString& message, const QString& now, const QString& later)
{
    QMessageBox box(QMessageBox::Question, "schneidi", message, QMessageBox::NoButton, this);
    QPushButton* nowButton = box.addButton(now, QMessageBox::AcceptRole);
    box.setDefaultButton(box.addButton(later, QMessageBox::RejectRole));
    box.exec();
    if (box.clickedButton() != nowButton) return;
    m_restartRequested = true;
    if (!close()) m_restartRequested = false; // Saving canceled -> keep working
}

QStringList MainWindow::restartArguments() const
{
    return LaunchArgs::restart(m_projectPath);
}

bool MainWindow::maybeSave()
{
    if (!m_project->isModified() || m_autosaveDisabled) return true; // test run: never ask
    const auto answer = QMessageBox::question(this, "schneidi", T("Änderungen am Projekt speichern?"),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Save);
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

bool MainWindow::maybeLeaveProject()
{
    if (m_tourSaved.active && m_tours) m_tours->stop(); // the user's project back first: that is the one being left
    endVoiceover();
    RenderQueue* queue = m_deliver->renderQueue();
    if (queue->isRunning() && !m_autosaveDisabled) {
        if (QMessageBox::question(this, "schneidi",
                                  T("Es wird gerade gerendert. Ein anderes Projekt bricht das Rendern ab. Trotzdem fortfahren?"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return false;
        queue->cancel(); // synchronous: status still lands in this project
    }
    return maybeSave();
}

void MainWindow::newProject()
{
    // "Save changes?" first: cancelled there, nobody asks for a template
    if (!maybeLeaveProject()) return;
    StartTemplate t = StartTemplates::defaultTemplate();
    if (StartTemplates::askOnNew()) { // only when the user chose to be asked (Settings › Start Templates)
        ChooseStartTemplateDialog dlg(StartTemplates::all(), t.id, this);
        if (dlg.exec() != QDialog::Accepted) return;
        t = dlg.chosen();
    }
    startProject(t);
}

void MainWindow::newProjectFromTemplate(const StartTemplate& t)
{
    if (!maybeLeaveProject()) return;
    startProject(t);
}

void MainWindow::startProject(const StartTemplate& t)
{
    m_engine->pause();
    m_selection->clear();
    if (m_colorPanel) m_colorPanel->discardTracking();
    m_chatSeen.reset(); // the chat's AI has not seen this project: no "the user changed" against the old one
    m_templateDir.clear();
    m_templateCopies.clear();
    const ProjectData d = StartTemplates::apply(t); // Neutral: the empty project as always
    m_engine->setFormat(d.format); // media lengths already fit (empty) -> do not re-read
    m_project->load(d);
    m_sourcePos.clear();
    m_engine->showTimeline(0);
    setProjectPath({});
    removeAutosave();
}

QString MainWindow::saveStartTemplate(QWidget* parent, bool* madeDefault)
{
    if (madeDefault) *madeDefault = false;
    if (!parent) parent = this;
    // Suggested name: the project's
    const QString suggestion = m_projectPath.isEmpty() ? QString() : QFileInfo(m_projectPath).completeBaseName();
    StartTemplate t = StartTemplates::fromProject(m_project->data(), suggestion);
    SaveStartTemplateDialog dlg(t, parent);
    if (dlg.exec() != QDialog::Accepted) return {};
    t.name = dlg.name();
    t.description = dlg.description();
    // Names that become the same file ("Shorts 9:16" / "Shorts 9_16"): the question names the one that is replaced
    if (const std::optional<StartTemplate> old = StartTemplates::sameFile(t.name)) {
        const QString question = old->name == t.name
                                     ? T("Eine Vorlage „%1“ gibt es schon. Ersetzen?").arg(t.name)
                                     : T("„%1“ ersetzt die Vorlage „%2“ (gleicher Dateiname). Ersetzen?").arg(t.name, old->name);
        if (QMessageBox::question(parent, T("Als Startvorlage speichern"), question) != QMessageBox::Yes) return {};
    }
    QString error;
    if (!StartTemplates::save(t, &error)) {
        QMessageBox::warning(parent, T("Als Startvorlage speichern"),
                             T("%1 konnte nicht gespeichert werden:\n%2").arg(QDir::toNativeSeparators(t.path), error));
        return {};
    }
    if (dlg.makeDefault()) {
        if (!m_autosaveDisabled) StartTemplates::setDefaultId(t.id); // test runs leave the user's settings alone
        if (madeDefault) *madeDefault = true;
    }
    statusBar()->showMessage(T("Startvorlage „%1“ gespeichert").arg(t.name), 5000);
    return t.id;
}

void MainWindow::rebuildTemplateMenu()
{
    if (!m_templateMenu) return;
    m_templateMenu->clear();
    const QString def = StartTemplates::defaultTemplate().id;
    bool own = false;
    for (const StartTemplate& t : StartTemplates::all()) {
        if (!t.builtIn && !own) {
            m_templateMenu->addSeparator();
            own = true;
        }
        QString text = t.displayName().replace('&', "&&");
        if (t.id == def) text = T("%1 (Standard)").arg(text);
        QAction* a = m_templateMenu->addAction(text, this, [this, t] { newProjectFromTemplate(t); });
        a->setToolTip(t.displayDescription());
    }
    m_templateMenu->addSeparator();
    m_templateMenu->addAction(T("Startvorlagen verwalten…"), this, [this] { settingsDialog(2); });
}

void MainWindow::openProjectDialog()
{
    if (!maybeLeaveProject()) return;
    const QString dir = m_projectPath.isEmpty() ? QDir::homePath() : QFileInfo(m_projectPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, T("Projekt öffnen"), dir, fileFilter());
    if (!path.isEmpty()) openProject(path);
}

void MainWindow::openMediaAsProjectDialog()
{
    if (!maybeLeaveProject()) return;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, T("Video als Projekt öffnen"), QDir::homePath(),
        T("Medien (*.mp4 *.mov *.mkv *.avi *.webm *.mts *.m4v *.mp3 *.wav *.flac *.ogg *.m4a *.aac "
          "*.png *.jpg *.jpeg *.webp *.bmp *.srt);;Alle Dateien (*)"));
    if (!files.isEmpty()) openMediaAsProject(files);
}

void MainWindow::openMediaAsProject(const QStringList& files)
{
    // Neutral, not the default start template: the clip decides the format, not a template's fixed 9:16
    startProject(StartTemplates::find(StartTemplates::NeutralId).value_or(StartTemplates::builtIn().value(0)));
    QStringList paths;
    for (const QString& f : files) paths << QFileInfo(f).absoluteFilePath();
    m_mediaPool->importFiles(paths);
    QStringList placed; // readable files (folders came in as bins, subtitles as subtitle tracks)
    for (const QString& p : paths)
        if (m_project->mediaInfo(p)) placed << p;
    if (placed.isEmpty()) return;
    offerClipFormat(placed, false);
    m_editor->addMediaAt(placed, 0);
    // That is where the project starts: undo does not take the clips off again; unsaved, Save As suggests the
    // video's folder and name
    m_project->undoStack()->clear();
    m_project->markModified();
    const QFileInfo first(placed.first());
    m_saveSuggestion = first.dir().filePath(first.completeBaseName() + "." + ProjectFile::Extension);
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
                                 T("Nicht im Preset (Seite Farbe bzw. Plugin fehlt): %1").arg(skipped.join(", ")));
}

bool MainWindow::openProject(const QString& path)
{
    OpenTrace::begin();
    ProjectData data;
    QString error, warning;
    if (!ProjectFile::load(path, &data, &error, &warning)) {
        QMessageBox::warning(this, T("Projekt öffnen"), T("%1 konnte nicht geöffnet werden:\n%2").arg(path, error));
        return false;
    }
    OpenTrace::mark("file parsed");
    if (OpenTrace::enabled()) {
        auto* watch = new OpenTrace::Watch(m_timeline->view(), [this, path] {
            openProject(qEnvironmentVariable("SCHNEIDI_OPEN_TRACE_NEXT", path));
        });
        connect(m_engine, &Engine::frameReady, watch, [](const QImage&) { OpenTrace::Watch::frameEmitted(); },
                Qt::DirectConnection);
        connect(m_engine, &Engine::frameReady, watch,
                [watch](const QImage& img) { watch->frameDelivered(OpenTrace::hasContent(img)); }, Qt::QueuedConnection);
    }
    if (!applyLoaded(std::move(data), path)) return false;
    removeAutosave();
    showLoadWarning(warning);
    OpenTrace::mark("openProject returned");
    return true;
}

void MainWindow::showLoadWarning(const QString& warning)
{
    if (warning.isEmpty()) return;
    // Not modal: an AI that opened the project goes on reading, the user reads it when they look
    auto* box = new QMessageBox(QMessageBox::Warning, T("Projekt öffnen"), warning, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}

bool MainWindow::applyLoaded(ProjectData data, const QString& path, bool ask)
{
    // Missing media like DaVinci "Media Offline": have the folder searched or leave offline
    int relinked = 0;
    for (QStringList missing = ask ? ProjectFile::missingMedia(data) : QStringList{}; !missing.isEmpty();
         missing = ProjectFile::missingMedia(data)) {
        // Files only the Media Pool still lists (no timeline reads them) cost nothing: said, so nobody hunts for them
        const QStringList unused = ProjectFile::unusedMedia(data);
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
        QStringList names;
        for (const QString& p : missing.mid(0, 15))
            names << (!allUnused && unused.contains(p) ? T("%1 (in keiner Timeline benutzt)").arg(p) : p);
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

    // Effects this version does not have (frei0r plugins of older projects): stay in the project, skipped
    if (const QStringList fx = ask ? ProjectFile::missingEffects(data) : QStringList{}; !fx.isEmpty()) {
        QMessageBox box(QMessageBox::Warning, T("Effekte fehlen"),
                        T("%1 Effekt(e) sind nicht verfügbar und werden übersprungen:").arg(fx.size()),
                        QMessageBox::Ok, this);
        box.setInformativeText(fx.mid(0, 15).join('\n') + (fx.size() > 15 ? "\n…" : "") + "\n\n"
                               + T("Sie bleiben mit ihren Werten im Projekt; der Inspektor zeigt sie an."));
        box.exec();
    }

    // Custom code from someone else (filter chains, shaders; core/CodeTrust.h): run it or keep it skipped
    if (ask) TrustDialog::askForProject(this, data, path);

    m_templateDir.clear(); // a project of its own (New from template sets it again afterwards)
    m_templateCopies.clear();
    m_engine->pause();
    m_selection->clear();
    if (m_colorPanel) m_colorPanel->discardTracking();
    m_chatSeen.reset();
    m_probeCache.clear();
    OpenTrace::mark("missing media/effects checked");
    m_engine->setFormat(data.format); // media lengths in the file already count at this frame rate
    OpenTrace::mark("engine format set");
    // The timeline shown after loading is set up below (restoreViews, the engine): the switch to another timeline
    // (currentSequenceChanged) has nothing to keep of the old project's and must not seek the engine, which still
    // has the old timeline - it drew the old project's picture once more before the new one
    m_shownSequence = data.currentSequence;
    m_project->load(data);
    OpenTrace::mark("project loaded (signals handled)");
    if (relinked > 0) m_project->markModified(); // new paths are only in the file after saving
    m_sourcePos.clear();
    // The new timeline reaches the picture engine now, not after the edit debounce (m_engineTimer): its first frame
    // renders while the window paints, instead of the old project's picture being shown first
    m_engineTimer.stop();
    m_engine->updateTimeline(previewTimeline());
    OpenTrace::timelineSent();
    updateRenderCacheBar();
    m_engine->showTimeline(data.playhead);
    OpenTrace::mark("engine showTimeline");
    m_timeline->view()->setPlayhead(data.playhead);
    restoreViews(data);
    setProjectPath(path);
    OpenTrace::mark("views restored, path set");
    return true;
}

QVector<SavedView> MainWindow::savedViews() const
{
    QVector<SavedView> out;
    const TimelineView* tv = m_timeline->view();
    const auto add = [&](int seq, const ViewState& v, int playhead) {
        out << SavedView{seq, v.pxPerFrame, v.leftFrame, v.scrollY, playhead};
    };
    add(m_project->currentSequence(), tv->view(), tv->playhead());
    for (auto it = m_sequenceView.cbegin(); it != m_sequenceView.cend(); ++it)
        if (it.key() != m_project->currentSequence() && m_project->sequence(it.key()))
            add(it.key(), it.value(), m_sequencePos.value(it.key(), 0));
    return out;
}

void MainWindow::restoreViews(const ProjectData& data)
{
    // The timelines as they were shown when saved (zoom, scroll, playhead); a file without that (older, made by the
    // CLI): the whole timeline in view - opening is the user's step, edits later never touch the zoom
    m_sequenceView.clear();
    m_sequencePos.clear();
    m_shownSequence = m_project->currentSequence();
    TimelineView* tv = m_timeline->view();
    bool shown = false;
    for (const SavedView& v : data.views) {
        ViewState s = tv->view();
        s.pxPerFrame = v.pxPerFrame;
        s.leftFrame = v.leftFrame;
        s.scrollY = v.scrollY;
        if (v.sequence == m_shownSequence) { // after the layout too: the scroll range needs the view's real height
            QTimer::singleShot(0, tv, [tv, s] { tv->setZoomAndScroll(s.pxPerFrame, s.leftFrame, s.scrollY); });
            shown = true;
        } else {
            m_sequenceView[v.sequence] = s;
            m_sequencePos[v.sequence] = v.playhead;
        }
    }
    if (!shown) // after the layout: the view's width is known then
        QTimer::singleShot(0, tv, [tv] { tv->zoomToFit(); });
}

// Load a backup (like DaVinci "Project Backups"): opens as an untitled project so that it is never
// accidentally saved into the backup folder; saving asks for the target
void MainWindow::openBackupDialog()
{
    if (!maybeLeaveProject()) return;
    QString dir = ProjectFile::backupDir(m_projectPath);
    if (!QFileInfo::exists(dir)) dir = ProjectFile::backupDir({});
    QDir().mkpath(dir);
    const QString path = QFileDialog::getOpenFileName(this, T("Sicherungskopie öffnen"), dir, fileFilter());
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
    if (m_tourSaved.active) { // the practice project of a tour is never saved (the user's comes back after it)
        statusBar()->showMessage(T("Das Übungsprojekt eines Rundgangs wird nicht gespeichert."), 5000);
        return false;
    }
    return m_projectPath.isEmpty() ? saveAs() : saveTo(m_projectPath);
}

bool MainWindow::saveAs()
{
    if (m_tourSaved.active) return save(); // says why not
    QString suggestion = !m_projectPath.isEmpty()     ? m_projectPath
                         : !m_saveSuggestion.isEmpty() ? m_saveSuggestion
                                                       : QDir::home().filePath(T("Unbenannt.%1").arg(ProjectFile::Extension));
    // A new project and the user chose a folder of their own for projects (Settings): its name there
    if (m_projectPath.isEmpty() && SettingsDialog::storedOwnFolder()) {
        const QString folder = SettingsDialog::storedFolder();
        QDir().mkpath(folder);
        suggestion = QDir(folder).filePath(QFileInfo(suggestion).fileName());
    }
    QString path = QFileDialog::getSaveFileName(this, T("Projekt speichern unter"), suggestion, fileFilter());
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix() != ProjectFile::Extension) path += QString(".%1").arg(ProjectFile::Extension);
    return saveTo(path);
}

bool MainWindow::saveTo(const QString& path)
{
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    data.views = savedViews();
    QString error;
    // schneidi-cli may be writing the same file right now: wait for it (it takes milliseconds)
    const std::unique_ptr<QLockFile> lock = ProjectFile::lock(path);
    if (!lock) {
        QMessageBox::warning(this, T("Projekt speichern"),
                             T("Die Projektdatei wird gerade von einem anderen Programm gespeichert. Bitte noch einmal versuchen."));
        return false;
    }
    QStringList copies;
    if (!copyTemplateFiles(data, path, &copies)) return false;
    if (!m_autosaveDisabled) ProjectFile::backup(path); // keep the previous state (test run: store nothing)
    if (!ProjectFile::save(data, path, &error)) {
        for (const QString& f : std::as_const(copies)) QFile::remove(f);
        for (auto it = m_templateCopies.begin(); it != m_templateCopies.end();)
            it = copies.contains(it.value()) ? m_templateCopies.erase(it) : std::next(it);
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
    if (!m_project->isModified() || m_tourSaved.active) return; // a tour's practice project: the user's backup stays
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    data.views = savedViews();
    QDir().mkpath(QFileInfo(autosavePath()).absolutePath());
    if (const QString path = autosavePath(); ProjectFile::save(data, path, nullptr))
        QSettings().setValue(autosaveKey(), m_projectPath); // where the backup belongs
}

void MainWindow::disableAutosave()
{
    m_autosaveTimer->stop();
    m_autosaveDisabled = true;
    m_mediaPool->setSaveSettings(false); // test runs: do not save the Media Pool view
    m_storage->setPersistent(false);
}

void MainWindow::removeAutosave()
{
    if (m_autosaveDisabled) return; // test run: do not touch the user's real backup

    QFile::remove(autosavePath());
    QSettings().remove(autosaveKey());
}

bool MainWindow::offerAutosaveRestore()
{
    if (m_autosaveDisabled) return false;
    if (!QFileInfo::exists(autosavePath()) && !adoptOrphanedAutosave()) return false;
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
    QString error;
    if (!ProjectFile::load(autosavePath(), &data, &error)) {
        // Broken (cut off by a crash while writing, a full disk): moved aside, never offered again; kept, not deleted
        // - the user may want to look into it
        const QFileInfo fi(autosavePath());
        const QString aside = fi.dir().filePath(QString("%1-broken-%2.%3")
                                                    .arg(fi.completeBaseName(),
                                                         QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"),
                                                         ProjectFile::Extension));
        const bool moved = QFile::rename(fi.filePath(), aside);
        if (!moved) QFile::remove(fi.filePath());
        QSettings().remove(autosaveKey());
        QString text = T("Die automatische Sicherung konnte nicht geladen werden:\n%1").arg(error);
        if (moved) text += "\n\n" + T("Sie liegt jetzt unter %1 und wird nicht mehr angeboten.").arg(QDir::toNativeSeparators(aside));
        QMessageBox::warning(this, T("Wiederherstellen"), text);
        return false;
    }
    if (!applyLoaded(std::move(data), original)) return false;
    m_project->markModified(); // restored, but not yet saved into the project file
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
    // A running export would be aborted on quit and the half file deleted -> ask first
    if (Exporter::anyRunning() && !m_autosaveDisabled
        && QMessageBox::question(this, "schneidi", T("Es wird gerade gerendert. Beenden bricht das Rendern ab. Trotzdem beenden?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    if (m_tourSaved.active && m_tours) m_tours->stop(); // the user's project back: asked about, not the practice one
    endVoiceover();
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    removeAutosave(); // quit normally -> nothing to restore
    // The preview on the second monitor goes with the main window (its window is a top-level of its own)
    if (m_output) showPreviewOn(nullptr);
    event->accept();
}
