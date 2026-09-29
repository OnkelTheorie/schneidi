#pragma once
#include "core/TimelineOps.h"
#include "core/Types.h"
#include "ui/timeline/ViewState.h"

#include <QSet>
#include <optional>
#include <QWidget>
#include <functional>

class Editor;
class MediaCache;
class QLineEdit;
class QMimeData;

// Selbst gezeichnete Timeline: Lineal, Spurköpfe, Clips, Playhead.
// Scrollbars gehören dem TimelinePanel; diese Klasse hält den ViewState.
class TimelineView : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Trim, Blade };

    static constexpr int kHeaderW = 180;
    static constexpr int kRulerH = 30;
    static constexpr int kSeparator = 6;
    static constexpr int kSubtitleTrackH = 34; // Untertitelspur (fest, wie DaVinci schmal)

    explicit TimelineView(Editor* editor, QWidget* parent = nullptr);

    const ViewState& view() const { return m_view; }
    Tool tool() const { return m_tool; }
    bool snapping() const { return m_snap; }
    int playhead() const { return m_playhead; }

    // Für die Scrollbars (Scrollbereich ist nach rechts "endlos")
    int visibleFrames() const;
    int scrollRangeFrames() const;
    int contentHeight() const;
    int viewportHeight() const { return height() - kRulerH; }

    void setLeftFrame(double frame);
    void setScrollY(int y);
    void zoomBy(double factor);
    void zoomToFit();

    // Liefert Infos zu einer (evtl. noch nicht importierten) Datei für die Drop-Vorschau
    void setProbe(std::function<MediaInfo(const QString&)> probe) { m_probe = std::move(probe); }
    // Quelle für Filmstreifen und Wellenformen (optional)
    void setMediaCache(MediaCache* cache);
    // Render-Cache wie DaVinci: dünner Balken oben im Lineal, rot = noch nicht gecacht, blau = gecacht
    // (done = schon gerenderter Anteil vom Clip-Anfang)
    struct CacheSpan {
        int start = 0, end = 0; // Timeline-Frames, end exklusiv
        double done = 0;
    };
    void setRenderCacheSpans(const QVector<CacheSpan>& spans);

public slots:
    void setPlayhead(int frame);
    void setTool(TimelineView::Tool tool);
    void setSnapping(bool on);
    // Ansicht wie DaVincis Timeline-Ansichtsoptionen: Clipname bzw. Dauer in der Titelleiste
    void setShowClipNames(bool on);
    void setShowClipDurations(bool on);

signals:
    void seekRequested(int frame);
    void viewChanged();
    void toolChanged(TimelineView::Tool tool);
    void snappingChanged(bool on);
    // Dateien (aus Media Pool oder Dateimanager) wurden auf Spur-Index `track` abgelegt
    void dropRequested(const QStringList& paths, int frame, int track);
    // Quellbereich [in, out] aus dem Viewer abgelegt (überschreiben wie DaVinci)
    void rangeDropRequested(const QString& path, int in, int out, int frame, int track);
    // Rechtsklick auf einen Clip (ist dann ausgewählt)
    void clipMenuRequested(const QPoint& globalPos);
    // Doppelklick auf einen Untertitel: Text im Inspector bearbeiten
    void subtitleEditRequested(int cueId);

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
        int y; // Widget-Koordinate (Scroll bereits eingerechnet)
        int h;
        int lane = 0; // Höhe der aufgeklappten Keyframe-Spur unten in der Zeile (0 = keine)
    };
    enum class Drag { None, Scrub, MaybeMove, Move, Trim, TrimEdit, Volume, TransitionLength, Fade, Keyframe,
                      CueMaybeMove, CueMove, CueTrim };
    // Übergang unter der Maus; edge: -1/+1 = linke/rechte Kante (Länge ziehen), 0 = Mitte
    struct TransitionHit {
        TrackRef ref;
        TimelineOps::TransitionSpan span;
        int edge = 0;
    };
    struct EdgeHit {
        int clipId;
        TimelineOps::Edge edge;
    };

    // Trim-Modus (T) wie DaVinci: an der Kante Ripple, genau am Schnitt zweier Clips Roll,
    // Titelleiste des Clips Slide, übriger Clip Slip
    struct TrimHit {
        TimelineOps::TrimKind kind;
        int clipId;
        TimelineOps::Edge edge;
    };
    std::optional<TrimHit> trimHitAt(const QPoint& pos) const;

    // Untertitelspuren (oben, über den Videospuren; höchster Index oben wie bei Video)
    struct SubRow {
        int index;
        int y;
        int h;
    };
    QVector<SubRow> subRows() const;
    int subtitlesHeight() const; // alle Untertitelspuren samt Trennfuge (0 = keine)
    std::optional<SubRow> subRowAt(int y) const;
    QRect cueRect(const SubRow& row, const SubtitleCue& c) const;
    int cueAt(const QPoint& pos) const; // id oder 0 (gesperrte Spur: 0)
    std::optional<EdgeHit> cueEdgeAt(const QPoint& pos) const;
    QRect subNameRect(const SubRow& row) const;
    void drawSubtitleTracks(QPainter& p);
    void drawSubtitleHeaders(QPainter& p);
    void subtitleHeaderMenu(int index, const QPoint& globalPos);
    void subtitleTrackMenu(const QPoint& pos, const QPoint& globalPos); // Rechtsklick in eine Untertitelspur
    bool subtitlePress(QMouseEvent* e, const QPoint& pos); // true = erledigt
    void startSubtitleRename(int index);

    QVector<Row> rows() const;
    std::optional<Row> rowAt(int y) const;
    std::optional<Row> rowFor(TrackRef ref) const;
    int clipAt(const QPoint& pos) const;
    std::optional<EdgeHit> edgeAt(const QPoint& pos) const;
    QRect clipRect(const Row& row, const Clip& c) const;
    std::optional<TransitionHit> transitionAt(const QPoint& pos) const;
    QRect transitionRect(const Row& row, const TimelineOps::TransitionSpan& s) const;
    // Fade-Griffe oben an den Clip-Ecken (wie DaVinci), nur sichtbar/greifbar, wenn die Maus über dem Clip ist
    QRect fadeHandleRect(const QRect& clip, const Clip& c, TimelineOps::Edge edge) const;
    std::optional<EdgeHit> fadeHandleAt(const QPoint& pos) const;
    // Keyframes: Symbol unten rechts im Clip (klappt die Keyframe-Spur auf) und die Spur darunter (wie DaVinci)
    QRect keyIconRect(const QRect& clip) const;
    int keyIconAt(const QPoint& pos) const; // Clip-ID oder 0
    QRect laneRect(const Row& row, const Clip& c) const;
    struct KeyHit {
        int clipId;
        int t; // Clip-Frame
    };
    std::optional<KeyHit> keyframeAt(const QPoint& pos) const;
    bool inLane(const QPoint& pos) const; // Maus in einer aufgeklappten Keyframe-Spur
    void drawKeyLane(QPainter& p, const Row& row, const Clip& c);
    // Audioclip, dessen Lautstärkelinie unter der Maus liegt (0 = keiner)
    int volumeLineAt(const QPoint& pos) const;
    void updateHoverCursor(const QPoint& pos);
    double frameToX(double frame) const;
    double xToFrame(double x) const;
    QVector<int> snapPoints(const QSet<int>& exclude) const;
    int snapDelta(const QVector<int>& edges, const QSet<int>& exclude) const;

    void drawRuler(QPainter& p);
    void drawTracks(QPainter& p);
    // spans: Audio-Übergänge der Spur (Wellenform folgt dem Crossfade); color: Spurfarbe (ungültig = Standard)
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
    // Spurkopf wie DaVinci: Kürzel „V1“, Name (Doppelklick = umbenennen), Schloss, Auge/M
    QRect lockButton(const Row& row) const;
    QRect nameRect(const Row& row) const;
    QRect shortNameRect(const Row& row) const; // Kürzel „V1“ = Zielspur-Knopf
    bool isLocked(const Row& row) const; // gesperrte Spur: nichts darauf greifbar
    void headerMenu(const Row& row, const QPoint& globalPos);
    void startRename(TrackRef ref);
    void finishRename(bool commit);
    QStringList dropPaths(const QMimeData* mime) const;
    // Quellbereich aus dem Viewer ("in out"), false = normaler Drag aus Media Pool/Dateimanager
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
    QPoint m_pressPos;
    int m_scrubFrame = -1; // letztes Sprungziel beim Ziehen im Lineal (gleiches Frame nicht erneut anspringen)
    TrackRef m_anchorRef;
    QVector<int> m_dragIds;
    int m_dragDelta = 0;
    int m_dragTrackDelta = 0;

    // Trimmen (Kante ziehen)
    EdgeHit m_trim{0, TimelineOps::Edge::Start};
    QVector<int> m_trimIds;
    int m_trimDelta = 0;

    // Trim-Modus: laufende Bearbeitung, Delta und Vorschau der Timeline
    TimelineOps::TrimEdit m_trimEdit;
    TrimHit m_trimHit{TimelineOps::TrimKind::Ripple, 0, TimelineOps::Edge::End};
    int m_trimEditDelta = 0;
    std::optional<Timeline> m_trimPreview;

    // Lautstärkelinie ziehen (wie DaVinci); Shift = fein
    int m_volClipId = 0;
    double m_volStartDb = 0;
    double m_volDb = 0;
    bool m_volFine = false;
    int m_hoverVolClip = 0;

    // Übergangslänge ziehen (zentrierte Überblendung wächst an beiden Seiten)
    TimelineOps::TransitionSpan m_transSpan;
    int m_transEdge = 0;

    int m_hoverFrame = -1; // Klingen-Vorschau

    // Keyframe-Spuren: aufgeklappte Clips (Ansichtszustand, nicht im Projekt) und Rauten ziehen
    QSet<int> m_keyLanes;
    int m_keyDragClip = 0;
    int m_keyDelta = 0;

    // Fade-Griff ziehen
    int m_hoverClip = 0; // Clip unter der Maus (zeigt die Fade-Griffe)
    EdgeHit m_fade{0, TimelineOps::Edge::Start};
    int m_fadeStart = 0;

    // Vorschau beim Reinziehen (Media Pool oder Dateimanager)
    struct DropItem {
        int length;
        bool video;
        bool audio;
    };
    std::function<MediaInfo(const QString&)> m_probe;
    QVector<DropItem> m_dropItems;
    int m_dropFrame = -1;
    int m_dropTrack = 0;

    // Untertitel ziehen: Einträge, Spurversatz (Anker = gegriffene Spur), Kante
    QVector<int> m_cueIds;
    int m_cueAnchor = 0;
    int m_cueTrackDelta = 0;
    int m_cueDelta = 0;
    EdgeHit m_cueTrim{0, TimelineOps::Edge::Start};

    // Spur umbenennen (Eingabefeld über dem Namen im Spurkopf)
    QLineEdit* m_nameEdit = nullptr;
    TrackRef m_nameRef;
    int m_nameSub = -1; // >= 0: Untertitelspur wird umbenannt
    bool m_renaming = false;

    // Übergang aus der Effects Library reinziehen (wie DaVinci): Schnitt unter der Maus hervorheben
    struct TransitionDrop {
        TrackRef ref;
        int leftId = 0, rightId = 0; // 0 = keiner (Ein-/Ausblenden)
        TransitionStyle style;       // Ausrichtung je nach Mausposition zum Schnitt
        TimelineOps::TransitionSpan span;
    };
    std::optional<TransitionDrop> transitionDropAt(const QPoint& pos) const;
    bool m_transDragging = false;
    TrackKind m_transDragKind = TrackKind::Video;
    TransitionStyle m_transDragStyle;
    std::optional<TransitionDrop> m_transDrop;
    // Filter (Open FX) aus der Effects Library auf einen Videoclip ziehen
    QString m_fxDragging;  // Effekt-ID, leer = kein Filter-Drag
    int m_fxDropClip = 0;  // Videoclip unter der Maus
    int fxDropClipAt(const QPoint& pos) const;
};
