// MainWindow: menus and actions.
#include "app/MainWindow.h"
#include "app/ExtensionsDialog.h"

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
#include "engine/audio/Device.h"
#include "engine/audio/Path.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaStorage.h"
#include "ui/ColorPanel.h"
#include "ui/ScopesPanel.h"
#include "ui/Inspector.h"
#include "ui/Mixer.h"
#include "ui/MediaPool.h"
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

QAction* MainWindow::makeAction(QMenu* menu, const QString& id, const QString& text, const QKeySequence& key,
                               const std::function<void()>& fn)
{
    auto* a = new QAction(text, this);
    // Category in the key bindings = main menu (submenus only serve the overview)
    QMenu* top = menu;
    while (auto* parent = qobject_cast<QMenu*>(top->parent())) top = parent;
    InputBindings::instance().registerAction(a, id, top->title().remove('&'), key);
    a->setShortcutContext(Qt::WindowShortcut);
    connect(a, &QAction::triggered, this, fn);
    menu->addAction(a);
    return a;
}

void MainWindow::buildActions()
{
    TimelineView* tv = m_timeline->view();
    QUndoStack* undo = m_project->undoStack();

    // Bindings after DaVinci Resolve (defaults), overridable in keybindings.json
    QMenu* file = menuBar()->addMenu(T("&Datei"));
    makeAction(file, "project_new", T("Neues Projekt"), QKeySequence("Ctrl+N"), [this] { newProject(); });
    makeAction(file, "project_open", T("Projekt öffnen…"), QKeySequence("Ctrl+O"), [this] { openProjectDialog(); });
    m_recentMenu = file->addMenu(T("Zuletzt geöffnet"));
    rebuildRecentMenu();
    makeAction(file, "project_save", T("Projekt speichern"), QKeySequence("Ctrl+S"), [this] { save(); });
    makeAction(file, "project_save_as", T("Projekt speichern unter…"), QKeySequence("Ctrl+Shift+S"), [this] { saveAs(); });
    makeAction(file, "project_backup_open", T("Sicherungskopie öffnen…"), QKeySequence(), [this] { openBackupDialog(); });
    file->addSeparator();
    makeAction(file, "import", T("Medien importieren…"), QKeySequence("Ctrl+I"), [this] { m_mediaPool->importDialog(); });
    makeAction(file, "new_bin", T("Neuer Bin"), QKeySequence("Ctrl+Shift+N"), [this] { m_mediaPool->newBin(); });
    makeAction(file, "new_timeline", T("Neue Timeline"), QKeySequence("Ctrl+Alt+N"), [this] { m_mediaPool->newTimeline(); });
    makeAction(file, "import_subtitles", T("Untertitel importieren (SRT)…"), QKeySequence(), [this] { importSubtitlesDialog(); });
    makeAction(file, "export_subtitles", T("Untertitel exportieren (SRT)…"), QKeySequence(), [this] { exportSubtitlesDialog(); });
    // Like DaVinci File › Import/Export › Timeline: cuts from and for Premiere, DaVinci, Final Cut 7, Avid …
    makeAction(file, "import_timeline", T("Timeline importieren (XML, OTIO, EDL)…"), QKeySequence("Ctrl+Shift+I"),
               [this] { importTimelineDialog(); });
    makeAction(file, "export_timeline", T("Timeline exportieren (Premiere, DaVinci, EDL)…"), QKeySequence(), [this] { exportTimelineDialog(); });
    file->addSeparator();
    makeAction(file, "project_settings", T("Projekteinstellungen…"), QKeySequence("Shift+9"), [this] { projectSettingsDialog(); });
    makeAction(file, "stream_presets", T("Import-Vorgaben für Ton-Streams…"), QKeySequence(), [this] { streamPresetsDialog(); });
    file->addSeparator();
    makeAction(file, "quit", T("Beenden"), QKeySequence("Ctrl+Q"), [this] { close(); });

    QMenu* edit = menuBar()->addMenu(T("&Bearbeiten"));
    QAction* undoAct = makeAction(edit, "undo", T("Rückgängig"), QKeySequence("Ctrl+Z"), [undo] { undo->undo(); });
    QAction* redoAct = makeAction(edit, "redo", T("Wiederholen"), QKeySequence("Ctrl+Shift+Z"), [undo] { undo->redo(); });
    connect(undo, &QUndoStack::canUndoChanged, undoAct, &QAction::setEnabled);
    connect(undo, &QUndoStack::canRedoChanged, redoAct, &QAction::setEnabled);
    undoAct->setEnabled(false);
    redoAct->setEnabled(false);
    edit->addSeparator();
    makeAction(edit, "cut", T("Ausschneiden"), QKeySequence("Ctrl+X"), [this] { m_editor->cutSelection(); });
    // On the Color page Ctrl+C/Ctrl+V copy and paste the grade, like DaVinci
    makeAction(edit, "copy", T("Kopieren"), QKeySequence("Ctrl+C"), [this] {
        if (m_page == Page::Color) m_colorPanel->copyGrade();
        else m_editor->copySelection();
    });
    makeAction(edit, "paste", T("Einfügen am Playhead"), QKeySequence("Ctrl+V"), [this, tv] {
        if (m_page == Page::Color) m_colorPanel->pasteGrade();
        else m_editor->paste(tv->playhead());
    });
    makeAction(edit, "paste_attributes", T("Attribute einfügen…"), QKeySequence("Alt+V"), [this] {
        const int targets = int(m_editor->editable(m_editor->clipIdsOf(m_selection->ids())).size());
        const int available = m_editor->pasteAttributesAvailable();
        if (!available || !targets) {
            QMessageBox::information(this, T("Attribute einfügen"),
                                     !available ? T("Erst einen Clip kopieren (Strg+C).")
                                                : T("Clips auswählen, auf die die Attribute übertragen werden sollen."));
            return;
        }
        PasteAttributesDialog dlg(available, m_editor->pasteAttributesSource(), targets, this);
        if (dlg.exec() == QDialog::Accepted) m_editor->pasteAttributes(dlg.attributes());
    });
    edit->addSeparator();
    makeAction(edit, "delete", T("Löschen (Lücke bleibt)"), QKeySequence(Qt::Key_Backspace), [this] { m_editor->deleteSelection(); });
    makeAction(edit, "delete_alt", T("Löschen (Entf)"), QKeySequence(Qt::Key_Delete), [this] { m_editor->deleteSelection(); });
    makeAction(edit, "ripple_delete", T("Löschen mit Ripple"), QKeySequence("Shift+Del"), [this] { m_editor->rippleDeleteSelection(); });
    makeAction(edit, "ripple_delete_alt", T("Löschen mit Ripple (Rücktaste)"), QKeySequence("Shift+Backspace"),
               [this] { m_editor->rippleDeleteSelection(); });
    edit->addSeparator();
    // Edits from the source Viewer like DaVinci (Edit → Insert/Overwrite/Replace/Place on Top …)
    using SE = Editor::SourceEditMode;
    makeAction(edit, "insert_clip", T("Clip einfügen"), QKeySequence(Qt::Key_F9), [this] { sourceEdit(SE::Insert); });
    makeAction(edit, "overwrite_clip", T("Clip überschreiben"), QKeySequence(Qt::Key_F10), [this] { sourceEdit(SE::Overwrite); });
    makeAction(edit, "replace_clip", T("Clip ersetzen"), QKeySequence(Qt::Key_F11), [this] { sourceEdit(SE::Replace); });
    // Target tracks like DaVinci (Destination Controls): Alt+1…8 video, Ctrl+Alt+1…8 audio; also click on the track label
    QMenu* dest = edit->addMenu(T("Zielspur"));
    for (int i = 0; i < 8; ++i) {
        makeAction(dest, QString("target_video_%1").arg(i + 1), T("Video-Zielspur V%1").arg(i + 1),
                   QKeySequence(QString("Alt+%1").arg(i + 1)),
                   [this, i] { m_editor->setTargetTracks(i, m_editor->targetAudioTrack()); });
    }
    dest->addSeparator();
    for (int i = 0; i < 8; ++i) {
        makeAction(dest, QString("target_audio_%1").arg(i + 1), T("Audio-Zielspur A%1").arg(i + 1),
                   QKeySequence(QString("Ctrl+Alt+%1").arg(i + 1)),
                   [this, i] { m_editor->setTargetTracks(m_editor->targetVideoTrack(), i); });
    }
    makeAction(edit, "fit_to_fill", T("Einpassen (Fit to Fill)"), QKeySequence("Shift+F11"), [this] { sourceEdit(SE::FitToFill); });
    makeAction(edit, "place_on_top", T("Oben platzieren"), QKeySequence(Qt::Key_F12), [this] { sourceEdit(SE::PlaceOnTop); });
    makeAction(edit, "ripple_overwrite", T("Ripple-Überschreiben"), QKeySequence("Shift+F10"),
               [this] { sourceEdit(SE::RippleOverwrite); });
    makeAction(edit, "append_at_end", T("Am Ende der Timeline anhängen"), QKeySequence("Shift+F12"),
               [this] { sourceEdit(SE::AppendAtEnd); });
    edit->addSeparator();
    makeAction(edit, "select_all", T("Alles auswählen"), QKeySequence("Ctrl+A"), [this] { m_editor->selectAll(); });
    makeAction(edit, "deselect", T("Auswahl aufheben"), QKeySequence("Ctrl+Shift+A"), [this] { m_selection->clear(); });
    QMenu* selectMenu = edit->addMenu(T("Ab Playhead auswählen"));
    makeAction(selectMenu, "select_forward", T("Clips ab Playhead nach rechts (diese Spur)"), QKeySequence("Y"),
               [this, tv] { m_editor->selectFromPlayhead(tv->playhead(), true, false); });
    makeAction(selectMenu, "select_backward", T("Clips ab Playhead nach links (diese Spur)"), QKeySequence("Ctrl+Y"),
               [this, tv] { m_editor->selectFromPlayhead(tv->playhead(), false, false); });
    makeAction(selectMenu, "select_forward_all", T("Clips ab Playhead nach rechts (alle Spuren)"), QKeySequence("Alt+Y"),
               [this, tv] { m_editor->selectFromPlayhead(tv->playhead(), true, true); });
    makeAction(selectMenu, "select_backward_all", T("Clips ab Playhead nach links (alle Spuren)"),
               QKeySequence("Ctrl+Alt+Y"), [this, tv] { m_editor->selectFromPlayhead(tv->playhead(), false, true); });
    edit->addSeparator();
    makeAction(edit, "edit_keybindings", T("Tastenbelegung…"), QKeySequence("Ctrl+Alt+K"), [this] {
        KeyBindingsDialog dlg(this);
        dlg.exec();
    });

    // Organized like DaVinci: the most frequent commands on top, the rest in submenus (otherwise the menu fills the screen).
    // Key bindings category stays "Timeline" (makeAction takes the topmost menu).
    QMenu* timeline = menuBar()->addMenu("&Timeline");
    makeAction(timeline, "split", T("Clip am Playhead teilen"), QKeySequence("Ctrl+B"),
               [this, tv] { m_editor->splitAtPlayhead(tv->playhead()); });
    makeAction(timeline, "add_transition", T("Übergang hinzufügen (Cross Dissolve)"), QKeySequence("Ctrl+T"),
               [this, tv] {
                   const bool trim = !m_editor->transitionsLackingHandles(tv->playhead()).isEmpty()
                                     && TimelineView::confirmTrimForTransition(this);
                   m_editor->addTransitions(tv->playhead(), {}, {}, trim);
               });
    makeAction(timeline, "add_title", T("Titel einfügen"), QKeySequence(), [this, tv] { m_editor->addTitle(tv->playhead()); });
    // In the source viewer M sets a source marker (multicam sync by marker), like DaVinci
    makeAction(timeline, "add_marker", T("Marker setzen/entfernen"), QKeySequence("M"), [this, tv] {
        if (sourceActive()) m_editor->toggleSourceMarker(m_engine->sourcePath(), m_engine->position());
        else m_editor->toggleMarker(tv->playhead());
    });
    timeline->addSeparator();
    QMenu* toolsMenu = timeline->addMenu(T("Werkzeuge"));
    makeAction(toolsMenu, "tool_select", T("Auswahl-Werkzeug"), QKeySequence("A"), [tv] { tv->setTool(TimelineView::Tool::Select); });
    makeAction(toolsMenu, "tool_trim", T("Trim-Modus"), QKeySequence("T"), [tv] { tv->setTool(TimelineView::Tool::Trim); });
    makeAction(toolsMenu, "tool_blade", T("Klingen-Werkzeug"), QKeySequence("B"), [tv] { tv->setTool(TimelineView::Tool::Blade); });
    makeAction(toolsMenu, "snapping", T("Snapping an/aus"), QKeySequence("N"), [tv] { tv->setSnapping(!tv->snapping()); });
    auto* linked = makeAction(toolsMenu, "linked_selection", T("Verknüpfte Auswahl"), QKeySequence("Ctrl+Shift+L"), [] {});
    linked->setCheckable(true);
    linked->setToolTip(T("Verknüpfte Auswahl (Strg+Shift+L): an = Video und Ton eines Clips bewegen sich zusammen, "
                         "aus = einzeln auswählen, verschieben und trimmen"));
    linked->setChecked(QSettings().value("edit/linkedSelection", true).toBool());
    m_editor->setLinkedSelection(linked->isChecked());
    connect(linked, &QAction::toggled, this, [this](bool on) {
        m_editor->setLinkedSelection(on);
        if (!m_autosaveDisabled) QSettings().setValue("edit/linkedSelection", on);
    });
    m_timeline->addToolAction(linked, TimelinePanel::Icon::Link);
    auto* splitTracks = makeAction(toolsMenu, "split_on_tracks", T("Teilen auf ganzer Spur der Auswahl"), QKeySequence(), [] {});
    splitTracks->setCheckable(true);
    splitTracks->setToolTip(T("An: Strg+B/Maustaste teilt auch den Nachbarclip auf der Spur des ausgewählten Clips. "
                            "Aus: nur ausgewählte Clips."));
    splitTracks->setChecked(QSettings().value("edit/splitOnSelectedTracks", true).toBool());
    m_editor->setSplitOnSelectedTracks(splitTracks->isChecked());
    connect(splitTracks, &QAction::toggled, this, [this](bool on) {
        m_editor->setSplitOnSelectedTracks(on);
        QSettings().setValue("edit/splitOnSelectedTracks", on);
    });
    QMenu* trimMenu = timeline->addMenu(T("Teilen, Trimmen und Verschieben"));
    makeAction(trimMenu, "split_alt", T("Clip teilen (alternativ)"), QKeySequence("Ctrl+\\"),
               [this, tv] { m_editor->splitAtPlayhead(tv->playhead()); });
    makeAction(trimMenu, "trim_start", T("Anfang bis Playhead trimmen"), QKeySequence("Shift+["),
               [this, tv] { m_editor->trimToPlayhead(TimelineOps::Edge::Start, tv->playhead()); });
    makeAction(trimMenu, "trim_end", T("Ende bis Playhead trimmen"), QKeySequence("Shift+]"),
               [this, tv] { m_editor->trimToPlayhead(TimelineOps::Edge::End, tv->playhead()); });
    // Selected edit point (V): , and . trim it. Otherwise trim mode: slip the selection (like DaVinci), otherwise move
    makeAction(trimMenu, "select_edit_point", T("Nächsten Schnittpunkt auswählen"), QKeySequence("V"),
               [this, tv] { m_editor->selectNearestEditPoint(tv->playhead()); });
    makeAction(trimMenu, "edit_point_type", T("Schnittpunkt-Seite wechseln"), QKeySequence("U"),
               [this] { m_editor->cycleEditPointSide(); });
    auto nudge = [this, tv](int frames) {
        if (m_editor->nudgeEditPoint(frames, tv->tool() == TimelineView::Tool::Trim)) return;
        if (tv->tool() == TimelineView::Tool::Trim) m_editor->slipSelection(frames);
        else m_editor->nudgeSelection(frames);
    };
    makeAction(trimMenu, "nudge_left", T("1 Frame nach links schieben"), QKeySequence(","), [nudge] { nudge(-1); });
    makeAction(trimMenu, "nudge_right", T("1 Frame nach rechts schieben"), QKeySequence("."), [nudge] { nudge(1); });
    makeAction(trimMenu, "nudge_left_multi", T("5 Frames nach links schieben"), QKeySequence("Shift+,"),
               [nudge] { nudge(-5); });
    makeAction(trimMenu, "nudge_right_multi", T("5 Frames nach rechts schieben"), QKeySequence("Shift+."),
               [nudge] { nudge(5); });
    QMenu* clipMenu = timeline->addMenu(T("Clip"));
    makeAction(clipMenu, "toggle_enabled", T("Clip aktivieren/deaktivieren"), QKeySequence("D"),
               [this] { m_editor->toggleSelectionEnabled(); });
    makeAction(clipMenu, "clip_speed", T("Clip-Geschwindigkeit ändern…"), QKeySequence("Ctrl+R"), [this] { clipSpeedDialog(); });
    makeAction(clipMenu, "clip_speed_reset", T("Geschwindigkeit zurücksetzen"), QKeySequence("Ctrl+Alt+R"), [this] {
        m_editor->setClipSpeed(m_editor->selection()->ids().values().toVector(), {}, true);
    });
    // Retime controls like DaVinci (Retime Controls): speed per section, speed points on the clip
    auto* retime = makeAction(clipMenu, "retime_controls", T("Retime-Steuerung"), QKeySequence("Ctrl+Shift+R"), [this] {
        const QVector<int> ids = m_editor->clipIdsOf(m_selection->ids());
        TimelineView* tv = m_timeline->view();
        const bool on = std::none_of(ids.begin(), ids.end(), [tv](int id) { return tv->retimeControlsShown(id); });
        tv->setRetimeControls(ids, on);
    });
    retime->setCheckable(true);
    // Curve editor like DaVinci (curve icon in the clip): keyframe curve under the selected clips
    auto* curves = makeAction(clipMenu, "curve_editor", T("Kurven-Editor"), QKeySequence("Shift+C"), [this] {
        const QVector<int> ids = m_editor->clipIdsOf(m_selection->ids());
        TimelineView* tv = m_timeline->view();
        const bool on = std::none_of(ids.begin(), ids.end(), [tv](int id) { return tv->curveEditorShown(id); });
        tv->setCurveEditor(ids, on);
        if (on && std::none_of(ids.begin(), ids.end(), [tv](int id) { return tv->curveEditorShown(id); }))
            QApplication::beep(); // no keyframes
    });
    curves->setCheckable(true);
    makeAction(clipMenu, "speed_point_add", T("Speed-Punkt hinzufügen"), QKeySequence(), [this] {
        // at the playhead position in the selected clips (resp. the video clip below), show the controls
        const int frame = m_timeline->view()->playhead();
        QVector<int> ids = m_editor->clipIdsOf(m_selection->ids());
        if (ids.isEmpty())
            for (const Track& t : m_project->timeline().video)
                for (const Clip& c : t.clips)
                    if (c.start < frame && frame < c.end()) ids << c.id;
        QSet<int> done;
        for (int id : ids) {
            const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
            if (!c || done.contains(c->linkId ? -c->linkId : id) || !m_editor->canRetime(id)) continue;
            if (!(c->start < frame && frame < c->end())) continue;
            done.insert(c->linkId ? -c->linkId : id);
            m_editor->addSpeedPoint(id, frame);
            m_timeline->view()->setRetimeControls({id}, true);
        }
        if (done.isEmpty()) QApplication::beep();
    });
    makeAction(clipMenu, "normalize_audio", T("Audiopegel normalisieren…"), QKeySequence(), [this] { normalizeAudioDialog(); });
    makeAction(clipMenu, "audio_ducking", T("Ducking…"), QKeySequence(), [this] { duckingDialog(); });
    makeAction(clipMenu, "stream_presets_learn", T("Spur-Einstellungen merken"), QKeySequence(), [this] { rememberStreamSettings(); });
    makeAction(clipMenu, "save_effect_preset", T("Effekte als Preset speichern…"), QKeySequence(),
               [this] { saveEffectPreset(); });
    makeAction(clipMenu, "link_clips", T("Clips verknüpfen/trennen"), QKeySequence("Ctrl+Alt+L"),
               [this] { m_editor->toggleLinkSelection(); });
    // Like DaVinci "Render Cache Clip Output": preview plays the pre-rendered clip output (Playback > Render Cache)
    auto* cacheClip = makeAction(clipMenu, "render_cache_clip", T("Render-Cache Clip-Ausgabe"), QKeySequence(),
                                 [this] { m_editor->toggleSelectionRenderCache(); });
    cacheClip->setCheckable(true);
    connect(clipMenu, &QMenu::aboutToShow, this, [this, retime] {
        const QVector<int> ids = m_editor->clipIdsOf(m_selection->ids());
        TimelineView* tv = m_timeline->view();
        retime->setEnabled(std::any_of(ids.begin(), ids.end(), [this](int id) { return m_editor->canRetime(id); }));
        retime->setChecked(std::any_of(ids.begin(), ids.end(), [tv](int id) { return tv->retimeControlsShown(id); }));
    });
    connect(clipMenu, &QMenu::aboutToShow, this, [this, cacheClip] {
        const int state = m_editor->selectionRenderCacheState();
        cacheClip->setEnabled(state >= 0);
        cacheClip->setChecked(state == 1);
    });
    QMenu* compoundMenu = timeline->addMenu(T("Compound Clip"));
    makeAction(compoundMenu, "compound_create", T("Neuer Compound Clip…"), QKeySequence(), [this] { createCompoundClip(); });
    makeAction(compoundMenu, "compound_open", T("Compound Clip in Timeline öffnen"), QKeySequence(),
               [this] { openSelectedCompound(); });
    makeAction(compoundMenu, "compound_decompose", T("Compound Clip auflösen"), QKeySequence(), [this] {
        m_editor->decomposeCompoundClips(m_editor->clipIdsOf(m_selection->ids()));
    });
    makeAction(compoundMenu, "timeline_back", T("Zurück zur übergeordneten Timeline"), QKeySequence(),
               [this] { m_timeline->back(); });
    buildMulticamActions(timeline);
    QMenu* subtitleMenu = timeline->addMenu(T("Untertitel"));
    makeAction(subtitleMenu, "add_subtitle", T("Untertitel hinzufügen"), QKeySequence(), [this, tv] {
        if (const int id = m_editor->addSubtitle(tv->playhead())) {
            m_inspectorToggle->setChecked(true);
            m_inspector->editSubtitle(id);
        }
    });
    makeAction(subtitleMenu, "add_subtitle_track", T("Untertitelspur hinzufügen"), QKeySequence(),
               [this] { m_editor->addSubtitleTrack(); });
    makeAction(subtitleMenu, "transcribe_subtitles", T("Untertitel aus Audio erzeugen…"), QKeySequence(),
               [this] { transcribeDialog(); });
    QMenu* markMenu = timeline->addMenu(T("In/Out und Marker"));
    // In/Out like DaVinci: if the Viewer shows the source, they apply to the source clip (remembered per Media Pool clip)
    auto markIn = [this, tv](int frame) {
        if (sourceActive()) m_editor->setSourceMarkIn(m_engine->sourcePath(), frame < 0 ? -1 : m_engine->position());
        else m_editor->setMarkIn(frame < 0 ? -1 : tv->playhead());
    };
    auto markOut = [this, tv](int frame) {
        if (sourceActive()) m_editor->setSourceMarkOut(m_engine->sourcePath(), frame < 0 ? -1 : m_engine->position());
        else m_editor->setMarkOut(frame < 0 ? -1 : tv->playhead());
    };
    auto gotoMark = [this](bool in) {
        if (sourceActive()) {
            const MediaInfo* m = m_project->mediaInfo(m_engine->sourcePath());
            const int f = in ? m->markIn : m->markOut;
            if (f < 0) return;
            m_engine->pause();
            m_engine->seek(f);
            return;
        }
        jumpToFrame(in ? m_project->timeline().markIn : m_project->timeline().markOut);
    };
    makeAction(markMenu, "mark_in", T("In-Punkt setzen"), QKeySequence("I"), [markIn] { markIn(0); });
    makeAction(markMenu, "mark_out", T("Out-Punkt setzen"), QKeySequence("O"), [markOut] { markOut(0); });
    makeAction(markMenu, "clear_in", T("In-Punkt entfernen"), QKeySequence("Alt+I"), [markIn] { markIn(-1); });
    makeAction(markMenu, "clear_out", T("Out-Punkt entfernen"), QKeySequence("Alt+O"), [markOut] { markOut(-1); });
    makeAction(markMenu, "clear_in_out", T("In und Out entfernen"), QKeySequence("Alt+X"), [this] {
        if (sourceActive()) m_editor->clearSourceMarks(m_engine->sourcePath());
        else m_editor->clearMarks();
    });
    makeAction(markMenu, "goto_in", T("Zum In-Punkt"), QKeySequence("Shift+I"), [gotoMark] { gotoMark(true); });
    makeAction(markMenu, "goto_out", T("Zum Out-Punkt"), QKeySequence("Shift+O"), [gotoMark] { gotoMark(false); });
    makeAction(markMenu, "marker_prev", T("Vorheriger Marker"), QKeySequence("Shift+Up"), [this] { jumpToMarker(-1); });
    makeAction(markMenu, "marker_next", T("Nächster Marker"), QKeySequence("Shift+Down"), [this] { jumpToMarker(1); });
    // Timeline view: show/hide clip names/duration (saved), zoom
    QMenu* viewOpts = timeline->addMenu(T("Timeline-Ansicht"));
    auto addViewOption = [this, viewOpts](const QString& id, const QString& text, const QString& key, bool def,
                                          std::function<void(bool)> apply) {
        auto* a = makeAction(viewOpts, id, text, QKeySequence(), [] {});
        a->setCheckable(true);
        a->setChecked(QSettings().value(key, def).toBool());
        apply(a->isChecked());
        connect(a, &QAction::toggled, this, [this, key, apply](bool on) {
            apply(on);
            if (!m_autosaveDisabled) QSettings().setValue(key, on);
        });
    };
    addViewOption("show_clip_names", T("Clipnamen anzeigen"), "timeline/showClipNames", true,
                  [tv](bool on) { tv->setShowClipNames(on); });
    addViewOption("show_clip_durations", T("Clipdauer anzeigen"), "timeline/showClipDurations", false,
                  [tv](bool on) { tv->setShowClipDurations(on); });
    viewOpts->addSeparator();
    makeAction(viewOpts, "zoom_in", T("Hineinzoomen"), QKeySequence("Ctrl+="), [tv] { tv->zoomBy(1.5); });
    makeAction(viewOpts, "zoom_out", T("Herauszoomen"), QKeySequence("Ctrl+-"), [tv] { tv->zoomBy(1 / 1.5); });
    makeAction(viewOpts, "zoom_fit", T("Ganze Timeline zeigen"), QKeySequence("Shift+Z"), [tv] { tv->zoomToFit(); });

    QMenu* play = menuBar()->addMenu(T("&Wiedergabe"));
    makeAction(play, "play_pause", "Play/Pause", QKeySequence(Qt::Key_Space), [this] { m_engine->togglePlay(); });
    makeAction(play, "shuttle_back", T("Rückwärts (J)"), QKeySequence("J"), [this] { shuttle(-1); });
    makeAction(play, "stop", T("Stopp (K)"), QKeySequence("K"), [this] { m_engine->pause(); });
    makeAction(play, "shuttle_fwd", T("Vorwärts (L)"), QKeySequence("L"), [this] { shuttle(1); });
    play->addSeparator();
    makeAction(play, "frame_prev", T("1 Frame zurück"), QKeySequence(Qt::Key_Left), [this] { stepFrames(-1); });
    makeAction(play, "frame_next", T("1 Frame vor"), QKeySequence(Qt::Key_Right), [this] { stepFrames(1); });
    makeAction(play, "second_prev", T("1 Sekunde zurück"), QKeySequence("Shift+Left"), [this] { stepFrames(-m_project->fps()); });
    makeAction(play, "second_next", T("1 Sekunde vor"), QKeySequence("Shift+Right"), [this] { stepFrames(m_project->fps()); });
    makeAction(play, "edit_prev", T("Vorheriger Schnittpunkt"), QKeySequence(Qt::Key_Up), [this] { jumpToEdit(-1); });
    makeAction(play, "edit_next", T("Nächster Schnittpunkt"), QKeySequence(Qt::Key_Down), [this] { jumpToEdit(1); });
    makeAction(play, "go_start", T("Zum Anfang"), QKeySequence(Qt::Key_Home), [this] { m_engine->pause(); m_engine->seek(0); });
    makeAction(play, "go_end", T("Zum Ende"), QKeySequence(Qt::Key_End), [this] {
        m_engine->pause();
        m_engine->seek(TimelineOps::endFrame(m_project->timeline()));
    });
    makeAction(play, "show_timeline", T("Timeline im Viewer zeigen"), QKeySequence("Q"),
              [this, tv] { m_engine->showTimeline(tv->playhead()); });
    buildMulticamViewerAction(play);
    // Like DaVinci "Grab Still" (Ctrl+Alt+G), here saved directly as PNG
    auto* grab = makeAction(play, "grab_still", T("Standbild exportieren…"), QKeySequence("Ctrl+Alt+G"), [this] { grabStill(); });
    m_viewer->addAction(grab);
    m_viewer->setContextMenuPolicy(Qt::ActionsContextMenu);
    play->addSeparator();
    // Like DaVinci (Playback → Use Proxy Media if Available); export always uses the originals
    auto* useProxy = makeAction(play, "use_proxy", T("Proxy-Medien verwenden, falls vorhanden"), QKeySequence(), [] {});
    useProxy->setCheckable(true);
    useProxy->setChecked(m_engine->proxies()->enabled());
    connect(useProxy, &QAction::toggled, m_engine->proxies(), &ProxyManager::setEnabled);
    // Before/After like DaVinci (Shift+D, Bypass Color Grades): preview only, export always with correction
    auto* bypass = makeAction(play, "color_bypass", T("Farbkorrektur umgehen (Vorher/Nachher)"), QKeySequence("Shift+D"),
                              [] {});
    bypass->setCheckable(true);
    connect(bypass, &QAction::toggled, m_engine, &Engine::setColorBypass);
    connect(m_engine, &Engine::colorBypassChanged, bypass, &QAction::setChecked);
    // Like DaVinci (Playback → Render Cache: None/Smart/User); export always renders from the originals
    QMenu* cacheMenu = play->addMenu(T("Render-Cache"));
    auto* cacheGroup = new QActionGroup(cacheMenu);
    for (const auto& [id, text, mode] : {std::tuple{"render_cache_off", N_("Aus"), RenderCache::Mode::Off},
                                         std::tuple{"render_cache_smart", N_("Smart"), RenderCache::Mode::Smart},
                                         std::tuple{"render_cache_user", N_("Benutzer"), RenderCache::Mode::User}}) {
        const RenderCache::Mode m = mode;
        QAction* a = makeAction(play, id, T("Render-Cache: %1").arg(T(text)), QKeySequence(),
                                [this, m] { m_engine->renderCache()->setMode(m); });
        play->removeAction(a); // settings category "Wiedergabe", shown in the submenu
        a->setText(T(text));
        a->setCheckable(true);
        a->setChecked(m_engine->renderCache()->mode() == m);
        cacheGroup->addAction(a);
        cacheMenu->addAction(a);
    }
    cacheMenu->addSeparator();
    QAction* clearCache = makeAction(play, "render_cache_clear", T("Render-Cache löschen"), QKeySequence(),
                                     [this] { m_engine->renderCache()->clear(); });
    play->removeAction(clearCache);
    cacheMenu->addAction(clearCache);

    // Audio output: buffer size (against crackling, takes effect from the next play) and on Windows the driver
    QMenu* audioMenu = play->addMenu(T("Audio-Ausgabe"));
    auto* bufferGroup = new QActionGroup(audioMenu);
    for (const auto& [samples, text] : {std::pair{1024, N_("Puffer klein (1024, wenig Verzögerung)")},
                                        std::pair{2048, N_("Puffer mittel (2048)")},
                                        std::pair{4096, N_("Puffer groß (4096, gegen Knacken)")}}) {
        QAction* a = audioMenu->addAction(T(text));
        a->setCheckable(true);
        a->setChecked(Engine::audioBuffer() == samples);
        bufferGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, n = samples] { m_engine->setAudioBuffer(n); });
    }
    // Output device like DaVinci (Video and Audio I/O → Speaker Setup); only the own sound engine can choose one. The
    // list is read when the menu opens: devices come and go.
    if (Audio::ownPath()) {
        QMenu* outputMenu = audioMenu->addMenu(T("Ausgabegerät"));
        connect(outputMenu, &QMenu::aboutToShow, this, [this, outputMenu] {
            outputMenu->clear();
            auto* group = new QActionGroup(outputMenu);
            const QString current = Engine::audioOutput();
            auto add = [&](const QString& id, const QString& text) {
                QAction* a = outputMenu->addAction(text);
                a->setCheckable(true);
                a->setChecked(id == current);
                group->addAction(a);
                connect(a, &QAction::triggered, this, [this, id] { m_engine->setAudioOutput(id); });
            };
            add(QString(), T("Systemstandard"));
            outputMenu->addSeparator();
            bool found = current.isEmpty();
            for (const Audio::Device::Output& o : Audio::Device::outputs(Engine::audioDriver())) {
                add(o.id, o.name);
                found = found || o.id == current;
            }
            if (!found) add(current, T("%1 (nicht angeschlossen)").arg(current));
        });
    }
    // Sound engine (docs/plan-0.10-ton.md): the own one is tried out here until it becomes the default
    audioMenu->addSeparator();
    auto* engineGroup = new QActionGroup(audioMenu);
    for (const auto& [own, text] : {std::pair{false, N_("Ton-Maschine: MLT (bisher)")},
                                    std::pair{true, N_("Ton-Maschine: SMT (neu, Test)")}}) {
        QAction* a = audioMenu->addAction(T(text));
        a->setCheckable(true);
        a->setChecked(Audio::ownPath() == own);
        engineGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, own = own] {
            if (own == Audio::ownPath()) return;
            Audio::setOwnPath(own);
            offerRestart(T("Die Ton-Maschine wechselt nach dem Neustart von schneidi."), T("Jetzt neu starten"),
                         T("Später"));
        });
    }
#ifdef Q_OS_WIN
    audioMenu->addSeparator();
    auto* driverGroup = new QActionGroup(audioMenu);
    for (const auto& [id, text] : {std::pair{"directsound", N_("Treiber: DirectSound (Standard)")},
                                   std::pair{"wasapi", N_("Treiber: WASAPI")}}) {
        QAction* a = audioMenu->addAction(T(text));
        a->setCheckable(true);
        a->setChecked(Engine::audioDriver() == id);
        driverGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, driver = QString(id)] {
            if (driver == Engine::audioDriver()) return;
            Engine::setAudioDriver(driver);
            offerRestart(T("Der Audiotreiber ändert sich nach dem Neustart von schneidi."), T("Jetzt neu starten"),
                         T("Später"));
        });
    }
#endif

    // Color menu like DaVinci (Color → Apply Grade from One/Two Clips Prior); works on the Color page
    QMenu* colorMenu = menuBar()->addMenu("&Color");
    makeAction(colorMenu, "grade_copy", T("Grade kopieren (Color-Seite: Strg+C)"), QKeySequence(),
               [this] { m_colorPanel->copyGrade(); });
    makeAction(colorMenu, "grade_paste", T("Grade einfügen (Color-Seite: Strg+V)"), QKeySequence(),
               [this] { m_colorPanel->pasteGrade(); });
    makeAction(colorMenu, "grade_prev", T("Grade vom vorigen Clip übernehmen"), QKeySequence("="), [this] {
        if (m_page == Page::Color) m_colorPanel->gradeFromPrevious(1);
    });
    makeAction(colorMenu, "grade_prev2", T("Grade vom vorvorigen Clip übernehmen"), QKeySequence("-"), [this] {
        if (m_page == Page::Color) m_colorPanel->gradeFromPrevious(2);
    });
    colorMenu->addSeparator();
    // Windows of the Color page: choose the next layer (whole picture → window 1 → …), add one at the playhead clip
    makeAction(colorMenu, "color_next_layer", T("Nächstes Fenster wählen"), QKeySequence(), [this] {
        showPage(Page::Color);
        m_colorPanel->nextLayer();
    });
    makeAction(colorMenu, "color_add_window", T("Fenster hinzufügen"), QKeySequence(), [this] {
        showPage(Page::Color);
        m_colorPanel->addWindow("ellipse");
    });

    QMenu* workspace = menuBar()->addMenu(T("&Arbeitsbereich"));
    makeAction(workspace, "page_media", T("Media-Seite"), QKeySequence("Shift+2"), [this] { showPage(Page::Media); });
    makeAction(workspace, "page_edit", T("Edit-Seite"), QKeySequence("Shift+4"), [this] { showPage(Page::Edit); });
    makeAction(workspace, "page_color", T("Color-Seite"), QKeySequence("Shift+5"), [this] { showPage(Page::Color); });
    makeAction(workspace, "page_deliver", T("Deliver-Seite"), QKeySequence("Shift+8"), [this] { showPage(Page::Deliver); });
    makeAction(workspace, "toggle_effects", T("Effects Library ein/aus"), QKeySequence(),
               [this] { if (m_effectsToggle->isEnabled()) m_effectsToggle->toggle(); });
    makeAction(workspace, "toggle_media_storage", T("Media Storage ein/aus"), QKeySequence(),
               [this] { if (m_storageToggle->isEnabled()) m_storageToggle->toggle(); });
    makeAction(workspace, "toggle_mixer", T("Mixer ein/aus"), QKeySequence(),
               [this] { if (m_mixerToggle->isEnabled()) m_mixerToggle->toggle(); });
    makeAction(workspace, "toggle_transcript", T("Transkript ein/aus"), QKeySequence(),
               [this] { if (m_transcriptToggle->isEnabled()) m_transcriptToggle->toggle(); });
    // DaVinci: Workspace > Video Scopes (Ctrl+Shift+W); here the scope panel of the Color page
    makeAction(workspace, "toggle_scopes", T("Video-Scopes ein/aus"), QKeySequence("Ctrl+Shift+W"), [this] {
        m_scopes->setVisible(!m_scopes->isVisibleTo(m_colorBottom));
        QSettings().setValue("color/scopesVisible", m_scopes->isVisibleTo(m_colorBottom));
        if (m_scopes->isVisibleTo(m_colorBottom)) showPage(Page::Color);
    });
    // Panel sets of the Edit page and panel sizes (remembered per page, MainWindow::applySizes)
    QMenu* panels = workspace->addMenu(T("Panels"));
    makeAction(panels, "panels_standard", T("Standard (Media Pool, Inspector)"), QKeySequence(),
               [this] { applyPanelSet(true, false, false, false, false, true); });
    makeAction(panels, "panels_simple", T("Einfach (nur Media Pool)"), QKeySequence(),
               [this] { applyPanelSet(true, false, false, false, false, false); });
    makeAction(panels, "panels_transcript", T("Schnitt nach Text (mit Transkript)"), QKeySequence(),
               [this] { applyPanelSet(true, false, false, true, false, true); });
    makeAction(panels, "panels_audio", T("Ton (mit Mixer)"), QKeySequence(),
               [this] { applyPanelSet(true, true, false, false, true, true); });
    makeAction(panels, "panels_all", T("Alle Panels"), QKeySequence(),
               [this] { applyPanelSet(true, true, true, true, true, true); });
    panels->addSeparator();
    makeAction(panels, "layout_reset", T("Panelgrößen zurücksetzen"), QKeySequence(), [this] { resetSizes(); });
    makeAction(workspace, "extensions", T("Erweiterungen…"), QKeySequence(), [this] {
        ExtensionsDialog dlg(this);
        dlg.exec();
    });
    workspace->addSeparator();
    // Language like in DaVinci (Preferences → User → UI Settings): takes effect after a restart
    QMenu* langMenu = workspace->addMenu(T("Sprache"));
    auto* langGroup = new QActionGroup(langMenu);
    for (const I18n::Language& l : I18n::languages()) {
        QAction* a = langMenu->addAction(l.name);
        a->setCheckable(true);
        a->setChecked(I18n::language() == l.code);
        langGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, lang = l.code] {
            if (I18n::language() == lang) return;
            I18n::setLanguage(lang);
            // The hint already in the new language (it is not installed until the restart)
            offerRestart(I18n::translate(lang, N_("Die Sprache ändert sich nach dem Neustart von schneidi.")),
                         I18n::translate(lang, N_("Jetzt neu starten")), I18n::translate(lang, N_("Später")));
        });
    }
    // Design (UI colors): takes effect after restart, like the language
    makeAction(workspace, "design", T("Design…"), QKeySequence(), [this] {
        if (DesignDialog(this).exec() == QDialog::Accepted && Theme::restartNeeded())
            offerRestart(T("Das Design ändert sich nach dem Neustart von schneidi."), T("Jetzt neu starten"), T("Später"));
    });

    // Help: log file for error messages (Windows has no terminal)
    QMenu* help = menuBar()->addMenu(T("&Hilfe"));
    makeAction(help, "open_log_folder", T("Log-Ordner öffnen"), QKeySequence(),
               [] { QDesktopServices::openUrl(QUrl::fromLocalFile(Log::directory())); });

    InputBindings::instance().saveIfIncomplete();
}
