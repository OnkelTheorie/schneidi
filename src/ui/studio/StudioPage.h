// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The Studio page (0.23): sound gets its own room. The audio tracks take the main area, full height (left): the tool
// bar on top, the lanes with large waveforms and the curves (StudioTracks), under them the tools drawer (StudioTools:
// clean voice, ducking, loudness, recording, effect sounds; closed until the bar's "Werkzeuge" button opens it). On the
// right a column: the viewer small at its top (MainWindow puts it into side() at index 0 when the page shows), the
// loudness reading, then the mixer (StudioMixer: a narrow strip per track that scroll sideways, the master pinned at
// the right). Built on the page's first showing (MainWindow::ensureStudioPage), never at the start.

#include <QWidget>

class Editor;
class Engine;
class MediaCache;
class QComboBox;
class QSplitter;
class QToolButton;
class StudioMixer;
class StudioTools;
class StudioTracks;

class StudioPage : public QWidget {
    Q_OBJECT
public:
    StudioPage(Editor* editor, Engine* engine, MediaCache* cache, QWidget* parent = nullptr);

    QSplitter* root() const { return m_root; } // the lanes (with the tools) | the column on the right
    QSplitter* side() const { return m_side; } // the right column: viewer | loudness + mixer
    QSplitter* left() const { return m_left; } // lanes | tools drawer
    StudioTracks* tracks() const { return m_tracks; }
    StudioMixer* mixer() const { return m_mixer; }
    StudioTools* tools() const { return m_tools; }

    // The tools drawer (closed by default, not remembered; the tool chosen in it is)
    bool toolsOpen() const;
    void setToolsOpen(bool open);
    // Default sizes for a page of this size (the user's own sizes win, MainWindow::applySizes): the right column as
    // wide as a few strips and the master, the viewer as tall as fits above the mixer
    int defaultSideWidth(int pageWidth) const;
    int defaultViewerHeight(int sideWidth, int pageHeight) const;

private:
    Editor* m_editor;
    QSplitter* m_root;
    QSplitter* m_side;
    QSplitter* m_left;
    QWidget* m_sideBox;
    QToolButton* m_toolsButton;
    StudioTracks* m_tracks;
    StudioMixer* m_mixer;
    StudioTools* m_tools;
    QToolButton* m_select;
    QToolButton* m_points;
    QToolButton* m_pen;
    QComboBox* m_smoothing;
};
