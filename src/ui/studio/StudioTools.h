// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Studio page (0.23 Block 2): the tools drawer under the lanes (closed by default, the page's "Werkzeuge" button
// opens it). A title row with the target track, below it the tools as a list on the left and the chosen one beside
// it (remembered: one click away once the drawer opens).
// - Ziel: one audio track (follows the selection); the clip tools act on the selected clips of that track, else on
//   all its clips.
// - "Stimme säubern": voice cleaning (RNNoise, effect voiceclean) on those clips and the EQ preset "Stimme" on the
//   track, one undo step (Editor::cleanVoice with voiceEq).
// - "Musik unter Sprache absenken": ducking of those clips under the other audible tracks or the tracks picked under
//   "Durch" (Editor::duck; amount and picked tracks remembered), "Aufheben" takes it off (Editor::unduck); the curve
//   shows in the lanes.
// - "Lautheit auf Ziel": the whole mix to YouTube -14 / Podcast -16 / Broadcast -23 LUFS through op mixer
//   target_lufs (the existing logic, master limiter on), one undo step; shows before -> after. It measures behind a
//   dialog with progress and cancel (core/TaskProgress.h); a cancel changes nothing.
// - "Aufnahme": voice-over from this page (the app's VoiceoverRecorder): track (auto or armed), input device (the
//   Sound menu's choice), record/stop, input level.
// - "Effektklänge": the effect sounds (EffectsLibrary::soundEntries) with search; double-click / Anhören plays it in
//   the player, "Einfügen" puts it at the playhead, dragging it onto a lane puts it there. The list follows the
//   own sounds' and the packs' folders.
// While the page is hidden, timeline changes are only noted; the combos and the target follow when it shows.

#include <QJsonObject>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

class Editor;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSlider;
class QToolButton;
class VoiceoverRecorder;
class StudioInputMeter;

class StudioTools : public QWidget {
    Q_OBJECT
public:
    // Runs a CLI/MCP command on the app's project as the user's own undo step (MainWindow's UserRunner)
    using Runner = std::function<QJsonObject(const QString& command, const QJsonObject& args, const QString& undoText)>;

    explicit StudioTools(Editor* editor, QWidget* parent = nullptr);

    void setRunner(const Runner& run) { m_run = run; }
    void setRecorder(VoiceoverRecorder* recorder);

    // The audio track the clip tools act on, and the clips (selected ones on it, else all its unlocked clips)
    int targetTrack() const { return m_target; }
    void setTargetTrack(int track);
    QVector<int> targetClips() const;
    // The tools in the list (0 = clean voice, 1 = ducking, 2 = loudness, 3 = recording, 4 = effect sounds)
    int toolCount() const;
    int currentTool() const;
    void setCurrentTool(int index);

    // The tools (their buttons call these; tests too). Each is one undo step; the result line of the card says
    // what happened.
    int cleanVoice();            // clips cleaned (0 = nothing to clean)
    int duck();                  // clips lowered (0 = none, or no sound on the other tracks)
    int unduck();                // clips whose ducking went
    bool loudnessToTarget();     // false = not run / failed (the line says why)
    double loudnessPreset() const; // LUFS of the chosen preset
    // The height the drawer needs to show the chosen tool whole at its width (title row included); less: it scrolls
    int neededHeight() const;

    // Ducking "Durch": the tracks picked to make the music duck (empty = all other audible tracks, the default;
    // remembered in the settings like the amount) and the tracks that duck it now
    void setDuckBy(const QVector<int>& tracks);
    QVector<int> duckBy() const { return m_duckBy; }
    QVector<int> duckTracks() const { return underTracks(); }

signals:
    void recordRequested(); // start or stop the voice-over (MainWindow::toggleVoiceover)
    void soundPreviewRequested(const QString& file);
    void closeRequested(); // the drawer's close button
    void placeSoundRequested(const QByteArray& sound); // at the playhead
    void loudnessTargetChanged(); // "Lautheit auf Ziel" set the setting audio/loudnessTarget
    void neededHeightChanged();   // the chosen tool needs another height (another tool, a result line, the width)

protected:
    void showEvent(QShowEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    // A tool: its button in the list (tab text, objectName studio_tab_<id>) and its page (studio_card_<id>, headed by
    // the title, else the tab text)
    void addTool(const QString& tab, const QString& id, QWidget* body, const QString& title = {});
    void syncTargets(); // track lists of the combos, target info (while hidden only marked: m_syncPending)
    bool m_syncPending = false;
    void syncRecorder();
    void fillSounds();
    QVector<int> underTracks() const; // what ducks the music: the picked tracks with clips, else the other audible ones
    QVector<int> pickedDuckTracks() const; // m_duckBy that exist here and are not the target
    QString duckByText() const;
    void fillDuckByMenu(class QMenu* menu);
    void filterSounds(const QString& text);
    void watchSounds();
    void fillInputs();
    void say(QLabel* line, const QString& text, bool bad = false);
    void keysBack(bool hadKeys); // after a progress dialog: the window active again if it had the keys (KeysWatch)

    Editor* m_editor;
    Runner m_run;
    QPointer<VoiceoverRecorder> m_recorder;
    int m_target = 0;
    class QVBoxLayout* m_list = nullptr;
    class QButtonGroup* m_tabs = nullptr;
    class QStackedWidget* m_stack = nullptr;
    class QScrollArea* m_scroll = nullptr;
    void fitInnerHeight(); // wrapped hints: the chosen tool as tall as its text at this width (the area scrolls)
    QWidget* m_head = nullptr;
    int m_lastNeeded = -1;
    QComboBox* m_targetBox = nullptr;
    QLabel* m_targetInfo = nullptr;
    QLabel* m_cleanLine = nullptr;
    QSlider* m_duckAmount = nullptr;
    QLabel* m_duckValue = nullptr;
    QLabel* m_duckLine = nullptr;
    QToolButton* m_unduck = nullptr;
    QToolButton* m_duckByButton = nullptr;
    QVector<int> m_duckBy;
    QComboBox* m_loudPreset = nullptr;
    QLabel* m_loudLine = nullptr;
    QComboBox* m_recordTrack = nullptr;
    QComboBox* m_recordInput = nullptr;
    QToolButton* m_record = nullptr;
    StudioInputMeter* m_input = nullptr;
    QLabel* m_recordLine = nullptr;
    QLineEdit* m_soundSearch = nullptr;
    QListWidget* m_sounds = nullptr;
    class QFileSystemWatcher* m_soundWatcher = nullptr;
    class QTimer* m_soundRescan = nullptr;
};
