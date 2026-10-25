#pragma once
#include "core/TimelineOps.h"
#include "core/TrimFrames.h"
#include "core/Types.h"
#include "ui/timeline/ViewState.h"

#include <QHash>
#include <QSet>
#include <optional>
#include <QWidget>
#include <functional>

class Editor;
class MediaCache;
class QLineEdit;
class QMimeData;
class QTimer;

// Self-drawn timeline: ruler, track headers, clips, playhead.
// Scrollbars belong to the TimelinePanel; this class holds the ViewState.
class TimelineView : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Trim, Blade };

    static constexpr int kHeaderW = 180;
    static constexpr int kRulerH = 30;
    static constexpr int kSeparator = 6;
    static constexpr int kSubtitleTrackH = 34; // Subtitle track (fixed, narrow like DaVinci)
    // DaVinci "Insufficient handles": ask whether to trim the clips so a transition fits
    static bool confirmTrimForTransition(QWidget* parent);

    explicit TimelineView(Editor* editor, QWidget* parent = nullptr);

    const ViewState& view() const { return m_view; }
    Tool tool() const { return m_tool; }
    bool snapping() const { return m_snap; }
    int playhead() const { return m_playhead; }
    // Clips an AI just changed (live connection): they light up briefly
    void flashClips(const QVector<int>& ids);

    // For the scrollbars (scroll range is "endless" to the right)
    int visibleFrames() const;
    int scrollRangeFrames() const;
    int contentHeight() const;
    int viewportHeight() const { return height() - kRulerH; }

    void setLeftFrame(double frame);
    // Set zoom + scroll at once (timeline switch: restore the timeline's view)
    void setZoomAndScroll(double pxPerFrame, double leftFrame, int scrollY);
    void setScrollY(int y);
    void zoomBy(double factor);
    void zoomToFit();

    // Returns info on a (possibly not yet imported) file for the drop preview
    void setProbe(std::function<MediaInfo(const QString&)> probe) { m_probe = std::move(probe); }
    // Source for filmstrips and waveforms (optional)
    void setMediaCache(MediaCache* cache);
    // Render cache like DaVinci: thin bar at the top of the ruler, red = not yet cached, blue = cached
    // (done = already rendered part from the clip start)
    struct CacheSpan {
        int start = 0, end = 0; // timeline frames, end exclusive
        double done = 0;
    };
    void setRenderCacheSpans(const QVector<CacheSpan>& spans);

    // Retime controls like DaVinci (Retime Controls): bar at the top of the clip with speed per section and speed points
    void setRetimeControls(const QVector<int>& ids, bool on);
    bool retimeControlsShown(int clipId) const { return m_retimeClips.contains(clipId); }
    // Curve editor under the clip (only clips with keyframes); param = shown parameter (Count = first animated)
    void setCurveEditor(const QVector<int>& ids, bool on, AnimParam param = AnimParam::Count);
    bool curveEditorShown(int clipId) const;

public slots:
    void setPlayhead(int frame);
    void setTool(TimelineView::Tool tool);
    void setSnapping(bool on);
    // View like DaVinci's timeline view options: clip name resp. duration in the title bar
    void setShowClipNames(bool on);
    void setShowClipDurations(bool on);

signals:
    void seekRequested(int frame);
    void viewChanged();
    void toolChanged(TimelineView::Tool tool);
    void snappingChanged(bool on);
    // Files (from Media Pool or file manager) were dropped onto track index `track`
    void dropRequested(const QStringList& paths, int frame, int track);
    // Source range [in, out] from the Viewer dropped (overwrite like DaVinci)
    void rangeDropRequested(const QString& path, int in, int out, int frame, int track);
    // Right-click on a clip (it is selected then)
    void clipMenuRequested(const QPoint& globalPos);
    // Double-click on a subtitle: edit the text in the Inspector
    void subtitleEditRequested(int cueId);
    // Trim view (like DaVinci): while an edit is dragged the viewer shows the frames on both sides of it
    void trimFramesChanged(const TrimFrames::View& view);
    void trimFramesEnded();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    bool eventFilter(QObject* obj, QEvent* e) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void leaveEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    struct Row {
        TrackRef ref;
        int y; // widget coordinate (scroll already included)
        int h;
        int lane = 0;    // height at the bottom of the row for keyframe lane + curve editor (0 = nothing expanded)
        int keyLane = 0; // of that the keyframe lane (top), the rest is the curve editor
        int curve = 0;
    };
    enum class Drag { None, Scrub, MaybeMove, Move, Trim, TrimEdit, Volume, TransitionLength, Fade, Keyframe,
                      CueMaybeMove, CueMove, CueTrim, SpeedPoint, CurvePoint, CurveHandle,
                      TrackMaybeMove, TrackMove, Rubber };
    // Retime bar: speed point (point >= 0) or section (segment >= 0) under the mouse
    struct RetimeHit {
        int clipId = 0;
        int point = -1;
        int segment = -1;
    };
    QRect retimeBarRect(const QRect& clipRect) const;
    std::optional<RetimeHit> retimeHitAt(const QPoint& pos) const;
    void drawRetime(QPainter& p, const QRect& r, const Clip& c);
    void segmentMenu(const RetimeHit& hit, int frame, const QPoint& globalPos);
    void speedPointMenu(const RetimeHit& hit, const QPoint& globalPos);
    // Transition under the mouse; edge: -1/+1 = left/right edge (drag length), 0 = middle
    struct TransitionHit {
        TrackRef ref;
        TimelineOps::TransitionSpan span;
        int edge = 0;
    };
    struct EdgeHit {
        int clipId;
        TimelineOps::Edge edge;
    };

    // Trim mode (T) like DaVinci: Ripple at the edge, Roll exactly at the cut of two clips,
    // Slide on the clip's title bar, Slip on the rest of the clip
    struct TrimHit {
        TimelineOps::TrimKind kind;
        int clipId;
        TimelineOps::Edge edge;
    };
    std::optional<TrimHit> trimHitAt(const QPoint& pos) const;

    // Subtitle tracks (top, above the video tracks; highest index on top like with video)
    struct SubRow {
        int index;
        int y;
        int h;
    };
    QVector<SubRow> subRows() const;
    int subtitlesHeight() const; // all subtitle tracks including separator gap (0 = none)
    std::optional<SubRow> subRowAt(int y) const;
    QRect cueRect(const SubRow& row, const SubtitleCue& c) const;
    int cueAt(const QPoint& pos) const; // id or 0 (locked track: 0)
    std::optional<EdgeHit> cueEdgeAt(const QPoint& pos) const;
    QRect subNameRect(const SubRow& row) const;
    void drawSubtitleTracks(QPainter& p);
    void drawSubtitleHeaders(QPainter& p);
    void subtitleHeaderMenu(int index, const QPoint& globalPos);
    void subtitleTrackMenu(const QPoint& pos, const QPoint& globalPos); // Right-click into a subtitle track
    bool subtitlePress(QMouseEvent* e, const QPoint& pos); // true = handled
    void startSubtitleRename(int index);

    QVector<Row> rows() const;
    std::optional<Row> rowAt(int y) const;
    std::optional<Row> rowFor(TrackRef ref) const;
    int clipAt(const QPoint& pos) const;
    std::optional<EdgeHit> edgeAt(const QPoint& pos) const;
    QRect clipRect(const Row& row, const Clip& c) const;
    std::optional<TransitionHit> transitionAt(const QPoint& pos) const;
    QRect transitionRect(const Row& row, const TimelineOps::TransitionSpan& s) const;
    // Fade handles at the top clip corners (like DaVinci), only visible/grabbable when the mouse is over the clip
    QRect fadeHandleRect(const QRect& clip, const Clip& c, TimelineOps::Edge edge) const;
    std::optional<EdgeHit> fadeHandleAt(const QPoint& pos) const;
    // Keyframes: icon at the bottom right in the clip (opens the keyframe lane) and the lane below (like DaVinci)
    QRect keyIconRect(const QRect& clip) const;
    int keyIconAt(const QPoint& pos) const; // clip ID or 0
    QRect laneRect(const Row& row, const Clip& c) const;
    struct KeyHit {
        int clipId;
        int t; // Clip-Frame
    };
    std::optional<KeyHit> keyframeAt(const QPoint& pos) const;
    bool inLane(const QPoint& pos) const; // Mouse in an expanded keyframe lane
    void drawKeyLane(QPainter& p, const Row& row, const Clip& c);
    int keyLaneTop(const Row& row) const { return row.y + row.h - row.lane - 2; }

    // Curve editor like DaVinci (curve icon left of the diamond): area under the clip with the curve
    // of a parameter; drag points (time + value, Shift = one axis), Bezier handles (Alt = split),
    // Double-click = keyframe, right-click = interpolation/delete. Implementation in TimelineCurves.cpp.
    struct CurveHit {
        int clipId = 0;
        AnimParam param = AnimParam::Count;
        int index = -1; // Keyframe in the KeyTrack
        int part = 0;   // 0 = point, -1 = handle in, 1 = handle out, 2 = parameter selector
    };
    struct CurveRange {
        double lo = 0, hi = 1;
    };
    std::optional<AnimParam> curveParam(const Clip& c) const; // shown parameter (nullopt = closed)
    QRect curveIconRect(const QRect& clip) const;
    int curveIconAt(const QPoint& pos) const;
    QRect curveRect(const Row& row, const Clip& c) const;
    QRect curvePlotRect(const QRect& curve) const;
    QRect curveChipRect(const QRect& curve, const Clip& c) const;
    CurveRange curveRange(const Clip& c, AnimParam p) const;
    double curveY(const QRect& plot, const CurveRange& r, double v) const;
    double curveValue(const QRect& plot, const CurveRange& r, double y) const;
    std::optional<CurveHit> curveHitAt(const QPoint& pos) const;
    bool inCurve(const QPoint& pos, int* clipId = nullptr) const;
    void drawCurveLane(QPainter& p, const Row& row, const Clip& c);
    void drawCurveIcon(QPainter& p, const QRect& clip, bool open);
    bool curvePress(QMouseEvent* e, const QPoint& pos);   // true = handled
    void curveMove(const QPoint& pos, Qt::KeyboardModifiers mods);
    void curveRelease();
    bool curveContextMenu(const QPoint& pos, const QPoint& globalPos);
    bool curveDoubleClick(const QPoint& pos);
    void curveParamMenu(int clipId, const QPoint& globalPos);
    // Snapping for keyframes (clip frames): playhead, clip boundaries, other keyframes; correction or 0
    int keySnapDelta(const Clip& c, const QVector<int>& times, const QSet<int>& exclude) const;
    // Audio clip whose volume line lies under the mouse (0 = none)
    int volumeLineAt(const QPoint& pos) const;
    void updateHoverCursor(const QPoint& pos);
    double frameToX(double frame) const;
    double xToFrame(double x) const;
    QVector<int> snapPoints(const QSet<int>& exclude) const;
    int snapDelta(const QVector<int>& edges, const QSet<int>& exclude, bool* hit = nullptr) const;

    void drawRuler(QPainter& p);
    void drawTracks(QPainter& p);
    // spans: audio transitions of the track (waveform follows the crossfade); color: track color (invalid = default)
    void drawClip(QPainter& p, const QRect& r, const Clip& c, TrackKind kind, bool selected, bool ghost,
                  const QVector<TimelineOps::TransitionSpan>& spans = {}, const QColor& color = {});
    void drawTransitions(QPainter& p, const Row& row, const QSet<int>& hiddenClips);
    void drawFilmstrip(QPainter& p, const QRect& body, const Clip& c);
    void drawWaveform(QPainter& p, const QRect& body, const Clip& c, const QVector<TimelineOps::TransitionSpan>& spans,
                      const QColor& color);
    void drawHeaders(QPainter& p);
    void drawPlayhead(QPainter& p);
    void drawLabel(QPainter& p, const QPoint& topLeft, const QString& text);
    QRect headerButton(const Row& row) const;
    // Track header like DaVinci: label "V1", name (double-click = rename), lock, eye/M
    QRect lockButton(const Row& row) const;
    QRect nameRect(const Row& row) const;
    QRect shortNameRect(const Row& row) const; // label "V1" = target track button
    bool isLocked(const Row& row) const; // locked track: nothing on it grabbable
    void headerMenu(const Row& row, const QPoint& globalPos);
    void emptyAreaMenu(const QPoint& globalPos);
    // Drag a track by its header (reorder within its type): index of the target track for mouse y
    int trackDropIndex(int y) const;
    // Selection frame (left-click on empty spot + drag): all touched clips/subtitles, Ctrl/Shift = add
    void startRubber(Qt::KeyboardModifiers mods);
    void updateRubber(const QPoint& pos);
    void drawRubber(QPainter& p);
    // Auto-scroll when dragging beyond the edge (counts as user input): timer runs as long as the mouse is in the edge zone
    void dragMove(const QPoint& pos, Qt::KeyboardModifiers mods);
    void updateAutoScroll(const QPoint& pos);
    void autoScrollStep();
    void drawTrackDrop(QPainter& p);
    void startRename(TrackRef ref);
    void finishRename(bool commit);
    QStringList dropPaths(const QMimeData* mime) const;
    // Source range from the Viewer ("in out"), false = normal drag from Media Pool/file manager
    static bool dropRange(const QMimeData* mime, int* in, int* out);
    int dropTrackAt(int y) const;

    Editor* m_editor;
    MediaCache* m_cache = nullptr;
    QVector<CacheSpan> m_cacheSpans;
    ViewState m_view;
    Tool m_tool = Tool::Select;
    bool m_snap = true;
    bool m_showNames = true;
    bool m_showDurations = false;
    int m_playhead = 0;

    Drag m_drag = Drag::None;
    QSet<int> m_retimeClips; // clips with visible retime controls
    RetimeHit m_rampDrag;    // dragged speed point
    int m_rampDelta = 0;     // timeline frames while dragging
    QPoint m_pressPos;
    int m_scrubFrame = -1; // last jump target while dragging in the ruler (do not jump to the same frame again)
    TrackRef m_anchorRef;
    QVector<int> m_dragIds;
    int m_dragDelta = 0;
    int m_dragFine = 0; // Shift in 1/100 frame (audio clips fine, otherwise m_dragDelta * 100)
    int m_dragAnchorId = 0;
    int m_dragTrackDelta = 0;

    // Trimming (drag edge)
    EdgeHit m_trim{0, TimelineOps::Edge::Start};
    QVector<int> m_trimIds;
    int m_trimDelta = 0;

    // Trim mode: running edit, delta and preview of the timeline
    TimelineOps::TrimEdit m_trimEdit;
    TrimHit m_trimHit{TimelineOps::TrimKind::Ripple, 0, TimelineOps::Edge::End};
    int m_trimEditDelta = 0;
    std::optional<Timeline> m_trimPreview;
    // Trim view: last emitted frames (only changes are sent); shown = trimFramesChanged sent since the press
    TrimFrames::View m_trimFrames;
    bool m_trimFramesShown = false;
    void updateTrimFrames(const Timeline& preview, const TimelineOps::TrimEdit& edit, int delta);
    void endTrimFrames();

    // Drag volume line (like DaVinci); Shift = fine
    int m_volClipId = 0;
    double m_volStartDb = 0;
    double m_volDb = 0;
    bool m_volFine = false;
    int m_hoverVolClip = 0;

    // Drag transition length (centered crossfade grows on both sides)
    TimelineOps::TransitionSpan m_transSpan;
    int m_transEdge = 0;

    int m_hoverFrame = -1; // Blade preview

    // Keyframe lanes: expanded clips (view state, not in the project) and dragging diamonds
    QSet<int> m_keyLanes;
    int m_keyDragClip = 0;
    int m_keyDelta = 0;

    // Curve editor: expanded clips with chosen parameter; dragging (state at press, range frozen)
    QHash<int, AnimParam> m_curves;
    CurveHit m_curveDrag;
    KeyTrack m_curveOrig;
    QSet<int> m_curveOrigTimes;
    CurveRange m_curveFrozen;
    bool m_curveFreeze = false;
    int m_curveDragDt = 0;     // current offset (frames) for display
    double m_curveDragDv = 0;

    // Drag fade handle
    int m_hoverClip = 0; // clip under the mouse (shows the fade handles)
    EdgeHit m_fade{0, TimelineOps::Edge::Start};
    int m_fadeStart = 0;

    // Preview when dragging in (Media Pool or file manager)
    struct DropItem {
        int length;
        bool video;
        int audio;      // Number of audio streams (one track each from A[n], see Editor::streamTrackStart)
        MediaInfo info; // files only
    };
    std::function<MediaInfo(const QString&)> m_probe;
    QVector<DropItem> m_dropItems;
    int m_dropFrame = -1;
    int m_dropTrack = 0;

    // Drag subtitles: entries, track offset (anchor = grabbed track), edge
    QVector<int> m_cueIds;
    int m_cueAnchor = 0;
    int m_cueTrackDelta = 0;
    int m_cueDelta = 0;
    EdgeHit m_cueTrim{0, TimelineOps::Edge::Start};

    // Selection frame: corner = m_pressPos (moves along with auto-scroll), selection before dragging (Ctrl/Shift)
    QPoint m_rubberPos;
    QSet<int> m_rubberBase;
    // Auto-scroll: last mouse position/keys while dragging
    QTimer* m_autoScroll = nullptr;
    QSet<int> m_flash;              // flashClips
    QTimer* m_flashTimer = nullptr;
    qint64 m_flashStart = 0;        // ms (QDateTime::currentMSecsSinceEpoch)
    static constexpr int kFlashMs = 1400;
    QPoint m_dragPos;
    Qt::KeyboardModifiers m_dragMods;

    // Drag track: grabbed track and target track (-1 = none yet)
    TrackRef m_trackDragRef;
    int m_trackDropTo = -1;

    // Rename track (input field over the name in the track header)
    QLineEdit* m_nameEdit = nullptr;
    TrackRef m_nameRef;
    int m_nameSub = -1; // >= 0: subtitle track is being renamed
    bool m_renaming = false;

    // Drag in a transition from the Effects Library (like DaVinci): highlight the cut under the mouse
    struct TransitionDrop {
        TrackRef ref;
        int leftId = 0, rightId = 0; // 0 = none (fade-in/out)
        TransitionStyle style;       // Alignment depending on the mouse position relative to the cut
        TimelineOps::TransitionSpan span;
        bool trim = false; // fits only after trimming the clips (no spare material at the cut)
    };
    std::optional<TransitionDrop> transitionDropAt(const QPoint& pos) const;
    bool m_transDragging = false;
    TrackKind m_transDragKind = TrackKind::Video;
    TransitionStyle m_transDragStyle;
    std::optional<TransitionDrop> m_transDrop;
    // Drag a filter (Open FX) from the Effects Library onto a video clip
    QString m_fxDragging;  // effect ID, empty = no filter drag
    int m_fxDropClip = 0;  // video clip under the mouse
    int fxDropClipAt(const QPoint& pos) const;
};
