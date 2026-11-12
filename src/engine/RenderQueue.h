// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Render queue like in DaVinci (Deliver → Render Queue): renders the jobs of the project one after another
// via the exporter, reports progress and sets the status per job in the project (saved with it).

#include "core/RenderJob.h"
#include "engine/Exporter.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class Project;
struct ProjectData;
class QThread;

class RenderQueue : public QObject {
    Q_OBJECT
public:
    explicit RenderQueue(Project* project, QObject* parent = nullptr);
    ~RenderQueue() override;

    // also between two jobs (the next one starts via timer)
    bool isRunning() const { return m_current > 0 || !m_pending.isEmpty(); }
    int currentJob() const { return m_current; } // id, 0 = none
    int currentProgress() const { return m_percent; }
    // The current job is being prepared: waiting for the graphics card probe, opening its files, stabilization
    // analyses. All of it in the background (the window stays usable, cancel() works), then progress(id, 0).
    bool isPreparing() const { return m_current > 0 && m_preparing; }

    // Renders the jobs ids in queue order (as it is when the next job starts: dragging waiting jobs changes which one
    // comes next); empty = all that are not finished yet.
    // Finished jobs in ids are rendered again. false = nothing to do or already running.
    bool start(const QVector<int>& ids = {});
    // Cancel the running job (half file is deleted) and leave the rest as they are
    void cancel();

    // Settings for the exporter (output size, codecs, range) from a job. Picks the video encoder: with Auto or a
    // hardware encoder this may wait once per session for the graphics card probe (HardwareEncoders).
    static ExportSettings exportSettings(const RenderJob& job, const ProjectFormat& format);
    // The same with the project's media lengths (ExportSettings::mediaLengths)
    static ExportSettings exportSettings(const RenderJob& job, const Project& project);
    // A review render (marks drawn onto every frame): no smart render, the fastest encoder setting, lower quality; the
    // own H.264 encoder (no fast setting) becomes plain H.264
    static void makeReview(ExportSettings& s);

    // Review render (RenderJob::review): what is burned into its frames. Made by the app (cli/RenderReview: cuts,
    // beats of the music track, hits, sections …) from a copy of the project, in a worker thread while the job is
    // being prepared; called with the job's timeline (sequence id, 0 = the open one). `cancel` turns true when the job
    // is canceled or the queue goes away: return soon (anything; the result is dropped). The job renders the timeline
    // as it was when the copy was taken, so the marks fit it. Not set (a bare engine): a review job renders its small
    // fast picture without marks.
    using Overlay = std::function<void(uint8_t* rgba, int width, int height, int64_t frame)>;
    using ReviewMarks = std::function<Overlay(const ProjectData& project, int sequence, const std::atomic<bool>& cancel)>;
    static void setReviewMarks(ReviewMarks make);

signals:
    void progress(int jobId, int percent); // percent 0 = preparing has ended, frames are rendered
    void preparing(int jobId);             // a job starts: preparing (isPreparing()) until its first progress()
    void runningChanged(bool running);
    void jobFinished(int jobId, RenderStatus status);
    void finished(int done, int failed); // queue finished resp. canceled

private:
    void next();
    bool launch(); // starts the exporter for m_current; false = failed at once (status set)
    void makeReviewMarks(const RenderJob& job); // in a worker thread, then launch() again with m_overlay
    void onExportFinished(bool ok, const QString& message);
    void setStatus(int id, RenderStatus status, const QString& message = {}, qint64 ms = -1, const QString& encoder = {});

    Project* m_project;
    Exporter* m_exporter;
    QVector<int> m_pending;
    int m_run = 0; // counts up on start/cancel: a still scheduled follow-up job of an old run does not start
    int m_current = 0;
    int m_percent = 0;
    bool m_preparing = false;
    QMetaObject::Connection m_probeWait; // waiting for the graphics card probe (HardwareEncoders::notifier)
    // Review job: its marks are being made (m_marksJob = that run's token; a canceled one's result is dropped)
    int m_marksJob = 0;
    bool m_marksReady = false;
    Overlay m_overlay;
    std::optional<Timeline> m_marksTimeline; // the timeline the marks were made of (rendered as it was then)
    struct MarksThread {
        QPointer<QThread> thread;
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    QVector<MarksThread> m_threads; // still running mark threads (canceled and waited for when the queue goes)
    int m_done = 0, m_failed = 0;
    bool m_canceling = false;
    QElapsedTimer m_clock;
};
