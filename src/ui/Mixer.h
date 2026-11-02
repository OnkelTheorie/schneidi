// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

class Editor;
class Engine;
class Project;
class QHBoxLayout;
class QTimer;
class ChannelStrip;
class InputMeter;
class LoudnessStrip;
class QToolButton;
class VoiceoverRecorder;

// Mixer like DaVinci's Edit page (slimmed down): per audio track a channel strip with meter, fader, pan,
// mute/solo, EQ/DYN; on the right the master and a loudness meter (LUFS). Dragging a fader = one undo step.
class Mixer : public QWidget {
    Q_OBJECT
public:
    Mixer(Project* project, Engine* engine, QWidget* parent = nullptr);
    // EQ/compressor changes go through this editor (the main window's); without one the mixer makes its own
    void setEditor(Editor* editor);
    // Voice-over: "R" per track arms it, the record button in the title row asks to start/stop (recordRequested /
    // stopRequested: the main window starts it, it shows messages), the input level shows while recording
    void setRecorder(VoiceoverRecorder* recorder);

signals:
    void recordRequested();
    void stopRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* e) override; // double click on EQ/DYN opens the editor

private:
    Editor* editor();
    void sync();         // model -> channel strips (recreates them when the track count changed)
    void rebuildStrips(int count);
    void fitStrips();    // strip area width: whole channel strips only
    int fixedWidth() const; // master + loudness
    void growToFit();    // widen the mixer in its splitter so all strips fit
    void resizeEvent(class QResizeEvent* e) override;
    void showEvent(class QShowEvent* e) override;
    void onLevels(const QVector<float>& db);
    void tick();         // let levels decay, peak hold
    void syncArm();      // "R" buttons from the recorder

    Project* m_project;
    Engine* m_engine;
    Editor* m_editor = nullptr;
    QHBoxLayout* m_stripLayout = nullptr;
    class QScrollArea* m_stripScroll = nullptr;
    QVector<ChannelStrip*> m_strips;
    ChannelStrip* m_master = nullptr;
    LoudnessStrip* m_loudness = nullptr; // Loudness meter to the right of the master
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock, m_lastLevels;
    VoiceoverRecorder* m_recorder = nullptr;
    QToolButton* m_record = nullptr;
    QToolButton* m_cleanVoice = nullptr; // "Stimme säubern" on the armed (or target) audio track
    InputMeter* m_inputMeter = nullptr;
};
