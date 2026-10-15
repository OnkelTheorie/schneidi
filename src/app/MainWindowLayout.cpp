// MainWindow: layout, top bar, page bar and pages.
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
#include "ui/TranscriptPanel.h"
#include "ui/ScopesPanel.h"
#include "ui/Inspector.h"
#include "ui/Mixer.h"
#include "ui/MediaPool.h"
#include "ui/MulticamView.h"
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
#include <QFile>
#include <QLockFile>
#include <QSaveFile>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QInputDialog>
#include <QStandardPaths>
#include <QTimer>

void MainWindow::buildLayout()
{
    m_mediaPool = new MediaPool(m_project, m_engine);
    m_effects = new EffectsLibrary;
    m_storage = new MediaStorage;
    m_viewer = new Viewer(m_engine);
    m_multicam = new MulticamView(m_project, m_selection, m_editor);
    m_viewer->setMulticamView(m_multicam);
    m_inspector = new Inspector(m_editor);
    m_inspector->setFrameSize(m_project->format().size());
    m_timeline = new TimelinePanel(m_editor);
    m_deliver = new DeliverPanel(m_project);
    m_mixer = new Mixer(m_project, m_engine);
    m_mixer->setEditor(m_editor);
    m_mixer->hide();
    m_colorPanel = new ColorPanel(m_editor);
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
    m_colorBottom->setSizes({1100, 420});
    m_colorPage->addWidget(m_colorBottom);
    m_pages->addWidget(m_colorPage);

    auto* root = new QWidget;
    root->setObjectName("Root");
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(buildTopBar());
    lay->addWidget(m_pages, 1);
    lay->addWidget(buildPageBar());
    setCentralWidget(root);

    showPage(Page::Edit);
}

QWidget* MainWindow::buildTopBar()
{
    // Top bar like in DaVinci: show/hide panels left/right, project name in the middle
    // Visibility is saved (panels/<key>); test runs do not change the setting
    auto makeToggle = [this](const QString& text, const QString& key, bool def, QWidget* panel) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setCheckable(true);
        b->setChecked(QSettings().value("panels/" + key, def).toBool());
        // Only hide panels not yet inserted (show() without a parent opened its own window)
        if (panel->parentWidget() || !b->isChecked()) panel->setVisible(b->isChecked());
        connect(b, &QToolButton::toggled, this, [this, key, panel](bool on) {
            panel->setVisible(on);
            updateLeftColumn();
            if (!m_autosaveDisabled) QSettings().setValue("panels/" + key, on);
        });
        return b;
    };
    m_poolToggle = makeToggle("▦ Media Pool", "mediaPool", true, m_mediaPool);
    m_effectsToggle = makeToggle("✦ Effects", "effects", false, m_effects);
    m_storageToggle = makeToggle("▤ Media Storage", "mediaStorage", false, m_storage);
    m_storageToggle->setToolTip(T("Ordner durchsuchen und Clips direkt in Media Pool/Timeline ziehen"));
    m_inspectorToggle = makeToggle("☰ Inspector", "inspector", true, m_inspector);
    m_mixerToggle = makeToggle("▥ Mixer", "mixer", false, m_mixer);
    m_transcriptToggle = makeToggle("¶ " + T("Transkript"), "transcript", false, m_transcript);
    m_transcriptToggle->setToolTip(T("Gesprochenen Text der Tonclips zeigen; Wörter löschen schneidet die Timeline"));

    auto* title = m_titleLabel = new QLabel;
    title->setStyleSheet("font-weight: 600;");
    title->setAlignment(Qt::AlignCenter);

    auto* bar = new QWidget;
    bar->setObjectName("TopBar");
    bar->setStyleSheet(QString("QWidget#TopBar { background: %1; border-bottom: 1px solid %2; }")
                           .arg(Theme::topBar.name(), Theme::border.name()));
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(6, 2, 6, 2);
    lay->addWidget(m_storageToggle);
    lay->addWidget(m_poolToggle);
    lay->addWidget(m_effectsToggle);
    lay->addStretch(1);
    lay->addWidget(title);
    lay->addWidget(buildLiveIndicator());
    lay->addStretch(1);
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

    auto* brand = new QLabel("schneidi");
    brand->setStyleSheet(QString("color: %1; font-weight: 600;").arg(Theme::textDim.name()));
    lay->addWidget(brand);
    lay->addStretch(1);

    m_pageButtons = new QButtonGroup(this);
    const struct { const char* icon; const char* name; Page page; } pages[] = {
        {"▤", "Media", Page::Media},
        {"✂", "Edit", Page::Edit},
        {"◐", "Color", Page::Color},
        {"⇪", "Deliver", Page::Deliver},
    };
    for (const auto& pg : pages) {
        auto* b = new QToolButton;
        b->setText(QString("%1\n%2").arg(pg.icon, pg.name));
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setCheckable(true);
        b->setChecked(pg.page == m_page);
        const Page page = pg.page;
        connect(b, &QToolButton::clicked, this, [this, page] { showPage(page); });
        m_pageButtons->addButton(b, int(pg.page));
        lay->addWidget(b);
    }
    lay->addStretch(1);
    lay->addSpacing(brand->sizeHint().width());
    return bar;
}

void MainWindow::showPage(Page page)
{
    m_page = page;
    switch (page) {
    case Page::Media:
        m_mediaTop->insertWidget(0, m_storage);
        m_mediaTop->insertWidget(1, m_viewer);
        m_mediaTop->setStretchFactor(1, 1);
        m_mediaTop->setSizes({700, 900});
        m_mediaPage->insertWidget(1, m_mediaPool);
        m_mediaPage->setSizes({520, 380});
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
        m_editBottom->setSizes({1300, 300});
        m_editTop->setStretchFactor(1, 1);
        m_editTop->setSizes({360, 880, 320, 360});
        m_editMain->setSizes({480, 420});
        m_mediaPool->setVisible(m_poolToggle->isChecked());
        m_storage->setVisible(m_storageToggle->isChecked());
        updateLeftColumn();
        m_pages->setCurrentWidget(m_editMain);
        break;
    case Page::Deliver:
        m_deliverRight->insertWidget(0, m_viewer);
        m_deliverRight->insertWidget(1, m_timeline);
        m_deliverRight->setSizes({480, 380});
        m_deliverPage->setStretchFactor(1, 1);
        m_deliverPage->setSizes({340, 940, 320});
        m_pages->setCurrentWidget(m_deliverPage);
        break;
    case Page::Color:
        m_colorPage->insertWidget(0, m_viewer);
        m_colorPage->insertWidget(1, m_timeline);
        m_colorPage->setStretchFactor(0, 1);
        m_colorPage->setSizes({460, 200, 300});
        m_pages->setCurrentWidget(m_colorPage);
        break;
    }
    if (auto* b = m_pageButtons->button(int(page))) b->setChecked(true);
    const bool edit = page == Page::Edit;
    m_poolToggle->setEnabled(edit);
    m_effectsToggle->setEnabled(edit);
    m_storageToggle->setEnabled(edit);
    m_inspectorToggle->setEnabled(edit);
    m_mixerToggle->setEnabled(edit);
    m_transcriptToggle->setEnabled(edit);
}

void MainWindow::updateLeftColumn()
{
    if (m_page == Page::Edit) m_editLeft->setVisible(m_poolToggle->isChecked() || m_effectsToggle->isChecked()
                                                  || m_storageToggle->isChecked());
}
