// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Studio page (0.23): the audio tracks as tall lanes with large waveforms on a time axis, the playhead of the timeline
// and the clips' volume curves. Two tools: Select (click = playhead there and the clip selected, ruler = scrub) and
// Pen (draw the volume curve: a stroke becomes volume keyframes of the clips under it, one undo step per stroke,
// Editor::drawVolume / core/VolumeDraw.h). With Select a keyframe dot drags (time and level, one undo step per drag)
// and a right click on it deletes it. An effect sound dropped onto a lane goes in at that frame (soundDropRequested);
// a running voice-over take shows on its lane (setLiveTake).
//
// Core rule (docs/architektur.md): zoom and scroll are the lanes' own (ViewState) and change only by user input - the
// mouse wheel (the wheel settings of the timeline), the scrollbars, the Fit button. A model change never touches them;
// the scroll range grows with the project and never shrinks under the view. The first showing takes the timeline's
// zoom and scroll once (setView).

#include "ui/timeline/ViewState.h"

#include <QPointer>
#include <QVector>
#include <QWidget>

#include "core/VolumeDraw.h"

class Editor;
class MediaCache;
class QScrollBar;
struct Clip;
namespace Voiceover { class LiveTake; }

class StudioTracks : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Pen };

    StudioTracks(Editor* editor, MediaCache* cache, QWidget* parent = nullptr);

    void setPlayhead(int frame);
    int playhead() const { return m_playhead; }
    void setTool(Tool tool);
    Tool tool() const { return m_tool; }
    // Smoothing of a pen stroke: frames to each side of the moving average (0 = as drawn)
    void setSmoothing(int frames) { m_smoothing = std::max(0, frames); }
    int smoothing() const { return m_smoothing; }

    const ViewState& view() const { return m_view; }
    // Zoom and scroll as given (the first showing: the timeline's)
    void setView(double pxPerFrame, double leftFrame);
    // The whole project in view (the Fit button: user input)
    void fit();

    // Geometry (tests, tours): the lane of audio track `track` (its clip area, right of the header); x of a frame
    QRect laneRect(int track) const;
    double xOf(double frame) const;
    double frameAt(double x) const;
    int laneHeight() const { return m_laneH; }
    // Where volume keyframe `index` of clip `clipId` is drawn (tests); null = not drawn (off view, curve too dense)
    QPointF keyPoint(int clipId, int index) const;
    // The take being recorded and the track it lands on (nullptr = none)
    void setLiveTake(const Voiceover::LiveTake* take, int track);

    static constexpr int kHeaderW = 132;
    static constexpr int kRulerH = 24;
    static constexpr int kMinLaneH = 64;

signals:
    void seekRequested(int frame);
    void soundDropRequested(const QByteArray& sound, int frame); // EffectsLibrary::SoundMimeType data
    void toolChanged(StudioTracks::Tool tool);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void layoutLanes();      // lane height, scrollbar ranges (never a change of zoom or scroll)
    void syncScrollBars();
    int trackAt(int y) const; // -1 = none
    QRect headerRect(int track) const;
    QRect muteRect(int track) const;
    QRect soloRect(int track) const;
    QRect clipArea() const;   // right of the headers, below the ruler, above the scrollbar
    const Clip* clipAt(int track, double frame) const;
    double dbAtY(int track, int y) const;
    void addPenPoint(const QPoint& pos);
    void drawRuler(class QPainter& p);
    void drawLane(class QPainter& p, int track);
    void drawWaveform(class QPainter& p, const QRect& body, const Clip& c, const QColor& color);
    void drawVolumeLine(class QPainter& p, const QRect& body, const Clip& c);
    bool keysShown(const Clip& c) const; // the dots are drawn (room enough between them)
    struct KeyHit {
        int clipId = -1, index = -1;
    };
    KeyHit keyAt(const QPoint& pos) const;
    void dragKey(const QPoint& pos);

    Editor* m_editor;
    QPointer<MediaCache> m_cache;
    ViewState m_view;
    int m_scrollY = 0; // pixels through the lanes
    int m_laneH = 96;
    int m_playhead = 0;
    Tool m_tool = Tool::Select;
    int m_smoothing = 2;
    QScrollBar* m_hbar;
    QScrollBar* m_vbar;
    bool m_syncing = false; // scrollbars set from the view, not by the user

    enum class Drag { None, Scrub, Pen, Key };
    Drag m_drag = Drag::None;
    KeyHit m_key;          // the keyframe being dragged
    QString m_keyMerge;    // its drag's undo merge key
    int m_keyDrags = 0;
    const Voiceover::LiveTake* m_take = nullptr;
    int m_takeTrack = -1;
    int m_penTrack = -1;
    QVector<VolumeDraw::Point> m_stroke;
    QPoint m_hover{-1, -1};
};
