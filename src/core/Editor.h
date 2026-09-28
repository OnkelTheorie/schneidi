#pragma once
#include "core/TimelineOps.h"
#include "core/Types.h"

#include <QObject>

class Project;
class Selection;

// Bearbeitungs-Operationen auf Nutzerebene (was ein Klick/Shortcut auslöst).
// UI ruft nur Editor-Methoden auf, nie direkt TimelineOps -> neue Werkzeuge
// werden hier ergänzt, ohne UI und Engine anzufassen.
class Editor : public QObject {
    Q_OBJECT
public:
    Editor(Project* project, Selection* selection, QObject* parent = nullptr);

    Project* project() const { return m_project; }
    Selection* selection() const { return m_selection; }

    bool linkedSelection() const { return m_linkedSelection; }
    void setLinkedSelection(bool on) { m_linkedSelection = on; }
    // Schnitt am Playhead: an = auf allen Spuren der Auswahl (auch der nächste Clip daneben),
    // aus = nur die ausgewählten Clips. Ohne Auswahl immer alle Clips unter dem Playhead.
    bool splitOnSelectedTracks() const { return m_splitOnSelectedTracks; }
    void setSplitOnSelectedTracks(bool on) { m_splitOnSelectedTracks = on; }

    // Clip + verknüpfte Partner (falls "Linked Selection" an ist)
    QVector<int> withLinked(const QVector<int>& ids) const;

    // Legt Media ab: Video auf V[track], Audio auf A[track] (wie DaVinci, V2 <-> A2).
    // Fehlende Spuren werden angelegt. Mehrere Dateien landen hintereinander.
    void addMediaAt(const QStringList& paths, int frame, int track = 0);
    void toggleTrackMute(TrackRef ref);
    void toggleTrackHidden(TrackRef ref);
    void moveClips(const QVector<int>& ids, int deltaFrames, TrackKind kind, int trackDelta);
    // Kante eines Clips (und verknüpfter Partner) ziehen, ohne Ripple
    void trimClip(int clipId, TimelineOps::Edge edge, int delta);
    // Wie weit sich die Kante tatsächlich bewegen lässt (für die Live-Vorschau)
    int clampTrim(int clipId, TimelineOps::Edge edge, int delta) const;
    // Lautstärke eines Audioclips (dB, wird auf kMinVolumeDb..kMaxVolumeDb begrenzt)
    void setClipVolume(int clipId, double db);
    // Beliebige Clip-Eigenschaft ändern (Inspector). Gleicher mergeKey = ein Undo-Schritt.
    void modifyClips(const QVector<int>& ids, const QString& text, const std::function<void(Clip&)>& fn,
                     const QString& mergeKey = {});
    void bladeAt(int clipId, int frame);
    void splitAtPlayhead(int frame);
    void deleteSelection();
    void rippleDeleteSelection();
    void selectAll();
    // Auswahl um `frames` verschieben (Nudge, wie , und . in DaVinci)
    void nudgeSelection(int frames);
    // Anfang/Ende bis zum Playhead trimmen (Shift+[ / Shift+]); Auswahl oder Clips unter dem Playhead
    void trimToPlayhead(TimelineOps::Edge edge, int frame);
    void toggleSelectionEnabled();
    // Ausgewählte Clips verknüpfen, bzw. trennen, wenn sie schon verknüpft sind
    void toggleLinkSelection();
    void toggleMarker(int frame);

    // Zwischenablage (Strg+C/X/V): Einfügen am Playhead auf denselben Spuren, überschreibt
    void copySelection();
    void cutSelection();
    void paste(int frame);

private:
    TimelineOps::SourceLength sourceLength() const;

    // Clips unter dem Playhead bzw. die Auswahl (mit Partnern), wie DaVinci bei Strg+B
    QVector<int> targetIds(int frame) const;

    struct ClipboardItem {
        Clip clip;
        TrackRef ref;
    };

    Project* m_project;
    Selection* m_selection;
    bool m_linkedSelection = true;
    bool m_splitOnSelectedTracks = true;
    QVector<ClipboardItem> m_clipboard; // Starts relativ zum frühesten Clip
};
