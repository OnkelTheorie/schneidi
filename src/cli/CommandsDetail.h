#pragma once
// Shared helpers of the schneidi-cli commands (Commands.cpp: projects, table, arguments; EditOps.cpp: edit
// operations; Analysis.cpp: looking at media). Not part of the public interface in Commands.h.

#include "cli/Commands.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QLockFile>

#include <memory>

namespace Cli::detail {

[[noreturn]] void fail(const QString& code, const QString& message);
QString absolute(const QString& path);

// ---------- Times ----------

// Frames from 120 (frames), "120" (frames), "4.8s" (seconds) or "00:00:04:20" (timecode)
int parseTime(const QJsonValue& v, const ProjectFormat& fmt, const QString& what);
QString tc(int frames, const ProjectFormat& fmt);
double seconds(int frames, const ProjectFormat& fmt);
// [from, to) as frames, seconds and timecode
QJsonObject range(int from, int to, const ProjectFormat& fmt);
ProjectFormat parseFormat(const QJsonObject& a);
QJsonObject formatJson(const ProjectFormat& f);

// ---------- Projects ----------

// A loaded project with an editor. Writing sessions hold the project's write lock (ProjectFile::lock) from
// loading until they end, so the app cannot save in between; save() writes back with a backup copy first.
struct Session {
    enum class Access { Read, Write };

    QString path;
    std::unique_ptr<QLockFile> lock;
    Project project;
    Selection selection;
    Editor editor{&project, &selection};

    explicit Session(const QString& file, Access access = Access::Read);
    const ProjectFormat& format() const { return project.format(); }
    QString save(); // path of the backup copy (empty = none)
    // Media in the project (imported on first use); throws if the file cannot be read
    const MediaInfo& media(const QString& file);
};

QString trackName(TrackRef r);
// "V1", "A2" or a number (1 = first track); returns the index (0-based)
int parseTrack(const QJsonValue& v, TrackKind* kind = nullptr);
QJsonObject timelineJson(const Project& p);
QJsonObject projectJson(const Project& p, const QString& path);

// ---------- Edit operations (EditOps.cpp) ----------

extern const QString kEditHelp;
void applyOp(Session& s, const QJsonObject& op);

// ---------- Analysis (Analysis.cpp) ----------

QJsonObject cmdProbe(const QJsonObject& a, Context& ctx);
QJsonObject cmdSilence(const QJsonObject& a, Context& ctx);
QJsonObject cmdScenes(const QJsonObject& a, Context& ctx);
QJsonObject cmdFrames(const QJsonObject& a, Context& ctx);
QJsonObject cmdTranscribe(const QJsonObject& a, Context& ctx);
QJsonObject cmdExtensions(const QJsonObject& a, Context& ctx);

} // namespace Cli::detail
