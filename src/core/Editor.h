#pragma once
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

    // Clip + verknüpfte Partner (falls "Linked Selection" an ist)
    QVector<int> withLinked(const QVector<int>& ids) const;

    // Legt Media ab: Video auf V[track], Audio auf A[track] (wie DaVinci, V2 <-> A2).
    // Fehlende Spuren werden angelegt. Mehrere Dateien landen hintereinander.
    void addMediaAt(const QStringList& paths, int frame, int track = 0);
    void toggleTrackMute(TrackRef ref);
    void toggleTrackHidden(TrackRef ref);
    void moveClips(const QVector<int>& ids, int deltaFrames, TrackKind kind, int trackDelta);
    void bladeAt(int clipId, int frame);
    void splitAtPlayhead(int frame);
    void deleteSelection();

private:
    Project* m_project;
    Selection* m_selection;
    bool m_linkedSelection = true;
};
