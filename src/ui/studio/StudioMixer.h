// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Studio page (0.23): the mixer column on the right of the page. Per audio track one narrow strip of a fixed width
// (kStripW; wider only when a large font needs it): name, small pictures of the EQ and the compressor curve (a click
// opens the full window with the big, draggable curve: EqDialog / DynamicsDialog; a click on their tag switches them
// on or off), pan, mute/solo with the pan value, peak, fader with level meters, the fader's value. Many tracks scroll
// sideways inside the column, the master (fader, meters, limiter) stays pinned at its right end. The loudness reading
// (momentary, short-term, integrated, range, true peak) belongs to the mixer but StudioPage places it (loudness()).
// Too little height: the curve pictures give way to two small buttons, the fader keeps its height. Every drag is one
// undo step (same merge key, closed on release).
//
// Channels: the meters show as many bars as the mix has channels. Engine::audioLevels sends one value per channel for
// every track and the master ([A1 ch1..chN, A2 …, master ch1..chN]), so the count is levels / (tracks + 1); until the
// first levels arrive the strips show a stereo pair. The levels come from Engine::audioSpeakerLevels: in stereo the
// master is true peak as everywhere, in 5.1/7.1 every speaker's sample peak with its name under the bar (L R C LFE …).

#include "core/AutomationWriter.h"

#include <QElapsedTimer>
#include <QStringList>
#include <QVector>
#include <QWidget>

class Editor;
class Engine;
class Project;
class QDialog;
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
    // Names under the bars of that meter: the master in 5.1/7.1 (ChannelLayout::label), else none
    QStringList meterLabels(int track) const;
    // Levels as Engine::audioLevels sends them (tests feed them directly)
    void onLevels(const QVector<float>& db);

    // The loudness reading (StudioPage puts it above the strips), the strips' scroll area, the master strip
    QWidget* loudness() const;
    class QScrollArea* stripsArea() const;
    QWidget* masterStrip() const;
    // Width of the first `strips` strips and the master (the page's default width of the column)
    int preferredWidth(int strips) const;
    // Strips showing their curve pictures now (the others show two small buttons: too little height)
    int curvesShown() const;
    // The EQ (eq) or compressor window of a track, as a click on its picture opens it (one per track and kind)
    QDialog* openEditor(int track, bool eq);

    // Write mode ("Schreiben", touch): while playing, moving a fader or pan knob writes into the track's automation,
    // on release it glides back to the curve; a whole pass (until playback stops) is one undo step. Off by default.
    void setWriteMode(bool on);
    bool writeMode() const { return m_writeMode; }
    AutomationWriter& writer() { return m_writer; }

    // The loudness target and true-peak limit (settings audio/loudnessTarget, audio/truePeakLimit) read again: on
    // showing and after "Lautheit auf Ziel" (StudioTools::loudnessTargetChanged); kept in between
    void refreshLoudnessSettings();

    static constexpr int kStripW = 76;

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    // model -> strips (rebuilt when the track count changed); while the page is hidden only marked (m_syncPending),
    // done when it shows
    void sync();
    bool m_syncPending = false;
    void rebuild(int count);
    void tick(); // meters fall, peak hold, loudness reading
    void fitHeight(); // curve pictures only where there is room for them
    bool m_fitting = false;

    Editor* m_editor;
    Project* m_project;
    Engine* m_engine;
    QHBoxLayout* m_stripLayout = nullptr;
    QScrollArea* m_scroll = nullptr;
    QVector<StudioStrip*> m_strips;
    StudioStrip* m_master = nullptr;
    StudioLoudness* m_loudness = nullptr;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock, m_lastLevels;
    int m_channels = 2; // until the first levels tell the mix's count
    double m_loudnessTarget = -14.0, m_truePeakLimit = -1.0; // refreshLoudnessSettings()
    bool m_writeMode = false;
    AutomationWriter m_writer;
    bool playing() const; // the timeline plays (not a file previewed in Source mode)
    // The timeline's playhead; in Source mode (a file previewed) the last one the timeline had
    int timelinePosition() const;
    mutable int m_lastTimelinePos = 0;
};
