// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core/ProjectFile.h"
#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <QUndoStack>
#include <functional>

// Empty timeline with the default tracks (new project, new timeline)
Timeline emptyTimeline();

// Organization of a Media Pool clip (part of MediaInfo), remembered separately for undo
struct MediaOrg {
    int bin = 0;
    QString color;
    QStringList flags;
    bool operator==(const MediaOrg&) const = default;
};
// State of the Media Pool organization (bins + organization per path) - undo snapshot
struct PoolState {
    QVector<MediaBin> bins;
    QHash<QString, MediaOrg> media;
    QHash<int, int> sequenceBins; // sequence id -> bin (timelines/compound clips in the Media Pool)
    bool operator==(const PoolState&) const = default;
};
// What undo steps change in the media list (removing from the Media Pool, files of watch folders): files that leave
// the pool or come into it at their place in the list, and files put on or taken off a watch folder's ignored list.
// Everything else about the media (imports, source marks, transcripts) is not in the history and stays as it is.
struct MediaChange {
    QVector<QPair<int, MediaInfo>> out;                // leave the pool (index in the list before)
    QVector<QPair<int, MediaInfo>> in;                 // come into the pool (index in the list after)
    QVector<QPair<QString, QString>> ignore, unignore; // (watch folder, file) onto / off its ignored list
    bool isEmpty() const { return out.isEmpty() && in.isEmpty() && ignore.isEmpty() && unignore.isEmpty(); }
};

// Holds the project (media + timeline). Every change of the timeline goes through edit()
// and is thus automatically undoable (snapshot undo - simple and robust;
// for very large projects possibly replace with finer commands later).
class Project : public QObject {
    Q_OBJECT
public:
    explicit Project(QObject* parent = nullptr);
    // On teardown the undo stack still reports "cleanChanged" -> do not pass it on anymore (main window is already gone)
    ~Project() override { blockSignals(true); }

    // Project settings (timeline resolution and frame rate)
    const ProjectFormat& format() const { return m_format; }
    // Whole frames per timecode second (29.97 -> 30) - for timecode and default lengths ("1 s")
    int fps() const { return m_format.rate.timebase(); }
    double frameRate() const { return m_format.rate.fps(); }
    // Like DaVinci: frame rate can only be changed as long as no timeline contains clips
    bool frameRateLocked() const;
    // One undo step; a new resolution also converts positions/sizes in all timelines (scaleTimeline).
    // A new frame rate keeps subtitles, markers and in/out at their time (retimeTimeline); a locked one stays unchanged.
    void setFormat(const ProjectFormat& format);
    void applyFormat(const ProjectFormat& format, const QVector<Sequence>& sequences); // only for undo
    // The currently opened timeline (sequence currentSequence()); all edits act on it
    const Timeline& timeline() const { return current().timeline; }
    QUndoStack* undoStack() { return &m_undo; }

    // mergeKey: consecutive edits with the same key become one undo step
    // (e.g. dragging a slider in the Inspector). closeMerge() ends the series.
    void edit(const QString& text, const std::function<void(Timeline&)>& fn, const QString& mergeKey = {});
    void closeMerge() { ++m_mergeSession; }
    // The undo steps after `fromIndex` (QUndoStack::index()) become one step called `text`, e.g. an AI chat turn the
    // user accepted: the state before them and the state now, everything the history covers (timelines, the open
    // one, project settings, Media Pool organization, files removed from or brought into the pool by those steps).
    // Nothing to combine (index <= fromIndex): no step.
    void combineSteps(int fromIndex, const QString& text);
    void setTimeline(const Timeline& tl); // only for undo/loading (current sequence)
    // Undo: set the timeline of a sequence; if it is not open, it gets opened (you see what changes)
    void setTimeline(int sequenceId, const Timeline& tl);

    // ---- Sequences: several timelines per project and compound clips (like DaVinci) ----
    // All sequences (normal timelines and compound clips) in creation order
    const QVector<Sequence>& sequences() const { return m_sequences; }
    const Sequence* sequence(int id) const;
    int currentSequence() const { return m_current; }
    QString sequenceName(int id) const;
    // Open (no undo, like DaVinci); emits currentSequenceChanged + timelineChanged
    void setCurrentSequence(int id);
    // New empty timeline (default tracks) in the bin, gets opened; empty name = "Timeline N". One undo step.
    int addTimeline(const QString& name = {}, int bin = 0);
    void renameSequence(int id, const QString& name);
    // Copy (new clip ids), name "... copy"; returns the new id
    int duplicateSequence(int id);
    // Deleting is not possible for the last normal timeline and for sequences used by a compound clip
    bool canRemoveSequence(int id) const;
    bool removeSequence(int id);
    void moveSequencesToBin(const QVector<int>& ids, int bin);
    // Is the sequence used anywhere (also in other sequences) as a compound clip?
    bool sequenceUsed(int id) const;
    // May `child` be a clip in `parent` (no loop: child does not contain parent even indirectly)?
    bool canNest(int child, int parent) const;
    int sequenceLength(int id) const; // end of the sequence (frames), 0 = empty/unknown
    // Name of a clip as in the timeline (compound clips: name of the sequence)
    QString clipName(const Clip& c) const;
    // Timeline with the content of all sequences (Timeline::nested) for rendering; id 0 = current
    Timeline renderTimeline(int sequenceId = 0) const;
    // Change several sequences in one undo step (create/dissolve compound clip ...); current = opened afterwards
    void editSequences(const QString& text, const std::function<void(QVector<Sequence>&, int& current)>& fn);
    void applySequences(const QVector<Sequence>& sequences, int current); // only for undo
    int newSequenceId() { return ++m_lastSequenceId; }

    const QVector<MediaInfo>& media() const { return m_media; }
    const MediaInfo* mediaInfo(const QString& path) const;
    void addMedia(const MediaInfo& info);
    // Source in/out of a Media Pool clip (-1 = none). Not in undo (like DaVinci), but counts as a change.
    void setMediaMarks(const QString& path, int markIn, int markOut);
    // Source markers of a Media Pool clip (sorted source frames); like setMediaMarks not in undo, but a change
    void setMediaMarkers(const QString& path, const QVector<int>& markers);
    // Transcript of one audio stream of a media file (Transcript panel); replaces the old one of that stream, empty
    // words remove it. Not in the undo history (like source marks), but counts as a change.
    void setTranscript(const QString& path, const MediaTranscript& transcript);
    // Replace media info (e.g. lengths after a new frame rate), does not count as a change
    void replaceMedia(const QVector<MediaInfo>& media);
    // After a frame rate change (from `oldRate` to the current one): media lengths, source in/out and source markers
    // counted anew. `probeLength` = a file's real length in the new rate (0 = unknown: offline files are scaled).
    // App and CLI both call this after setFormat.
    void recountMedia(FrameRate oldRate, const std::function<int(const QString& path)>& probeLength);
    // The clips that use these media, per timeline (every sequence, compound and multicam clips included): sequence
    // id -> number of clips, in sequence order; empty = no timeline uses them
    QVector<QPair<int, int>> mediaUsage(const QStringList& paths) const;
    // Remove media from the Media Pool as one undo step. Media a timeline uses only go with `withClips`: then their
    // clips leave every timeline in the same step (a gap stays); without it nothing changes and it returns false, so
    // no clip ever points to a file missing in the pool. A file of a watch folder goes on its ignored list (the watch
    // does not bring it back). Undo brings everything back as it was: place in the pool, bin, color, flags, source
    // marks, transcripts, the clips. Paths not in the pool are left out; false = nothing removed.
    bool removeMedia(const QStringList& paths, bool withClips);
    void applyMediaChange(MediaChange& change, bool forward); // only for undo (forward = as the step did it)

    // Media Pool like DaVinci: bins (Master = 0), clip colors, flags. Every change is an undo step
    // (affects only the organization, not the media list itself).
    const QVector<MediaBin>& bins() const { return m_bins; }
    const MediaBin* bin(int id) const;
    QString binName(int id) const;                // 0 = "Master"
    QVector<int> childBins(int parent) const;     // sorted by name (like DaVinci)
    bool binInside(int id, int ancestor) const;   // id == ancestor or below
    int addBin(int parent, const QString& name = {}); // default name "Bin 1", "Bin 2" ...; returns the id
    void renameBin(int id, const QString& name);
    void removeBin(int id);                       // content (clips, sub-bins) moves into the parent bin
    void moveBin(int id, int parent);
    void moveMediaToBin(const QStringList& paths, int bin, const QVector<int>& sequences = {}); // also sequences
    void setClipColor(const QStringList& paths, const QString& colorId); // empty = no clip color
    void setFlag(const QStringList& paths, const QString& flagId, bool on);
    void clearFlags(const QStringList& paths);
    PoolState poolState() const;
    void applyPool(const PoolState& state); // only for undo

    // Deliver: render queue. No undo (like DaVinci), but counts as a change (saved in the project).
    const QVector<RenderJob>& renderQueue() const { return m_renderQueue; }
    void setRenderQueue(const QVector<RenderJob>& jobs);

    // From the start template (core/StartTemplates.h) or Project Settings: the first clip in an empty timeline leaves
    // the resolution / frame rate as they are. No undo, but counts as a change (saved in the project).
    bool fixedResolution() const { return m_fixedResolution; }
    bool fixedFrameRate() const { return m_fixedFrameRate; }
    void setFixedFormat(bool resolution, bool frameRate);
    // The project's own import presets for audio streams (core/StreamPresets.h); none = those of the settings (all
    // projects). No undo, but counts as a change. effectiveImportPresets(): what applies to this project.
    const std::optional<StreamPresets>& importPresets() const { return m_importPresets; }
    void setImportPresets(const std::optional<StreamPresets>& presets);
    StreamPresets effectiveImportPresets() const { return m_importPresets ? *m_importPresets : StreamPresets::load(); }
    // Watch folders (core/WatchFolders.h): new media files of these folders come into the Media Pool on their own.
    // Setting them up is not in the undo history (like the render queue), but counts as a change.
    const QVector<WatchFolder>& watchFolders() const { return m_watchFolders; }
    void setWatchFolders(const QVector<WatchFolder>& list);
    // The files watch folders brought in (the newest WatchFolders::kLogSize, oldest first); watchRevision() = the
    // revision of the newest one (0 = none yet), it only counts up
    const QVector<WatchArrival>& watchLog() const { return m_watchLog; }
    int watchRevision() const { return m_watchRevision; }
    // New files of watch folders into the Media Pool (in their watch's bin) as one undo step; every file gets the next
    // revision in the watch log. Files already in the pool are left out. Undo takes them out of the pool again and
    // puts them on their watch's `ignored` list (a watch does not bring them back). Returns the arrivals.
    QVector<WatchArrival> addWatchedMedia(const QVector<WatchImport>& files);
    void applyWatchedMedia(const QVector<WatchImport>& files, bool in); // only for undo (in = into the pool)

    // Save/load (.schneidi). load() clears the undo history like a freshly opened project.
    ProjectData data() const;
    void load(const ProjectData& d);
    void reset(); // empty project
    bool isModified() const { return m_mediaDirty || !m_undo.isClean(); }
    void markSaved();
    void markModified();

    int newClipId() { return ++m_lastClipId; }
    int newLinkId() { return ++m_lastLinkId; }

signals:
    void timelineChanged();
    void currentSequenceChanged(); // another timeline opened (comes before timelineChanged)
    void sequencesChanged();       // sequence list/names/bins changed
    void formatChanged(); // comes before the corresponding timelineChanged()
    void mediaChanged();
    void poolChanged(); // bins/clip colors/flags changed (media list unchanged)
    void mediaMarksChanged(const QString& path); // source in/out changed (without mediaChanged)
    void transcriptChanged(const QString& path);
    void modifiedChanged(bool modified);
    void renderQueueChanged();
    void importPresetsChanged(); // own import presets set/removed, or another project loaded
    void watchFoldersChanged(); // the list of watch folders (also after load())
    void loaded(); // another project (or a new empty one) replaced everything, after the other signals of load()

private:
    ProjectFormat m_format;
    QVector<Sequence> m_sequences; // never empty
    int m_current = 1;             // id of the opened sequence
    int m_lastSequenceId = 1;
    Sequence& current();
    const Sequence& current() const;
    QVector<MediaInfo> m_media;
    QVector<MediaBin> m_bins;
    QVector<RenderJob> m_renderQueue;
    bool m_fixedResolution = false;
    bool m_fixedFrameRate = false;
    std::optional<StreamPresets> m_importPresets;
    int m_lastBinId = 0;
    QUndoStack m_undo;
    int m_lastClipId = 0;
    int m_lastLinkId = 0;
    int m_mergeSession = 0;
    void editPool(const QString& text, const std::function<void(PoolState&)>& fn);
    void sanitizePool(); // references to missing bins -> Master, resolve parent cycles
    bool m_mediaDirty = false;
    QVector<WatchFolder> m_watchFolders;
    QVector<WatchArrival> m_watchLog;
    int m_watchRevision = 0;
};
