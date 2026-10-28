// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/PoolFilter.h"

#include <QHash>
#include <QImage>
#include <QKeySequence>
#include <QSet>
#include <QPixmap>
#include <QWidget>

class Project;
class ElidedLabel;
class Engine;
class HoverScrub;
class PoolFilterButton;
class QAction;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPainter;
class QRectF;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QThreadPool;
class TimelinePosters;

// Media Pool (top left like in DaVinci): on the left the bin list (Master with sub-bins), on the right the clips of the
// selected bin with thumbnail (icon or list view). Double-click -> view in the Viewer as source,
// drag -> into the timeline resp. onto a bin (move). Right-click: clip color, flags, proxy media.
class MediaPool : public QWidget {
    Q_OBJECT
public:
    static constexpr const char* MimeType = "application/x-schneidi-media";
    // All dragged Media Pool clips (paths, one line per clip) – for moving into a bin
    static constexpr const char* ListMimeType = "application/x-schneidi-media-list";
    // Dragged bin (id) – drag onto another bin = move it there
    static constexpr const char* BinMimeType = "application/x-schneidi-bin";
    // Entry "Text" (title generator) for dragging in; stands in place of a path in the drag data
    static constexpr const char* TitleItem = "schneidi:title";
    // Timeline/compound clip (sequence) as an entry: "schneidi:seq:<id>" instead of a path
    static QString sequenceItem(int id);
    static int sequenceOfItem(const QString& item); // 0 = not a sequence entry

    enum class ViewMode { Thumbnails, List };
    enum class SortKey { Name, Date, Duration };

    MediaPool(Project* project, Engine* engine, QWidget* parent = nullptr);
    ~MediaPool() override;

    // Selected media (without title and sequence entries)
    QStringList selectedMedia() const;
    QVector<int> selectedSequences() const; // selected timelines/compound clips
    int currentBin() const { return m_currentBin; }
    void setCurrentBin(int id);
    void setViewMode(ViewMode mode);
    void setSort(SortKey key, bool ascending);
    // Search like DaVinci (magnifier on top): shows matching clips (name contains text) in the bin and its sub-bins
    void setSearch(const QString& text);
    // Filter (funnel next to the search): by kind, flag and clip color; like the search it shows the matching clips of
    // the bin and its sub-bins, and both combine. For this session only (not in the project).
    const PoolFilter& filter() const;
    void setFilter(const PoolFilter& filter);
    // Keys of the Media Pool for the key bindings (MainWindow registers them with InputBindings::registerScopedAction):
    // they act only while their widget has the focus
    struct KeyAction {
        QAction* action;
        const char* id; // key in keybindings.json
        QKeySequence defaultKey;
        QWidget* scope;
        QKeySequence defaultKey2 = {}; // second slot (Backspace beside Delete)
    };
    QVector<KeyAction> keyActions() const;
    // do not save view/sorting in the settings (test runs)
    void setSaveSettings(bool on) { m_saveSettings = on; }
    // New from a watch folder: drawn with a frame in the accent colour for a few seconds
    void markFresh(const QStringList& paths);

    // right-click menus "Clip color" and "Flags" (also in the timeline) for the clips of these media
    static void addClipColorMenu(QMenu* menu, Project* project, const QStringList& paths);
    static void addFlagsMenu(QMenu* menu, Project* project, const QStringList& paths);
    // Small flag like DaVinci (pole + pennant) in r
    static void drawFlag(QPainter& p, const QRectF& r, const QColor& color);

public slots:
    void importDialog();
    void importFiles(const QStringList& paths); // lands in the selected bin
    // Like DaVinci "Add Folder and SubFolders into Media Pool (Create Bins)": bin with the folder name in the selected bin,
    // subfolders with media as sub-bins; one undo step for the bins
    void importFolder(const QString& dir);
    void newBin();                               // in the selected bin, edit the name right away (Ctrl+Shift+N)
    void newTimeline();                          // new empty timeline in the selected bin, gets opened
    void renameSequence(int id);                 // ask for the name
    // Delete/Backspace on the clips (right-click "Remove from Media Pool"): the selected media leave the pool, the
    // selected timelines are deleted (like their "Delete"), one undo step. Media a timeline uses: a question names how
    // many clips in which timelines, "Remove with Their Clips" takes those clips out too, Cancel keeps everything.
    void removeSelected();

signals:
    void sourceRequested(const QString& path);
    // double-click on a timeline/compound clip: open it in the timeline
    void sequenceOpenRequested(int id);
    // subtitle files (.srt) imported/dragged in: become subtitle tracks, not media
    void subtitleFilesImported(const QStringList& paths);
    // Right-click "Create Multicam Clip Using Selected Clips" (like DaVinci): the main window asks for name and sync
    void multicamRequested(const QStringList& paths);
    // Right-click on a bin "Watch Folder…": a folder whose new files come into this bin on their own
    void watchFolderRequested(int bin);
    // Right-click "Clean up proxy media…": the main window asks and deletes (app/ProxyCleanupDialog)
    void proxyCleanupRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void rebuild();      // media list changed: view anew (thumbnails of new media come from requestThumb)
    void rebuildBins();  // bin list anew (selection/expanded bins stay)
    void rebuildClips(); // clips of the selected bin (sorted)
    void showContextMenu(const QPoint& pos);
    void showBinMenu(const QPoint& pos);
    void removeBin(int id); // with confirmation if clips are inside
    QStringList addFiles(const QStringList& paths, int bin); // returns the unreadable file names
    bool importFolderInto(const QString& dir, int parentBin, int depth, QStringList* failed);
    void updateItem(const QString& path);
    // Thumbnail in a worker thread (opens the file and decodes up to a third of it: blocked the window per clip)
    void requestThumb(const QString& path, int frame);
    void thumbReady(const QString& path, const QImage& img, int generation);
    void updateSequenceItems(); // duration of the timeline entries (after changes in the edit) // thumbnail with clip color, flags, proxy icon resp. progress
    void updateProxyStatus();
    void applyViewMode();
    void saveSettings() const;

    Project* m_project;
    Engine* m_engine;
    QTreeWidget* m_bins;
    ElidedLabel* m_binTitle;
    QListWidget* m_list;
    QLabel* m_proxyStatus;
    QToolButton* m_proxyCancel;
    QToolButton* m_thumbView;
    QToolButton* m_listView;
    QToolButton* m_sortBtn;
    QToolButton* m_searchBtn;
    QLineEdit* m_search;
    PoolFilterButton* m_filterBtn;
    QToolButton* m_filterReset; // beside the bin title while a filter is active
    QLabel* m_emptyHint;        // over the empty list: nothing matches the search or filter
    QAction* m_removeBinKey;   // Delete/Backspace in the bin list
    QAction* m_closeSearchKey; // Esc in the search field
    QAction* m_removeClipsKey; // Delete/Backspace on the clips
    HoverScrub* m_scrub;        // pictures of the clip under the mouse (icon view)
    QHash<QString, QPixmap> m_scrubPicture; // path -> picture shown instead of the thumbnail while hovering
    QHash<QString, QListWidgetItem*> m_items; // path -> entry
    QHash<QString, QPixmap> m_thumbs;         // thumbnail without displaying
    QSet<QString> m_thumbPending;             // being computed
    QSet<QString> m_offline;                  // showing "Media Offline" (computed anew when the file is back)
    int m_thumbGeneration = 0;                // project format changed: older results are dropped
    QThreadPool* m_thumbPool;
    TimelinePosters* m_posters = nullptr; // pictures of the timeline entries (first frame with its grade)
    int m_currentBin = 0;
    int m_editBin = 0; // newly created: edit the name after the rebuild
    ViewMode m_viewMode = ViewMode::Thumbnails;
    SortKey m_sortKey = SortKey::Name;
    bool m_sortAscending = true;
    bool m_saveSettings = true;
    bool m_rebuildQueued = false;
    QSet<QString> m_fresh; // markFresh
};
