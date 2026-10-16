#pragma once
// Multicam viewer like DaVinci (docs/plan-0.8.md): the grid of all angles of the multicam clip at the playhead
// (picture from the engine's grid tractor, see Engine::showMulticam), the active angle framed red, angle names,
// layout auto/2x2/3x3/4x4 with pages, modes video+audio / video / audio. Click into the grid: while playing = cut
// to that angle, while stopped = switch the segment's angle, Alt+click = cut. Knows no engine: the main window
// feeds position, playback state, the grid picture and the angles' proxy state and rebuilds the grid on gridChanged().

#include "core/Multicam.h"

#include <QImage>
#include <QWidget>

class Editor;
class Project;
class Selection;
class QComboBox;
class QLabel;
class QToolButton;

class MulticamView : public QWidget {
    Q_OBJECT
public:
    MulticamView(Project* project, Selection* selection, Editor* editor, QWidget* parent = nullptr);

    // Timeline playhead; picks the multicam clip there (Multicam::clipAt) and its active angle
    void setPosition(int frame);
    int position() const { return m_position; }
    void setPlaying(bool playing);
    bool playing() const { return m_playing; }
    // Picture of the grid tractor (cells as MulticamGrid::cell of the frame)
    void setFrame(const QImage& grid);

    void setMode(Multicam::Mode mode);
    Multicam::Mode mode() const { return m_mode; }
    // Layout: 0 = auto, 2..4 = fixed (MulticamGrid::columns)
    void setColumns(int columns);
    int columns() const { return m_columns; }
    void setPage(int page);
    int page() const { return m_page; }
    int pageCount() const;
    int perPage() const;

    // Multicam clip under the playhead (pointer into the project's timeline), nullptr = none
    const Clip* currentClip() const;
    int sequenceId() const { return m_sequence; }
    // Timeline frame minus the multicam sequence's frame shown there (where the grid tractor plays)
    int sequenceOffset() const { return m_offset; }
    int angleCount() const;
    QString angleName(int angle) const;
    // Angle shown framed: of the video part (audio mode: of the sound part); -1 = no multicam clip
    int activeAngle() const { return m_active; }
    // Angle whose sound plays in the grid: the one of the sound part (as in the edit), else the picture's
    int soundAngle() const { return m_sound; }
    // Hint instead of the grid (no multicam clip at the playhead); empty = grid shown
    QString message() const;
    // Area of the grid picture (aspect ratio of the project), cells as MulticamGrid::cell of it
    QRect gridRect() const;

    // More cells than this on a page play smoothly only with proxies: H.264 decodes only at full size, so 16 1080p
    // decoders are a slide show (docs/plan-0.8.md)
    static constexpr int kProxyHintCells = 4;
    // Media files on the angles' picture tracks (what proxies help with), each once
    QStringList angleFiles() const;
    // Proxy state of angleFiles(), fed by the main window: files without a proxy (not queued), files being generated
    void setProxyState(const QStringList& missing, int pending);
    // Hint above the grid (many cells without proxies, proxies being generated); empty = none
    QString proxyHint() const;

public slots:
    // Absolute angle (0-based); invalid angles or no multicam clip: nothing. Each is one undo step.
    void cutToAngle(int angle);
    void switchToAngle(int angle);

signals:
    // Multicam clip, its offset, its content or the page/layout changed: the grid tractor needs to follow
    void gridChanged();
    void soundAngleChanged(int angle);
    // "Generate proxies" in the hint: these files have none yet
    void proxiesRequested(const QStringList& files);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void refresh(); // state from the timeline at the playhead
    void updateControls();
    void updateProxyHint();
    void layoutBars();
    int barHeight() const; // control bar plus the proxy hint (if shown)

    Project* m_project;
    Selection* m_selection;
    Editor* m_editor;
    QWidget* m_bar;
    QToolButton* m_both;
    QToolButton* m_video;
    QToolButton* m_audio;
    QComboBox* m_layout;
    QToolButton* m_prev;
    QToolButton* m_next;
    QLabel* m_pageLabel;
    QWidget* m_hint;
    QLabel* m_hintLabel;
    QToolButton* m_hintButton;
    QStringList m_missingProxies;
    int m_pendingProxies = 0;
    QImage m_frame;
    Multicam::Mode m_mode = Multicam::Mode::Both;
    int m_position = 0;
    bool m_playing = false;
    int m_columns = 0;
    int m_page = 0;
    int m_sequence = 0;
    int m_offset = 0;
    int m_active = -1;
    int m_sound = -1;
};
