// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/BrandKit.h"
#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/Ducking.h"
#include "core/GradeWindows.h"
#include "core/NameBlur.h"
#include "core/Multicam.h"
#include "core/ProjectFormat.h"
#include "core/Reframe.h"
#include "core/RoomTone.h"
#include "core/StreamPresets.h"
#include "core/TemplateFill.h"
#include "core/TimelineOps.h"
#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <algorithm>
#include <functional>
#include <optional>

class Project;
class Selection;
class RetimeMap;
namespace Accents {
struct Amounts;
}

// Editing operations at user level (what a click/shortcut triggers).
// The UI only calls Editor methods, never TimelineOps directly -> new tools
// are added here without touching UI and engine.
class Editor : public QObject {
    Q_OBJECT
public:
    Editor(Project* project, Selection* selection, QObject* parent = nullptr);

    Project* project() const { return m_project; }
    Selection* selection() const { return m_selection; }

    bool linkedSelection() const { return m_linkedSelection; }
    void setLinkedSelection(bool on) { m_linkedSelection = on; }
    // Cut at the playhead: on = on all tracks of the selection (also the next clip beside it),
    // off = only the selected clips. Without a selection always all clips under the playhead.
    bool splitOnSelectedTracks() const { return m_splitOnSelectedTracks; }
    void setSplitOnSelectedTracks(bool on) { m_splitOnSelectedTracks = on; }

    // Clip + linked partners (if "Linked Selection" is on); never clips on locked tracks. A kill feed copy
    // (Reframe::keepCopy) is linked to its clip but is no partner here: selecting one does not select the other.
    QVector<int> withLinked(const QVector<int>& ids) const;
    // The kill feed copies (Reframe::keepCopy, linked to their clip) that follow a clip among `ids` that is no copy
    // itself, also those among `ids`; never on locked tracks. They move, trim, split, retime and go with their clip.
    QVector<int> followingCopies(const QVector<int>& ids) const;
    // `ids` and their following copies (the edits that change a clip's time: moving, deleting, trimming, splitting,
    // retiming - whatever the "Linked Selection" setting)
    QVector<int> withCopies(const QVector<int>& ids) const;
    // withLinked plus the following copies of all of them
    QVector<int> withFollowers(const QVector<int>& ids) const { return withCopies(withLinked(ids)); }
    // Track delta for moving `ids` (TimelineOps::clampTrackDelta), where following copies above their clip may go
    // onto new tracks (moveClips adds them) instead of holding their clip back
    int clampTrackDelta(const QVector<int>& ids, TrackKind kind, int trackDelta) const;
    // Only clips that can be edited (not on locked tracks)
    QVector<int> editable(const QVector<int>& ids) const;
    bool isTrackLocked(TrackRef ref) const;

    // Places media: video on V[track], audio on A[track] (like DaVinci, V2 <-> A2).
    // Missing tracks are created. Several files end up one after another.
    void addMediaAt(const QStringList& paths, int frame, int track = 0);
    // A voice-over recording (core/Voiceover.h) from the Media Pool onto A[track] (missing tracks are created): file
    // frame 0 lies at timeline frame `fileStart` (fractional, latency already taken off), the clip starts at the
    // first file frame at or after `from` (the playhead where recording started; the pre-roll is cut off) and sits
    // there to 1/100 frame (Clip::subframe). Overwrites what lies there; the new clip is selected. One undo step.
    // Returns the clip id, 0 = file not in the pool, locked track or nothing after `from`.
    int placeRecording(const QString& path, int track, double fileStart, int from);
    // Audio clips made elsewhere (core/MusicFit: a song cut to a length, its parts with transitions) onto audio track
    // `track` (missing tracks are created), overwriting what lies there; clip `replace` (> 0, e.g. the song before it
    // was cut) is removed first. They get new ids and are selected. One undo step `text`. Returns the ids (empty =
    // locked track or nothing to place).
    QVector<int> placeAudioClips(const QVector<Clip>& clips, int track, int replace, const QString& text);
    // Title of `length` frames (<= 0: 5 s, like DaVinci "Text") at the frame on the lowest video track that is free
    // above all clips in that range (missing track is created); track >= 0 = fixed track (drag from the Media Pool),
    // overwritten only in [frame, frame + length). New clip is selected. kind: text, counter or progress bar (a
    // progress bar without length spans the whole timeline from frame 0, on a free track when `track` has clips)
    void addTitle(int frame, int track = -1, int length = 0, TitleKind kind = TitleKind::Text);
    // New length of a title from its start: shorter frees the end, longer overwrites what lies in the new range on
    // its track (like a new title there; nothing after it changes)
    void setTitleLength(int clipId, int length);
    void toggleTrackMute(TrackRef ref);
    void toggleTrackHidden(TrackRef ref);
    // Lock track (lock like DaVinci): clips on it can no longer be selected or changed,
    // Ripple leaves the track alone. Selected clips of the track are deselected.
    void toggleTrackLock(TrackRef ref);
    // Lock/unlock several tracks and subtitle tracks (by index) in one undo step, with the deselection of
    // toggleTrackLock (which uses it, as does toggleSubtitleTrackLock)
    void setTracksLocked(const QVector<TrackRef>& tracks, const QVector<int>& subtitleTracks, bool lock);
    // Lock key: the tracks of the selection (clips, subtitles, a selected transition or edit point), without a
    // selection the target tracks (V and A, the rule of selectFromPlayhead). One of them unlocked -> all get locked,
    // else all unlocked; one undo step. Pressed again right after locking a selection (nothing changed, the lock left
    // the selection empty): it unlocks those tracks again, not the target tracks.
    void toggleSelectionTracksLock();
    // Track name (empty = default "Video 1" etc.) and track color (id from kTrackColors, empty = default)
    void renameTrack(TrackRef ref, const QString& name);
    void setTrackColor(TrackRef ref, const QString& colorId);
    // EQ and compressor of audio track `track` (Mixer "EQ"/"DYN", dialogs), values clamped to their ranges; the
    // preview takes them live. Same mergeKey = one undo step (dragging a control).
    void setTrackEq(int track, const TrackEq& eq, const QString& mergeKey = {});
    void setTrackDynamics(int track, const TrackDynamics& dynamics, const QString& mergeKey = {});
    // Mixer faders and buttons of audio track `track` (volume: -1 = master), clamped to their ranges; the preview
    // takes them live. Same mergeKey = one undo step (dragging a fader). Mixer panel and CLI both go through these.
    void setTrackVolume(int track, double db, const QString& mergeKey = {});
    void setTrackPan(int track, double pan, const QString& mergeKey = {});
    void setTrackMuted(int track, bool on);
    void setTrackSolo(int track, bool on);
    // Master limiter (like a bus limiter in Fairlight): on/off; a new ceiling (dBFS, -20..0) switches it on
    void setMasterLimiter(bool on);
    void setMasterLimiterCeiling(double db);
    // Add/delete tracks (DaVinci "Add Track"/"Delete Track"), one undo step each. addTrack: new empty
    // track at index `index` (0 = very bottom for video or A1 for audio), the others move up. Own names and
    // properties stay with their track, default names ("Video 2") follow the position. The last track of a type
    // and locked tracks cannot be deleted; clips on them are deleted too. Target tracks move along.
    void addTrack(TrackKind kind, int index);
    bool canRemoveTrack(TrackRef ref) const;
    void removeTrack(TrackRef ref);
    // Move a track within its type to index `to` (dragging the track header); clips, name, mute/solo/level etc.
    // move along, target tracks follow. One undo step.
    void moveTrack(TrackKind kind, int from, int to);
    // Subtitles among `ids` (a selection of clips and subtitles) move along by the frames, on their own tracks
    void moveClips(const QVector<int>& ids, int deltaFrames, TrackKind kind, int trackDelta);
    // Move in 1/100 frame (fine position of audio clips, Clip::subframe); whole frames like moveClips
    void moveClipsFine(const QVector<int>& ids, int fine, TrackKind kind, int trackDelta);
    // Drag the edge of a clip (and linked partners), without ripple
    void trimClip(int clipId, TimelineOps::Edge edge, int delta);
    // How far the edge can actually be moved (for the live preview)
    int clampTrim(int clipId, TimelineOps::Edge edge, int delta) const;
    // Trim mode (T): build the edit for the grabbed clip (with linked partners).
    // Ripple: edge `edge`; Roll: cut at this edge (clips on both sides on the partners' tracks); Slip/Slide: clip.
    TimelineOps::TrimEdit trimEdit(TimelineOps::TrimKind kind, int clipId, TimelineOps::Edge edge = TimelineOps::Edge::End) const;
    int clampTrimEdit(const TimelineOps::TrimEdit& e, int delta) const;
    Timeline previewTrimEdit(const TimelineOps::TrimEdit& e, int delta) const; // for the live preview
    void applyTrimEdit(const TimelineOps::TrimEdit& e, int delta);
    // Split edit (Alt-drag at a cut): a roll of the cut at `edge` of the clip on its own track only; its linked
    // partners keep their cut and stay linked (J/L cut). Kill feed copies of the clips go along.
    TimelineOps::TrimEdit splitEdit(int clipId, TimelineOps::Edge edge) const;
    // J/L cut at the cut leftId|rightId (two adjacent clips, usually video): the sound of their linked audio clips.
    // pairs = (sound clip of leftId, sound clip of rightId) on each unlocked audio track where both meet; offset =
    // where their cut lies now relative to the picture cut (frames, < 0 = J: the next sound starts early, > 0 = L:
    // the previous sound runs on); lo..hi = the offsets the material allows on all tracks.
    struct SoundEdge {
        QVector<QPair<int, int>> pairs;
        int cut = 0;
        int offset = 0;
        int lo = 0, hi = 0;
    };
    std::optional<SoundEdge> soundEdge(int leftId, int rightId) const;
    // Rolls the sound cut of every pair to cut + offset (one undo step). False = no pairs or offset outside lo..hi.
    bool setSoundEdge(int leftId, int rightId, int offset);
    // Room tone (core/RoomTone.h): fills the gaps on audio track `track` with loops of quiet from their takes, one
    // undo step. Returns the new clip ids (empty = nothing changed, e.g. locked track).
    QVector<int> fillRoomTone(int track, const QVector<RoomTone::Fill>& fills);
    // Trim mode + , / . like DaVinci: slip selected clips by `frames`
    void slipSelection(int frames);
    // Slide tool + , / .: slide selected clips by `frames` between their neighbours (one edit like a slide drag)
    void slideSelection(int frames);
    // Volume of an audio clip (dB, limited to kMinVolumeDb..kMaxVolumeDb)
    void setClipVolume(int clipId, double db);
    // Normalize audio levels (DaVinci Normalize Audio Levels, sample peak): peakDb = measured peak level per
    // Audio clip (without clip volume). Set clip volume so that the peak is at targetDb; relative = all
    // clips by the same amount (loudest hits the target), otherwise each individually. Volume keyframes: shift the curve so
    // that its highest point gets the value. Silent/locked clips stay. One undo step.
    // Loudness (LUFS, BS.1770) likewise: peakDb = integrated loudness per clip; relativeRef = reference value for
    // relative (e.g. common loudness of all clips), without = loudest clip.
    void normalizeAudio(const QHash<int, double>& peakDb, double targetDb, bool relative,
                        std::optional<double> relativeRef = std::nullopt);
    // Ducking (core/Ducking.h): replaces the volume keyframes of the audio clips `ids` (the music) so they are lowered
    // during `ranges` (timeline frames where the 'under' tracks have sound, AudioAnalysis::soundRanges). Locked clips
    // stay. Returns how many clips got lowered; no clip or no range = nothing changes. One undo step.
    int duck(const QVector<int>& ids, const Ducking::Ranges& ranges, const Ducking::Params& params);
    // Audio clips for normalizing: selection (with linked partners), audio only, not locked
    QVector<int> selectedAudioClips() const;
    // Change any clip property (Inspector). Same mergeKey = one undo step.
    void modifyClips(const QVector<int>& ids, const QString& text, const std::function<void(Clip&)>& fn,
                     const QString& mergeKey = {});
    // Blade: split clip (with linked partners) or subtitle entry (only this one) at `frame`
    void bladeAt(int clipId, int frame);
    // Drag fade handle (only this clip, like DaVinci); frames is limited to the clip length
    void setClipFade(int clipId, TimelineOps::Edge edge, int frames, const QString& mergeKey = {});
    // Ctrl+B: like DaVinci without a selection all clips and subtitles under the playhead (unlocked tracks),
    // with a selection only the selected ones (see splitOnSelectedTracks). One undo step.
    void splitAtPlayhead(int frame);
    void deleteSelection();
    void rippleDeleteSelection();
    // Remove the time range [from, to) on all unlocked tracks (clips are cut at both ends); ripple = everything
    // after it moves up, otherwise a gap stays. One undo step. Used by schneidi-cli (cutting out pauses).
    void deleteRange(int from, int to, bool ripple);
    // Several ranges [from, to) in one undo step `text` (Transcript panel: removed words, pauses); ranges may overlap
    void deleteRanges(const QVector<QPair<int, int>>& ranges, bool ripple, const QString& text);
    // Ctrl+A: every clip and subtitle on unlocked tracks
    void selectAll();
    // Select clips from the playhead (DaVinci Y / Ctrl+Y / Alt+Y / Ctrl+Alt+Y): forward = playhead and right of it,
    // otherwise playhead and left of it. allTracks = all tracks, otherwise the tracks of the selection (without selection: target tracks)
    void selectFromPlayhead(int frame, bool forward, bool allTracks);
    // Edit points by keyboard (DaVinci V/U, , and .): next cut on the target tracks (not locked),
    // switch side (both -> left -> right), trim the selected cut (both = roll; one side: ripple =
    // trim mode, otherwise without moving up). nudgeEditPoint: false = no edit point selected.
    void selectNearestEditPoint(int frame);
    void cycleEditPointSide();
    bool nudgeEditPoint(int frames, bool ripple);
    // Move selection (clips and subtitles) by `frames` (nudge, like , and . in DaVinci)
    void nudgeSelection(int frames);
    // Trim start/end up to the playhead (Shift+[ / Shift+]); selection or clips under the playhead
    void trimToPlayhead(TimelineOps::Edge edge, int frame);
    void toggleSelectionEnabled();
    // Render cache clip output (like DaVinci) for the selected video clips: if all are marked, off, otherwise on.
    // Titles are not cached. State: 0 = none marked, 1 = all, 2 = mixed, -1 = no matching clips
    void toggleSelectionRenderCache();
    int selectionRenderCacheState() const;
    // Link selected clips, or unlink them if they are already linked
    void toggleLinkSelection();
    // Link `ids` together (link) or unlink them (also the CLI ops link/unlink); their kill feed copies keep following
    // them (Editor::followingCopies). One undo step, none when nothing changes.
    void setLinked(const QVector<int>& ids, bool link, const QString& text);
    // Is the clip linked to another clip (sound, picture), not only to its own kill feed copies?
    bool hasPartner(int id) const;
    void toggleMarker(int frame);
    // In/out point (I/O like DaVinci); -1 = remove. If out is before in, the other point is discarded.
    void setMarkIn(int frame);
    void setMarkOut(int frame);
    void clearMarks();

    // Transitions (Cross Dissolve, Ctrl+T) with default length 1 s: at both edges of the selected clips,
    // without a selection at the cut nearest to the playhead. Too few handles -> shorter.
    // style set (double-click in the Effects Library): this style, existing ones are replaced;
    // onlyKind = only edges on video or audio tracks. If a transition is selected, it gets the style.
    // A cut without spare material on a needed side gets none (DaVinci "insufficient handles"); trimClips =
    // ripple-trim the clips there first (with linked partners, like the trim mode) so it fits. One undo step.
    // Returns the cuts (left, right clip id) that got no transition for lack of material.
    // length > 0: the wanted length instead of the default 1 s, align: the alignment of new and existing ones (both
    // decide how much trimClips takes off)
    QVector<QPair<int, int>> addTransitions(int frame, std::optional<TransitionStyle> style = {},
                                            std::optional<TrackKind> onlyKind = {}, bool trimClips = false,
                                            int length = 0, std::optional<TransitionAlign> align = {});
    // Like addTransitions without trimClips, but changes nothing: the cuts that lack material
    QVector<QPair<int, int>> transitionsLackingHandles(int frame, std::optional<TransitionStyle> style = {},
                                                       std::optional<TrackKind> onlyKind = {}) const;
    // Transition at a cut (drop from the Effects Library): leftId ends, rightId starts there
    // (0 = none -> fade in/out). Default length 1 s, replaces an existing one; one undo step.
    // trimClips like addTransitions. false = it does not fit (nothing changed).
    bool addTransitionAt(int leftId, int rightId, const TransitionStyle& style, bool trimClips = false);
    // Like addTransitionAt, but changes nothing: effective transition (for the preview while dragging)
    std::optional<TimelineOps::TransitionSpan> previewTransitionAt(int leftId, int rightId, const TransitionStyle& style,
                                                                   bool trimClips = false) const;
    // Remove the transition between leftId and rightId (0 = black/silence) or change its length
    void removeTransition(int leftId, int rightId);
    void setTransitionLength(int leftId, int rightId, int length, const QString& mergeKey = {});
    void setTransitionStyle(int leftId, int rightId, const TransitionStyle& style, const QString& mergeKey = {});
    // Effective transitions of a track (for display)
    QVector<TimelineOps::TransitionSpan> transitions(TrackRef ref) const;

    // Keyframes (Inspector diamonds): set (on) or remove at timeline frame `frame` for the parameters,
    // each at the clip frame under the playhead (limited to the clip)
    void setKeyframes(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, bool on);
    // Interpolation (Linear/Ease) at the keyframes; params empty = all parameters (keyframe track of the timeline)
    void setKeyframeEase(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, KeyEase ease);
    // Keyframe track of the timeline: move/delete diamonds at clip frames `times` (all parameters)
    void moveKeyframes(int clipId, const QVector<int>& times, int delta);
    // params empty = all parameters (keyframe track), otherwise only these (curve editor)
    void removeKeyframes(int clipId, const QVector<int>& times, const QVector<AnimParam>& params = {});

    // Speed like DaVinci "Change Clip Speed" (Ctrl+R) for the clips (with partners). Titles/stills stay.
    // ripple = clip length follows the speed, later clips of the same tracks move up; otherwise the length stays
    // (limited to the available material). Reverse shows the same section backwards; keyframes move along.
    struct Retime {
        double speed = 1.0;
        bool reverse = false, freeze = false, keepPitch = true;
        std::optional<FrameInterp> interpolate; // in-between pictures for slow motion; unset = keep the clip's
    };
    void setClipSpeed(const QVector<int>& ids, const Retime& r, bool ripple);

    // Speed ramp like DaVinci "Retime Controls" on the clip (with linked partners of the same file), one undo step each.
    // Start/end and keyframes stay at their source position, the clip length follows the speed (later clips of the same
    // tracks move up). frame = timeline frame in the clip; section 0 lies before the first speed point.
    bool canRetime(int clipId) const;
    void addSpeedPoint(int clipId, int frame);
    void removeSpeedPoint(int clipId, int index);
    void moveSpeedPoint(int clipId, int index, int delta); // move point by delta timeline frames, speed before it stays
    void setSegmentSpeed(int clipId, int segment, double speed);
    void setSpeedPointSmooth(int clipId, int index, int frames);
    // Slow motion: what fills the time between two source frames (repeat, blend, computed motion), one undo step
    void setFrameInterpolation(const QVector<int>& ids, FrameInterp mode);

    // Look at a file before importing it (click in the Media Storage): the source viewer shows it, In/Out and markers
    // set there stay here, not in the project. The first edit from it (sourceEdit, placeSourceRange) imports it into
    // the Media Pool first (bin `info.bin`, with these marks) – the import is no undo step, like every import; the edit
    // is one. Imported another way meanwhile (double-click in the Media Storage): the marks go to the Media Pool clip
    // if it has none, and the preview ends. Empty path = no preview.
    void setPreviewSource(const MediaInfo& info);
    const MediaInfo& previewSource() const { return m_preview; }
    // The source clip of `path`: the Media Pool clip, else the preview source with that path, else nullptr
    const MediaInfo* sourceMedia(const QString& path) const;
    // Imports the preview source into `bin` if `path` is it (and readable); true if `path` is in the Media Pool now
    bool importPreviewSource(const QString& path, int bin);

    // Source in/out (I/O in the source viewer) of a Media Pool clip (or the preview source), -1 = remove; rules like
    // setMarkIn/setMarkOut
    void setSourceMarkIn(const QString& path, int frame);
    void setSourceMarkOut(const QString& path, int frame);
    void clearSourceMarks(const QString& path);
    // Source marker (M in the source viewer, multicam sync by marker): set at the source frame or remove the one there.
    // Like source in/out not in the undo history, but a change of the project.
    void toggleSourceMarker(const QString& path, int frame);

    // Edits from the source viewer like DaVinci (Edit menu, F9-F12)
    enum class SourceEditMode {
        Insert,          // F9: insert at timeline in or playhead, later clips of the target tracks move up
        Overwrite,       // F10: overwrite
        Replace,         // F11: replace clip under the playhead (length stays, source playhead/in coincides with the playhead)
        PlaceOnTop,      // F12: onto the first free track above all clips in the range
        RippleOverwrite, // Shift+F10: replace clip under the playhead, rest of the track moves by the length difference
        AppendAtEnd,     // Shift+F12: to the end of the timeline
        FitToFill,       // Shift+F11: source in/out via speed exactly between timeline in and out (overwrites)
    };
    // 3-point edit: source range = source in/out (if one is missing: clip start/end), target = timeline in, otherwise playhead;
    // timeline in+out limits the length, timeline out only = backwards from out. Used timeline in/out are
    // removed afterwards (like DaVinci). srcPos = source playhead (for Replace without source in).
    // Video/audio linked onto the target tracks, one undo step. Return value: end of the new clip (new playhead),
    // -1 = nothing happened (no clip under the playhead, too little material).
    int sourceEdit(SourceEditMode mode, const QString& path, int srcPos, int playhead);
    // Overwrite source range [in, out] at frame `frame` on track index `track` (V[n]/A[n]) (drag from the viewer);
    // speed != 1: the same file frames retimed (speed 0.5 = twice as long). Returns the timeline frames it took (0 =
    // nothing placed)
    // audioOnly: its sound only (op 'sfx': a video file as an effect sound leaves the picture alone)
    int placeSourceRange(const QString& path, int in, int out, int frame, int track, double speed = 1.0, bool audioOnly = false);
    // Target tracks for F9/F10 ... (like DaVinci Destination Controls), index 0 = V1/A1
    // Import presets for files with several audio streams (core/StreamPresets.h): used by every edit that makes
    // clips from files; changing them leaves the timeline as it is (not in the undo)
    void setStreamPresets(const StreamPresets& presets) { m_streamPresets = presets; }
    const StreamPresets& streamPresets() const { return m_streamPresets; }
    // First audio track for the placed streams of file m (one track each, from there on) when an edit puts them over
    // [start, start + length) and targets audio track aTrack (V2 -> A2): aTrack, unless one of the tracks from there
    // on is named after another stream of the file (a second copy on V2 would put the Mix on the "Spotify" track of
    // the first one). Then tracks named like the streams that are free there, else the first free tracks below
    // whose names fit (new ones at the bottom), so names stay true and nothing is overwritten.
    int streamTrackStart(const Timeline& tl, const MediaInfo& m, int aTrack, int start, int length) const;
    // The audio track of each placed stream of file m (index = StreamPresets::placements(m), -1 = not placed: locked)
    // for such an edit with the picture on video track vTrack (-1 = none):
    // - over material (overwrite, replace, a drop onto clips): the streams take the tracks of the sound linked to
    //   the pictures they cover (same file and stream first), the rest one after another from streamTrackStart as
    //   before, so nothing of the old clip is left under the new picture;
    // - into empty space: one after another from streamTrackStart while that fits; when the file's streams already
    //   have tracks (a clip of the stream, or the track named after it) that are free there but that layout would
    //   pass them by (tracks deleted or moved since: AI test run 2026-11-07 made five new tracks for every further
    //   piece), each stream goes to its own track again, the rest to free unnamed tracks (new ones at the bottom).
    QVector<int> streamTracks(const Timeline& tl, const MediaInfo& m, int aTrack, int start, int length,
                              int vTrack = -1) const;
    int targetVideoTrack() const { return m_targetVideo; }
    int targetAudioTrack() const { return m_targetAudio; }
    void setTargetTracks(int video, int audio);
    // Effects (Effects Library: Open FX on video clips, Audio FX on audio clips): add to the clips of the effect's
    // kind (clips that have it already keep it), or remove; one undo step each
    void addEffect(const QVector<int>& ids, const QString& effectId); // also "lut:<path>" (EffectFolders::LutPrefix), "preset:<path>" (Presets::Prefix)
    void removeEffect(const QVector<int>& ids, const QString& effectId);
    // "Stimme säubern" in one click (Mixer button, CLI op clean_voice): voice cleaning (engine/VoiceClean) on the audio
    // clips among ids (those that have it keep it) with the given values (keys of the "voiceclean" effect: amount,
    // gate, clicks, shape; switches it on); one undo step. Returns the clips it is on now
    QVector<int> cleanVoice(const QVector<int>& ids, const QVariantMap& values = {});
    // The unlocked clips on audio track `index` (whole track at once: a voice-over track, a game clip's voice chat)
    QVector<int> audioTrackClips(int index) const;
    // Every effect of the clips (Effects Library, Color page grade/LUT) in one undo step; the Green Screen of the
    // Video tab stays
    void removeAllEffects(const QVector<int>& ids);
    // Effect order: the movable effects (EffectRegistry::movable) render in the clip's order, after the fixed stages.
    // Puts `effectId` at place `to` among them (0 = rendered first, past the end = last) in every clip that has it;
    // the fixed stages keep their place. One undo step; false = nothing moved (a fixed stage, no clip has it, or it
    // is there already)
    bool moveEffect(const QVector<int>& ids, const QString& effectId, int to);
    // The same by `steps` places per clip (-1 = one earlier, up in the Inspector), stopping at the ends
    bool moveEffectBy(const QVector<int>& ids, const QString& effectId, int steps);
    // Kind of clip an effect id goes on (audio effects, else video: filters, LUTs, presets)
    static TrackKind effectKind(const QString& effectId);
    // Target of a double click in the Effects Library: selected clips of the kind, else the top video clip (audio:
    // the clip on the first audio track) at the playhead
    QVector<int> effectTargets(int frame, TrackKind kind = TrackKind::Video) const;
    // Accents (core/Accents.h: zoom punch, flash, shake as keyframes) at timeline frames, one undo step: on the video
    // clips among `ids`, or with ids empty on every enabled media clip of a visible video track under each frame.
    // Returns the frames that landed on a clip (sorted).
    QVector<int> addAccents(const QVector<int>& ids, const QVector<int>& frames, const Accents::Amounts& amounts,
                            const QString& text);

    // Color page (effect "grade", target like effectTargets): set values at timeline frame `frame`; creates the correction
    // if needed, animated values get a keyframe there (as in the Inspector). Same mergeKey = one undo step.
    void setGradeValues(const QVector<int>& ids, const QVector<QPair<AnimParam, double>>& values, int frame,
                        const QString& text, const QString& mergeKey = {});
    // Reset: params = these values to the default (at the frame), empty = whole correction including LUT/keyframes removed
    void resetGrade(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, const QString& text);
    void setGradeLut(const QVector<int>& ids, const QString& path); // empty = remove LUT
    void setGradeEnabled(const QVector<int>& ids, bool on);
    // Keyframe for the whole correction (like a DaVinci node): set or remove all values at the frame.
    // window = 0: the grade's own values, 1..GradeWindows::kMax: that window's correction, shape and fill amount
    void setGradeKeyframe(const QVector<int>& ids, int frame, bool on, int window = 0);
    // Windows of the grade (shape masks, core/GradeWindows): add window n (shape "rectangle"/"ellipse", creates the
    // grade if needed; clips that already have window n keep it), remove it, set a fixed property
    // (shape, invert, fill, color – the numbers go through setGradeValues with GradeWindows::param)
    void addGradeWindow(const QVector<int>& ids, int n, const QString& shape);
    void removeGradeWindow(const QVector<int>& ids, int n);
    void setGradeWindowOption(const QVector<int>& ids, int n, const QString& key, const QVariant& value);
    // Tracking result: window n of the clip follows the points (keyframes for x/y on every point; keyframes there and
    // between `from` and the last point are replaced, `from` keeps its position as a keyframe). One undo step.
    void setGradeWindowPath(int clipId, int n, int from, const QVector<GradeWindows::PathPoint>& points);
    // Tracking result of a tracker node of the clip's "comp" graph: keyframes of n<node>.x/.y/.scale/.angle on every
    // point (keyframes between the first and the last point are replaced, others stay). One undo step. false = no
    // such clip, comp effect or tracker node, or no points
    bool trackCompNode(int clipId, int node, const QVector<Comp::TrackPoint>& points);
    // The cutout node `node` of the clip's graph uses analysis `cache` (its matte file, engine/picture/CompCutout):
    // sets the node's value "cache" as one undo step (core/EditorCutout.cpp). false + *err = no such clip or node, or
    // the node is no cutout
    bool cutoutCompNode(int clipId, int node, const QString& cache, QString* err = nullptr);

    // ---- Compositing graph of a clip (effect "comp", core/CompGraph.h; core/EditorComp.cpp) ----
    // Shared by the CLI ops (comp, node) and the Comp page. `kinds` = the node kinds (Picture::Comp::kindInfo; core
    // knows only their names): every op checks the graph it makes against them and changes nothing when it is
    // broken (false, *err in English: unknown kind, bad input, cycle). Each op is one undo step. Node values are keyed
    // without the "n<id>." prefix; the caller checks them against the kind's parameters.
    // The whole graph on video clips `ids`: replaces a graph they have (its values and keyframes go), values per node
    // id. Clips on locked tracks stay. false = nothing changed (no video clip, broken graph)
    bool setCompGraph(const QVector<int>& ids, const CompGraph& g, const QHash<int, QVariantMap>& values,
                      const CompGraph::KindInfo& kinds, QString* err = nullptr);
    // Removes the graph (effect "comp" with its values and keyframes): the picture is as without it. Returns the clips
    // that had one
    QVector<int> removeComp(const QVector<int>& ids);
    // Adds a node of `type` with `values` to the clip's graph (a clip without one gets a graph "clip -> new node").
    // after > 0: the new node goes behind node `after` - `after` feeds its input `input` and whatever read `after`
    // (other nodes, the output) reads the new node instead; after = 0 and a graph: behind the output node. Empty
    // input = the node is only added (a generator such as text or color; link it with linkCompNodes). Returns the new
    // node's id, 0 = nothing changed
    int addCompNode(int clipId, const QString& type, const QVariantMap& values, int after, const QString& input,
                    const CompGraph::KindInfo& kinds, QString* err = nullptr);
    // Sets values of a node (keys without prefix; a value of a key with keyframes changes the static value only)
    bool setCompNodeValues(int clipId, int node, const QVariantMap& values, QString* err = nullptr);
    // Removes a node with its links, values and keyframes; what read it reads the node's first connected input
    // instead (src, bg, fg …), so a chain stays a chain; the output moves the same way
    bool removeCompNode(int clipId, int node, const CompGraph::KindInfo& kinds, QString* err = nullptr);
    // on: link `l` (replaces a link already in that input); off: removes the link into l.to's input l.input
    bool linkCompNodes(int clipId, const CompLink& l, bool on, const CompGraph::KindInfo& kinds, QString* err = nullptr);
    // Makes `node` the output node
    bool setCompOut(int clipId, int node, const CompGraph::KindInfo& kinds, QString* err = nullptr);
    // Comp page: places of nodes on the canvas (CompNode::pos, no meaning for the picture)
    bool moveCompNodes(int clipId, const QHash<int, QPointF>& places, QString* err = nullptr);
    // Values of a node at timeline frame `frame` like the Inspector: a number with keyframes gets a keyframe there,
    // others change their static value. The same mergeKey = one undo step (a drag of a slider or a handle)
    bool setCompNodeValuesAt(int clipId, int node, const QVariantMap& values, int frame, const QString& mergeKey = {},
                             QString* err = nullptr);
    // Keyframe diamond of a node's number at timeline frame `frame`: on = a keyframe with `value` (the value shown
    // there; a value never set is its default, which the graph does not store), off = the keyframe there removed
    bool setCompNodeKeyframe(int clipId, int node, const QString& key, int frame, bool on, double value,
                             QString* err = nullptr);
    // A copy of node `node` with its values and keyframes (no links) at `pos`; returns its id, 0 = nothing changed
    int duplicateCompNode(int clipId, int node, const QPointF& pos, QString* err = nullptr);
    // Hide names (core/NameBlur): every clip's masks become new rectangle windows with fill "blur" or "pixelate" of
    // strength `amount`, their place and size keyed along the mask (off = size 0 and amount 0). Free window numbers
    // only; masks beyond them are left out and counted in `dropped` (their names stay visible). Clips on locked tracks
    // stay as they are: their masks are counted in `locked` (names visible too). One undo step.
    // Returns the new window numbers per clip.
    QHash<int, QVector<int>> blurNames(const QHash<int, QVector<NameBlur::Mask>>& masks, const QString& fill,
                                       double amount, int* dropped = nullptr, int* locked = nullptr);

    // Clipboard (Ctrl+C/X/V): paste at the playhead on the same tracks, overwrites
    void copySelection();
    void cutSelection();
    void paste(int frame);

    // Paste Attributes (Alt+V) like DaVinci "Paste Attributes": transfers the chosen properties (with their
    // keyframes, timing stays) from the copied clip to the selected clips, one undo step. Video attributes
    // come from the first copied clip on a video track, audio attributes from the one on an audio track. EditorAttributes.cpp
    enum PasteAttr {
        AttrZoom = 1 << 0, AttrPosition = 1 << 1, AttrRotation = 1 << 2, AttrCrop = 1 << 3,
        AttrComposite = 1 << 4, AttrEffects = 1 << 5, AttrColor = 1 << 6, AttrSpeed = 1 << 7,
        AttrVolume = 1 << 8, AttrPan = 1 << 9, AttrFades = 1 << 10, AttrAudioEffects = 1 << 11, // Audio FX
        AttrVideoMask = AttrZoom | AttrPosition | AttrRotation | AttrCrop | AttrComposite | AttrEffects | AttrColor,
        AttrAudioMask = AttrVolume | AttrPan | AttrAudioEffects,
    };
    // Which attributes the clipboard can provide (0 = nothing copied)
    int pasteAttributesAvailable() const;
    QString pasteAttributesSource() const; // name of the source clip for the dialog
    void pasteAttributes(int attrs);

    // Grade copy/paste on the Color page like DaVinci (Ctrl+C/Ctrl+V there, "=" / "-" = grade of the clip one/two
    // before). Copies the Color page grade (wheels, LUT, its keyframes) and leaves other effects alone; a source
    // without a grade resets the targets. Each paste is one undo step. EditorAttributes.cpp
    bool copyGrade(int clipId);           // false = no video clip
    bool hasCopiedGrade() const { return m_gradeClipboard.has_value(); }
    void pasteGrade(const QVector<int>& ids);
    // Grade of the video clip `back` places before `clipId` in timeline order (start, then lower track first);
    // false = there is no such clip
    bool applyGradeFromPrevious(int clipId, int back);

    // ---- Brand kit (core/BrandKit.h, 0.18) ----
    // Copies the effective kit (the project's own, else the global one) into everything that references it: titles on
    // all timelines and the styles of subtitle tracks. A missing brand colour/font keeps the stored value; values
    // without a reference stay. One undo step; nothing changes = no step (returns false).
    bool applyBrand();
    // The project's own kit (none = the global one again) and applyBrand, as one undo step; unchanged = no step
    void setProjectBrand(const std::optional<BrandKit>& brand);
    // The kit's logo as a still/video clip on a new video track on top of timeline `sequence` (0 = the open one) over
    // its whole length, at the kit's corner and size. The logo must be in the Media Pool. One undo step; returns the
    // clip id (0 = no logo, not in the pool, or an empty timeline).
    int addBrandLogo(int sequence = 0);
    // The kit's intro at the start of the open timeline (everything on unlocked tracks moves right by its length) and
    // its outro after the end, on V1/A1. Both must be in the Media Pool (one of them is enough). One undo step;
    // false = neither placed (none set, not in the pool, V1 locked).
    bool addIntroOutro();

    // ---- Subtitles (DaVinci Subtitle Tracks, core/Subtitles.h) ----
    // Subtitle entries share the selection with the clips (own ids from the same counter)
    bool isSubtitle(int id) const;
    QVector<int> clipIdsOf(const QSet<int>& ids) const; // only clips (without subtitles)
    QVector<int> selectedSubtitles() const; // selected entries, not on locked tracks
    bool isSubtitleTrackLocked(int index) const;
    // New track on top (ST1, ST2 ...), style fitting the project format; visible if no other is visible. Returns the index.
    int addSubtitleTrack();
    void removeSubtitleTrack(int index);
    // Visible in the viewer/when burning in; like DaVinci only one track at a time (on = others off)
    void setSubtitleTrackEnabled(int index, bool on);
    void toggleSubtitleTrackLock(int index);
    void renameSubtitleTrack(int index, const QString& name);
    // Track style (font, color, outline, background, position); same mergeKey = one undo step
    void setSubtitleStyle(int index, const QString& text, const std::function<void(TitleStyle&)>& fn,
                          const QString& mergeKey = {});
    // Subtitle at the frame (default 3 s, shortened before the next entry) on track `track`, < 0 = visible track
    // (none present: create ST1). Gets selected; returns the id, 0 = one already lies there / track locked.
    int addSubtitle(int frame, int track = -1, const QString& text = {});
    void setSubtitleText(int id, const QString& text, const QString& mergeKey = {});
    // Start/end (Inspector): limited to the neighbors, at least 1 frame
    void setSubtitleTiming(int id, int start, int end);
    // Move (also onto other subtitle tracks); overwrites what is there (like clips). Clips among `ids` (a selection
    // of clips and subtitles) move along in time on their tracks, in the same undo step.
    void moveSubtitles(const QVector<int>& ids, int delta, int trackDelta);
    int clampSubtitleTrackDelta(const QVector<int>& ids, int trackDelta) const;
    // Drag edge: limited to the neighbors and at least 1 frame
    int clampSubtitleTrim(int id, TimelineOps::Edge edge, int delta) const;
    void trimSubtitle(int id, TimelineOps::Edge edge, int delta);
    // Imported entries (SRT) as a new track; name = track name (e.g. file name). Returns the track index, -1 = empty.
    // stack >= 0: always visible, `stack` lines above the default position (several speakers at once)
    int importSubtitles(const QVector<SubtitleCue>& cues, const QString& name, int stack = -1);
    // Word captions (core/Captions.h) as a new track that becomes the visible one: `captions` = how the words show,
    // the text style the format's default for word captions. Returns the track index, -1 = empty.
    int addCaptions(const QVector<SubtitleCue>& cues, const QString& name, const CaptionStyle& captions);
    // The track's cues replaced by `cues` (e.g. Subtitles::readable of them) in one undo step named `text`; false =
    // no such track, locked or no cues
    bool setSubtitleCues(int index, const QVector<SubtitleCue>& cues, const QString& text);
    // How a subtitle track shows its words (lines, karaoke, word by word); same mergeKey = one undo step
    void setSubtitleCaptions(int index, const CaptionStyle& captions, const QString& mergeKey = {});

    // ---- Compound clips and nested timelines (core/EditorCompound.cpp) ----
    // Combine selected clips (with partners) into a compound clip (DaVinci "New Compound Clip"): new
    // sequence with these clips (tracks from the lowest used one, time from the earliest clip); in the timeline
    // a compound clip replaces them over the whole span on the lowest video or audio track from the selection on on
    // which the span is free (video + audio linked, never overwrites other clips). One undo step; new
    // clip is selected. Returns the sequence id, 0 = nothing selected.
    int createCompoundClip(const QString& name = {});
    // "Comp from selection" (0.19): the same compound clip, its video clip carrying a compositing graph (effect
    // "comp") that lays the inner video tracks on each other - a "clip" node per track (param track = inner index)
    // and a chain of normal merges - so the picture stays bit-identical and the graph can be rebuilt freely.
    // One undo step. Returns the sequence id, 0 = nothing selected or no video clip in the selection.
    int compFrom(const QString& name = {});
    // Decompose compound clips (with partners) back into their clips (DaVinci "Decompose in Place"): content in the
    // used section at the same position, tracks from that of the compound clip. The sequence stays in the Media Pool.
    bool decomposeCompoundClips(const QVector<int>& ids);
    // Selected compound clip (for "Open in Timeline"), 0 = none
    int selectedCompoundSequence() const;
    // Place timeline/compound clip from the Media Pool at the frame on V[track]/A[track] (overwrites, linked).
    // false = empty or loop (sequence contains the opened timeline).
    bool addSequenceAt(int sequenceId, int frame, int track = 0);

    // ---- Multicam (core/EditorMulticam.cpp, core/Multicam.h) ----
    // New multicam clip in the Media Pool (bin): one angle per entry, its files placed by their offsets (the earliest
    // recording starts at frame 0). Per angle one video track and one audio track per audio stream; files that are not
    // in the Media Pool are left out, an angle without files too. Empty name = "Multicam-Clip N". One undo step;
    // returns the sequence id, 0 = no usable angle. Placing it in a timeline: addSequenceAt.
    int createMulticamClip(const QString& name, const QVector<Multicam::Angle>& angles, int bin = 0);
    // Cut to angle `angle` at `frame` (keys 1-9, click in the grid while playing): splits the multicam segment there
    // (video and audio part together, they stay linked 1:1) and gives the part(s) of `mode` the angle. On an existing
    // edit only switches. Target: Multicam::clipAt (selection, else top-most). false = nothing changed. One undo step.
    bool multicamCut(int frame, int angle, Multicam::Mode mode);
    // Switch the segment under `frame` to the angle without a cut (Alt+1-9, click in the grid while stopped)
    bool multicamSwitch(int frame, int angle, Multicam::Mode mode);
    // Angle of the given segments (video or audio parts, the linked part follows by `mode`), one undo step
    void setMulticamAngle(const QVector<int>& ids, int angle, Multicam::Mode mode);
    // Replace the segments (with their linked parts) by the clips of their angle (DaVinci "Flatten Multicam Clip"):
    // what the segment shows, at the same place; attributes of the segment (transform, effects, volume, fades,
    // transitions at its edges) go onto them; extra audio streams go to the tracks above that are free. One undo step.
    bool flattenMulticamClips(const QVector<int>& ids);
    // Rename an angle (empty = file name), one undo step
    void renameMulticamAngle(int sequenceId, int angle, const QString& name);
    // Move all tracks of an angle in the multicam sequence by `frames` (resync; never before frame 0), one undo step
    void nudgeMulticamAngle(int sequenceId, int angle, int frames);
    // Add an angle (offsets relative to the reference = first file of angle 1; the part before frame 0 is cut off).
    // Returns its index, -1 = no multicam clip or no usable file. One undo step.
    int addMulticamAngle(int sequenceId, const Multicam::Angle& angle);
    // Is the opened timeline a multicam clip (its tracks can't be removed or reordered)?
    bool inMulticamSequence() const;

    // ---- Auto-Reframe (core/Reframe.h) ----
    // The video clips among `ids` (empty = every clip of the open timeline) fill the open timeline's shape: zoom to fill
    // plus the position (keyframes for Mode::Auto from `paths`, clip frames; a clip without a path stays centered), or
    // fitted in with a blurred copy behind (Mode::Fit). Titles, compound/multicam clips and locked tracks are left out.
    // contents: per file the part of the picture between black bars (engine/BarDetect; missing = whole picture):
    // those bars are cropped and the content fills the frame. One undo step; returns the number of clips changed.
    // keeps (given): per clip the regions kept readable above it (Reframe::keepCopy, a copy on a free track above,
    // in the same undo step); the clip's earlier kept-region copies are replaced (an empty list removes them).
    // Not given: they stay as they are.
    int reframeClips(const QVector<int>& ids, Reframe::Mode mode, const QHash<int, QVector<Reframe::Key>>& paths = {},
                     const QHash<QString, QRectF>& contents = {},
                     const QHash<int, QVector<Reframe::Keep>>* keeps = nullptr);
    // The video clips reframeClips would change. With `overlays` given and ids empty (reframing the whole
    // timeline), overlays (Reframe::isOverlay, e.g. a small webcam picture in a corner on V2) are left out and listed
    // there: they keep their place and size in the frame (as the timeline's new size converted them, scaleTimeline);
    // an explicit id of one still reframes it. reframeClips with empty ids leaves them out too.
    QVector<int> reframeTargets(const QVector<int>& ids, QVector<int>* overlays = nullptr) const;
    // `ids` and the kept-region copies (kill feed above a reframed clip, followingCopies or Reframe::isKeepAlong) of the clips among
    // them: they show the same picture, so hiding names on a clip has to reach them too
    QVector<int> withKeptCopies(const QVector<int>& ids) const;
    // The regions to keep per clip (ids as reframeClips' targets) for `choice` in the open timeline's shape;
    // `noGame` gets the clips without a known game when the choice asks for a kill feed
    QHash<int, QVector<Reframe::Keep>> reframeKeeps(const QVector<int>& ids, Reframe::Mode mode,
                                                    const QHash<QString, QRectF>& contents,
                                                    const Reframe::KeepChoice& choice, QVector<int>* noGame = nullptr) const;
    // Crops the black bars of the clips among `ids` (video clips; empty = every one of the open timeline) to their
    // content (per file, engine/BarDetect). One undo step; returns the number of clips changed.
    int cropBars(const QVector<int>& ids, const QHash<QString, QRectF>& contents);
    // Shows the clips' pictures at `aspect` (width / height; Clip::pictureAspect, 0 = the file's own shape again): a 4:3
    // game image stretched to 16:9 by the recorder gets its true shape with 4/3. Crop values keep their share of the
    // picture. With `fill` the area around the picture too (FitFill::Blur: a 4:3 picture in a 16:9 frame gets blurred
    // sides instead of black ones). One undo step; returns the number of clips changed.
    int setPictureAspect(const QVector<int>& ids, double aspect, std::optional<FitFill> fill = std::nullopt);

    // ---- Story sections and notes of the open timeline (Timeline::sections / notes, core/Story.h). EditorStory.cpp
    // Each call is one undo step. addSection: empty = done, else why not (overlap, empty range)
    QString addSection(const StorySection& section);
    bool removeSectionAt(int frame);                     // false = no section there
    void setSections(const QVector<StorySection>& sections, const QString& text); // replaces all of them
    // A dragged edge (the same mergeKey = one undo step for the whole drag); false = it would overlap or be empty
    bool setSectionRange(int index, int from, int to, const QString& mergeKey = {});
    int addNote(int frame, const QString& text);          // returns its id
    bool setNoteDone(int id, bool done);                  // false = no such note
    bool removeNote(int id);
    // A teaser: [from, to) of the open timeline copied in front of it, everything else moves back (Story::prependTeaser).
    // false = nothing in the range
    bool prependTeaser(int from, int to);

    // ---- Template placeholders (core/TemplateFill.h). EditorTemplate.cpp
    // Marks clip `clipId` of the open timeline as placeholder `slot` (an empty slot id removes the mark). One undo
    // step; empty = done, else why not (English): no such clip, locked track, the clip cannot hold that kind, the id
    // is a slot of another kind already.
    QString setClipSlot(int clipId, const ClipSlot& slot);
    // Fills every instance of slot `slotId` (all timelines) with `value`, by the slot's rules (TemplateFill::apply); a
    // file not in the Media Pool yet comes in (ctx.probe). One undo step, none when nothing could be filled.
    TemplateFill::Report fillSlot(const QString& slotId, const TemplateFill::Value& value,
                                  const TemplateFill::Context& ctx = {});
    // Several slots at once (`template fill` live in the app), one undo step
    TemplateFill::Report fillSlots(const TemplateFill::Values& values, const TemplateFill::Context& ctx = {});

signals:
    void targetTracksChanged(); // target tracks (redraw track header marking)
    void mixerOnlyEdit();       // next change affects only the mixer (live preview, without rebuild)
    void previewSourceChanged(); // preview source set, its marks changed, or it was imported

private:
    TimelineOps::SourceLength sourceLength() const;
    // createCompoundClip / compFrom (comp = the video clip gets the graph of its tracks)
    int createCompound(const QString& name, bool comp);
    // One clip's graph changed by fn (on a copy; a clip without one starts empty), checked against kinds, then stored
    // with orphan values/keyframes dropped as one undo step `text` (core/EditorComp.cpp)
    bool editComp(int clipId, const QString& text, const std::function<bool(CompGraph&, EffectInstance&, QString*)>& fn,
                  const CompGraph::KindInfo& kinds, QString* err);
    // Copy of the timeline with a transition at the cut (see addTransitionAt); nullopt = clips not found
    std::optional<Timeline> withTransitionAt(int leftId, int rightId, const TransitionStyle& style, bool trimClips,
                                             TrackRef* where = nullptr) const;
    // Copy of the timeline with the transitions of addTransitions; nullopt = nothing to do
    std::optional<Timeline> withTransitions(int frame, std::optional<TransitionStyle> style,
                                            std::optional<TrackKind> onlyKind, bool trimClips,
                                            QVector<QPair<int, int>>* lacking, int length = 0,
                                            std::optional<TransitionAlign> align = {}) const;
    // Ripple-trim the clips at the cut leftId|rightId in tl so a dissolve of `length` with `align` fits there
    // (shorter if the clips are too short for it); false = not possible (locked, no room)
    bool trimForTransition(Timeline& tl, int leftId, int rightId, TransitionAlign align, int length) const;

    // Place source range from source frame sIn (len frames) at `start` on V[vTrack]/A[aTrack] (overwrites, linked)
    // sIn/len in frames of the converted material if speed != 1 (see Clip::speed)
    // tracks: the audio track per placed stream (streamTracks, worked out before a ripple); nullptr = streamTracks now
    void placeSource(Timeline& tl, const MediaInfo& m, int sIn, int len, int start, int vTrack, int aTrack,
                     double speed = 1.0, const QVector<int>* tracks = nullptr);
    // New audio clips whose sound already plays there (TimelineOps::repeatedSound, e.g. a second copy of a moment
    // for a blurred background) come in disabled, so nothing is heard twice
    static void muteRepeatedSound(Timeline& tl, const QVector<int>& ids);

    // Common flow of the speed ramp edits (see addSpeedPoint); change gets the old curve
    // and the file length and returns false if nothing changes
    void editRamp(const QString& text, int clipId, const std::function<bool(Clip&, const RetimeMap&, int)>& change);

    // Clips under the playhead or the selection (with partners), like DaVinci for Ctrl+B
    QVector<int> targetIds(int frame) const;
    // Subtitle entries among `ids` on unlocked tracks, and their earliest start (INT_MAX = none)
    QVector<int> unlockedSubtitles(const QVector<int>& ids, int* minStart = nullptr) const;
    void setSourceMarks(const QString& path, int markIn, int markOut); // Media Pool clip or preview source
    void adoptPreviewSource(); // the preview file arrived in the Media Pool: marks along, preview ends

    struct ClipboardItem {
        Clip clip;
        TrackRef ref;
        std::optional<MediaInfo> media; // media clip: its Media Pool entry (pasting into another project adds it)
    };
    // Paste Attributes: first copied clip on a track of this kind (nullptr = none)
    static const Clip* clipboardSource(const QVector<ClipboardItem>& items, TrackKind kind);

    Project* m_project;
    Selection* m_selection;
    bool m_linkedSelection = true;
    bool m_splitOnSelectedTracks = true;
    int m_targetVideo = 0, m_targetAudio = 0;
    // What the lock key last locked from a selection, valid while the undo stack, the open timeline and the target
    // tracks stay as they were then (toggleSelectionTracksLock)
    struct KeyLocked {
        QVector<TrackRef> tracks;
        QVector<int> subtitles;
        int undoIndex = -1;
        int sequence = -1;
        int targetVideo = -1, targetAudio = -1;
    };
    KeyLocked m_keyLocked;
    StreamPresets m_streamPresets;
    QVector<ClipboardItem> m_clipboard; // starts relative to the earliest clip
    std::optional<Clip> m_gradeClipboard; // copyGrade: the source clip (only its grade is used)
    FrameRate m_clipboardRate; // frame rate the clipboards count in (clip times, keyframes)
    QSize m_clipboardSize; // resolution the clipboard's pixel values refer to (the timeline it was copied in)
    // The clipboard in the open timeline's pixels (positions, crops, title sizes scaled like scaleTimeline): copied
    // in a 16:9 edit and pasted into a 9:16 short
    QVector<ClipboardItem> clipboardHere() const;
    // Another project opened or a new frame rate: what still fits of the clipboards (keepClipboardFitting)
    void keepClipboardFitting(bool otherProject);
    MediaInfo m_preview; // setPreviewSource: a file shown in the source viewer that is not in the Media Pool
};
