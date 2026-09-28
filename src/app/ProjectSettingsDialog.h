#pragma once
#include "core/ProjectFormat.h"

#include <QDialog>

class QComboBox;
class QSpinBox;
class QLabel;

// Projekteinstellungen wie DaVinci (Datei → Projekteinstellungen…, Shift+9), Bereich "Master Settings":
// Timeline-Auflösung (Vorlagen oder eigene Größe) und Timeline-Framerate. Die Framerate ist wie in DaVinci
// gesperrt, sobald Clips in der Timeline liegen.
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
