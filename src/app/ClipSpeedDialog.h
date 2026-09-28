#pragma once
#include "core/Editor.h"

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;

// „Clip-Geschwindigkeit ändern“ wie DaVinci (Strg+R): Geschwindigkeit in %, Bilder pro Sekunde, Dauer,
// Rückwärts, Standbild, Tonhöhe halten und Ripple Sequence. Werte des ersten ausgewählten Clips als Vorgabe.
class ClipSpeedDialog : public QDialog {
    Q_OBJECT
public:
    // clipLength = aktuelle Länge des Clips (Frames), fps = Projekt-Framerate
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
    double m_baseLength; // Länge bei 100 %
    bool m_updating = false;
};
