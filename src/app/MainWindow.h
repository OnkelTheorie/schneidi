#pragma once
#include "core/Types.h"

#include <QHash>
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
class QSplitter;
class QStackedWidget;
class QButtonGroup;
class QToolButton;
class DeliverPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Engine* engine, QWidget* parent = nullptr);
    void importFiles(const QStringList& paths, bool placeOnTimeline = false);

    // Seiten wie in DaVinci (unten umschaltbar). Neue Seite: hier + in showPage() ergänzen.
    enum class Page { Media, Edit, Deliver };
    void showPage(Page page);

private:
    void buildLayout();
    QWidget* buildTopBar();
    QWidget* buildPageBar();
    void onDrop(const QStringList& paths, int frame, int track);
    MediaInfo probeCached(const QString& path);
    void buildActions();
    QAction* makeAction(QMenu* menu, const QString& id, const QString& text, const QKeySequence& key,
                       const std::function<void()>& fn);
    void jumpToEdit(int direction);
    void jumpToMarker(int direction);
    void jumpTo(const QVector<int>& points, int direction);
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
    DeliverPanel* m_deliver = nullptr;

    QStackedWidget* m_pages = nullptr;
    QSplitter* m_editTop = nullptr;
    QSplitter* m_editMain = nullptr;
    QSplitter* m_mediaPage = nullptr;
    QSplitter* m_deliverPage = nullptr;
    QSplitter* m_deliverRight = nullptr;
    QButtonGroup* m_pageButtons = nullptr;
    QToolButton* m_poolToggle = nullptr;
    QToolButton* m_inspectorToggle = nullptr;
    Page m_page = Page::Edit;

    QHash<QString, MediaInfo> m_probeCache;
};
