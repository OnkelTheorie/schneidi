// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Studio page (0.23): the audio tracks as tall lanes with large waveforms on a time axis, the playhead of the timeline,
// the clips' volume curves and the tracks' volume automation. Two tools: Select (click = playhead there and the clip
// selected, ruler = scrub) and Pen (draw the volume curve, one undo step per stroke). The pen writes either the track's
// automation (PenTarget::Track, the default: a curve over the timeline's time, gaps too, Editor::drawTrackVolume) or
// volume keyframes of the clips under the stroke (PenTarget::Clip, Editor::drawVolume; core/VolumeDraw.h). With Select a
// keyframe or automation dot drags (time and level, one undo step per drag) and a right click on it deletes it. An effect sound dropped onto a lane goes in at that frame (soundDropRequested);
// a running voice-over take shows on its lane (setLiveTake).
//
// Core rule (docs/architektur.md): zoom and scroll are the lanes' own (ViewState) and change only by user input - the
// mouse wheel (the wheel settings of the timeline), the scrollbars, the Fit button. A model change never touches them;
// the scroll range grows with the project and never shrinks under the view. The first showing takes the timeline's
// zoom and scroll once (setView). The lanes fill the height (a few tracks: tall lanes, many: they scroll); the
// track-height wheel (Shift+wheel by default) gives them a height of their own.

#include "ui/timeline/ViewState.h"

#include <QPointer>
#include <QRegion>
#include <QVector>
#include <QWidget>

#include "core/Types.h"
#include "core/VolumeDraw.h"

class Editor;
class MediaCache;
class QScrollBar;
struct Clip;
struct Keyframe;
namespace Voiceover { class LiveTake; }

class StudioTracks : public QWidget {
    Q_OBJECT
public:
    // Select: click = playhead + clip; Points (the page's default, rubber band): a click on the volume line sets a
    // point there, a point drags, right click deletes, double click / Alt+click puts it at 0 dB; Pen: free-hand strokes
    enum class Tool { Select, Points, Pen };
    enum class PenTarget { Clip, Track };

    StudioTracks(Editor* editor, MediaCache* cache, QWidget* parent = nullptr);

    void setPlayhead(int frame);
    int playhead() const { return m_playhead; }
    void setTool(Tool tool);
    Tool tool() const { return m_tool; }
    // Smoothing of a pen stroke: frames to each side of the moving average (0 = as drawn)
    void setSmoothing(int frames) { m_smoothing = std::max(0, frames); }
    int smoothing() const { return m_smoothing; }
    // What the pen writes: the track's automation or the clips' volume keyframes
    void setPenTarget(PenTarget target);
    PenTarget penTarget() const { return m_penTarget; }
    // Which curve of the track Points and Pen edit in track mode (volume or pan; clip mode: always volume)
    void setAutoTarget(AutoTarget target);
    AutoTarget autoTarget() const { return m_autoTarget; }

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
    // The user's lane height (the track-height wheel); 0 = the lanes fill the room (the default)
    void setLaneHeight(int px);
    int userLaneHeight() const { return m_userLaneH; }
    // Where volume keyframe `index` of clip `clipId` is drawn (tests); null = not drawn (off view, curve too dense)
    QPointF keyPoint(int clipId, int index) const;
    // Where automation point `index` of audio track `track` is drawn (tests); null = not drawn
    QPointF autoPoint(int track, int index) const;
    // The take being recorded and the track it lands on (nullptr = none); a track past the last one (a new track)
    // gets a temporary lane below the others while it records
    void setLiveTake(const Voiceover::LiveTake* take, int track);
    int laneCount() const; // the audio tracks and that temporary lane
    // Tests: paint events so far and the part the last one drew
    int paintCount() const { return m_paints; }
    QRegion lastPaintRegion() const { return m_painted; }

    static constexpr int kHeaderW = 132;
    static constexpr int kRulerH = 24;
    static constexpr int kMinLaneH = 72;      // filling the room: never lower (more tracks scroll)
    static constexpr int kMaxAutoLaneH = 320; // filling the room: never taller
    static constexpr int kMinUserLaneH = 40, kMaxUserLaneH = 480;

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
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void updateChangedLanes(); // the model changed: repaint only the lanes whose track changed
    QVector<Track> m_shownAudio; // the audio tracks as last drawn (updateChangedLanes)
    void layoutLanes();      // lane height, scrollbar ranges (never a change of zoom or scroll)
    void syncScrollBars();
    int trackAt(int y) const; // -1 = none
    QRect headerRect(int track) const;
    QRect muteRect(int track) const;
    QRect soloRect(int track) const;
    QRect clipArea() const;   // right of the headers, below the ruler, above the scrollbar
    const Clip* clipAt(int track, double frame) const;
    double dbAtY(int track, int y) const;
    // The curve shown and edited (track mode: m_autoTarget, clip mode: volume); its value scale on a lane
    AutoTarget curveTarget() const { return m_penTarget == PenTarget::Track ? m_autoTarget : AutoTarget::Volume; }
    double posOfValue(double v) const; // 0 = bottom, 1 = top
    double valueAtY(int track, int y) const;
    QString valueText(double v) const;
    void addPenPoint(const QPoint& pos);
    // The drag ends (a release; also a move without the button or the lanes hidden mid-drag): the pen's stroke
    // becomes one undo step (addPoint: the release point too), a dragged point's merge closes
    void endDrag(const QPoint& pos, bool addPoint);
    void paintPart(class QPainter& p); // what lies in m_dirty
    void drawRuler(class QPainter& p);
    void drawLane(class QPainter& p, int track);
    void drawWaveform(class QPainter& p, const QRect& body, const Clip& c, const QColor& color);
    void drawVolumeLine(class QPainter& p, const QRect& body, const Clip& c);
    void drawAutomation(class QPainter& p, int track); // the track's level (fader + automation) over the whole lane
    bool autoShown(const Track& t) const; // its dots are drawn (room enough between them)
    bool keysShown(const Clip& c) const; // the dots are drawn (room enough between the keys inside the clip)
    static bool keyInside(const Clip& c, const Keyframe& k); // at a frame of the clip (0 <= t < length)
    QRect playheadRect(int frame) const;  // what the playhead at `frame` covers (line, triangle)
    bool takeOnNewTrack() const;
    void drawNewTrackLane(class QPainter& p, int track);
    struct KeyHit {
        int clipId = -1, index = -1;
        int track = -1; // an automation point of this audio track (clipId -1)
        bool valid() const { return clipId >= 0 || track >= 0; }
    };
    KeyHit keyAt(const QPoint& pos) const;
    void dragKey(const QPoint& pos);
    // Points: a point on the line under pos (the track's level line in track mode, the clip's volume line in clip
    // mode), already one undo step under m_keyMerge (the drag that follows joins it); invalid = not on the line
    KeyHit addPointAt(int track, const QPoint& pos);
    bool onLine(int track, const QPoint& pos) const;
    void resetPoint(const KeyHit& hit); // to 0 dB (the level heard)

    Editor* m_editor;
    QPointer<MediaCache> m_cache;
    ViewState m_view;
    int m_scrollY = 0; // pixels through the lanes
    int m_laneH = 96;
    int m_userLaneH = 0; // 0 = fill the room
    int m_playhead = 0;
    Tool m_tool = Tool::Select;
    int m_smoothing = 2;
    PenTarget m_penTarget = PenTarget::Track;
    AutoTarget m_autoTarget = AutoTarget::Volume;
    QScrollBar* m_hbar;
    QScrollBar* m_vbar;
    bool m_syncing = false; // scrollbars set from the view, not by the user

    enum class Drag { None, Scrub, Pen, Key };
    Drag m_drag = Drag::None;
    KeyHit m_key;          // the keyframe being dragged
    QString m_keyMerge;    // its drag's undo merge key
    KeyHit m_added;        // the point the last press set (a double click on it joins that undo step)
    int m_addedBefore = -1; // the undo index before it was set
    int m_keyDrags = 0;
    const Voiceover::LiveTake* m_take = nullptr;
    int m_takeTrack = -1;
    QRect m_takeRect; // where the take was drawn last (setLiveTake draws only what it grew by)
    QRect takeRect() const;
    int m_penTrack = -1;
    QVector<VolumeDraw::Point> m_stroke;
    QPoint m_hover{-1, -1};
    QRect m_dirty;    // the part being drawn
    QRegion m_painted; // what the last paint event drew (tests)
    int m_paints = 0; // paint events (tests)
};
