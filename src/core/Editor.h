#pragma once
#include "core/Multicam.h"
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

    // Clip + linked partners (if "Linked Selection" is on); never clips on locked tracks
    QVector<int> withLinked(const QVector<int>& ids) const;
    // Only clips that can be edited (not on locked tracks)
    QVector<int> editable(const QVector<int>& ids) const;
    bool isTrackLocked(TrackRef ref) const;

    // Places media: video on V[track], audio on A[track] (like DaVinci, V2 <-> A2).
    // Missing tracks are created. Several files end up one after another.
    void addMediaAt(const QStringList& paths, int frame, int track = 0);
    // Title (5 s, like DaVinci "Text") at the frame on the lowest video track that is free above all clips there
    // (missing track is created); track >= 0 = fixed track (drag from the Media Pool). New clip is selected.
    void addTitle(int frame, int track = -1);
    void toggleTrackMute(TrackRef ref);
    void toggleTrackHidden(TrackRef ref);
    // Lock track (lock like DaVinci): clips on it can no longer be selected or changed,
    // Ripple leaves the track alone. Selected clips of the track are deselected.
    void toggleTrackLock(TrackRef ref);
    // Track name (empty = default "Video 1" etc.) and track color (id from kTrackColors, empty = default)
    void renameTrack(TrackRef ref, const QString& name);
    void setTrackColor(TrackRef ref, const QString& colorId);
    // EQ and compressor of audio track `track` (Mixer "EQ"/"DYN", dialogs), values clamped to their ranges; the
    // preview takes them live. Same mergeKey = one undo step (dragging a control).
    void setTrackEq(int track, const TrackEq& eq, const QString& mergeKey = {});
    void setTrackDynamics(int track, const TrackDynamics& dynamics, const QString& mergeKey = {});
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
    // Trim mode + , / . like DaVinci: slip selected clips by `frames`
    void slipSelection(int frames);
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
    // Move selection by `frames` (nudge, like , and . in DaVinci)
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
    void toggleMarker(int frame);
    // In/out point (I/O like DaVinci); -1 = remove. If out is before in, the other point is discarded.
    void setMarkIn(int frame);
    void setMarkOut(int frame);
    void clearMarks();

    // Transitions (Cross Dissolve, Ctrl+T) with default length 1 s: at both edges of the selected clips,
    // without a selection at the cut nearest to the playhead. Too few handles -> shorter.
    // style set (double-click in the Effects Library): this style, existing ones are replaced;
    // onlyKind = only edges on video or audio tracks. If a transition is selected, it gets the style.
    void addTransitions(int frame, std::optional<TransitionStyle> style = {}, std::optional<TrackKind> onlyKind = {});
    // Transition at a cut (drop from the Effects Library): leftId ends, rightId starts there
    // (0 = none -> fade in/out). Default length 1 s, replaces an existing one; one undo step.
    void addTransitionAt(int leftId, int rightId, const TransitionStyle& style);
    // Like addTransitionAt, but changes nothing: effective transition (for the preview while dragging)
    std::optional<TimelineOps::TransitionSpan> previewTransitionAt(int leftId, int rightId,
                                                                   const TransitionStyle& style) const;
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

    // Source in/out (I/O in the source viewer) of a Media Pool clip, -1 = remove; rules like setMarkIn/setMarkOut
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
    // Overwrite source range [in, out] at frame `frame` on track index `track` (V[n]/A[n]) (drag from the viewer)
    void placeSourceRange(const QString& path, int in, int out, int frame, int track);
    // Target tracks for F9/F10 ... (like DaVinci Destination Controls), index 0 = V1/A1
    int targetVideoTrack() const { return m_targetVideo; }
    int targetAudioTrack() const { return m_targetAudio; }
    void setTargetTracks(int video, int audio);
    // Effects (Effects Library: Open FX on video clips, Audio FX on audio clips): add to the clips of the effect's
    // kind (clips that have it already keep it), or remove; one undo step each
    void addEffect(const QVector<int>& ids, const QString& effectId); // also "lut:<path>" (EffectFolders::LutPrefix), "preset:<path>" (Presets::Prefix)
    void removeEffect(const QVector<int>& ids, const QString& effectId);
    // Every effect of the clips (Effects Library, Color page grade/LUT) in one undo step; the Green Screen of the
    // Video tab stays
    void removeAllEffects(const QVector<int>& ids);
    // Kind of clip an effect id goes on (audio effects, else video: filters, LUTs, presets)
    static TrackKind effectKind(const QString& effectId);
    // Target of a double click in the Effects Library: selected clips of the kind, else the top video clip (audio:
    // the clip on the first audio track) at the playhead
    QVector<int> effectTargets(int frame, TrackKind kind = TrackKind::Video) const;

    // Color page (effect "grade", target like effectTargets): set values at timeline frame `frame`; creates the correction
    // if needed, animated values get a keyframe there (as in the Inspector). Same mergeKey = one undo step.
    void setGradeValues(const QVector<int>& ids, const QVector<QPair<AnimParam, double>>& values, int frame,
                        const QString& text, const QString& mergeKey = {});
    // Reset: params = these values to the default (at the frame), empty = whole correction including LUT/keyframes removed
    void resetGrade(const QVector<int>& ids, const QVector<AnimParam>& params, int frame, const QString& text);
    void setGradeLut(const QVector<int>& ids, const QString& path); // empty = remove LUT
    void setGradeEnabled(const QVector<int>& ids, bool on);
    // Keyframe for the whole correction (like a DaVinci node): set or remove all values at the frame
    void setGradeKeyframe(const QVector<int>& ids, int frame, bool on);

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
    // Move (also onto other subtitle tracks); overwrites what is there (like clips)
    void moveSubtitles(const QVector<int>& ids, int delta, int trackDelta);
    int clampSubtitleTrackDelta(const QVector<int>& ids, int trackDelta) const;
    // Drag edge: limited to the neighbors and at least 1 frame
    int clampSubtitleTrim(int id, TimelineOps::Edge edge, int delta) const;
    void trimSubtitle(int id, TimelineOps::Edge edge, int delta);
    // Imported entries (SRT) as a new track; name = track name (e.g. file name). Returns the track index, -1 = empty.
    // stack >= 0: always visible, `stack` lines above the default position (several speakers at once)
    int importSubtitles(const QVector<SubtitleCue>& cues, const QString& name, int stack = -1);

    // ---- Compound clips and nested timelines (core/EditorCompound.cpp) ----
    // Combine selected clips (with partners) into a compound clip (DaVinci "New Compound Clip"): new
    // sequence with these clips (tracks from the lowest used one, time from the earliest clip); in the timeline
    // a compound clip replaces them over the whole span on the lowest video or audio track from the selection on on
    // which the span is free (video + audio linked, never overwrites other clips). One undo step; new
    // clip is selected. Returns the sequence id, 0 = nothing selected.
    int createCompoundClip(const QString& name = {});
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

signals:
    void targetTracksChanged(); // target tracks (redraw track header marking)
    void mixerOnlyEdit();       // next change affects only the mixer (live preview, without rebuild)

private:
    TimelineOps::SourceLength sourceLength() const;
    // Copy of the timeline with a transition at the cut (see addTransitionAt); nullopt = clips not found
    std::optional<Timeline> withTransitionAt(int leftId, int rightId, const TransitionStyle& style,
                                             TrackRef* where = nullptr) const;

    // Place source range from source frame sIn (len frames) at `start` on V[vTrack]/A[aTrack] (overwrites, linked)
    // sIn/len in frames of the converted material if speed != 1 (see Clip::speed)
    void placeSource(Timeline& tl, const MediaInfo& m, int sIn, int len, int start, int vTrack, int aTrack,
                     double speed = 1.0);

    // Common flow of the speed ramp edits (see addSpeedPoint); change gets the old curve
    // and the file length and returns false if nothing changes
    void editRamp(const QString& text, int clipId, const std::function<bool(Clip&, const RetimeMap&, int)>& change);

    // Clips under the playhead or the selection (with partners), like DaVinci for Ctrl+B
    QVector<int> targetIds(int frame) const;

    struct ClipboardItem {
        Clip clip;
        TrackRef ref;
    };
    // Paste Attributes: first copied clip on a track of this kind (nullptr = none)
    static const Clip* clipboardSource(const QVector<ClipboardItem>& items, TrackKind kind);

    Project* m_project;
    Selection* m_selection;
    bool m_linkedSelection = true;
    bool m_splitOnSelectedTracks = true;
    int m_targetVideo = 0, m_targetAudio = 0;
    QVector<ClipboardItem> m_clipboard; // starts relative to the earliest clip
    std::optional<Clip> m_gradeClipboard; // copyGrade: the source clip (only its grade is used)
};
