#include "core/Project.h"

#include "core/I18n.h"
#include "core/TimelineOps.h"

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

// Projekteinstellungen ändern: Format und (umgerechneter) Schnitt zusammen, damit Undo beides zurücknimmt
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

// Mehrere Sequenzen zugleich (neue Timeline, Compound Clip erstellen/auflösen …) samt geöffneter Sequenz
class SequencesCommand : public QUndoCommand {
public:
    SequencesCommand(Project* p, const QString& text, QVector<Sequence> before, int curBefore,
                     QVector<Sequence> after, int curAfter)
        : QUndoCommand(text), m_project(p), m_before(std::move(before)), m_after(std::move(after)),
          m_curBefore(curBefore), m_curAfter(curAfter) {}
    void undo() override { m_project->applySequences(m_before, m_curBefore); }
    void redo() override { m_project->applySequences(m_after, m_curAfter); }

private:
    Project* m_project;
    QVector<Sequence> m_before, m_after;
    int m_curBefore, m_curAfter;
};

// Media-Pool-Organisation (Bins, Clipfarben, Flags) ändern – wie in DaVinci rückgängig machbar
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

Track makeTrack(TrackKind kind)
{
    Track t;
    t.kind = kind; // Name bleibt leer = Standard „Video 1“ usw.
    return t;
}

} // namespace

Timeline emptyTimeline()
{
    // Standard wie ein leeres DaVinci-Projekt, nur mit je einer Spur mehr
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
    // Doppelte/ungültige ids (kaputte Datei) aussortieren
    QSet<int> ids;
    m_sequences.erase(std::remove_if(m_sequences.begin(), m_sequences.end(),
                                     [&](const Sequence& s) {
                                         if (s.id <= 0 || ids.contains(s.id)) return true;
                                         ids.insert(s.id);
                                         return false;
                                     }),
                      m_sequences.end());
    if (!ids.contains(m_current)) {
        // alte Datei (nur eine Timeline) oder geöffnete fehlt: `timeline` als eigene Sequenz
        m_current = 1;
        while (ids.contains(m_current)) ++m_current;
        m_sequences.prepend(Sequence{m_current, T("Timeline %1").arg(1), false, 0, {}});
    }
    current().timeline = d.timeline;
    if (std::none_of(m_sequences.begin(), m_sequences.end(), [](const Sequence& s) { return !s.compound; }))
        current().compound = false; // es gibt immer mindestens eine normale Timeline
    m_lastSequenceId = 0;
    for (Sequence& s : m_sequences) {
        m_lastSequenceId = std::max(m_lastSequenceId, s.id);
        if (s.timeline.video.isEmpty()) s.timeline.video << makeTrack(TrackKind::Video);
        if (s.timeline.audio.isEmpty()) s.timeline.audio << makeTrack(TrackKind::Audio);
        if (s.name.trimmed().isEmpty()) s.name = T("Timeline %1").arg(s.id);
    }
    // Compound Clips mit Verweis auf fehlende Sequenzen oder Schleifen (kaputte Datei) -> entfernen
    for (Sequence& s : m_sequences)
        for (auto* tracks : {&s.timeline.video, &s.timeline.audio})
            for (Track& t : *tracks)
                t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                             [&](const Clip& c) {
                                                 return c.isCompound()
                                                        && (!ids.contains(c.sequenceId) || c.sequenceId == s.id);
                                             }),
                              t.clips.end());
    sanitizePool();
    m_lastClipId = d.lastClipId;
    m_lastLinkId = d.lastLinkId;
    m_renderQueue = d.renderQueue;
    markSaved();
    emit formatChanged();
    emit mediaChanged();
    emit sequencesChanged();
    emit poolChanged();
    emit currentSequenceChanged();
    emit timelineChanged();
    emit renderQueueChanged();
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
    TimelineOps::resolvePendingLinks(after, [this] { return newLinkId(); }); // Reststücke aus clearRange
    TimelineOps::unpairBrokenDissolves(timeline(), after);
    if (after == timeline()) {
        // Nichts geändert (z. B. Strg+B neben der Auswahl): kein Undo-Schritt. Signal trotzdem, damit ein
        // vorgemerktes Engine::mixerOnlyNext verbraucht wird und nicht an der nächsten echten Änderung hängt.
        emit timelineChanged();
        return;
    }
    const QString key = mergeKey.isEmpty() ? QString() : QString("%1#%2").arg(mergeKey).arg(m_mergeSession);
    m_undo.push(new SnapshotCommand(this, text, m_current, timeline(), after, key)); // push ruft redo()
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
    m_undo.push(new FormatCommand(this, m_format, f, m_sequences, after)); // push ruft redo()
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

// ---------- Sequenzen (mehrere Timelines, Compound Clips) ----------

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
    m_undo.push(new SequencesCommand(this, text, m_sequences, m_current, after, cur)); // push ruft redo()
}

void Project::applySequences(const QVector<Sequence>& sequences, int cur)
{
    m_sequences = sequences;
    for (const Sequence& s : m_sequences) m_lastSequenceId = std::max(m_lastSequenceId, s.id);
    const bool switched = cur != m_current;
    m_current = cur;
    emit sequencesChanged();
    emit poolChanged(); // Sequenzen stehen im Media Pool
    if (switched) emit currentSequenceChanged();
    emit timelineChanged();
}

int Project::addTimeline(const QString& name, int binId)
{
    if (binId != 0 && !bin(binId)) binId = 0;
    QString n = name.trimmed();
    if (n.isEmpty()) { // wie DaVinci „Timeline 1“, „Timeline 2“ … (erste freie Nummer)
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
    // eigene Clip-/Verknüpfungs-ids (Auswahl, Keyframe-Spur usw. hängen an der id)
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
                      if (cur == id) { // nächste normale Timeline öffnen
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
                if (c.isCompound() && c.sequenceId == id) return true;
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
    // Tiefensuche von child aus: erreicht sie parent, entstünde eine Schleife
    QSet<int> seen;
    QVector<int> todo{child};
    while (!todo.isEmpty()) {
        const int id = todo.takeLast();
        if (id == parent) return false;
        if (seen.contains(id)) continue;
        seen.insert(id);
        const Sequence* s = sequence(id);
        if (!s) return id != child; // unbekannte Sequenz taugt nicht
        for (const auto* tracks : {&s->timeline.video, &s->timeline.audio})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    if (c.isCompound()) todo << c.sequenceId;
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
    if (c.isCompound())
        if (const Sequence* s = sequence(c.sequenceId)) return s->name;
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

void Project::markModified()
{
    m_mediaDirty = true;
    emit modifiedChanged(true);
}

// ---------- Media Pool: Bins, Clipfarben, Flags ----------

const MediaBin* Project::bin(int id) const
{
    for (const MediaBin& b : m_bins)
        if (b.id == id) return &b;
    return nullptr;
}

QString Project::binName(int id) const
{
    if (id == 0) return QStringLiteral("Master"); // heißt auch im deutschen DaVinci so
    const MediaBin* b = bin(id);
    return b ? b->name : QString();
}

QVector<int> Project::childBins(int parent) const
{
    QVector<const MediaBin*> list;
    for (const MediaBin& b : m_bins)
        if (b.parent == parent) list << &b;
    std::sort(list.begin(), list.end(), [](const MediaBin* a, const MediaBin* b) {
        const int c = QString::localeAwareCompare(a->name, b->name);
        return c != 0 ? c < 0 : a->id < b->id;
    });
    QVector<int> ids;
    for (const MediaBin* b : list) ids << b->id;
    return ids;
}

bool Project::binInside(int id, int ancestor) const
{
    for (int guard = 0; guard <= m_bins.size(); ++guard) { // Zyklen gibt es nicht (sanitizePool), trotzdem begrenzen
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
        if (it == state.media.cend()) continue; // später importiert -> unverändert lassen
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
    ids.remove(0); // 0 ist immer Master
    for (MediaBin& b : m_bins) {
        if (b.parent != 0 && !ids.contains(b.parent)) b.parent = 0;
    }
    // Eltern-Zyklen (kaputte Datei) aufbrechen: wer sich über die Eltern selbst erreicht, kommt unter Master
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
    m_undo.push(new PoolCommand(this, text, before, after)); // push ruft redo()
}

int Project::addBin(int parent, const QString& name)
{
    if (parent != 0 && !bin(parent)) parent = 0;
    QString n = name.trimmed();
    if (n.isEmpty()) { // wie DaVinci "Bin 1", "Bin 2" … (erste freie Nummer)
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
                // Reihenfolge wie in der Farbliste (Anzeige stabil)
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
