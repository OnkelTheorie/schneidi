#pragma once
// Render queue like in DaVinci (Deliver → Render Queue): renders the jobs of the project one after another
// via the exporter, reports progress and sets the status per job in the project (saved with it).

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

    // also between two jobs (the next one starts via timer)
    bool isRunning() const { return m_current > 0 || !m_pending.isEmpty(); }
    int currentJob() const { return m_current; } // id, 0 = none
    int currentProgress() const { return m_percent; }

    // Renders the jobs ids (in queue order); empty = all that are not finished yet.
    // Finished jobs in ids are rendered again. false = nothing to do or already running.
    bool start(const QVector<int>& ids = {});
    // Cancel the running job (half file is deleted) and leave the rest as they are
    void cancel();

    // Settings for the exporter (output size, codecs, range) from a job. Picks the video encoder: with Auto or a
    // hardware encoder this may wait once per session for the graphics card probe (HardwareEncoders).
    static ExportSettings exportSettings(const RenderJob& job, const ProjectFormat& format);

signals:
    void progress(int jobId, int percent);
    void runningChanged(bool running);
    void jobFinished(int jobId, RenderStatus status);
    void finished(int done, int failed); // queue finished resp. canceled

private:
    void next();
    void onExportFinished(bool ok, const QString& message);
    void setStatus(int id, RenderStatus status, const QString& message = {}, qint64 ms = -1, const QString& encoder = {});

    Project* m_project;
    Exporter* m_exporter;
    QVector<int> m_pending;
    int m_run = 0; // counts up on start/cancel: a still scheduled follow-up job of an old run does not start
    int m_current = 0;
    int m_percent = 0;
    int m_done = 0, m_failed = 0;
    bool m_canceling = false;
    QElapsedTimer m_clock;
};
