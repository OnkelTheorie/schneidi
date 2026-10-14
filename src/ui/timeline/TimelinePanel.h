#pragma once
#include <QIcon>
#include <QWidget>

class Editor;
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
    enum class Icon { Select, Trim, Blade, Snap, Link };
    static QIcon toolIcon(Icon icon);
    // Switch in the toolbar after "Snapping" (e.g. Linked Selection), follows the action
    void addToolAction(QAction* action, Icon icon);

    // Open timeline/compound clip like DaVinci: an opened compound clip remembers the timeline it was
    // opened from (button "◂" resp. back() returns there)
    void openSequence(int id);
    void back();
    bool canGoBack() const;

private:
    void syncScrollbars();
    void rebuildTimelines(); // selection list of the timelines

    Editor* m_editor;
    QComboBox* m_timelines;
    QToolButton* m_back;
    QVector<int> m_history; // timelines from which compound clips were opened

    TimelineView* m_view;
    QScrollBar* m_hbar;
    QScrollBar* m_vbar;
    QHBoxLayout* m_bar = nullptr;
    int m_toolInsert = 0;
};
