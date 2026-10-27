#include "core/Project.h"

#include "core/I18n.h"
#include "core/TimelineOps.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QUndoCommand>
#include <algorithm>

namespace {

class SnapshotCommand : public QUndoCommand {
public:
    SnapshotCommand(Project* p, const QString& text, int sequence, Timeline before, Timeline after, QString mergeKey)
        : QUndoCommand(text), m_project(p), m_sequence(sequence), m_before(std::move(before)),
          m_after(std::move(after)), m_mergeKey(std::move(mergeKey)) {}
    void undo() override { m_project->setTimeline(m_sequence, m_before); }
    void redo() override { m_project->setTimeline(m_sequence, m_after); }
    int id() const override { return m_mergeKey.isEmpty() ? -1 : 1; }
    bool mergeWith(const QUndoCommand* other) override
    {
        const auto* o = static_cast<const SnapshotCommand*>(other);
        if (o->m_mergeKey != m_mergeKey || o->m_sequence != m_sequence) return false;
        m_after = o->m_after;
        return true;
    }

private:
    Project* m_project;
    int m_sequence;
    Timeline m_before, m_after;
    QString m_mergeKey;
};

// Change project settings: format and (converted) cut together, so that undo reverts both
class FormatCommand : public QUndoCommand {
public:
    FormatCommand(Project* p, ProjectFormat before, ProjectFormat after, QVector<Sequence> seqBefore,
                  QVector<Sequence> seqAfter)
        : QUndoCommand(T("Projekteinstellungen")), m_project(p), m_before(before), m_after(after),
          m_seqBefore(std::move(seqBefore)), m_seqAfter(std::move(seqAfter)) {}
    void undo() override { m_project->applyFormat(m_before, m_seqBefore); }
    void redo() override { m_project->applyFormat(m_after, m_seqAfter); }

private:
    Project* m_project;
    ProjectFormat m_before, m_after;
    QVector<Sequence> m_seqBefore, m_seqAfter;
};

// Several sequences at once (new timeline, create/dissolve compound clip ...) together with the opened sequence
class SequencesCommand : public QUndoCommand {
public:
    SequencesCommand(Project* p, const QString& text, QVector<Sequence> before, int curBefore,
                     QVector<Sequence> after, int curAfter, QUndoCommand* parent = nullptr)
        : QUndoCommand(text, parent), m_project(p), m_before(std::move(before)), m_after(std::move(after)),
          m_curBefore(curBefore), m_curAfter(curAfter) {}
    void undo() override { m_project->applySequences(m_before, m_curBefore); }
    void redo() override { m_project->applySequences(m_after, m_curAfter); }

private:
    Project* m_project;
    QVector<Sequence> m_before, m_after;
    int m_curBefore, m_curAfter;
};

// Change Media Pool organization (bins, clip colors, flags) - undoable as in DaVinci
class PoolCommand : public QUndoCommand {
public:
    PoolCommand(Project* p, const QString& text, PoolState before, PoolState after)
        : QUndoCommand(text), m_project(p), m_before(std::move(before)), m_after(std::move(after)) {}
    void undo() override { m_project->applyPool(m_before); }
    void redo() override { m_project->applyPool(m_after); }

private:
    Project* m_project;
    PoolState m_before, m_after;
};

// Files into or out of the Media Pool (Project::applyMediaChange)
class MediaCommand : public QUndoCommand {
public:
    MediaCommand(Project* p, MediaChange change, QUndoCommand* parent)
        : QUndoCommand(parent), m_project(p), m_change(std::move(change)) {}
    void undo() override { m_project->applyMediaChange(m_change, false); }
    void redo() override { m_project->applyMediaChange(m_change, true); }

private:
    Project* m_project;
    MediaChange m_change;
};

// Several steps as one (Project::combineSteps): the whole state the history covers, before and after, and what those
// steps did to the media list (only that: imports outside the history stay)
class CombinedCommand : public QUndoCommand {
public:
    struct State {
        ProjectFormat format;
        QVector<Sequence> sequences;
        int current = 0;
        PoolState pool;
    };
    CombinedCommand(Project* p, const QString& text, State before, State after, MediaChange media)
        : QUndoCommand(text), m_project(p), m_before(std::move(before)), m_after(std::move(after)),
          m_media(std::move(media)) {}
    void undo() override
    {
        if (!m_media.isEmpty()) m_project->applyMediaChange(m_media, false); // files back before their clips
        apply(m_before);
    }
    void redo() override
    {
        apply(m_after);
        if (!m_media.isEmpty()) m_project->applyMediaChange(m_media, true);
    }

    static State capture(const Project& p) { return {p.format(), p.sequences(), p.currentSequence(), p.poolState()}; }

private:
    void apply(const State& s)
    {
        if (!(s.format == m_project->format())) m_project->applyFormat(s.format, s.sequences);
        m_project->applySequences(s.sequences, s.current);
        m_project->applyPool(s.pool);
    }

    Project* m_project;
    State m_before, m_after;
    MediaChange m_media;
};

// The media list from `before` to `after` (both states of the undo history): files that left or came, and the files
// put on or taken off the ignored list of a watch folder there in both
MediaChange mediaChange(const QVector<MediaInfo>& before, const QVector<WatchFolder>& watchesBefore,
                        const QVector<MediaInfo>& after, const QVector<WatchFolder>& watchesAfter)
{
    MediaChange c;
    QSet<QString> pathsBefore, pathsAfter;
    for (const MediaInfo& m : before) pathsBefore.insert(m.path);
    for (const MediaInfo& m : after) pathsAfter.insert(m.path);
    for (int i = 0; i < before.size(); ++i)
        if (!pathsAfter.contains(before[i].path)) c.out << qMakePair(i, before[i]);
    for (int i = 0; i < after.size(); ++i)
        if (!pathsBefore.contains(after[i].path)) c.in << qMakePair(i, after[i]);
    const auto listed = [](const QStringList& list, const QString& file) {
        return std::any_of(list.begin(), list.end(), [&](const QString& p) { return WatchFolders::samePath(p, file); });
    };
    for (const WatchFolder& w : watchesAfter) {
        const int b = WatchFolders::indexOf(watchesBefore, w.path);
        if (b < 0) continue;
        for (const QString& f : w.ignored)
            if (!listed(watchesBefore[b].ignored, f)) c.ignore << qMakePair(w.path, f);
        for (const QString& f : watchesBefore[b].ignored)
            if (!listed(w.ignored, f)) c.unignore << qMakePair(w.path, f);
    }
    return c;
}

// Files of watch folders into the Media Pool (Project::addWatchedMedia); undo takes them out again
class WatchImportCommand : public QUndoCommand {
public:
    WatchImportCommand(Project* p, const QString& text, QVector<WatchImport> files)
        : QUndoCommand(text), m_project(p), m_files(std::move(files)) {}
    void undo() override
    {
        // What changed meanwhile outside the undo history (source In/Out, transcript) comes back with a redo
        for (WatchImport& f : m_files)
            if (const MediaInfo* m = m_project->mediaInfo(f.media.path)) f.media = *m;
        m_project->applyWatchedMedia(m_files, false);
    }
    void redo() override { m_project->applyWatchedMedia(m_files, true); }

private:
    Project* m_project;
    QVector<WatchImport> m_files;
};

Track makeTrack(TrackKind kind)
{
    Track t;
    t.kind = kind; // name stays empty = default "Video 1" etc.
    return t;
}

} // namespace

Timeline emptyTimeline()
{
    // Default like an empty DaVinci project, just with one more track each
    Timeline tl;
    tl.video << makeTrack(TrackKind::Video) << makeTrack(TrackKind::Video);
    tl.audio << makeTrack(TrackKind::Audio) << makeTrack(TrackKind::Audio);
    return tl;
}

Project::Project(QObject* parent) : QObject(parent)
{
    m_sequences << Sequence{1, T("Timeline %1").arg(1), false, 0, emptyTimeline()};
    connect(&m_undo, &QUndoStack::cleanChanged, this, [this] { emit modifiedChanged(isModified()); });
}

Sequence& Project::current()
{
    for (Sequence& s : m_sequences)
        if (s.id == m_current) return s;
    return m_sequences.first();
}

const Sequence& Project::current() const
{
    for (const Sequence& s : m_sequences)
        if (s.id == m_current) return s;
    return m_sequences.first();
}

ProjectData Project::data() const
{
    ProjectData d;
    d.format = m_format;
    d.media = m_media;
    d.bins = m_bins;
    d.timeline = timeline();
    d.renderQueue = m_renderQueue;
    d.lastClipId = m_lastClipId;
    d.lastLinkId = m_lastLinkId;
    d.sequences = m_sequences;
    d.currentSequence = m_current;
    d.fixedResolution = m_fixedResolution;
    d.fixedFrameRate = m_fixedFrameRate;
    d.importPresets = m_importPresets;
    d.watchFolders = m_watchFolders;
    d.watchLog = m_watchLog;
    d.watchRevision = m_watchRevision;
    return d;
}

void Project::load(const ProjectData& d)
{
    m_undo.clear();
    m_format = d.format;
    m_media = d.media;
    m_bins = d.bins;
    m_lastBinId = 0;
    for (const MediaBin& b : m_bins) m_lastBinId = std::max(m_lastBinId, b.id);
    m_sequences = d.sequences;
    m_current = d.currentSequence;
    // Sort out duplicate/invalid ids (broken file)
    QSet<int> ids;
    m_sequences.erase(std::remove_if(m_sequences.begin(), m_sequences.end(),
                                     [&](const Sequence& s) {
                                         if (s.id <= 0 || ids.contains(s.id)) return true;
                                         ids.insert(s.id);
                                         return false;
                                     }),
                      m_sequences.end());
    if (!ids.contains(m_current)) {
        // old file (only one timeline) or the opened one is missing: `timeline` as its own sequence
        m_current = 1;
        while (ids.contains(m_current)) ++m_current;
        m_sequences.prepend(Sequence{m_current, T("Timeline %1").arg(1), false, 0, {}});
    }
    current().timeline = d.timeline;
    if (std::none_of(m_sequences.begin(), m_sequences.end(), [](const Sequence& s) { return !s.compound; }))
        current().compound = false; // there is always at least one normal timeline
    m_lastSequenceId = 0;
    for (Sequence& s : m_sequences) {
        m_lastSequenceId = std::max(m_lastSequenceId, s.id);
        if (s.timeline.video.isEmpty()) s.timeline.video << makeTrack(TrackKind::Video);
        if (s.timeline.audio.isEmpty()) s.timeline.audio << makeTrack(TrackKind::Audio);
        if (s.name.trimmed().isEmpty()) s.name = T("Timeline %1").arg(s.id);
    }
    // Multicam clips: angles only point at existing tracks (broken file)
    QHash<int, int> angles; // multicam sequence id -> number of angles
    for (Sequence& s : m_sequences) {
        if (!s.multicam) {
            s.angles.clear();
            continue;
        }
        s.compound = true;
        for (MulticamAngle& a : s.angles) {
            if (a.videoTrack >= s.timeline.video.size()) a.videoTrack = -1;
            a.videoTrack = std::max(-1, a.videoTrack);
            a.audioTracks.removeIf([&](int i) { return i < 0 || i >= s.timeline.audio.size(); });
        }
        angles.insert(s.id, s.angles.size());
    }
    // Remove compound/multicam clips referring to missing (or not multicam) sequences or loops (broken file)
    for (Sequence& s : m_sequences)
        for (auto* tracks : {&s.timeline.video, &s.timeline.audio})
            for (Track& t : *tracks) {
                t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                             [&](const Clip& c) {
                                                 if (c.isMulticam() && !angles.contains(c.sequenceId)) return true;
                                                 return c.isNested()
                                                        && (!ids.contains(c.sequenceId) || c.sequenceId == s.id);
                                             }),
                              t.clips.end());
                for (Clip& c : t.clips)
                    if (c.isMulticam() && (c.angle < 0 || c.angle >= angles.value(c.sequenceId))) c.angle = 0;
            }
    sanitizePool();
    m_lastClipId = d.lastClipId;
    m_lastLinkId = d.lastLinkId;
    m_renderQueue = d.renderQueue;
    m_fixedResolution = d.fixedResolution;
    m_fixedFrameRate = d.fixedFrameRate;
    m_importPresets = d.importPresets;
    m_watchFolders = d.watchFolders;
    for (WatchFolder& w : m_watchFolders)
        if (w.bin != 0 && !bin(w.bin)) w.bin = 0;
    m_watchLog = d.watchLog;
    m_watchRevision = d.watchRevision;
    for (const WatchArrival& a : m_watchLog) m_watchRevision = std::max(m_watchRevision, a.revision);
    markSaved();
    emit formatChanged();
    emit mediaChanged();
    emit sequencesChanged();
    emit poolChanged();
    emit currentSequenceChanged();
    emit timelineChanged();
    emit renderQueueChanged();
    emit importPresetsChanged();
    emit watchFoldersChanged();
}

void Project::reset()
{
    ProjectData d;
    d.timeline = emptyTimeline();
    load(d);
}

void Project::markSaved()
{
    m_mediaDirty = false;
    m_undo.setClean();
    emit modifiedChanged(false);
}

void Project::edit(const QString& text, const std::function<void(Timeline&)>& fn, const QString& mergeKey)
{
    Timeline after = timeline();
    fn(after);
    TimelineOps::resolvePendingLinks(after, [this] { return newLinkId(); }); // leftover pieces from clearRange
    TimelineOps::unpairBrokenDissolves(timeline(), after);
    TimelineOps::keepFadesApart(timeline(), after);
    if (after == timeline()) {
        // Nothing changed (e.g. Ctrl+B beside the selection): no undo step. Signal anyway, so that a
        // pending Engine::mixerOnlyNext is consumed and does not stick to the next real change.
        emit timelineChanged();
        return;
    }
    const QString key = mergeKey.isEmpty() ? QString() : QString("%1#%2").arg(mergeKey).arg(m_mergeSession);
    m_undo.push(new SnapshotCommand(this, text, m_current, timeline(), after, key)); // push calls redo()
}

void Project::combineSteps(int fromIndex, const QString& text)
{
    if (fromIndex < 0 || m_undo.index() <= fromIndex) return;
    closeMerge();
    CombinedCommand::State after = CombinedCommand::capture(*this);
    const QVector<MediaInfo> mediaAfter = m_media;
    const QVector<WatchFolder> watchesAfter = m_watchFolders;
    m_undo.setIndex(fromIndex); // undoes them: the state before
    // Only what the steps did to the media list (they are undone now): files imported meanwhile are in both lists
    MediaChange media = mediaChange(m_media, m_watchFolders, mediaAfter, watchesAfter);
    m_undo.push(new CombinedCommand(this, text, CombinedCommand::capture(*this), std::move(after),
                                    std::move(media))); // push calls redo()
    closeMerge();
}

bool Project::frameRateLocked() const
{
    for (const Sequence& s : m_sequences)
        for (const auto* tracks : {&s.timeline.video, &s.timeline.audio})
            for (const Track& t : *tracks)
                if (!t.clips.isEmpty()) return true;
    return false;
}

void Project::setFormat(const ProjectFormat& format)
{
    ProjectFormat f = format;
    if (frameRateLocked()) f.rate = m_format.rate;
    if (f == m_format) return;
    QVector<Sequence> after = m_sequences;
    for (Sequence& s : after) scaleTimeline(s.timeline, m_format.size(), f.size());
    m_undo.push(new FormatCommand(this, m_format, f, m_sequences, after)); // push calls redo()
}

void Project::applyFormat(const ProjectFormat& format, const QVector<Sequence>& sequences)
{
    m_format = format;
    m_sequences = sequences;
    emit formatChanged();
    emit timelineChanged();
}

void Project::setTimeline(const Timeline& tl)
{
    current().timeline = tl;
    current().timeline.nested.reset();
    emit timelineChanged();
}

void Project::setTimeline(int sequenceId, const Timeline& tl)
{
    if (!sequence(sequenceId)) return;
    if (sequenceId != m_current) {
        m_current = sequenceId;
        emit currentSequenceChanged();
    }
    setTimeline(tl);
}

// ---------- Sequences (several timelines, compound clips) ----------

const Sequence* Project::sequence(int id) const
{
    for (const Sequence& s : m_sequences)
        if (s.id == id) return &s;
    return nullptr;
}

QString Project::sequenceName(int id) const
{
    const Sequence* s = sequence(id);
    return s ? s->name : QString();
}

void Project::setCurrentSequence(int id)
{
    if (id == m_current || !sequence(id)) return;
    closeMerge();
    m_current = id;
    emit currentSequenceChanged();
    emit timelineChanged();
}

void Project::editSequences(const QString& text, const std::function<void(QVector<Sequence>&, int& current)>& fn)
{
    QVector<Sequence> after = m_sequences;
    int cur = m_current;
    fn(after, cur);
    if (after.isEmpty()) return;
    for (Sequence& seq : after) TimelineOps::resolvePendingLinks(seq.timeline, [this] { return newLinkId(); });
    if (std::none_of(after.begin(), after.end(), [&](const Sequence& s) { return s.id == cur; })) cur = after[0].id;
    m_undo.push(new SequencesCommand(this, text, m_sequences, m_current, after, cur)); // push calls redo()
}

void Project::applySequences(const QVector<Sequence>& sequences, int cur)
{
    m_sequences = sequences;
    for (const Sequence& s : m_sequences) m_lastSequenceId = std::max(m_lastSequenceId, s.id);
    const bool switched = cur != m_current;
    m_current = cur;
    emit sequencesChanged();
    emit poolChanged(); // sequences are in the Media Pool
    if (switched) emit currentSequenceChanged();
    emit timelineChanged();
}

int Project::addTimeline(const QString& name, int binId)
{
    if (binId != 0 && !bin(binId)) binId = 0;
    QString n = name.trimmed();
    if (n.isEmpty()) { // like DaVinci "Timeline 1", "Timeline 2" ... (first free number)
        QSet<QString> used;
        for (const Sequence& s : m_sequences) used.insert(s.name);
        for (int i = 1;; ++i) {
            n = T("Timeline %1").arg(i);
            if (!used.contains(n)) break;
        }
    }
    const int id = newSequenceId();
    editSequences(T("Neue Timeline"), [&](QVector<Sequence>& list, int& cur) {
        list << Sequence{id, n, false, binId, emptyTimeline()};
        cur = id;
    });
    return id;
}

void Project::renameSequence(int id, const QString& name)
{
    const QString n = name.trimmed();
    const Sequence* s = sequence(id);
    if (n.isEmpty() || !s || s->name == n) return;
    editSequences(s->compound ? T("Compound Clip umbenennen") : T("Timeline umbenennen"),
                  [&](QVector<Sequence>& list, int&) {
                      for (Sequence& x : list)
                          if (x.id == id) x.name = n;
                  });
}

int Project::duplicateSequence(int id)
{
    const Sequence* src = sequence(id);
    if (!src) return 0;
    Sequence copy = *src;
    copy.id = newSequenceId();
    copy.name = T("%1 Kopie").arg(src->name);
    // own clip/link ids (selection, keyframe track etc. depend on the id)
    QHash<int, int> links;
    for (auto* tracks : {&copy.timeline.video, &copy.timeline.audio})
        for (Track& t : *tracks)
            for (Clip& c : t.clips) {
                c.id = newClipId();
                if (c.linkId) {
                    if (!links.contains(c.linkId)) links.insert(c.linkId, newLinkId());
                    c.linkId = links.value(c.linkId);
                }
            }
    for (SubtitleTrack& t : copy.timeline.subtitles)
        for (SubtitleCue& c : t.cues) c.id = newClipId();
    editSequences(src->compound ? T("Compound Clip duplizieren") : T("Timeline duplizieren"),
                  [&](QVector<Sequence>& list, int&) { list << copy; });
    return copy.id;
}

bool Project::canRemoveSequence(int id) const
{
    const Sequence* s = sequence(id);
    if (!s || sequenceUsed(id)) return false;
    if (s->compound) return true;
    return std::count_if(m_sequences.begin(), m_sequences.end(), [](const Sequence& x) { return !x.compound; }) > 1;
}

bool Project::removeSequence(int id)
{
    if (!canRemoveSequence(id)) return false;
    const bool compound = sequence(id)->compound;
    editSequences(compound ? T("Compound Clip löschen") : T("Timeline löschen"),
                  [&](QVector<Sequence>& list, int& cur) {
                      const int idx = int(std::find_if(list.begin(), list.end(),
                                                       [&](const Sequence& s) { return s.id == id; })
                                          - list.begin());
                      list.removeAt(idx);
                      if (cur == id) { // open the next normal timeline
                          for (const Sequence& s : list)
                              if (!s.compound) {
                                  cur = s.id;
                                  break;
                              }
                      }
                  });
    return true;
}

void Project::moveSequencesToBin(const QVector<int>& ids, int binId)
{
    moveMediaToBin({}, binId, ids);
}

namespace {
bool timelineUses(const Timeline& tl, int id)
{
    for (const auto* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips)
                if (c.isNested() && c.sequenceId == id) return true;
    return false;
}
} // namespace

bool Project::sequenceUsed(int id) const
{
    return std::any_of(m_sequences.begin(), m_sequences.end(),
                       [&](const Sequence& s) { return s.id != id && timelineUses(s.timeline, id); });
}

bool Project::canNest(int child, int parent) const
{
    // Depth-first search from child: if it reaches parent, a loop would arise
    QSet<int> seen;
    QVector<int> todo{child};
    while (!todo.isEmpty()) {
        const int id = todo.takeLast();
        if (id == parent) return false;
        if (seen.contains(id)) continue;
        seen.insert(id);
        const Sequence* s = sequence(id);
        if (!s) return id != child; // unknown sequence is no good
        for (const auto* tracks : {&s->timeline.video, &s->timeline.audio})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    if (c.isNested()) todo << c.sequenceId;
    }
    return true;
}

int Project::sequenceLength(int id) const
{
    const Sequence* s = sequence(id);
    return s ? TimelineOps::endFrame(s->timeline) : 0;
}

QString Project::clipName(const Clip& c) const
{
    if (c.isNested())
        if (const Sequence* s = sequence(c.sequenceId))
            return c.isMulticam() && c.angle >= 0 && c.angle < s->angles.size() ? s->angles[c.angle].name : s->name;
    return c.displayName();
}

Timeline Project::renderTimeline(int sequenceId) const
{
    const Sequence* s = sequenceId ? sequence(sequenceId) : &current();
    Timeline tl = s ? s->timeline : timeline();
    auto nested = std::make_shared<NestedTimelines>();
    for (const Sequence& x : m_sequences) {
        Timeline t = x.timeline;
        t.nested.reset();
        t.angles = x.multicam ? x.angles : QVector<MulticamAngle>{};
        nested->insert(x.id, t);
    }
    tl.nested = std::move(nested);
    return tl;
}

const MediaInfo* Project::mediaInfo(const QString& path) const
{
    for (const auto& m : m_media)
        if (m.path == path) return &m;
    return nullptr;
}

void Project::addMedia(const MediaInfo& info)
{
    if (mediaInfo(info.path)) return;
    m_media << info;
    if (m_media.last().bin != 0 && !bin(m_media.last().bin)) m_media.last().bin = 0;
    m_mediaDirty = true;
    emit mediaChanged();
    emit modifiedChanged(true);
}

void Project::setMediaMarks(const QString& path, int markIn, int markOut)
{
    for (MediaInfo& m : m_media) {
        if (m.path != path) continue;
        if (m.markIn == markIn && m.markOut == markOut) return;
        m.markIn = markIn;
        m.markOut = markOut;
        m_mediaDirty = true;
        emit mediaMarksChanged(path);
        emit modifiedChanged(true);
        return;
    }
}

void Project::setMediaMarkers(const QString& path, const QVector<int>& markers)
{
    for (MediaInfo& m : m_media) {
        if (m.path != path) continue;
        if (m.markers == markers) return;
        m.markers = markers;
        m_mediaDirty = true;
        emit mediaMarksChanged(path);
        emit modifiedChanged(true);
        return;
    }
}

void Project::setTranscript(const QString& path, const MediaTranscript& transcript)
{
    for (MediaInfo& m : m_media) {
        if (m.path != path) continue;
        QVector<MediaTranscript> list = m.transcripts;
        list.removeIf([&](const MediaTranscript& t) { return t.audioStream == transcript.audioStream; });
        if (!transcript.words.isEmpty()) {
            list << transcript;
            std::sort(list.begin(), list.end(),
                      [](const MediaTranscript& a, const MediaTranscript& b) { return a.audioStream < b.audioStream; });
        }
        if (list == m.transcripts) return;
        m.transcripts = list;
        m_mediaDirty = true;
        emit transcriptChanged(path);
        emit modifiedChanged(true);
        return;
    }
}

void Project::recountMedia(FrameRate oldRate, const std::function<int(const QString&)>& probeLength)
{
    const double k = m_format.rate.fps() / oldRate.fps();
    QVector<MediaInfo> media = m_media;
    for (MediaInfo& m : media) {
        const int fresh = probeLength && QFileInfo::exists(m.path) ? probeLength(m.path) : 0;
        m.length = fresh > 0 ? fresh : std::max(1, int(std::lround(m.length * k)));
        const auto scale = [&](int frame) { return frame < 0 ? -1 : std::min(m.length - 1, int(std::lround(frame * k))); };
        m.markIn = scale(m.markIn);
        m.markOut = scale(m.markOut);
        for (int& marker : m.markers) marker = scale(marker);
    }
    replaceMedia(media);
}

QVector<QPair<int, int>> Project::mediaUsage(const QStringList& paths) const
{
    const QSet<QString> files(paths.begin(), paths.end());
    QVector<QPair<int, int>> usage;
    for (const Sequence& s : m_sequences) {
        int clips = 0;
        for (const auto* tracks : {&s.timeline.video, &s.timeline.audio})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    if (c.kind == ClipKind::Media && files.contains(c.mediaPath)) ++clips;
        if (clips > 0) usage << qMakePair(s.id, clips);
    }
    return usage;
}

bool Project::removeMedia(const QStringList& paths, bool withClips)
{
    QSet<QString> files;
    for (const QString& p : paths)
        if (mediaInfo(p)) files.insert(p);
    if (files.isEmpty()) return false;
    const QStringList list = files.values();
    const QVector<QPair<int, int>> usage = mediaUsage(list);
    if (!usage.isEmpty() && !withClips) return false;
    MediaChange change;
    for (int i = 0; i < m_media.size(); ++i)
        if (files.contains(m_media[i].path)) change.out << qMakePair(i, m_media[i]);
    // A watch would take a file of its folder again at once: on its ignored list, like an undone watch import
    for (const auto& [index, m] : change.out) {
        const int w = WatchFolders::covering(m_watchFolders, m.path);
        if (w < 0) continue;
        const QStringList& ignored = m_watchFolders[w].ignored;
        if (std::none_of(ignored.begin(), ignored.end(), [&](const QString& p) { return WatchFolders::samePath(p, m.path); }))
            change.ignore << qMakePair(m_watchFolders[w].path, m.path);
    }
    const QString text = change.out.size() == 1 ? T("„%1“ aus dem Media Pool entfernen").arg(change.out.first().second.name)
                                                : T("%1 Clips aus dem Media Pool entfernen").arg(change.out.size());
    closeMerge(); // never part of a slider series of the user
    auto* step = new QUndoCommand(text);
    if (!usage.isEmpty()) {
        // Their clips first (undo: the files come back before them): a gap stays like after Delete, a dissolve with a
        // clip that goes is released
        QVector<Sequence> after = m_sequences;
        for (Sequence& s : after) {
            QVector<int> ids;
            for (const auto* tracks : {&s.timeline.video, &s.timeline.audio})
                for (const Track& t : *tracks)
                    for (const Clip& c : t.clips)
                        if (c.kind == ClipKind::Media && files.contains(c.mediaPath)) ids << c.id;
            if (ids.isEmpty()) continue;
            const Timeline before = s.timeline;
            TimelineOps::detachTransitions(s.timeline, ids);
            for (int id : ids) TimelineOps::removeClip(s.timeline, id);
            TimelineOps::unpairBrokenDissolves(before, s.timeline);
            TimelineOps::keepFadesApart(before, s.timeline);
        }
        new SequencesCommand(this, text, m_sequences, m_current, after, m_current, step);
    }
    new MediaCommand(this, std::move(change), step);
    m_undo.push(step); // push calls redo()
    closeMerge();
    return true;
}

void Project::applyMediaChange(MediaChange& change, bool forward)
{
    auto& leaving = forward ? change.out : change.in;
    auto& coming = forward ? change.in : change.out;
    // Where they are now, and what changed meanwhile outside the history (source marks, transcript): next time they
    // come back like that, at that place
    QSet<QString> gone;
    for (auto& [index, info] : leaving)
        for (int i = 0; i < m_media.size(); ++i)
            if (m_media[i].path == info.path) {
                index = i;
                info = m_media[i];
                gone.insert(info.path);
                break;
            }
    m_media.removeIf([&](const MediaInfo& m) { return gone.contains(m.path); });
    std::sort(coming.begin(), coming.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [index, info] : coming) {
        if (mediaInfo(info.path)) continue;
        const int at = std::clamp(index, 0, int(m_media.size()));
        m_media.insert(at, info);
        if (m_media[at].bin != 0 && !bin(m_media[at].bin)) m_media[at].bin = 0;
    }
    bool watches = false;
    const auto onList = [](const QStringList& list, const QString& file) {
        return std::any_of(list.begin(), list.end(), [&](const QString& p) { return WatchFolders::samePath(p, file); });
    };
    for (const auto& [folder, file] : forward ? change.ignore : change.unignore) {
        const int w = WatchFolders::indexOf(m_watchFolders, folder);
        if (w < 0 || onList(m_watchFolders[w].ignored, file)) continue;
        m_watchFolders[w].ignored << file;
        watches = true;
    }
    for (const auto& [folder, file] : forward ? change.unignore : change.ignore) {
        const int w = WatchFolders::indexOf(m_watchFolders, folder);
        if (w >= 0)
            watches = m_watchFolders[w].ignored.removeIf([&](const QString& p) { return WatchFolders::samePath(p, file); }) > 0
                      || watches;
    }
    // The ignored lists are part of the file, also when the undo history comes back to its clean state
    if (watches) m_mediaDirty = true;
    emit mediaChanged();
    if (watches) emit watchFoldersChanged();
    emit modifiedChanged(isModified());
}

void Project::replaceMedia(const QVector<MediaInfo>& media)
{
    m_media = media;
    emit mediaChanged();
}

void Project::setRenderQueue(const QVector<RenderJob>& jobs)
{
    if (jobs == m_renderQueue) return;
    m_renderQueue = jobs;
    emit renderQueueChanged();
    markModified();
}

void Project::setFixedFormat(bool resolution, bool frameRate)
{
    if (resolution == m_fixedResolution && frameRate == m_fixedFrameRate) return;
    m_fixedResolution = resolution;
    m_fixedFrameRate = frameRate;
    markModified();
}

void Project::setImportPresets(const std::optional<StreamPresets>& presets)
{
    if (presets == m_importPresets) return;
    m_importPresets = presets;
    emit importPresetsChanged();
    markModified();
}

void Project::setWatchFolders(const QVector<WatchFolder>& list)
{
    if (list == m_watchFolders) return;
    m_watchFolders = list;
    emit watchFoldersChanged();
    markModified();
}

QVector<WatchArrival> Project::addWatchedMedia(const QVector<WatchImport>& files)
{
    QSet<QString> known;
    for (const MediaInfo& m : m_media) known.insert(WatchFolders::pathKey(m.path));
    QVector<WatchImport> add;
    for (WatchImport f : files) {
        const QString key = WatchFolders::pathKey(f.media.path);
        if (f.media.path.isEmpty() || known.contains(key)) continue;
        known.insert(key);
        const int w = WatchFolders::indexOf(m_watchFolders, f.folder);
        f.media.bin = w >= 0 && bin(m_watchFolders[w].bin) ? m_watchFolders[w].bin : 0;
        add << f;
    }
    if (add.isEmpty()) return {};
    QVector<WatchArrival> arrivals;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const WatchImport& f : add) arrivals << WatchArrival{++m_watchRevision, f.media.path, f.folder, now};
    m_watchLog << arrivals;
    if (m_watchLog.size() > WatchFolders::kLogSize) m_watchLog.remove(0, m_watchLog.size() - WatchFolders::kLogSize);
    QString folder = QFileInfo(add.first().folder).fileName();
    if (folder.isEmpty()) folder = add.first().folder;
    const QString text = add.size() == 1
                             ? T("Überwachter Ordner „%1“: neue Datei „%2“").arg(folder, QFileInfo(add.first().media.path).fileName())
                             : T("Überwachter Ordner „%1“: %2 neue Dateien").arg(folder, QString::number(add.size()));
    closeMerge(); // never part of a slider series of the user
    m_undo.push(new WatchImportCommand(this, text, add)); // push calls redo()
    closeMerge();
    return arrivals;
}

void Project::applyWatchedMedia(const QVector<WatchImport>& files, bool in)
{
    bool watches = false;
    for (const WatchImport& f : files) {
        const int w = WatchFolders::indexOf(m_watchFolders, f.folder);
        QStringList* ignored = w >= 0 ? &m_watchFolders[w].ignored : nullptr;
        if (in) {
            if (!mediaInfo(f.media.path)) {
                m_media << f.media;
                if (m_media.last().bin != 0 && !bin(m_media.last().bin)) m_media.last().bin = 0;
            }
            if (ignored)
                watches = ignored->removeIf([&](const QString& p) { return WatchFolders::samePath(p, f.media.path); }) > 0
                          || watches;
        } else {
            m_media.removeIf([&](const MediaInfo& m) { return m.path == f.media.path; });
            if (ignored && std::none_of(ignored->cbegin(), ignored->cend(),
                                        [&](const QString& p) { return WatchFolders::samePath(p, f.media.path); })) {
                *ignored << f.media.path;
                watches = true;
            }
        }
    }
    // The ignored list is part of the file, also when the undo history comes back to its clean state
    if (!in) m_mediaDirty = true;
    emit mediaChanged();
    if (watches) emit watchFoldersChanged();
    emit modifiedChanged(isModified());
}

void Project::markModified()
{
    m_mediaDirty = true;
    emit modifiedChanged(true);
}

// ---------- Media Pool: bins, clip colors, flags ----------

const MediaBin* Project::bin(int id) const
{
    for (const MediaBin& b : m_bins)
        if (b.id == id) return &b;
    return nullptr;
}

QString Project::binName(int id) const
{
    if (id == 0) return QStringLiteral("Master"); // is called that in German DaVinci too
    const MediaBin* b = bin(id);
    return b ? b->name : QString();
}

QVector<int> Project::childBins(int parent) const
{
    QVector<const MediaBin*> list;
    for (const MediaBin& b : m_bins)
        if (b.parent == parent) list << &b;
    std::sort(list.begin(), list.end(), [](const MediaBin* a, const MediaBin* b) {
        const int c = I18n::compareNames(a->name, b->name);
        return c != 0 ? c < 0 : a->id < b->id;
    });
    QVector<int> ids;
    for (const MediaBin* b : list) ids << b->id;
    return ids;
}

bool Project::binInside(int id, int ancestor) const
{
    for (int guard = 0; guard <= m_bins.size(); ++guard) { // there are no cycles (sanitizePool), limit anyway
        if (id == ancestor) return true;
        if (id == 0) return false;
        const MediaBin* b = bin(id);
        if (!b) return false;
        id = b->parent;
    }
    return false;
}

PoolState Project::poolState() const
{
    PoolState s;
    s.bins = m_bins;
    for (const MediaInfo& m : m_media) s.media.insert(m.path, MediaOrg{m.bin, m.clipColor, m.flags});
    for (const Sequence& q : m_sequences) s.sequenceBins.insert(q.id, q.bin);
    return s;
}

void Project::applyPool(const PoolState& state)
{
    m_bins = state.bins;
    for (const MediaBin& b : m_bins) m_lastBinId = std::max(m_lastBinId, b.id);
    for (MediaInfo& m : m_media) {
        const auto it = state.media.constFind(m.path);
        if (it == state.media.cend()) continue; // imported later -> leave unchanged
        m.bin = it->bin;
        m.clipColor = it->color;
        m.flags = it->flags;
    }
    for (Sequence& q : m_sequences)
        if (const auto it = state.sequenceBins.constFind(q.id); it != state.sequenceBins.cend()) q.bin = *it;
    sanitizePool();
    emit poolChanged();
}

void Project::sanitizePool()
{
    QSet<int> ids;
    for (const MediaBin& b : m_bins) ids.insert(b.id);
    ids.remove(0); // 0 is always Master
    for (MediaBin& b : m_bins) {
        if (b.parent != 0 && !ids.contains(b.parent)) b.parent = 0;
    }
    // Break up parent cycles (broken file): whatever reaches itself via the parents goes under Master
    for (MediaBin& b : m_bins) {
        int cur = b.parent;
        for (int guard = 0; cur != 0 && guard <= m_bins.size(); ++guard) {
            if (cur == b.id || guard == m_bins.size()) {
                b.parent = 0;
                break;
            }
            const MediaBin* p = bin(cur);
            cur = p ? p->parent : 0;
        }
    }
    for (MediaInfo& m : m_media) {
        if (m.bin != 0 && !ids.contains(m.bin)) m.bin = 0;
        if (!m.clipColor.isEmpty() && !trackColorInfo(m.clipColor)) m.clipColor.clear();
    }
    for (Sequence& q : m_sequences)
        if (q.bin != 0 && !ids.contains(q.bin)) q.bin = 0;
}

void Project::editPool(const QString& text, const std::function<void(PoolState&)>& fn)
{
    PoolState after = poolState();
    fn(after);
    const PoolState before = poolState();
    if (after == before) return;
    m_undo.push(new PoolCommand(this, text, before, after)); // push calls redo()
}

int Project::addBin(int parent, const QString& name)
{
    if (parent != 0 && !bin(parent)) parent = 0;
    QString n = name.trimmed();
    if (n.isEmpty()) { // like DaVinci "Bin 1", "Bin 2" ... (first free number)
        QSet<QString> used;
        for (const MediaBin& b : m_bins) used.insert(b.name);
        for (int i = 1;; ++i) {
            n = T("Bin %1").arg(i);
            if (!used.contains(n)) break;
        }
    }
    const int id = m_lastBinId + 1;
    editPool(T("Neuer Bin"), [&](PoolState& s) { s.bins << MediaBin{id, parent, n}; });
    return id;
}

void Project::renameBin(int id, const QString& name)
{
    const QString n = name.trimmed();
    if (n.isEmpty() || !bin(id)) return;
    editPool(T("Bin umbenennen"), [&](PoolState& s) {
        for (MediaBin& b : s.bins)
            if (b.id == id) b.name = n;
    });
}

void Project::removeBin(int id)
{
    const MediaBin* b = bin(id);
    if (!b) return;
    const int parent = b->parent;
    editPool(T("Bin entfernen"), [&](PoolState& s) {
        s.bins.erase(std::remove_if(s.bins.begin(), s.bins.end(), [&](const MediaBin& x) { return x.id == id; }),
                     s.bins.end());
        for (MediaBin& x : s.bins)
            if (x.parent == id) x.parent = parent;
        for (MediaOrg& o : s.media)
            if (o.bin == id) o.bin = parent;
        for (int& b : s.sequenceBins)
            if (b == id) b = parent;
    });
}

void Project::moveBin(int id, int parent)
{
    const MediaBin* b = bin(id);
    if (!b || b->parent == parent || (parent != 0 && !bin(parent)) || binInside(parent, id)) return;
    editPool(T("Bin verschieben"), [&](PoolState& s) {
        for (MediaBin& x : s.bins)
            if (x.id == id) x.parent = parent;
    });
}

void Project::moveMediaToBin(const QStringList& paths, int binId, const QVector<int>& sequences)
{
    if (binId != 0 && !bin(binId)) return;
    editPool(T("In Bin verschieben"), [&](PoolState& s) {
        for (const QString& p : paths)
            if (s.media.contains(p)) s.media[p].bin = binId;
        for (int id : sequences)
            if (s.sequenceBins.contains(id)) s.sequenceBins[id] = binId;
    });
}

void Project::setClipColor(const QStringList& paths, const QString& colorId)
{
    if (!colorId.isEmpty() && !trackColorInfo(colorId)) return;
    editPool(colorId.isEmpty() ? T("Clipfarbe entfernen") : T("Clipfarbe ändern"), [&](PoolState& s) {
        for (const QString& p : paths)
            if (s.media.contains(p)) s.media[p].color = colorId;
    });
}

void Project::setFlag(const QStringList& paths, const QString& flagId, bool on)
{
    if (!flagColorInfo(flagId)) return;
    editPool(on ? T("Flag hinzufügen") : T("Flag entfernen"), [&](PoolState& s) {
        for (const QString& p : paths) {
            if (!s.media.contains(p)) continue;
            QStringList& flags = s.media[p].flags;
            if (on && !flags.contains(flagId)) {
                flags << flagId;
                // order as in the color list (stable display)
                std::sort(flags.begin(), flags.end(), [](const QString& a, const QString& b) {
                    return flagColorInfo(a) < flagColorInfo(b);
                });
            } else if (!on) {
                flags.removeAll(flagId);
            }
        }
    });
}

void Project::clearFlags(const QStringList& paths)
{
    editPool(T("Alle Flags entfernen"), [&](PoolState& s) {
        for (const QString& p : paths)
            if (s.media.contains(p)) s.media[p].flags.clear();
    });
}
