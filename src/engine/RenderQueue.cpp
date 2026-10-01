#include "engine/RenderQueue.h"

#include "core/I18n.h"
#include "core/Project.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>

RenderQueue::RenderQueue(Project* project, QObject* parent)
    : QObject(parent), m_project(project), m_exporter(new Exporter(this))
{
    connect(m_exporter, &Exporter::progress, this, [this](int percent) {
        if (!m_current) return;
        m_percent = percent;
        emit progress(m_current, percent);
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
    s.audioCodec = job.settings.audioCodec();
    s.audioSampleFormat = job.settings.audioSampleFormat();
    s.rate = job.settings.outputRate(format.rate);
    s.pixFmt = f.pixFmt;
    s.crf = job.settings.crf();
    s.audioBitrateK = job.settings.audioBitrateK;
    s.cores = Exporter::savedCores(); // gilt pro Rechner, nicht pro Auftrag
    // Größe beim Hinzufügen festgehalten; ohne (nur Audio, alte Datei) nach den Einstellungen
    s.size = job.size.isEmpty() ? job.settings.outputSize(format.size()) : job.size;
    if (job.settings.audioOnly()) s.size = format.size(); // wird nicht gerendert, aber nichts umrechnen
    s.burnSubtitles = job.settings.subtitles == RenderSettings::BurnSubtitles;
    if (job.settings.subtitles == RenderSettings::SrtFile) {
        const QFileInfo fi(job.path);
        s.subtitlePath = fi.dir().filePath(fi.completeBaseName() + ".srt");
    }
    if (job.inOut) {
        s.from = std::max(0, job.from);
        s.to = job.to;
    }
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
    for (int id : m_pending) setStatus(id, RenderStatus::Queued);
    emit runningChanged(true);
    next();
    return true;
}

void RenderQueue::next()
{
    while (!m_pending.isEmpty()) {
        const int id = m_pending.takeFirst();
        const auto& q = m_project->renderQueue();
        const auto it = std::find_if(q.cbegin(), q.cend(), [id](const RenderJob& j) { return j.id == id; });
        if (it == q.cend()) continue; // inzwischen gelöscht
        const RenderJob job = *it;
        m_current = id;
        m_percent = 0;
        setStatus(id, RenderStatus::Rendering);
        m_clock.start();
        QString error;
        if (job.sequence && !m_project->sequence(job.sequence)) {
            error = T("Die Timeline gibt es nicht mehr.");
        } else if (m_exporter->start(m_project->renderTimeline(job.sequence), exportSettings(job, m_project->format()),
                                     &error)) {
            emit progress(id, 0);
            return; // weiter in onExportFinished
        }
        m_current = 0;
        ++m_failed;
        setStatus(id, RenderStatus::Failed, error);
        emit jobFinished(id, RenderStatus::Failed);
    }
    m_current = 0;
    emit runningChanged(false);
    emit finished(m_done, m_failed);
}

void RenderQueue::onExportFinished(bool ok, const QString& message)
{
    const int id = m_current;
    if (!id) return;
    const int run = m_run; // cancel() aus jobFinished heraus beendet den Lauf schon selbst
    m_current = 0;
    RenderStatus status = RenderStatus::Done;
    if (m_canceling) {
        status = RenderStatus::Canceled;
    } else if (!ok) {
        status = RenderStatus::Failed;
        ++m_failed;
    } else {
        ++m_done;
    }
    setStatus(id, status, ok || m_canceling ? QString() : message, m_clock.elapsed());
    emit jobFinished(id, status);
    if (m_canceling) {
        m_canceling = false;
        for (int rest : std::as_const(m_pending)) setStatus(rest, RenderStatus::Queued);
        m_pending.clear();
        emit runningChanged(false);
        emit finished(m_done, m_failed);
        return;
    }
    // Nächsten Auftrag erst nach dem Aufräumen des Exporters starten
    QTimer::singleShot(0, this, [this, run] {
        if (run == m_run) next();
    });
}

void RenderQueue::cancel()
{
    if (!isRunning()) return;
    if (!m_current) { // zwischen zwei Aufträgen: Rest nicht mehr starten
        ++m_run;
        for (int rest : std::as_const(m_pending)) setStatus(rest, RenderStatus::Queued);
        m_pending.clear();
        emit runningChanged(false);
        emit finished(m_done, m_failed);
        return;
    }
    m_canceling = true;
    m_exporter->cancel(); // meldet finished() sofort
}

void RenderQueue::setStatus(int id, RenderStatus status, const QString& message, qint64 ms)
{
    QVector<RenderJob> q = m_project->renderQueue();
    for (RenderJob& j : q)
        if (j.id == id) {
            j.status = status;
            j.message = message;
            if (ms >= 0) j.renderMs = ms;
        }
    m_project->setRenderQueue(q);
}
