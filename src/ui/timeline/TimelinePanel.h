// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QIcon>
#include <QWidget>

class Editor;
class TimelineOverview;
class TimelineView;
class QAction;
class QComboBox;
class QHBoxLayout;
class QToolButton;
class QScrollBar;

// Frame around the timeline: toolbar on top, scrollbars right/bottom.
class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(Editor* editor, QWidget* parent = nullptr);
    TimelineView* view() const { return m_view; }
    // Toolbar icons (drawn like in DaVinci: off = gray, on = white)
    enum class Icon { Select, Trim, Blade, Snap, Link, Slip, Slide, Record };
    static QIcon toolIcon(Icon icon);
    // Switch in the toolbar after "Snapping" (e.g. Linked Selection), follows the action
    void addToolAction(QAction* action, Icon icon);

    // Open timeline/compound clip like DaVinci: an opened compound clip remembers the timeline it was
    // opened from (button "◂" resp. back() returns there)
    void openSequence(int id);
    void back();
    bool canGoBack() const;

    // Overview strip above the lanes (TimelineOverview), shown only when the user switched it on (Timeline ›
    // Timeline view) AND the page allows it (only Edit; the panel also sits on Deliver and Color). Neither switch
    // touches the ViewState.
    TimelineOverview* overview() const { return m_overview; }
    void setOverviewEnabled(bool on);
    void setOverviewOnPage(bool on);
    bool overviewShown() const { return m_overviewEnabled && m_overviewOnPage; }

    // Phone layout (ui/phone/PhoneShell): the tool bar never sets the panel's width - fewer tools (slip and slide stay
    // in the menu), a narrow timeline list, the rest scrolls sideways
    void setCompact(bool on);

private:
    void syncScrollbars();
    void syncOverview();      // shown or not; when shown, the current timeline/view/playhead into it
    void feedOverview();      // the project's current timeline into the strip (only while shown)
    void syncOverviewRange(); // the view's visible frames as the strip's rectangle (only mirrors, never sets)
    void rebuildTimelines(); // selection list of the timelines

    Editor* m_editor;
    QComboBox* m_timelines;
    QToolButton* m_back;
    QVector<int> m_history; // timelines from which compound clips were opened

    TimelineView* m_view;
    TimelineOverview* m_overview = nullptr;
    bool m_overviewEnabled = false;
    bool m_overviewOnPage = true;
    QScrollBar* m_hbar;
    QScrollBar* m_vbar;
    QHBoxLayout* m_bar = nullptr;
    QWidget* m_barWidget = nullptr;
    class QScrollArea* m_barScroll = nullptr; // setCompact

    int m_toolInsert = 0;
};
