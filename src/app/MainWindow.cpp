#include "app/MainWindow.h"

#include "app/InputBindings.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
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

    statusBar()->showMessage(QString("Tastenbelegung anpassbar: %1").arg(InputBindings::instance().filePath()));
}

void MainWindow::importFiles(const QStringList& paths, bool placeOnTimeline)
{
    m_mediaPool->importFiles(paths);
    if (!placeOnTimeline) return;
    // Testhilfe (--demo): Dateien hintereinander auf V1/A1 legen
    for (const QString& path : paths) m_editor->addMediaAt(path, TimelineOps::endFrame(m_project->timeline()));
}

void MainWindow::buildLayout()
{
    m_mediaPool = new MediaPool(m_project, m_engine);
    m_viewer = new Viewer(m_engine);
    m_inspector = new Inspector(m_editor);
    m_timeline = new TimelinePanel(m_editor);

    auto* top = new QSplitter(Qt::Horizontal);
    top->addWidget(m_mediaPool);
    top->addWidget(m_viewer);
    top->addWidget(m_inspector);
    top->setStretchFactor(0, 0);
    top->setStretchFactor(1, 1);
    top->setStretchFactor(2, 0);
    top->setSizes({360, 880, 360});

    auto* main = new QSplitter(Qt::Vertical);
    main->addWidget(top);
    main->addWidget(m_timeline);
    main->setStretchFactor(0, 1);
    main->setStretchFactor(1, 1);
    main->setSizes({480, 420});
    setCentralWidget(main);
}

QAction* MainWindow::makeAction(QMenu* menu, const QString& id, const QString& text, const QKeySequence& key,
                               const std::function<void()>& fn)
{
    auto* a = new QAction(text, this);
    a->setShortcut(InputBindings::instance().shortcut(id, key));
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
    makeAction(edit, "delete", "Löschen (Lücke bleibt)", QKeySequence(Qt::Key_Backspace), [this] { m_editor->deleteSelection(); });
    makeAction(edit, "delete_alt", "Löschen (Entf)", QKeySequence(Qt::Key_Delete), [this] { m_editor->deleteSelection(); });
    makeAction(edit, "split", "Clip am Playhead teilen", QKeySequence("Ctrl+B"),
              [this, tv] { m_editor->splitAtPlayhead(tv->playhead()); });
    makeAction(edit, "deselect", "Auswahl aufheben", QKeySequence("Ctrl+Shift+A"), [this] { m_selection->clear(); });

    QMenu* timeline = menuBar()->addMenu("&Timeline");
    makeAction(timeline, "tool_select", "Auswahl-Werkzeug", QKeySequence("A"), [tv] { tv->setTool(TimelineView::Tool::Select); });
    makeAction(timeline, "tool_blade", "Klingen-Werkzeug", QKeySequence("B"), [tv] { tv->setTool(TimelineView::Tool::Blade); });
    makeAction(timeline, "snapping", "Snapping an/aus", QKeySequence("N"), [tv] { tv->setSnapping(!tv->snapping()); });
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

    InputBindings::instance().saveIfMissing();
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
    const int pos = m_timeline->view()->playhead();
    int best = direction > 0 ? INT_MAX : -1;
    const Timeline& tl = m_project->timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const auto& t : tl.tracks(k))
            for (const auto& c : t.clips)
                for (int e : {c.start, c.end()}) {
                    if (direction > 0 && e > pos) best = std::min(best, e);
                    if (direction < 0 && e < pos) best = std::max(best, e);
                }
    if (best == INT_MAX || best < 0) return;
    m_engine->pause();
    if (m_engine->mode() != Engine::Mode::Timeline) m_engine->showTimeline(best);
    else m_engine->seek(best);
}
