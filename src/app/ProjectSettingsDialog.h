#pragma once
#include "core/ProjectFormat.h"

#include <QDialog>

class QComboBox;
class QSpinBox;
class QLabel;

// Project settings like DaVinci (File → Project Settings…, Shift+9), section "Master Settings":
// Timeline resolution (presets or custom size) and timeline frame rate. The frame rate is, as in DaVinci,
// locked as soon as there are clips in the timeline.
class ProjectSettingsDialog : public QDialog {
    Q_OBJECT
public:
    ProjectSettingsDialog(const ProjectFormat& current, bool rateLocked, QWidget* parent = nullptr);
    ProjectFormat format() const;

private:
    void presetChosen();
    void sizeEdited();

    QComboBox* m_resolution;
    QSpinBox* m_width;
    QSpinBox* m_height;
    QComboBox* m_rate;
    bool m_updating = false;
};
