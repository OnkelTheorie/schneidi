// SPDX-License-Identifier: AGPL-3.0-only
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

namespace ReviewSheet {
struct Data;
}
namespace Beats {
struct Result;
}
struct BrandKit;
struct CompRecipe;

namespace Cli::detail {

[[noreturn]] void fail(const QString& code, const QString& message);
QString absolute(const QString& path);
// "cannot read media: <path> (<why>)" for a file Engine::probe found nothing playable in (MediaProbe::whyUnreadable)
QString unreadableMessage(const QString& path);

// ---------- Times ----------

// Frames from 120 (frames), "120" (frames), "4.8s" (seconds), "00:00:04:20" (timecode) or "1:30" (clock time)
int parseTime(const QJsonValue& v, const ProjectFormat& fmt, const QString& what);
QString tc(int frames, const ProjectFormat& fmt);
double seconds(int frames, const ProjectFormat& fmt);
// [from, to) as frames, seconds and timecode
QJsonObject range(int from, int to, const ProjectFormat& fmt);
// width/height/fps of `a` over `f` (the format of a new project's start template)
ProjectFormat parseFormat(const QJsonObject& a, ProjectFormat f = {});
// A project frame rate from `fps`: a standard rate (23.976 … 60) or 48/100/120; any other fails with the list
// (`what` names the argument in that message)
FrameRate projectFrameRate(double fps, const QString& what = "fps");
// An output frame rate (`render --fps`): the project rates plus 10/12/15; any other fails with the list
FrameRate exportFrameRate(double fps);
// A content template's frame rate (from its file): an offered rate when it is one, else that rate itself (> 0)
FrameRate templateFrameRate(double fps);
QJsonObject formatJson(const ProjectFormat& f);
// Own import presets for audio streams (`info`, `templates`): mute_mix, rules {title, action, volume_db, track}
QJsonObject importPresetsJson(const StreamPresets& p);

// ---------- Projects ----------

// A new plan id (p1, p2 …) in the plan folder `dir`, its file created there exclusively (empty; the caller writes it)
QString newPlanId(const QString& dir);

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
    // Writing session on a file: its content when it was loaded. save() writes only if the file is still that: a
    // writer that did not take the same lock (another TMPDIR, so another lock folder; a copy of schneidi that
    // does not lock) must not have its changes replaced silently.
    QByteArray loadedFingerprint;
    Project& project;
    Selection selection;
    Editor editor{&project, &selection};
    bool live = false;  // the project as open in the app (inside the app, or a copy of it)
    QJsonObject app;    // live: the app's state (playhead, selection …) for `info`
    QJsonArray notes;   // hints for the AI from the ops (warnings, what changed besides the given clips)
    // The reasons the ops were given ('why' of every edit op, free text, not evaluated): [{index?, op, why}] in the
    // edit's answer. opIndex = the op of the edit list being applied (-1 = an op another command runs)
    QJsonArray why;
    int opIndex = -1;
    // The edit only shows what it would do (dry_run, plan): ops with effects outside the project (a recipe saved to
    // the library) leave those out
    bool dryRun = false;
    // How the recordings of the multicam clips made in this session were synced (sequence id -> placement); the
    // model keeps only the positions, so `synced`/`via`/`confidence` show in the answer of that edit only
    QHash<int, QVector<Multicam::Angle>> multicamSync;

    explicit Session(const QString& file, Access access = Access::Read);
    ~Session();
    // The format of the timeline being edited (its own resolution or the project's; Project::timelineFormat): what
    // renders, checks and pixel values use. The project's default is project.format().
    ProjectFormat format() const { return project.timelineFormat(); }
    // Edit/read another timeline (sequence id or name; undefined = keep). The project still opens with the
    // timeline that was open before, unless open = true.
    void selectTimeline(const QJsonValue& v, bool open = false);
    int sequenceOf(const QJsonValue& v) const; // sequence id from an id or a name; throws if there is none
    QString save(); // path of the backup copy (empty = none)
    // EMPTY for the timeline looked at, naming it and the timelines that have clips (on a file the open timeline
    // stays the user's: an AI that made a new one has to pass 'timeline')
    [[noreturn]] void failEmpty() const;
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
    // Keys the op reads besides those of its {op:'<name>', …} objects in the texts above: aliases (clip for clips),
    // linked, fields only the details name, private fields. Any other key is an error (a typo did nothing)
    QStringList moreKeys = {};
};
// moreKeys of ops that take `clips` through clipIds/clipsOn: one id as 'clip', and 'linked' (editHelp tells both)
inline const QStringList kClipKeys{"clip", "linked"};
const QVector<OpDef>& editOps();
// The {op:'…', key?, …} objects written in a text (usage strings, docs; JSON like {"op":"split","at":4} too): the
// op names of each (empty: an object without op) and its top-level keys
struct OpObject {
    QStringList ops;
    QStringList keys;
};
QVector<OpObject> opObjects(const QString& text);
// Every key the op accepts ("op" included), from its texts and moreKeys; empty = no such op
QStringList editOpKeys(const QString& name);
void addTimelineOps(QVector<OpDef>& ops); // EditOpsTimelines.cpp
void addLookOps(QVector<OpDef>& ops);     // EditOpsLook.cpp
void addAudioOps(QVector<OpDef>& ops);    // EditOpsAudio.cpp
void addEditOps(QVector<OpDef>& ops);     // EditOpsEdit.cpp
void addMulticamOps(QVector<OpDef>& ops); // EditOpsMulticam.cpp
// Ops of the 0.17 packages, one file each (filled in by the packages; registered in makeOps)
void addStoryOps(QVector<OpDef>& ops); // EditOpsStory.cpp: story sections, notes
void addMusicOps(QVector<OpDef>& ops); // EditOpsMusic.cpp: music edit
void addSfxOps(QVector<OpDef>& ops);   // EditOpsSfx.cpp: sound effects
void addTalkOps(QVector<OpDef>& ops);  // EditOpsTalk.cpp: talking head / interview
// Compositing graph (0.19, EditOpsComp.cpp): ops comp, comp_from, node, track_node
void addCompOps(QVector<OpDef>& ops);
// AI cutout (0.19, EditOpsCutout.cpp): op cutout_node
void addCutoutOps(QVector<OpDef>& ops);
// `effects kind:'nodes'`: every node kind with inputs, output and parameters (key, type, default, range, choices)
QJsonArray compNodeKindsJson();
// Compositing recipes (core/CompRecipes.h) for `effects kind:'recipes'`: id, name, slots with type and default
QJsonArray recipesJson();
QJsonArray recipeSlotsJson(const ::CompRecipe& r);
// A clip's graph for `info` and answers: nodes with their values, links [from, to, input], out, unused (nodes not
// reaching out), cost_ms (rough estimate at the format's size, ExportCheck::compCostMs)
QJsonObject compJson(const EffectInstance& comp, const ProjectFormat& f);
// op 'keyframe' on a node value ("comp.n3.size"): every clip must have that node with a number parameter of that
// key; *lo/*hi = its range (unchanged for other parameters)
void checkCompParam(Session& s, const QVector<int>& ids, AnimParam p, double* lo, double* hi);
// `frames` {clip, node?, view?}: a copy of the edited timeline that shows only the clip a["clip"] as its graph makes it
// up to node a["node"] (default: the output), a mask as gray, view 'matte' = the alpha as gray; no transform, fades
// or other clips. *start/*end = the clip's timeline range
Timeline compNodeTimeline(Session& s, const QJsonObject& a, int* start, int* end);
// Live: the beats fit_music/snap_to_beat need, analysed by schneidi-cli before the edit goes to the app (`_beats`)
QJsonObject prepareMusic(const QJsonObject& editArgs, Context& ctx); // EditOpsMusic.cpp
// The whole song file's beats (EditOpsMusic.cpp; cached per file, `given` = beats measured before, live `_beats`
// keyed by beatsKey); throws NO_AUDIO / NO_BEAT
Beats::Result songBeats(const ProjectFormat& f, const MediaInfo& m, int stream, const QJsonObject& given,
                        Context* ctx = nullptr);
QString beatsKey(const QString& path, int stream);
QJsonObject songBeatsJson(const Beats::Result& r);
// The track op 'sfx' puts its sounds on (by name); music lookups leave it out
inline const QString kSfxTrackName = QStringLiteral("SFX");
// The drops of the music on the timeline (EditOpsMusic.cpp): timeline frames (exact) and score, sorted by time; the
// music as snap_to_beat finds it (op key 'music'), beats from `_beats` or the cache
QVector<QPair<double, double>> musicDrops(Session& s, const QJsonObject& op);
// The effect sounds op 'sfx' takes (EditOpsSfx.cpp): built-in, of installed sound packs, the user's (effects folder
// "Sounds"): [{name, from:'built-in'|'pack'|'own', description?|file}]
QJsonArray sfxSounds();
// Live: what op 'sfx' on:'kills' needs, read by schneidi-cli before the edit goes to the app (`_kills`)
QJsonObject prepareSfx(const QJsonObject& editArgs, Context& ctx); // EditOpsSfx.cpp
// whisper for rough_cut's files without a transcript, here and not in the app (EditOpsTalk.cpp)
QJsonObject prepareTalk(const QJsonObject& editArgs, Context& ctx);
// Multicam (EditOpsMulticam.cpp): the angles of a multicam sequence for `info` (1-based, files with their start
// in the multicam clip; with `sync` also how they were synced) and the sync method's name in the CLI
QJsonArray multicamAnglesJson(const Sequence& s, const QVector<Multicam::Angle>* sync = nullptr);
QString syncMethodName(Multicam::SyncMethod m);
// Live: the sound sync of 'multicam' ops measured here in schneidi-cli and handed to the op (private field), so the
// app does not decode recordings for minutes while the user waits
QJsonObject withSoundSync(const QJsonObject& editArgs, Context& ctx);
// The op's 'interpolate' (in-between pictures for slow motion) onto the clips (EditOpsEdit.cpp); without it a hint when
// slowest (the slowest speed the op set) stutters
void applyInterpolate(Session& s, const QJsonObject& op, const QVector<int>& ids, double slowest);
// The edit op 'captions' (EditOpsEdit.cpp), for ops that make word captions of their own (speak)
void captionsOp(Session& s, const QJsonObject& op);
// Live: the speech of the 'speak' ops is made here in schneidi-cli (piper, whisper), the app finds the files ready;
// each op gets its voice written in (the default voice is the same in both then)
QJsonObject prepareSpeech(const QJsonObject& editArgs, Context& ctx);
// Live: the pictures Auto-Reframe ops ('reframe', new_timeline with reframe:'auto') need are measured here in
// schneidi-cli into the shared cache (engine/ReframeAnalysis), so the app does not decode while the user waits
void prepareReframe(const QJsonObject& editArgs, Context& ctx);
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
// A picture shape (EditOpsLook.cpp): '4:3', a number (width / height, 0.1..10) or 'file' (0 = the file's own shape)
double pictureAspectOf(const QJsonValue& v);
// Auto-Reframe (EditOpsLook.cpp): the video clips among ids (empty = all) fill the edited timeline's shape, following
// their subject (mode auto measures the pictures, blocking) - one undo step, a note per clip
void reframeClips(Session& s, const QVector<int>& ids, const QJsonObject& op);

// ---------- Commands on projects and the app (CommandsProject.cpp) ----------

QJsonObject cmdNew(const QJsonObject& a, Context& ctx);
QJsonObject cmdTemplates(const QJsonObject& a, Context& ctx);
QJsonObject cmdInfo(const QJsonObject& a, Context& ctx);
QJsonObject cmdImport(const QJsonObject& a, Context& ctx);
QJsonObject cmdEdit(const QJsonObject& a, Context& ctx);
QJsonObject cmdBackups(const QJsonObject& a, Context& ctx);
QJsonObject cmdRestore(const QJsonObject& a, Context& ctx);
QJsonObject cmdRender(const QJsonObject& a, Context& ctx);
// Review render (RenderReview.cpp, `render review`): the marks of timeline tl up to frame `end` (cuts, accents,
// notes, markers, sections; hits/kills from the cached highlights per recording, `review_hits`; the beat grid of
// `beats_track`, auto = the free music track). allWaves: a waveform of every audio track (the sheet), else only the
// music track's. info gets what was looked at (beats, hits_from, review_notes). The project is only read.
std::shared_ptr<const ReviewSheet::Data> reviewData(Session& s, const Timeline& tl, int end, const QJsonObject& a,
                                                    Context& ctx, bool allWaves, QJsonObject* info);
// The same on any project (the app's review render, Cli::reviewOverlay): `format`/`title` of the timeline tl
std::shared_ptr<const ReviewSheet::Data> reviewData(const Project& p, const ProjectFormat& format, const QString& title,
                                                    const Timeline& tl, int end, const QJsonObject& a, Context& ctx,
                                                    bool allWaves, QJsonObject* info);
// offsets (hits next to the nearest beat), cuts_off_beat, counts of the marks
QJsonObject reviewNumbers(const ReviewSheet::Data& d);
// The review sheet (one PNG) into out: {out, size, tiles}
QJsonObject reviewSheetRender(Session& s, const Timeline& tl, const ReviewSheet::Data& d, const QString& out,
                              Context& ctx);
QJsonObject cmdOverview(const QJsonObject& a, Context& ctx);
QJsonObject cmdCheck(const QJsonObject& a, Context& ctx);
QJsonObject cmdEffects(const QJsonObject& a, Context& ctx);
QJsonObject cmdApp(const QJsonObject& a, Context& ctx);
QJsonObject cmdWatch(const QJsonObject& a, Context& ctx); // CommandsWatch.cpp
QJsonObject cmdShort(const QJsonObject& a, Context& ctx); // CommandsShort.cpp
QJsonObject cmdBrand(const QJsonObject& a, Context& ctx); // CommandsBrand.cpp
// The kit fields of `brand set` (logo, logo_corner, logo_size, colors, fonts, intro, outro) in `a` over `k`
BrandKit brandWithFields(BrandKit k, const QJsonObject& a); // CommandsBrand.cpp
// The kit a project's own kit starts from (`p.effectiveBrand()`, maybe the global one): in a work folder without the
// files a project may not name there (cli/Sandbox.h projectFileAllowed) - the global kit's may lie anywhere
BrandKit projectKitBase(BrandKit k); // CommandsBrand.cpp
// Templates (CommandsTemplate.cpp): `templates` (start templates of CommandsProject.cpp + content templates), filling
// one into a new project (reusable: render N times), a placeholder as `info`/`templates` show it, the edit ops'
// values and context, the beats a live fill needs measured here
QJsonObject cmdStartTemplates(const QJsonObject& a, Context& ctx); // CommandsProject.cpp
QJsonObject fillTemplate(const QJsonObject& args, Context& ctx);
// The rows of one `fill` + rows share one folder for the template's files (copied once, not per row)
struct FillBatch {
    QString mediaDir;               // empty = next to each project ("<name> media/")
    QHash<QString, QString> copies; // template file -> its copy (TemplateLibrary::copyTemplateFiles)
};
QJsonObject fillTemplateInto(const QJsonObject& args, Context& ctx, FillBatch* batch);
QJsonObject slotJson(const ClipSlot& s, const ProjectFormat& f);
QString fillProblemsText(const QVector<TemplateFill::Problem>& problems); // "slot CODE: message; …"
TemplateFill::Values fillValues(const QJsonValue& values, const ProjectData& d, const ProjectFormat& f);
TemplateFill::Context fillContext(const ProjectFormat& f, const QJsonObject& beats, bool measureReframe,
                                  QStringList* notes, Context* ctx = nullptr);
QJsonObject fillBeats(const ProjectData& d, const TemplateFill::Values& values, const ProjectFormat& f, Context& ctx);
// Live: the beats of the songs a `templates` fill or the edit op 'fill' needs, measured here (`_beats`)
QJsonObject prepareTemplateFill(const QString& command, const QJsonObject& args, Context& ctx);
void addTemplateOps(QVector<OpDef>& ops); // EditOpsTemplate.cpp
// What `import` and `watch` say when a video arrives in a project whose timelines are all empty and whose frame rate
// differs from the video's (the `format` op can still match it); empty = nothing to say
QString frameRateNote(const Project& p, const QString& video);
QString presetHelp(); // the app's Deliver presets for the `render` description

// ---------- Analysis (Analysis*.cpp) ----------

// Audio tracks of `tl`: "A3", "A2,A5", numbers, track names or the audio stream names of their clips
// ("Discord,Mikrofon"); empty = none given. Throws if one is not found
QVector<int> audioTracksOf(const Project& p, const Timeline& tl, const QJsonValue& list);
// "A5 Mikrofon": an audio track with its name, or else the audio stream names of its clips
QString trackLabel(const Project& p, const Timeline& tl, int audioTrack);
// Audio streams of a file: "4", "Mikrofon", "Discord,Mikrofon" (numbers from 0 or stream names as `probe` lists them).
// Throws if one is not there
QVector<int> audioStreamsOf(const MediaInfo& info, const QString& list);
// Throws BAD_ARGUMENT if `stream` is no stream of the file or one schneidi cannot play (no decoder here, no sound in it)
void requirePlayableStream(const MediaInfo& info, int stream);

QJsonObject cmdProbe(const QJsonObject& a, Context& ctx);
QJsonObject cmdSilence(const QJsonObject& a, Context& ctx);
QJsonObject cmdScenes(const QJsonObject& a, Context& ctx);
QJsonObject cmdPeaks(const QJsonObject& a, Context& ctx);
QJsonObject cmdBeats(const QJsonObject& a, Context& ctx); // AnalysisBeats.cpp
QJsonObject cmdIndex(const QJsonObject& a, Context& ctx); // CommandsIndex.cpp
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
