#pragma once
// Project file (.schneidi, JSON): stores only references to the original files and the edit
// (tracks, clips, effects, markers) - the media themselves are never modified (like DaVinci).

#include "core/ProjectFormat.h"
#include "core/RenderJob.h"
#include "core/StreamPresets.h"
#include "core/Types.h"
#include "core/WatchFolders.h"

#include <QJsonObject>
#include <QLockFile>
#include <QString>

#include <memory>
#include <optional>

struct ProjectData {
    ProjectFormat format; // old files without this: 1920 x 1080, 25 fps
    int playhead = 0;
    QVector<MediaInfo> media;
    QVector<MediaBin> bins; // Media Pool bins (without Master)
    Timeline timeline; // opened timeline (sequence currentSequence)
    int lastClipId = 0;
    int lastLinkId = 0;
    QVector<RenderJob> renderQueue; // Deliver: render queue (optional)
    // All sequences (timelines, compound clips); the opened one gets `timeline` on loading.
    // Empty (old files) = only `timeline` as "Timeline 1".
    QVector<Sequence> sequences;
    int currentSequence = 0;
    // From a start template (core/StartTemplates.h) or Project Settings, optional: the first clip in an empty
    // timeline leaves the resolution / frame rate as they are (false = the app offers the clip's, as always)
    bool fixedResolution = false;
    bool fixedFrameRate = false;
    // The project's own import presets for audio streams (optional; none = those of the settings, all projects)
    std::optional<StreamPresets> importPresets;
    // Watch folders (core/WatchFolders.h, optional): what is watched, and the files they brought in (the newest
    // WatchFolders::kLogSize) with the revision of the newest one
    QVector<WatchFolder> watchFolders;
    QVector<WatchArrival> watchLog;
    int watchRevision = 0;
};

namespace ProjectFile {

inline constexpr const char* Extension = "schneidi";

// Paths are stored absolute and relative to the project file, so that a
// folder moved together (project + media) keeps working without relinking.
QByteArray toJson(const ProjectData& data, const QString& projectPath);
bool fromJson(const QByteArray& json, const QString& projectPath, ProjectData* data, QString* error);

// A clip as in the project file, media reference as path (e.g. for cache keys: new clip fields count
// automatically as soon as they are saved)
QJsonObject clipJson(const Clip& c);

bool save(const ProjectData& data, const QString& path, QString* error);
// Backup copies like DaVinci "Project Backups": before overwriting, the previous state moves into its
// own folder per project (under AppDataLocation/backups), the newest `keep` stay. An unchanged state
// (equal to the newest copy) is not stored twice. Returns the path of the new copy (empty = none).
QString backup(const QString& projectPath, int keep = 20);
// Backup copies of a project, newest first (full paths)
QStringList backups(const QString& projectPath);
// Folder of a project's backup copies (empty path = folder of all projects)
QString backupDir(const QString& projectPath);
bool load(const QString& path, ProjectData* data, QString* error);
// Write lock of a project file, so that the app and schneidi-cli never write it at the same time: held from
// reading until the save is done. The lock file lives in the temp folder (not next to the project); a crashed
// holder leaves a stale lock that QLockFile detects. nullptr = still locked by someone else after timeoutMs.
std::unique_ptr<QLockFile> lock(const QString& projectPath, int timeoutMs = 5000);
// Content fingerprint of a saved project file (empty = unreadable): tells own saves from changes by others
QByteArray fingerprint(const QString& path);

// Missing media: search for a file of the same name in a folder (recursively) and redirect all references.
// Returns the number of files found again.
int relink(ProjectData* data, const QString& searchDir);
QStringList missingMedia(const ProjectData& data);
// Effects this computer does not know (e.g. frei0r plugin missing, project from another computer), sorted: they stay
// in the file with their values and keyframes but have no effect
QStringList missingEffects(const ProjectData& data);
// File name from a path of either system ("/home/.../a.mp4", "C:\...\a.mp4"): backslash is not a
// separator on Linux, QFileInfo::fileName() would return the whole path there for Windows paths
QString fileNameAnyOs(const QString& path);

} // namespace ProjectFile
