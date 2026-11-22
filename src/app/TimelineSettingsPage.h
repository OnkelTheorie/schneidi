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
class QFormLayout;

// Settings › Timeline: how the timelines behave where editors differ (wheel zoom anchor, playback after a playhead
// move, ...). Each field shows the stored choice; store() writes them all (MainWindow::settingsDialog after OK). The
// timelines read the choices when they need them, so a change takes effect at once, no restart.
class TimelineSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit TimelineSettingsPage(QWidget* parent = nullptr);

    ZoomAnchor::Mode zoomAnchor() const;          // where wheel zoom stays put (ZoomAnchor::mode)
    PlayheadMove::Mode afterPlayheadMove() const; // playback after moving the playhead (PlayheadMove::mode)
    TimelineOptions::Follow follow() const;       // follow the playhead while playing
    bool clickSetsPlayhead() const;               // a click into the tracks sets the playhead too
    TimelineOptions::TimeFormat timeFormat() const; // how times are shown
    int snapTargets() const;                        // TimelineOptions::SnapTarget flags
    TimelineOptions::SnapReach snapReach() const;

    void store() const; // every choice into QSettings (only what changed)

private:
    QComboBox* m_zoomAnchor = nullptr;
    QComboBox* m_afterSeek = nullptr;
    QComboBox* m_follow = nullptr;
    QComboBox* m_click = nullptr;
    QComboBox* m_timeFormat = nullptr;
    QVector<QPair<int, QCheckBox*>> m_snapTargets;
    QComboBox* m_snapReach = nullptr;
};
