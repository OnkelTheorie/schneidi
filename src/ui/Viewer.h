// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/TrimFrames.h"

#include <QImage>
#include <QWidget>

class Engine;
class QAction;
class QHBoxLayout;
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

    // The timeline's picture is on another monitor (ui/SecondScreen, "main window shows source clips"): the viewer
    // becomes the source viewer - it shows source clips only, never the timeline (no duplicate of the monitor). Back
    // in the timeline it keeps the last source frame (paused; the engine has one preview, it plays the timeline onto
    // the monitor now); without one an empty page says where the program is, with a button to bring it back
    // (programBackWanted)
    void setProgramElsewhere(bool on);
    bool programElsewhere() const { return m_programAway; }
    // The empty page is shown (program elsewhere, no source clip opened since)
    bool programAwayPageShown() const;

    // Trim view like DaVinci: while an edit is dragged, show the frames on both sides of it (two-up/four-up,
    // fetched in the background via Engine::stills()); endTrim() goes back to the normal picture
    void showTrim(const TrimFrames::View& view);
    void endTrim();
    bool trimShown() const;
    TrimView* trimView() const { return m_trim; }

    // On-screen controls over the picture (Color page windows); nullptr = none
    void setOverlay(class ViewerOverlay* overlay);
    ViewerOverlay* overlay() const;

    // Hint bar above the picture (playback too slow: offer proxy media) with an optional button and a close button;
    // empty text = hidden
    void setHint(const QString& text, const QString& button = {});
    QString hint() const;
    // Voice-over recording runs: a red sign in the title row
    void setRecording(bool on);
    bool recording() const { return m_recording; }

    // Button for a checkable view action (large viewer, full screen) at the right end of the transport bar, beside
    // Before/After: a click triggers it, it shows the action's state
    enum class ViewIcon { Large, FullScreen };
    QToolButton* addViewAction(QAction* action, ViewIcon icon);
    // The picture shown last (the engine's preview frame)
    QImage picture() const;
    // Display zoom of the picture: FrameView::kFit (0) or a factor of the project frame (1 = 100 %); pure display,
    // the timeline is not touched. addZoomActions: menu button with the zoom steps in the transport bar
    void setZoom(double zoom);
    double zoom() const;
    void addZoomActions(const QList<QAction*>& actions);
    class FrameView* screen() const { return m_screen; }
    // Height of the whole viewer (bars included) at which the picture fills `width` (0 = no frame size known)
    int heightFor(int width) const;

signals:
    void hintAccepted(); // the hint's button
    void hintClosed();   // the hint's close button
    void zoomChanged(double zoom);
    // Zoomed to 100 % or more: the preview should come at the project size (Engine::setPreviewFullSize)
    void fullSizeWanted(bool full);
    void programBackWanted(); // the empty page's button (program elsewhere)

private:
    void updateTimecode(int frame);
    void updateModeText(); // "Timeline"/"Quelle – …", hint when the color correction is bypassed
    QMimeData* dragData() const;
    void updatePage();        // which page the stack shows (trim, multicam, picture, program-away page)
    bool keepsPicture() const; // program elsewhere and the engine on the timeline: the picture is not replaced

    Engine* m_engine;
    class FrameView* m_screen;
    TrimView* m_trim;
    QStackedWidget* m_stack;
    class Scrubber* m_scrubber;
    QLabel* m_mode;
    QWidget* m_hint;
    QLabel* m_hintLabel;
    QToolButton* m_hintButton;
    QLabel* m_timecode;
    QToolButton* m_playBtn;
    QHBoxLayout* m_barRight;        // Before/After and the view buttons
    class QSpacerItem* m_barBalance; // keeps the transport buttons centered (left: timecode, right: m_barRight)
    QWidget* m_multicam = nullptr;
    bool m_multicamOn = false;
    QWidget* m_awayPage;
    bool m_programAway = false;
    QString m_keptSource;        // program elsewhere: name of the source clip whose frame the picture still shows
    class ViewerOverlay* m_overlay = nullptr; // given by setOverlay; off the picture while it keeps a source frame
    QImage m_programFrame;       // the timeline's last frame: back on screen when the program comes home
    QString m_sourcePath, m_sourceName;
    int m_length = 0, m_in = -1, m_out = -1;
    bool m_recording = false;
};
