#include "core/Project.h"

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

Track makeTrack(TrackKind kind, const QString& name)
{
    Track t;
    t.kind = kind;
    t.name = name;
    return t;
}

} // namespace

Project::Project(QObject* parent) : QObject(parent)
{
    // Standard wie ein leeres DaVinci-Projekt, nur mit je einer Spur mehr
    m_timeline.video << makeTrack(TrackKind::Video, "V1") << makeTrack(TrackKind::Video, "V2");
    m_timeline.audio << makeTrack(TrackKind::Audio, "A1") << makeTrack(TrackKind::Audio, "A2");
}

void Project::edit(const QString& text, const std::function<void(Timeline&)>& fn, const QString& mergeKey)
{
    Timeline after = m_timeline;
    fn(after);
    const QString key = mergeKey.isEmpty() ? QString() : QString("%1#%2").arg(mergeKey).arg(m_mergeSession);
    m_undo.push(new SnapshotCommand(this, text, m_timeline, after, key)); // push ruft redo()
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
    emit mediaChanged();
}
