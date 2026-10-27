#pragma once
// Shared helpers of the schneidi-cli commands (Commands.cpp: projects, table, arguments; EditOps.cpp: edit
// operations; Analysis*.cpp: looking at media). Not part of the public interface in Commands.h.

#include "cli/Commands.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"

#include <QColor>
#include <QHash>
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
// width/height/fps of `a` over `f` (the format of a new project's start template)
ProjectFormat parseFormat(const QJsonObject& a, ProjectFormat f = {});
QJsonObject formatJson(const ProjectFormat& f);
// Own import presets for audio streams (`info`, `templates`): mute_mix, rules {title, action, volume_db, track}
QJsonObject importPresetsJson(const StreamPresets& p);

// ---------- Projects ----------

// A loaded project with an editor. Writing sessions hold the project's write lock (ProjectFile::lock) from
// loading until they end, so the app cannot save in between; save() writes back with a backup copy first.
// Live (cli/Live.h): is the project open in the app, a writing session in the app process edits the app's project
// (no file, no lock; the server makes it one undo step) and a reading one in schneidi-cli gets a copy of it.
struct Session {
    enum class Access { Read, Write };

    struct Opened; // where the project comes from (Commands.cpp)

private:
    explicit Session(Opened&& o);
    std::unique_ptr<Project> m_own; // the project unless it is the app's

public:
    QString path;
    std::unique_ptr<QLockFile> lock;
    Project& project;
    Selection selection;
    Editor editor{&project, &selection};
    bool live = false;  // the project as open in the app (inside the app, or a copy of it)
    QJsonObject app;    // live: the app's state (playhead, selection …) for `info`
    QJsonArray notes;   // hints for the AI from the ops (warnings, what changed besides the given clips)
    // How the recordings of the multicam clips made in this session were synced (sequence id -> placement); the
    // model keeps only the positions, so `synced`/`via`/`confidence` show in the answer of that edit only
    QHash<int, QVector<Multicam::Angle>> multicamSync;

    explicit Session(const QString& file, Access access = Access::Read);
    ~Session();
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
    bool m_inApp = false; // edits the app's project directly
};

// A project file (.schneidi) or "@app" (the project open in the app)
bool isProjectFile(const QString& path);
QString trackName(TrackRef r);
// "V1", "A2" or a number (1 = first track); returns the index (0-based)
int parseTrack(const QJsonValue& v, TrackKind* kind = nullptr);
// A clip as `info` shows it
QJsonObject clipJson(const Project& p, const Clip& c, TrackRef ref);
QJsonObject timelineJson(const Project& p);
QJsonObject projectJson(const Project& p, const QString& path);
// "Parent/Child" of a Media Pool bin (0 = top level: "")
QString binPath(const Project& p, int bin);

// sync = how the recordings of multicam clips were synced (Session::multicamSync), if known
QJsonArray timelinesJson(const Project& p, const QHash<int, QVector<Multicam::Angle>>& sync = {});
// LUT / luma transition / effect preset by built-in id, name in the effects folder or file path; throws if none
enum class Asset { Lut, Luma, Preset };
QString findAsset(Asset kind, const QString& name);
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
void addMulticamOps(QVector<OpDef>& ops); // EditOpsMulticam.cpp
// Multicam (EditOpsMulticam.cpp): the angles of a multicam sequence for `info` (1-based, files with their start
// in the multicam clip; with `sync` also how they were synced) and the sync method's name in the CLI
QJsonArray multicamAnglesJson(const Sequence& s, const QVector<Multicam::Angle>* sync = nullptr);
QString syncMethodName(Multicam::SyncMethod m);
// Live: the sound sync of 'multicam' ops measured here in schneidi-cli and handed to the op (private field), so the
// app does not decode recordings for minutes while the user waits
QJsonObject withSoundSync(const QJsonObject& editArgs, Context& ctx);
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
// For ops on one kind of clip (effects, volume, color …): the given clips of that kind alone (Discord's volume must
// not change the other streams of the file); a clip of the other kind stands for its linked partners of this kind.
// linked:true = all linked partners of this kind, linked:false = strictly the given clips. Fails if none is left
QVector<int> clipsOn(Session& s, const QJsonObject& op, TrackKind kind, const char* what);
int timeOf(Session& s, const QJsonObject& op, const char* key, int fallback = -1);
double numberOf(const QJsonObject& op, const char* key, double lo, double hi);
QColor colorOf(const QJsonValue& v, const QString& what);

// ---------- Commands on projects and the app (CommandsProject.cpp) ----------

QJsonObject cmdNew(const QJsonObject& a, Context& ctx);
QJsonObject cmdTemplates(const QJsonObject& a, Context& ctx);
QJsonObject cmdInfo(const QJsonObject& a, Context& ctx);
QJsonObject cmdImport(const QJsonObject& a, Context& ctx);
QJsonObject cmdEdit(const QJsonObject& a, Context& ctx);
QJsonObject cmdBackups(const QJsonObject& a, Context& ctx);
QJsonObject cmdRestore(const QJsonObject& a, Context& ctx);
QJsonObject cmdRender(const QJsonObject& a, Context& ctx);
QJsonObject cmdOverview(const QJsonObject& a, Context& ctx);
QJsonObject cmdCheck(const QJsonObject& a, Context& ctx);
QJsonObject cmdEffects(const QJsonObject& a, Context& ctx);
QJsonObject cmdApp(const QJsonObject& a, Context& ctx);
QJsonObject cmdWatch(const QJsonObject& a, Context& ctx); // CommandsWatch.cpp
QString presetHelp(); // the app's Deliver presets for the `render` description

// ---------- Analysis (Analysis*.cpp) ----------

// Audio tracks of `tl`: "A3", "A2,A5", numbers, track names or the audio stream names of their clips
// ("Discord,Mikrofon"); empty = none given. Throws if one is not found
QVector<int> audioTracksOf(const Project& p, const Timeline& tl, const QJsonValue& list);

QJsonObject cmdProbe(const QJsonObject& a, Context& ctx);
QJsonObject cmdSilence(const QJsonObject& a, Context& ctx);
QJsonObject cmdScenes(const QJsonObject& a, Context& ctx);
QJsonObject cmdPeaks(const QJsonObject& a, Context& ctx);
QJsonObject cmdBeats(const QJsonObject& a, Context& ctx); // AnalysisBeats.cpp
QJsonObject cmdHighlights(const QJsonObject& a, Context& ctx); // AnalysisHighlights.cpp
QJsonObject cmdFrames(const QJsonObject& a, Context& ctx);
QJsonObject cmdTranscribe(const QJsonObject& a, Context& ctx);
QJsonObject cmdLoudness(const QJsonObject& a, Context& ctx);
QJsonObject cmdScopes(const QJsonObject& a, Context& ctx);
QJsonObject cmdExtensions(const QJsonObject& a, Context& ctx);
QJsonObject cmdSync(const QJsonObject& a, Context& ctx); // EditOpsMulticam.cpp
// Exchange.cpp: timelines to/from EDL, FCP7 XML, OpenTimelineIO
QJsonObject cmdExportTimeline(const QJsonObject& a, Context& ctx);
QJsonObject importTimeline(Session& s, const QJsonObject& a); // import with timeline_file

} // namespace Cli::detail
