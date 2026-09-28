#include "app/MainWindow.h"

#include "app/InputBindings.h"
#include "app/KeyBindingsDialog.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/MediaCache.h"
#include "ui/DeliverPanel.h"
#include "ui/Inspector.h"
#include "ui/Mixer.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
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
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
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
    connect(m_project, &Project::timelineChanged, this, [this] {
        m_engine->updateTimeline(m_project->timeline());
    });
    m_engine->updateTimeline(m_project->timeline());

    // Engine -> Playhead (nur im Timeline-Modus; im Quellmodus bleibt der Playhead stehen)
    TimelineView* tv = m_timeline->view();
    connect(m_engine, &Engine::positionChanged, this, [this, tv](int f) {
        if (m_engine->mode() == Engine::Mode::Timeline) tv->setPlayhead(f);
    });
    connect(tv, &TimelineView::seekRequested, this, [this](int f) {
        if (m_engine->mode() != Engine::Mode::Timeline) m_engine->showTimeline(f);
        else m_engine->seek(f);
    });
    connect(m_mediaPool, &MediaPool::sourceRequested, m_engine, &Engine::showSource);
    connect(tv, &TimelineView::dropRequested, this, &MainWindow::onDrop);
    tv->setProbe([this](const QString& path) { return probeCached(path); });
    tv->setMediaCache(new MediaCache(this));

    connect(m_project, &Project::modifiedChanged, this, &MainWindow::updateTitle);
    updateTitle();
    // Automatische Sicherung jede Minute (nur bei Änderungen), getrennt von der Projektdatei
    m_autosaveTimer = new QTimer(this);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
    m_autosaveTimer->start(60 * 1000);

}

void MainWindow::importFiles(const QStringList& paths, bool placeOnTimeline)
{
    m_mediaPool->importFiles(paths);
    if (!placeOnTimeline) return;
    // Testhilfe (--demo): Dateien hintereinander auf V1/A1 legen
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
    // Direkt aus dem Dateimanager: erst in den Media Pool, dann auf die Timeline (wie DaVinci)
    QStringList unknown;
    for (const QString& p : paths)
        if (!m_project->mediaInfo(p)) unknown << p;
    if (!unknown.isEmpty()) m_mediaPool->importFiles(unknown);
    m_editor->addMediaAt(paths, frame, track);
}

void MainWindow::buildLayout()
{
    m_mediaPool = new MediaPool(m_project, m_engine);
    m_viewer = new Viewer(m_engine);
    m_inspector = new Inspector(m_editor);
    m_inspector->setFrameSize(m_engine->frameSize());
    m_timeline = new TimelinePanel(m_editor);
    m_deliver = new DeliverPanel(m_project);
    m_mixer = new Mixer(m_project, m_engine);
    m_mixer->hide();

    // Seiten; die gemeinsamen Panels (Pool, Viewer, Timeline) wandern beim Umschalten mit
    m_editTop = new QSplitter(Qt::Horizontal);
    m_editMain = new QSplitter(Qt::Vertical);
    m_editMain->addWidget(m_editTop);
    m_editBottom = new QSplitter(Qt::Horizontal); // Mixer rechts neben der Timeline wie in DaVinci
    m_editBottom->addWidget(m_mixer);
    m_editBottom->setStretchFactor(0, 1);
    m_editMain->addWidget(m_editBottom);
    m_mediaPage = new QSplitter(Qt::Horizontal);
    m_deliverPage = new QSplitter(Qt::Horizontal);
    m_deliverRight = new QSplitter(Qt::Vertical);
    m_deliverPage->addWidget(m_deliver);
    m_deliverPage->addWidget(m_deliverRight);

    m_pages = new QStackedWidget;
    m_pages->addWidget(m_mediaPage);
    m_pages->addWidget(m_editMain);
    m_pages->addWidget(m_deliverPage);

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
    // Obere Leiste wie in DaVinci: Panels links/rechts ein- und ausblenden, Projektname mittig
    auto makeToggle = [](const QString& text) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setCheckable(true);
        b->setChecked(true);
        return b;
    };
    m_poolToggle = makeToggle("▦ Media Pool");
    m_inspectorToggle = makeToggle("☰ Inspector");
    connect(m_poolToggle, &QToolButton::toggled, m_mediaPool, &QWidget::setVisible);
    connect(m_inspectorToggle, &QToolButton::toggled, m_inspector, &QWidget::setVisible);
    m_mixerToggle = makeToggle("▥ Mixer");
    m_mixerToggle->setChecked(false);
    connect(m_mixerToggle, &QToolButton::toggled, m_mixer, &QWidget::setVisible);

    auto* title = m_titleLabel = new QLabel;
    title->setStyleSheet("font-weight: 600;");
    title->setAlignment(Qt::AlignCenter);

    auto* bar = new QWidget;
    bar->setObjectName("TopBar");
    bar->setStyleSheet("QWidget#TopBar { background: #2a2a30; border-bottom: 1px solid #141417; }");
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(6, 2, 6, 2);
    lay->addWidget(m_poolToggle);
    lay->addStretch(1);
    lay->addWidget(title);
    lay->addStretch(1);
    lay->addWidget(m_mixerToggle);
    lay->addWidget(m_inspectorToggle);
    return bar;
}

QWidget* MainWindow::buildPageBar()
{
    // Seitenleiste unten wie in DaVinci (Media | Edit | Deliver)
    auto* bar = new QWidget;
    bar->setObjectName("PageBar");
    bar->setStyleSheet(
        "QWidget#PageBar { background: #1f1f23; border-top: 1px solid #141417; }"
        "QToolButton { color: #8c8c94; padding: 4px 18px; border-radius: 0; }"
        "QToolButton:checked { color: #e87a3a; background: transparent; border-bottom: 2px solid #e87a3a; }"
        "QToolButton:hover { color: #d2d2d6; }");
    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(4);

    auto* brand = new QLabel("schneidi");
    brand->setStyleSheet("color: #8c8c94; font-weight: 600;");
    lay->addWidget(brand);
    lay->addStretch(1);

    m_pageButtons = new QButtonGroup(this);
    const struct { const char* icon; const char* name; Page page; } pages[] = {
        {"▤", "Media", Page::Media},
        {"✂", "Edit", Page::Edit},
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
        m_mediaPage->insertWidget(0, m_mediaPool);
        m_mediaPage->insertWidget(1, m_viewer);
        m_mediaPage->setSizes({900, 700});
        m_mediaPool->setVisible(true);
        m_pages->setCurrentWidget(m_mediaPage);
        break;
    case Page::Edit:
        m_editTop->insertWidget(0, m_mediaPool);
        m_editTop->insertWidget(1, m_viewer);
        m_editTop->insertWidget(2, m_inspector);
        m_editBottom->insertWidget(0, m_timeline);
        m_editBottom->setStretchFactor(0, 1);
        m_editBottom->setSizes({1300, 300});
        m_editTop->setStretchFactor(1, 1);
        m_editTop->setSizes({360, 880, 360});
        m_editMain->setSizes({480, 420});
        m_mediaPool->setVisible(m_poolToggle->isChecked());
        m_pages->setCurrentWidget(m_editMain);
        break;
    case Page::Deliver:
        m_deliverRight->insertWidget(0, m_viewer);
        m_deliverRight->insertWidget(1, m_timeline);
        m_deliverRight->setSizes({480, 380});
        m_deliverPage->setStretchFactor(1, 1);
        m_deliverPage->setSizes({340, 1260});
        m_pages->setCurrentWidget(m_deliverPage);
        break;
    }
    if (auto* b = m_pageButtons->button(int(page))) b->setChecked(true);
    const bool edit = page == Page::Edit;
    m_poolToggle->setEnabled(edit);
    m_inspectorToggle->setEnabled(edit);
    m_mixerToggle->setEnabled(edit);
}

QAction* MainWindow::makeAction(QMenu* menu, const QString& id, const QString& text, const QKeySequence& key,
                               const std::function<void()>& fn)
{
    auto* a = new QAction(text, this);
    InputBindings::instance().registerAction(a, id, menu->title().remove('&'), key);
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, fn);
    menu->addAction(a);
    return a;
}

void MainWindow::buildActions()
{
    TimelineView* tv = m_timeline->view();
    QUndoStack* undo = m_project->undoStack();

    // Belegung nach DaVinci Resolve (Defaults), überschreibbar in keybindings.json
    QMenu* file = menuBar()->addMenu("&Datei");
    makeAction(file, "project_new", "Neues Projekt", QKeySequence("Ctrl+N"), [this] { newProject(); });
    makeAction(file, "project_open", "Projekt öffnen…", QKeySequence("Ctrl+O"), [this] { openProjectDialog(); });
    m_recentMenu = file->addMenu("Zuletzt geöffnet");
    rebuildRecentMenu();
    makeAction(file, "project_save", "Projekt speichern", QKeySequence("Ctrl+S"), [this] { save(); });
    makeAction(file, "project_save_as", "Projekt speichern unter…", QKeySequence("Ctrl+Shift+S"), [this] { saveAs(); });
    file->addSeparator();
    makeAction(file, "import", "Medien importieren…", QKeySequence("Ctrl+I"), [this] { m_mediaPool->importDialog(); });
    file->addSeparator();
    makeAction(file, "quit", "Beenden", QKeySequence("Ctrl+Q"), [this] { close(); });

    QMenu* edit = menuBar()->addMenu("&Bearbeiten");
    QAction* undoAct = makeAction(edit, "undo", "Rückgängig", QKeySequence("Ctrl+Z"), [undo] { undo->undo(); });
    QAction* redoAct = makeAction(edit, "redo", "Wiederholen", QKeySequence("Ctrl+Shift+Z"), [undo] { undo->redo(); });
    connect(undo, &QUndoStack::canUndoChanged, undoAct, &QAction::setEnabled);
    connect(undo, &QUndoStack::canRedoChanged, redoAct, &QAction::setEnabled);
    undoAct->setEnabled(false);
    redoAct->setEnabled(false);
    edit->addSeparator();
    makeAction(edit, "cut", "Ausschneiden", QKeySequence("Ctrl+X"), [this] { m_editor->cutSelection(); });
    makeAction(edit, "copy", "Kopieren", QKeySequence("Ctrl+C"), [this] { m_editor->copySelection(); });
    makeAction(edit, "paste", "Einfügen am Playhead", QKeySequence("Ctrl+V"), [this, tv] { m_editor->paste(tv->playhead()); });
    edit->addSeparator();
    makeAction(edit, "delete", "Löschen (Lücke bleibt)", QKeySequence(Qt::Key_Backspace), [this] { m_editor->deleteSelection(); });
    makeAction(edit, "delete_alt", "Löschen (Entf)", QKeySequence(Qt::Key_Delete), [this] { m_editor->deleteSelection(); });
    makeAction(edit, "ripple_delete", "Löschen mit Ripple", QKeySequence("Shift+Del"), [this] { m_editor->rippleDeleteSelection(); });
    makeAction(edit, "ripple_delete_alt", "Löschen mit Ripple (Rücktaste)", QKeySequence("Shift+Backspace"),
               [this] { m_editor->rippleDeleteSelection(); });
    edit->addSeparator();
    makeAction(edit, "select_all", "Alles auswählen", QKeySequence("Ctrl+A"), [this] { m_editor->selectAll(); });
    makeAction(edit, "deselect", "Auswahl aufheben", QKeySequence("Ctrl+Shift+A"), [this] { m_selection->clear(); });
    edit->addSeparator();
    makeAction(edit, "edit_keybindings", "Tastenbelegung…", QKeySequence("Ctrl+Alt+K"), [this] {
        KeyBindingsDialog dlg(this);
        dlg.exec();
    });

    QMenu* timeline = menuBar()->addMenu("&Timeline");
    makeAction(timeline, "tool_select", "Auswahl-Werkzeug", QKeySequence("A"), [tv] { tv->setTool(TimelineView::Tool::Select); });
    makeAction(timeline, "tool_blade", "Klingen-Werkzeug", QKeySequence("B"), [tv] { tv->setTool(TimelineView::Tool::Blade); });
    makeAction(timeline, "snapping", "Snapping an/aus", QKeySequence("N"), [tv] { tv->setSnapping(!tv->snapping()); });
    timeline->addSeparator();
    makeAction(timeline, "split", "Clip am Playhead teilen", QKeySequence("Ctrl+B"),
               [this, tv] { m_editor->splitAtPlayhead(tv->playhead()); });
    makeAction(timeline, "split_alt", "Clip teilen (alternativ)", QKeySequence("Ctrl+\\"),
               [this, tv] { m_editor->splitAtPlayhead(tv->playhead()); });
    makeAction(timeline, "trim_start", "Anfang bis Playhead trimmen", QKeySequence("Shift+["),
               [this, tv] { m_editor->trimToPlayhead(TimelineOps::Edge::Start, tv->playhead()); });
    makeAction(timeline, "trim_end", "Ende bis Playhead trimmen", QKeySequence("Shift+]"),
               [this, tv] { m_editor->trimToPlayhead(TimelineOps::Edge::End, tv->playhead()); });
    makeAction(timeline, "nudge_left", "1 Frame nach links schieben", QKeySequence(","), [this] { m_editor->nudgeSelection(-1); });
    makeAction(timeline, "nudge_right", "1 Frame nach rechts schieben", QKeySequence("."), [this] { m_editor->nudgeSelection(1); });
    makeAction(timeline, "nudge_left_multi", "5 Frames nach links schieben", QKeySequence("Shift+,"),
               [this] { m_editor->nudgeSelection(-5); });
    makeAction(timeline, "nudge_right_multi", "5 Frames nach rechts schieben", QKeySequence("Shift+."),
               [this] { m_editor->nudgeSelection(5); });
    makeAction(timeline, "toggle_enabled", "Clip aktivieren/deaktivieren", QKeySequence("D"),
               [this] { m_editor->toggleSelectionEnabled(); });
    makeAction(timeline, "link_clips", "Clips verknüpfen/trennen", QKeySequence("Ctrl+Alt+L"),
               [this] { m_editor->toggleLinkSelection(); });
    timeline->addSeparator();
    makeAction(timeline, "add_marker", "Marker setzen/entfernen", QKeySequence("M"),
               [this, tv] { m_editor->toggleMarker(tv->playhead()); });
    makeAction(timeline, "mark_in", "In-Punkt setzen", QKeySequence("I"), [this, tv] { m_editor->setMarkIn(tv->playhead()); });
    makeAction(timeline, "mark_out", "Out-Punkt setzen", QKeySequence("O"), [this, tv] { m_editor->setMarkOut(tv->playhead()); });
    makeAction(timeline, "clear_in", "In-Punkt entfernen", QKeySequence("Alt+I"), [this] { m_editor->setMarkIn(-1); });
    makeAction(timeline, "clear_out", "Out-Punkt entfernen", QKeySequence("Alt+O"), [this] { m_editor->setMarkOut(-1); });
    makeAction(timeline, "clear_in_out", "In und Out entfernen", QKeySequence("Alt+X"), [this] { m_editor->clearMarks(); });
    makeAction(timeline, "goto_in", "Zum In-Punkt", QKeySequence("Shift+I"), [this] { jumpToFrame(m_project->timeline().markIn); });
    makeAction(timeline, "goto_out", "Zum Out-Punkt", QKeySequence("Shift+O"), [this] { jumpToFrame(m_project->timeline().markOut); });
    makeAction(timeline, "marker_prev", "Vorheriger Marker", QKeySequence("Shift+Up"), [this] { jumpToMarker(-1); });
    makeAction(timeline, "marker_next", "Nächster Marker", QKeySequence("Shift+Down"), [this] { jumpToMarker(1); });
    auto* linked = makeAction(timeline, "linked_selection", "Verknüpfte Auswahl", QKeySequence("Ctrl+Shift+L"), [] {});
    linked->setCheckable(true);
    linked->setChecked(m_editor->linkedSelection());
    connect(linked, &QAction::toggled, this, [this](bool on) { m_editor->setLinkedSelection(on); });
    auto* splitTracks = makeAction(timeline, "split_on_tracks", "Teilen auf ganzer Spur der Auswahl", QKeySequence(), [] {});
    splitTracks->setCheckable(true);
    splitTracks->setToolTip("An: Strg+B/Maustaste teilt auch den Nachbarclip auf der Spur des ausgewählten Clips. "
                            "Aus: nur ausgewählte Clips.");
    splitTracks->setChecked(QSettings().value("edit/splitOnSelectedTracks", true).toBool());
    m_editor->setSplitOnSelectedTracks(splitTracks->isChecked());
    connect(splitTracks, &QAction::toggled, this, [this](bool on) {
        m_editor->setSplitOnSelectedTracks(on);
        QSettings().setValue("edit/splitOnSelectedTracks", on);
    });
    timeline->addSeparator();
    makeAction(timeline, "zoom_in", "Hineinzoomen", QKeySequence("Ctrl+="), [tv] { tv->zoomBy(1.5); });
    makeAction(timeline, "zoom_out", "Herauszoomen", QKeySequence("Ctrl+-"), [tv] { tv->zoomBy(1 / 1.5); });
    makeAction(timeline, "zoom_fit", "Ganze Timeline zeigen", QKeySequence("Shift+Z"), [tv] { tv->zoomToFit(); });

    QMenu* play = menuBar()->addMenu("&Wiedergabe");
    makeAction(play, "play_pause", "Play/Pause", QKeySequence(Qt::Key_Space), [this] { m_engine->togglePlay(); });
    makeAction(play, "shuttle_back", "Rückwärts (J)", QKeySequence("J"), [this] { shuttle(-1); });
    makeAction(play, "stop", "Stopp (K)", QKeySequence("K"), [this] { m_engine->pause(); });
    makeAction(play, "shuttle_fwd", "Vorwärts (L)", QKeySequence("L"), [this] { shuttle(1); });
    play->addSeparator();
    makeAction(play, "frame_prev", "1 Frame zurück", QKeySequence(Qt::Key_Left), [this] { stepFrames(-1); });
    makeAction(play, "frame_next", "1 Frame vor", QKeySequence(Qt::Key_Right), [this] { stepFrames(1); });
    makeAction(play, "second_prev", "1 Sekunde zurück", QKeySequence("Shift+Left"), [this] { stepFrames(-m_project->fps()); });
    makeAction(play, "second_next", "1 Sekunde vor", QKeySequence("Shift+Right"), [this] { stepFrames(m_project->fps()); });
    makeAction(play, "edit_prev", "Vorheriger Schnittpunkt", QKeySequence(Qt::Key_Up), [this] { jumpToEdit(-1); });
    makeAction(play, "edit_next", "Nächster Schnittpunkt", QKeySequence(Qt::Key_Down), [this] { jumpToEdit(1); });
    makeAction(play, "go_start", "Zum Anfang", QKeySequence(Qt::Key_Home), [this] { m_engine->pause(); m_engine->seek(0); });
    makeAction(play, "go_end", "Zum Ende", QKeySequence(Qt::Key_End), [this] {
        m_engine->pause();
        m_engine->seek(TimelineOps::endFrame(m_project->timeline()));
    });
    makeAction(play, "show_timeline", "Timeline im Viewer zeigen", QKeySequence("Q"),
              [this, tv] { m_engine->showTimeline(tv->playhead()); });

    QMenu* workspace = menuBar()->addMenu("&Arbeitsbereich");
    makeAction(workspace, "page_media", "Media-Seite", QKeySequence("Shift+2"), [this] { showPage(Page::Media); });
    makeAction(workspace, "page_edit", "Edit-Seite", QKeySequence("Shift+4"), [this] { showPage(Page::Edit); });
    makeAction(workspace, "page_deliver", "Deliver-Seite", QKeySequence("Shift+8"), [this] { showPage(Page::Deliver); });
    makeAction(workspace, "toggle_mixer", "Mixer ein/aus", QKeySequence(),
               [this] { if (m_mixerToggle->isEnabled()) m_mixerToggle->toggle(); });

    InputBindings::instance().saveIfIncomplete();
}

// ---- Projektdatei ---------------------------------------------------------------

namespace {
const QString kFileFilter = QString("schneidi-Projekt (*.%1)").arg(ProjectFile::Extension);
}

QString MainWindow::autosavePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dir).filePath(QString("autosave.%1").arg(ProjectFile::Extension));
}

void MainWindow::updateTitle()
{
    const QString name = m_projectPath.isEmpty() ? "Unbenannt" : QFileInfo(m_projectPath).completeBaseName();
    const QString shown = name + (m_project->isModified() ? " *" : "");
    setWindowTitle(shown + " – schneidi");
    if (m_titleLabel) m_titleLabel->setText(shown);
}

void MainWindow::setProjectPath(const QString& path)
{
    m_projectPath = path;
    if (!path.isEmpty()) addRecent(path);
    updateTitle();
}

bool MainWindow::maybeSave()
{
    if (!m_project->isModified()) return true;
    const auto answer = QMessageBox::question(this, "schneidi", "Änderungen am Projekt speichern?",
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Save);
    if (answer == QMessageBox::Save) return save();
    return answer == QMessageBox::Discard;
}

void MainWindow::newProject()
{
    if (!maybeSave()) return;
    m_engine->pause();
    m_selection->clear();
    m_project->reset();
    m_engine->showTimeline(0);
    setProjectPath({});
    removeAutosave();
}

void MainWindow::openProjectDialog()
{
    if (!maybeSave()) return;
    const QString dir = m_projectPath.isEmpty() ? QDir::homePath() : QFileInfo(m_projectPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, "Projekt öffnen", dir, kFileFilter);
    if (!path.isEmpty()) openProject(path);
}

bool MainWindow::openProject(const QString& path)
{
    ProjectData data;
    QString error;
    if (!ProjectFile::load(path, &data, &error)) {
        QMessageBox::warning(this, "Projekt öffnen", QString("%1 konnte nicht geöffnet werden:\n%2").arg(path, error));
        return false;
    }
    if (!applyLoaded(std::move(data), path)) return false;
    removeAutosave();
    return true;
}

bool MainWindow::applyLoaded(ProjectData data, const QString& path)
{
    // Fehlende Medien wie DaVinci "Media Offline": Ordner durchsuchen lassen oder offline lassen
    for (QStringList missing = ProjectFile::missingMedia(data); !missing.isEmpty();
         missing = ProjectFile::missingMedia(data)) {
        QMessageBox box(QMessageBox::Warning, "Medien fehlen",
                        QString("%1 Datei(en) nicht gefunden (verschoben oder umbenannt?):").arg(missing.size()),
                        QMessageBox::NoButton, this);
        QStringList names;
        for (const QString& p : missing.mid(0, 15)) names << p;
        if (missing.size() > 15) names << "…";
        box.setInformativeText(names.join('\n'));
        QPushButton* search = box.addButton("Ordner durchsuchen…", QMessageBox::AcceptRole);
        box.addButton("Offline lassen", QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != search) break;
        const QString dir = QFileDialog::getExistingDirectory(this, "Ordner mit den Medien wählen",
                                                              QFileInfo(path).absolutePath());
        if (dir.isEmpty()) break;
        if (ProjectFile::relink(&data, dir) == 0)
            QMessageBox::information(this, "Medien fehlen", "In diesem Ordner wurden keine der Dateien gefunden.");
    }

    m_engine->pause();
    m_selection->clear();
    m_probeCache.clear();
    m_project->load(data);
    m_engine->showTimeline(data.playhead);
    m_timeline->view()->setPlayhead(data.playhead);
    setProjectPath(path);
    return true;
}

bool MainWindow::save()
{
    return m_projectPath.isEmpty() ? saveAs() : saveTo(m_projectPath);
}

bool MainWindow::saveAs()
{
    QString path = QFileDialog::getSaveFileName(
        this, "Projekt speichern unter",
        m_projectPath.isEmpty() ? QDir::home().filePath(QString("Unbenannt.%1").arg(ProjectFile::Extension)) : m_projectPath,
        kFileFilter);
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix() != ProjectFile::Extension) path += QString(".%1").arg(ProjectFile::Extension);
    return saveTo(path);
}

bool MainWindow::saveTo(const QString& path)
{
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    QString error;
    if (!ProjectFile::save(data, path, &error)) {
        QMessageBox::warning(this, "Projekt speichern", QString("Speichern fehlgeschlagen:\n%1").arg(error));
        return false;
    }
    m_project->markSaved();
    setProjectPath(path);
    removeAutosave();
    return true;
}

void MainWindow::autosave()
{
    if (!m_project->isModified()) return;
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    QDir().mkpath(QFileInfo(autosavePath()).absolutePath());
    if (ProjectFile::save(data, autosavePath(), nullptr))
        QSettings().setValue("autosave/projectPath", m_projectPath); // wohin die Sicherung gehört
}

void MainWindow::disableAutosave()
{
    m_autosaveTimer->stop();
    m_autosaveDisabled = true;
}

void MainWindow::removeAutosave()
{
    if (m_autosaveDisabled) return; // Testlauf: echte Sicherung des Nutzers nicht anfassen

    QFile::remove(autosavePath());
    QSettings().remove("autosave/projectPath");
}

void MainWindow::offerAutosaveRestore()
{
    if (!QFileInfo::exists(autosavePath())) return;
    const QString original = QSettings().value("autosave/projectPath").toString();
    const QString name = original.isEmpty() ? "Unbenannt" : QFileInfo(original).fileName();
    const auto answer = QMessageBox::question(
        this, "Wiederherstellen",
        QString("schneidi wurde nicht normal beendet.\nNicht gespeicherte Änderungen von „%1“ wiederherstellen?")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        removeAutosave();
        return;
    }
    ProjectData data;
    if (!ProjectFile::load(autosavePath(), &data, nullptr)) return;
    applyLoaded(std::move(data), original);
    m_project->markModified(); // wiederhergestellt, aber noch nicht in die Projektdatei gespeichert
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
            if (maybeSave()) openProject(path);
        });
        a->setToolTip(path);
        a->setEnabled(QFileInfo::exists(path));
    }
    m_recentMenu->setEnabled(!recent.isEmpty());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    removeAutosave(); // normal beendet -> nichts wiederherzustellen
    event->accept();
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
