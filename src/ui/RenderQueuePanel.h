#pragma once
#include <QWidget>

class Project;
class RenderQueue;
class QListWidget;
class QPushButton;
class QLabel;

// Render-Warteschlange rechts auf der Deliver-Seite (wie DaVinci „Render Queue“): Aufträge als Karten mit
// Name, Einstellungen, Bereich, Ziel und Status (Fortschrittsbalken beim Rendern). „Alle rendern“ bzw.
// „Auswahl rendern“, Stoppen; Rechtsklick: erneut rendern, Einstellungen laden, löschen; Entf löscht.
class RenderQueuePanel : public QWidget {
    Q_OBJECT
public:
    RenderQueuePanel(Project* project, RenderQueue* queue, QWidget* parent = nullptr);

    void removeJobs(const QVector<int>& ids); // laufender Auftrag bleibt
    QVector<int> selectedJobs() const;
    void renderClicked();                     // wie der Knopf: Auswahl bzw. alle rendern, läuft = stoppen

signals:
    void loadJobRequested(int id); // Doppelklick: Einstellungen in die Render-Einstellungen übernehmen

protected:
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void rebuild();
    void updateButton();
    void contextMenu(const QPoint& pos);

    Project* m_project;
    RenderQueue* m_queue;
    QLabel* m_title;
    QListWidget* m_list;
    QPushButton* m_renderBtn;
    QLabel* m_empty;
};
