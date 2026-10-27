#include "app/MainWindow.h"
#include "app/AiKeys.h"
#include "app/AiLog.h"
#include "app/ExtensionsDialog.h"
#include "app/TranscribeDialog.h"
#include "ui/TranscriptPanel.h"

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
#include "core/Accents.h"
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
#include "engine/GradedThumbs.h"
#include "engine/MediaCache.h"
#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "engine/Stabilizer.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaStorage.h"
#include "ui/ColorPanel.h"
#include "ui/ScopesPanel.h"
#include "ui/Inspector.h"
#include "ui/KeyColorPicker.h"
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
#include <QPointer>
#include <QStatusBar>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>

QString MainWindow::appName()
{
    return "schneidi " + QCoreApplication::applicationVersion().section(" (", 0, 0);
}

MainWindow::MainWindow(Engine* engine, QWidget* parent) : QMainWindow(parent), m_engine(engine)
{
    setWindowTitle(appName());
    resize(1600, 950);
    m_aiSettings = AiSettings::load(); // Settings › AI (MainWindowSettings.cpp)
    m_aiLog = new AiLog(this);
    m_aiLog->setLimit(m_aiSettings.logLength);

    m_project = new Project(this);
    m_selection = new Selection(this);
    m_editor = new Editor(m_project, m_selection, this);
    // Import presets for audio streams: the project's own (from its start template) or the settings' (all projects)
    const auto importPresets = [this] { m_editor->setStreamPresets(m_project->effectiveImportPresets()); };
    connect(m_project, &Project::importPresetsChanged, this, importPresets);
    importPresets();

    buildLayout();
    buildActions();
    setupChat();
    applyAiMarkStyle();
    qApp->installEventFilter(this); // Mouse side buttons -> actions (keybindings.json, section "mouse")

    // model -> engine
    // Bundled: while dragging (fade handle, volume line, Inspector …) every mouse move changes the model;
    // rebuilding the preview immediately each time blocked the timeline (handle lagged behind).
    // Mixer changes are cheap (set live) and still go through immediately.
    m_engineTimer.setSingleShot(true);
    m_engineTimer.setInterval(30);
    connect(&m_engineTimer, &QTimer::timeout, this, [this] {
        m_engine->updateTimeline(m_project->renderTimeline());
        updateRenderCacheBar();
    });
    connect(m_editor, &Editor::mixerOnlyEdit, m_engine, &Engine::mixerOnlyNext);
    // The sound is mixed with the project's media lengths, like in the export (RenderQueue::exportSettings)
    auto mediaLengths = [this] {
        QHash<QString, int> lengths;
        for (const MediaInfo& m : m_project->media()) lengths.insert(m.path, m.length);
        m_engine->setMediaLengths(lengths);
    };
    connect(m_project, &Project::mediaChanged, this, mediaLengths);
    mediaLengths();
    connect(m_project, &Project::timelineChanged, this, [this] {
        if (m_engine->mixerOnlyPending()) m_engine->updateTimeline(m_project->renderTimeline());
        else if (!m_engineTimer.isActive()) m_engineTimer.start();
    });
    // Files open in a thread (MLT probes them: seconds on weak hardware): the window stays usable meanwhile
    m_engine->setOpenInBackground(true);
    connect(m_engine, &Engine::openingChanged, this, [this](bool opening) {
        if (opening) statusBar()->showMessage(T("Medien werden geöffnet …"));
        else if (statusBar()->currentMessage() == T("Medien werden geöffnet …")) statusBar()->clearMessage();
    });
    m_engine->updateTimeline(m_project->renderTimeline());
    // Proxy finished/deleted or "use proxy media" toggled -> rebuild the preview
    ProxyManager* proxies = m_engine->proxies();
    auto proxiesChanged = [this] { m_engine->updateTimeline(m_project->renderTimeline()); };
    connect(proxies, &ProxyManager::proxyChanged, this, [this, proxiesChanged](const QString& path) {
        // also clips in compound clips (nested sequences)
        const auto& seqs = m_project->sequences();
        if (std::any_of(seqs.begin(), seqs.end(),
                        [&](const Sequence& s) { return TimelineOps::usesMediaOnVideo(s.timeline, path); }))
            proxiesChanged();
    });
    connect(proxies, &ProxyManager::enabledChanged, this, proxiesChanged);
    setupProxyHint();
    // Before/After: cached clips contain the correction -> rebuild the preview so they come from the original
    connect(m_engine, &Engine::colorBypassChanged, this, [this] {
        if (m_engine->renderCache()->mode() != RenderCache::Mode::Off) m_engine->updateTimeline(m_project->renderTimeline());
    });
    // Render cache: file finished/deleted or mode changed -> rebuild the preview, bar above the timeline
    RenderCache* renderCache = m_engine->renderCache();
    connect(renderCache, &RenderCache::cacheChanged, this, [this, proxiesChanged] {
        proxiesChanged();
        updateRenderCacheBar();
    });
    connect(renderCache, &RenderCache::progressChanged, this, &MainWindow::updateRenderCacheBar);
    // Stabilization analysis done -> build the preview again (now stabilized); also lets the render cache start
    connect(Stabilizer::instance(), &Stabilizer::resultReady, this, [this, proxiesChanged] {
        proxiesChanged();
        updateRenderCacheBar();
    });
    connect(renderCache, &RenderCache::failed, this,
            [this](const QString& msg) { QMessageBox::warning(this, T("Render-Cache"), msg); });

    // Engine -> playhead (timeline and multicam grid; in source mode the playhead stays put)
    TimelineView* tv = m_timeline->view();
    connect(m_engine, &Engine::positionChanged, this, [this, tv](int f) {
        if (m_engine->timelinePositions()) tv->setPlayhead(f);
    });
    connect(tv, &TimelineView::seekRequested, this, [this](int f) {
        if (m_engine->mode() == Engine::Mode::Source) m_engine->showTimeline(f);
        else m_engine->seek(f);
    });
    // Keyframes: Inspector shows values at the playhead, arrows ◀ ▶ jump like a click into the ruler
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (m_engine->timelinePositions()) m_inspector->setPlayhead(f);
    });
    connect(m_inspector, &Inspector::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_inspector, &Inspector::savePresetRequested, this, &MainWindow::saveEffectPreset);
    connect(m_inspector, &Inspector::keyPickRequested, this, &MainWindow::startKeyPick);
    connect(m_inspector, &Inspector::keyPickCancelled, this, &MainWindow::stopKeyPick);
    connect(m_selection, &Selection::changed, this, [this] {
        if (m_keyPicker) stopKeyPick();
    });
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (m_engine->timelinePositions()) m_colorPanel->setPlayhead(f);
    });
    connect(m_colorPanel, &ColorPanel::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (m_engine->timelinePositions()) m_transcript->setPlayhead(f);
    });
    connect(m_transcript, &TranscriptPanel::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_deliver, &DeliverPanel::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_transcript, &TranscriptPanel::openExtensions, this, [this] {
        ExtensionsDialog ext(this);
        ext.exec();
    });
    connect(m_engine, &Engine::frameReady, m_scopes, &ScopesPanel::setFrame);
    connect(m_mediaPool, &MediaPool::sourceRequested, this, &MainWindow::showSource);
    connect(m_mediaPool, &MediaPool::sequenceOpenRequested, m_timeline, &TimelinePanel::openSequence);
    // Another timeline opened: selection gone, playhead and zoom/scroll as last in this timeline (like DaVinci)
    m_shownSequence = m_project->currentSequence();
    connect(m_project, &Project::currentSequenceChanged, this, [this, tv] {
        const int id = m_project->currentSequence();
        if (id == m_shownSequence) return;
        m_sequencePos[m_shownSequence] = tv->playhead();
        m_sequenceView[m_shownSequence] = tv->view();
        m_shownSequence = id;
        // Restore zoom/scroll of this timeline; never shown: keep zoom, start at the front and top
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
    // Right-click on clips: the matching actions (shortcuts as in the menu)
    connect(tv, &TimelineView::clipMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        const bool audio = !m_editor->selectedAudioClips().isEmpty();
        const int cacheState = m_editor->selectionRenderCacheState();
        const bool compound = m_editor->selectedCompoundSequence() != 0;
        const bool multicam = selectedMulticamSequence() != 0;
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
        for (const char* id : {"clip_speed", "retime_controls", "clip_speed_reset", "curve_editor", "accents",
                               "normalize_audio", "audio_ducking", "stream_presets_learn", "",
                               "toggle_enabled", "link_clips",
                               "render_cache_clip", "save_effect_preset", "", "compound_create", "compound_open", "compound_decompose", "",
                               "delete", "ripple_delete"}) {
            if (!*id) {
                menu.addSeparator();
                continue;
            }
            if (QString(id) == "accents") { // zoom punch, flash, shake as keyframes on the selected clips there
                const int frame = m_timeline->view()->playhead();
                QVector<int> ids;
                for (int cid : m_editor->clipIdsOf(m_selection->ids()))
                    if (TrackRef r; const Clip* c = TimelineOps::findClip(m_project->timeline(), cid, &r))
                        if (r.kind == TrackKind::Video && frame >= c->start && frame < c->end()) ids << cid;
                QMenu* accents = menu.addMenu(T("Akzent am Playhead"));
                accents->setEnabled(!ids.isEmpty());
                for (const Accents::Preset& p : Accents::presets())
                    connect(accents->addAction(T(p.name)), &QAction::triggered, this, [this, ids, frame, p] {
                        m_editor->addAccents(ids, {frame}, p.amounts, T("Akzent: %1").arg(T(p.name)));
                    });
                continue;
            }
            QAction* a = InputBindings::instance().action(id);
            if (!a) continue;
            if ((QString(id) == "normalize_audio" || QString(id) == "audio_ducking") && !audio) continue;
            if (QString(id) == "stream_presets_learn" && !streamPresetsClip()) continue;
            if (QString(id) == "compound_open" && !compound && !multicam) continue;
            if (QString(id) == "compound_decompose") {
                if (compound) menu.addAction(a);
                if (multicam) addMulticamMenu(&menu); // switch angle, flatten (like DaVinci)
                continue;
            }
            if (QString(id) == "save_effect_preset" && !presetClip()) continue;
            if (QString(id) == "render_cache_clip") {
                if (cacheState < 0) continue; // video clips only (no titles)
                a->setChecked(cacheState == 1);
            }
            menu.addAction(a);
        }
        // Clip color/flags like DaVinci: apply to the Media Pool clip (titles have none)
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
    // Source range dragged from the Viewer into the timeline: overwrite there (like DaVinci)
    connect(tv, &TimelineView::rangeDropRequested, this, [this](const QString& path, int in, int out, int frame, int track) {
        // Looked at in the Media Storage without importing: into the selected bin first
        if (!m_editor->importPreviewSource(path, m_mediaPool->currentBin())) return;
        if (m_project->frameRateLocked()) {
            m_editor->placeSourceRange(path, in, out, frame, track);
            return;
        }
        QTimer::singleShot(0, this, [this, path, in, out, frame, track] { // confirmation only after the drop (see onDrop)
            offerClipFormat({path}, true);
            m_editor->placeSourceRange(path, in, out, frame, track);
        });
    });
    // Viewer: scrubber (length, In/Out) and title depending on source/timeline
    connect(m_engine, &Engine::modeChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::timelineChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::mediaMarksChanged, this, &MainWindow::updateViewer);
    connect(m_project, &Project::mediaChanged, this, &MainWindow::updateViewer);
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (sourceActive()) m_sourcePos[m_engine->sourcePath()] = f;
    });
    updateViewer();
    setupMulticam();
    setupWatchFolders();
    connect(m_storage, &MediaStorage::importRequested, m_mediaPool, &MediaPool::importFiles);
    connect(m_storage, &MediaStorage::folderImportRequested, m_mediaPool, &MediaPool::importFolder);
    connect(m_storage, &MediaStorage::previewRequested, this, &MainWindow::previewFile);
    connect(m_editor, &Editor::previewSourceChanged, this, &MainWindow::updateViewer);
    // Media Storage marks what is in the Media Pool already
    auto poolFiles = [this] {
        QStringList paths;
        paths.reserve(m_project->media().size());
        for (const MediaInfo& m : m_project->media()) paths << m.path;
        m_storage->setPoolFiles(paths);
    };
    connect(m_project, &Project::mediaChanged, this, poolFiles);
    poolFiles();
    // Effects Library: double-click = like Ctrl+T resp. "Insert title", only with the chosen kind
    connect(m_effects, &EffectsLibrary::transitionRequested, this, [this, tv](TrackKind kind, const TransitionStyle& style) {
        const bool trim = !m_editor->transitionsLackingHandles(tv->playhead(), style, kind).isEmpty()
                          && TimelineView::confirmTrimForTransition(this);
        m_editor->addTransitions(tv->playhead(), style, kind, trim);
    });
    connect(m_effects, &EffectsLibrary::titleRequested, this, [this, tv] { m_editor->addTitle(tv->playhead()); });
    // Filters: onto the selected clips of the effect's kind, without a selection onto the clip at the playhead
    connect(m_effects, &EffectsLibrary::effectRequested, this, [this, tv](const QString& id) {
        m_editor->addEffect(m_editor->effectTargets(tv->playhead(), Editor::effectKind(id)), id);
    });
    tv->setProbe([this](const QString& path) { return probeCached(path); });
    m_mediaCache = new MediaCache(this);
    tv->setMediaCache(m_mediaCache);
    // Project settings -> engine (profile anew), cache, Inspector; comes before timelineChanged
    connect(m_project, &Project::formatChanged, this, &MainWindow::onFormatChanged);

    connect(m_project, &Project::modifiedChanged, this, &MainWindow::updateTitle);
    updateTitle();
    // Automatic backup every minute (only on changes), separate from the project file
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

    // schneidi starts with the user's start template too (Settings › Start Templates; Neutral = as it always did)
    if (const StartTemplate t = StartTemplates::defaultTemplate(); t.id != QLatin1String(StartTemplates::NeutralId)) {
        const ProjectData d = StartTemplates::apply(t);
        m_engine->setFormat(d.format);
        m_project->load(d);
        m_engine->showTimeline(0);
    }
}

void MainWindow::importFiles(const QStringList& paths, bool placeOnTimeline, bool adoptFormat)
{
    m_mediaPool->importFiles(paths);
    if (!placeOnTimeline) return;
    // Test helper (--demo): put files one after another on V1/A1; project format from the first clip without asking
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
    if (paths.size() == 1 && MediaPool::sequenceOfItem(paths.first())) { // Nest timeline/compound clip
        m_editor->addSequenceAt(MediaPool::sequenceOfItem(paths.first()), frame, track);
        return;
    }
    // Subtitle files become subtitle tracks (times from the file, like DaVinci)
    QStringList media, subtitles;
    for (const QString& p : paths)
        (QFileInfo(p).suffix().compare("srt", Qt::CaseInsensitive) == 0 ? subtitles : media) << p;
    if (!subtitles.isEmpty()) {
        QTimer::singleShot(0, this, [this, subtitles] { importSubtitleFiles(subtitles); }); // messages not inside the drop
        if (media.isEmpty()) return;
        onDrop(media, frame, track);
        return;
    }
    // Directly from the file manager: first into the Media Pool, then onto the timeline (like DaVinci)
    QStringList unknown;
    for (const QString& p : paths)
        if (!m_project->mediaInfo(p)) unknown << p;
    if (!unknown.isEmpty()) m_mediaPool->importFiles(unknown);
    if (m_project->frameRateLocked()) {
        m_editor->addMediaAt(paths, frame, track);
        return;
    }
    // Empty timeline: possibly ask about the project format – only after the drop (no modal query in the middle
    // of drag & drop, the source, e.g. the file manager, would otherwise wait)
    QTimer::singleShot(0, this, [this, paths, frame, track] {
        offerClipFormat(paths, true);
        m_editor->addMediaAt(paths, frame, track);
    });
}

void MainWindow::offerClipFormat(const QStringList& paths, bool ask)
{
    if (m_project->frameRateLocked()) return; // only in an empty timeline
    if (m_project->fixedResolution() && m_project->fixedFrameRate()) return; // start template / Project Settings
    // The first clip with video decides (still images and audio have no format of their own)
    for (const QString& path : paths) {
        const MediaInfo* info = m_project->mediaInfo(path);
        if (!info || !info->hasVideo || info->isImage) continue;
        const ClipFormat cf = detectClipFormat(path);
        if (!cf.ok) return;
        const ProjectFormat cur = m_project->format();
        // A fixed resolution or frame rate (start template, Project Settings) stays as it is
        ProjectFormat want;
        want.width = m_project->fixedResolution() ? cur.width : cf.width;
        want.height = m_project->fixedResolution() ? cur.height : cf.height;
        want.rate = m_project->fixedFrameRate() ? cur.rate : cf.suggested;
        if (want == cur) return;
        if (ask) {
            // Like DaVinci "Change project frame rate?", here including resolution
            QString clipRate = cf.rate.label();
            if (cf.variable)
                clipRate = T("variabel, etwa %1").arg(QString::number(cf.averageFps, 'f', 2).replace('.', I18n::decimalPoint()));
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
    else if (const int mc = selectedMulticamSequence()) m_timeline->openSequence(mc); // multicam clip like a compound clip
}

void MainWindow::showSource(const QString& path)
{
    const MediaInfo* m = m_project->mediaInfo(path);
    if (!m) return;
    m_engine->pause();
    m_engine->showSource(path, m_sourcePos.value(path, m->markIn >= 0 ? m->markIn : 0));
}

void MainWindow::previewFile(const QString& path)
{
    if (m_project->mediaInfo(path)) { // in the Media Pool already: its clip with its marks
        showSource(path);
        return;
    }
    const MediaInfo& before = m_editor->previewSource();
    MediaInfo info;
    const auto cached = m_probeCache.constFind(path);
    if (cached != m_probeCache.cend()) {
        info = *cached;
    } else {
        info.path = path;
        info.name = QFileInfo(path).fileName();
    }
    if (before.path == path) { // clicked again: its marks stay
        info.markIn = before.markIn;
        info.markOut = before.markOut;
        info.markers = before.markers;
    }
    m_editor->setPreviewSource(info);
    m_engine->pause();
    m_engine->showSource(path, m_sourcePos.value(path, 0));
    if (cached != m_probeCache.cend()) return;
    // Probing opens the file once more (~0.5 s with several audio streams): in the background, the window stays usable
    QThreadPool::globalInstance()->start([guard = QPointer<MainWindow>(this), format = m_project->format(), path] {
        const MediaInfo probed = Engine::probe(format, path);
        QMetaObject::invokeMethod(qApp, [guard, format, probed] {
            if (!guard || guard->m_project->format() != format) return; // lengths count in another frame rate now
            guard->m_probeCache.insert(probed.path, probed);
            const MediaInfo& shown = guard->m_editor->previewSource();
            if (shown.path != probed.path) return; // another file meanwhile, or imported
            MediaInfo info = probed;
            info.markIn = shown.markIn;
            info.markOut = shown.markOut;
            info.markers = shown.markers;
            guard->m_editor->setPreviewSource(info);
        }, Qt::QueuedConnection);
    });
}

bool MainWindow::sourceActive() const
{
    return m_engine->mode() == Engine::Mode::Source && m_editor->sourceMedia(m_engine->sourcePath());
}

QString MainWindow::editSource() const
{
    if (m_editor->sourceMedia(m_engine->sourcePath())) return m_engine->sourcePath();
    const QStringList selected = m_mediaPool->selectedMedia(); // nothing in the source view yet
    return selected.isEmpty() ? QString() : selected.first();
}

void MainWindow::sourceEdit(Editor::SourceEditMode mode)
{
    const QString path = editSource();
    if (path.isEmpty()) return;
    // Looked at in the Media Storage without importing: into the selected bin first (no undo step, like every
    // import; the edit is one). Not probed yet (just clicked): nothing to place yet.
    if (!m_editor->importPreviewSource(path, m_mediaPool->currentBin())) {
        QApplication::beep();
        return;
    }
    offerClipFormat({path}, true); // first clip in an empty timeline: adapt project format?
    TimelineView* tv = m_timeline->view();
    const int srcPos = sourceActive() ? m_engine->position() : m_sourcePos.value(path, 0);
    const int end = m_editor->sourceEdit(mode, path, srcPos, tv->playhead());
    if (end < 0) {
        QApplication::beep(); // no clip under the playhead resp. too little material
        return;
    }
    // Playhead to the end of the new clip (like DaVinci); the source view stays open
    if (m_engine->timelinePositions()) {
        m_engine->pause();
        m_engine->seek(end);
    } else {
        tv->setPlayhead(end);
    }
}

void MainWindow::updateViewer()
{
    if (sourceActive()) {
        const MediaInfo* m = m_editor->sourceMedia(m_engine->sourcePath());
        m_viewer->setSource(m->path, m_project->mediaInfo(m->path) ? m->name : T("%1 (nicht importiert)").arg(m->name));
        m_viewer->setRange(m->length, m->markIn, m->markOut);
        m_viewer->setMarkers(m->markers);
        return;
    }
    const Timeline& tl = m_project->timeline();
    m_viewer->setSource({}, {});
    m_viewer->setRange(TimelineOps::endFrame(tl), tl.markIn, tl.markOut);
    m_viewer->setMarkers(tl.markers);
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
    // Suggestion: source clip resp. project name + timecode (colons are not allowed in file names)
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

void MainWindow::startKeyPick(int clipId)
{
    stopKeyPick();
    m_engine->pause();
    const Timeline full = m_project->renderTimeline();
    const Timeline alone = KeyColorPicker::isolate(full, clipId);
    const Clip* clip = nullptr;
    for (const Track& t : alone.video)
        if (!t.clips.isEmpty()) clip = &t.clips.first();
    QImage picture;
    if (clip) {
        // At the playhead; outside the clip (or a source shown in the viewer) its nearest frame
        const int at = m_engine->timelinePositions() ? m_engine->position() : clip->start;
        picture = m_engine->grabFrame(alone, std::clamp(at, clip->start, clip->end() - 1));
    }
    if (picture.isNull()) {
        m_inspector->setKeyPicking(false);
        statusBar()->showMessage(T("Das Bild des Clips konnte nicht erzeugt werden."), 4000);
        return;
    }
    m_keyPicker = new KeyColorPicker(picture, this);
    connect(m_keyPicker, &KeyColorPicker::picked, this, [this](const QColor& color) {
        m_inspector->applyKeyColor(color);
        stopKeyPick();
    });
    connect(m_keyPicker, &KeyColorPicker::cancelled, this, &MainWindow::stopKeyPick);
    m_viewer->setOverlay(m_keyPicker);
    m_inspector->setKeyPicking(true); // the stopKeyPick() above switched the button off
}

void MainWindow::stopKeyPick()
{
    m_inspector->setKeyPicking(false);
    if (!m_keyPicker) return;
    qApp->removeEventFilter(m_keyPicker); // its Esc filter ends now, not when deleteLater gets to it
    m_viewer->setOverlay(m_colorPanel->overlay());
    m_keyPicker->deleteLater();
    m_keyPicker = nullptr;
}

void MainWindow::projectSettingsDialog()
{
    ProjectSettingsDialog dlg(m_project->format(), m_project->frameRateLocked(), this);
    dlg.setFixed(m_project->fixedResolution(), m_project->fixedFrameRate());
    if (dlg.exec() != QDialog::Accepted) return;
    m_project->setFormat(dlg.format());
    m_project->setFixedFormat(dlg.fixedResolution(), dlg.fixedFrameRate());
}

void MainWindow::clipSpeedDialog()
{
    // Default from the first selected media clip (titles have no speed)
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
        QApplication::beep(); // no audio clips selected
        return;
    }
    NormalizeDialog dlg(int(ids.size()), this);
    if (dlg.exec() != QDialog::Accepted) return;
    // Measure peak level resp. loudness (decodes the audio of the clips; originals, never proxies)
    const bool loudness = dlg.mode() == NormalizeDialog::Mode::Loudness;
    QProgressDialog progress(loudness ? T("Lautheit wird gemessen…") : T("Audiopegel werden gemessen…"), T("Abbrechen"),
                             0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    const Timeline tl = m_project->timeline(); // copy: stays the same while events are processed
    const auto measured = AudioAnalysis::measureClips(m_project->format(), tl, ids, loudness, [&](double f) {
        progress.setValue(int(f * 1000));
        QApplication::processEvents();
        return !progress.wasCanceled();
    });
    if (!measured) return;
    progress.setValue(1000);
    m_editor->normalizeAudio(measured->levels, dlg.targetDb(), dlg.relative(), measured->shared);
}

void MainWindow::onFormatChanged()
{
    const ProjectFormat& f = m_project->format();
    const FrameRate oldRate = m_engine->format().rate;
    m_engine->setFormat(f); // timeline rebuilds in the following timelineChanged()
    m_mediaCache->setFormat(f);
    // Every load comes here (also with the same format): graded tiles are kept by clip id, and the next project has
    // clips with the same ids
    m_mediaCache->graded()->clear();
    m_inspector->setFrameSize(f.size());
    m_probeCache.clear();
    m_editor->setPreviewSource({}); // its length counts in the old frame rate; the Viewer shows the timeline now
    if (f.rate == oldRate) return;
    // Lengths in the Media Pool count in project frames -> re-read (only possible in an empty timeline,
    // undo/redo included)
    m_project->recountMedia(oldRate, [this](const QString& path) { return m_engine->probe(path).length; });
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    const QEvent::Type t = event->type();
    if (t != QEvent::MouseButtonPress && t != QEvent::MouseButtonRelease && t != QEvent::MouseButtonDblClick)
        return QMainWindow::eventFilter(obj, event);
    // Only in the main window, not in dialogs (there e.g. the binding is being chosen)
    auto* w = qobject_cast<QWidget*>(obj);
    if (!w || w->window() != this || QApplication::activeModalWidget())
        return QMainWindow::eventFilter(obj, event);
    const auto* me = static_cast<QMouseEvent*>(event);
    const QString id = InputBindings::instance().mouseAction(me->button());
    if (id.isEmpty()) return QMainWindow::eventFilter(obj, event);
    // A scoped or panel action (Media Pool, Render Queue, Transcript keys) only over its widget
    if (const QWidget* scope = InputBindings::instance().scopeOf(id); scope && scope != w && !scope->isAncestorOf(w))
        return QMainWindow::eventFilter(obj, event);
    if (t == QEvent::MouseButtonPress)
        if (QAction* a = InputBindings::instance().action(id); a && a->isEnabled()) a->trigger();
    return true; // also swallow release/double-click of the bound key
}

void MainWindow::shuttle(int direction)
{
    // Pressing J/L repeatedly = faster, like in DaVinci
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
    if (sourceActive()) { // source markers (M in the source viewer)
        const int pos = m_engine->position();
        const QVector<int>& markers = m_editor->sourceMedia(m_engine->sourcePath())->markers;
        int best = -1;
        for (int m : markers)
            if (direction > 0 ? m > pos && (best < 0 || m < best) : m < pos && m > best) best = m;
        if (best < 0) return;
        m_engine->pause();
        m_engine->seek(best);
        return;
    }
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
    if (m_engine->mode() == Engine::Mode::Source) m_engine->showTimeline(frame);
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

// Like DaVinci "Create Subtitles from Audio": whole timeline or In/Out, result as a new subtitle track
void MainWindow::transcribeDialog()
{
    const Timeline tl = m_project->renderTimeline();
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) {
        QMessageBox::information(this, T("Untertitel aus Audio erzeugen"), T("Die Timeline ist leer."));
        return;
    }
    const Timeline& cur = m_project->timeline();
    const int from = cur.markIn >= 0 ? std::min(cur.markIn, end - 1) : 0;
    const int to = cur.markOut >= 0 ? std::min(cur.markOut + 1, end) : end;
    m_engine->pause();
    // "A3 Audio 3 – Mikrofon": the stream names tell voice from game sound (OBS-style recordings)
    QStringList labels;
    for (int i = 0; i < tl.audio.size(); ++i) {
        const Track& t = tl.audio[i];
        QStringList streams;
        for (const Clip& c : t.clips)
            if (const MediaInfo* m = c.mediaPath.isEmpty() ? nullptr : m_project->mediaInfo(c.mediaPath);
                m && m->audioStreamCount() > 1 && !streams.contains(m->audioStreamName(c.audioStream)))
                streams << m->audioStreamName(c.audioStream);
        const QString name = trackShortName({TrackKind::Audio, i}) + "  " + trackDisplayName(t, {TrackKind::Audio, i});
        labels << (t.clips.isEmpty() ? QString() : streams.isEmpty() ? name : name + " – " + streams.join(", "));
    }
    TranscribeDialog dlg(tl, m_project->format(), from, std::max(from + 1, to), labels, this);
    connect(&dlg, &TranscribeDialog::openExtensions, this, [this] {
        ExtensionsDialog ext(this);
        ext.exec();
    });
    if (dlg.exec() != QDialog::Accepted) return;
    QUndoStack* undo = m_project->undoStack();
    undo->beginMacro(T("Untertitel aus Audio erzeugen"));
    const QVector<TranscribeDialog::Result> results = dlg.results();
    for (int i = 0; i < results.size(); ++i) // one track per speaker: all visible, one above the other
        m_editor->importSubtitles(results[i].cues, results[i].name, results.size() > 1 ? i : -1);
    undo->endMacro();
}

void MainWindow::exportSubtitlesDialog()
{
    // Visible subtitle track (like DaVinci "Export Subtitle"), otherwise the first one with entries
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

void MainWindow::setupProxyHint()
{
    m_playClock.start();
    connect(m_engine, &Engine::speedChanged, this, [this](double speed) {
        if (speed == 1.0) m_playMeter.start(m_project->frameRate(), m_playClock.elapsed());
        else m_playMeter.stop();
    });
    connect(m_engine, &Engine::positionChanged, this, [this] {
        if (!m_playMeter.frame(m_playClock.elapsed()) || m_proxyHintClosed || !m_slowFiles.isEmpty()) return;
        m_slowFiles = playingVideoFiles();
        m_slowEffects = expensiveAtPlayhead();
        m_slowRate = m_playMeter.rate();
        m_slowFps = m_playMeter.fps();
        qInfo("Playback too slow: %.1f of %.2f pictures/s", m_slowRate, m_slowFps);
        updateProxyHint();
    });
    ProxyManager* proxies = m_engine->proxies();
    RenderCache* cache = m_engine->renderCache();
    connect(m_viewer, &Viewer::hintAccepted, this, [this, proxies, cache] {
        if (offersRenderCache()) {
            // Effects were the cost: their clips get rendered in the background; if it still stutters afterwards,
            // a new measurement offers proxies
            m_slowFiles.clear();
            m_slowEffects = false;
            cache->setMode(RenderCache::Mode::Smart);
            return updateProxyHint();
        }
        QStringList missing;
        for (const QString& f : std::as_const(m_slowFiles))
            if (!proxies->isPending(f) && !proxies->hasProxy(f)) missing << f;
        proxies->generate(missing);
    });
    connect(m_viewer, &Viewer::hintClosed, this, [this] {
        m_proxyHintClosed = true;
        m_slowFiles.clear();
    });
    connect(cache, &RenderCache::modeChanged, this, &MainWindow::updateProxyHint);
    connect(proxies, &ProxyManager::queueChanged, this, &MainWindow::updateProxyHint);
    connect(proxies, &ProxyManager::proxyChanged, this, &MainWindow::updateProxyHint);
    connect(proxies, &ProxyManager::enabledChanged, this, &MainWindow::updateProxyHint);
}

bool MainWindow::offersRenderCache() const
{
    // Proxies only make decoding cheaper; with effects in the picture the smart render cache helps more (it also
    // plays a quickly decodable file instead of the original)
    return !m_slowFiles.isEmpty() && m_slowEffects && m_engine->renderCache()->mode() != RenderCache::Mode::Smart;
}

void MainWindow::updateProxyHint()
{
    if (offersRenderCache())
        return m_viewer->setHint(T("Die Wiedergabe ruckelt (%1 von %2 Bildern/s): Hier kosten die Effekte am meisten. "
                                   "Der Render-Cache „Smart“ rechnet Clips mit Effekten in Pausen vor, der Export nutzt "
                                   "weiter die Originale")
                                     .arg(qRound(m_slowRate)).arg(qRound(m_slowFps)),
                                 T("Render-Cache: Smart"));
    const ProxyManager* proxies = m_engine->proxies();
    int missing = 0, pending = 0;
    if (proxies->enabled()) // proxy media switched off: the user wants the originals
        for (const QString& f : std::as_const(m_slowFiles)) {
            if (proxies->isPending(f)) ++pending;
            else if (!proxies->hasProxy(f)) ++missing;
        }
    if (missing > 0)
        m_viewer->setHint(T("Die Wiedergabe ruckelt (%1 von %2 Bildern/s): Proxy-Medien machen die Vorschau flüssig, "
                            "der Export nutzt weiter die Originale")
                              .arg(qRound(m_slowRate)).arg(qRound(m_slowFps)),
                          T("Proxy-Medien erzeugen"));
    else if (pending > 0)
        m_viewer->setHint(T("Proxy-Medien werden erzeugt (noch %1) …").arg(pending));
    else {
        m_slowFiles.clear(); // all there (or switched off): a new measurement decides again
        m_viewer->setHint({});
    }
}

bool MainWindow::expensiveAtPlayhead() const
{
    if (m_engine->mode() == Engine::Mode::Source) return false;
    const int at = m_engine->position();
    const Timeline tl = m_project->renderTimeline();
    for (const Track& track : tl.video)
        for (const Clip& c : track.clips)
            if (c.kind == ClipKind::Media && c.enabled && c.start <= at && at < c.end() && RenderCache::isExpensive(c))
                return true;
    return false;
}

QStringList MainWindow::playingVideoFiles() const
{
    QStringList out;
    auto add = [&](const QString& path) {
        const MediaInfo* m = m_project->mediaInfo(path);
        if (m && m->hasVideo && !m->isImage && !out.contains(path) && QFileInfo::exists(path)) out << path;
    };
    if (m_engine->mode() == Engine::Mode::Source) {
        add(m_engine->sourcePath());
        return out;
    }
    auto scan = [&](const Timeline& tl) {
        for (const Track& track : tl.video)
            for (const Clip& c : track.clips)
                if (c.kind == ClipKind::Media && c.enabled) add(c.mediaPath);
    };
    const Timeline tl = m_project->renderTimeline();
    scan(tl);
    if (tl.nested)
        for (const Timeline& nested : *tl.nested) scan(nested);
    return out;
}
