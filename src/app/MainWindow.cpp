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
#include <QFileSystemWatcher>
#include <QFile>
#include <QLockFile>
#include <QSaveFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QInputDialog>
#include <QStandardPaths>
#include <QTimer>

MainWindow::MainWindow(Engine* engine, QWidget* parent) : QMainWindow(parent), m_engine(engine)
{
    setWindowTitle("schneidi");
    resize(1600, 950);

    m_project = new Project(this);
    m_selection = new Selection(this);
    m_editor = new Editor(m_project, m_selection, this);

    buildLayout();
    buildActions();
    qApp->installEventFilter(this); // Maus-Seitentasten -> Aktionen (keybindings.json, Abschnitt "mouse")

    // Modell -> Engine
    // Gebündelt: beim Ziehen (Fade-Griff, Lautstärkelinie, Inspector …) ändert jede Mausbewegung das Modell;
    // die Vorschau jedes Mal sofort neu zu bauen blockierte die Timeline (Griff hing hinterher).
    // Mixer-Änderungen sind billig (live gesetzt) und gehen weiter sofort durch.
    m_engineTimer.setSingleShot(true);
    m_engineTimer.setInterval(30);
    connect(&m_engineTimer, &QTimer::timeout, this, [this] {
        m_engine->updateTimeline(m_project->renderTimeline());
        updateRenderCacheBar();
    });
    connect(m_editor, &Editor::mixerOnlyEdit, m_engine, &Engine::mixerOnlyNext);
    connect(m_project, &Project::timelineChanged, this, [this] {
        if (m_engine->mixerOnlyPending()) m_engine->updateTimeline(m_project->renderTimeline());
        else if (!m_engineTimer.isActive()) m_engineTimer.start();
    });
    m_engine->updateTimeline(m_project->renderTimeline());
    // Proxy fertig/gelöscht oder "Proxy-Medien verwenden" umgeschaltet -> Vorschau neu aufbauen
    ProxyManager* proxies = m_engine->proxies();
    auto proxiesChanged = [this] { m_engine->updateTimeline(m_project->renderTimeline()); };
    connect(proxies, &ProxyManager::proxyChanged, this, [this, proxiesChanged](const QString& path) {
        // auch Clips in Compound Clips (verschachtelte Sequenzen)
        const auto& seqs = m_project->sequences();
        if (std::any_of(seqs.begin(), seqs.end(),
                        [&](const Sequence& s) { return TimelineOps::usesMediaOnVideo(s.timeline, path); }))
            proxiesChanged();
    });
    connect(proxies, &ProxyManager::enabledChanged, this, proxiesChanged);
    // Vorher/Nachher: gecachte Clips enthalten die Korrektur -> Vorschau neu aufbauen, damit sie aus dem Original kommen
    connect(m_engine, &Engine::colorBypassChanged, this, [this] {
        if (m_engine->renderCache()->mode() != RenderCache::Mode::Off) m_engine->updateTimeline(m_project->renderTimeline());
    });
    // Render-Cache: Datei fertig/gelöscht oder Modus geändert -> Vorschau neu aufbauen, Balken über der Timeline
    RenderCache* renderCache = m_engine->renderCache();
    connect(renderCache, &RenderCache::cacheChanged, this, [this, proxiesChanged] {
        proxiesChanged();
        updateRenderCacheBar();
    });
    connect(renderCache, &RenderCache::progressChanged, this, &MainWindow::updateRenderCacheBar);
    connect(renderCache, &RenderCache::failed, this,
            [this](const QString& msg) { QMessageBox::warning(this, T("Render-Cache"), msg); });

    // Engine -> Playhead (nur im Timeline-Modus; im Quellmodus bleibt der Playhead stehen)
    TimelineView* tv = m_timeline->view();
    connect(m_engine, &Engine::positionChanged, this, [this, tv](int f) {
        if (m_engine->mode() == Engine::Mode::Timeline) tv->setPlayhead(f);
    });
    connect(tv, &TimelineView::seekRequested, this, [this](int f) {
        if (m_engine->mode() != Engine::Mode::Timeline) m_engine->showTimeline(f);
        else m_engine->seek(f);
    });
    // Keyframes: Inspector zeigt Werte am Playhead, Pfeile ◀ ▶ springen wie ein Klick ins Lineal
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (m_engine->mode() == Engine::Mode::Timeline) m_inspector->setPlayhead(f);
    });
    connect(m_inspector, &Inspector::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_inspector, &Inspector::savePresetRequested, this, &MainWindow::saveEffectPreset);
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (m_engine->mode() == Engine::Mode::Timeline) m_colorPanel->setPlayhead(f);
    });
    connect(m_colorPanel, &ColorPanel::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_engine, &Engine::frameReady, m_scopes, &ScopesPanel::setFrame);
    connect(m_mediaPool, &MediaPool::sourceRequested, this, &MainWindow::showSource);
    connect(m_mediaPool, &MediaPool::sequenceOpenRequested, m_timeline, &TimelinePanel::openSequence);
    // Andere Timeline geöffnet: Auswahl weg, Playhead und Zoom/Scroll wie zuletzt in dieser Timeline (wie DaVinci)
    m_shownSequence = m_project->currentSequence();
    connect(m_project, &Project::currentSequenceChanged, this, [this, tv] {
        const int id = m_project->currentSequence();
        if (id == m_shownSequence) return;
        m_sequencePos[m_shownSequence] = tv->playhead();
        m_sequenceView[m_shownSequence] = tv->view();
        m_shownSequence = id;
        // Zoom/Scroll dieser Timeline wiederherstellen; noch nie gezeigt: Zoom behalten, vorn und oben anfangen
        const ViewState v = m_sequenceView.value(id, [&] {
            ViewState d = tv->view();
            d.leftFrame = 0;
            d.scrollY = 0;
            return d;
        }());
        tv->setZoomAndScroll(v.pxPerFrame, v.leftFrame, v.scrollY);
        m_selection->clear();
        m_engine->pause();
        const int pos = m_sequencePos.value(id, 0);
        if (m_engine->mode() != Engine::Mode::Timeline) m_engine->showTimeline(pos);
        else m_engine->seek(pos);
        tv->setPlayhead(pos);
    });
    connect(tv, &TimelineView::dropRequested, this, &MainWindow::onDrop);
    // Trim view in the viewer while an edit is dragged (like DaVinci)
    connect(tv, &TimelineView::trimFramesChanged, m_viewer, &Viewer::showTrim);
    connect(tv, &TimelineView::trimFramesEnded, m_viewer, &Viewer::endTrim);
    connect(m_mediaPool, &MediaPool::subtitleFilesImported, this, &MainWindow::importSubtitleFiles);
    connect(tv, &TimelineView::subtitleEditRequested, this, [this](int id) {
        m_inspectorToggle->setChecked(true);
        m_inspector->editSubtitle(id);
    });
    // Rechtsklick auf Clips: die passenden Aktionen (Tastenkürzel wie im Menü)
    connect(tv, &TimelineView::clipMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        const bool audio = !m_editor->selectedAudioClips().isEmpty();
        const int cacheState = m_editor->selectionRenderCacheState();
        const bool compound = m_editor->selectedCompoundSequence() != 0;
        {
            QAction* r = InputBindings::instance().action("retime_controls");
            const QVector<int> ids = m_editor->clipIdsOf(m_selection->ids());
            TimelineView* tv = m_timeline->view();
            r->setEnabled(std::any_of(ids.begin(), ids.end(), [this](int id) { return m_editor->canRetime(id); }));
            r->setChecked(std::any_of(ids.begin(), ids.end(), [tv](int id) { return tv->retimeControlsShown(id); }));
            QAction* k = InputBindings::instance().action("curve_editor");
            k->setEnabled(std::any_of(ids.begin(), ids.end(), [this](int id) {
                const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
                return c && Keys::hasKeys(*c);
            }));
            k->setChecked(std::any_of(ids.begin(), ids.end(), [tv](int id) { return tv->curveEditorShown(id); }));
        }
        for (const char* id : {"clip_speed", "retime_controls", "clip_speed_reset", "curve_editor", "normalize_audio", "",
                               "toggle_enabled", "link_clips",
                               "render_cache_clip", "save_effect_preset", "", "compound_create", "compound_open", "compound_decompose", "",
                               "delete", "ripple_delete"}) {
            if (!*id) {
                menu.addSeparator();
                continue;
            }
            QAction* a = InputBindings::instance().action(id);
            if (!a) continue;
            if (QString(id) == "normalize_audio" && !audio) continue;
            if ((QString(id) == "compound_open" || QString(id) == "compound_decompose") && !compound) continue;
            if (QString(id) == "save_effect_preset" && !presetClip()) continue;
            if (QString(id) == "render_cache_clip") {
                if (cacheState < 0) continue; // nur Videoclips (keine Titel)
                a->setChecked(cacheState == 1);
            }
            menu.addAction(a);
        }
        // Clipfarbe/Flags wie DaVinci: gelten für den Media-Pool-Clip (Titel haben keinen)
        QStringList paths;
        for (int cid : m_editor->selection()->ids())
            if (const Clip* c = TimelineOps::findClip(m_project->timeline(), cid); c && !c->isTitle()
                && !paths.contains(c->mediaPath))
                paths << c->mediaPath;
        if (!paths.isEmpty()) {
            menu.addSeparator();
            MediaPool::addClipColorMenu(&menu, m_project, paths);
            MediaPool::addFlagsMenu(&menu, m_project, paths);
        }
        menu.exec(pos);
    });
    // Quellbereich aus dem Viewer in die Timeline gezogen: dort überschreiben (wie DaVinci)
    connect(tv, &TimelineView::rangeDropRequested, this, [this](const QString& path, int in, int out, int frame, int track) {
        if (m_project->frameRateLocked()) {
            m_editor->placeSourceRange(path, in, out, frame, track);
            return;
        }
        QTimer::singleShot(0, this, [this, path, in, out, frame, track] { // Rückfrage erst nach dem Drop (s. onDrop)
            offerClipFormat({path}, true);
            m_editor->placeSourceRange(path, in, out, frame, track);
        });
    });
    // Viewer: Scrubber (Länge, In/Out) und Titel je nach Quelle/Timeline
    connect(m_engine, &Engine::modeChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::timelineChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::mediaMarksChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::mediaChanged, this, &MainWindow::updateViewer);
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (sourceActive()) m_sourcePos[m_engine->sourcePath()] = f;
    });
    updateViewer();
    connect(m_storage, &MediaStorage::importRequested, m_mediaPool, &MediaPool::importFiles);
    connect(m_storage, &MediaStorage::folderImportRequested, m_mediaPool, &MediaPool::importFolder);
    // Effects Library: Doppelklick = wie Strg+T bzw. "Titel einfügen", nur mit der gewählten Art
    connect(m_effects, &EffectsLibrary::transitionRequested, this, [this, tv](TrackKind kind, const TransitionStyle& style) {
        m_editor->addTransitions(tv->playhead(), style, kind);
    });
    connect(m_effects, &EffectsLibrary::titleRequested, this, [this, tv] { m_editor->addTitle(tv->playhead()); });
    // Filter: auf die ausgewählten Videoclips, ohne Auswahl auf den obersten Clip am Playhead
    connect(m_effects, &EffectsLibrary::effectRequested, this, [this, tv](const QString& id) {
        m_editor->addEffect(m_editor->effectTargets(tv->playhead()), id);
    });
    tv->setProbe([this](const QString& path) { return probeCached(path); });
    m_mediaCache = new MediaCache(this);
    tv->setMediaCache(m_mediaCache);
    // Projekteinstellungen -> Engine (Profil neu), Cache, Inspector; kommt vor timelineChanged
    connect(m_project, &Project::formatChanged, this, &MainWindow::onFormatChanged);

    connect(m_project, &Project::modifiedChanged, this, &MainWindow::updateTitle);
    updateTitle();
    // Automatische Sicherung jede Minute (nur bei Änderungen), getrennt von der Projektdatei
    m_autosaveTimer = new QTimer(this);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    m_autosaveTimer->start(60 * 1000);
    // Outside changes of the project file (schneidi-cli): QSaveFile replaces the file, so the watcher loses it
    // and checkProjectFile() adds it again
    m_projectWatcher = new QFileSystemWatcher(this);
    m_projectCheck.setSingleShot(true);
    m_projectCheck.setInterval(300);
    connect(&m_projectCheck, &QTimer::timeout, this, &MainWindow::checkProjectFile);
    connect(m_projectWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_projectCheck.start(); });

}

void MainWindow::importFiles(const QStringList& paths, bool placeOnTimeline, bool adoptFormat)
{
    m_mediaPool->importFiles(paths);
    if (!placeOnTimeline) return;
    // Testhilfe (--demo): Dateien hintereinander auf V1/A1 legen; Projektformat ohne Nachfrage vom ersten Clip
    if (adoptFormat) offerClipFormat(paths, false);
    m_editor->addMediaAt(paths, TimelineOps::endFrame(m_project->timeline()));
}

MediaInfo MainWindow::probeCached(const QString& path)
{
    auto it = m_probeCache.find(path);
    if (it == m_probeCache.end()) it = m_probeCache.insert(path, m_engine->probe(path));
    return *it;
}

void MainWindow::onDrop(const QStringList& paths, int frame, int track)
{
    if (paths == QStringList{MediaPool::TitleItem}) {
        m_editor->addTitle(frame, track);
        return;
    }
    if (paths.size() == 1 && MediaPool::sequenceOfItem(paths.first())) { // Timeline/Compound Clip verschachteln
        m_editor->addSequenceAt(MediaPool::sequenceOfItem(paths.first()), frame, track);
        return;
    }
    // Untertiteldateien werden Untertitelspuren (Zeiten aus der Datei, wie DaVinci)
    QStringList media, subtitles;
    for (const QString& p : paths)
        (QFileInfo(p).suffix().compare("srt", Qt::CaseInsensitive) == 0 ? subtitles : media) << p;
    if (!subtitles.isEmpty()) {
        QTimer::singleShot(0, this, [this, subtitles] { importSubtitleFiles(subtitles); }); // Meldungen nicht im Drop
        if (media.isEmpty()) return;
        onDrop(media, frame, track);
        return;
    }
    // Direkt aus dem Dateimanager: erst in den Media Pool, dann auf die Timeline (wie DaVinci)
    QStringList unknown;
    for (const QString& p : paths)
        if (!m_project->mediaInfo(p)) unknown << p;
    if (!unknown.isEmpty()) m_mediaPool->importFiles(unknown);
    if (m_project->frameRateLocked()) {
        m_editor->addMediaAt(paths, frame, track);
        return;
    }
    // Leere Timeline: evtl. Rückfrage zum Projektformat – erst nach dem Drop (keine modale Abfrage mitten
    // im Drag & Drop, die Quelle, z. B. der Dateimanager, wartet sonst)
    QTimer::singleShot(0, this, [this, paths, frame, track] {
        offerClipFormat(paths, true);
        m_editor->addMediaAt(paths, frame, track);
    });
}

void MainWindow::offerClipFormat(const QStringList& paths, bool ask)
{
    if (m_project->frameRateLocked()) return; // nur in leerer Timeline
    // Der erste Clip mit Bild entscheidet (Standbilder und Ton haben kein eigenes Format)
    for (const QString& path : paths) {
        const MediaInfo* info = m_project->mediaInfo(path);
        if (!info || !info->hasVideo || info->isImage) continue;
        const ClipFormat cf = detectClipFormat(path);
        if (!cf.ok) return;
        const ProjectFormat cur = m_project->format();
        ProjectFormat want;
        want.width = cf.width;
        want.height = cf.height;
        want.rate = cf.suggested;
        if (want == cur) return;
        if (ask) {
            // Wie DaVinci "Change project frame rate?", hier samt Auflösung
            QString clipRate = cf.rate.label();
            if (cf.variable)
                clipRate = T("variabel, etwa %1").arg(QString::number(cf.averageFps, 'f', 2).replace('.', I18n::language() == "de" ? "," : "."));
            QMessageBox box(QMessageBox::Question, T("Projekteinstellungen"),
                            T("Das Format des Clips passt nicht zu den Projekteinstellungen. Projekt an den Clip anpassen?"),
                            QMessageBox::NoButton, this);
            box.setInformativeText(T("Clip: %1, %2 fps\nProjekt: %3, %4 fps\n\nNeu: %5, %6 fps")
                                       .arg(resolutionLabel(cf.width, cf.height), clipRate,
                                            resolutionLabel(cur.width, cur.height), cur.rate.label(),
                                            resolutionLabel(want.width, want.height), want.rate.label()));
            QPushButton* change = box.addButton(T("Ändern"), QMessageBox::AcceptRole);
            box.addButton(T("Nicht ändern"), QMessageBox::RejectRole);
            box.setDefaultButton(change);
            box.exec();
            if (box.clickedButton() != change) return;
        }
        m_project->setFormat(want);
        return;
    }
}

void MainWindow::createCompoundClip()
{
    if (m_editor->clipIdsOf(m_selection->ids()).isEmpty()) return;
    QSet<QString> used;
    for (const Sequence& s : m_project->sequences()) used.insert(s.name);
    QString def;
    for (int i = 1;; ++i) {
        def = T("Compound Clip %1").arg(i);
        if (!used.contains(def)) break;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, T("Neuer Compound Clip"), T("Name:"), QLineEdit::Normal, def, &ok);
    if (ok) m_editor->createCompoundClip(name);
}

void MainWindow::openSelectedCompound()
{
    if (const int id = m_editor->selectedCompoundSequence()) m_timeline->openSequence(id);
}

void MainWindow::showSource(const QString& path)
{
    const MediaInfo* m = m_project->mediaInfo(path);
    if (!m) return;
    m_engine->pause();
    m_engine->showSource(path, m_sourcePos.value(path, m->markIn >= 0 ? m->markIn : 0));
}

bool MainWindow::sourceActive() const
{
    return m_engine->mode() == Engine::Mode::Source && m_project->mediaInfo(m_engine->sourcePath());
}

QString MainWindow::editSource() const
{
    if (m_project->mediaInfo(m_engine->sourcePath())) return m_engine->sourcePath();
    const QStringList selected = m_mediaPool->selectedMedia(); // noch nichts in der Quellansicht
    return selected.isEmpty() ? QString() : selected.first();
}

void MainWindow::sourceEdit(Editor::SourceEditMode mode)
{
    const QString path = editSource();
    if (path.isEmpty()) return;
    offerClipFormat({path}, true); // erster Clip in leerer Timeline: Projektformat anpassen?
    TimelineView* tv = m_timeline->view();
    const int srcPos = sourceActive() ? m_engine->position() : m_sourcePos.value(path, 0);
    const int end = m_editor->sourceEdit(mode, path, srcPos, tv->playhead());
    if (end < 0) {
        QApplication::beep(); // kein Clip unter dem Playhead bzw. zu wenig Material
        return;
    }
    // Playhead ans Ende des neuen Clips (wie DaVinci); die Quellansicht bleibt offen
    if (m_engine->mode() == Engine::Mode::Timeline) {
        m_engine->pause();
        m_engine->seek(end);
    } else {
        tv->setPlayhead(end);
    }
}

void MainWindow::updateViewer()
{
    if (sourceActive()) {
        const MediaInfo* m = m_project->mediaInfo(m_engine->sourcePath());
        m_viewer->setSource(m->path, m->name);
        m_viewer->setRange(m->length, m->markIn, m->markOut);
        return;
    }
    const Timeline& tl = m_project->timeline();
    m_viewer->setSource({}, {});
    m_viewer->setRange(TimelineOps::endFrame(tl), tl.markIn, tl.markOut);
}

void MainWindow::setProjectFormat(const ProjectFormat& format)
{
    m_project->setFormat(format);
}

void MainWindow::updateRenderCacheBar()
{
    QVector<TimelineView::CacheSpan> spans;
    for (const RenderCache::Span& s : m_engine->renderCache()->spans(m_project->timeline()))
        spans << TimelineView::CacheSpan{s.start, s.end, s.done};
    m_timeline->view()->setRenderCacheSpans(spans);
}

void MainWindow::grabStill()
{
    m_engine->pause();
    const QImage img = m_engine->grabStill(m_project->renderTimeline());
    if (img.isNull()) {
        QMessageBox::warning(this, T("Standbild exportieren"), T("Das Bild konnte nicht erzeugt werden."));
        return;
    }
    // Vorschlag: Quellclip bzw. Projektname + Timecode (Doppelpunkte gehen nicht in Dateinamen)
    const QString base = m_engine->mode() == Engine::Mode::Source
        ? QFileInfo(m_engine->sourcePath()).completeBaseName()
        : (m_projectPath.isEmpty() ? T("Unbenannt") : QFileInfo(m_projectPath).completeBaseName());
    const QString tc = Timecode::format(m_engine->position(), m_project->fps()).replace(':', '-');
    QSettings settings;
    const QString dir = settings.value("still/dir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    QString path = QFileDialog::getSaveFileName(this, T("Standbild exportieren"), QDir(dir).filePath(base + "_" + tc + ".png"),
                                                T("Bilder (*.png *.jpg *.tif)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".png";
    settings.setValue("still/dir", QFileInfo(path).absolutePath());
    if (Exporter::sameFile(path, m_engine->sourcePath()) || Exporter::readsFile(m_project->renderTimeline(), path)) {
        QMessageBox::warning(this, T("Standbild exportieren"),
                             T("%1 wird in der Timeline verwendet und kann nicht überschrieben werden.")
                                 .arg(QFileInfo(path).fileName()));
        return;
    }
    if (!img.save(path, nullptr, 95))
        QMessageBox::warning(this, T("Standbild exportieren"), T("Datei konnte nicht gespeichert werden:\n%1").arg(path));
}

void MainWindow::projectSettingsDialog()
{
    ProjectSettingsDialog dlg(m_project->format(), m_project->frameRateLocked(), this);
    if (dlg.exec() == QDialog::Accepted) m_project->setFormat(dlg.format());
}

void MainWindow::clipSpeedDialog()
{
    // Vorgabe vom ersten ausgewählten Medienclip (Titel haben keine Geschwindigkeit)
    const Clip* first = nullptr;
    for (int id : m_editor->selection()->ids())
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && !c->isTitle()
            && (!first || c->start < first->start))
            first = c;
    if (!first) return;
    const Editor::Retime cur{first->speed, first->reverse, first->freeze, first->keepPitch};
    ClipSpeedDialog dlg(cur, first->length(), m_project->format().rate.fps(), this);
    if (dlg.exec() == QDialog::Accepted)
        m_editor->setClipSpeed(m_editor->selection()->ids().values().toVector(), dlg.retime(), dlg.ripple());
}

void MainWindow::normalizeAudioDialog()
{
    const QVector<int> ids = m_editor->selectedAudioClips();
    if (ids.isEmpty()) {
        QApplication::beep(); // keine Audioclips ausgewählt
        return;
    }
    NormalizeDialog dlg(int(ids.size()), this);
    if (dlg.exec() != QDialog::Accepted) return;
    // Spitzenpegel bzw. Lautheit messen (dekodiert den Ton der Clips; Originale, nie Proxies)
    const bool loudness = dlg.mode() == NormalizeDialog::Mode::Loudness;
    QProgressDialog progress(loudness ? T("Lautheit wird gemessen…") : T("Audiopegel werden gemessen…"), T("Abbrechen"),
                             0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    QHash<int, double> levels;
    std::vector<double> blocks; // Lautheit: 400-ms-Blöcke aller Clips -> gemeinsame Lautheit für "Relativ"
    const Timeline tl = m_project->timeline(); // Kopie: Zeiger bleiben gültig
    for (int i = 0; i < ids.size(); ++i) {
        const Clip* c = TimelineOps::findClip(tl, ids[i]);
        if (!c) continue;
        const auto step = [&](double f) {
            progress.setValue(int((i + f) / ids.size() * 1000));
            QApplication::processEvents();
            return !progress.wasCanceled();
        };
        if (loudness) {
            const auto l = AudioAnalysis::clipLoudness(m_project->format(), *c, step);
            if (l) {
                levels[c->id] = l->integrated;
                blocks.insert(blocks.end(), l->blocks.begin(), l->blocks.end());
            }
        } else if (const auto peak = AudioAnalysis::clipPeakDb(m_project->format(), *c, step)) {
            levels[c->id] = *peak;
        }
        if (progress.wasCanceled()) return;
    }
    progress.setValue(1000);
    std::optional<double> ref;
    if (loudness) ref = LoudnessMeter::integratedOf(blocks);
    m_editor->normalizeAudio(levels, dlg.targetDb(), dlg.relative(), ref);
}

void MainWindow::onFormatChanged()
{
    const ProjectFormat& f = m_project->format();
    const FrameRate oldRate = m_engine->format().rate;
    m_engine->setFormat(f); // Timeline baut das folgende timelineChanged() neu auf
    m_mediaCache->setFormat(f);
    m_inspector->setFrameSize(f.size());
    m_probeCache.clear();
    if (f.rate == oldRate) return;
    // Längen im Media Pool zählen in Projekt-Frames -> neu einlesen (nur in leerer Timeline möglich,
    // Undo/Redo eingeschlossen). Offline-Medien umrechnen.
    QVector<MediaInfo> media = m_project->media();
    for (MediaInfo& m : media) {
        const MediaInfo fresh = QFileInfo::exists(m.path) ? m_engine->probe(m.path) : MediaInfo{};
        m.length = fresh.length > 0 ? fresh.length : int(std::lround(m.length * f.rate.fps() / oldRate.fps()));
        auto scaleMark = [&](int mark) {
            return mark < 0 ? -1 : std::min(m.length - 1, int(std::lround(mark * f.rate.fps() / oldRate.fps())));
        };
        m.markIn = scaleMark(m.markIn);
        m.markOut = scaleMark(m.markOut);
    }
    m_project->replaceMedia(media);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    const QEvent::Type t = event->type();
    if (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonRelease && t != QEvent::MouseButtonDblClick)
        return QMainWindow::eventFilter(obj, event);
    // Nur im Hauptfenster, nicht in Dialogen (dort wird z. B. die Belegung gewählt)
    auto* w = qobject_cast<QWidget*>(obj);
    if (!w || w->window() != this || QApplication::activeModalWidget())
        return QMainWindow::eventFilter(obj, event);
    const auto* me = static_cast<QMouseEvent*>(event);
    const QString id = InputBindings::instance().mouseAction(me->button());
    if (id.isEmpty()) return QMainWindow::eventFilter(obj, event);
    if (t == QEvent::MouseButtonPress)
        if (QAction* a = InputBindings::instance().action(id); a && a->isEnabled()) a->trigger();
    return true; // Loslassen/Doppelklick der belegten Taste ebenfalls schlucken
}

void MainWindow::shuttle(int direction)
{
    // J/L mehrfach drücken = schneller, wie in DaVinci
    const double s = m_engine->speed();
    if (direction > 0) m_engine->setSpeed(s <= 0 ? 1.0 : std::min(s * 2, 16.0));
    else m_engine->setSpeed(s >= 0 ? -1.0 : std::max(s * 2, -16.0));
}

void MainWindow::stepFrames(int frames)
{
    m_engine->pause();
    m_engine->seek(m_engine->position() + frames);
}

void MainWindow::jumpToEdit(int direction)
{
    QVector<int> points;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips) points << c.start << c.end();
    jumpTo(points, direction);
}

void MainWindow::jumpToMarker(int direction)
{
    jumpTo(m_project->timeline().markers, direction);
}

void MainWindow::jumpTo(const QVector<int>& points, int direction)
{
    const int pos = m_timeline->view()->playhead();
    int best = direction > 0 ? INT_MAX : -1;
    for (int e : points) {
        if (direction > 0 && e > pos) best = std::min(best, e);
        if (direction < 0 && e < pos) best = std::max(best, e);
    }
    if (best == INT_MAX || best < 0) return;
    jumpToFrame(best);
}

void MainWindow::jumpToFrame(int frame)
{
    if (frame < 0) return;
    m_engine->pause();
    if (m_engine->mode() != Engine::Mode::Timeline) m_engine->showTimeline(frame);
    else m_engine->seek(frame);
}

void MainWindow::importSubtitleFiles(const QStringList& paths)
{
    QStringList problems;
    for (const QString& path : paths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            problems << T("%1: nicht lesbar").arg(QFileInfo(path).fileName());
            continue;
        }
        int skipped = 0;
        const QVector<SubtitleCue> cues = Subtitles::parseSrt(f.readAll(), m_project->frameRate(), &skipped);
        if (cues.isEmpty()) {
            problems << T("%1: keine Untertitel gefunden").arg(QFileInfo(path).fileName());
            continue;
        }
        m_editor->importSubtitles(cues, QFileInfo(path).completeBaseName());
        if (skipped) problems << T("%1: %2 fehlerhafte Einträge übersprungen").arg(QFileInfo(path).fileName()).arg(skipped);
    }
    if (!problems.isEmpty()) QMessageBox::warning(this, T("Untertitel importieren"), problems.join('\n'));
}

void MainWindow::importSubtitlesDialog()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, T("Untertitel importieren"), m_projectPath.isEmpty() ? QString() : QFileInfo(m_projectPath).absolutePath(),
                                                            T("Untertitel (*.srt);;Alle Dateien (*)"));
    if (!files.isEmpty()) importSubtitleFiles(files);
}

void MainWindow::exportSubtitlesDialog()
{
    // Sichtbare Untertitelspur (wie DaVinci „Export Subtitle“), sonst die erste mit Einträgen
    const Timeline& tl = m_project->timeline();
    int index = -1;
    for (int i = 0; i < tl.subtitles.size(); ++i)
        if (tl.subtitles[i].enabled && !tl.subtitles[i].cues.isEmpty()) index = i;
    for (int i = 0; i < tl.subtitles.size() && index < 0; ++i)
        if (!tl.subtitles[i].cues.isEmpty()) index = i;
    if (index < 0) {
        QMessageBox::information(this, T("Untertitel exportieren"), T("Die Timeline enthält keine Untertitel."));
        return;
    }
    const SubtitleTrack& t = tl.subtitles[index];
    const QString base = m_projectPath.isEmpty() ? QString() : QFileInfo(m_projectPath).absolutePath();
    const QString name = (t.name.isEmpty() ? (m_projectPath.isEmpty() ? QString("untertitel") : QFileInfo(m_projectPath).completeBaseName()) : t.name) + ".srt";
    QString path = QFileDialog::getSaveFileName(this, T("Untertitel exportieren"), base.isEmpty() ? name : base + "/" + name,
                                                T("Untertitel (*.srt)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".srt";
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(Subtitles::toSrt(t.cues, m_project->frameRate())) < 0 || !f.commit())
        QMessageBox::warning(this, T("Untertitel exportieren"), T("Untertiteldatei kann nicht geschrieben werden: %1").arg(path));
}
