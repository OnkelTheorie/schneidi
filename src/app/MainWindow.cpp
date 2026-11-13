// SPDX-License-Identifier: AGPL-3.0-only
#include "app/MainWindow.h"
#include "app/OpenTrace.h"
#include "core/StartupTrace.h"
#include "app/TourManager.h"
#include "ui/TourOverlay.h"
#include "app/AiKeys.h"
#include "app/AiLog.h"
#include "app/ExtensionsDialog.h"
#include "app/TranscribeDialog.h"
#include "app/SpeechDialog.h"
#include "core/Captions.h"
#include "core/Speech.h"
#include "core/TranscriptCut.h"
#include "engine/Extensions.h"
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
#include "core/ProxyChoice.h"
#include "core/Selection.h"
#include "core/StartTemplates.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/video/Hardware.h"
#include "engine/GradedThumbs.h"
#include "engine/MediaCache.h"
#include "engine/ClipFormat.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "engine/Stabilizer.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"
#include "ui/BackgroundJobs.h"
#include "ui/ProxyProgress.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "ui/EffectsLibrary.h"
#include "ui/comp/CompShared.h"
#include "ui/MediaStorage.h"
#include "ui/ColorPanel.h"
#include "ui/ScopesPanel.h"
#include "ui/Inspector.h"
#include "ui/KeyColorPicker.h"
#include "ui/KeyPickWindow.h"
#include "ui/Mixer.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/comp/CompCanvas.h"
#include "ui/comp/CompPage.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QCursor>
#include <QScreen>
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
#include <utility>

QString MainWindow::appName()
{
    return "schneidi " + QCoreApplication::applicationVersion().section(" (", 0, 0);
}

QRect MainWindow::fitToScreen(const QRect& wanted, const QRect& available, const QMargins& frame)
{
    const QRect room = available.marginsRemoved(frame);
    if (room.width() < 100 || room.height() < 100) return room; // a tiny screen: all of it
    const bool fits = wanted.isValid() && wanted.width() <= room.width() && wanted.height() <= room.height();
    if (fits && wanted.intersects(available)) { // on this screen: pushed fully inside
        QRect r = wanted;
        r.moveLeft(std::clamp(r.left(), room.left(), room.right() - r.width() + 1));
        r.moveTop(std::clamp(r.top(), room.top(), room.bottom() - r.height() + 1));
        return r;
    }
    QRect r(0, 0, room.width() * 9 / 10, room.height() * 9 / 10);
    if (fits) r.setSize(wanted.size()); // from another (disconnected) screen: its size, centred here
    r.moveCenter(room.center());
    return r;
}

QScreen* MainWindow::startScreen()
{
    if (QScreen* s = QGuiApplication::screenAt(QCursor::pos())) return s;
    return QGuiApplication::primaryScreen();
}

void MainWindow::placeOnScreen(const QRect& wanted)
{
    // Before the first show the window has no screen of its own yet (Qt gives it the primary one): it starts where
    // the user is looking, the screen under the mouse cursor (two monitors: not always on the primary one)
    QScreen* s = isVisible() && screen() ? screen() : startScreen();
    if (!s) return;
    if (!isVisible() && screen() != s) setScreen(s);
    if (layout()) layout()->activate(); // the minimum of the pages as they are now
    const QRect available = s->availableGeometry();
    QRect r = fitToScreen(wanted, available);
    // Never below the window's own minimum (the Edit page's panels): grown, then moved inside again; a screen smaller
    // than it gets the window at its top left
    const QSize least = minimumSizeHint().expandedTo(minimumSize());
    if (r.width() < least.width() || r.height() < least.height()) {
        r.setSize(r.size().expandedTo(least));
        r = fitToScreen(r, available);
        if (r.width() < least.width() || r.height() < least.height())
            r = QRect(available.marginsRemoved(kFrameEstimate).topLeft(), r.size().expandedTo(least));
    }
    setGeometry(r);
}

MainWindow::MainWindow(Engine* engine, QWidget* parent) : QMainWindow(parent), m_engine(engine)
{
    setWindowTitle(appName());
    resize(1600, 950);
    m_aiSettings = AiSettings::load(); // Settings › AI (MainWindowSettings.cpp)
    m_viewerRes = PreviewResolution::stored(PreviewResolution::Target::Viewer, m_engine->frameSize());
    m_monitorRes = PreviewResolution::stored(PreviewResolution::Target::Monitor, m_engine->frameSize());
    m_aiLog = new AiLog(this);
    m_aiLog->setLimit(m_aiSettings.logLength);

    m_project = new Project(this);
    m_selection = new Selection(this);
    m_editor = new Editor(m_project, m_selection, this);
    // Import presets for audio streams: the project's own (from its start template) or the settings' (all projects)
    const auto importPresets = [this] { m_editor->setStreamPresets(m_project->effectiveImportPresets()); };
    connect(m_project, &Project::importPresetsChanged, this, importPresets);
    importPresets();

    StartupTrace::mark("MainWindow ctor begin");
    buildLayout();
    StartupTrace::mark("buildLayout");
    buildActions();
    StartupTrace::mark("buildActions");
    setupChat();
    StartupTrace::mark("setupChat");
    applyAiMarkStyle();
    qApp->installEventFilter(this); // Mouse side buttons -> actions (keybindings.json, section "mouse")

    // model -> engine
    // Bundled: while dragging (fade handle, volume line, Inspector …) every mouse move changes the model;
    // rebuilding the preview immediately each time blocked the timeline (handle lagged behind).
    // Mixer changes are cheap (set live) and still go through immediately.
    m_engineTimer.setSingleShot(true);
    m_engineTimer.setInterval(30);
    connect(&m_engineTimer, &QTimer::timeout, this, [this] {
        m_engine->updateTimeline(previewTimeline()); // the Comp page may show one node of a graph
        OpenTrace::timelineSent();
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
    // A file overwritten under its name (re-rendered elsewhere): its length, picture/sound and streams again, probed
    // in the background; the pool entry follows without an undo step. Clips now reaching past its end are said.
    connect(m_engine, &Engine::mediaFilesChanged, this, [this](const QStringList& paths) {
        for (const QString& path : paths) m_probeCache.remove(path);
        QStringList pooled;
        for (const QString& path : paths)
            if (m_project->mediaInfo(path)) pooled << path;
        if (pooled.isEmpty()) return;
        QThreadPool::globalInstance()->start([guard = QPointer<MainWindow>(this), format = m_project->format(), pooled] {
            QVector<MediaInfo> probed;
            for (const QString& path : pooled) probed << Engine::probe(format, path);
            QMetaObject::invokeMethod(qApp, [guard, format, probed] {
                if (!guard || guard->m_project->format() != format) return; // counted in another frame rate now
                for (const MediaInfo& m : probed) {
                    if (!guard->m_project->refreshMedia(m)) continue;
                    if (const int past = guard->m_project->clipsPastEnd(m.path); past > 0)
                        guard->statusBar()->showMessage(
                            past == 1 ? T("%1 ist jetzt kürzer: 1 Clip reicht über sein Ende hinaus").arg(QFileInfo(m.path).fileName())
                                      : T("%1 ist jetzt kürzer: %2 Clips reichen über sein Ende hinaus")
                                            .arg(QFileInfo(m.path).fileName()).arg(past),
                            15000);
                }
            }, Qt::QueuedConnection);
        });
    });
    connect(m_project, &Project::timelineChanged, this, [this] {
        if (m_engine->mixerOnlyPending()) m_engine->updateTimeline(previewTimeline());
        else if (!m_engineTimer.isActive()) m_engineTimer.start();
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
    connect(proxies, &ProxyManager::qualityChanged, this, proxiesChanged); // other proxy files now
    setupProxyHint();
    setupBackgroundJobs();
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
    // Stabilization analysis done: the engine draws the preview again and lets the render cache start (Engine.cpp)
    connect(Stabilizer::instance(), &Stabilizer::resultReady, this, &MainWindow::updateRenderCacheBar);
    connect(renderCache, &RenderCache::failed, this,
            [this](const QString& msg) { QMessageBox::warning(this, T("Render-Cache"), msg); });
    // A source lost while the project is open (disk or share gone, file moved): named, the preview tries it again
    connect(m_engine, &Engine::sourceLost, this, [this](const QString& path) {
        statusBar()->showMessage(T("Quelldatei nicht lesbar: %1 – neuer Versuch beim nächsten Abspielen oder Springen")
                                     .arg(QFileInfo(path).fileName()),
                                 8000);
    });

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
    connect(m_inspector, &Inspector::compOpenRequested, this, &MainWindow::openComp);
    connect(m_inspector, &Inspector::drawToolRequested, this, [this] { setDrawTool(true); });
    connect(m_selection, &Selection::changed, this, [this] {
        if (m_keyPicker) stopKeyPick();
        if (m_page == Page::Color) playheadInto(selectedVideoClip()); // the viewer shows the clip being graded
    });
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (m_engine->timelinePositions()) m_transcript->setPlayhead(f);
    });
    connect(m_transcript, &TranscriptPanel::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_deliver, &DeliverPanel::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_deliver, &DeliverPanel::openExtensions, this, [this] {
        ExtensionsDialog ext(this);
        ext.exec();
        m_deliver->refreshEncoders();
    });
    connect(m_transcript, &TranscriptPanel::openExtensions, this, [this] {
        ExtensionsDialog ext(this);
        ext.exec();
    });
    connect(m_mediaPool, &MediaPool::sourceRequested, this, &MainWindow::showSource);
    connect(m_mediaPool, &MediaPool::sourceRequestedAt, this, [this](const QString& path, int frame) {
        m_sourcePos.insert(path, frame); // a hit of the material search: the source viewer opens there
        showSource(path);
    });
    connect(m_mediaPool, &MediaPool::sequenceOpenRequested, m_timeline, &TimelinePanel::openSequence);
    connect(m_mediaPool, &MediaPool::reframedCopyRequested, this, &MainWindow::reframedCopy);
    // The source viewer follows a file's shape set in the Media Pool (and its undo)
    connect(m_project, &Project::poolChanged, this, [this] {
        if (!sourceActive()) return;
        const MediaInfo* m = m_project->mediaInfo(m_engine->sourcePath());
        if (m && m->pictureAspect != m_engine->sourceAspect()) showSource(m->path);
    });
    connect(m_mediaPool, &MediaPool::stretchedRequested, this, [this](const QStringList& paths, bool on) {
        // The files (Media Pool tiles, clips placed later) and their clips in the open timeline: one undo step
        QVector<int> ids;
        for (const Track& t : m_project->timeline().video)
            for (const Clip& c : t.clips)
                if (c.kind == ClipKind::Media && paths.contains(c.mediaPath)) ids << c.id;
        const double aspect = on ? 4.0 / 3.0 : 0.0;
        m_project->undoStack()->beginMacro(on ? T("Seitenverhältnis entzerren") : T("Seitenverhältnis der Datei"));
        m_project->setMediaPictureAspect(paths, aspect);
        m_editor->setPictureAspect(ids, aspect);
        m_project->undoStack()->endMacro();
    });
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
    // Drag-fill of a template placeholder: what was done, briefly
    connect(tv, &TimelineView::slotFilled, this, [this](const QString& message, bool) { statusBar()->showMessage(message, 6000); });
    connect(tv, &TimelineView::message, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); });
    // An edit refused or only partly done (a ripple that would take picture and sound apart): why
    connect(m_editor, &Editor::notice, this, [this](const QString& text) { statusBar()->showMessage(text, 8000); });
    // Recipe dropped on a clip: applied after the drop (it may ask for a file)
    connect(tv, &TimelineView::recipeDropped, this, [this](int clipId, const QString& recipe) {
        // The clip that got it selected: the Comp page and the Inspector show it next (not another one selected before)
        QTimer::singleShot(0, this, [this, clipId, recipe] {
            if (applyRecipe({clipId}, recipe)) m_selection->set({clipId});
        });
    });
    // Double-click on a node's time range bar: the node on the Comp page, the playhead inside its range
    connect(tv, &TimelineView::compNodeOpenRequested, this, [this](int clipId, int node) {
        openComp(clipId);
        if (m_compPage) m_compPage->selectNode(node);
        const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
        const EffectInstance* e = c ? EffectRegistry::instance(*c, Comp::kEffectId) : nullptr;
        Comp::NodeRange r;
        if (!e || !Comp::activeRange(*e, node, &r)) return;
        int first = 0, end = 0;
        Comp::rangeFrames(*c, r, m_project->format().rate.fps(), &first, &end);
        const int at = m_timeline->view()->playhead();
        if (first < end && (at < first || at >= end)) {
            if (m_engine->mode() == Engine::Mode::Source) m_engine->showTimeline(first);
            else m_engine->seek(first);
        }
    });
    // After the drop (the drag source waits until dropEvent returns; a message box there would block it)
    connect(tv, &TimelineView::soundDropRequested, this, [this](const QByteArray& sound, int frame) {
        QTimer::singleShot(0, this, [this, sound, frame] { placeSound(sound, frame); });
    });
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
        // Proxy media of the clips' files (as in the Media Pool's menu; compound/multicam clips: the files inside)
        const auto addProxyEntries = [this, &menu] {
            const QStringList files = proxyFiles(int(ProxyChoice::Scope::Selected));
            if (files.isEmpty()) return;
            ProxyManager* proxies = m_engine->proxies();
            const QStringList missing = proxies->missing(files);
            QStringList withProxy, pending;
            for (const QString& f : files) {
                if (proxies->isPending(f)) pending << f;
                else if (proxies->hasProxy(f)) withProxy << f;
            }
            QAction* make = menu.addAction(ProxyChoice::label(T("Proxy-Medien erzeugen"), int(files.size()),
                                                              int(missing.size()), int(pending.size())),
                                           this, [this] { generateProxies(int(ProxyChoice::Scope::Selected)); });
            make->setObjectName("clip_proxy_generate");
            make->setEnabled(!missing.isEmpty());
            if (!withProxy.isEmpty())
                menu.addAction(T("Proxy-Medien löschen"), this, [proxies, withProxy] { proxies->remove(withProxy); })
                    ->setObjectName("clip_proxy_remove");
            if (!pending.isEmpty())
                menu.addAction(T("Proxy-Erzeugung abbrechen"), this, [proxies, pending] { proxies->cancel(pending); });
        };
        for (const char* id : {"clip_speed", "retime_controls", "clip_speed_reset", "curve_editor", "accents", "auto_reframe", "crop_bars", "stretched_43", "blur_names",
                               "normalize_audio", "audio_ducking", "fit_music", "stream_presets_learn", "",
                               "slot_mark", "slot_remove", "",
                               "toggle_enabled", "link_clips",
                               "render_cache_clip", "proxies", "save_effect_preset", "", "comp_open", "comp_from", "compound_create", "compound_open", "compound_decompose", "",
                               "delete", "ripple_delete"}) {
            if (!*id) {
                menu.addSeparator();
                continue;
            }
            if (QString(id) == "proxies") {
                addProxyEntries();
                continue;
            }
            if (QString(id) == "accents") { // zoom punch, flash, shake as keyframes on the selected clips there
                const int frame = m_timeline->view()->playhead();
                QVector<int> ids;
                for (int cid : m_editor->clipIdsOf(m_selection->ids()))
                    if (TrackRef r; const Clip* c = TimelineOps::findClip(m_project->timeline(), cid, &r))
                        if (r.kind == TrackKind::Video && frame >= c->start && frame < c->end()) ids << cid;
                QMenu* accents = menu.addMenu(T("Akzent am Abspielkopf"));
                accents->setEnabled(!ids.isEmpty());
                for (const Accents::Preset& p : Accents::presets())
                    connect(accents->addAction(T(p.name)), &QAction::triggered, this, [this, ids, frame, p] {
                        m_editor->addAccents(ids, {frame}, p.amounts, T("Akzent: %1").arg(T(p.name)));
                    });
                continue;
            }
            QAction* a = InputBindings::instance().action(id);
            if (!a) continue;
            if ((QString(id) == "normalize_audio" || QString(id) == "audio_ducking" || QString(id) == "fit_music") && !audio)
                continue;
            if (QString(id) == "stream_presets_learn" && !streamPresetsClip()) continue;
            if (QString(id) == "slot_mark" || QString(id) == "slot_remove") { // the one that fits the clips
                const QVector<int> ids = m_editor->clipIdsOf(m_selection->ids());
                const bool marked = std::any_of(ids.begin(), ids.end(), [this](int cid) {
                    const Clip* c = TimelineOps::findClip(m_project->timeline(), cid);
                    return c && c->slot.isSet();
                });
                if (marked != (QString(id) == "slot_remove")) continue;
            }
            if (QString(id) == "compound_open" && !compound && !multicam) continue;
            if ((QString(id) == "comp_open" || QString(id) == "comp_from") && !selectedVideoClip()) continue;
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
            if (QString(id) == "crop_bars" || QString(id) == "stretched_43") {
                const QVector<int> media = m_editor->reframeTargets(m_editor->clipIdsOf(m_selection->ids()));
                if (media.isEmpty()) continue; // video clips only
                if (QString(id) == "stretched_43")
                    a->setChecked(std::all_of(media.begin(), media.end(), [this](int cid) {
                    const Clip* c = TimelineOps::findClip(m_project->timeline(), cid);
                    return c && c->pictureAspect > 0;
                }));
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
    connect(tv, &TimelineView::rangeDropRequested, this,
            [this](const QString& path, int in, int out, int frame, int track, PlaceParts parts) {
        // Looked at in the Media Storage without importing: into the selected bin first
        if (!m_editor->importPreviewSource(path, m_mediaPool->currentBin())) return;
        if (m_project->frameRateLocked()) {
            m_editor->placeSourceRange(path, in, out, frame, track, 1.0, parts);
            return;
        }
        QTimer::singleShot(0, this, [this, path, in, out, frame, track, parts] { // confirmation only after the drop (see onDrop)
            offerClipFormat({path}, true);
            m_editor->placeSourceRange(path, in, out, frame, track, 1.0, parts);
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
    setupVoiceover();
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
    connect(m_effects, &EffectsLibrary::titleRequested, this,
            [this, tv](int kind) { m_editor->addTitle(tv->playhead(), -1, 0, TitleKind(kind)); });
    // Filters: onto the selected clips of the effect's kind, without a selection onto the clip at the playhead
    connect(m_effects, &EffectsLibrary::effectRequested, this, [this, tv](const QString& id) {
        const QVector<int> targets = m_editor->effectTargets(tv->playhead(), Editor::effectKind(id));
        if (CompUi::isRecipe(id)) { // a compositing recipe: its graph onto the clips (one undo step)
            if (!targets.isEmpty()) applyRecipe(targets, id.mid(int(qstrlen(CompUi::kRecipePrefix))));
            return;
        }
        m_editor->addEffect(targets, id);
    });
    // Sounds: listen in the Viewer (the source, from its start); placing is dragging them into the timeline
    connect(m_effects, &EffectsLibrary::soundPreviewRequested, this, [this](const QString& file) {
        m_sourcePos.remove(file);
        previewFile(file);
        m_engine->play();
    });
    tv->setProbe([this](const QString& path) { return probeCached(path); });
    m_mediaCache = new MediaCache(this);
    tv->setMediaCache(m_mediaCache);
    // Project settings -> engine (profile anew), cache, Inspector; comes before timelineChanged
    connect(m_project, &Project::formatChanged, this, &MainWindow::onFormatChanged);
    // Another timeline opened or a timeline's own resolution: the viewer renders at that size
    connect(m_project, &Project::timelineFormatChanged, this, &MainWindow::onTimelineFormatChanged);

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
    StartupTrace::mark("MainWindow ctor end");
}

void MainWindow::connectColorPage()
{
    TimelineView* tv = m_timeline->view();
    m_colorPanel->setPlayhead(tv->playhead());
    connect(m_engine, &Engine::positionChanged, m_colorPanel, [this](int f) {
        if (m_engine->timelinePositions()) m_colorPanel->setPlayhead(f);
    });
    connect(m_colorPanel, &ColorPanel::seekRequested, tv, &TimelineView::seekRequested);
    connect(m_engine, &Engine::frameReady, m_scopes, &ScopesPanel::setFrame);
    m_colorPanel->setTourButtonAllowed(!m_tourSaved.active);
    if (m_tours) connect(m_colorPanel, &ColorPanel::tourRequested, m_tours, [this] { m_tours->start("color"); });
}

void MainWindow::warmUpVideoDecoding()
{
    // The graphics card's decoder (CUDA: 100-250 ms to open) opens beside the rest of the work, not when the first
    // picture waits for it. Not without a screen (tests, --screenshot): nothing plays there.
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) Video::Hardware::warmUp();
}

void MainWindow::importFiles(const QStringList& paths, bool placeOnTimeline, bool adoptFormat)
{
    warmUpVideoDecoding();
    m_mediaPool->importFiles(paths);
    if (!placeOnTimeline) return;
    // Test helper (--demo): put files one after another on V1/A1; project format from the first clip without asking
    if (adoptFormat) offerClipFormat(paths, false);
    m_editor->addMediaAt(paths, TimelineOps::endFrame(m_project->timeline()));
}

MediaInfo MainWindow::probeCached(const QString& path)
{
    // On the UI thread (the timeline's drag-enter, a template slot): a quick look that never runs the long stream
    // analysis; when it was skipped, the full probe follows in the background (kept for the next drag-enter and the
    // import of the drop)
    if (std::optional<ProbeResult> known = m_probeCache.find(path)) return known->info;
    const ProbeResult quick = ProbeCache::probe(path, m_project->format().rate, false);
    if (quick.partial) probeLater(path);
    else m_probeCache.insert(quick);
    return quick.info;
}

void MainWindow::onDrop(const QStringList& paths, int frame, int track, PlaceParts parts)
{
    if (paths.size() == 1 && MediaPool::isTitleItem(paths.first())) {
        const QString& p = paths.first();
        m_editor->addTitle(frame, track, 0,
                           p == MediaPool::CounterItem    ? TitleKind::Counter
                           : p == MediaPool::ProgressItem ? TitleKind::Progress
                           : p == MediaPool::ShapeItem    ? TitleKind::Shape
                                                          : TitleKind::Text);
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
        onDrop(media, frame, track, parts);
        return;
    }
    // Directly from the file manager: first into the Media Pool, then onto the timeline (like DaVinci)
    QStringList unknown;
    for (const QString& p : paths)
        if (!m_project->mediaInfo(p)) unknown << p;
    // Empty timeline: possibly ask about the project format – only after the drop (no modal query in the middle
    // of drag & drop, the source, e.g. the file manager, would otherwise wait)
    const auto place = [this, paths, frame, track, parts] {
        if (!m_project->frameRateLocked()) offerClipFormat(paths, true);
        m_editor->addMediaAt(paths, frame, track, parts);
    };
    if (!unknown.isEmpty()) {
        // New files are probed after the drop returned: the import waits for its probes (a progress dialog, an
        // "unreadable" message) and the drag source (Windows Explorer over OLE) would wait with it
        QTimer::singleShot(0, this, [this, unknown, place] {
            m_mediaPool->importFiles(unknown);
            place();
        });
        return;
    }
    if (m_project->frameRateLocked()) place();
    else QTimer::singleShot(0, this, place);
}

void MainWindow::offerClipFormat(const QStringList& paths, bool ask)
{
    if (m_project->frameRateLocked()) return; // only in an empty timeline
    // A timeline of its own resolution (a 9:16 short) keeps it like a fixed one: only the frame rate is offered
    const Sequence* open = m_project->sequence(m_project->currentSequence());
    const bool fixedSize = m_project->fixedResolution() || (open && open->hasOwnSize());
    if (fixedSize && m_project->fixedFrameRate()) return; // start template / Project Settings
    // The first clip with video decides (still images and audio have no format of their own)
    for (const QString& path : paths) {
        const MediaInfo* info = m_project->mediaInfo(path);
        if (!info || !info->hasVideo || info->isImage) continue;
        const ClipFormat cf = detectClipFormat(path);
        if (!cf.ok) return;
        const ProjectFormat cur = m_project->format();
        // A fixed resolution or frame rate (start template, Project Settings) stays as it is
        ProjectFormat want;
        want.width = fixedSize ? cur.width : cf.width;
        want.height = fixedSize ? cur.height : cf.height;
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
        def = T("Verbundclip %1").arg(i);
        if (!used.contains(def)) break;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, T("Neuer Verbundclip"), T("Name:"), QLineEdit::Normal, def, &ok);
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
    m_engine->showSource(path, m_sourcePos.value(path, m->markIn >= 0 ? m->markIn : 0), m->pictureAspect);
}

void MainWindow::previewFile(const QString& path)
{
    if (m_project->mediaInfo(path)) { // in the Media Pool already: its clip with its marks
        showSource(path);
        return;
    }
    const MediaInfo& before = m_editor->previewSource();
    MediaInfo info;
    const std::optional<ProbeResult> cached = m_probeCache.find(path);
    if (cached) {
        info = cached->info;
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
    if (!cached) probeLater(path);
}

void MainWindow::updateLiveWaiting()
{
    if (!m_liveWaiting) return;
    if (!m_liveTourMessage.isEmpty()) m_liveWaiting(m_liveTourMessage);
    else if (m_mediaPool && m_mediaPool->probing())
        m_liveWaiting("schneidi is reading media files the user imports; try again in a moment");
    else m_liveWaiting({});
}

void MainWindow::probeLater(const QString& path)
{
    // Probing opens the file once more (mostly ~1 ms, other containers up to ~0.5 s, a long stream analysis up to a
    // minute): in the background, the window stays usable. The Viewer's file takes the result.
    if (m_probing.contains(path)) return;
    m_probing.insert(path);
    QThreadPool::globalInstance()->start([guard = QPointer<MainWindow>(this), format = m_project->format(), path] {
        const ProbeResult probed = ProbeCache::probe(path, format.rate);
        QMetaObject::invokeMethod(qApp, [guard, format, probed] {
            if (!guard) return;
            guard->m_probing.remove(probed.info.path);
            if (guard->m_project->format() != format) return; // lengths count in another frame rate now
            guard->m_probeCache.insert(probed); // dropped if the file changed meanwhile
            const MediaInfo& shown = guard->m_editor->previewSource();
            if (shown.path != probed.info.path) return; // another file meanwhile, or imported
            MediaInfo info = probed.info;
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
    sourceEditPath(mode, editSource());
}

void MainWindow::sourceEditPath(Editor::SourceEditMode mode, const QString& path)
{
    if (path.isEmpty()) return;
    // Looked at in the Media Storage without importing: into the selected bin first (no undo step, like every
    // import; the edit is one). Not probed yet (just clicked): nothing to place yet.
    if (!m_editor->importPreviewSource(path, m_mediaPool->currentBin())) {
        QApplication::beep();
        return;
    }
    offerClipFormat({path}, true); // first clip in an empty timeline: adapt project format?
    TimelineView* tv = m_timeline->view();
    const int srcPos = sourceActive() && m_engine->sourcePath() == path ? m_engine->position() : m_sourcePos.value(path, 0);
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
    RenderCache* cache = m_engine->renderCache();
    QVector<TimelineView::CacheSpan> spans;
    bool allDone = true;
    for (const RenderCache::Span& s : cache->spans(m_project->timeline())) {
        spans << TimelineView::CacheSpan{s.start, s.end, s.done, s.name};
        allDone &= s.done >= 1.0;
    }
    m_timeline->view()->setRenderCacheSpans(spans, cache->isPaused());
    updateRenderCacheJob(allDone);
}

void MainWindow::updateRenderCacheJob(bool allDone)
{
    // Rendering in the background shows as a job (bottom right) with a ✕ that pauses it: without it the user saw
    // only the bar above the ruler and took it for a proxy he could not stop
    if (!m_backgroundJobs) return;
    RenderCache* cache = m_engine->renderCache();
    const QString id = "render_cache";
    if (cache->isRendering()) {
        BackgroundJobs::Job job;
        job.text = T("Render-Cache – %1 – %2 %").arg(cache->currentName()).arg(int(cache->currentProgress() * 100));
        job.fraction = cache->currentProgress();
        QStringList tip{T("Rendert Clips mit Effekten oder geänderter Geschwindigkeit vor, damit die Vorschau flüssig "
                          "läuft (der Export rechnet immer aus den Originalen).")};
        if (const QStringList waiting = cache->pendingNames(); !waiting.isEmpty())
            tip << T("Danach: %1").arg(waiting.join(", "));
        tip << T("✕ pausiert bis zur nächsten Änderung. Ausschalten: Wiedergabe › Render-Cache › Aus");
        job.toolTip = tip.join("\n");
        job.cancel = [cache] { cache->pauseUntilChange(); };
        job.cancelToolTip = T("Render-Cache pausieren (bis zur nächsten Änderung)");
        m_backgroundJobs->setJob(id, job);
        m_renderCacheJob = true;
        return;
    }
    if (!m_renderCacheJob) return;
    if (cache->pendingCount() > 0) { // the next clip starts in a moment
        BackgroundJobs::Job job;
        job.text = T("Render-Cache – wartet (%1)").arg(cache->pendingNames().join(", "));
        job.fraction = -1;
        job.cancel = [cache] { cache->pauseUntilChange(); };
        job.cancelToolTip = T("Render-Cache pausieren (bis zur nächsten Änderung)");
        m_backgroundJobs->setJob(id, job);
        return;
    }
    m_renderCacheJob = false;
    if (cache->isPaused()) m_backgroundJobs->finishJob(id, T("Render-Cache pausiert bis zur nächsten Änderung"));
    else if (allDone && cache->mode() != RenderCache::Mode::Off) m_backgroundJobs->finishJob(id, T("Render-Cache fertig"));
    else m_backgroundJobs->removeJob(id);
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
    // The picture large in a window of its own (the viewer is often too small to hit the exact green)
    m_keyPickWindow = new KeyPickWindow(m_keyPicker, picture.size(), this);
    connect(m_keyPickWindow, &KeyPickWindow::closeRequested, this, &MainWindow::stopKeyPick);
    m_keyPickWindow->showOn(screen());
    m_inspector->setKeyPicking(true); // the stopKeyPick() above switched the button off
}

void MainWindow::stopKeyPick()
{
    m_inspector->setKeyPicking(false);
    if (m_tours) m_tours->overlay()->setPaused(false); // the tour's bubble back
    if (KeyPickWindow* window = std::exchange(m_keyPickWindow, nullptr)) {
        window->disconnect(this);
        window->hide();
        window->deleteLater();
        activateWindow();
        // Keys go back to the work area (graph or timeline), not to whatever Inspector field Qt picks next: typed
        // letters there hit shortcuts (Tab after a pick opened no node search, "d" disabled the clip). Deferred: the
        // Inspector rebuilds its rows for the new color first
        QTimer::singleShot(0, this, [this] {
            if (m_page == Page::Comp && m_compPage) m_compPage->canvas()->setFocus(Qt::OtherFocusReason);
            else m_timeline->view()->setFocus(Qt::OtherFocusReason);
        });
    }
    if (!m_keyPicker) return;
    qApp->removeEventFilter(m_keyPicker); // its Esc filter ends now, not when deleteLater gets to it
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
    const Editor::Retime cur{first->speed, first->reverse, first->freeze, first->keepPitch, first->interpolate};
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
    onTimelineFormatChanged();
    // Every load comes here (also with the same format): graded tiles are kept by clip id, and the next project has
    // clips with the same ids
    m_mediaCache->graded()->clear();
    m_probeCache.clear();
    m_editor->setPreviewSource({}); // its length counts in the old frame rate; the Viewer shows the timeline now
    if (f.rate == oldRate) return;
    // Lengths in the Media Pool count in project frames -> re-read (only possible in an empty timeline,
    // undo/redo included)
    m_project->recountMedia(oldRate, [this](const QString& path) { return m_engine->probe(path).length; });
}

void MainWindow::onTimelineFormatChanged()
{
    const ProjectFormat f = m_project->timelineFormat();
    m_engine->setFormat(f); // nothing when the same; the timeline rebuilds in the following timelineChanged()
    updatePreviewEdge();    // "half" depends on the project's size
    m_mediaCache->setFormat(f); // thumbnails in the timeline's shape
    m_inspector->setFrameSize(f.size());
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    const QEvent::Type t = event->type();
    // Double-click on a clip with a compositing graph in the timeline: open it on the Comp page (the press before
    // selected it; the timeline itself has no double-click on clips)
    if (t == QEvent::MouseButtonDblClick && m_timeline && obj == m_timeline->view()) {
        const auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton && me->position().x() >= TimelineView::kHeaderW)
            if (const int id = selectedVideoClip())
                if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
                    c && EffectRegistry::has(*c, Comp::kEffectId)) {
                    QTimer::singleShot(0, this, [this, id] { openComp(id); });
                    return true;
                }
    }
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
    if (!dlg.languageHint().isEmpty()) QMessageBox::information(this, T("Untertitel aus Audio erzeugen"), dlg.languageHint());
}

// Text -> voice clip (Piper) at the playhead on the first audio track free there; its words become the file's
// transcript (Transcript panel, word captions)
void MainWindow::speechDialog()
{
    m_engine->pause();
    SpeechDialog dlg(Speech::folder(m_projectPath), this);
    connect(&dlg, &SpeechDialog::openExtensions, this, [this] {
        ExtensionsDialog ext(this);
        ext.exec();
    });
    connect(&dlg, &SpeechDialog::preview, this, [this](const QString& path) {
        previewFile(path);
        m_engine->play();
    });
    if (dlg.exec() != QDialog::Accepted) return;
    const Tts::Result r = dlg.result();
    if (!m_project->mediaInfo(r.path)) {
        const MediaInfo info = probeCached(r.path);
        if (info.length <= 0) return;
        m_project->addMedia(info);
    }
    const MediaInfo m = *m_project->mediaInfo(r.path);
    const int at = m_timeline->view()->playhead();
    const int track = Speech::freeAudioTrack(m_project->timeline(), at, at + m.length);
    // The words as the file's transcript (Transcript panel, op 'captions' later), as the CLI op 'speak' does
    if (!r.words.isEmpty())
        m_project->setTranscript(m.path, MediaTranscript{0, Speech::voiceLanguage(Extensions::voiceName(r.voice)), r.words});
    QUndoStack* undo = m_project->undoStack();
    undo->beginMacro(T("Sprechertext"));
    const int len = m_editor->placeSourceRange(m.path, 0, m.length - 1, at, track);
    if (len > 0 && dlg.captions()) {
        // Word captions of this clip only (like the Transcript panel's, karaoke style)
        const double fps = m_project->frameRate();
        QVector<TranscriptCut::Word> words
            = TranscriptCut::timelineWords(m_project->timeline(), m_project->media(), fps, track, true);
        words.removeIf([&](const TranscriptCut::Word& w) { return w.from < at || w.from >= at + len; });
        Captions::Options o;
        o.maxGapFrames = int(std::lround(0.6 * fps));
        o.minFrames = int(std::lround(0.4 * fps));
        CaptionStyle style;
        style.mode = CaptionMode::Karaoke;
        if (const QVector<SubtitleCue> cues = Captions::fromWords(words, o); !cues.isEmpty())
            m_editor->addCaptions(cues, T("Wort-Untertitel"), style);
    }
    undo->endMacro();
}

void MainWindow::exportSubtitlesDialog()
{
    // The visible subtitle track (otherwise the first one with entries) into the chosen file, every other track with
    // entries next to it as "<name>.<track name>.srt" (Subtitles::srtFiles); a ".vtt" name writes WebVTT
    const Timeline& tl = m_project->timeline();
    const QVector<Subtitles::SrtFile> preview = Subtitles::srtFiles(tl, "x.srt");
    if (preview.isEmpty()) {
        QMessageBox::information(this, T("Untertitel exportieren"), T("Die Timeline enthält keine Untertitel."));
        return;
    }
    const SubtitleTrack& t = tl.subtitles[preview[0].track];
    const QString base = m_projectPath.isEmpty() ? QString() : QFileInfo(m_projectPath).absolutePath();
    const QString name = (t.name.isEmpty() ? (m_projectPath.isEmpty() ? QString("untertitel") : QFileInfo(m_projectPath).completeBaseName()) : t.name) + ".srt";
    QString path = QFileDialog::getSaveFileName(this, T("Untertitel exportieren"), base.isEmpty() ? name : base + "/" + name,
                                                T("Untertitel (*.srt *.vtt)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".srt";
    const QVector<Subtitles::SrtFile> files = Subtitles::srtFiles(tl, path);
    // The file dialog asked only about the chosen file: the other tracks' files next to it would be replaced silently
    QStringList existing;
    for (int i = 1; i < files.size(); ++i)
        if (QFileInfo::exists(files[i].path)) existing << QFileInfo(files[i].path).fileName();
    if (!existing.isEmpty()
        && QMessageBox::question(this, T("Untertitel exportieren"),
                                 T("Die weiteren Untertitelspuren ersetzen diese Dateien:\n%1").arg(existing.join('\n')))
               != QMessageBox::Yes)
        return;
    for (const Subtitles::SrtFile& srt : files) {
        QSaveFile f(srt.path);
        if (!f.open(QIODevice::WriteOnly) || f.write(Subtitles::toFile(srt.path, tl.subtitles[srt.track].cues, m_project->frameRate())) < 0
            || !f.commit()) {
            QMessageBox::warning(this, T("Untertitel exportieren"), T("Untertiteldatei kann nicht geschrieben werden: %1").arg(srt.path));
            return;
        }
    }
}

void MainWindow::setupBackgroundJobs()
{
    m_backgroundJobs = new BackgroundJobs;
    m_pageBarRight->addWidget(m_backgroundJobs); // right in the page bar (the status bar is hidden)
    ProxyManager* proxies = m_engine->proxies();
    ProxyProgress* progress = ProxyProgress::of(proxies);
    connect(progress, &ProxyProgress::changed, m_backgroundJobs, [this, proxies, progress] {
        if (!progress->active()) return;
        BackgroundJobs::Job job;
        job.text = progress->text();
        job.fraction = progress->fraction();
        job.toolTip = progress->waitingText();
        job.cancel = [proxies] { proxies->cancelAll(); }; // the same as the Media Pool's cancel
        m_backgroundJobs->setJob("proxies", job);
    });
    connect(progress, &ProxyProgress::finished, m_backgroundJobs,
            [this, progress] { m_backgroundJobs->finishJob("proxies", progress->finishedText()); });
    // Failed proxies: never a message box (a batch of failures opened one per file and stopped the work) - one line
    // in the status line, and one entry here that stays with the details per file (ffmpeg's last lines) in its tooltip
    connect(progress, &ProxyProgress::failuresChanged, m_backgroundJobs, [this, progress] {
        if (progress->failures().isEmpty()) {
            m_backgroundJobs->removeJob("proxy_errors");
            return;
        }
        BackgroundJobs::Job job;
        job.text = progress->failureText();
        job.toolTip = progress->failureDetails();
        job.notice = true;
        job.cancel = [progress] { progress->clearFailures(); };
        m_backgroundJobs->setJob("proxy_errors", job);
        statusBar()->showMessage(T("Proxy fehlgeschlagen: %1 – Details unten rechts")
                                     .arg(QFileInfo(progress->failures().back().file).fileName()),
                                 8000);
    });
}

void MainWindow::setupProxyHint()
{
    m_playClock.start();
    connect(m_engine, &Engine::speedChanged, this, [this](double speed) {
        if (speed == 1.0) m_playMeter.start(m_project->frameRate(), m_playClock.elapsed());
        else m_playMeter.stop();
    });
    ProxyManager* proxies = m_engine->proxies();
    RenderCache* cache = m_engine->renderCache();
    connect(m_engine, &Engine::positionChanged, this, [this, proxies] {
        if (!m_playMeter.frame(m_playClock.elapsed()) || m_proxyHintClosed || m_slow) return;
        m_slow = true;
        m_slowFiles = playingVideoFiles();
        m_slowEffects = expensiveAtPlayhead();
        m_slowWithProxies = true;
        if (proxies->enabled()) // switched off: the user wants the originals, proxies are no offer
            for (const QString& f : std::as_const(m_slowFiles))
                if (!proxies->hasProxy(f)) m_slowWithProxies = false;
        m_slowRate = m_playMeter.rate();
        m_slowFps = m_playMeter.fps();
        m_slowRes = m_viewerRes;
        qInfo("Playback too slow: %.1f of %.2f pictures/s at a %dx%d preview", m_slowRate, m_slowFps,
              m_engine->previewSize().width(), m_engine->previewSize().height());
        updateProxyHint();
    });
    connect(m_viewer, &Viewer::hintAccepted, this, [this, proxies, cache] {
        switch (std::exchange(m_hintOffer, HintOffer::None)) {
        case HintOffer::SmallerPreview:
            // Exactly the size the hint named (the resolution changed since: offered anew). Done at once (the viewer
            // shows the new size for a moment); still too slow: a new measurement decides
            if (m_viewerRes != m_offeredFrom) return updateProxyHint();
            m_slow = false;
            m_slowFiles.clear();
            m_slowEffects = false;
            setPreviewResolution(PreviewResolution::Target::Viewer, m_offeredRes); // (updates the hint)
            restartPlayMeter();
            return;
        case HintOffer::RenderCache:
            // Effects were the cost: their clips get rendered in the background; if it still stutters afterwards,
            // a new measurement offers proxies
            m_slow = false;
            m_slowFiles.clear();
            m_slowEffects = false;
            cache->setMode(RenderCache::Mode::Smart);
            restartPlayMeter();
            return updateProxyHint();
        case HintOffer::Proxies: {
            QStringList missing;
            for (const QString& f : std::as_const(m_slowFiles))
                if (!proxies->isPending(f) && !proxies->hasProxy(f)) missing << f;
            proxies->generate(missing);
            return updateProxyHint(); // (generate may queue nothing: no signal, the hint must not stay with a dead button)
        }
        case HintOffer::None: return;
        }
    });
    connect(m_viewer, &Viewer::hintClosed, this, [this] {
        m_proxyHintClosed = true;
        m_slow = false;
        m_slowFiles.clear();
        m_hintOffer = HintOffer::None;
    });
    connect(cache, &RenderCache::modeChanged, this, &MainWindow::updateProxyHint);
    connect(proxies, &ProxyManager::queueChanged, this, &MainWindow::updateProxyHint);
    connect(proxies, &ProxyManager::proxyChanged, this, &MainWindow::updateProxyHint);
    connect(proxies, &ProxyManager::enabledChanged, this, &MainWindow::updateProxyHint);
    connect(proxies, &ProxyManager::qualityChanged, this, &MainWindow::updateProxyHint);

    // Proxies on import (Settings › General): every way a file comes into the open project (Media Pool, drop on the
    // timeline, watch folders, the live MCP/CLI import) says mediaImported; undo/redo and loading a project do not
    connect(m_project, &Project::mediaImported, this, [this, proxies](const QStringList& paths) {
        if (m_importedMedia.isEmpty())
            QTimer::singleShot(0, this, [this, proxies] {
                proxies->importedFiles(std::exchange(m_importedMedia, {}),
                                       [project = QPointer<Project>(m_project)](const QString& path) {
                                           return project && project->mediaInfo(path);
                                       });
            });
        m_importedMedia << paths;
    });
    connect(m_project, &Project::loaded, proxies, [this, proxies] {
        m_importedMedia.clear();
        proxies->dropImports();
        ProxyProgress::of(proxies)->clearFailures(); // the old project's files
    });
}

bool MainWindow::offersRenderCache() const
{
    // Proxies only make decoding cheaper; with effects in the picture the smart render cache helps more (it also
    // plays a quickly decodable file instead of the original)
    return !m_slowFiles.isEmpty() && m_slowEffects && m_engine->renderCache()->mode() != RenderCache::Mode::Smart;
}

void MainWindow::updateProxyHint()
{
    // What helps first: a preview the user raised above the automatic size goes back to it (decoding is rarely the
    // cost then, proxies would leave every effect at as many pixels); then the smart render cache when effects play,
    // proxies for files without them; a smaller preview again when every file had its proxy already
    m_hintOffer = HintOffer::None;
    if (!m_slow) return m_viewer->setHint({});
    // Measured with another resolution choice (changed in the menu since): says nothing about this one
    const std::optional<PreviewResolution::Choice> smaller = m_viewerRes == m_slowRes ? smallerPreview() : std::nullopt;
    const auto offerSmaller = [&] {
        m_hintOffer = HintOffer::SmallerPreview;
        m_offeredRes = *smaller;
        m_offeredFrom = m_viewerRes;
        const QSize then = PreviewResolution::sizeFor(m_engine->frameSize(), previewEdgeFor(*smaller));
        m_viewer->setHint(T("Die Wiedergabe ruckelt (%1 von %2 Bildern/s) bei einer Vorschau von %3 Pixeln. Mit %4 läuft "
                            "sie flüssiger, der Export bleibt in voller Auflösung. Zurückstellen: Rechtsklick auf die "
                            "Vorschau › Vorschau-Auflösung")
                              .arg(qRound(m_slowRate))
                              .arg(qRound(m_slowFps))
                              .arg(PreviewResolution::sizeText(m_engine->previewSize()), PreviewResolution::sizeText(then)),
                          T("Vorschau verkleinern"));
    };
    if (smaller == PreviewResolution::Choice::Auto) return offerSmaller();
    if (offersRenderCache()) {
        m_hintOffer = HintOffer::RenderCache;
        return m_viewer->setHint(T("Die Wiedergabe ruckelt (%1 von %2 Bildern/s): Hier kosten die Effekte am meisten. "
                                   "Der Render-Cache „Smart“ rechnet Clips mit Effekten in Pausen vor, der Export nutzt "
                                   "weiter die Originale")
                                     .arg(qRound(m_slowRate)).arg(qRound(m_slowFps)),
                                 T("Render-Cache: Intelligent"));
    }
    const ProxyManager* proxies = m_engine->proxies();
    int missing = 0, pending = 0;
    if (proxies->enabled()) // proxy media switched off: the user wants the originals
        for (const QString& f : std::as_const(m_slowFiles)) {
            if (proxies->isPending(f)) ++pending;
            else if (!proxies->hasProxy(f)) ++missing;
        }
    if (missing > 0) {
        m_hintOffer = HintOffer::Proxies;
        m_viewer->setHint(T("Die Wiedergabe ruckelt (%1 von %2 Bildern/s): Proxy-Medien machen die Vorschau flüssig, "
                            "der Export nutzt weiter die Originale")
                              .arg(qRound(m_slowRate)).arg(qRound(m_slowFps)),
                          T("Proxy-Medien erzeugen"));
    } else if (pending > 0) {
        m_viewer->setHint(T("Proxy-Medien werden erzeugt (noch %1) …").arg(pending));
    } else if (m_slowWithProxies && smaller) {
        offerSmaller(); // measured with every proxy there: they did not help
    } else {
        // All there now (or switched off, or nothing smaller): a new measurement decides again
        m_slow = false;
        m_slowFiles.clear();
        m_viewer->setHint({});
    }
}

void MainWindow::restartPlayMeter()
{
    if (m_playMeter.running()) m_playMeter.start(m_playMeter.fps(), m_playClock.elapsed());
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

QStringList MainWindow::proxyFiles(int scope) const
{
    const auto sc = ProxyChoice::Scope(scope);
    if (sc != ProxyChoice::Scope::Selected || !m_selection->isEmpty())
        return ProxyChoice::files(*m_project, sc, m_selection->ids());
    const QStringList chosen = m_mediaPool->selectedMedia();
    QStringList out;
    for (const QString& f : ProxyChoice::files(*m_project, ProxyChoice::Scope::Pool))
        if (chosen.contains(f)) out << f;
    return out;
}

int MainWindow::generateProxies(int scope)
{
    ProxyManager* proxies = m_engine->proxies();
    const QStringList queue = proxies->missing(proxyFiles(scope));
    proxies->generate(queue);
    statusBar()->showMessage(queue.isEmpty() ? T("Keine fehlenden Proxy-Medien.")
                                             : T("Proxy-Medien werden erzeugt: %1 Dateien").arg(queue.size()),
                             5000);
    return int(queue.size());
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
