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
class QMimeData;

// Selbst gezeichnete Timeline: Lineal, Spurköpfe, Clips, Playhead.
// Scrollbars gehören dem TimelinePanel; diese Klasse hält den ViewState.
class TimelineView : public QWidget {
    Q_OBJECT
public:
    enum class Tool { Select, Blade };

    static constexpr int kHeaderW = 150;
    static constexpr int kRulerH = 30;
    static constexpr int kSeparator = 6;

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

public slots:
    void setPlayhead(int frame);
    void setTool(TimelineView::Tool tool);
    void setSnapping(bool on);

signals:
    void seekRequested(int frame);
    void viewChanged();
    void toolChanged(TimelineView::Tool tool);
    void snappingChanged(bool on);
    // Dateien (aus Media Pool oder Dateimanager) wurden auf Spur-Index `track` abgelegt
    void dropRequested(const QStringList& paths, int frame, int track);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
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
    };
    enum class Drag { None, Scrub, MaybeMove, Move, Trim, Volume };
    struct EdgeHit {
        int clipId;
        TimelineOps::Edge edge;
    };

    QVector<Row> rows() const;
    std::optional<Row> rowAt(int y) const;
    std::optional<Row> rowFor(TrackRef ref) const;
    int clipAt(const QPoint& pos) const;
    std::optional<EdgeHit> edgeAt(const QPoint& pos) const;
    QRect clipRect(const Row& row, const Clip& c) const;
    // Audioclip, dessen Lautstärkelinie unter der Maus liegt (0 = keiner)
    int volumeLineAt(const QPoint& pos) const;
    void updateHoverCursor(const QPoint& pos);
    double frameToX(double frame) const;
    double xToFrame(double x) const;
    QVector<int> snapPoints(const QSet<int>& exclude) const;
    int snapDelta(const QVector<int>& edges, const QSet<int>& exclude) const;

    void drawRuler(QPainter& p);
    void drawTracks(QPainter& p);
    void drawClip(QPainter& p, const QRect& r, const Clip& c, TrackKind kind, bool selected, bool ghost);
    void drawFilmstrip(QPainter& p, const QRect& body, const Clip& c);
    void drawWaveform(QPainter& p, const QRect& body, const Clip& c);
    void drawHeaders(QPainter& p);
    void drawPlayhead(QPainter& p);
    void drawLabel(QPainter& p, const QPoint& topLeft, const QString& text);
    QRect headerButton(const Row& row) const;
    QStringList dropPaths(const QMimeData* mime) const;
    int dropTrackAt(int y) const;

    Editor* m_editor;
    MediaCache* m_cache = nullptr;
    ViewState m_view;
    Tool m_tool = Tool::Select;
    bool m_snap = true;
    int m_playhead = 0;

    Drag m_drag = Drag::None;
    QPoint m_pressPos;
    TrackRef m_anchorRef;
    QVector<int> m_dragIds;
    int m_dragDelta = 0;
    int m_dragTrackDelta = 0;

    // Trimmen (Kante ziehen)
    EdgeHit m_trim{0, TimelineOps::Edge::Start};
    QVector<int> m_trimIds;
    int m_trimDelta = 0;

    // Lautstärkelinie ziehen (wie DaVinci); Shift = fein
    int m_volClipId = 0;
    double m_volStartDb = 0;
    double m_volDb = 0;
    bool m_volFine = false;
    int m_hoverVolClip = 0;

    int m_hoverFrame = -1; // Klingen-Vorschau

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
};
