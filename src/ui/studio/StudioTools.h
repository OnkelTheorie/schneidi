// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Studio page (0.23 Block 2): the tools column between the track strips and the master. It takes the room the
// strips leave (few tracks: a wide area, cards side by side; many tracks: one narrow column that scrolls).
// - Ziel: one audio track (follows the selection); the clip tools act on the selected clips of that track, else on
//   all its clips.
// - "Stimme säubern": voice cleaning (RNNoise, effect voiceclean) on those clips and the EQ preset "Stimme" on the
//   track, one undo step (Editor::cleanVoice with voiceEq).
// - "Musik unter Sprache absenken": ducking of those clips under the other audible tracks (Editor::duck, amount
//   remembered), "Aus" takes it off (Editor::unduck); the curve shows in the lanes.
// - "Lautheit auf Ziel": the whole mix to YouTube -14 / Podcast -16 / Broadcast -23 LUFS through op mixer
//   target_lufs (the existing logic, master limiter on), one undo step; shows before -> after.
// - "Aufnahme": voice-over from this page (the app's VoiceoverRecorder): track (auto or armed), record/stop, input level.
// - "Effektklänge": the effect sounds (EffectsLibrary::soundEntries) with search; double-click / Anhören plays it in
//   the player, "Einfügen" puts it at the playhead, dragging it onto a lane puts it there.

#include <QJsonObject>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

class Editor;
class QComboBox;
class QGridLayout;
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
    // Columns the cards are laid out in now (1-3, by width)
    int columns() const { return m_columns; }

    // The tools (their buttons call these; tests too). Each is one undo step; the result line of the card says
    // what happened.
    int cleanVoice();            // clips cleaned (0 = nothing to clean)
    int duck();                  // clips lowered (0 = none, or no sound on the other tracks)
    int unduck();                // clips whose ducking went
    bool loudnessToTarget();     // false = not run / failed (the line says why)
    double loudnessPreset() const; // LUFS of the chosen preset

    // Natural width of one card (font dependent)
    int cardWidth() const;

signals:
    void recordRequested(); // start or stop the voice-over (MainWindow::toggleVoiceover)
    void soundPreviewRequested(const QString& file);
    void placeSoundRequested(const QByteArray& sound); // at the playhead

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    QWidget* card(const QString& title, const QString& name, QWidget* body);
    void reflow();
    void syncTargets(); // track lists of the combos, target info
    void syncRecorder();
    void fillSounds();
    QVector<int> underTracks() const; // the other audible tracks with clips (what ducks the music)
    void say(QLabel* line, const QString& text, bool bad = false);

    Editor* m_editor;
    Runner m_run;
    QPointer<VoiceoverRecorder> m_recorder;
    int m_target = 0;
    int m_columns = 0;
    QGridLayout* m_grid = nullptr;
    class QScrollArea* m_scroll = nullptr;
    void fitInnerHeight(); // wrapped hints: the cards as tall as their text at this width (the area scrolls)
    QVector<QWidget*> m_cards;
    QComboBox* m_targetBox = nullptr;
    QLabel* m_targetInfo = nullptr;
    QLabel* m_cleanLine = nullptr;
    QSlider* m_duckAmount = nullptr;
    QLabel* m_duckValue = nullptr;
    QLabel* m_duckLine = nullptr;
    QToolButton* m_unduck = nullptr;
    QComboBox* m_loudPreset = nullptr;
    QLabel* m_loudLine = nullptr;
    QComboBox* m_recordTrack = nullptr;
    QToolButton* m_record = nullptr;
    StudioInputMeter* m_input = nullptr;
    QLabel* m_recordLine = nullptr;
    QLineEdit* m_soundSearch = nullptr;
    QListWidget* m_sounds = nullptr;
};
