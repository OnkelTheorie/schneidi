#pragma once
#include "core/ProjectFile.h"
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
    // Beim Abbau meldet der Undo-Stack noch "cleanChanged" -> nicht mehr weiterreichen (Hauptfenster ist schon weg)
    ~Project() override { blockSignals(true); }

    // Projekteinstellungen (Timeline-Auflösung und -Framerate)
    const ProjectFormat& format() const { return m_format; }
    // Ganze Frames pro Timecode-Sekunde (29,97 -> 30) – für Timecode und Standardlängen ("1 s")
    int fps() const { return m_format.rate.timebase(); }
    double frameRate() const { return m_format.rate.fps(); }
    // Wie DaVinci: Framerate nur änderbar, solange keine Clips in der Timeline liegen
    bool frameRateLocked() const;
    // Ein Undo-Schritt; neue Auflösung rechnet Positionen/Größen im Schnitt mit um (scaleTimeline).
    // Eine gesperrte Framerate bleibt unverändert.
    void setFormat(const ProjectFormat& format);
    void applyFormat(const ProjectFormat& format, const Timeline& tl); // nur für Undo
    const Timeline& timeline() const { return m_timeline; }
    QUndoStack* undoStack() { return &m_undo; }

    // mergeKey: aufeinanderfolgende Edits mit gleichem Schlüssel werden ein Undo-Schritt
    // (z. B. Regler im Inspector ziehen). closeMerge() beendet die Serie.
    void edit(const QString& text, const std::function<void(Timeline&)>& fn, const QString& mergeKey = {});
    void closeMerge() { ++m_mergeSession; }
    void setTimeline(const Timeline& tl); // nur für Undo/Laden

    const QVector<MediaInfo>& media() const { return m_media; }
    const MediaInfo* mediaInfo(const QString& path) const;
    void addMedia(const MediaInfo& info);
    // Quell-In/Out eines Media-Pool-Clips (-1 = keiner). Nicht im Undo (wie DaVinci), gilt aber als Änderung.
    void setMediaMarks(const QString& path, int markIn, int markOut);
    // Medieninfos ersetzen (z. B. Längen nach einer neuen Framerate), gilt nicht als Änderung
    void replaceMedia(const QVector<MediaInfo>& media);

    // Speichern/Laden (.schneidi). load() leert den Undo-Verlauf wie ein frisch geöffnetes Projekt.
    ProjectData data() const;
    void load(const ProjectData& d);
    void reset(); // leeres Projekt
    bool isModified() const { return m_mediaDirty || !m_undo.isClean(); }
    void markSaved();
    void markModified();

    int newClipId() { return ++m_lastClipId; }
    int newLinkId() { return ++m_lastLinkId; }

signals:
    void timelineChanged();
    void formatChanged(); // kommt vor dem zugehörigen timelineChanged()
    void mediaChanged();
    void mediaMarksChanged(const QString& path); // Quell-In/Out geändert (ohne mediaChanged)
    void modifiedChanged(bool modified);

private:
    ProjectFormat m_format;
    Timeline m_timeline;
    QVector<MediaInfo> m_media;
    QUndoStack m_undo;
    int m_lastClipId = 0;
    int m_lastLinkId = 0;
    int m_mergeSession = 0;
    bool m_mediaDirty = false;
};
