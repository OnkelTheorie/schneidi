// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/Editor.h"

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;

// "Change Clip Speed" like DaVinci (Ctrl+R): speed in %, frames per second, duration,
// reverse, freeze frame, keep pitch and Ripple Sequence. Values of the first selected clip as default.
class ClipSpeedDialog : public QDialog {
    Q_OBJECT
public:
    // clipLength = current length of the clip (frames), fps = project frame rate
    ClipSpeedDialog(const Editor::Retime& current, int clipLength, double fps, QWidget* parent = nullptr);
    Editor::Retime retime() const;
    bool ripple() const;

private:
    void percentEdited();
    void fpsEdited();
    void updateDuration();

    QDoubleSpinBox* m_percent;
    QDoubleSpinBox* m_fps;
    QLabel* m_duration;
    QCheckBox* m_reverse;
    QCheckBox* m_freeze;
    QCheckBox* m_pitch;
    QCheckBox* m_ripple;
    double m_projectFps;
    int m_clipLength;
    double m_baseLength; // length at 100 %
    bool m_updating = false;
};
