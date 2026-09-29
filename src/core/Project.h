#pragma once
#include "core/ProjectFile.h"
#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <QUndoStack>
#include <functional>

// Leere Timeline mit den Standardspuren (neues Projekt, neue Timeline)
Timeline emptyTimeline();

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
    QHash<int, int> sequenceBins; // Sequenz-id -> Bin (Timelines/Compound Clips im Media Pool)
    bool operator==(const PoolState& o) const
    {
        return bins == o.bins && media == o.media && sequenceBins == o.sequenceBins;
    }
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
    // Wie DaVinci: Framerate nur änderbar, solange in keiner Timeline Clips liegen
    bool frameRateLocked() const;
    // Ein Undo-Schritt; neue Auflösung rechnet Positionen/Größen in allen Timelines mit um (scaleTimeline).
    // Eine gesperrte Framerate bleibt unverändert.
    void setFormat(const ProjectFormat& format);
    void applyFormat(const ProjectFormat& format, const QVector<Sequence>& sequences); // nur für Undo
    // Die gerade geöffnete Timeline (Sequenz currentSequence()); alle Bearbeitungen wirken auf sie
    const Timeline& timeline() const { return current().timeline; }
    QUndoStack* undoStack() { return &m_undo; }

    // mergeKey: aufeinanderfolgende Edits mit gleichem Schlüssel werden ein Undo-Schritt
    // (z. B. Regler im Inspector ziehen). closeMerge() beendet die Serie.
    void edit(const QString& text, const std::function<void(Timeline&)>& fn, const QString& mergeKey = {});
    void closeMerge() { ++m_mergeSession; }
    void setTimeline(const Timeline& tl); // nur für Undo/Laden (aktuelle Sequenz)
    // Undo: Timeline einer Sequenz setzen; ist sie nicht geöffnet, wird sie geöffnet (man sieht, was sich ändert)
    void setTimeline(int sequenceId, const Timeline& tl);

    // ---- Sequenzen: mehrere Timelines pro Projekt und Compound Clips (wie DaVinci) ----
    // Alle Sequenzen (normale Timelines und Compound Clips) in Anlegereihenfolge
    const QVector<Sequence>& sequences() const { return m_sequences; }
    const Sequence* sequence(int id) const;
    int currentSequence() const { return m_current; }
    QString sequenceName(int id) const;
    // Öffnen (kein Undo, wie DaVinci); meldet currentSequenceChanged + timelineChanged
    void setCurrentSequence(int id);
    // Neue leere Timeline (Standardspuren) im Bin, wird geöffnet; Name leer = „Timeline N“. Ein Undo-Schritt.
    int addTimeline(const QString& name = {}, int bin = 0);
    void renameSequence(int id, const QString& name);
    // Kopie (neue Clip-ids), Name „… Kopie“; liefert die neue id
    int duplicateSequence(int id);
    // Löschen geht nicht bei der letzten normalen Timeline und bei Sequenzen, die ein Compound Clip benutzt
    bool canRemoveSequence(int id) const;
    bool removeSequence(int id);
    void moveSequencesToBin(const QVector<int>& ids, int bin);
    // Wird die Sequenz irgendwo (auch in anderen Sequenzen) als Compound Clip benutzt?
    bool sequenceUsed(int id) const;
    // Darf `child` als Clip in `parent` liegen (keine Schleife: child enthält parent auch nicht indirekt)?
    bool canNest(int child, int parent) const;
    int sequenceLength(int id) const; // Ende der Sequenz (Frames), 0 = leer/unbekannt
    // Name eines Clips wie in der Timeline (Compound Clips: Name der Sequenz)
    QString clipName(const Clip& c) const;
    // Timeline mit dem Inhalt aller Sequenzen (Timeline::nested) zum Rendern; id 0 = aktuelle
    Timeline renderTimeline(int sequenceId = 0) const;
    // Mehrere Sequenzen in einem Undo-Schritt ändern (Compound Clip erstellen/auflösen …); current = danach geöffnete
    void editSequences(const QString& text, const std::function<void(QVector<Sequence>&, int& current)>& fn);
    void applySequences(const QVector<Sequence>& sequences, int current); // nur für Undo
    int newSequenceId() { return ++m_lastSequenceId; }

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
    void moveMediaToBin(const QStringList& paths, int bin, const QVector<int>& sequences = {}); // auch Sequenzen
    void setClipColor(const QStringList& paths, const QString& colorId); // leer = keine Clipfarbe
    void setFlag(const QStringList& paths, const QString& flagId, bool on);
    void clearFlags(const QStringList& paths);
    PoolState poolState() const;
    void applyPool(const PoolState& state); // nur für Undo

    // Deliver: Render-Warteschlange. Kein Undo (wie DaVinci), gilt aber als Änderung (im Projekt gespeichert).
    const QVector<RenderJob>& renderQueue() const { return m_renderQueue; }
    void setRenderQueue(const QVector<RenderJob>& jobs);

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
    void currentSequenceChanged(); // andere Timeline geöffnet (kommt vor timelineChanged)
    void sequencesChanged();       // Sequenzliste/Namen/Bins geändert
    void formatChanged(); // kommt vor dem zugehörigen timelineChanged()
    void mediaChanged();
    void poolChanged(); // Bins/Clipfarben/Flags geändert (Medienliste gleich)
    void mediaMarksChanged(const QString& path); // Quell-In/Out geändert (ohne mediaChanged)
    void modifiedChanged(bool modified);
    void renderQueueChanged();

private:
    ProjectFormat m_format;
    QVector<Sequence> m_sequences; // nie leer
    int m_current = 1;             // id der geöffneten Sequenz
    int m_lastSequenceId = 1;
    Sequence& current();
    const Sequence& current() const;
    QVector<MediaInfo> m_media;
    QVector<MediaBin> m_bins;
    QVector<RenderJob> m_renderQueue;
    int m_lastBinId = 0;
    QUndoStack m_undo;
    int m_lastClipId = 0;
    int m_lastLinkId = 0;
    int m_mergeSession = 0;
    void editPool(const QString& text, const std::function<void(PoolState&)>& fn);
    void sanitizePool(); // Verweise auf fehlende Bins -> Master, Eltern-Zyklen auflösen
    bool m_mediaDirty = false;
};
