#pragma once
#include "core/TrimFrames.h"

#include <QImage>
#include <QWidget>

class Engine;
class QLabel;
class QMimeData;
class QStackedWidget;
class TrimView;
class QToolButton;

// Preview window with transport bar (top center like in DaVinci).
// Shows timeline or source (Media Pool clip); scrubber under the image with In/Out range.
// Source: dragging the image into the timeline = overwrite source In/Out there (like DaVinci).
class Viewer : public QWidget {
    Q_OBJECT
public:
    // Drag from the Viewer: path as with the Media Pool (MediaPool::MimeType), plus the source range "in out"
    static constexpr const char* RangeMimeType = "application/x-schneidi-source-range";

    explicit Viewer(Engine* engine, QWidget* parent = nullptr);

    // Scrubber: length of the shown source/timeline and In/Out (-1 = not set)
    void setRange(int length, int markIn, int markOut);
    // Shown source (title above the image, drag into the timeline); empty = timeline
    void setSource(const QString& path, const QString& name);
    // Markers on the scrubber (source markers resp. timeline markers, frames)
    void setMarkers(const QVector<int>& markers);

    // Multicam viewer (ui/MulticamView) in place of the picture; shown while `on` (main window: multicam mode and
    // the engine not in the source view)
    void setMulticamView(QWidget* view);
    void showMulticam(bool on);
    bool multicamShown() const { return m_multicamOn; }

    // Trim view like DaVinci: while an edit is dragged, show the frames on both sides of it (two-up/four-up,
    // fetched in the background via Engine::stills()); endTrim() goes back to the normal picture
    void showTrim(const TrimFrames::View& view);
    void endTrim();
    bool trimShown() const;
    TrimView* trimView() const { return m_trim; }

    // On-screen controls over the picture (Color page windows); nullptr = none
    void setOverlay(class ViewerOverlay* overlay);

    // Hint bar above the picture (playback too slow: offer proxy media) with an optional button and a close button;
    // empty text = hidden
    void setHint(const QString& text, const QString& button = {});
    QString hint() const;

signals:
    void hintAccepted(); // the hint's button
    void hintClosed();   // the hint's close button

private:
    void updateTimecode(int frame);
    void updateModeText(); // "Timeline"/"Quelle – …", hint when the color correction is bypassed
    QMimeData* dragData() const;

    Engine* m_engine;
    class Screen* m_screen;
    TrimView* m_trim;
    QStackedWidget* m_stack;
    class Scrubber* m_scrubber;
    QLabel* m_mode;
    QWidget* m_hint;
    QLabel* m_hintLabel;
    QToolButton* m_hintButton;
    QLabel* m_timecode;
    QToolButton* m_playBtn;
    QWidget* m_multicam = nullptr;
    bool m_multicamOn = false;
    QString m_sourcePath, m_sourceName;
    int m_length = 0, m_in = -1, m_out = -1;
};
