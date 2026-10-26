#include "engine/RenderQueue.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "engine/HardwareEncoders.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>

RenderQueue::RenderQueue(Project* project, QObject* parent)
    : QObject(parent), m_project(project), m_exporter(new Exporter(this))
{
    connect(m_exporter, &Exporter::progress, this, [this](int percent) {
        if (!m_current || m_preparing) return;
        m_percent = percent;
        emit progress(m_current, percent);
    });
    connect(m_exporter, &Exporter::started, this, [this] {
        if (!m_current) return;
        m_preparing = false;
        emit progress(m_current, 0);
    });
    connect(m_exporter, &Exporter::finished, this, &RenderQueue::onExportFinished);
}

ExportSettings RenderQueue::exportSettings(const RenderJob& job, const ProjectFormat& format)
{
    const RenderFormatInfo& f = renderFormat(job.settings.format);
    ExportSettings s;
    s.path = job.path;
    s.format = format;
    s.videoCodec = f.videoCodec;
    if (f.hardware) { // H.264/H.265: software or hardware encoder (may probe the graphics card once)
        const HardwareEncoders::Choice c = HardwareEncoders::resolve(job.settings.encoder, job.settings.format);
        s.videoCodec = c.codec;
        s.softwareCodec = c.fallback;
        s.encoderNote = c.note;
    }
    s.audioCodec = job.settings.audioCodec();
    s.audioSampleFormat = job.settings.audioSampleFormat();
    s.rate = job.settings.outputRate(format.rate);
    s.pixFmt = f.pixFmt;
    s.output = f.kind == RenderKind::Gif             ? ExportSettings::Output::Gif
               : f.kind == RenderKind::ImageSequence ? ExportSettings::Output::ImageSequence
                                                     : ExportSettings::Output::File;
    s.codecOptions = QString(f.options).split(' ', Qt::SkipEmptyParts);
    s.crf = job.settings.crf();
    s.audioBitrateK = job.settings.audioBitrateK;
    s.threads = Exporter::savedThreads(); // applies per machine, not per job
    // Size fixed when adding; without it (audio only, old file) per the settings
    s.size = job.size.isEmpty() ? job.settings.outputSize(format.size()) : job.size;
    if (job.settings.audioOnly()) s.size = format.size(); // is not rendered, but convert nothing
    s.burnSubtitles = job.settings.subtitles == RenderSettings::BurnSubtitles;
    // H.264/H.265 at quality "High" or "Web": copied pictures keep the file's bit rate (a camera or game recording often
    // has 50–130 Mbit/s), so "Medium"/"Small" encode everything and "Web" copies only below its cap (ExportWriter)
    s.smartRender = job.settings.smartRender && f.hardware
                    && (job.settings.quality == 0 || job.settings.quality == RenderSettings::kQualityWeb);
    if (job.settings.subtitles == RenderSettings::SrtFile) {
        const QFileInfo fi(job.path);
        s.subtitlePath = fi.dir().filePath(fi.completeBaseName() + ".srt");
    }
    s.maxBitrateK = job.settings.maxBitrateK(s.size, s.rate.num > 0 ? s.rate : format.rate);
    if (job.inOut) {
        s.from = std::max(0, job.from);
        s.to = job.to;
    }
    return s;
}

ExportSettings RenderQueue::exportSettings(const RenderJob& job, const Project& project)
{
    ExportSettings s = exportSettings(job, project.format());
    for (const MediaInfo& m : project.media()) s.mediaLengths.insert(m.path, m.length);
    return s;
}

bool RenderQueue::start(const QVector<int>& ids)
{
    if (isRunning()) return false;
    m_pending.clear();
    for (const RenderJob& j : m_project->renderQueue()) {
        const bool wanted = ids.isEmpty() ? j.status != RenderStatus::Done : ids.contains(j.id);
        if (wanted) m_pending << j.id;
    }
    if (m_pending.isEmpty()) return false;
    ++m_run;
    m_done = m_failed = 0;
    QVector<RenderJob> q = m_project->renderQueue(); // in one go: every change rebuilds the queue's cards
    for (RenderJob& j : q)
        if (m_pending.contains(j.id)) {
            j.status = RenderStatus::Queued;
            j.message.clear();
        }
    m_project->setRenderQueue(q);
    emit runningChanged(true);
    next();
    return true;
}

void RenderQueue::next()
{
    while (!m_pending.isEmpty()) {
        // The first job of this run in the queue's order as it is now (jobs may have been dragged meanwhile)
        const auto& q = m_project->renderQueue();
        const auto it = std::find_if(q.cbegin(), q.cend(), [this](const RenderJob& j) { return m_pending.contains(j.id); });
        if (it == q.cend()) { // the rest was deleted meanwhile
            m_pending.clear();
            break;
        }
        const RenderJob job = *it;
        const int id = job.id;
        m_pending.removeOne(id);
        m_current = id;
        m_percent = 0;
        m_preparing = true;
        setStatus(id, RenderStatus::Rendering);
        m_clock.start();
        emit preparing(id);
        // Auto or a hardware encoder needs the graphics card probe (once per session, a few seconds): wait for it
        // without blocking the window, then start
        if (renderFormat(job.settings.format).hardware && job.settings.encoder != VideoEncoder::Software
            && !HardwareEncoders::probed()) {
            HardwareEncoders::probeInBackground(); // no-op when it runs already
            m_probeWait = connect(HardwareEncoders::notifier(), &HardwareEncoders::Notifier::probed, this, [this] {
                disconnect(m_probeWait);
                if (!launch()) next();
            });
            if (!HardwareEncoders::probed()) return; // continues when the probe has finished
            disconnect(m_probeWait); // finished in between
        }
        if (launch()) return; // continues in onExportFinished
    }
    m_current = 0;
    m_preparing = false;
    emit runningChanged(false);
    emit finished(m_done, m_failed);
}

bool RenderQueue::launch()
{
    const int id = m_current;
    const auto& q = m_project->renderQueue();
    const auto it = std::find_if(q.cbegin(), q.cend(), [id](const RenderJob& j) { return j.id == id; });
    QString error;
    if (it == q.cend()) { // cannot happen: the running job is not deleted (RenderQueuePanel::removeJobs)
        error = T("Export abgebrochen.");
    } else if (it->sequence && !m_project->sequence(it->sequence)) {
        error = T("Die Timeline gibt es nicht mehr.");
    } else if (m_exporter->start(m_project->renderTimeline(it->sequence), exportSettings(*it, *m_project), &error)) {
        // Preparing (opening the files) goes on in the background until the exporter reports started()
        return true;
    }
    m_current = 0;
    m_preparing = false;
    ++m_failed;
    setStatus(id, RenderStatus::Failed, error);
    emit jobFinished(id, RenderStatus::Failed);
    return false;
}

void RenderQueue::onExportFinished(bool ok, const QString& message)
{
    const int id = m_current;
    if (!id) return;
    const int run = m_run; // cancel() from within jobFinished already ends the run itself
    m_current = 0;
    m_preparing = false;
    RenderStatus status = RenderStatus::Done;
    if (m_canceling) {
        status = RenderStatus::Canceled;
    } else if (!ok) {
        status = RenderStatus::Failed;
        ++m_failed;
    } else {
        ++m_done;
    }
    // Done: the encoder note (e.g. hardware encoder fell back to software) stays with the job
    setStatus(id, status, m_canceling ? QString() : ok ? m_exporter->note() : message, m_clock.elapsed(),
              ok ? m_exporter->usedCodec() : QString());
    emit jobFinished(id, status);
    if (m_canceling) {
        m_canceling = false;
        for (int rest : std::as_const(m_pending)) setStatus(rest, RenderStatus::Queued);
        m_pending.clear();
        emit runningChanged(false);
        emit finished(m_done, m_failed);
        return;
    }
    // Only start the next job after the exporter has been cleaned up
    QTimer::singleShot(0, this, [this, run] {
        if (run == m_run) next();
    });
}

void RenderQueue::cancel()
{
    if (!isRunning()) return;
    if (!m_current) { // between two jobs: do not start the rest anymore
        ++m_run;
        for (int rest : std::as_const(m_pending)) setStatus(rest, RenderStatus::Queued);
        m_pending.clear();
        emit runningChanged(false);
        emit finished(m_done, m_failed);
        return;
    }
    m_canceling = true;
    if (m_probeWait) { // still waiting for the graphics card probe: the exporter has not started
        disconnect(m_probeWait);
        onExportFinished(false, {});
        return;
    }
    m_exporter->cancel(); // reports finished() immediately
}

void RenderQueue::setStatus(int id, RenderStatus status, const QString& message, qint64 ms, const QString& encoder)
{
    QVector<RenderJob> q = m_project->renderQueue();
    for (RenderJob& j : q)
        if (j.id == id) {
            j.status = status;
            j.message = message;
            if (ms >= 0) j.renderMs = ms;
            if (!encoder.isEmpty()) j.encoderUsed = encoder;
        }
    m_project->setRenderQueue(q);
}
