#pragma once
// Shared helpers of the schneidi-cli commands (Commands.cpp: projects, table, arguments; EditOps.cpp: edit
// operations; Analysis.cpp: looking at media). Not part of the public interface in Commands.h.

#include "cli/Commands.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLockFile>

#include <functional>
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
    // Edit/read another timeline (sequence id or name; undefined = keep). The project still opens with the
    // timeline that was open before, unless open = true.
    void selectTimeline(const QJsonValue& v, bool open = false);
    int sequenceOf(const QJsonValue& v) const; // sequence id from an id or a name; throws if there is none
    QString save(); // path of the backup copy (empty = none)
    // Media in the project (imported on first use); throws if the file cannot be read
    const MediaInfo& media(const QString& file);

private:
    int m_open = 0; // sequence the app opens (restored before saving)
};

QString trackName(TrackRef r);
// "V1", "A2" or a number (1 = first track); returns the index (0-based)
int parseTrack(const QJsonValue& v, TrackKind* kind = nullptr);
QJsonObject timelineJson(const Project& p);
QJsonObject projectJson(const Project& p, const QString& path);

QJsonArray timelinesJson(const Project& p);
QJsonObject titleStyleJson(const TitleStyle& t, const TitleStyle& def);

// ---------- Edit operations (EditOps*.cpp) ----------

// One `op` of the edit command; usage/summary go into the parameter description the AI reads, details into
// `help edit`
struct OpDef {
    QString name;
    QString usage;   // e.g. "{op:'split', at, clip?}"
    QString summary; // one line, may be empty
    QString details; // longer description (parameter names, ranges), may be empty
    std::function<void(Session&, const QJsonObject&)> run;
};
const QVector<OpDef>& editOps();
void addTimelineOps(QVector<OpDef>& ops); // EditOpsTimelines.cpp
void addLookOps(QVector<OpDef>& ops);     // EditOpsLook.cpp
void addAudioOps(QVector<OpDef>& ops);    // EditOpsAudio.cpp
void addEditOps(QVector<OpDef>& ops);     // EditOpsEdit.cpp
QJsonObject transitionStyleJson(const TransitionStyle& st);
// Name of an animatable value as the ops use it ("x", "opacity", "grade.liftY") and a clip's keyframes by name
// (times in timeline frames)
QString paramName(AnimParam p);
QJsonObject keyframesJson(const Clip& c);
QString editHelp();
QJsonArray editOpsHelp();
void applyOp(Session& s, const QJsonObject& op);

// Helpers for the operations
const Clip& clipOf(Session& s, const QJsonObject& op, const char* key = "clip");
// Clip ids of `clips` (or `clip`) with linked partners unless linked:false; cues = subtitle ids allowed too
QVector<int> clipIds(Session& s, const QJsonObject& op, bool cues = false);
// Only the clips on video (or audio) tracks; fails if none is left
QVector<int> onTracks(Session& s, const QVector<int>& ids, TrackKind kind, const char* what);
int timeOf(Session& s, const QJsonObject& op, const char* key, int fallback = -1);
double numberOf(const QJsonObject& op, const char* key, double lo, double hi);
QColor colorOf(const QJsonValue& v, const QString& what);

// ---------- Analysis (Analysis.cpp) ----------

QJsonObject cmdProbe(const QJsonObject& a, Context& ctx);
QJsonObject cmdSilence(const QJsonObject& a, Context& ctx);
QJsonObject cmdScenes(const QJsonObject& a, Context& ctx);
QJsonObject cmdFrames(const QJsonObject& a, Context& ctx);
QJsonObject cmdTranscribe(const QJsonObject& a, Context& ctx);
QJsonObject cmdLoudness(const QJsonObject& a, Context& ctx);
QJsonObject cmdExtensions(const QJsonObject& a, Context& ctx);

} // namespace Cli::detail
