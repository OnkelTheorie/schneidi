// SPDX-License-Identifier: AGPL-3.0-only
// The phone layout (small screens, docs/dev/touch.md › Handy-Layout): the main window's panels rearranged for a
// screen whose shorter side is a phone's. Portrait: top bar, viewer, timeline, a tool bar at the bottom; a tool opens
// its panel as a sheet over the timeline (one at a time). Landscape: the top bar's buttons and the tools as a rail at the
// left, the viewer above the timeline, the open panel at the right over the whole height. A selected clip switches the bar to clip tools (with a back arrow).
// The shell only arranges widgets it is given; MainWindow (MainWindowPhone.cpp) hands in its panels and actions.
// Nothing here is ever wider or taller than the shell: a panel larger than its room scrolls inside the sheet.
#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>
#include <functional>

class QAbstractButton;
class QBoxLayout;
class QLabel;
class QScrollArea;
class QToolButton;

class PhoneShell : public QWidget {
    Q_OBJECT
public:
    explicit PhoneShell(QWidget* parent = nullptr);

    // Whether a screen of `logical` size (device-independent pixels) is a phone's: its shorter side below kPhoneEdge.
    // Tablets, laptops and desktops keep the normal layout
    static constexpr int kPhoneEdge = 600;
    static bool isPhoneSize(const QSize& logical);

    void setTopBar(QWidget* bar);
    void setViewer(QWidget* viewer);
    void setTimeline(QWidget* timeline);
    QWidget* viewer() const { return m_viewer; }
    QWidget* timeline() const { return m_timeline; }

    // A tool of the main bar that opens a panel in the sheet; `make` gives the panel on its first opening (built
    // lazily, e.g. the color wheels) and the sheet keeps it from then on
    void addPanelTool(const QString& id, const QString& symbol, const QString& label, std::function<QWidget*()> make);
    // A tool that runs `run` (no panel of its own), in the clip bar (shown while clips are selected) or the main bar
    void addActionTool(const QString& id, const QString& symbol, const QString& label, std::function<void()> run,
                       bool clipBar = true);
    // A clip tool that opens a panel of the main bar (e.g. "Lautstärke" -> the Inspector); `opened` runs after the
    // sheet opened (e.g. the Inspector's Audio tab), with the panel's scroll at its top
    void addClipPanelTool(const QString& id, const QString& symbol, const QString& label, const QString& panelId,
                          std::function<void()> opened = {});
    QToolButton* toolButton(const QString& id) const; // tests

    // The sheet: one panel at a time; opening another closes the one before
    void openSheet(const QString& id);
    void closeSheet();
    QString sheet() const { return m_open; }               // empty = closed
    QWidget* sheetFrame() const { return m_sheet; }        // the sheet itself (header and panel)
    QWidget* panel(const QString& id) const;               // nullptr until it was opened once
    QString panelId(const QWidget* panel) const;           // the tool that shows `panel` (empty = none)
    void scrollPanelToTop(const QString& id);               // the sheet's own scroll of that panel back to its top
    // Height of the sheet in portrait as a share of the room above the tool bar (dragging its handle changes it)
    double sheetShare() const { return m_share; }
    void setSheetShare(double share); // the open panel's (each panel keeps its own height)
    // A panel's first height (e.g. the export settings with the queue's "Render all": tall); the user's drag wins
    void setPanelShare(const QString& id, double share);
    // Where a sheet without its own height starts (shell y): the timeline's video rows stay in sight above it, as
    // phone editors keep the clips in sight under a tool's panel. Empty = the usual share
    void setSheetTopHint(std::function<int()> top) { m_sheetTopHint = std::move(top); }
    // The timeline as tall as this, the viewer gets the rest. Landscape: its first video and audio rows (the picture
    // was ~70 px high at a fixed 1 : 2); portrait: all its rows (few tracks left empty lanes under the last one).
    // `most` = the tallest it may get (portrait: the old 5 : 4 share). Called again on refreshTimelineHeight (tracks
    // added, heights changed)
    void setTimelineHeightHint(std::function<int(int most)> height) { m_timelineHeight = std::move(height); }
    void refreshTimelineHeight();
    double defaultShare() const;

    // The clip bar instead of the main tools (MainWindow: while clips are selected)
    void setClipToolsShown(bool on);
    bool clipToolsShown() const { return m_clipMode; }

    // One step back (Android's back key): an open sheet closes, then the clip bar goes (clipToolsLeft); false = nothing
    // was open
    bool back();

    bool landscape() const { return m_landscape; } // the tools as a rail at the left (phone landscape, tablet)
    // The middle form (a tablet, an unfolded foldable, a narrow desktop window): the top bar across, the tools as a
    // rail at the left, viewer above timeline, one panel docked at the right (not a sheet over the timeline)
    void setTablet(bool on);
    bool tablet() const { return m_tablet; }
    // Gives the panels back (before the shell goes: MainWindow puts them where they were on the desktop)
    void releasePanels();

    // Before the shell is shown its panels report the desktop's minimums (polish and their first layout come with the
    // show): none then, the window is never sized to them (Android maximizes it before anything is laid out). Shown,
    // at most the window's shorter side in both directions (see the .cpp)
    QSize minimumSizeHint() const override;

signals:
    void sheetChanged(const QString& id); // opened (id) or closed (empty)
    void clipToolsLeft();                 // the clip bar's back arrow or the back key: MainWindow clears the selection
    // The arrangement changed (and on the first layout), before it is built: the top bar is the head of the rail
    // (true: a phone in landscape) or a row across (portrait, tablet); MainWindow arranges its buttons
    void landscapeChanged(bool barInRail);

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    struct Tool {
        QString id, label;
        std::function<QWidget*()> make;
        std::function<void()> run;
        QString panelId;
        QToolButton* button = nullptr;
        QScrollArea* scroll = nullptr; // the panel's place in the sheet (made on its first opening)
        bool clip = false;
    };
    QToolButton* makeToolButton(const QString& symbol, const QString& label);
    void relayout();      // the layout for the orientation (rebuilt when it changes)
    void placeSheet();    // portrait: the sheet's geometry over the timeline
    void updateButtons(); // checked = the open panel's tool; the bar's direction
    void tuneScroller();  // the bar's flick and bounce (once, after the first show)
    bool m_scrollTuned = false;
    Tool* tool(const QString& id);

    QWidget* m_top = nullptr;
    QWidget* m_viewer = nullptr;
    QWidget* m_timeline = nullptr;
    QScrollArea* m_toolScroll;   // the bar (scrolls when its tools do not fit)
    QWidget* m_bar;              // the bar's frame: the clip bar's fixed back arrow, then the scrolling tools
    QBoxLayout* m_barLay;
    QToolButton* m_back;
    QWidget* m_toolBox;          // inside it: main tools, clip tools
    QBoxLayout* m_toolLay;
    QWidget* m_mainTools;
    QBoxLayout* m_mainLay;
    QWidget* m_clipTools;
    QBoxLayout* m_clipLay;
    QWidget* m_sheet;            // header + the panels' scroll areas
    QWidget* m_sheetHeader;
    QLabel* m_sheetTitle;
    QBoxLayout* m_sheetLay;
    QVector<Tool> m_tools;
    QString m_open;
    QString m_openedBy; // the clip tool that opened the sheet (its panel belongs to a main tool), else empty
    double m_share = 0.55;
    std::function<int()> m_sheetTopHint;
    std::function<int(int)> m_timelineHeight;
    QHash<QString, double> m_shares; // per panel: its height (setPanelShare, then the user's drags)
    bool m_landscape = false;
    bool m_tablet = false;
    bool m_laidOut = false;
    QSize m_steadySize; // portrait: the shell's size when the rows' room was last measured (not under a keyboard)
    int m_steadyMost = 0;
    bool m_clipMode = false;
    int m_dragStartY = -1;
    double m_dragStartShare = 0;
};
