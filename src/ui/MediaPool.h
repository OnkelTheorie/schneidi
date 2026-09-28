#pragma once
#include <QWidget>

class Project;
class Engine;
class QListWidget;

// Media Pool (oben links wie in DaVinci): importierte Dateien mit Vorschaubild.
// Doppelklick -> im Viewer als Quelle ansehen, Ziehen -> in die Timeline.
class MediaPool : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* MimeType = "application/x-schneidi-media";
    // Eintrag "Text" (Titel-Generator) zum Reinziehen; steht statt eines Pfads in den Drag-Daten
    static constexpr const char* TitleItem = "schneidi:title";

    MediaPool(Project* project, Engine* engine, QWidget* parent = nullptr);

public slots:
    void importDialog();
    void importFiles(const QStringList& paths);

signals:
    void sourceRequested(const QString& path);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void rebuild();

    Project* m_project;
    Engine* m_engine;
    QListWidget* m_list;
};
