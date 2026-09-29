#pragma once
#include "core/ProjectFile.h"
#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <QUndoStack>
#include <functional>

// Organisation eines Media-Pool-Clips (Teil von MediaInfo), für Undo getrennt gemerkt
struct MediaOrg {
    int bin = 0;
    QString color;
    QStringList flags;
    bool operator==(const MediaOrg& o) const { return bin == o.bin && color == o.color && flags == o.flags; }
};
// Stand der Media-Pool-Organisation (Bins + Organisation je Pfad) – Undo-Schnappschuss
struct PoolState {
    QVector<MediaBin> bins;
    QHash<QString, MediaOrg> media;
    bool operator==(const PoolState& o) const { return bins == o.bins && media == o.media; }
};

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

    // Media Pool wie DaVinci: Bins (Master = 0), Clipfarben, Flags. Jede Änderung ist ein Undo-Schritt
    // (betrifft nur die Organisation, nicht die Medienliste selbst).
    const QVector<MediaBin>& bins() const { return m_bins; }
    const MediaBin* bin(int id) const;
    QString binName(int id) const;                // 0 = "Master"
    QVector<int> childBins(int parent) const;     // nach Name sortiert (wie DaVinci)
    bool binInside(int id, int ancestor) const;   // id == ancestor oder darunter
    int addBin(int parent, const QString& name = {}); // Standardname "Bin 1", "Bin 2" …; liefert die id
    void renameBin(int id, const QString& name);
    void removeBin(int id);                       // Inhalt (Clips, Unter-Bins) wandert in den Eltern-Bin
    void moveBin(int id, int parent);
    void moveMediaToBin(const QStringList& paths, int bin);
    void setClipColor(const QStringList& paths, const QString& colorId); // leer = keine Clipfarbe
    void setFlag(const QStringList& paths, const QString& flagId, bool on);
    void clearFlags(const QStringList& paths);
    PoolState poolState() const;
    void applyPool(const PoolState& state); // nur für Undo

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
    void poolChanged(); // Bins/Clipfarben/Flags geändert (Medienliste gleich)
    void mediaMarksChanged(const QString& path); // Quell-In/Out geändert (ohne mediaChanged)
    void modifiedChanged(bool modified);

private:
    ProjectFormat m_format;
    Timeline m_timeline;
    QVector<MediaInfo> m_media;
    QVector<MediaBin> m_bins;
    int m_lastBinId = 0;
    QUndoStack m_undo;
    int m_lastClipId = 0;
    int m_lastLinkId = 0;
    int m_mergeSession = 0;
    void editPool(const QString& text, const std::function<void(PoolState&)>& fn);
    void sanitizePool(); // Verweise auf fehlende Bins -> Master, Eltern-Zyklen auflösen
    bool m_mediaDirty = false;
};
