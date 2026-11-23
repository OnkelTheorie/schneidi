// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: another design or language while it runs - the widgets, menus and actions are made again under a still
// of the window (ui/SwitchCurtain) that then fades; the project and how the window was arranged stay. And the restart
// for a new interface size, which comes back as it was (app/SessionResume.h).
#include "app/MainWindow.h"

#include "app/AiChatPanel.h"
#include "app/AiLog.h"
#include "app/KeyTrainer.h"
#include "app/SessionResume.h"
#include "app/Theme.h"
#include "app/TourManager.h"
#include "app/VoiceoverRecorder.h"
#include "core/I18n.h"
#include "core/NestedLoop.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "engine/Engine.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "ui/BackgroundJobs.h"
#include "ui/ColorPanel.h"
#include "ui/DrawOverlay.h"
#include "ui/DeliverPanel.h"
#include "ui/FrameView.h"
#include "ui/MediaPool.h"
#include "ui/FullScreenViewer.h"
#include "ui/KeyColorPicker.h"
#include "ui/KeyPickWindow.h"
#include "ui/RenderQueuePanel.h"
#include "ui/ScopesPanel.h"
#include "ui/SecondScreen.h"
#include "ui/ShapeOverlay.h"
#include "ui/SwitchCurtain.h"
#include "ui/Viewer.h"
#include "ui/comp/CompPage.h"
#include "ui/direct/DirectMaterial.h"
#include "ui/direct/DirectPage.h"
#include "ui/direct/DirectRecipes.h"
#include "ui/direct/DirectSuggestions.h"
#include "ui/studio/StudioPage.h"
#include "ui/studio/StudioTracks.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

#include <optional>

namespace {

// A splitter's place in the window, the same in the widgets made again: the names (or class and number among its
// siblings of that class) from the window down
QString splitterKey(const QWidget* splitter, const QWidget* window)
{
    QStringList parts;
    for (const QWidget* w = splitter; w && w != window; w = w->parentWidget()) {
        int index = 0;
        if (const QObject* parent = w->parent())
            for (const QObject* c : parent->children()) {
                if (c == w) break;
                if (c->isWidgetType() && c->objectName() == w->objectName()
                    && qstrcmp(c->metaObject()->className(), w->metaObject()->className()) == 0)
                    ++index;
            }
        parts.prepend(QString("%1:%2#%3").arg(w->metaObject()->className(), w->objectName()).arg(index));
    }
    return parts.join('/');
}

QHash<QString, QList<int>> splitterSizes(const QWidget* window)
{
    QHash<QString, QList<int>> out;
    QSet<QString> twice;
    for (const QSplitter* s : window->findChildren<QSplitter*>()) {
        const QString key = splitterKey(s, window);
        if (out.contains(key)) twice.insert(key);
        out.insert(key, s->sizes());
    }
    for (const QString& key : twice) out.remove(key); // not to be told apart: left as laid out
    return out;
}

void applySplitterSizes(QWidget* window, const QHash<QString, QList<int>>& sizes)
{
    for (QSplitter* s : window->findChildren<QSplitter*>()) {
        const auto it = sizes.constFind(splitterKey(s, window));
        if (it != sizes.cend() && it->size() == s->count()) s->setSizes(*it);
    }
}

} // namespace

void MainWindow::applyLookChange()
{
    if (m_rebuildQueued) return;
    m_rebuildQueued = true;
    // After the dialog that changed it is gone (it is a child of this window, maybe still on the stack)
    QTimer::singleShot(0, this, [this] {
        m_rebuildQueued = false;
        if (I18n::languageChanged() || Theme::changed()) rebuildUi();
    });
}

bool MainWindow::rebuildUi(bool retry)
{
    if (m_rebuilding) return false;
    // Only from the main loop, never in the middle of something that lives in the widgets: a nested loop (a menu's or
    // dialog's exec, a drag, a progress loop - code on the stack still uses the widgets), an open popup, a Media Pool
    // import probing, a voice-over take, a Regie job, an AI's or the chat's command or turn. Tried again a moment later.
    const bool busy = NestedLoop::running() || QApplication::activeModalWidget() || QApplication::activePopupWidget()
                      || (m_mediaPool && m_mediaPool->probing()) || (m_voiceover && m_voiceover->recording())
                      || m_directCancel || m_directApplyCancel || m_directWishTurn || m_liveCommand || m_chatCommand
                      || m_review.active;
    if (busy) {
        if (retry && !m_rebuildQueued) {
            m_rebuildQueued = true;
            QTimer::singleShot(500, this, [this] {
                m_rebuildQueued = false;
                rebuildUi();
            });
        }
        return false;
    }
    const QScopedValueRollback rebuilding(m_rebuilding, true);
    // A tour gives the user's project back first (its steps point at widgets that go now)
    if (m_tours && (m_tourSaved.active || m_tours->runner()->running() || m_tours->asking())) m_tours->stop();
    if (m_keyPicker || m_keyPickWindow) stopKeyPick();
    // The window's still on top: everything below changes unseen, then it fades (reduced motion: it simply goes)
    SwitchCurtain* curtain = SwitchCurtain::cover(this);

    // ---- What the widgets hold
    // The layout as the user sees it: the large viewer and a viewer moved to a second monitor give their space back
    // first, so that the sizes taken are the page's own (both come back below)
    const bool large = m_large.active;
    setViewerLarge(false);
    restoreViewerArea();
    const bool fullScreenViewer = m_fullScreen != nullptr;
    if (fullScreenViewer) setViewerFullScreen(false);
    QScreen* outputScreen = nullptr;
    if (auto* screenOut = dynamic_cast<ScreenOutput*>(m_output.data())) outputScreen = screenOut->screen();
    if (outputScreen) showPreviewOn(nullptr); // made again below, in the new language
    const bool drawing = m_drawOverlay && m_drawOverlay->active();
    if (drawing) setDrawTool(false);

    const Page page = m_page;
    QVector<bool> toggles;
    for (QToolButton* b : tourToggles()) toggles << (b && b->isChecked());
    const std::optional<bool> scopes = m_scopes ? std::optional<bool>(!m_scopes->isHidden()) : std::nullopt;
    const QHash<QString, QList<int>> sizes = splitterSizes(this);
    TimelineView* tv = m_timeline->view();
    const ViewState view = tv->view();
    const int playhead = tv->playhead();
    const auto tool = tv->tool();
    const bool snapping = tv->snapping();
    const QImage picture = m_viewer->picture();
    const bool hadColor = m_colorPanel, hadComp = m_compPage, hadStudio = m_studioPage, hadDirect = m_directPage;
    const int compClip = m_compPage ? m_compPage->clip() : 0;
    std::optional<QPointF> studioView;
    bool studioFitPending = false;
    if (m_studioPage) {
        studioView = QPointF(m_studioPage->tracks()->view().pxPerFrame, m_studioPage->tracks()->view().leftFrame);
        studioFitPending = m_studioPage->tracks()->fitPending();
    }
    QJsonObject directMaterial, directSession, directApplied = m_directApplied;
    QString directRecipe;
    QList<int> directChecked;
    if (m_directPage) {
        directMaterial = m_directPage->material()->materialJson();
        directRecipe = m_directPage->recipes()->recipe();
        directSession = m_directPage->suggestions()->session();
        directChecked = m_directPage->suggestions()->checkedOrder();
    }
    const RenderSettings deliver = m_deliver->settings();
    const QString chatDraft = m_chatPanel ? m_chatPanel->input()->toPlainText() : QString();
    const bool updateBar = m_updateBar && !m_updateBar->isHidden() && m_updateNews.size() == 2;
    const bool scaleBar = m_scaleBar && !m_scaleBar->isHidden();
    const bool multicam = m_multicamMode;

    // ---- The old widgets go
    tearDownUi();

    // ---- The new look, then everything made again in it
    // (With the widgets gone, the app's new style sheet and palette are cheap: nothing to polish again yet)
    if (I18n::languageChanged()) I18n::switchTo(I18n::language());
    if (!Theme::switchToSaved(*qApp)) Theme::apply(*qApp); // the font too (a language of its own may want another size)
    ++m_uiGeneration;
    buildUi();

    // ---- What the widgets held, back
    if (hadColor) ensureColorPage();
    if (hadComp) ensureCompPage();
    if (hadStudio) ensureStudioPage();
    if (hadDirect) ensureDirectPage();
    const QVector<QToolButton*> buttons = tourToggles();
    for (int i = 0; i < buttons.size() && i < toggles.size(); ++i)
        if (buttons[i] && buttons[i]->isChecked() != toggles[i]) buttons[i]->setChecked(toggles[i]);
    if (m_scopes && scopes) m_scopes->setVisible(*scopes);
    showPage(page); // the same page: nothing of a page entry (playhead, Comp clip) happens
    applySplitterSizes(this, sizes);
    tv = m_timeline->view();
    tv->setTool(tool);
    tv->setSnapping(snapping);
    tv->setPlayhead(playhead);
    // Core rule: the zoom and scroll exactly as the user left them; once more after the layout settled (the scroll
    // range needs the view's real height)
    const auto restoreView = [tv, view] { tv->setZoomAndScroll(view.pxPerFrame, view.leftFrame, view.scrollY); };
    restoreView();
    QTimer::singleShot(0, tv, restoreView);
    if (m_compPage && compClip) m_compPage->setClip(compClip);
    if (m_studioPage && studioFitPending) m_studioPage->tracks()->fitOnShow();
    else if (m_studioPage && studioView) m_studioPage->tracks()->setView(studioView->x(), studioView->y());
    if (m_directPage) {
        if (!directSession.isEmpty()) {
            showDirectSession(directSession);
            for (int n : m_directPage->suggestions()->rowOrder())
                m_directPage->suggestions()->setChecked(n, directChecked.contains(n));
        } else {
            const QSignalBlocker block(m_directPage->material()); // its changed() would reset the page
            if (directMaterial.contains("timeline")) m_directPage->material()->setUseTimeline(true);
            else m_directPage->material()->addFiles(directMaterial.value("files").toVariant().toStringList());
            m_directPage->recipes()->setRecipe(directRecipe);
        }
        m_directApplied = directApplied;
        m_directPage->updateResult();
    }
    m_deliver->setSettings(deliver);
    if (m_chatPanel && !chatDraft.isEmpty()) m_chatPanel->input()->setPlainText(chatDraft);
    if (updateBar) {
        m_updateText->setText(T("schneidi %1 ist da (du hast %2).").arg(m_updateNews[0], m_updateNews[1]));
        m_updateBar->show();
    }
    if (scaleBar) {
        m_scaleBar = nullptr;
        offerUiScale(m_scaleOffer);
    }
    if (multicam) setMulticamMode(true);
    if (!picture.isNull()) { // the picture as it was until the engine sends the next one
        m_viewer->screen()->setFrameSize(m_engine->frameSize());
        m_viewer->screen()->setFrame(picture);
        if (m_scopes && m_page == Page::Color) m_scopes->setFrame(picture);
    }
    // The new widgets learn where the playhead is (paused, the engine says nothing until it moves): the viewer's time
    // and bar, the Inspector's keyframe values, the transcript, ... - a display, nothing moves
    if (m_engine->speed() == 0.0) emit m_engine->positionChanged(m_engine->position());
    if (outputScreen) showPreviewOn(outputScreen);
    if (drawing && m_page == Page::Edit) setDrawTool(true);
    if (large) setViewerLarge(true);
    if (fullScreenViewer) setViewerFullScreen(true);
    updatePageButtons();
    updateRenderCacheBar();
    if (curtain) curtain->reveal();
    return true;
}

void MainWindow::tearDownUi()
{
    // First the connections from what stays (engine, project, ...) to the widgets: nothing reaches them from now on
    delete std::exchange(m_ui, nullptr);
    delete std::exchange(m_tours, nullptr);
    // A running render goes on; the new Deliver page takes its queue. Jobs in the corner move to the new indicator.
    m_keptQueue = m_deliver->takeRenderQueue();
    m_oldJobs = m_backgroundJobs;
    m_backgroundJobs->hide();
    m_backgroundJobs->setParent(nullptr);
    if (m_keyTrainer) delete m_keyTrainer.data();
    delete m_drawBar; // a window of its own
    // The widget tree with every page, the menus, the actions and shortcuts of the window (all made by buildUi)
    delete takeCentralWidget();
    if (QWidget* bar = menuWidget()) {
        setMenuWidget(nullptr);
        delete bar;
    }
    delete m_aiLogView;
    delete m_drawOverlay;
    delete m_shapeOverlay;
    QList<QPointer<QObject>> made; // what buildUi hung on the window itself
    for (QObject* child : children()) {
        // (No menus: buildUi hangs none on the window; one of the window is a context menu on someone's stack)
        const bool action = qobject_cast<QAction*>(child) || qobject_cast<QActionGroup*>(child)
                            || qobject_cast<QShortcut*>(child) || qobject_cast<QButtonGroup*>(child);
        auto* widget = qobject_cast<QWidget*>(child);
        const bool keep = widget && (widget->isWindow() || widget == statusBar() || widget->objectName() == "switchCurtain");
        if (action || (widget && !keep)) made << child;
    }
    for (const QPointer<QObject>& child : std::as_const(made)) delete child.data();
    statusBar()->clearMessage();

    // Nothing points at the old widgets any more
    m_mediaPool = nullptr;
    m_backgroundJobs = nullptr;
    m_renderCacheJob = false;
    m_effects = nullptr;
    m_storage = nullptr;
    m_viewer = nullptr;
    m_multicam = nullptr;
    m_multicamAction = nullptr;
    m_inspector = nullptr;
    m_colorPanel = nullptr;
    m_shapeOverlay = nullptr;
    m_drawOverlay = nullptr;
    m_drawBar = nullptr;
    m_drawAction = nullptr;
    m_transcript = nullptr;
    m_scopes = nullptr;
    m_mixer = nullptr;
    m_timeline = nullptr;
    m_deliver = nullptr;
    m_compPage = nullptr;
    m_studioPage = nullptr;
    m_directPage = nullptr;
    m_directRemoveSuggestion = nullptr;
    m_directRemoveMaterial = nullptr;
    m_pages = nullptr;
    m_editTop = m_editLeft = m_editMain = m_editRow = m_editRight = nullptr;
    m_inspectorSlot = nullptr;
    m_mediaPage = m_mediaTop = m_deliverPage = m_deliverRight = m_colorPage = m_colorBottom = nullptr;
    m_defaultSizes.clear();
    m_pageButtons = nullptr;
    m_statusText = nullptr;
    m_pageBarRight = nullptr;
    m_poolToggle = m_effectsToggle = m_storageToggle = m_inspectorToggle = m_mixerToggle = m_transcriptToggle = nullptr;
    m_large = {};
    m_largeAction = nullptr;
    m_largeEscape = m_drawEscape = nullptr;
    m_fullScreenAction = m_appFullScreenAction = nullptr;
    m_secondScreenAction = nullptr;
    m_secondScreenModeActions[0] = m_secondScreenModeActions[1] = nullptr;
    m_viewerScreenModeMenu = nullptr;
    for (auto& row : m_resolutionActions)
        for (QAction*& a : row) a = nullptr;
    for (const QMetaObject::Connection& c : std::as_const(m_viewerAway.watches)) disconnect(c);
    m_viewerAway = {};
    m_findMissingAction = nullptr;
    m_recentMenu = m_templateMenu = nullptr;
    m_titleLabel = nullptr;
    m_updateBar = m_scaleBar = nullptr;
    m_updateText = nullptr;
    m_aiLogView = nullptr;
    m_liveBox = nullptr;
    m_liveLabel = m_liveButton = nullptr;
    m_chatPanel = nullptr;
    m_chatSplit = nullptr;
    m_chatToggle = nullptr;
    m_topBarLayout = nullptr;
    m_voiceoverAction = nullptr;
    m_voiceoverEscape = nullptr;
}

// ---------- Restart for the interface size ----------

bool MainWindow::restartRestoring()
{
    // A running render would end with the process: asked like on quit (the project itself is never asked about)
    if (Exporter::anyRunning() && !m_autosaveDisabled
        && QMessageBox::question(this, "schneidi", T("Es wird gerade gerendert. Der Neustart bricht das Rendern ab. Trotzdem neu starten?"),
                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return false;
    if (m_tours && (m_tourSaved.active || m_tours->runner()->running())) m_tours->stop(); // the user's project back
    endVoiceover();
    SessionResume::State state;
    state.projectPath = m_projectPath;
    state.saveSuggestion = m_saveSuggestion;
    state.modified = m_project->isModified();
    state.page = pageId(m_page);
    state.selection = m_selection->ids();
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    data.views = savedViews();
    QString error;
    const QString file = SessionResume::write(state, data, &error);
    if (file.isEmpty()) { // nothing kept: the usual way (asks about saving)
        qWarning("Resume file not written: %s", qPrintable(error));
        m_restartRequested = true;
        if (!close()) m_restartRequested = false;
        return m_restartRequested;
    }
    m_resumeFile = file;
    m_resumeQuit = true;
    m_restartRequested = true;
    if (!close()) { // (a render the user kept)
        m_restartRequested = false;
        m_resumeQuit = false;
        SessionResume::remove(std::exchange(m_resumeFile, {}));
    }
    return m_restartRequested;
}

bool MainWindow::resumeSession(const QString& file)
{
    SessionResume::State state;
    ProjectData data;
    QString error;
    if (!SessionResume::read(file, &state, &data, &error)) {
        qWarning("Resume file not readable: %s", qPrintable(error));
        return false;
    }
    SessionResume::remove(file);
    // As if opened by the user, but without its questions (asked before the restart already) and with the file name
    // of the project: saving writes its own file again
    if (!applyLoaded(std::move(data), state.projectPath, false)) return false;
    m_projectFingerprint = state.projectPath.isEmpty() ? QByteArray() : ProjectFile::fingerprint(state.projectPath);
    m_saveSuggestion = state.saveSuggestion;
    if (state.modified) m_project->markModified();
    updateTitle();
    if (const std::optional<Page> p = pageForId(state.page)) showPage(*p);
    m_selection->set(state.selection);
    return true;
}
