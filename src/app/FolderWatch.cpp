#include "app/FolderWatch.h"

#include "core/Project.h"
#include "engine/Background.h"
#include "engine/Engine.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QUndoStack>

#include <algorithm>
#include <utility>

namespace {
constexpr int kMaxWatchedDirs = 1000; // the system's watches are limited (inotify); the rescan covers the rest
}

struct FolderWatch::Batch {
    int generation = 0;
    QVector<WatchImport> imports;
    QStringList pending;
    QStringList folders;
};

FolderWatch::FolderWatch(Project* project, QObject* parent)
    : QObject(parent), m_project(project), m_fs(new QFileSystemWatcher(this)),
      m_scanner(std::make_shared<WatchFolders::Scanner>())
{
    m_pool.setMaxThreadCount(1);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &FolderWatch::startScan);
    m_rescan.setInterval(5000);
    connect(&m_rescan, &QTimer::timeout, this, [this] { scanSoon(0); });
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &FolderWatch::importWaiting);
    connect(m_fs, &QFileSystemWatcher::directoryChanged, this, [this] { scanSoon(); });
    connect(m_project, &Project::watchFoldersChanged, this, &FolderWatch::restart);
    connect(m_project, &Project::formatChanged, this, [this] { // lengths count in the project's frame rate
        ++m_generation;
        scanSoon();
    });
    restart();
}

FolderWatch::~FolderWatch()
{
    m_pool.waitForDone(); // its answer is queued to this object: dropped with it
}

void FolderWatch::setSettleMs(int ms)
{
    m_settleMs = std::max(0, ms);
    scanSoon();
}

void FolderWatch::setRescanMs(int ms)
{
    m_rescan.setInterval(std::max(100, ms));
}

bool FolderWatch::idle() const
{
    return !m_running && !m_debounce.isActive() && m_waiting.isEmpty();
}

QStringList FolderWatch::watchedDirectories() const { return m_fs->directories(); }

void FolderWatch::scanSoon(int delayMs)
{
    if (m_project->watchFolders().isEmpty()) return;
    if (!m_debounce.isActive() || m_debounce.remainingTime() > delayMs) m_debounce.start(delayMs);
}

// Watch list changed or another project opened: what was scanned for the old one is dropped; strictly opt-in, so
// without watch folders nothing runs at all
void FolderWatch::restart()
{
    ++m_generation;
    m_waiting.clear();
    if (m_project->watchFolders().isEmpty()) {
        m_rescan.stop();
        m_debounce.stop();
        updateWatched({});
        return;
    }
    m_rescan.start();
    scanSoon(0); // catch up on what came while the project was closed
}

void FolderWatch::startScan()
{
    if (m_running) {
        m_again = true;
        return;
    }
    const QVector<WatchFolder> watches = m_project->watchFolders();
    if (watches.isEmpty()) return;
    QSet<QString> pool;
    for (const MediaInfo& m : m_project->media()) pool.insert(WatchFolders::pathKey(m.path));
    m_running = true;
    m_scanner->setSettleMs(m_settleMs);
    const ProjectFormat format = m_project->format();
    const int generation = m_generation;
    const std::shared_ptr<WatchFolders::Scanner> scanner = m_scanner;
    m_pool.start([this, watches, pool, format, generation, scanner] {
        Background::lowerThisThread(Background::Level::Low); // own pool: it does nothing else
        auto batch = std::make_shared<Batch>();
        batch->generation = generation;
        const WatchFolders::Scanner::Result found =
            scanner->scan(watches, [&pool](const QString& key) { return pool.contains(key); });
        batch->pending = found.pending;
        batch->folders = found.folders;
        for (const WatchFolders::Scanner::Found& f : found.ready) {
            Background::yieldToForeground(); // opening files for the preview first
            const MediaInfo info = Engine::probe(format, f.path);
            // Not readable (yet): an MP4 whose recorder writes the index last, a broken file - again when it changes
            if (info.length <= 0 || (!info.hasVideo && !info.hasAudio)) scanner->rejected(f.path);
            else batch->imports << WatchImport{info, watches[f.watch].path};
        }
        QMetaObject::invokeMethod(this, [this, batch] { scanDone(batch); }, Qt::QueuedConnection);
    });
}

void FolderWatch::scanDone(const std::shared_ptr<Batch>& batch)
{
    m_running = false;
    if (batch->generation == m_generation) {
        updateWatched(batch->folders);
        if (!batch->imports.isEmpty()) {
            if (m_waitingGeneration != m_generation) m_waiting.clear();
            m_waiting += batch->imports; // the project leaves out what is in the pool by now
            m_waitingGeneration = m_generation;
            importWaiting();
        }
        // Still being written: look again once it may be done
        if (!batch->pending.isEmpty()) scanSoon(m_settleMs + 250);
    } else {
        m_again = true;
    }
    if (m_again) {
        m_again = false;
        scanSoon(0);
    }
}

void FolderWatch::importWaiting()
{
    if (m_waiting.isEmpty()) return;
    if (m_waitingGeneration != m_generation) {
        m_waiting.clear();
        return;
    }
    // An undo macro is open (a command composing several steps) or the app is busy: a moment later
    const QUndoStack* undo = m_project->undoStack();
    const bool macro = undo->index() < undo->count() && !undo->canRedo();
    if (macro || (m_busy && m_busy())) {
        m_retry.start(1000);
        return;
    }
    const QVector<WatchImport> files = std::exchange(m_waiting, {});
    const QVector<WatchArrival> arrivals = m_project->addWatchedMedia(files);
    QStringList folders;
    QHash<QString, QStringList> byFolder;
    for (const WatchArrival& a : arrivals) {
        if (!byFolder.contains(a.folder)) folders << a.folder;
        byFolder[a.folder] << a.path;
    }
    for (const QString& f : folders) emit imported(byFolder.value(f), f);
}

void FolderWatch::updateWatched(const QStringList& folders)
{
    const QStringList now = m_fs->directories();
    const QSet<QString> wanted(folders.begin(), folders.begin() + std::min<qsizetype>(folders.size(), kMaxWatchedDirs));
    QStringList gone, added;
    for (const QString& d : now)
        if (!wanted.contains(d)) gone << d;
    for (const QString& d : wanted)
        if (!now.contains(d)) added << d;
    if (!gone.isEmpty()) m_fs->removePaths(gone);
    if (!added.isEmpty()) m_fs->addPaths(added); // a folder it cannot watch: the rescan still sees it
}
