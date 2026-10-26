#pragma once
// Watch folders: a project can watch folders and take new media files from them into the Media Pool on its own - the
// output folder of a screen recorder, a game clip tool or another program, a pipeline that drops recordings for an AI
// to cut. Strictly opt-in: nothing is watched unless the user (or an AI for the user) adds a folder.
// What counts as a new, complete file is decided here (no engine): the app watches with QFileSystemWatcher plus a
// rescan (app/FolderWatch.h), schneidi-cli with the `watch` command (cli/CommandsWatch.cpp). Both import through
// Project::addWatchedMedia (one undo step per batch).

#include "core/Types.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

struct WatchFolder {
    QString path;            // absolute folder
    bool subfolders = false; // also files in subfolders (no hidden ones, no symbolic links)
    bool video = true;       // media kinds it takes
    bool audio = true;
    bool images = true;
    int bin = 0;             // Media Pool bin for its files (0 = Master; a bin removed meanwhile = Master)
    QDateTime since;         // when the watch started (UTC): files that were there before stay out (invalid = all)
    QStringList ignored;     // files whose import was undone: not taken again
    bool operator==(const WatchFolder&) const = default;
};

// One file a watch folder brought in (Project::watchLog). `revision` counts up per project, so a script or an AI can
// ask for what arrived since the revision it saw last.
struct WatchArrival {
    int revision = 0;
    QString path;
    QString folder;  // the watch folder it came from
    QDateTime time;  // when it was imported (UTC)
    bool operator==(const WatchArrival&) const = default;
};

// A file to import (probed) and the watch folder it came from
struct WatchImport {
    MediaInfo media;
    QString folder;
};

namespace WatchFolders {

inline constexpr int kLogSize = 200;   // arrivals the project keeps
inline constexpr int kSettleMs = 3000; // a file unchanged this long (and readable) counts as written completely

enum class Kind { Other, Video, Audio, Image };
Kind kindOf(const QString& path); // by the extension
bool takes(const WatchFolder& w, const QString& path); // a media file of a kind w wants
// Hidden, temporary or partly downloaded files: ".clip.mp4", "~clip.mp4", "clip.mp4.part", "clip.crdownload",
// "clip.tmp.mp4" … (the recorder or browser renames them when they are done)
bool isTemporary(const QString& fileName);
// Comparable form of a path (cleaned; Windows: case does not matter)
QString pathKey(const QString& path);
bool samePath(const QString& a, const QString& b);
int indexOf(const QVector<WatchFolder>& list, const QString& folder); // -1 = not watched
QStringList typeNames(const WatchFolder& w); // "video", "audio", "image"
// "video,audio" -> kinds of w; false = an unknown name (w unchanged)
bool setTypes(WatchFolder& w, const QStringList& names);

// A media file in a watch folder as listed
struct Listed {
    QString path;
    qint64 size = 0;
    qint64 modified = 0; // ms since the epoch
    qint64 created = 0;  // birth or metadata change (moved in), whichever is later; 0 = unknown
};
// The media files of w's kinds in its folder (subfolders too if it says so; no hidden or temporary files, no symbolic
// links, at most 8 levels deep). `folders` gets every folder listed. A folder that is not there lists nothing.
QVector<Listed> list(const WatchFolder& w, QStringList* folders = nullptr);

// Lists the watched folders and tells which files are new and complete. Between scans it remembers what it saw: a file
// still being written (recorders write for minutes) is taken once its size and time stay unchanged for settleMs, a
// file that could not be read only after it changed. Without an earlier scan (`watch` from the command line) the
// file's modification time alone decides. Not thread-safe: one scan at a time.
class Scanner {
public:
    explicit Scanner(int settleMs = kSettleMs) : m_settleMs(settleMs) {}
    void setSettleMs(int ms) { m_settleMs = ms; }
    int settleMs() const { return m_settleMs; }

    struct Found {
        QString path;
        int watch = 0; // index into the list scan() was given
    };
    struct Result {
        QVector<Found> ready; // new and complete: probe and import these
        QStringList pending;  // new, but still changing or too fresh: look again in a moment
        QStringList folders;  // folders listed (subfolders included): what to watch for changes
    };
    // inPool(pathKey) = already in the project
    Result scan(const QVector<WatchFolder>& watches, const std::function<bool(const QString& key)>& inPool,
                const QDateTime& now = QDateTime::currentDateTimeUtc());
    // Probing failed (an MP4 whose index is written last, a broken file): left alone until it changes
    void rejected(const QString& path);
    // What the last scan listed (paths, sizes, times): unchanged = nothing new to look at
    QByteArray fingerprint() const { return m_fingerprint; }

private:
    struct Seen {
        qint64 size = -1;
        qint64 modified = 0;       // ms since the epoch
        qint64 unchangedSince = 0; // ms: since when size and time are as they are
        qint64 appeared = 0;       // ms: the file came while watching (0 = it was there when the folder was first listed)
        bool rejected = false;
    };
    int m_settleMs;
    QHash<QString, Seen> m_seen; // by pathKey
    QSet<QString> m_listed;      // watch folders listed before (pathKey): files new to a listed folder have appeared
    QByteArray m_fingerprint;
};

} // namespace WatchFolders
