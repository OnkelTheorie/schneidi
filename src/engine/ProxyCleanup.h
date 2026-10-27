#pragma once
// Cleaning up the proxy folder (ProxyManager::cacheDir()): deletes the proxy files no project needs any more, instead
// of only "delete the proxies of these clips". A proxy's name derives from path, size and modification time of its
// original (ProxyManager::proxyPath), so a proxy is in use exactly when a file a project uses still maps to it.
//
// The rule:
// - kept: the proxies of every file the open project uses (media pool and all timelines, unsaved changes included)
//   and of every file of the recently opened projects (File › Recent), and the file being written right now;
// - deleted: everything else in the folder that is a proxy – proxies whose original was deleted or moved, changed
//   (a changed original gets a new proxy anyway), is used by no recent project, or that were made at another proxy
//   size; half-written files a crash left behind; notes without their proxy.
// Other files in the folder are never touched, neither are the originals. Proxies can always be created again.

#include "core/ProjectFile.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace ProxyCleanup {

enum class Reason {
    Missing,   // the original is gone (deleted, moved, renamed)
    Changed,   // the original was changed since (it maps to another proxy now)
    OtherSize, // made at another proxy size
    Unused,    // the original is there, but no open or recent project uses it
    Unknown,   // made by an older version (no note which file it was for), no project uses it
    Leftover,  // half-written file (a crash during generation) or a note without its proxy
};

struct Item {
    QString file;     // the proxy file (or leftover)
    QString original; // the file it was made for (empty = unknown)
    Reason reason = Reason::Unknown;
    qint64 bytes = 0; // with its note
};

struct Plan {
    QVector<Item> items; // what cleaning up deletes, largest first
    qint64 bytes = 0;    // their total size
    int kept = 0;        // proxies that stay
    qint64 keptBytes = 0;
};

// What would be deleted in `dir`. keep: originals whose proxies stay (open + recent projects); busy: files being written
// right now (ProxyManager's current generation), never touched
Plan plan(const QStringList& keep, const QStringList& busy = {}, const QString& dir = {});
// Deletes the plan's files (proxies with their notes); returns the bytes freed. Files being written by now (busy),
// proxies of originals in `keep` (checked again right before deleting: the project may have changed since the plan)
// and files that cannot be deleted (gone already, open elsewhere) are skipped.
qint64 apply(const Plan& plan, const QStringList& busy = {}, const QStringList& keep = {});

// Every media file a project uses: media pool and the clips of all timelines (compound and multicam contents too)
QStringList projectMedia(const ProjectData& data);
// The projects of File › Recent (MainWindow::addRecent, QSettings "recentProjects")
QStringList recentProjects();
// The media of these project files, each read from disk; unreadable ones are skipped
QStringList mediaOfProjects(const QStringList& projectFiles);

// Note next to a finished proxy (<proxy>.json): which file it was made for, at which size (ProxyManager writes it)
void writeNote(const QString& proxy, const QString& original, int maxSize);
QString notePath(const QString& proxy);

// For the UI and the CLI
QString reasonText(Reason reason); // translated
const char* reasonId(Reason reason); // "missing", "changed", … (CLI)
QString sizeText(qint64 bytes);    // "1,4 GB" in the UI language

} // namespace ProxyCleanup
