// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: layout, top bar, page bar and pages.
#include "app/MainWindow.h"

#include "app/AiChatPanel.h"

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
    m_mediaPool = new MediaPool(m_project, m_engine);
    m_effects = new EffectsLibrary;
    m_storage = new MediaStorage;
    m_viewer = new Viewer(m_engine);
    m_multicam = new MulticamView(m_project, m_selection, m_editor);
    m_viewer->setMulticamView(m_multicam);
    m_inspector = new Inspector(m_editor);
    m_inspector->setFrameSize(m_project->timelineFormat().size());
    m_timeline = new TimelinePanel(m_editor);
    m_deliver = new DeliverPanel(m_project);
    m_mixer = new Mixer(m_project, m_engine);
    m_mixer->setEditor(m_editor);
    m_mixer->hide();
    m_colorPanel = new ColorPanel(m_editor);
    m_viewer->setOverlay(m_colorPanel->overlay()); // window outline, only while the Color page shows
    m_transcript = new TranscriptPanel(m_project, m_editor);
    m_scopes = new ScopesPanel;
    m_scopes->setVisible(QSettings().value("color/scopesVisible", true).toBool());

    // Pages; the shared panels (pool, viewer, timeline) move along when switching
    m_editTop = new QSplitter(Qt::Horizontal);
    m_editLeft = new QSplitter(Qt::Vertical);
    m_editLeft->addWidget(m_effects);
    m_editMain = new QSplitter(Qt::Vertical);
    m_editMain->addWidget(m_editTop);
    m_editBottom = new QSplitter(Qt::Horizontal); // mixer to the right of the timeline like in DaVinci
    m_editBottom->addWidget(m_mixer);
    m_editBottom->setStretchFactor(0, 1);
    m_editMain->addWidget(m_editBottom);
    // Media page like DaVinci: Media Storage | Viewer on top, the Media Pool below
    m_mediaPage = new QSplitter(Qt::Vertical);
    m_mediaTop = new QSplitter(Qt::Horizontal);
    m_mediaPage->addWidget(m_mediaTop);
    m_deliverPage = new QSplitter(Qt::Horizontal);
    m_deliverRight = new QSplitter(Qt::Vertical);
    m_deliverPage->addWidget(m_deliver);
    m_deliverPage->addWidget(m_deliverRight);
    m_deliverPage->addWidget(m_deliver->queuePanel()); // Render queue on the right like in DaVinci

    m_pages = new QStackedWidget;
    m_pages->addWidget(m_mediaPage);
    m_pages->addWidget(m_editMain);
    m_pages->addWidget(m_deliverPage);
    // Color page like DaVinci (slimmed down): viewer on top, timeline in the middle, color wheels below
    // and the scopes right of them (DaVinci's scope panel); the scopes analyse each preview frame after the grade
    m_colorPage = new QSplitter(Qt::Vertical);
    m_colorBottom = new QSplitter(Qt::Horizontal);
    m_colorBottom->addWidget(m_colorPanel);
    m_colorBottom->addWidget(m_scopes);
    m_colorBottom->setStretchFactor(0, 1);
    m_colorPage->addWidget(m_colorBottom);
    m_pages->addWidget(m_colorPage);
    const QPair<QSplitter*, const char*> splitters[] = {
        {m_editTop, "editTop"},         {m_editMain, "editMain"},
        {m_editBottom, "editBottom"},   {m_mediaPage, "mediaPage"},   {m_mediaTop, "mediaTop"},
        {m_deliverPage, "deliverPage"}, {m_deliverRight, "deliverRight"}, {m_colorPage, "colorPage"},
        {m_colorBottom, "colorBottom"}};
    for (const auto& [sp, name] : splitters) watchSizes(sp, name);
    applySizes(m_colorBottom, {1100, 420});

    // The AI chat beside the pages (all of them), left or right (MainWindowChat.cpp)
    buildChat();

    auto* root = new QWidget;
    root->setObjectName("Root");
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(buildTopBar());
    lay->addWidget(buildUpdateBar());
    lay->addWidget(m_chatSplit, 1);
    lay->addWidget(buildPageBar());
    setCentralWidget(root);

    // The Edit page, or the nearest shown one when Edit is hidden
    const QString start = m_pageVisibility.nearestVisible(pageId(Page::Edit));
    Page first = Page::Edit;
    for (Page p : {Page::Media, Page::Edit, Page::Comp, Page::Color, Page::Deliver})
        if (pageId(p) == start) first = p;
    showPage(first);
}

QWidget* MainWindow::buildTopBar()
{
    // Top bar like in DaVinci: show/hide panels left/right, project name in the middle
    // Visibility is saved (panels/<key>); test runs do not change the setting
    auto makeToggle = [this](const QString& text, const QString& key, bool def, QWidget* panel) {
        auto* b = new QToolButton;
        b->setObjectName("panelToggle_" + key);
        b->setText(text);
        b->setCheckable(true);
        b->setChecked(QSettings().value("panels/" + key, def).toBool());
        // Only hide panels not yet inserted (show() without a parent opened its own window)
        if (panel->parentWidget() || !b->isChecked()) panel->setVisible(b->isChecked());
        connect(b, &QToolButton::toggled, this, [this, key, panel](bool on) {
            setViewerLarge(false); // the layout comes back, with this panel as asked
            panel->setVisible(on);
            updateLeftColumn();
            if (!m_autosaveDisabled && !m_tourSaved.active) QSettings().setValue("panels/" + key, on); // a tour sets its own
        });
        return b;
    };
    m_poolToggle = makeToggle("▦ " + T("Medienpool"), "mediaPool", true, m_mediaPool);
    m_effectsToggle = makeToggle("✦ " + T("Effekte"), "effects", false, m_effects);
    m_storageToggle = makeToggle("▤ " + T("Dateibrowser"), "mediaStorage", false, m_storage);
    m_storageToggle->setToolTip(T("Ordner durchsuchen und Clips direkt in Medienpool/Timeline ziehen"));
    m_inspectorToggle = makeToggle("☰ " + T("Inspektor"), "inspector", true, m_inspector);
    m_mixerToggle = makeToggle("▥ " + T("Mischpult"), "mixer", false, m_mixer);
    m_transcriptToggle = makeToggle("¶ " + T("Transkript"), "transcript", false, m_transcript);
    m_transcriptToggle->setToolTip(T("Gesprochenen Text der Tonclips zeigen; Wörter löschen schneidet die Timeline"));
    m_chatToggle = makeToggle("✧ " + T("KI"), "aiChat", false, m_chatPanel);
    m_chatToggle->setObjectName("ai_chat_toggle");
    m_chatToggle->setToolTip(T("KI-Chat: sag einer KI, was sie im offenen Projekt tun soll"));

    auto* title = m_titleLabel = new QLabel;
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
    // Sidebar at the bottom like in DaVinci (Media | Edit | Deliver)
    auto* bar = new QWidget;
    bar->setObjectName("PageBar");
    bar->setStyleSheet(
        QString("QWidget#PageBar { background: %1; border-top: 1px solid %2; }"
                "QToolButton { color: %3; padding: 4px 18px; border-radius: 0; }"
                "QToolButton:checked { color: %4; background: transparent; border-bottom: 2px solid %4; }"
                "QToolButton:hover { color: %5; }")
            .arg(Theme::window.name(), Theme::border.name(), Theme::textDim.name(), Theme::primary.name(),
                 Theme::text.name()));
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(4);

    auto* brand = new QLabel(appName()); // with the version, like DaVinci (bug reports, screenshots)
    brand->setStyleSheet(QString("color: %1; font-weight: 600;").arg(Theme::textDim.name()));
    lay->addWidget(brand);
    lay->addStretch(1);

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
        b->setText(QString("%1\n%2").arg(pg.icon, T(pg.name)));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
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
    lay->addStretch(1);
    lay->addSpacing(brand->sizeHint().width());
    updatePageButtons();
    return bar;
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
    m_pageVisibility.save();
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
    m_pageVisibility.save();
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
        m_viewer->setOverlay(m_colorPanel->overlay());
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
        m_editTop->insertWidget(3, m_inspector);
        m_editBottom->insertWidget(0, m_timeline);
        m_editBottom->setStretchFactor(0, 1);
        applySizes(m_editBottom, {1300, 300});
        m_editTop->setStretchFactor(1, 1);
        applySizes(m_editTop, {360, 880, 320, 360});
        applySizes(m_editMain, {480, 420});
        m_mediaPool->setVisible(m_poolToggle->isChecked());
        m_storage->setVisible(m_storageToggle->isChecked());
        updateLeftColumn();
        m_pages->setCurrentWidget(m_editMain);
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
        if (before != Page::Comp) { // the selected clip (the playhead goes into it), else the one under the playhead
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
    if (auto* b = m_pageButtons->button(int(page))) b->setChecked(true);
    updatePageButtons();
    const bool edit = page == Page::Edit;
    m_poolToggle->setEnabled(edit);
    m_effectsToggle->setEnabled(edit);
    m_storageToggle->setEnabled(edit);
    m_inspectorToggle->setEnabled(edit);
    m_mixerToggle->setEnabled(edit);
    m_transcriptToggle->setEnabled(edit);
    updateViewerArea();
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
    });
}

void MainWindow::applySizes(QSplitter* s, const QList<int>& defaults)
{
    m_defaultSizes[s] = defaults;
    // A tour shows the defaults (its targets where it expects them); the user's sizes come back after it
    const QVariantList saved = m_tourSaved.active ? QVariantList() : QSettings().value("layout/" + s->objectName()).toList();
    if (saved.size() != defaults.size() || s->count() != defaults.size()) {
        s->setSizes(defaults);
        return;
    }
    QList<int> sizes;
    for (const QVariant& v : saved) sizes << std::max(0, v.toInt());
    s->setSizes(sizes);
}

void MainWindow::resetSizes()
{
    QSettings settings;
    settings.beginGroup("layout");
    settings.remove("");
    settings.endGroup();
    applySizes(m_colorBottom, {1100, 420});
    showPage(m_page); // sets the page's defaults again
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
    if (m_largeEscape) m_largeEscape->setEnabled(m_large.active && !(m_voiceover && m_voiceover->recording()));
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

void MainWindow::updatePreviewEdge()
{
    using namespace PreviewResolution;
    const QSize project = m_engine->frameSize();
    int edge = 0;
    // The viewer's automatic: the pixels its picture covers (0 before it is shown: the engine's default)
    if (!m_viewerAway.active && m_viewer) edge = PreviewResolution::edge(m_viewerRes, m_viewer->wantedPreviewEdge(), project);
    if (m_fullScreen) edge = std::max(edge, PreviewResolution::edge(m_viewerRes, m_fullScreen->wantedPreviewEdge(), project));
    if (m_output) edge = std::max(edge, PreviewResolution::edge(m_monitorRes, m_output->wantedPreviewEdge(), project));
    m_engine->setPreviewEdge(edge);
    updateResolutionMenus();
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
        m_output = out;
        m_screenPlan.setScreens(screenList());
        m_screenPlan.setTarget(screenId(screen));
    } else {
        m_screenPlan.setTarget({});
    }
    updateViewerArea();
    updatePreviewEdge();
    if (m_secondScreenAction) m_secondScreenAction->setChecked(m_output != nullptr);
}

void MainWindow::screensChanged(QScreen* removed)
{
    // A screen coming never moves the preview; one going takes it back into the window (ScreenOutput says lost() too)
    if (m_screenPlan.setScreens(screenList(removed)) && m_output) showPreviewOn(nullptr);
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
}

void MainWindow::restoreViewerArea()
{
    if (!m_viewerAway.active) return;
    const ViewerAway was = std::exchange(m_viewerAway, {});
    for (const QPointer<QWidget>& w : was.hidden)
        if (w) w->show();
    // Outer splitters first (see setViewerLarge: an inner one restored first is scaled again)
    for (auto it = was.sizes.crbegin(); it != was.sizes.crend(); ++it)
        if (it->first) it->first->setSizes(it->second);
}
