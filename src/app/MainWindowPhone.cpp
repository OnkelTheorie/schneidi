// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the layout forms (ui/phone/PhoneShell.h). The same panels, actions and colors as on the desktop,
// rearranged for a phone or a tablet: the desktop's top bar, page bar, menu bar and the pages' splitters are hidden,
// the viewer and the timeline move into the shell, every panel opens in the shell's sheet (one at a time). Each widget
// the shell takes has its desktop place recorded (ShellHomes), so the desktop comes back exactly when the form
// changes live (a window resized past a threshold, a foldable unfolded, the setting).
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
#include "ui/MediaGallery.h"
#include "ui/MediaPool.h"
#include "core/TimelineOps.h"
#include "engine/DeviceMedia.h"
#include "ui/RenderQueuePanel.h"
#include "ui/SwitchCurtain.h"
#include "ui/Mixer.h"
#include "ui/TranscriptPanel.h"
#include "ui/Viewer.h"
#include "ui/phone/PhoneShell.h"
#include "ui/FrameView.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QFileDialog>
#include <QGridLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
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

// Where a widget the shell takes lives on the desktop: its parent and its place there (a splitter's index, a box
// layout's index, stretch and alignment, a grid cell, a scroll area, a stacked widget), its visibility; plus the
// desktop's splitter sizes and other state the shell changes
struct ShellHomes : QObject { // (a child of the window: goes with it)
    using QObject::QObject;
    struct Home {
        QPointer<QWidget> widget, parent;
        enum Kind { Plain, Splitter, Box, Grid, Scroll, Stack } kind = Plain;
        QPointer<QLayout> layout;
        int index = -1, stretch = 0, row = 0, column = 0, rowSpan = 1, columnSpan = 1;
        Qt::Alignment align;
        bool hidden = false;
    };
    QVector<Home> homes;
    QVector<QPair<QPointer<QSplitter>, QList<int>>> splitters;
    // The desktop's splitters at its last desktop-wide size (taken on resizes): the form switches only after the
    // window got narrower, which squeezed them already
    QVector<QPair<QPointer<QSplitter>, QList<int>>> desktopSplitters;
    QHash<QToolButton*, bool> toggles;
    MainWindow::Page page = MainWindow::Page::Edit;
    QSize viewerMin;
    int timelineMin = 0;
};

namespace {
// The layout of `parent` that holds `w` (directly or in a nested layout), with w's index there
QLayout* layoutHolding(QLayout* lay, QWidget* w, int* index)
{
    if (!lay) return nullptr;
    for (int i = 0; i < lay->count(); ++i) {
        QLayoutItem* item = lay->itemAt(i);
        if (item->widget() == w) {
            *index = i;
            return lay;
        }
        if (QLayout* inner = item->layout())
            if (QLayout* found = layoutHolding(inner, w, index)) return found;
    }
    return nullptr;
}
} // namespace

QWidget* MainWindow::shellHome(QWidget* w)
{
    if (!w || !m_shellHomes) return w;
    for (const ShellHomes::Home& h : m_shellHomes->homes)
        if (h.widget == w) return w; // known: its first place counts
    ShellHomes::Home h;
    h.widget = w;
    h.parent = w->parentWidget();
    h.hidden = w->isHidden();
    if (auto* split = qobject_cast<QSplitter*>(h.parent.data())) {
        h.kind = ShellHomes::Home::Splitter;
        h.index = split->indexOf(w);
    } else if (auto* stack = qobject_cast<QStackedWidget*>(h.parent.data())) {
        h.kind = ShellHomes::Home::Stack;
        h.index = stack->indexOf(w);
    } else if (h.parent && h.parent->parentWidget()
               && qobject_cast<QScrollArea*>(h.parent->parentWidget())
               && static_cast<QScrollArea*>(h.parent->parentWidget())->widget() == w) {
        h.kind = ShellHomes::Home::Scroll;
        h.parent = h.parent->parentWidget(); // the scroll area (the viewport holds it)
    } else if (h.parent) {
        int index = -1;
        if (QLayout* lay = layoutHolding(h.parent->layout(), w, &index)) {
            h.layout = lay;
            h.index = index;
            if (auto* grid = qobject_cast<QGridLayout*>(lay)) {
                h.kind = ShellHomes::Home::Grid;
                grid->getItemPosition(index, &h.row, &h.column, &h.rowSpan, &h.columnSpan);
                h.align = grid->itemAt(index)->alignment();
            } else if (auto* box = qobject_cast<QBoxLayout*>(lay)) {
                h.kind = ShellHomes::Home::Box;
                h.stretch = box->stretch(index);
                h.align = box->itemAt(index)->alignment();
            }
        }
    }
    m_shellHomes->homes << h;
    return w;
}

MainWindow::LayoutForm MainWindow::wantedForm(const QSize& logical, LayoutForm current)
{
    const auto named = [](const QString& v) -> std::optional<LayoutForm> {
        if (v == "phone" || v == "on" || v == "1") return LayoutForm::Phone;
        if (v == "tablet") return LayoutForm::Tablet;
        if (v == "desktop" || v == "off" || v == "0") return LayoutForm::Desktop;
        return std::nullopt;
    };
    if (auto f = named(qEnvironmentVariable("SCHNEIDI_LAYOUT"))) return *f;
    if (auto f = named(qEnvironmentVariable("SCHNEIDI_PHONE_LAYOUT"))) return *f;
    QSettings settings;
    if (auto f = named(settings.value("ui/layoutForm").toString())) return *f;
    if (auto f = named(settings.value("ui/phoneLayout").toString())) return *f; // (0.26 prototype's setting)
    if (logical.isEmpty()) return current;
    // Phone: the shorter side below 600 (a phone either way up) unless the window is desktop-wide (a short wide
    // desktop window stays a desktop). Desktop: 1100 wide and up. Between: the tablet form (an unfolded foldable,
    // a tablet upright, half a desktop screen). 24 px of hysteresis: a size on the line does not flip back and forth
    constexpr int kSlack = 24;
    const int w = logical.width(), shorter = std::min(logical.width(), logical.height());
    const int phoneEdge = kPhoneEdge + (current == LayoutForm::Phone ? kSlack : -kSlack) / 2;
    const int desktopWidth = kDesktopWidth + (current == LayoutForm::Desktop ? -kSlack : kSlack) / 2;
    if (shorter < phoneEdge && w < desktopWidth) return LayoutForm::Phone;
    return w >= desktopWidth ? LayoutForm::Desktop : LayoutForm::Tablet;
}

bool MainWindow::phoneLayoutWanted()
{
    // The start: the screen the window opens on (it is maximized there); later resizes check the window itself
    QScreen* screen = startScreen();
    const QSize size = screen ? screen->availableGeometry().size() : QSize();
    return wantedForm(size, LayoutForm::Desktop) == LayoutForm::Phone;
}

MainWindow::LayoutForm MainWindow::layoutForm() const
{
    return !m_phone ? LayoutForm::Desktop : m_phone->tablet() ? LayoutForm::Tablet : LayoutForm::Phone;
}

void MainWindow::enablePhoneLayout() { setLayoutForm(LayoutForm::Phone); }

void MainWindow::setLayoutForm(LayoutForm form)
{
    if (form == layoutForm()) return;
    // The switch under the window's frozen picture, which then fades (ui/SwitchCurtain)
    SwitchCurtain* curtain = SwitchCurtain::cover(this);
    if (form == LayoutForm::Desktop) {
        disableShell();
    } else if (m_phone) { // phone <-> tablet: the same shell, another arrangement
        m_phone->setTablet(form == LayoutForm::Tablet);
        if (form == LayoutForm::Tablet && m_phone->sheet().isEmpty()) m_phone->openSheet("inspector");
    } else {
        enableShell(form == LayoutForm::Tablet);
    }
    if (curtain) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest); // (the new arrangement laid out)
        curtain->reveal();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (!isVisible()) return; // (the constructor's sizes: the start decided already)
    snapshotDesktop();
    if (!m_formTimer) {
        m_formTimer = new QTimer(this);
        m_formTimer->setSingleShot(true);
        m_formTimer->setInterval(120); // a drag-resize settles first; a fold is one step anyway
        connect(m_formTimer, &QTimer::timeout, this, &MainWindow::checkLayoutForm);
    }
    m_formTimer->start();
}

void MainWindow::snapshotDesktop()
{
    // The desktop's splitters at a desktop width: what comes back after a tablet or phone form
    if (m_phone || width() < kDesktopWidth) return;
    if (!m_shellHomes) m_shellHomes = new ShellHomes(this);
    m_shellHomes->desktopSplitters.clear();
    for (QSplitter* split : centralWidget()->findChildren<QSplitter*>())
        m_shellHomes->desktopSplitters << qMakePair(QPointer<QSplitter>(split), split->sizes());
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    QTimer::singleShot(0, this, &MainWindow::snapshotDesktop); // (after the first layout)
}

void MainWindow::checkLayoutForm()
{
    const LayoutForm want = wantedForm(size(), layoutForm());
    if (want != layoutForm()) setLayoutForm(want);
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

void MainWindow::enableShell(bool tablet)
{
    if (m_phone) return;
    // What the desktop looks like now, to come back to it exactly
    if (!m_shellHomes) m_shellHomes = new ShellHomes(this);
    m_shellHomes->homes.clear();
    m_shellHomes->toggles.clear();
    m_shellHomes->splitters.clear();
    m_shellHomes->page = m_page;
    for (QSplitter* split : centralWidget()->findChildren<QSplitter*>()) {
        QList<int> sizes = split->sizes();
        if (width() < kDesktopWidth) // squeezed already: the sizes it had at a desktop width
            for (const auto& [known, wide] : m_shellHomes->desktopSplitters)
                if (known == split) sizes = wide;
        m_shellHomes->splitters << qMakePair(QPointer<QSplitter>(split), sizes);
    }
    for (QToolButton* t : {m_poolToggle, m_effectsToggle, m_inspectorToggle, m_mixerToggle, m_transcriptToggle, m_chatToggle})
        if (t) m_shellHomes->toggles.insert(t, t->isChecked());
    m_shellHomes->viewerMin = m_viewer->screen()->minimumSize();
    m_shellHomes->timelineMin = m_timeline->view()->minimumHeight();
    // The Edit page's state first (the viewer's overlay for shapes and painting): the phone layout is the Edit page
    if (m_page != Page::Edit) showPage(Page::Edit);
    setViewerLarge(false);
    shellHome(m_viewer);
    shellHome(m_timeline);
    shellHome(m_titleLabel);
    if (m_liveBox) shellHome(m_liveBox);
    if (m_backgroundJobs) shellHome(m_backgroundJobs);
    const bool starting = !isVisible(); // the constructor: start values (track heights) are the phone's
    m_phone = new PhoneShell;
    m_phone->setTablet(tablet);
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
    TimelineView::setCompactHeaders(true); // V1 + eye/mute; the rest in the header's menu (a tap)
    m_timeline->view()->setFingerPans(true); // one finger on empty lanes moves the view
    m_timeline->view()->setMinimumHeight(64);
    // Rows a finger still hits but a phone's height holds (landscape: V2, V1 and A1 under the ruler); a start value
    // like touch mode's, the user's pinch on the headers changes them as before
    if (starting) m_timeline->view()->setTrackHeights(56, 46); // (a live switch keeps the user's heights)
    m_mediaPool->setCompactTiles(true); // two columns beside the bins in portrait
    m_mediaPool->setGeneratorsLast(true); // the material first, the text and shape tiles after it
    m_mediaPool->setImportHandler([this] { openGallery(); }); // "Importieren…": the own gallery
    // An empty timeline: one big button for the first step
    m_addMediaButton = new QPushButton(T("+ Videos hinzufügen"), m_timeline->view());
    m_addMediaButton->setObjectName("addMediaButton");
    m_addMediaButton->setStyleSheet(Theme::primaryButtonStyle() + " QPushButton { padding: 14px 28px; }");
    QFont big = m_addMediaButton->font();
    big.setPointSizeF(big.pointSizeF() * 1.3);
    big.setBold(true);
    m_addMediaButton->setFont(big);
    connect(m_addMediaButton, &QPushButton::clicked, this, &MainWindow::openGallery);
    connect(m_project, &Project::timelineChanged, m_addMediaButton, [this] { updateAddMediaButton(); });
    m_timeline->view()->installEventFilter(this); // (its resizes: the button placed again, MainWindow::eventFilter)
    updateAddMediaButton();
    m_viewer->screen()->setMinimumSize(160, 72);
    m_viewer->setCompact(true);
    // The large viewer: nothing beside the viewer to hide (full screen stays)
    if (auto* large = m_viewer->findChild<QToolButton*>("viewAction_large")) large->hide();

    // The main tools: the desktop's panels and pages, one at a time in the sheet
    m_phone->addPanelTool("media", QStringLiteral("▦"), T("Medien"), [this] { return shellHome(m_mediaPool); });
    m_phone->addPanelTool("effects", QStringLiteral("✦"), T("Effekte"), [this] { return shellHome(m_effects); });
    m_phone->addActionTool("text", QStringLiteral("T"), T("Text"), [this] {
        trigger("add_title"); // at the playhead, selected: its fields in the Inspector
        m_phone->openSheet("inspector");
    }, false);
    m_phone->addPanelTool("mixer", QStringLiteral("▥"), T("Ton"), [this] { return shellHome(m_mixer); });
    m_phone->addPanelTool("color", QStringLiteral("◐"), T("Farbe"), [this]() -> QWidget* {
        ensureColorPage();
        return shellHome(m_colorPanel);
    });
    m_phone->addPanelTool("inspector", QStringLiteral("☰"), T("Inspektor"), [this] { return shellHome(m_inspector); });
    m_phone->addPanelTool("transcript", QStringLiteral("¶"), T("Transkript"), [this] { return shellHome(m_transcript); });
    m_phone->addPanelTool("ai", QStringLiteral("✧"), T("KI"), [this]() -> QWidget* { return shellHome(m_chatPanel); });
    m_phone->addPanelTool("export", QStringLiteral("⇪"), T("Export"), [this]() -> QWidget* {
        // The settings and, below them, the render queue (on the desktop it stands beside them): "Add to Render
        // Queue" alone rendered nothing on the phone, the queue's "Render all" was not on the screen
        auto* box = new QWidget;
        box->setObjectName("phoneExport");
        auto* lay = new QVBoxLayout(box);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(1);
        lay->addWidget(shellHome(m_deliver), 3);
        lay->addWidget(shellHome(m_deliver->queuePanel()), 2);
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
    if (tablet) m_phone->openSheet("inspector"); // the tablet's docked panel: the Inspector first
    m_phone->ensurePolished(); // (style sheets: the sizes they give)
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest); // the panels' own layouts first
    centralWidget()->layout()->activate();
    if (layout()) layout()->activate();
}

void MainWindow::disableShell()
{
    if (!m_phone || !m_shellHomes) return;
    setUpdatesEnabled(false);
    m_shellHomes->desktopSplitters.clear(); // (taken anew at desktop widths)
#ifdef Q_OS_ANDROID
    if (isFullScreen()) showMaximized(); // (the phone's landscape)
#endif
    m_phone->closeSheet();
    m_phone->releasePanels();
    if (m_backgroundJobs) {
        disconnect(m_backgroundJobs, &BackgroundJobs::detailsRequested, this, nullptr);
        m_backgroundJobs->setCompact(false);
    }
    // Every widget back to its desktop place, the last taken first (indices of a parent stay right)
    for (int i = int(m_shellHomes->homes.size()) - 1; i >= 0; --i) {
        const ShellHomes::Home& h = m_shellHomes->homes[i];
        QWidget* w = h.widget;
        if (!w || !h.parent) continue;
        switch (h.kind) {
        case ShellHomes::Home::Splitter: static_cast<QSplitter*>(h.parent.data())->insertWidget(h.index, w); break;
        case ShellHomes::Home::Stack: static_cast<QStackedWidget*>(h.parent.data())->insertWidget(h.index, w); break;
        case ShellHomes::Home::Scroll: static_cast<QScrollArea*>(h.parent.data())->setWidget(w); break;
        case ShellHomes::Home::Box:
            if (auto* box = qobject_cast<QBoxLayout*>(h.layout.data())) box->insertWidget(h.index, w, h.stretch, h.align);
            else w->setParent(h.parent);
            break;
        case ShellHomes::Home::Grid:
            if (auto* grid = qobject_cast<QGridLayout*>(h.layout.data()))
                grid->addWidget(w, h.row, h.column, h.rowSpan, h.columnSpan, h.align);
            else w->setParent(h.parent);
            break;
        case ShellHomes::Home::Plain: w->setParent(h.parent); break;
        }
        w->setVisible(!h.hidden);
    }
    // The desktop's own state
    m_timeline->setCompact(false);
    TimelineView::setCompactHeaders(false);
    m_timeline->view()->setFingerPans(false);
    m_timeline->view()->setMinimumHeight(m_shellHomes->timelineMin);
    m_mediaPool->setCompactTiles(false);
    m_mediaPool->setGeneratorsLast(false);
    m_mediaPool->setImportHandler(nullptr);
    m_timeline->view()->removeEventFilter(this);
    delete m_addMediaButton;
    m_addMediaButton = nullptr;
    m_viewer->screen()->setMinimumSize(m_shellHomes->viewerMin);
    m_viewer->setCompact(false);
    if (auto* large = m_viewer->findChild<QToolButton*>("viewAction_large")) large->show();
    delete m_phone; // (its own bar with it; every desktop widget is out)
    m_phone = nullptr;
    m_phoneTopBar = nullptr;
    m_topBar->show();
    m_pageBar->show();
    m_chatSplit->show();
    menuBar()->show();
    for (auto it = m_shellHomes->toggles.cbegin(); it != m_shellHomes->toggles.cend(); ++it) {
        const QSignalBlocker block(it.key());
        it.key()->setChecked(it.value());
    }
    // The splitters as they were, once the desktop's layout has its sizes again (set before, a splitter that comes
    // back narrower first keeps squeezed sizes); hidden pages' splitters never laid out (all 0) are left to their layout
    const auto splitters = m_shellHomes->splitters;
    const auto restoreSizes = [splitters] {
        for (const auto& [split, sizes] : splitters) {
            const bool laidOut = std::any_of(sizes.cbegin(), sizes.cend(), [](int v) { return v > 0; });
            if (split && laidOut) split->setSizes(sizes);
        }
    };
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    centralWidget()->layout()->activate();
    restoreSizes();
    QTimer::singleShot(0, this, [this, restoreSizes] {
        restoreSizes();
        snapshotDesktop(); // (the desktop as it is now: what the next switch brings back)
    });
    const Page page = m_shellHomes->page;
    m_shellHomes->homes.clear();
    if (page != Page::Edit) showPage(page);
    setUpdatesEnabled(true);
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

void MainWindow::updateAddMediaButton()
{
    if (!m_addMediaButton) return;
    const Timeline& tl = m_project->timeline();
    bool empty = true;
    for (const auto* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            if (!t.clips.isEmpty()) empty = false;
    m_addMediaButton->setVisible(empty);
    if (!empty) return;
    m_addMediaButton->adjustSize();
    QWidget* view = m_timeline->view();
    const int left = TimelineView::kHeaderW;
    m_addMediaButton->move(left + (view->width() - left - m_addMediaButton->width()) / 2,
                           std::max(TimelineView::kRulerH + 8, (view->height() - m_addMediaButton->height()) / 2));
    m_addMediaButton->raise();
}

void MainWindow::openGallery()
{
    if (DeviceMedia::hasPermission()) {
        MediaGallery gallery(this);
        if (m_phone) gallery.setWindowState(gallery.windowState() | Qt::WindowMaximized);
        else gallery.resize(std::min(width() - 40, 980), std::min(height() - 40, 760));
        const bool ok = gallery.exec() == QDialog::Accepted;
        if (ok) addPicked(gallery.chosenPaths());
        else if (gallery.otherFileWanted()) pickWithSystem();
        return;
    }
    // Android: the permission first, with the why; refused = the system's picker (it needs none)
    QMessageBox ask(this);
    ask.setWindowTitle(T("Videos hinzufügen"));
    ask.setText(T("schneidi zeigt dir hier deine Videos und Fotos zum Auswählen. Dafür fragt Android gleich, ob "
                  "schneidi sie lesen darf. Es wird nichts hochgeladen oder verändert."));
    QPushButton* go = ask.addButton(T("Weiter"), QMessageBox::AcceptRole);
    QPushButton* other = ask.addButton(T("Andere Datei…"), QMessageBox::RejectRole);
    ask.exec();
    if (ask.clickedButton() == other || ask.clickedButton() != go) {
        if (ask.clickedButton() == other) pickWithSystem();
        return;
    }
    DeviceMedia::requestPermission(this, [this](bool granted) {
        if (granted) QTimer::singleShot(0, this, &MainWindow::openGallery);
        else {
            m_viewer->showNotice(T("Ohne Erlaubnis öffnet sich die Dateiauswahl von Android."), 3500);
            QTimer::singleShot(0, this, &MainWindow::pickWithSystem);
        }
    });
}

void MainWindow::pickWithSystem()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, T("Videos hinzufügen"), QString(),
        T("Medien (*.mp4 *.mov *.mkv *.avi *.webm *.mts *.m4v *.mp3 *.wav *.flac *.ogg *.m4a *.aac "
          "*.png *.jpg *.jpeg *.webp *.bmp);;Alle Dateien (*)"));
    if (!files.isEmpty()) addPicked(files);
}

void MainWindow::addPicked(const QStringList& paths)
{
    if (paths.isEmpty()) return;
    // Into the pool and onto the timeline's end, in the order picked; an empty timeline takes the first clip's format
    const Timeline& tl = m_project->timeline();
    bool empty = true;
    for (const auto* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            if (!t.clips.isEmpty()) empty = false;
    importFiles(paths, true, empty);
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
