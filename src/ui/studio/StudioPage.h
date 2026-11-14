// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Studio page (0.23): sound gets its own room. Top left the viewer (small; MainWindow puts it into top() at index
// 0 when the page shows), top right the audio tracks with large waveforms and the pen for volume curves (StudioTracks),
// below the big mixer with EQ/compressor curves per track and the master's loudness (StudioMixer). Built on the
// page's first showing (MainWindow::ensureStudioPage), never at the start.

#include <QWidget>

class Editor;
class Engine;
class MediaCache;
class QComboBox;
class QSplitter;
class QToolButton;
class StudioMixer;
class StudioTracks;

class StudioPage : public QWidget {
    Q_OBJECT
public:
    StudioPage(Editor* editor, Engine* engine, MediaCache* cache, QWidget* parent = nullptr);

    QSplitter* root() const { return m_root; } // top row over the mixer
    QSplitter* top() const { return m_top; }   // viewer | tracks
    StudioTracks* tracks() const { return m_tracks; }
    StudioMixer* mixer() const { return m_mixer; }

private:
    QSplitter* m_root;
    QSplitter* m_top;
    StudioTracks* m_tracks;
    StudioMixer* m_mixer;
    QToolButton* m_select;
    QToolButton* m_pen;
    QComboBox* m_smoothing;
};
