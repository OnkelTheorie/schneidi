// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QDialog>
#include <QVector>

#include "core/Types.h"
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
    void detach(); // another project or timeline: forget the track, drop typed text, close
    // Tracks added, removed or reordered (also by undo): the window follows its track, gone = detach; then sync
    void followTrack();
    void showTitle();
    void commit(int band); // fields of a band -> model

    Editor* m_editor;
    int m_track;
    QVector<Track> m_tracks; // the audio tracks as last seen (where m_track is)
    QCheckBox* m_on;
    EqCurve* m_curve;
    struct BandRow {
        QCheckBox* on = nullptr;
        QLineEdit *freq = nullptr, *gain = nullptr, *q = nullptr;
    };
    std::array<BandRow, 6> m_bands;
};
