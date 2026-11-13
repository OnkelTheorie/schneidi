// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: layout, top bar, page bar and pages.
#include "app/MainWindow.h"
#include "core/StartupTrace.h"

#include "app/AiChatPanel.h"

#include "app/TourManager.h"
#include "app/InputBindings.h"
#include "app/Theme.h"
#include "app/KeyBindingsDialog.h"
#include "app/ClipSpeedDialog.h"
#include "app/DesignDialog.h"
#include "app/Log.h"
#include "app/NormalizeDialog.h"
#include "app/PasteAttributesDialog.h"
#include "app/VoiceoverRecorder.h"
#include "core/Loudness.h"
#include "app/ProjectSettingsDialog.h"
#include "app/TourManager.h"
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
#include "engine/ClipFormat.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "ui/EffectsLibrary.h"
#include "ui/ShapeOverlay.h"
#include "ui/DrawOverlay.h"
#include "ui/CompactButton.h"
#include "ui/ElidedLabel.h"
#include "ui/SplitterMemory.h"
#include "ui/MediaStorage.h"
#include "ui/ColorPanel.h"
#include "ui/TranscriptPanel.h"
#include "ui/ScopesPanel.h"
#include "ui/Inspector.h"
#include "ui/Mixer.h"
#include "ui/MediaPool.h"
#include "ui/MulticamView.h"
#include "ui/FullScreenViewer.h"
#include "ui/SecondScreen.h"
#include "ui/FrameView.h"
#include "ui/Viewer.h"
#include "ui/comp/CompOverlay.h"
#include "ui/comp/CompPage.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QWindow>
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
#include <QFile>
#include <QLockFile>
#include <QSaveFile>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QScreen>
#include <QShortcut>
#include <QStatusBar>
#include <QInputDialog>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <numeric>
#include <tuple>
#include <utility>

void MainWindow::buildLayout()
{
    // The skeleton first, already in the window: root, the chat splitter, the pages and their splitters. The panels are
    // then built straight into the place they take on the Edit page (the usual start page; showPage only orders them
    // there). Every move of a built widget tree into another parent re-applies the app style sheet to all of its
    // widgets (scroll areas lay their content out again): built bottom-up and hung in at the end, the whole tree did
    // that several times (the Inspector alone ~60 ms at the start). The empty splitters are still made as top-level
    // widgets and hung in (cheap): their first geometry (640x480) is what the first sizes are computed in before the
    // window shows (updateRightColumn takes editMain's) - children of the window started at 100x30 and split otherwise
    auto* root = new QWidget;
    root->setObjectName("Root");
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    setCentralWidget(root);
    m_chatSplit = new QSplitter(Qt::Horizontal, root);
    m_chatSplit->setObjectName("aiChatSplit");
    lay->addWidget(m_chatSplit, 1);
    m_pages = new QStackedWidget(m_chatSplit);
    m_chatSplit->addWidget(m_pages);
    // Pages; the shared panels (pool, viewer, timeline) move along when switching
    // Media page like DaVinci: Media Storage | Viewer on top, the Media Pool below
    m_mediaPage = new QSplitter(Qt::Vertical);
    m_pages->addWidget(m_mediaPage);
    m_mediaTop = new QSplitter(Qt::Horizontal);
    m_mediaPage->addWidget(m_mediaTop);
    m_editTop = new QSplitter(Qt::Horizontal);
    m_editLeft = new QSplitter(Qt::Vertical, m_editTop);
    m_editLeft->setObjectName("editLeft");
    m_editTop->addWidget(m_editLeft);
    m_editMain = new QSplitter(Qt::Vertical);
    m_editMain->addWidget(m_editTop);
    // The mixer on the right below the Inspector, in a column of its own: the handle above the timeline moves only the
    // timeline, the mixer keeps its height (and the Inspector comes down to it)
    m_editRight = new QSplitter(Qt::Vertical);
    m_editRight->hide();
    // The mixer hung into the column before that hangs in the window (one cheap move of the mixer's tree, as before): built
    // in the column, its size hints at the column's first sizing (updateRightColumn) were others and the column split
    // differently (Inspector 10 px lower)
    m_mixer = new Mixer(m_project, m_engine);
    m_mixer->hide();
    m_editRight->addWidget(m_mixer);
    StartupTrace::mark("ctor Mixer");
    m_editRow = new QSplitter(Qt::Horizontal);
    m_editRow->addWidget(m_editMain);
    m_editRow->addWidget(m_editRight);
    m_editRow->setStretchFactor(0, 1);
    m_pages->addWidget(m_editRow);
    m_deliverPage = new QSplitter(Qt::Horizontal);
    m_pages->addWidget(m_deliverPage);
    StartupTrace::mark("  skeleton");

    m_mediaPool = new MediaPool(m_project, m_engine, m_editLeft);
    m_mediaPool->setProbeCache(&m_probeCache); // a dropped file: the drop preview's probe, not a second one
    connect(m_mediaPool, &MediaPool::probingChanged, this, &MainWindow::updateLiveWaiting);
    StartupTrace::mark("ctor MediaPool");
    m_effects = new EffectsLibrary(m_editLeft); // below Media Storage / Media Pool: showPage puts those first
    StartupTrace::mark("ctor EffectsLibrary");
    m_storage = new MediaStorage(m_editLeft);
    StartupTrace::mark("ctor MediaStorage");
    m_viewer = new Viewer(m_engine, m_editTop);
    StartupTrace::mark("ctor Viewer");
    m_multicam = new MulticamView(m_project, m_selection, m_editor);
    StartupTrace::mark("ctor MulticamView");
    m_viewer->setMulticamView(m_multicam);
    // The Inspector right in its place: the mixer column (saved setting, as the toggle reads it), else the top row. Moved
    // there later it took ~60 ms (updateRightColumn sizes the column as before: the slot keeps the row's sizes)
    const bool column = QSettings().value("panels/mixer", false).toBool();
    m_inspector = new Inspector(m_editor, column ? m_editRight : m_editTop);
    if (column) m_editRight->insertWidget(0, m_inspector); // over the mixer
    else m_editTop->addWidget(m_inspector);
    StartupTrace::mark("ctor Inspector");
    m_inspector->setFrameSize(m_project->timelineFormat().size());
    m_timeline = new TimelinePanel(m_editor, m_editMain);
    m_editMain->addWidget(m_timeline);
    StartupTrace::mark("ctor TimelinePanel");
    m_deliver = new DeliverPanel(m_project, m_deliverPage);
    StartupTrace::mark("ctor DeliverPanel");
    m_mixer->setEditor(m_editor);
    m_transcript = new TranscriptPanel(m_project, m_editor, m_editTop);
    StartupTrace::mark("ctor TranscriptPanel");

    StartupTrace::mark("layout panels done");
    m_inspectorSlot = new QWidget(this); // not in any layout while unused
    m_inspectorSlot->hide();
    m_deliverRight = new QSplitter(Qt::Vertical);
    m_deliverPage->addWidget(m_deliver);
    m_deliverPage->addWidget(m_deliverRight);
    m_deliverPage->addWidget(m_deliver->queuePanel()); // Render queue on the right like in DaVinci
    const QPair<QSplitter*, const char*> splitters[] = {
        {m_editTop, "editTop"},         {m_editMain, "editMain"},     {m_editLeft, "editLeft"},
        {m_editRow, "editRow"},         {m_editRight, "editRight"},   {m_mediaPage, "mediaPage"},   {m_mediaTop, "mediaTop"},
        {m_deliverPage, "deliverPage"}, {m_deliverRight, "deliverRight"}};
    for (const auto& [sp, name] : splitters) watchSizes(sp, name);
    // The Color page (color wheels, scopes) is built on its first showing: ensureColorPage()
    StartupTrace::mark("  splitters");

    // The AI chat beside the pages (all of them), left or right (MainWindowChat.cpp)
    buildChat();
    StartupTrace::mark("  buildChat");

    lay->insertWidget(0, buildTopBar());
    StartupTrace::mark("  topbar");
    lay->insertWidget(1, buildUpdateBar());
    lay->addWidget(buildPageBar());
    StartupTrace::mark("  pagebar");
    // The status bar's messages show in the page bar (one compact row at the bottom, the room goes to the pages):
    // the QStatusBar itself stays hidden, every statusBar()->showMessage() lands there through messageChanged
    statusBar()->hide();
    connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString& text) { showStatusText(text); });
    StartupTrace::mark("  statusbar");

    // The Edit page, or the nearest shown one when Edit is hidden
    const QString start = m_pageVisibility.nearestVisible(pageId(Page::Edit));
    Page first = Page::Edit;
    for (Page p : {Page::Media, Page::Edit, Page::Comp, Page::Color, Page::Deliver})
        if (pageId(p) == start) first = p;
    StartupTrace::mark("pages assembled");
    showPage(first);
    StartupTrace::mark("showPage");
}

QWidget* MainWindow::buildTopBar()
{
    // Top bar like in DaVinci: show/hide panels left/right, project name in the middle
    // Visibility is saved (panels/<key>); test runs do not change the setting. A narrow window (1024 px, a long
    // language, a big font) shows only the symbols, the name is in the tooltip
    auto makeToggle = [this](const char* symbol, const QString& name, const QString& key, bool def, QWidget* panel) {
        auto* b = new CompactButton;
        b->setObjectName("panelToggle_" + key);
        b->setText(QString::fromUtf8(symbol) + " " + name);
        b->setCompactText(QString::fromUtf8(symbol));
        b->setCheckable(true);
        b->setChecked(QSettings().value("panels/" + key, def).toBool());
        // Only hide panels not yet inserted (show() without a parent opened its own window)
        if (panel->parentWidget() || !b->isChecked()) panel->setVisible(b->isChecked());
        connect(b, &QToolButton::toggled, this, [this, key, panel](bool on) {
            setViewerLarge(false); // the layout comes back, with this panel as asked
            panel->setVisible(on);
            updateLeftColumn();
            updateRightColumn();
            // After the layout (and after the large viewer's own restore, also a zero timer): still at size 0?
            if (on) QTimer::singleShot(0, panel, [panel] { uncollapse(panel); });
            if (!m_autosaveDisabled && !m_tourSaved.active) QSettings().setValue("panels/" + key, on); // a tour sets its own
        });
        return b;
    };
    m_poolToggle = makeToggle("▦", T("Medienpool"), "mediaPool", true, m_mediaPool);
    m_effectsToggle = makeToggle("✦", T("Effekte"), "effects", false, m_effects);
    m_storageToggle = makeToggle("▤", T("Dateibrowser"), "mediaStorage", false, m_storage);
    m_storageToggle->setToolTip(T("Ordner durchsuchen und Clips direkt in Medienpool/Timeline ziehen"));
    m_inspectorToggle = makeToggle("☰", T("Inspektor"), "inspector", true, m_inspector);
    m_mixerToggle = makeToggle("▥", T("Mischpult"), "mixer", false, m_mixer);
    m_transcriptToggle = makeToggle("¶", T("Transkript"), "transcript", false, m_transcript);
    m_transcriptToggle->setToolTip(T("Gesprochenen Text der Tonclips zeigen; Wörter löschen schneidet die Timeline"));
    m_chatToggle = makeToggle("✧", T("KI"), "aiChat", false, m_chatPanel);
    m_chatToggle->setObjectName("ai_chat_toggle");
    m_chatToggle->setToolTip(T("KI-Chat: sag einer KI, was sie im offenen Projekt tun soll"));

    auto* title = m_titleLabel = new ElidedLabel; // a long project name ends in "…" instead of widening the window
    title->setStyleSheet("font-weight: 600;");
    title->setAlignment(Qt::AlignCenter);

    auto* bar = new QWidget;
    bar->setObjectName("TopBar");
    bar->setStyleSheet(QString("QWidget#TopBar { background: %1; border-bottom: 1px solid %2; }")
                           .arg(Theme::topBar.name(), Theme::border.name()));
    auto* lay = m_topBarLayout = new QHBoxLayout(bar);
    lay->setContentsMargins(6, 2, 6, 2);
    // The AI button sits on the panel's side, on the inner end of the group: the buttons there stay where they were
    const bool chatLeft = m_chatSplit->indexOf(m_chatPanel) == 0;
    lay->addWidget(m_storageToggle);
    lay->addWidget(m_poolToggle);
    lay->addWidget(m_effectsToggle);
    if (chatLeft) lay->addWidget(m_chatToggle);
    lay->addStretch(1);
    lay->addWidget(title);
    lay->addWidget(buildLiveIndicator());
    lay->addStretch(1);
    if (!chatLeft) lay->addWidget(m_chatToggle);
    lay->addWidget(m_transcriptToggle);
    lay->addWidget(m_mixerToggle);
    lay->addWidget(m_inspectorToggle);
    return bar;
}

QWidget* MainWindow::buildPageBar()
{
    // One row at the bottom: the status text (or the name and version) on the left, the page buttons in the
    // middle (icon and name side by side), background work on the right. The status bar's row is gone, its messages
    // show here (showStatusText) - every pixel the bars do not need goes to the pages
    auto* bar = new QWidget;
    bar->setObjectName("PageBar");
    bar->setStyleSheet(
        QString("QWidget#PageBar { background: %1; border-top: 1px solid %2; }"
                // one line, but a comfortable click target (about 35 px with the normal font)
                "QToolButton { color: %3; padding: 8px 16px 7px 16px; border-radius: 0;"
                " border-bottom: 2px solid transparent; }"
                "QToolButton:checked { color: %4; background: transparent; border-bottom: 2px solid %4; }"
                "QToolButton:hover { color: %5; }")
            .arg(Theme::window.name(), Theme::border.name(), Theme::textDim.name(), Theme::primary.name(),
                 Theme::text.name()));
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(2);

    // Left and right of the buttons equally wide (their size hints ignored): the buttons stay in the middle
    auto side = [](QWidget* parent) {
        auto* w = new QWidget(parent);
        w->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto* l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(6);
        return std::pair{w, l};
    };
    auto [left, leftLay] = side(bar);
    m_statusText = new ElidedLabel;
    m_statusText->setObjectName("statusText");
    leftLay->addWidget(m_statusText, 1);
    showStatusText({}); // the name and version (bug reports, screenshots)
    lay->addWidget(left, 1);

    // Right-click on the bar or a page button: which pages it shows
    m_pageVisibility.load();
    auto pagesMenu = [this](QWidget* from, const QPoint& pos) {
        QMenu menu(this);
        fillPagesMenu(&menu);
        menu.exec(from->mapToGlobal(pos));
    };
    bar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bar, &QWidget::customContextMenuRequested, this, [bar, pagesMenu](const QPoint& pos) { pagesMenu(bar, pos); });

    m_pageButtons = new QButtonGroup(this);
    const struct { const char* icon; const char* name; Page page; } pages[] = {
        {"▤", N_("Medien"), Page::Media},
        {"✂", N_("Schneiden"), Page::Edit},
        {"◇", N_("Compositing"), Page::Comp},
        {"◐", N_("Farbe"), Page::Color},
        {"⇪", N_("Export"), Page::Deliver},
    };
    for (const auto& pg : pages) {
        auto* b = new QToolButton;
        b->setText(QString("%1  %2").arg(pg.icon, T(pg.name)));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        // As tall as the bar (its line at the bottom of the bar): a symbol drawn by a taller fallback font makes one
        // button and so the bar taller, the others stretch along instead of keeping their own height
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        b->setCheckable(true);
        b->setChecked(pg.page == m_page);
        const Page page = pg.page;
        connect(b, &QToolButton::clicked, this, [this, page] {
            showPage(page);
            if (m_tours) m_tours->pageOpened(pageName(page)); // the user opened it: its tour's question, once
        });
        b->setObjectName("pageButton_" + pageId(page)); // stable: tours point at it
        b->setToolTip(T("Rechtsklick: Seiten ein- und ausblenden"));
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QWidget::customContextMenuRequested, this, [b, pagesMenu](const QPoint& pos) { pagesMenu(b, pos); });
        m_pageButtons->addButton(b, int(pg.page));
        lay->addWidget(b);
    }
    auto [right, rightLay] = side(bar);
    rightLay->addStretch(1);
    m_pageBarRight = rightLay; // background work (setupBackgroundJobs)
    lay->addWidget(right, 1);
    updatePageButtons();
    return bar;
}

void MainWindow::showStatusText(const QString& text)
{
    if (!m_statusText) return;
    // A message in the normal text color, else the name and version dimmed
    m_statusText->setFullText(text.isEmpty() ? appName() : text);
    m_statusText->setStyleSheet(text.isEmpty() ? QString("color: %1; font-weight: 600;").arg(Theme::textDim.name())
                                               : QString("color: %1;").arg(Theme::text.name()));
}

QString MainWindow::pageId(Page page)
{
    switch (page) {
    case Page::Media: return "media";
    case Page::Edit: return "edit";
    case Page::Comp: return "comp";
    case Page::Color: return "color";
    case Page::Deliver: return "deliver";
    }
    return {};
}

bool MainWindow::isPageHidden(Page page) const { return m_pageVisibility.isHidden(pageId(page)); }

void MainWindow::setPageHidden(Page page, bool hidden)
{
    if (!m_pageVisibility.setHidden(pageId(page), hidden)) return;
    if (!m_autosaveDisabled && !m_tourSaved.active) m_pageVisibility.save(); // test runs and tours: not the user's
    if (hidden && page == m_page) { // away from the page that just went
        const QString next = m_pageVisibility.nearestVisible(pageId(page));
        for (Page p : {Page::Media, Page::Edit, Page::Comp, Page::Color, Page::Deliver})
            if (pageId(p) == next) showPage(p);
    }
    updatePageButtons();
}

void MainWindow::showAllPages()
{
    m_pageVisibility.showAll();
    if (!m_autosaveDisabled && !m_tourSaved.active) m_pageVisibility.save();
    updatePageButtons();
}

void MainWindow::fillPagesMenu(QMenu* menu)
{
    menu->clear();
    const struct { const char* name; Page page; } pages[] = {
        {N_("Medien"), Page::Media},     {N_("Schneiden"), Page::Edit}, {N_("Compositing"), Page::Comp},
        {N_("Farbe"), Page::Color},      {N_("Export"), Page::Deliver},
    };
    for (const auto& pg : pages) {
        const Page page = pg.page;
        QAction* a = menu->addAction(T(pg.name));
        a->setObjectName("pageVisible_" + pageId(page));
        a->setCheckable(true);
        a->setChecked(!isPageHidden(page));
        a->setEnabled(isPageHidden(page) || m_pageVisibility.canHide(pageId(page))); // the last shown page stays
        connect(a, &QAction::toggled, this, [this, page](bool on) { setPageHidden(page, !on); });
    }
    menu->addSeparator();
    QAction* all = menu->addAction(T("Alle Seiten zeigen"), this, [this] { showAllPages(); });
    all->setObjectName("pageVisible_all");
    all->setEnabled(m_pageVisibility.visibleCount() < int(m_pageVisibility.order().size()));
}

void MainWindow::updatePageButtons()
{
    if (!m_pageButtons) return;
    for (QAbstractButton* b : m_pageButtons->buttons()) {
        const Page page = Page(m_pageButtons->id(b));
        // A key to a hidden page shows it while it is current; a tour shows every page (it points at their buttons)
        b->setVisible(!isPageHidden(page) || page == m_page || m_tourSaved.active);
    }
}

void MainWindow::showPage(Page page)
{
    setViewerLarge(false); // the page's own layout first, then the switch
    restoreViewerArea();   // the viewer moves to the new page's place: closed up there again at the end
    const Page before = m_page;
    m_page = page;
    if (before == Page::Comp && page != Page::Comp && m_compPage) { // the Color page's windows over the viewer again
        m_compPage->overlay()->setVisible(false);
        m_viewer->setOverlay(m_colorPanel ? m_colorPanel->overlay() : nullptr);
        if (m_compPage->viewNode() || m_compPage->matte()) updatePreview(); // the output again
    }
    switch (page) {
    case Page::Media:
        m_mediaTop->insertWidget(0, m_storage);
        m_mediaTop->insertWidget(1, m_viewer);
        m_mediaTop->setStretchFactor(1, 1);
        applySizes(m_mediaTop, {700, 900});
        m_mediaPage->insertWidget(1, m_mediaPool);
        applySizes(m_mediaPage, {520, 380});
        m_storage->setVisible(true);
        m_mediaPool->setVisible(true);
        m_pages->setCurrentWidget(m_mediaPage);
        break;
    case Page::Edit:
        m_editLeft->insertWidget(0, m_storage);
        m_editLeft->insertWidget(1, m_mediaPool);
        m_editTop->insertWidget(0, m_editLeft);
        m_editTop->insertWidget(1, m_viewer);
        m_editTop->insertWidget(2, m_transcript);
        if (m_editTop->indexOf(m_inspector) < 0 && m_editTop->indexOf(m_inspectorSlot) < 0) {
            // First placement (else it is there, or its slot: updateRightColumn). With the mixer column the slot
            // goes here and the Inspector stays in the column it was built in (buildLayout): every move of the
            // Inspector re-applies the style sheet to its whole widget tree (~60-150 ms at the start)
            if (m_mixerToggle && m_mixerToggle->isChecked()) {
                m_inspectorSlot->setVisible(true); // takes the Inspector's size below, hidden again in the column swap
                m_editTop->insertWidget(3, m_inspectorSlot);
            } else {
                m_editTop->insertWidget(3, m_inspector);
            }
        }
        m_editMain->insertWidget(1, m_timeline);
        m_editTop->setStretchFactor(1, 1);
        // The user's Media Storage / Media Pool / Effects heights (else as laid out)
        if (const QVariantList left = QSettings().value("layout/editLeft").toList(); left.size() == m_editLeft->count()) {
            QList<int> sizes;
            for (const QVariant& v : left) sizes << std::max(0, v.toInt());
            m_editLeft->setSizes(sizes);
        }
        applySizes(m_editTop, {360, 880, 320, 360});
        applySizes(m_editMain, {480, 420});
        m_mediaPool->setVisible(m_poolToggle->isChecked());
        m_storage->setVisible(m_storageToggle->isChecked());
        updateLeftColumn();
        updateRightColumn(); // its sizes when the mixer comes (a page switch keeps the ones it has)
        m_pages->setCurrentWidget(m_editRow);
        break;
    case Page::Deliver:
        m_deliverRight->insertWidget(0, m_viewer);
        m_deliverRight->insertWidget(1, m_timeline);
        applySizes(m_deliverRight, {480, 380});
        m_deliverPage->setStretchFactor(1, 1);
        applySizes(m_deliverPage, {340, 940, 320});
        m_pages->setCurrentWidget(m_deliverPage);
        break;
    case Page::Color:
        ensureColorPage();
        if (before != Page::Color) playheadInto(selectedVideoClip()); // the viewer shows the clip being graded
        m_colorPage->insertWidget(0, m_viewer);
        m_colorPage->insertWidget(1, m_timeline);
        m_colorPage->setStretchFactor(0, 1);
        applySizes(m_colorPage, {460, 200, 300});
        m_pages->setCurrentWidget(m_colorPage);
        break;
    case Page::Comp:
        // Built on the first showing; the selected video clip is opened (openComp sets its own after this)
        ensureCompPage();
        if (before != Page::Comp && !m_tourSwap) { // the selected clip (the playhead goes into it), else the one under the playhead
            playheadInto(selectedVideoClip());
            if (const int id = compClipAt(m_timeline->view()->playhead()); id && id != m_compPage->clip())
                m_compPage->setClip(id);
        }
        m_compPage->top()->insertWidget(0, m_viewer);
        applySizes(m_compPage->top(), {500, 420});
        m_compPage->main()->setStretchFactor(0, 1);
        applySizes(m_compPage->main(), {1240, 360});
        m_viewer->setOverlay(m_compPage->overlay());
        m_compPage->overlay()->setVisible(true);
        m_pages->setCurrentWidget(m_compPage);
        if (m_compPage->viewNode() || m_compPage->matte()) updatePreview();
        break;
    }
    // Edit page: the selected shape's box (move, size, turn); the other pages their own overlay again
    if (page == Page::Edit) {
        if (!m_shapeOverlay) {
            m_shapeOverlay = new ShapeOverlay(m_editor, m_selection, [this] { return m_timeline->view()->playhead(); }, this,
                                              [this] { return !m_viewer->showsSource(); });
            // Painting (0.22) over it: what the paint tool and the drawing box do not take goes to the shape box
            m_drawOverlay = new DrawOverlay(m_editor, m_selection, [this] { return m_timeline->view()->playhead(); }, this,
                                            [this] { return !m_viewer->showsSource(); }, m_shapeOverlay);
            connect(m_drawOverlay, &DrawOverlay::activeChanged, this, &MainWindow::drawToolChanged);
            connect(m_drawOverlay, &DrawOverlay::notice, this, [this](const QString& t) { m_viewer->showNotice(t, 3000); });
        }
        m_viewer->setOverlay(m_drawOverlay);
    } else if (before == Page::Edit && page != Page::Comp) {
        m_viewer->setOverlay(m_colorPanel ? m_colorPanel->overlay() : nullptr);
    }
    if (page != Page::Edit && m_drawOverlay) m_drawOverlay->setActive(false); // painting is the Edit page's
    if (auto* b = m_pageButtons->button(int(page))) b->setChecked(true);
    updatePageButtons();
    // The armed track's level meter listens only where the timeline (its mic) is shown
    if (m_voiceover) m_voiceover->setMeterShown(page != Page::Media && page != Page::Comp);
    const bool edit = page == Page::Edit;
    m_poolToggle->setEnabled(edit);
    m_effectsToggle->setEnabled(edit);
    m_storageToggle->setEnabled(edit);
    m_inspectorToggle->setEnabled(edit);
    m_mixerToggle->setEnabled(edit);
    m_transcriptToggle->setEnabled(edit);
    updateViewerArea();
    if (m_tours) QMetaObject::invokeMethod(m_tours, &TourManager::poke, Qt::QueuedConnection); // a step may wait for it
}

void MainWindow::setDrawTool(bool on)
{
    // Painting goes into the timeline: with a Media Pool clip in the viewer there is nothing to paint on
    if (on && m_page == Page::Edit && m_viewer->showsSource()) {
        m_viewer->showNotice(T("Malen geht nur auf der Timeline"), 3000);
        if (m_drawAction) {
            const QSignalBlocker b(m_drawAction);
            m_drawAction->setChecked(m_drawOverlay && m_drawOverlay->active());
        }
        return;
    }
    if (on && m_page != Page::Edit) showPage(Page::Edit);
    if (m_drawOverlay) m_drawOverlay->setActive(on);
    else if (m_drawAction) m_drawAction->setChecked(false);
}

void MainWindow::drawToolChanged(bool on)
{
    if (m_drawAction && m_drawAction->isChecked() != on) {
        const QSignalBlocker b(m_drawAction);
        m_drawAction->setChecked(on);
    }
    if (on && !m_drawBar) {
        m_drawBar = new DrawBar([this] { return m_project->timelineFormat().height; }, m_viewer->screen());
        connect(m_drawBar, &DrawBar::closeWanted, this, [this] { setDrawTool(false); });
    }
    if (m_drawBar) {
        m_drawBar->setVisible(on);
        if (on) {
            m_drawBar->sync();
            m_drawBar->place(); // never wider than the viewer (it follows its width)
        }
    }
    updateEscapeKeys();
}

// Esc ends what is on, one at a time (two enabled Esc shortcuts are ambiguous and neither fires): a recording first,
// then painting, then the large viewer. Window actions bound to Esc give it up meanwhile (InputBindings::holdWindowKey)
void MainWindow::updateEscapeKeys()
{
    const bool recording = m_voiceover && m_voiceover->recording();
    const bool drawing = m_drawOverlay && m_drawOverlay->active();
    if (m_voiceoverEscape) m_voiceoverEscape->setEnabled(recording);
    if (m_drawEscape) m_drawEscape->setEnabled(drawing && !recording);
    if (m_largeEscape) m_largeEscape->setEnabled(m_large.active && !recording && !drawing);
    InputBindings::instance().holdWindowKey("voiceover", "Esc", recording);
    InputBindings::instance().holdWindowKey("draw_tool", "Esc", drawing && !recording);
    if (m_largeEscape) InputBindings::instance().holdWindowKey("large_viewer", "Esc", m_largeEscape->isEnabled());
}

void MainWindow::ensureColorPage()
{
    if (m_colorPanel) return;
    // Built on the first use (Color page, a grade shortcut, a tour): the wheels and scopes are a good part of the
    // start's widgets, and most sessions never open them
    m_colorPanel = new ColorPanel(m_editor);
    m_scopes = new ScopesPanel;
    m_scopes->setProperty("tourTarget", "scopes");
    // Color page (slimmed down): viewer on top, timeline in the middle, color wheels below and the scopes right of
    // them; the scopes analyse each preview frame after the grade
    m_colorPage = new QSplitter(Qt::Vertical);
    m_colorBottom = new QSplitter(Qt::Horizontal);
    m_colorBottom->addWidget(m_colorPanel);
    m_colorBottom->addWidget(m_scopes);
    m_colorBottom->setStretchFactor(0, 1);
    // A tour shows them (its steps point at them); otherwise as the user left them. Hidden only once in the splitter:
    // setVisible on the parentless panel would show it as a window of its own and take the activation from this one
    if (!m_tourSaved.active && !QSettings().value("color/scopesVisible", true).toBool()) m_scopes->hide();
    m_colorPage->addWidget(m_colorBottom);
    m_pages->addWidget(m_colorPage);
    watchSizes(m_colorPage, "colorPage");
    watchSizes(m_colorBottom, "colorBottom");
    applySizes(m_colorBottom, {1100, 420});
    // The window outline over the viewer (only while the Color page shows); the Comp page keeps its own meanwhile
    if (m_page != Page::Comp && m_page != Page::Edit) m_viewer->setOverlay(m_colorPanel->overlay()); // Edit: the shapes
    connectColorPage();
}

namespace {
constexpr const char* kUserSetKey = "layout/userSet"; // splitters the user dragged (their sizes are kept as they are)
}

void MainWindow::watchSizes(QSplitter* s, const QString& name)
{
    s->setObjectName(name);
    connect(s, &QSplitter::splitterMoved, this, [this, s] {
        if (m_autosaveDisabled || m_tourSaved.active) return; // test runs and tours leave the user's layout alone
        // Hidden panels report 0: keep their last size so they come back as they were
        const QString key = "layout/" + s->objectName();
        QList<int> sizes = s->sizes();
        const QVariantList saved = QSettings().value(key).toList();
        const QList<int> defaults = m_defaultSizes.value(s);
        QVariantList out;
        for (int i = 0; i < sizes.size(); ++i) {
            if (s->widget(i)->isHidden())
                sizes[i] = i < saved.size() ? saved[i].toInt() : i < defaults.size() ? defaults[i] : sizes[i];
            out << sizes[i];
        }
        QSettings().setValue(key, out);
        // The user's own size from now on: never fitted to the content on start (fitRightColumn), only "Reset panel
        // sizes" forgets it (it clears the whole layout group)
        QStringList user = QSettings().value(kUserSetKey).toStringList();
        if (!user.contains(s->objectName())) {
            user << s->objectName();
            QSettings().setValue(kUserSetKey, user);
        }
    });
}

void MainWindow::applySizes(QSplitter* s, const QList<int>& defaults)
{
    s->setSizes(sizesFor(s, defaults));
}

QList<int> MainWindow::sizesFor(QSplitter* s, const QList<int>& defaults)
{
    m_defaultSizes[s] = defaults;
    // A tour shows the defaults (its targets where it expects them); the user's sizes come back after it
    const QVariantList saved = m_tourSaved.active ? QVariantList() : QSettings().value("layout/" + s->objectName()).toList();
    if (saved.size() != defaults.size() || s->count() != defaults.size()) return defaults;
    QList<int> sizes;
    for (const QVariant& v : saved) sizes << std::max(0, v.toInt());
    return sizes;
}

void MainWindow::uncollapse(QWidget* panel)
{
    auto* s = qobject_cast<QSplitter*>(panel->parentWidget());
    if (!s || !s->isVisible() || panel->isHidden()) return;
    const int index = s->indexOf(panel);
    QList<int> sizes = s->sizes();
    if (index < 0 || sizes.value(index) > 0) return;
    int total = 0, shown = 0;
    for (int i = 0; i < sizes.size(); ++i)
        if (!s->widget(i)->isHidden()) {
            total += sizes[i];
            ++shown;
        }
    if (shown < 2 || total <= 0) return;
    const int share = total / shown;
    for (int i = 0; i < sizes.size(); ++i)
        if (!s->widget(i)->isHidden() && i != index) sizes[i] = sizes[i] * (total - share) / total;
    sizes[index] = share;
    s->setSizes(sizes);
}

void MainWindow::resetSizes()
{
    QSettings settings;
    settings.beginGroup("layout");
    settings.remove("");
    settings.endGroup();
    if (m_colorBottom) applySizes(m_colorBottom, {1100, 420});
    SplitterMemory::resetAll(this); // the splitters inside the panels (Media Pool, Media Storage, Effects)
    showPage(m_page); // sets the page's defaults again
    if (m_editRight->isVisibleTo(m_editRow)) {
        fitRightColumn(m_editRow->sizes(), true); // the column's default: as wide as its content
        applySizes(m_editRight, m_editMain->sizes());
    }
}

void MainWindow::applyPanelSet(bool pool, bool effects, bool storage, bool transcript, bool mixer, bool inspector)
{
    m_poolToggle->setChecked(pool);
    m_effectsToggle->setChecked(effects);
    m_storageToggle->setChecked(storage);
    m_transcriptToggle->setChecked(transcript);
    m_mixerToggle->setChecked(mixer);
    m_inspectorToggle->setChecked(inspector);
    if (m_page == Page::Edit) showPage(Page::Edit);
}

void MainWindow::updateLeftColumn()
{
    if (m_page == Page::Edit) m_editLeft->setVisible(m_poolToggle->isChecked() || m_effectsToggle->isChecked()
                                                  || m_storageToggle->isChecked());
}

int MainWindow::rightColumnWidth() const
{
    // The mixer's strips at their natural width; the Inspector usable (its sliders and fields need about this much)
    constexpr int kInspectorWidth = 320;
    const int inspector = m_inspectorToggle->isChecked()
                              ? std::max(kInspectorWidth, m_inspector->minimumSizeHint().width())
                              : 0;
    return std::max(inspector, m_mixer->naturalWidth());
}

void MainWindow::fitRightColumn(QList<int> sizes, bool exact)
{
    // The sizes are given: before the first layout the splitter's own are not the ones it will take
    if (sizes.size() == 2) {
        const int total = sizes[0] + sizes[1];
        const int want = std::min(rightColumnWidth(), total * 45 / 100); // many tracks: the strips scroll
        // A width the user dragged stays as it is (wider or narrower); one never set by the user (an older version's,
        // a default) only a few pixels wider than needed, else the room goes to the timeline
        const bool user = !exact && QSettings().value(kUserSetKey).toStringList().contains(m_editRow->objectName());
        m_mixer->setKeepWidth(user); // nor widened to fit all strips
        if (exact || (!user && sizes[1] > want + 16)) {
            sizes[0] = total - want;
            sizes[1] = want;
        }
    }
    m_editRow->setSizes(sizes);
}

void MainWindow::updateRightColumn()
{
    // The Inspector's place in the top row stays (its slot), so the row keeps its saved sizes either way
    const bool column = m_mixerToggle && m_mixerToggle->isChecked();
    // In place: in the column (built there at the start: the slot holds its first place in the row, the column still
    // gets its sizes below) or in the row
    if (column == (m_inspector->parentWidget() == m_editRight) && m_inspectorSlot->isHidden()) {
        m_editRight->setVisible(column);
        return;
    }
    const bool shown = m_inspectorToggle->isChecked();
    if (column) {
        const int at = m_editTop->indexOf(m_inspector);
        const int place = at >= 0 ? at : m_editTop->indexOf(m_inspectorSlot); // the slot: the first placement
        const int width = place >= 0 && shown ? m_editTop->sizes().value(place) : 0;
        if (at >= 0) m_editTop->replaceWidget(at, m_inspectorSlot); // same size, the inspector comes out
        m_inspectorSlot->hide();
        m_editRight->insertWidget(0, m_inspector);
        m_inspector->setVisible(shown);
        m_editRight->show();
        // The column as wide as the Inspector was (else as the mixer's own), the saved width if there is one: the
        // user's dragged width as it is; one never set by the user not much wider than its content needs (0.20 saved
        // 520 px for two tracks) - the room goes to the timeline. Only when the column comes
        const int total = std::max(m_editRow->width(), 1000);
        const int right = width > 0 ? width : rightColumnWidth();
        fitRightColumn(sizesFor(m_editRow, {total - right - m_editRow->handleWidth(), right}), false);
        applySizes(m_editRight, m_editMain->sizes()); // the mixer's top level with the timeline's
    } else {
        const int at = m_editTop->indexOf(m_inspectorSlot);
        if (at >= 0) {
            m_editTop->replaceWidget(at, m_inspector);
            m_inspectorSlot->setParent(this);
            m_inspectorSlot->hide();
        } else {
            m_editTop->insertWidget(3, m_inspector);
        }
        m_inspector->setVisible(shown);
        m_editRight->hide();
    }
}

void MainWindow::setViewerLarge(bool on)
{
    if (on && m_viewerAway.active) on = false; // no viewer in the window to enlarge (second monitor)
    if (on != m_large.active) {
        if (!on) {
            // Back as it was: the panels it hid, then the sizes (no splitterMoved: the saved layout stays as it is)
            const LargeViewer was = std::exchange(m_large, {});
            for (const QPointer<QWidget>& w : was.hidden)
                if (w) w->show();
            // Scroll positions too: a panel laid out at another size on the way back loses them (the Inspector)
            // Outer splitters first: an inner one restored before its parent has its width back scales its sizes to
            // the width it has then and loses pixels to rounding
            auto depth = [](QWidget* w) {
                int d = 0;
                for (; w; w = w->parentWidget()) ++d;
                return d;
            };
            auto sizes = was.sizes;
            std::stable_sort(sizes.begin(), sizes.end(), [&depth](const auto& a, const auto& b) {
                return depth(a.first) < depth(b.first);
            });
            auto restore = [sizes, scrolls = was.scrolls] {
                for (const auto& [splitter, list] : sizes)
                    if (splitter) splitter->setSizes(list);
                for (const auto& [bar, value] : scrolls)
                    if (bar) bar->setValue(value);
            };
            restore();
            // Panels that size themselves when shown (the mixer grows to fit its strips) do it in a zero timer: once
            // more after that, the layout is the one from before (unless the large viewer is on again meanwhile)
            QTimer::singleShot(0, this, [this, restore] {
                if (!m_large.active) restore();
            });
        } else {
            // What stays: the viewer and the timeline (if it is on this page); every splitter above them hides its
            // other panels and gives their room (and their handles) to the viewer's side, else the timeline's
            QList<QWidget*> keep{m_viewer};
            if (m_pages->currentWidget()->isAncestorOf(m_timeline)) keep << m_timeline;
            auto holds = [](QWidget* w, QWidget* k) { return w == k || w->isAncestorOf(k); };
            QList<QSplitter*> splitters;
            for (QWidget* k : keep)
                for (QWidget* w = k->parentWidget(); w && w != this; w = w->parentWidget())
                    if (auto* s = qobject_cast<QSplitter*>(w); s && !splitters.contains(s)) splitters << s;
            m_large.active = true;
            // Every splitter's sizes before any of them changes: giving an outer splitter's room to the viewer's side
            // widens the inner ones at once (the timeline's splitter beside the mixer), their sizes then are not the
            // ones to come back to
            QHash<QSplitter*, QList<int>> original;
            for (QSplitter* s : splitters) original.insert(s, s->sizes());
            QSize viewer = m_viewer->size(); // its size once the room is given (the layout follows later)
            QSplitter* shared = nullptr;     // the splitter with the viewer above the timeline
            int viewerAt = -1, timelineAt = -1;
            for (QSplitter* s : splitters) {
                QList<int> after = s->sizes();
                int freed = 0, gainer = -1, timelineIndex = -1;
                for (int i = 0; i < s->count(); ++i) {
                    QWidget* w = s->widget(i);
                    if (holds(w, m_viewer)) gainer = i;
                    else if (keep.size() > 1 && holds(w, m_timeline)) timelineIndex = i;
                    else if (!w->isHidden()) {
                        freed += after[i] + s->handleWidth();
                        after[i] = 0;
                        for (QScrollBar* bar : w->findChildren<QScrollBar*>()) m_large.scrolls.append({bar, bar->value()});
                        w->hide();
                        m_large.hidden << w;
                    }
                }
                if (gainer >= 0 && timelineIndex >= 0 && s->orientation() == Qt::Vertical)
                    std::tie(shared, viewerAt, timelineAt) = std::tuple(s, gainer, timelineIndex);
                if (gainer < 0) gainer = timelineIndex;
                if (freed == 0 || gainer < 0) continue;
                m_large.sizes.append({s, original.value(s)});
                after[gainer] += freed;
                s->setSizes(after);
                if (s->widget(gainer)->isAncestorOf(m_viewer) || s->widget(gainer) == m_viewer)
                    (s->orientation() == Qt::Horizontal ? viewer.rwidth() : viewer.rheight()) += freed;
            }
            // With the room beside it the picture is often limited by the height (one viewer, 16:9 on a wide
            // screen): then the timeline gives the viewer height too, as much as the picture can use, but keeps a
            // third of the column (or its own height if that is less)
            if (shared) {
                QList<int> sizes = shared->sizes();
                const int total = std::accumulate(sizes.begin(), sizes.end(), 0);
                const int floor = std::max(total / 3, m_timeline->minimumSizeHint().height());
                const int take = std::min(m_viewer->heightFor(viewer.width()) - viewer.height(),
                                          sizes[timelineAt] - floor);
                if (take > 0) {
                    if (std::none_of(m_large.sizes.begin(), m_large.sizes.end(),
                                     [shared](const auto& entry) { return entry.first == shared; }))
                        m_large.sizes.append({shared, original.value(shared)});
                    sizes[timelineAt] -= take;
                    sizes[viewerAt] += take;
                    shared->setSizes(sizes);
                }
            }
        }
    }
    if (m_largeAction) m_largeAction->setChecked(m_large.active);
    // Two enabled Esc shortcuts are ambiguous and neither fires: while recording, Esc ends the take first
    updateEscapeKeys();
}

namespace {

// The keys of these window actions work in the preview's own windows too (full screen, second monitor): an action's
// window shortcut counts in every window it is added to
QList<QAction*> previewWindowActions()
{
    QList<QAction*> out;
    for (const char* id : {"play_pause", "shuttle_back", "stop", "shuttle_fwd", "frame_prev", "frame_next", "second_prev",
                           "second_next", "edit_prev", "edit_next", "go_start", "go_end", "mark_in", "mark_out"})
        if (QAction* a = InputBindings::instance().action(id)) out << a;
    return out;
}

// A screen's name for SecondScreenPlan: QScreen::name(), its place on the desktop where the platform gives none
QString screenId(const QScreen* s)
{
    if (!s) return {};
    return s->name().isEmpty() ? QString("@%1,%2").arg(s->geometry().x()).arg(s->geometry().y()) : s->name();
}

QVector<SecondScreenPlan::Screen> screenList(const QScreen* without = nullptr)
{
    QVector<SecondScreenPlan::Screen> out;
    for (QScreen* s : QGuiApplication::screens())
        if (s != without) out.append({screenId(s), s->geometry()});
    return out;
}

} // namespace

void MainWindow::setAppFullScreen(bool on)
{
    if (on != isFullScreen()) {
        if (on) {
            m_maximizedBeforeFullScreen = isMaximized();
            showFullScreen();
        } else if (m_maximizedBeforeFullScreen) {
            showMaximized();
        } else {
            showNormal();
        }
    }
    if (m_appFullScreenAction) m_appFullScreenAction->setChecked(isFullScreen());
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    // Left by the window manager (or entered by it): the menu entry shows the state
    if (event->type() == QEvent::WindowStateChange && m_appFullScreenAction)
        m_appFullScreenAction->setChecked(isFullScreen());
}

void MainWindow::setViewerFullScreen(bool on)
{
    if (on && !m_fullScreen) {
        m_fullScreen = new FullScreenViewer(m_engine, m_viewer->picture()); // no parent: see there; deleted here
        connect(m_fullScreen, &FullScreenViewer::exitRequested, this, [this] { setViewerFullScreen(false); });
        // The keys of these window actions work in the full-screen window too (an action's window shortcut counts in
        // every window it is added to)
        m_fullScreen->addActions(previewWindowActions());
        if (QAction* a = InputBindings::instance().action("viewer_full_screen")) m_fullScreen->addAction(a);
        m_focusBeforeFullScreen = QApplication::focusWidget();
        m_fullScreen->showOn(screen());
        // Belongs to the main window for the window manager (no own taskbar entry, stays with it) - set after the
        // first show, the parentless window's surface is fixed by then (FullScreenViewer.h)
        if (QWindow* w = m_fullScreen->windowHandle()) w->setTransientParent(windowHandle());
        // The picture as sharp as the screen (the viewer's small preview enlarged looked soft), rendered no larger
        updatePreviewEdge();
        connect(m_fullScreen, &FullScreenViewer::previewEdgeWanted, this, &MainWindow::updatePreviewEdge);
    } else if (!on && m_fullScreen) {
        FullScreenViewer* fs = std::exchange(m_fullScreen, nullptr);
        fs->hide();
        fs->deleteLater();
        updatePreviewEdge(); // back to the viewer's own size (or the second monitor's)
        raise();
        activateWindow();
        // Without a parent nothing hands the focus back: the main window's focus widget stayed unfocused
        if (m_focusBeforeFullScreen && m_focusBeforeFullScreen->window() == this) m_focusBeforeFullScreen->setFocus();
    }
    if (m_fullScreenAction) m_fullScreenAction->setChecked(m_fullScreen != nullptr);
}

FullScreenViewer* MainWindow::fullScreenViewer() const
{
    return m_fullScreen;
}

int MainWindow::previewEdgeFor(PreviewResolution::Choice viewerRes) const
{
    const QSize project = m_engine->frameSize();
    int edge = 0;
    // The viewer's automatic: the pixels its picture covers (0 before it is shown: the engine's default)
    if (!m_viewerAway.active && m_viewer) edge = PreviewResolution::edge(viewerRes, m_viewer->wantedPreviewEdge(), project);
    if (m_fullScreen) edge = std::max(edge, PreviewResolution::edge(viewerRes, m_fullScreen->wantedPreviewEdge(), project));
    if (m_output) edge = std::max(edge, PreviewResolution::edge(m_monitorRes, m_output->wantedPreviewEdge(), project));
    return edge;
}

void MainWindow::updatePreviewEdge()
{
    m_engine->setPreviewEdge(previewEdgeFor(m_viewerRes));
    updateResolutionMenus();
    if (m_viewer) updateProxyHint(); // the slow hint's offer depends on the preview size
}

std::optional<PreviewResolution::Choice> MainWindow::smallerPreview() const
{
    if (m_engine->previewFullSize() || m_viewerAway.active || !m_viewer) return std::nullopt;
    const QSize project = m_engine->frameSize();
    // The automatic size of what shows the main window's preview now (as updateResolutionMenus)
    const int autoEdge = m_fullScreen ? m_fullScreen->wantedPreviewEdge() : m_viewer->wantedPreviewEdge();
    const PreviewResolution::Choice c = PreviewResolution::smaller(m_viewerRes, autoEdge, project);
    if (c == m_viewerRes) return std::nullopt;
    const QSize now = m_engine->previewSize(), then = PreviewResolution::sizeFor(project, previewEdgeFor(c));
    if (qint64(then.width()) * then.height() * 4 > qint64(now.width()) * now.height() * 3) return std::nullopt;
    return c;
}

void MainWindow::updateResolutionMenus()
{
    using namespace PreviewResolution;
    const QSize project = m_engine->frameSize();
    for (const Target target : {Target::Viewer, Target::Monitor}) {
        int autoEdge = 0; // what the display wants right now (0 = not shown)
        if (target == Target::Viewer) autoEdge = m_fullScreen ? m_fullScreen->wantedPreviewEdge() : m_viewer ? m_viewer->wantedPreviewEdge() : 0;
        else if (m_output) autoEdge = m_output->wantedPreviewEdge();
        const Choice chosen = target == Target::Viewer ? m_viewerRes : m_monitorRes;
        for (const Choice choice : kChoices) {
            QAction* a = m_resolutionActions[int(target)][int(choice)];
            if (!a) continue;
            const bool larger = largerThanProject(choice, project);
            const QString size = sizeText(resultSize(choice, autoEdge, project));
            a->setText(larger ? T("%1 (größer als das Projekt)").arg(label(choice)) : label(choice));
            a->setEnabled(!larger || choice == chosen); // a stored choice stays visible (it gives the project size)
            if (choice == Choice::Auto)
                a->setToolTip(autoEdge > 0 ? T("So groß, wie die Vorschau gerade angezeigt wird: jetzt %1, höchstens %2 "
                                               "Pixel an der längeren Seite").arg(size).arg(kAutoCap)
                                           : T("So groß, wie die Vorschau gerade angezeigt wird"));
            else if (choice == Choice::Project)
                a->setToolTip(T("Vorschau: %1, so groß wie das Projekt. Am schärfsten, braucht aber einen schnellen "
                                "Rechner.").arg(size));
            else if (larger)
                a->setToolTip(T("Das Projekt hat nur %1: größer wird die Vorschau nie.").arg(sizeText(project)));
            else
                a->setToolTip(T("Vorschau: %1. Die kürzere Seite hat %2 Pixel, die längere folgt dem Seitenverhältnis "
                                "des Projekts (ein 9:16-Projekt bei 1080p: 1080×1920).").arg(size).arg(lines(choice)));
        }
    }
}

void MainWindow::setPreviewResolution(PreviewResolution::Target target, PreviewResolution::Choice choice)
{
    PreviewResolution::Choice& current = target == PreviewResolution::Target::Viewer ? m_viewerRes : m_monitorRes;
    const bool changed = current != choice;
    current = choice;
    if (!m_autosaveDisabled && changed) PreviewResolution::store(target, choice);
    if (QAction* a = m_resolutionActions[int(target)][int(choice)]) a->setChecked(true);
    updatePreviewEdge();
    // What the choice did, for ~2 s over the picture: the size the engine renders now (the larger wish of main window
    // and monitor, the project size while zoomed to 100 %)
    if (changed && m_viewer) m_viewer->showNotice(T("Vorschau: %1").arg(PreviewResolution::sizeText(m_engine->previewSize())));
}

void MainWindow::moveViewerToNextScreen()
{
    m_screenPlan.setScreens(screenList());
    const QString next = m_screenPlan.next(screenId(screen()));
    QScreen* target = nullptr;
    for (QScreen* s : QGuiApplication::screens())
        if (!next.isEmpty() && screenId(s) == next) target = s; // empty = back into the window
    if (!target && !m_output) statusBar()->showMessage(T("Kein zweiter Monitor gefunden."), 5000);
    showPreviewOn(target);
}

void MainWindow::showPreviewOn(QScreen* screen)
{
    if (m_output) {
        PreviewOutput* old = std::exchange(m_output, nullptr);
        old->disconnect(this);
        delete old; // hides its window at once (a removed monitor: before Qt moves it over the main window)
    }
    if (screen) {
        auto* out = new ScreenOutput(m_engine, screen, m_programFrame.isNull() ? m_viewer->picture() : m_programFrame, this);
        out->addActions(previewWindowActions());
        if (m_secondScreenAction) out->addActions({m_secondScreenAction});
        // Deleted later by showPreviewOn: lost() may come from inside its own window's event
        connect(out, &PreviewOutput::lost, this, [this, out] {
            if (m_output != out) return;
            m_output = nullptr;
            out->disconnect(this);
            out->deleteLater();
            showPreviewOn(nullptr);
        });
        connect(out, &PreviewOutput::previewEdgeWanted, this, &MainWindow::updatePreviewEdge);
        // The main window moved onto the preview's monitor (dragged, or its own monitor went away)
        if (QWindow* handle = windowHandle())
            connect(handle, &QWindow::screenChanged, out, [this] { screensChanged(nullptr); });
        m_output = out;
        m_screenPlan.setScreens(screenList());
        m_screenPlan.setTarget(screenId(screen));
    } else {
        m_screenPlan.setTarget({});
    }
    updateViewerArea();
    updatePreviewEdge();
    if (m_secondScreenAction) m_secondScreenAction->setChecked(m_output != nullptr);
    if (m_viewerScreenModeMenu) m_viewerScreenModeMenu->menuAction()->setVisible(m_output != nullptr);
}

void MainWindow::screensChanged(QScreen* removed)
{
    // A screen coming never moves the preview; one going takes it back into the window (ScreenOutput says lost() too),
    // and so does the main window ending up on the preview's monitor (it would be covered)
    const bool gone = m_screenPlan.setScreens(screenList(removed));
    if (m_output && (gone || m_screenPlan.coversMain(screenId(screen())))) showPreviewOn(nullptr);
}

void MainWindow::feedPreviewOutput(const QImage& frame)
{
    const bool source = m_engine->mode() == Engine::Mode::Source;
    if (!source) m_programFrame = frame; // shared, not copied
    if (m_output && !SecondScreenPlan::holdsTimeline(m_secondScreenMode, source))
        m_output->showFrame(frame, m_engine->frameSize());
}

void MainWindow::setSecondScreenMode(SecondScreenPlan::Mode mode)
{
    if (mode == m_secondScreenMode) return;
    m_secondScreenMode = mode;
    if (!m_autosaveDisabled) SecondScreenPlan::storeMode(mode);
    if (QAction* a = m_secondScreenModeActions[int(mode)]) a->setChecked(true); // also when Settings changed it
    updateViewerArea();
    // Now the source belongs onto the monitor too (or no longer): the current picture
    if (m_output) m_output->showFrame(SecondScreenPlan::holdsTimeline(mode, m_engine->mode() == Engine::Mode::Source)
                                          ? m_programFrame : m_viewer->picture(),
                                      m_engine->frameSize());
}

void MainWindow::updateViewerArea()
{
    const bool inWindow = SecondScreenPlan::viewerInWindow(m_secondScreenMode, m_output != nullptr);
    m_viewer->setProgramElsewhere(SecondScreenPlan::programElsewhere(m_secondScreenMode, m_output != nullptr));
    if (inWindow == !m_viewerAway.active) return;
    if (inWindow) restoreViewerArea();
    else hideViewerArea();
    updatePreviewEdge(); // the viewer's own wish counts only while it is there
}

void MainWindow::hideViewerArea()
{
    if (m_viewerAway.active) return;
    setViewerLarge(false);
    m_viewerAway.active = true;
    // The viewer, and every splitter it leaves without a visible widget (the Edit page's top row with the side panels
    // off): the room goes to what is left beside/below it - the timeline keeps its zoom and scroll (core rule)
    if (m_viewer->parentWidget() == m_editTop && m_page == Page::Edit) {
        // The Edit page: the timeline moves up into the viewer's place, from the top bar down between the left column
        // and the Inspector (their widths stay); the mixer keeps its own column and height
        const QList<int> top = m_editTop->sizes();
        m_viewerAway.sizes.append({m_editTop, top});
        m_viewerAway.sizes.append({m_editMain, m_editMain->sizes()});
        QWidget* focus = focusWidget(); // the window's, also while the monitor's window is the active one
        const int at = m_editTop->indexOf(m_viewer);
        m_viewerAway.timelinePolicy = m_timeline->sizePolicy();
        m_editTop->replaceWidget(at, m_timeline); // the viewer comes out ...
        m_editMain->insertWidget(1, m_viewer);    // ... into the timeline's old place
        m_viewer->hide();
        m_timeline->show();
        // The viewer's stretch and width: else the splitter shares the row out anew and the side panels grow
        m_editTop->setStretchFactor(at, 1);
        m_editTop->setSizes(top);
        m_viewerAway.hidden << m_viewer;
        m_viewerAway.timelineUp = true;
        if (focus && m_timeline->isAncestorOf(focus)) focus->setFocus(); // moving a widget takes its focus
        watchViewerAwaySplitters();
        return;
    }
    QWidget* gone = m_viewer;
    while (auto* s = qobject_cast<QSplitter*>(gone->parentWidget())) {
        m_viewerAway.sizes.append({s, s->sizes()});
        gone->hide();
        m_viewerAway.hidden << gone;
        bool others = false;
        for (int i = 0; i < s->count(); ++i)
            others = others || (s->widget(i) != gone && !s->widget(i)->isHidden());
        if (others) break;
        gone = s;
    }
    if (m_viewerAway.hidden.isEmpty()) { // not in a splitter (not built yet)
        m_viewer->hide();
        m_viewerAway.hidden << m_viewer;
    }
    watchViewerAwaySplitters();
}

void MainWindow::watchViewerAwaySplitters()
{
    for (const auto& [splitter, sizes] : m_viewerAway.sizes)
        if (QSplitter* s = splitter)
            m_viewerAway.watches << connect(s, &QSplitter::splitterMoved, this, [this, s] { m_viewerAway.moved.insert(s); });
}

void MainWindow::restoreViewerArea()
{
    if (!m_viewerAway.active) return;
    const ViewerAway was = std::exchange(m_viewerAway, {});
    for (const QMetaObject::Connection& c : was.watches) disconnect(c);
    // The splitters the user dragged meanwhile keep that: their sizes now for what stayed in view, the saved size only
    // for what comes back (the other widths shrink in proportion to make room)
    QHash<QSplitter*, QList<int>> dragged;
    for (const auto& [splitter, saved] : was.sizes) {
        QSplitter* s = splitter;
        if (!s || !was.moved.contains(s) || s->count() != saved.size()) continue;
        QList<int> now = s->sizes();
        int back = 0, stayed = 0;
        for (int i = 0; i < now.size(); ++i) {
            if (s->widget(i)->isHidden()) back += saved[i];
            else stayed += now[i];
        }
        const int total = std::accumulate(now.begin(), now.end(), 0);
        for (int i = 0; i < now.size(); ++i)
            now[i] = s->widget(i)->isHidden() ? saved[i]
                     : stayed > 0         ? int(qint64(now[i]) * std::max(0, total - back) / stayed)
                                          : now[i];
        dragged.insert(s, now);
    }
    if (was.timelineUp && m_editTop->indexOf(m_timeline) >= 0) { // the viewer and the timeline swap back
        QWidget* focus = focusWidget(); // the window's, also while the monitor's window is the active one
        m_editTop->replaceWidget(m_editTop->indexOf(m_timeline), m_viewer);
        m_editMain->insertWidget(1, m_timeline);
        m_timeline->setSizePolicy(was.timelinePolicy);
        m_timeline->show();
        if (focus && m_timeline->isAncestorOf(focus)) focus->setFocus();
    }
    for (const QPointer<QWidget>& w : was.hidden)
        if (w) w->show();
    // Outer splitters first (see setViewerLarge: an inner one restored first is scaled again)
    for (auto it = was.sizes.crbegin(); it != was.sizes.crend(); ++it)
        if (it->first) it->first->setSizes(dragged.value(it->first, it->second));
}
