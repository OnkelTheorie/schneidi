#pragma once
#include <QMainWindow>
#include <functional>

class Engine;
class Project;
class Selection;
class Editor;
class MediaPool;
class Viewer;
class Inspector;
class TimelinePanel;
class QMenu;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Engine* engine, QWidget* parent = nullptr);
    void importFiles(const QStringList& paths, bool placeOnTimeline = false);

private:
    void buildLayout();
    void buildActions();
    QAction* makeAction(QMenu* menu, const QString& id, const QString& text, const QKeySequence& key,
                       const std::function<void()>& fn);
    void jumpToEdit(int direction);
    void stepFrames(int frames);
    void shuttle(int direction);

    Engine* m_engine;
    Project* m_project;
    Selection* m_selection;
    Editor* m_editor;

    MediaPool* m_mediaPool = nullptr;
    Viewer* m_viewer = nullptr;
    Inspector* m_inspector = nullptr;
    TimelinePanel* m_timeline = nullptr;
};
