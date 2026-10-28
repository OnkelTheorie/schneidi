// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QWidget>

class Project;
class RenderQueue;
class QAction;
class QListWidget;
class QPushButton;
class QLabel;

// Render queue on the right of the Deliver page: jobs as cards with name, settings, range, target and status
// (progress bar while rendering). "Render all" resp. "Render selection", stop; drag jobs to change the order;
// right-click: render again, load settings, show the file, delete; double-click: a finished job's file in the file
// manager, otherwise load its settings; Del deletes (deleteAction(), a bindable key of this panel).
class RenderQueuePanel : public QWidget {
    Q_OBJECT
public:
    RenderQueuePanel(Project* project, RenderQueue* queue, QWidget* parent = nullptr);

    void removeJobs(const QVector<int>& ids); // running job stays
    // Moves the jobs ids in front of the job beforeId (0 = to the end); the running job stays where it is
    void moveJobs(const QVector<int>& ids, int beforeId);
    QVector<int> selectedJobs() const;
    void renderClicked();                     // like the button: render selection resp. all, running = stop
    // Deletes the selected jobs. Its keys (InputBindings "render_queue_delete", default Del and Backspace) only count
    // while the queue has the focus, there they win over the timeline's Del (the panel checks them itself)
    QAction* deleteAction() const { return m_deleteAction; }

signals:
    void loadJobRequested(int id); // double-click on a job that is not finished: its settings into the render settings
    // A finished job's file: show it in the file manager (double-click, menu) resp. open it (menu); the Deliver page
    // passes them on to ui/FileManager
    void revealFileRequested(const QString& path);
    void openFileRequested(const QString& path);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void rebuild();
    void updateButton();
    void contextMenu(const QPoint& pos);
    void jobDoubleClicked(int id);

    Project* m_project;
    RenderQueue* m_queue;
    QLabel* m_title;
    QListWidget* m_list;
    QPushButton* m_renderBtn;
    QLabel* m_empty;
    QAction* m_deleteAction;
    // The start button turns into "Stop": clicks right after starting (a double-click, clicks made while the start was
    // busy) would stop the render that was just started -> ignore them (input event times, milliseconds)
    quint64 m_clickTime = 0;
    quint64 m_ignoreClicksBefore = 0;
};
