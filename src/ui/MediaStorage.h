#pragma once
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QWidget>

class HoverScrub;
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
// without a second monitor/file manager next to it. A click shows a file in the Viewer without importing it,
// double-click resp. right-click imports into the Media Pool. Moving the mouse across a video's thumbnail shows its
// pictures (hover scrub). Files already in the Media Pool carry a check mark. The folder is watched: new files appear
// on their own.
class MediaStorage : public QWidget {
    Q_OBJECT
public:
    explicit MediaStorage(QWidget* parent = nullptr);
    ~MediaStorage() override;

    static bool isMediaFile(const QString& path);
    QString currentFolder() const { return m_folder; }
    void setFolder(const QString& dir);
    void setPersistent(bool on) { m_persistent = on; } // test runs: do not touch the settings
    // The Media Pool's files: they get a check mark (and say so in the tooltip). Call again when the pool changes.
    void setPoolFiles(const QStringList& paths);
    bool inPool(const QString& path) const;
    HoverScrub* hoverScrub() const { return m_scrub; }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

signals:
    void importRequested(const QStringList& paths); // into the selected bin of the Media Pool
    void folderImportRequested(const QString& dir);  // including subfolders as bins
    void previewRequested(const QString& path);      // click on a file: show it in the Viewer, not imported
    void watchFolderRequested(const QString& dir);   // "Watch This Folder…": new files come into the Media Pool

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
    int nextInView() const; // index in m_thumbQueue to make next
    void thumbDone(const QString& path, const QPixmap& pm);
    void updateItem(QListWidgetItem* it); // icon (thumbnail or hover picture + pool mark) and tooltip of a file
    static QString poolKey(const QString& path);

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
    QHash<QString, QPixmap> m_shown;  // path -> thumbnail resp. placeholder of the entry (without the pool mark)
    QSet<QString> m_inPool;           // poolKey() of the Media Pool's files
    HoverScrub* m_scrub = nullptr;
    QString m_hoverPath;   // entry showing a hover picture
    QPixmap m_hoverPixmap;
    QStringList m_thumbQueue;
    bool m_thumbRetry = false; // nextThumb() waits for foreground work (Background::foregroundBusy)
    QProcess* m_ffmpeg = nullptr;
    QString m_thumbPath;
};
