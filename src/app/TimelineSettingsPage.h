// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "ui/timeline/PlayheadMove.h"
#include "ui/timeline/ZoomAnchor.h"

#include <QWidget>

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

    void store() const; // every choice into QSettings (only what changed)

private:
    QComboBox* m_zoomAnchor = nullptr;
    QComboBox* m_afterSeek = nullptr;
};
