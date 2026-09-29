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

// Rahmen um die Timeline: Werkzeugleiste oben, Scrollbars rechts/unten.
class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(Editor* editor, QWidget* parent = nullptr);
    TimelineView* view() const { return m_view; }
    // Symbole der Werkzeugleiste (gezeichnet wie in DaVinci: aus = grau, an = weiß)
    enum class Icon { Select, Trim, Blade, Snap, Link };
    static QIcon toolIcon(Icon icon);
    // Schalter in der Werkzeugleiste hinter „Snapping“ (z. B. Verknüpfte Auswahl), folgt der Aktion
    void addToolAction(QAction* action, Icon icon);

    // Timeline/Compound Clip öffnen wie DaVinci: ein geöffneter Compound Clip merkt sich die Timeline, aus der er
    // geöffnet wurde (Knopf „◂“ bzw. back() kehrt dorthin zurück)
    void openSequence(int id);
    void back();
    bool canGoBack() const;

private:
    void syncScrollbars();
    void rebuildTimelines(); // Auswahlliste der Timelines

    Editor* m_editor;
    QComboBox* m_timelines;
    QToolButton* m_back;
    QVector<int> m_history; // Timelines, aus denen Compound Clips geöffnet wurden

    TimelineView* m_view;
    QScrollBar* m_hbar;
    QScrollBar* m_vbar;
    QHBoxLayout* m_bar = nullptr;
    int m_toolInsert = 0;
};
