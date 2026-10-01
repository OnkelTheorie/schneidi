#pragma once
// Render-Warteschlange wie in DaVinci (Deliver → Render Queue): rendert die Aufträge des Projekts nacheinander
// über den Exporter, meldet Fortschritt und setzt den Status je Auftrag im Projekt (wird mit ihm gespeichert).

#include "core/RenderJob.h"
#include "engine/Exporter.h"

#include <QElapsedTimer>
#include <QObject>
#include <QVector>

class Project;

class RenderQueue : public QObject {
    Q_OBJECT
public:
    explicit RenderQueue(Project* project, QObject* parent = nullptr);

    // auch zwischen zwei Aufträgen (nächster startet per Timer)
    bool isRunning() const { return m_current > 0 || !m_pending.isEmpty(); }
    int currentJob() const { return m_current; } // id, 0 = keiner
    int currentProgress() const { return m_percent; }

    // Rendert die Aufträge ids (in Reihenfolge der Warteschlange); leer = alle, die noch nicht fertig sind.
    // Fertige Aufträge in ids werden neu gerendert. false = nichts zu tun oder läuft schon.
    bool start(const QVector<int>& ids = {});
    // Laufenden Auftrag abbrechen (halbe Datei wird gelöscht) und die restlichen stehen lassen
    void cancel();

    // Einstellungen für den Exporter (Ausgabegröße, Codecs, Bereich) aus einem Auftrag
    static ExportSettings exportSettings(const RenderJob& job, const ProjectFormat& format);

signals:
    void progress(int jobId, int percent);
    void runningChanged(bool running);
    void jobFinished(int jobId, RenderStatus status);
    void finished(int done, int failed); // Warteschlange durchgelaufen bzw. abgebrochen

private:
    void next();
    void onExportFinished(bool ok, const QString& message);
    void setStatus(int id, RenderStatus status, const QString& message = {}, qint64 ms = -1);

    Project* m_project;
    Exporter* m_exporter;
    QVector<int> m_pending;
    int m_run = 0; // zählt Start/Abbruch hoch: ein noch geplanter Folgeauftrag eines alten Laufs startet nicht
    int m_current = 0;
    int m_percent = 0;
    int m_done = 0, m_failed = 0;
    bool m_canceling = false;
    QElapsedTimer m_clock;
};
