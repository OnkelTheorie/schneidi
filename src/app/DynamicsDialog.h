// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QDialog>

class Editor;
class QCheckBox;
class QLineEdit;

// Compressor of one audio track (Mixer: double click on "DYN"), like Fairlight Dynamics: on switch, threshold,
// ratio, attack, release, knee, make-up. A value applies on Return/leaving the field (preview live, undo per change);
// values outside the range are clamped.
class DynamicsDialog : public QDialog {
    Q_OBJECT
public:
    DynamicsDialog(Editor* editor, int track, QWidget* parent = nullptr);

private:
    void sync();   // model -> controls
    void commit(); // controls -> model

    Editor* m_editor;
    int m_track;
    QCheckBox* m_on;
    QLineEdit *m_threshold, *m_ratio, *m_attack, *m_release, *m_knee, *m_makeup;
};
