// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Finding moved media files again: which folders to look in (near their old place, or a folder the user picked), the
// bounded walk through them, and pairing the files found with the missing ones. Applying the pairs to a project:
// ProjectFile::applyRelink (file) and Project::relinkMedia (the open project, one undo step).
// Pure core: no widgets; search() runs in a worker thread (the app) or right away (CLI, ProjectFile::relink).

#include "core/Types.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace Relink {

// A file of a wanted name found by the walk (size in bytes, -1 = unreadable)
struct Found {
    QString path;
    qint64 size = -1;
};

// A folder to walk and how many levels below it (0 = only its own files)
struct Root {
    QString dir;
    int depth = 0;
};

// Near = looked for by itself (next to the old places, at opening or "Find Missing Media"): only clear matches.
// Folder = in a folder the user picked: also a file of the same name whose size/length differ (re-encoded), as before.
enum class Mode { Near, Folder };

// Bounds of a walk: folders visited at most, time at most (0 = none)
struct Limits {
    int maxDirs = 0;
    int timeMs = 0;
};
// Near: a few seconds and some thousand folders at most (a parent folder may be a whole disk of recordings)
inline constexpr Limits kNearLimits{20000, 8000};
inline constexpr int kNearDepth = 4;
// A folder the user picked: as deep as it goes, until cancelled
inline constexpr Limits kFolderLimits{0, 0};
inline constexpr int kFolderDepth = 64;

// Shared with the walk while it runs (another thread): folders done so far, and a request to stop
struct Progress {
    std::atomic<int> dirs{0};
    std::atomic<bool> cancel{false};
};

// The folders near the old places of the missing files, each once: first every old folder (the file moved into a
// sub-folder), then every old parent folder (moved into a sibling folder or one level up: clipi/cs2 ->
// clipi/Alt/cs2), each with kNearDepth levels. Folders not there are left out, and so are the filesystem's root and the
// home folder itself (far too much to walk and nothing "near").
QVector<Root> nearRoots(const QStringList& missing);

// The files of these names below the roots: no hidden files or folders, never through a linked folder (no loops), no
// folder twice. truncated = stopped by the limits or cancelled before the end.
QVector<Found> find(const QVector<Root>& roots, const QSet<QString>& names, const Limits& limits,
                    Progress* progress = nullptr, bool* truncated = nullptr);

// Pairs missing media with files found (old path -> new path). A pair needs the same file name; a file of another kind
// (picture/still/sound, as `probed` says) never fits. Ranked by how sure it is - the same size (MediaInfo::fileSize),
// else the same length - then by how many folders at the end of both paths match (camA/C0001.MP4 belongs to
// .../camA/C0001.MP4). Of equally good pairs that compete (C0001.MP4 on two camera cards) none is taken: the file stays
// offline rather than showing wrong material. Near also leaves out files whose size or length differ, and unreadable
// ones. `probed`: new path -> its probe (empty = not probed: kind and length are not checked). `taken`: paths already in
// the Media Pool (never a second entry of one file).
QHash<QString, QString> match(const QVector<MediaInfo>& missing, const QVector<Found>& found, Mode mode,
                              const QHash<QString, MediaInfo>& probed, const QSet<QString>& taken = {});

// What a search found: the pairs, the probes of the files paired, how far it got
struct Result {
    QHash<QString, QString> moved;
    QHash<QString, MediaInfo> probed;
    int dirs = 0;
    bool truncated = false;
};
// The whole search in one call (a worker thread may run it): walk, probe the files of wanted names (probe empty = not
// probed), pair them
Result search(const QVector<MediaInfo>& missing, const QSet<QString>& taken, const QVector<Root>& roots, Mode mode,
              const Limits& limits, const std::function<MediaInfo(const QString&)>& probe, Progress* progress = nullptr);

} // namespace Relink
