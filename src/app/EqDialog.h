// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QDialog>
#include <array>

class Editor;
class EqCurve;
class QCheckBox;
class QLineEdit;

// EQ of one audio track (Mixer: double click on "EQ"), like the Fairlight EQ window: on switch, the curve with
// draggable points and per band on/frequency/gain/Q to type. Changes apply at once (preview live, undo per change).
class EqDialog : public QDialog {
    Q_OBJECT
public:
    EqDialog(Editor* editor, int track, QWidget* parent = nullptr);

private:
    void sync(); // model -> controls
    void commit(int band); // fields of a band -> model

    Editor* m_editor;
    int m_track;
    QCheckBox* m_on;
    EqCurve* m_curve;
    struct BandRow {
        QCheckBox* on = nullptr;
        QLineEdit *freq = nullptr, *gain = nullptr, *q = nullptr;
    };
    std::array<BandRow, 6> m_bands;
};
