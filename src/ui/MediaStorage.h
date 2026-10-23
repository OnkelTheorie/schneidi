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

// Media Storage like DaVinci: file browser over fixed storage locations (folders). On the left the locations with their
// subfolders, on the right the contents of the selected folder (subfolders + media files with thumbnail).
// Clips can be dragged directly into the Media Pool or timeline (file URLs as from the file manager) –
// without a second monitor/file manager next to it. Double-click resp. right-click imports into the Media Pool.
// The folder is watched: new files appear on their own.
class MediaStorage : public QWidget {
    Q_OBJECT
public:
    explicit MediaStorage(QWidget* parent = nullptr);
    ~MediaStorage() override;

    static bool isMediaFile(const QString& path);
    QString currentFolder() const { return m_folder; }
    void setFolder(const QString& dir);
    void setPersistent(bool on) { m_persistent = on; } // test runs: do not touch the settings

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

signals:
    void importRequested(const QStringList& paths); // into the selected bin of the Media Pool
    void folderImportRequested(const QString& dir);  // including subfolders as bins

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
    // Thumbnails: one after another via ffmpeg (videos) resp. QImageReader (images), remembered in memory
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
    QHash<QString, QPixmap> m_thumbs; // key: path + modification time
    QStringList m_thumbQueue;
    bool m_thumbRetry = false; // nextThumb() waits for foreground work (Background::foregroundBusy)
    QProcess* m_ffmpeg = nullptr;
    QString m_thumbPath;
};
