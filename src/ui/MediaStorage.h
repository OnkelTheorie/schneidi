// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QFileInfo>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <memory>

class HoverScrub;
class QCheckBox;
class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QThreadPool;
class QTimer;
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
// Search (magnifier in the header): typing filters the folder at once (core/NameMatch: all words, a typo, any
// separator); "With subfolders" searches below it too, in the background (cancellable, at most kMaxDeepHits hits,
// hidden folders skipped), each hit with its folder under the name. A quick filter by kind while searching; the sort
// order (name, newest, oldest, size) is remembered.
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

    enum class Sort { Name, Newest, Oldest, Size };
    enum class Kind { All, Video, Audio, Image };
    static constexpr int kMaxDeepHits = 500;
    static Kind kindOf(const QString& path); // All = no media file
    // Search: opens the field with this text (empty closes it)
    void setSearch(const QString& text);
    QString search() const;
    void setSearchSubfolders(bool on);
    bool searchSubfolders() const;
    bool searchRunning() const; // the search below the folder still looks (or waits for a pause in typing)
    bool searchCapped() const { return m_deepCapped; }   // it stopped at kMaxDeepHits
    void setKindFilter(Kind kind);
    Kind kindFilter() const { return m_kind; }
    void setSort(Sort sort);
    Sort sort() const { return m_sort; }
    QStringList shownFiles() const; // the files in the list, in its order (tests)

protected:
    void showEvent(QShowEvent* event) override; // the start's folder listing, if it still waits (see the constructor)
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
    void rebuildFiles(); // lists the folder anew, then showFiles()
    void showFiles();    // the list from the folder's entries resp. the hits below it (search, kind, sort)
    void addFileItem(const QFileInfo& fi, int row, const QString& subtitle, const QStringList& words);
    bool sortsBefore(const QFileInfo& a, const QFileInfo& b) const;
    bool kindMatches(const QString& path) const;
    bool deepActive() const; // the search looks below the folder
    void startDeepSearch();
    void cancelDeepSearch();
    void addDeepHits(const QFileInfoList& hits);
    void updateSearchStatus();
    void updateGrid();
    void showFileMenu(const QPoint& pos);
    void showLocationMenu(const QPoint& pos);
    QStringList selectedFiles() const;
    QStringList mediaIn(const QString& dir) const;
    void saveLocations();
    void setListMode(bool list);
    void updatePathLabel();
    // Thumbnails: in m_thumbPool via engine/video/Thumbnails (videos: the key frame near 1 s) resp. QImageReader
    // (images), remembered in memory and on disk (engine/StorageThumbCache); the ones on disk come first, then what is
    // in view
    void queueThumb(const QString& path);
    void nextThumb();
    int nextInView(bool onDiskOnly) const; // index in m_thumbQueue to make next, -1: none (onDiskOnly: of m_thumbOnDisk)
    void thumbDone(const QString& path, const QPixmap& pm);
    void updateItem(QListWidgetItem* it); // icon (thumbnail or hover picture + pool mark) and tooltip of a file
    static QString poolKey(const QString& path);

    QStringList m_locations;
    QString m_folder;
    QString m_savedFolder;
    bool m_persistent = true;
    // The start's folder listing waits until the panel shows or the window is up (the constructor's setFolder; ~15 ms
    // for a folder of 200 clips, most users have the panel off)
    bool m_deferFiles = false;
    bool m_filesPending = false;
    void listPendingFiles();
    QTreeWidget* m_tree;
    QListWidget* m_list;
    QLabel* m_pathLabel;
    QToolButton* m_thumbBtn;
    QToolButton* m_listBtn;
    QToolButton* m_sortBtn;
    QToolButton* m_searchBtn;
    QLineEdit* m_search;
    QWidget* m_searchRow;
    QCheckBox* m_deepBox;
    QComboBox* m_kindBox;
    QLabel* m_searchStatus;
    Sort m_sort = Sort::Name;
    Kind m_kind = Kind::All;
    QFileInfoList m_dirEntries;  // the folder's subfolders (by name)
    QFileInfoList m_fileEntries; // its media files
    QFileInfoList m_shownFiles;  // the files of the list in its order (after its folder rows)
    int m_dirRows = 0;           // folder rows at the top of the list
    // Search below the folder: one thread, a new search cancels the old one (its hits come with its generation)
    QThreadPool* m_searchPool;
    std::shared_ptr<std::atomic_bool> m_deepCancel;
    int m_deepGeneration = 0;
    bool m_deepRunning = false;
    bool m_deepCapped = false;
    QFileInfoList m_deepHits;
    QTimer* m_deepTimer; // typing: search after a short pause
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
    QSet<QString> m_thumbOnDisk; // queued files whose thumbnail is in StorageThumbCache: loaded first (~1 ms each)
    bool m_thumbRetry = false; // nextThumb() waits for foreground work (Background::foregroundBusy)
    QThreadPool* m_thumbPool;  // a few low-priority threads (a quarter of the cores, 1-3)
    QSet<QString> m_thumbRunning; // files whose thumbnail is being made
};
