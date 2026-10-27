#pragma once
// The watch folders of the open project (core/WatchFolders.h) while the app runs: QFileSystemWatcher on the folders
// (portable: inotify, ReadDirectoryChangesW, FSEvents underneath) plus a cheap rescan every few seconds (network
// folders, very full folders, events the system dropped). Listing and probing run in a worker thread, never in the
// window's; the new complete files of a scan come into the Media Pool as one undo step. When a project opens, files
// that came while it was closed are imported too (newer than the watch's start, not in the pool).

#include "core/WatchFolders.h"

#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include <functional>
#include <memory>

class Project;
class QFileSystemWatcher;

class FolderWatch : public QObject {
    Q_OBJECT
public:
    explicit FolderWatch(Project* project, QObject* parent = nullptr);
    ~FolderWatch() override;

    // While it answers true, a finished scan waits with its import (an AI command runs, the AI chat works)
    void setBusy(const std::function<bool()>& busy) { m_busy = busy; }
    void setSettleMs(int ms);  // how long a file must stay unchanged (default WatchFolders::kSettleMs)
    void setRescanMs(int ms);  // the rescan besides the system's events (default 5 s)
    bool idle() const;         // no scan running, nothing waiting to be imported (tests)
    QStringList watchedDirectories() const; // what the QFileSystemWatcher has (tests)
    void scanSoon(int delayMs = 300);

signals:
    // Files of one watch folder came into the Media Pool
    void imported(const QStringList& paths, const QString& folder);

private:
    struct Batch;
    void restart();
    void startScan();
    void scanDone(const std::shared_ptr<Batch>& batch);
    void importWaiting();
    void updateWatched(const QStringList& folders);

    Project* m_project;
    QFileSystemWatcher* m_fs;
    QTimer m_debounce, m_rescan, m_retry;
    QThreadPool m_pool; // one worker: listing and probing
    std::shared_ptr<WatchFolders::Scanner> m_scanner; // used by one scan at a time, in the worker
    std::function<bool()> m_busy;
    int m_settleMs = WatchFolders::kSettleMs;
    bool m_running = false, m_again = false;
    int m_generation = 0;           // another project / format / watch list: older scans are dropped
    QVector<WatchImport> m_waiting; // scanned, waiting until the app is free
    int m_waitingGeneration = 0;
};
