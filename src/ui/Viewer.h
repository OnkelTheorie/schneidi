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

    // Trim view like DaVinci: while an edit is dragged, show the frames on both sides of it (two-up/four-up,
    // fetched in the background via Engine::stills()); endTrim() goes back to the normal picture
    void showTrim(const TrimFrames::View& view);
    void endTrim();
    bool trimShown() const;
    TrimView* trimView() const { return m_trim; }

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
    QLabel* m_timecode;
    QToolButton* m_playBtn;
    QString m_sourcePath, m_sourceName;
    int m_length = 0, m_in = -1, m_out = -1;
};
