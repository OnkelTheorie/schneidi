#pragma once
#include <QWidget>

class Project;
class RenderQueue;
class QListWidget;
class QPushButton;
class QLabel;

// Render queue on the right of the Deliver page (like DaVinci "Render Queue"): jobs as cards with
// name, settings, range, target and status (progress bar while rendering). "Render all" resp.
// "Render selection", stop; right-click: render again, load settings, delete; Del deletes.
class RenderQueuePanel : public QWidget {
    Q_OBJECT
public:
    RenderQueuePanel(Project* project, RenderQueue* queue, QWidget* parent = nullptr);

    void removeJobs(const QVector<int>& ids); // running job stays
    QVector<int> selectedJobs() const;
    void renderClicked();                     // like the button: render selection resp. all, running = stop

signals:
    void loadJobRequested(int id); // double-click: take the settings over into the render settings

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
