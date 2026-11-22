// SPDX-License-Identifier: AGPL-3.0-only
// The phone layout (small screens, docs/dev/touch.md › Handy-Layout): the main window's panels rearranged for a
// screen whose shorter side is a phone's. Portrait: top bar, viewer, timeline, a tool bar at the bottom; a tool opens
// its panel as a sheet over the timeline (one at a time). Landscape: the tools as a rail at the left, the viewer and
// the open panel side by side, the timeline below. A selected clip switches the bar to clip tools (with a back arrow).
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
    // A clip tool that opens a panel of the main bar (e.g. "Lautstärke" -> the Inspector)
    void addClipPanelTool(const QString& id, const QString& symbol, const QString& label, const QString& panelId);
    QToolButton* toolButton(const QString& id) const; // tests

    // The sheet: one panel at a time; opening another closes the one before
    void openSheet(const QString& id);
    void closeSheet();
    QString sheet() const { return m_open; }               // empty = closed
    QWidget* sheetFrame() const { return m_sheet; }        // the sheet itself (header and panel)
    QWidget* panel(const QString& id) const;               // nullptr until it was opened once
    QString panelId(const QWidget* panel) const;           // the tool that shows `panel` (empty = none)
    // Height of the sheet in portrait as a share of the room above the tool bar (dragging its handle changes it)
    double sheetShare() const { return m_share; }
    void setSheetShare(double share);

    // The clip bar instead of the main tools (MainWindow: while clips are selected)
    void setClipToolsShown(bool on);
    bool clipToolsShown() const { return m_clipMode; }

    // One step back (Android's back key): an open sheet closes, then the clip bar goes (clipToolsLeft); false = nothing
    // was open
    bool back();

    bool landscape() const { return m_landscape; }

    // Before the shell is shown its panels report the desktop's minimums (polish and their first layout come with the
    // show): none then, the window is never sized to them (Android maximizes it before anything is laid out). Shown,
    // at most the window's shorter side in both directions (see the .cpp)
    QSize minimumSizeHint() const override;

signals:
    void sheetChanged(const QString& id); // opened (id) or closed (empty)
    void clipToolsLeft();                 // the clip bar's back arrow or the back key: MainWindow clears the selection

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
    Tool* tool(const QString& id);

    QWidget* m_top = nullptr;
    QWidget* m_viewer = nullptr;
    QWidget* m_timeline = nullptr;
    QScrollArea* m_toolScroll;   // the bar (scrolls when its tools do not fit)
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
    double m_share = 0.55;
    bool m_landscape = false;
    bool m_laidOut = false;
    bool m_clipMode = false;
    int m_dragStartY = -1;
    double m_dragStartShare = 0;
};
