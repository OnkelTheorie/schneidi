// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QDialog>

class DynCurve;
class Editor;
class QCheckBox;
class QLineEdit;

// Compressor of one audio track (Mixer: double click on "DYN"; Studio page: click on a strip's compressor curve):
// on switch, the static curve (threshold and ratio draggable, DynCurve), threshold, ratio, attack, release, knee, make-up. A value applies on Return/leaving the field (preview live, undo per change);
// values outside the range are clamped.
class DynamicsDialog : public QDialog {
    Q_OBJECT
public:
    DynamicsDialog(Editor* editor, int track, QWidget* parent = nullptr);

private:
    void sync();   // model -> controls
    void detach(); // another project or timeline: forget the track, drop typed text, close
    // Tracks added, removed or reordered (also by undo): the window follows its track, gone = detach; then sync
    void followTrack();
    void showTitle();
    void commit(); // controls -> model

    Editor* m_editor;
    int m_track;
    quint64 m_trackUid = 0; // Track::uid of m_track: followed when tracks are added, removed or reordered
    QCheckBox* m_on;
    DynCurve* m_curve;
    QLineEdit *m_threshold, *m_ratio, *m_attack, *m_release, *m_knee, *m_makeup;
};
