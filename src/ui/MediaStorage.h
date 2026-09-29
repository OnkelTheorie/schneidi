#pragma once
#include <QHash>
#include <QPixmap>
#include <QStringList>
#include <QWidget>

class QFileSystemWatcher;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProcess;
class QTreeWidget;
class QToolButton;
class QTreeWidgetItem;

// Media Storage wie DaVinci: Dateibrowser über feste Speicherorte (Ordner). Links die Speicherorte mit ihren
// Unterordnern, rechts der Inhalt des gewählten Ordners (Unterordner + Mediendateien mit Vorschaubild).
// Clips lassen sich direkt in Media Pool oder Timeline ziehen (Datei-URLs wie aus dem Dateimanager) –
// ohne zweiten Monitor/Dateimanager daneben. Doppelklick bzw. Rechtsklick importiert in den Media Pool.
// Der Ordner wird überwacht: neue Dateien erscheinen von selbst.
class MediaStorage : public QWidget {
    Q_OBJECT
public:
    explicit MediaStorage(QWidget* parent = nullptr);
    ~MediaStorage() override;

    static bool isMediaFile(const QString& path);
    QString currentFolder() const { return m_folder; }
    void setFolder(const QString& dir);
    void setPersistent(bool on) { m_persistent = on; } // Testläufe: Einstellungen nicht anfassen

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

signals:
    void importRequested(const QStringList& paths); // in den gewählten Bin des Media Pools

private:
    void addLocation();
    void removeLocation(QTreeWidgetItem* item);
    void rebuildLocations();
    void populateChildren(QTreeWidgetItem* item);
    void selectFolderInTree(const QString& dir);
    void rebuildFiles();
    void showFileMenu(const QPoint& pos);
    void showLocationMenu(const QPoint& pos);
    QStringList selectedFiles() const;
    QStringList mediaIn(const QString& dir) const;
    void saveLocations();
    void setListMode(bool list);
    void updatePathLabel();
    // Vorschaubilder: nacheinander per ffmpeg (Videos) bzw. QImageReader (Bilder), im Speicher gemerkt
    void queueThumb(const QString& path);
    void nextThumb();
    void thumbDone(const QString& path, const QPixmap& pm);

    QStringList m_locations;
    QString m_folder;
    QString m_savedFolder;
    bool m_persistent = true;
    QTreeWidget* m_tree;
    QListWidget* m_list;
    QLabel* m_pathLabel;
    QToolButton* m_thumbBtn;
    QToolButton* m_listBtn;
    bool m_listMode = false;
    QFileSystemWatcher* m_watcher;
    QHash<QString, QListWidgetItem*> m_items;
    QHash<QString, QPixmap> m_thumbs; // Schlüssel: Pfad + Änderungszeit
    QStringList m_thumbQueue;
    QProcess* m_ffmpeg = nullptr;
    QString m_thumbPath;
};
