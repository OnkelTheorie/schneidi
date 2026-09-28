#pragma once
#include "core/Types.h"

#include <QObject>
#include <QUndoStack>
#include <functional>

// Hält das Projekt (Media + Timeline). Jede Änderung der Timeline läuft über edit()
// und ist damit automatisch rückgängig machbar (Snapshot-Undo – einfach und robust;
// bei sehr großen Projekten später ggf. durch feinere Commands ersetzen).
class Project : public QObject {
    Q_OBJECT
public:
    explicit Project(QObject* parent = nullptr);

    int fps() const { return m_fps; }
    const Timeline& timeline() const { return m_timeline; }
    QUndoStack* undoStack() { return &m_undo; }

    void edit(const QString& text, const std::function<void(Timeline&)>& fn);
    void setTimeline(const Timeline& tl); // nur für Undo/Laden

    const QVector<MediaInfo>& media() const { return m_media; }
    const MediaInfo* mediaInfo(const QString& path) const;
    void addMedia(const MediaInfo& info);

    int newClipId() { return ++m_lastClipId; }
    int newLinkId() { return ++m_lastLinkId; }

signals:
    void timelineChanged();
    void mediaChanged();

private:
    int m_fps = 25;
    Timeline m_timeline;
    QVector<MediaInfo> m_media;
    QUndoStack m_undo;
    int m_lastClipId = 0;
    int m_lastLinkId = 0;
};
