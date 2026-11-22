// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "ui/timeline/PlayheadMove.h"
#include "ui/timeline/TimelineOptions.h"
#include "ui/timeline/ZoomAnchor.h"

#include <QPair>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QFormLayout;

// Settings › Timeline: how the timelines behave where editors differ (wheel zoom anchor, playback after a playhead
// move, ...). Each field shows the stored choice; store() writes them all (MainWindow::settingsDialog after OK; not in
// a test run of the app, which never writes the user's settings: there the tab has no effect). The
// timelines read the choices when they need them, so a change takes effect at once, no restart.
class TimelineSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit TimelineSettingsPage(QWidget* parent = nullptr);

    ZoomAnchor::Mode zoomAnchor() const;          // where wheel zoom stays put (ZoomAnchor::mode)
    PlayheadMove::Mode afterPlayheadMove() const; // playback after moving the playhead (PlayheadMove::mode)
    TimelineOptions::Follow follow() const;       // follow the playhead while playing
    bool spaceReturns() const;                    // Space stops and goes back to the start position
    bool clickSetsPlayhead() const;               // a click into the tracks sets the playhead too
    TimelineOptions::TimeFormat timeFormat() const; // how times are shown
    TimelineOptions::Thumbnails thumbnails() const; // pictures on video clips
    TimelineOptions::WaveShape waveShape() const;
    TimelineOptions::WaveScale waveScale() const;
    int snapTargets() const;                        // TimelineOptions::SnapTarget flags
    double stillSeconds() const;                    // default length of new stills
    double transitionSeconds() const;               // default length of new transitions
    TimelineOptions::Step bigStep() const;          // Shift+arrow
    TimelineOptions::Step bigNudge() const;         // Shift+, / Shift+.
    TimelineOptions::SnapReach snapReach() const;

    void store() const; // every choice into QSettings (only what changed)

private:
    QComboBox* m_zoomAnchor = nullptr;
    QComboBox* m_afterSeek = nullptr;
    QComboBox* m_follow = nullptr;
    QComboBox* m_space = nullptr;
    QComboBox* m_click = nullptr;
    QComboBox* m_timeFormat = nullptr;
    QComboBox* m_thumbnails = nullptr;
    QComboBox* m_waveShape = nullptr;
    QComboBox* m_waveScale = nullptr;
    QVector<QPair<int, QCheckBox*>> m_snapTargets;
    QComboBox* m_snapReach = nullptr;
    QDoubleSpinBox* m_stillSeconds = nullptr;
    QDoubleSpinBox* m_transitionSeconds = nullptr;
    QSpinBox* m_stepCount = nullptr;
    QComboBox* m_stepUnit = nullptr;
    QSpinBox* m_nudgeCount = nullptr;
    QComboBox* m_nudgeUnit = nullptr;
};
