// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Studio page (0.23): the big mixer. Per audio track a wide strip: name, the EQ curve (bands draggable, EqCurve) and
// the compressor's curve (threshold and ratio draggable, DynCurve) with their on switches, pan, mute/solo, peak value,
// fader with level meters; on the right the master (fader, meters, limiter) and the loudness (momentary, short-term,
// integrated, range, true peak) live. Every drag is one undo step (same merge key, closed on release).
//
// Channels: the meters show as many bars as the mix has channels. Engine::audioLevels sends one value per channel for
// every track and the master ([A1 ch1..chN, A2 …, master ch1..chN]), so the count is levels / (tracks + 1); until the
// first levels arrive the strips show a stereo pair.

#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

class Editor;
class Engine;
class Project;
class QHBoxLayout;
class QScrollArea;
class QTimer;
class StudioStrip;
class StudioLoudness;

class StudioMixer : public QWidget {
    Q_OBJECT
public:
    StudioMixer(Editor* editor, Engine* engine, QWidget* parent = nullptr);

    int channels() const { return m_channels; } // of the mix, as the last levels told
    int stripCount() const { return int(m_strips.size()); }
    // Bars the meter of track `track` shows (-1 = the master)
    int meterChannels(int track) const;
    // Levels as Engine::audioLevels sends them (tests feed them directly)
    void onLevels(const QVector<float>& db);

    // The tools column (StudioTools) between the strips and the master: it takes the room the strips leave, the
    // strips never get wider than they need (few tracks: no gap before the master)
    void setTools(QWidget* tools);

    static constexpr int kStripW = 150;

protected:
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void sync(); // model -> strips (rebuilt when the track count changed)
    void rebuild(int count);
    void tick(); // meters fall, peak hold, loudness reading
    void openEditor(int track, bool eq);
    void fitHeight(); // curves only where there is room for them
    void fitStripsWidth(); // the strips' area no wider than its strips (the tools take the rest)

    Editor* m_editor;
    Project* m_project;
    Engine* m_engine;
    QHBoxLayout* m_stripLayout = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_tools = nullptr;
    QVector<StudioStrip*> m_strips;
    StudioStrip* m_master = nullptr;
    StudioLoudness* m_loudness = nullptr;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock, m_lastLevels;
    int m_channels = 2; // until the first levels tell the mix's count
};
