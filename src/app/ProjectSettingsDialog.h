// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/ProjectFormat.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QSpinBox;
class QLabel;

// Project settings (File → Project Settings…, Shift+9): timeline resolution (presets or custom size), timeline frame
// rate and the sound channels of the mix (stereo, 5.1, 7.1). The frame rate is locked as soon as there are clips in
// the timeline. Whether the first clip in an empty timeline may still change the
// resolution / frame rate (off = fixed, e.g. from a start template).
class ProjectSettingsDialog : public QDialog {
    Q_OBJECT
public:
    ProjectSettingsDialog(const ProjectFormat& current, bool rateLocked, QWidget* parent = nullptr);
    ProjectFormat format() const;
    void setFixed(bool resolution, bool frameRate);
    bool fixedResolution() const;
    bool fixedFrameRate() const;

private:
    void presetChosen();
    void sizeEdited();

    QComboBox* m_resolution;
    QSpinBox* m_width;
    QSpinBox* m_height;
    QComboBox* m_rate;
    QComboBox* m_channels;
    QCheckBox* m_clipResolution;
    QCheckBox* m_clipRate;
    ProjectFormat m_current;
    bool m_updating = false;
};
