#include "core/Project.h"

#include "core/I18n.h"

#include <QSet>
#include <QUndoCommand>
#include <algorithm>

namespace {

class SnapshotCommand : public QUndoCommand {
public:
    SnapshotCommand(Project* p, const QString& text, Timeline before, Timeline after, QString mergeKey)
        : QUndoCommand(text), m_project(p), m_before(std::move(before)), m_after(std::move(after)),
          m_mergeKey(std::move(mergeKey)) {}
    void undo() override { m_project->setTimeline(m_before); }
    void redo() override { m_project->setTimeline(m_after); }
    int id() const override { return m_mergeKey.isEmpty() ? -1 : 1; }
    bool mergeWith(const QUndoCommand* other) override
    {
        const auto* o = static_cast<const SnapshotCommand*>(other);
        if (o->m_mergeKey != m_mergeKey) return false;
        m_after = o->m_after;
        return true;
    }

private:
    Project* m_project;
    Timeline m_before, m_after;
    QString m_mergeKey;
};

// Projekteinstellungen ändern: Format und (umgerechneter) Schnitt zusammen, damit Undo beides zurücknimmt
class FormatCommand : public QUndoCommand {
public:
    FormatCommand(Project* p, ProjectFormat before, ProjectFormat after, Timeline tlBefore, Timeline tlAfter)
        : QUndoCommand(T("Projekteinstellungen")), m_project(p), m_before(before), m_after(after),
          m_tlBefore(std::move(tlBefore)), m_tlAfter(std::move(tlAfter)) {}
    void undo() override { m_project->applyFormat(m_before, m_tlBefore); }
    void redo() override { m_project->applyFormat(m_after, m_tlAfter); }

private:
    Project* m_project;
    ProjectFormat m_before, m_after;
    Timeline m_tlBefore, m_tlAfter;
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
    m_timeline = emptyTimeline();
    connect(&m_undo, &QUndoStack::cleanChanged, this, [this] { emit modifiedChanged(isModified()); });
}

ProjectData Project::data() const
{
    ProjectData d;
    d.format = m_format;
    d.media = m_media;
    d.bins = m_bins;
    d.timeline = m_timeline;
    d.lastClipId = m_lastClipId;
    d.lastLinkId = m_lastLinkId;
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
    sanitizePool();
    m_timeline = d.timeline;
    if (m_timeline.video.isEmpty()) m_timeline.video << makeTrack(TrackKind::Video);
    if (m_timeline.audio.isEmpty()) m_timeline.audio << makeTrack(TrackKind::Audio);
    m_lastClipId = d.lastClipId;
    m_lastLinkId = d.lastLinkId;
    markSaved();
    emit formatChanged();
    emit mediaChanged();
    emit poolChanged();
    emit timelineChanged();
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
    Timeline after = m_timeline;
    fn(after);
    const QString key = mergeKey.isEmpty() ? QString() : QString("%1#%2").arg(mergeKey).arg(m_mergeSession);
    m_undo.push(new SnapshotCommand(this, text, m_timeline, after, key)); // push ruft redo()
}

bool Project::frameRateLocked() const
{
    for (const auto* tracks : {&m_timeline.video, &m_timeline.audio})
        for (const Track& t : *tracks)
            if (!t.clips.isEmpty()) return true;
    return false;
}

void Project::setFormat(const ProjectFormat& format)
{
    ProjectFormat f = format;
    if (frameRateLocked()) f.rate = m_format.rate;
    if (f == m_format) return;
    Timeline after = m_timeline;
    scaleTimeline(after, m_format.size(), f.size());
    m_undo.push(new FormatCommand(this, m_format, f, m_timeline, after)); // push ruft redo()
}

void Project::applyFormat(const ProjectFormat& format, const Timeline& tl)
{
    m_format = format;
    m_timeline = tl;
    emit formatChanged();
    emit timelineChanged();
}

void Project::setTimeline(const Timeline& tl)
{
    m_timeline = tl;
    emit timelineChanged();
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

void Project::moveMediaToBin(const QStringList& paths, int binId)
{
    if (binId != 0 && !bin(binId)) return;
    editPool(T("In Bin verschieben"), [&](PoolState& s) {
        for (const QString& p : paths)
            if (s.media.contains(p)) s.media[p].bin = binId;
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
