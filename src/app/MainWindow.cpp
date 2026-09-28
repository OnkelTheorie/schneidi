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

    // Seiten; die gemeinsamen Panels (Pool, Viewer, Timeline) wandern beim Umschalten mit
    m_editTop = new QSplitter(Qt::Horizontal);
    m_editMain = new QSplitter(Qt::Vertical);
    m_editMain->addWidget(m_editTop);
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

    auto* title = new QLabel("Unbenanntes Projekt");
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
        m_editMain->insertWidget(1, m_timeline);
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
    makeAction(timeline, "marker_prev", "Vorheriger Marker", QKeySequence("Shift+Up"), [this] { jumpToMarker(-1); });
    makeAction(timeline, "marker_next", "Nächster Marker", QKeySequence("Shift+Down"), [this] { jumpToMarker(1); });
    auto* linked = makeAction(timeline, "linked_selection", "Verknüpfte Auswahl", QKeySequence("Ctrl+Shift+L"), [] {});
    linked->setCheckable(true);
    linked->setChecked(m_editor->linkedSelection());
    connect(linked, &QAction::toggled, this, [this](bool on) { m_editor->setLinkedSelection(on); });
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

    InputBindings::instance().saveIfIncomplete();
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
    m_engine->pause();
    if (m_engine->mode() != Engine::Mode::Timeline) m_engine->showTimeline(best);
    else m_engine->seek(best);
}
