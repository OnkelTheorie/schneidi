// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the phone layout (ui/phone/PhoneShell.h). The same panels, actions and colors as on the desktop,
// rearranged for a phone: the desktop's top bar, page bar, menu bar and the pages' splitters are hidden, the viewer
// and the timeline move into the shell, every panel opens as the shell's sheet (one at a time).
#include "app/MainWindow.h"

#include "app/AiChatPanel.h"
#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "ui/BackgroundJobs.h"
#include "ui/ColorPanel.h"
#include "ui/DeliverPanel.h"
#include "ui/EffectsLibrary.h"
#include "ui/ElidedLabel.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/RenderQueuePanel.h"
#include "ui/Mixer.h"
#include "ui/TranscriptPanel.h"
#include "ui/Viewer.h"
#include "ui/phone/PhoneShell.h"
#include "ui/FrameView.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QCoreApplication>
#include <QBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QToolButton>
#include <QUndoStack>

namespace {
void trigger(const char* id)
{
    if (QAction* a = InputBindings::instance().action(QString::fromLatin1(id)); a && a->isEnabled()) a->trigger();
}
} // namespace

bool MainWindow::phoneLayoutWanted()
{
    const QString env = qEnvironmentVariable("SCHNEIDI_PHONE_LAYOUT");
    if (env == "1") return true;
    if (env == "0") return false;
    const QString setting = QSettings().value("ui/phoneLayout").toString();
    if (setting == "on") return true;
    if (setting == "off") return false;
    // The screen's own size, not the window's: a narrow desktop window keeps the desktop layout
    QScreen* screen = startScreen();
    return screen && PhoneShell::isPhoneSize(screen->size());
}

QWidget* MainWindow::buildPhoneTopBar()
{
    // The desktop top bar's look: the menus behind one button, the project name, undo and redo (arranged by
    // phoneArrangeTopBar: a row in portrait, the head of the tool rail in landscape)
    auto* bar = new QWidget;
    bar->setObjectName("TopBar");
    auto bigGlyph = [](QToolButton* b) {
        QFont f = b->font();
        f.setPixelSize(std::max(18, QFontMetrics(f).height()));
        b->setFont(f);
        b->setFocusPolicy(Qt::NoFocus);
    };
    auto* menuButton = new QToolButton(bar);
    menuButton->setObjectName("phoneMenu");
    menuButton->setText(QStringLiteral("⋮"));
    menuButton->setToolTip(T("Menü"));
    menuButton->setPopupMode(QToolButton::InstantPopup);
    menuButton->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0; }");
    bigGlyph(menuButton);
    auto* menu = new QMenu(menuButton);
    menu->addActions(menuBar()->actions()); // the menu bar's menus (File, Edit, ...), the same actions
    menuButton->setMenu(menu);
    m_titleLabel->setParent(bar);
    if (m_liveBox) m_liveBox->setParent(bar);
    const auto historyButton = [&](const char* name, const QString& symbol, const QString& tip, const char* action) {
        auto* b = new QToolButton(bar);
        b->setObjectName(QString::fromLatin1(name));
        b->setText(symbol);
        b->setToolTip(tip);
        bigGlyph(b);
        connect(b, &QToolButton::clicked, this, [action] { trigger(action); });
        return b;
    };
    QUndoStack* stack = m_project->undoStack();
    QToolButton* undo = historyButton("phoneUndo", QStringLiteral("↶"), T("Rückgängig"), "undo");
    QToolButton* redo = historyButton("phoneRedo", QStringLiteral("↷"), T("Wiederherstellen"), "redo");
    undo->setEnabled(stack->canUndo());
    redo->setEnabled(stack->canRedo());
    connect(stack, &QUndoStack::canUndoChanged, undo, &QWidget::setEnabled);
    connect(stack, &QUndoStack::canRedoChanged, redo, &QWidget::setEnabled);
    m_phoneTopBar = bar;
    phoneArrangeTopBar(false);
    return bar;
}

void MainWindow::phoneArrangeTopBar(bool landscape)
{
    QWidget* bar = m_phoneTopBar;
    if (!bar) return;
    auto* menuButton = bar->findChild<QToolButton*>("phoneMenu");
    auto* undo = bar->findChild<QToolButton*>("phoneUndo");
    auto* redo = bar->findChild<QToolButton*>("phoneRedo");
    // The jobs once adopted (phoneAdoptBackgroundJobs), the live box when the app has one
    QWidget* jobs = m_backgroundJobs && m_backgroundJobs->parentWidget() == bar ? m_backgroundJobs : nullptr;
    QWidget* live = m_liveBox && m_liveBox->parentWidget() == bar ? m_liveBox : nullptr;
    delete bar->layout(); // the widgets stay (children of the bar), only their arrangement changes
    // Portrait: the desktop top bar's color. Landscape: the rail's (window) color - the rail is one surface down to the
    // screen's edge, the camera cutout's strip beside it (the safe area, painted in the window color) included
    bar->setStyleSheet(QString("QWidget#TopBar { background: %1; border-bottom: 1px solid %2; }"
                               "QToolButton { min-width: 36px; min-height: 36px; padding: 2px; }")
                           .arg((landscape ? Theme::window : Theme::topBar).name(), Theme::border.name()));
    m_titleLabel->setAlignment(landscape ? Qt::AlignCenter : Qt::AlignVCenter | Qt::AlignHCenter);
    if (!landscape) {
        // One row: menu, name, (live), (jobs), undo, redo
        auto* lay = new QHBoxLayout(bar);
        lay->setContentsMargins(4, 2, 4, 2);
        lay->setSpacing(2);
        lay->addWidget(menuButton);
        lay->addWidget(m_titleLabel, 1);
        if (live) lay->addWidget(live);
        if (jobs) lay->addWidget(jobs);
        lay->addWidget(undo);
        lay->addWidget(redo);
        return;
    }
    // The rail's head: menu, undo, redo in a row, the name below them (cut with "…"), then live and jobs
    auto* lay = new QGridLayout(bar);
    lay->setContentsMargins(2, 2, 2, 4);
    lay->setHorizontalSpacing(0);
    lay->setVerticalSpacing(2);
    lay->addWidget(menuButton, 0, 0);
    lay->addWidget(undo, 0, 1);
    lay->addWidget(redo, 0, 2);
    lay->addWidget(m_titleLabel, 1, 0, 1, 3);
    int row = 2;
    if (live) lay->addWidget(live, row++, 0, 1, 3);
    if (jobs) lay->addWidget(jobs, row++, 0, 1, 3, Qt::AlignHCenter);
}

void MainWindow::enablePhoneLayout()
{
    if (m_phone) return;
    // The Edit page's state first (the viewer's overlay for shapes and painting): the phone layout is the Edit page
    if (m_page != Page::Edit) showPage(Page::Edit);
    setViewerLarge(false);
    m_phone = new PhoneShell;
    connect(m_phone, &PhoneShell::landscapeChanged, this, [this](bool landscape) {
        phoneArrangeTopBar(landscape);
#ifdef Q_OS_ANDROID
        // Landscape: the height is scarce, the status bar goes (a swipe from the edge shows it for a moment);
        // portrait keeps it (clock, battery). (The full-screen viewer is a window of its own: not touched.) Only once
        // the window is up: the shell's first layout in the constructor has no real size yet (it looked landscape)
        if (!isVisible()) return;
        if (landscape && !isFullScreen()) showFullScreen();
        else if (!landscape && isFullScreen()) showMaximized();
#endif
    });
    m_topBar->hide();
    m_pageBar->hide();
    m_chatSplit->hide(); // with every desktop page: their minimum sizes no longer count for the window
    menuBar()->hide();
    static_cast<QBoxLayout*>(centralWidget()->layout())->addWidget(m_phone, 1);
    m_phone->setTopBar(buildPhoneTopBar());
    m_phone->setViewer(m_viewer);
    m_phone->setTimeline(m_timeline);
    // Their minimums sized for the desktop's Edit page: on a phone in landscape the viewer, the timeline and the bars
    // share ~380 px of height, in portrait 411 px of width
    m_timeline->setCompact(true);
    m_timeline->view()->setMinimumHeight(64);
    // Rows a finger still hits but a phone's height holds (landscape: V2, V1 and A1 under the ruler); a start value
    // like touch mode's, the user's pinch on the headers changes them as before
    m_timeline->view()->setTrackHeights(56, 46);
    m_mediaPool->setCompactTiles(true); // two columns beside the bins in portrait
    m_mediaPool->setGeneratorsLast(true); // the material first, the text and shape tiles after it
    m_viewer->screen()->setMinimumSize(160, 72);
    m_viewer->setCompact(true);
    // The large viewer: nothing beside the viewer to hide (full screen stays)
    if (auto* large = m_viewer->findChild<QToolButton*>("viewAction_large")) large->hide();

    // The main tools: the desktop's panels and pages, one at a time in the sheet
    m_phone->addPanelTool("media", QStringLiteral("▦"), T("Medien"), [this] { return m_mediaPool; });
    m_phone->addPanelTool("effects", QStringLiteral("✦"), T("Effekte"), [this] { return m_effects; });
    m_phone->addActionTool("text", QStringLiteral("T"), T("Text"), [this] {
        trigger("add_title"); // at the playhead, selected: its fields in the Inspector
        m_phone->openSheet("inspector");
    }, false);
    m_phone->addPanelTool("mixer", QStringLiteral("▥"), T("Ton"), [this] { return m_mixer; });
    m_phone->addPanelTool("color", QStringLiteral("◐"), T("Farbe"), [this]() -> QWidget* {
        ensureColorPage();
        return m_colorPanel;
    });
    m_phone->addPanelTool("inspector", QStringLiteral("☰"), T("Inspektor"), [this] { return m_inspector; });
    m_phone->addPanelTool("transcript", QStringLiteral("¶"), T("Transkript"), [this] { return m_transcript; });
    m_phone->addPanelTool("ai", QStringLiteral("✧"), T("KI"), [this]() -> QWidget* { return m_chatPanel; });
    m_phone->addPanelTool("export", QStringLiteral("⇪"), T("Export"), [this]() -> QWidget* {
        // The settings and, below them, the render queue (on the desktop it stands beside them): "Add to Render
        // Queue" alone rendered nothing on the phone, the queue's "Render all" was not on the screen
        auto* box = new QWidget;
        box->setObjectName("phoneExport");
        auto* lay = new QVBoxLayout(box);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(1);
        lay->addWidget(m_deliver, 3);
        lay->addWidget(m_deliver->queuePanel(), 2);
        m_deliver->queuePanel()->show();
        return box;
    });
    m_phone->setPanelShare("export", 0.92); // tall at once: the settings and "Render all" without a drag

    // The clip tools (while clips are selected)
    m_phone->addActionTool("split", QStringLiteral("✂"), T("Teilen"), [] { trigger("split"); });
    m_phone->addClipPanelTool("volume", QStringLiteral("♪"), T("Lautstärke"), "inspector", [this] {
        // The Audio tab, its Volume section at the top; a video without sound keeps the tab it shows
        if (!m_inspector->showAudio()) m_viewer->showNotice(T("Der ausgewählte Clip hat keinen Ton."), 2500);
    });
    m_phone->addActionTool("speed", QStringLiteral("≫"), T("Geschwindigkeit"), [] { trigger("clip_speed"); });
    m_phone->addClipPanelTool("clipEffects", QStringLiteral("✦"), T("Effekte"), "effects");
    m_phone->addClipPanelTool("clipColor", QStringLiteral("◐"), T("Farbe"), "color");
    m_phone->addActionTool("delete", QStringLiteral("⌫"), T("Löschen"), [] { trigger("ripple_delete"); });

    connect(m_selection, &Selection::changed, m_phone, [this] { m_phone->setClipToolsShown(!m_selection->isEmpty()); });
    connect(m_phone, &PhoneShell::clipToolsLeft, this, [this] { m_selection->clear(); });
    m_phone->setClipToolsShown(!m_selection->isEmpty());
    // The desktop's panel toggles follow the sheet (menus and keys show the open one), without saving anything
    const auto syncToggles = [this](const QString& open) {
        const QPair<QToolButton*, const char*> toggles[] = {
            {m_poolToggle, "media"}, {m_effectsToggle, "effects"},       {m_inspectorToggle, "inspector"},
            {m_mixerToggle, "mixer"}, {m_transcriptToggle, "transcript"}, {m_chatToggle, "ai"}};
        for (const auto& [toggle, id] : toggles) {
            const QSignalBlocker block(toggle);
            toggle->setChecked(open == QLatin1String(id));
        }
        if (open == "color") playheadInto(selectedVideoClip()); // as the Color page: the clip being graded
    };
    connect(m_phone, &PhoneShell::sheetChanged, this, syncToggles);
    syncToggles({});
    if (m_backgroundJobs) phoneAdoptBackgroundJobs(); // (a window built before: tests)
    // The window's minimum from the phone layout now (the desktop's was set when its layout first ran): a resize to
    // the phone's size before the first show must not be held at the desktop's minimum
    m_phone->ensurePolished(); // (style sheets: the sizes they give)
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest); // the panels' own layouts first
    centralWidget()->layout()->activate();
    if (layout()) layout()->activate();
}

void MainWindow::phoneAdoptBackgroundJobs()
{
    if (!m_phone || !m_phoneTopBar || !m_backgroundJobs || m_backgroundJobs->compact()) return;
    m_backgroundJobs->setCompact(true);
    // After the title (and the live box), before undo/redo: the name gives up its room while something runs
    const bool hidden = m_backgroundJobs->isHidden(); // (shown only while there is work)
    m_backgroundJobs->setParent(m_phoneTopBar);
    m_backgroundJobs->setVisible(!hidden);
    phoneArrangeTopBar(m_phone->landscape());
    // A tap: what runs, in full (a phone has no tooltips)
    connect(m_backgroundJobs, &BackgroundJobs::detailsRequested, this,
            [this](const QString& text) { m_viewer->showNotice(text, 4000); });
}

void MainWindow::phoneTogglePanel(QWidget* panel, bool on)
{
    const QString id = panel == m_mediaPool   ? "media"
                       : panel == m_effects    ? "effects"
                       : panel == m_inspector  ? "inspector"
                       : panel == m_mixer      ? "mixer"
                       : panel == m_transcript ? "transcript"
                       : panel == m_chatPanel  ? "ai"
                                               : QString();
    if (id.isEmpty()) return; // the file browser has no place on the phone (the system's file picker instead)
    if (on) m_phone->openSheet(id);
    else if (m_phone->sheet() == id) m_phone->closeSheet();
}

void MainWindow::phoneShowPage(Page page)
{
    // The phone layout is the Edit page; the pages with a panel open it as the sheet
    switch (page) {
    case Page::Edit: m_phone->closeSheet(); break;
    case Page::Media: m_phone->openSheet("media"); break;
    case Page::Color: m_phone->openSheet("color"); break;
    case Page::Deliver: m_phone->openSheet("export"); break;
    case Page::Comp:
    case Page::Studio:
    case Page::Direct:
        m_viewer->showNotice(T("Diese Seite gibt es auf dem Handy noch nicht."), 3000);
        break;
    }
}
