#pragma once
#include <QWidget>

class Editor;
class TimelineView;
class QScrollBar;

// Rahmen um die Timeline: Werkzeugleiste oben, Scrollbars rechts/unten.
class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(Editor* editor, QWidget* parent = nullptr);
    TimelineView* view() const { return m_view; }

private:
    void syncScrollbars();

    TimelineView* m_view;
    QScrollBar* m_hbar;
    QScrollBar* m_vbar;
};
