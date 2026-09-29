#include "core/Project.h"

#include "core/I18n.h"

#include <QUndoCommand>

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

Track makeTrack(TrackKind kind, const QString& name)
{
    Track t;
    t.kind = kind;
    t.name = name;
    return t;
}

} // namespace

Timeline emptyTimeline()
{
    // Standard wie ein leeres DaVinci-Projekt, nur mit je einer Spur mehr
    Timeline tl;
    tl.video << makeTrack(TrackKind::Video, "V1") << makeTrack(TrackKind::Video, "V2");
    tl.audio << makeTrack(TrackKind::Audio, "A1") << makeTrack(TrackKind::Audio, "A2");
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
    m_timeline = d.timeline;
    if (m_timeline.video.isEmpty()) m_timeline.video << makeTrack(TrackKind::Video, "V1");
    if (m_timeline.audio.isEmpty()) m_timeline.audio << makeTrack(TrackKind::Audio, "A1");
    m_lastClipId = d.lastClipId;
    m_lastLinkId = d.lastLinkId;
    markSaved();
    emit formatChanged();
    emit mediaChanged();
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
