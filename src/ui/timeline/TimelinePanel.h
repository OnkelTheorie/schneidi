#pragma once
#include <QWidget>

class Editor;
class TimelineView;
class QAction;
class QHBoxLayout;
class QScrollBar;

// Rahmen um die Timeline: Werkzeugleiste oben, Scrollbars rechts/unten.
class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(Editor* editor, QWidget* parent = nullptr);
    TimelineView* view() const { return m_view; }
    // Schalter in der Werkzeugleiste hinter „Snapping“ (z. B. Verknüpfte Auswahl), folgt der Aktion
    void addToolAction(QAction* action, const QString& text);

private:
    void syncScrollbars();

    TimelineView* m_view;
    QScrollBar* m_hbar;
    QScrollBar* m_vbar;
    QHBoxLayout* m_bar = nullptr;
    int m_toolInsert = 0;
};
