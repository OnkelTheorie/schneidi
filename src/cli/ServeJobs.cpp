// Jobs and worker processes of `schneidi-cli serve`, see ServeJobs.h
#include "cli/ServeJobs.h"

#include "cli/Mcp.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>

namespace Cli::Serve {

namespace {
constexpr int kRecycleAfter = 200;            // calls; a fresh process now and then (caches, leaks)
constexpr qint64 kIdleStopMs = 10 * 60 * 1000; // idle workers go away (memory), new ones start when needed
constexpr qint64 kKeepEndedMs = 60 * 60 * 1000;
constexpr int kKeepEnded = 200;
} // namespace

QString stateName(Job::State s)
{
    switch (s) {
    case Job::State::Queued: return "queued";
    case Job::State::Running: return "running";
    case Job::State::Done: return "done";
    case Job::State::Failed: return "failed";
    case Job::State::Cancelled: return "cancelled";
    }
    return {};
}

// ---------- Worker ----------

Worker::Worker(const QString& program, const QStringList& args, const QString& dir, QObject* parent) : QObject(parent)
{
    m_proc.setProgram(program);
    m_proc.setArguments(args);
    m_proc.setWorkingDirectory(dir);
    m_proc.setProcessChannelMode(QProcess::ForwardedErrorChannel); // its warnings end up in the server's log
    connect(&m_proc, &QProcess::readyReadStandardOutput, this, &Worker::onOutput);
    connect(&m_proc, &QProcess::finished, this, &Worker::onGone);
    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        // Queued: it may come right out of start() below, before Jobs listens to `gone`
        if (e == QProcess::FailedToStart) QMetaObject::invokeMethod(this, &Worker::onGone, Qt::QueuedConnection);
    });
    m_idle.start();
    m_proc.start();
    send({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"},
          {"params", QJsonObject{{"protocolVersion", Mcp::versions().first()},
                                 {"clientInfo", QJsonObject{{"name", "schneidi-serve"}}}}}});
    send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
}

Worker::~Worker()
{
    m_gone = true; // no signals from the dying process
    disconnect(&m_proc, nullptr, this, nullptr);
    if (m_proc.state() != QProcess::NotRunning) {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }
}

void Worker::send(const QJsonObject& msg)
{
    m_proc.write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
}

void Worker::run(Job* job)
{
    m_job = job;
    m_state = State::Busy;
    ++m_calls;
    send({{"jsonrpc", "2.0"}, {"id", ++m_callId}, {"method", "tools/call"},
          {"params", QJsonObject{{"name", job->tool}, {"arguments", job->args},
                                 {"_meta", QJsonObject{{"progressToken", job->id}}}}}});
}

void Worker::kill()
{
    // Never picked again: an answer already in the pipe must not make it idle for the next call
    m_state = State::Stopping;
    if (m_proc.state() != QProcess::NotRunning) m_proc.kill();
}

void Worker::stop()
{
    m_state = State::Stopping;
    m_proc.closeWriteChannel();
}

void Worker::onOutput()
{
    m_buf += m_proc.readAllStandardOutput();
    for (qsizetype nl; (nl = m_buf.indexOf('\n')) >= 0;) {
        const QByteArray line = m_buf.left(nl).trimmed();
        m_buf.remove(0, nl + 1);
        if (!line.isEmpty()) onLine(line);
    }
}

void Worker::onLine(const QByteArray& line)
{
    const QJsonObject msg = QJsonDocument::fromJson(line).object();
    if (msg["method"].toString() == "notifications/progress") {
        if (m_state == State::Busy) emit progress(this, msg["params"]["progress"].toInt());
        return;
    }
    if (!msg.contains("id")) return;
    const int id = msg["id"].toInt(-1);
    if (id == 0 && m_state == State::Starting) {
        m_state = State::Idle;
        m_idle.restart();
        emit ready(this);
    } else if (id == m_callId && m_state == State::Busy) {
        const QJsonObject result = msg.contains("error")
                                       ? Mcp::toolError("INTERNAL", msg["error"]["message"].toString())
                                       : msg["result"].toObject();
        Job* job = m_job;
        m_job = nullptr;
        m_state = State::Idle;
        m_idle.restart();
        emit done(this, job, result);
    }
}

void Worker::onGone()
{
    if (m_gone) return;
    m_gone = true;
    const bool started = m_state != State::Starting;
    if (m_state != State::Busy) m_state = State::Stopping; // never picked for a call any more
    emit gone(this, started);
}

// ---------- Jobs ----------

Jobs::Jobs(const QStringList& workerArgs, const QString& workerDir, int maxWorkers, QObject* parent)
    : QObject(parent), m_args(workerArgs), m_dir(workerDir), m_max(std::max(1, maxWorkers))
{
    m_tag = QString::number(QRandomGenerator::system()->generate() & 0xffff, 16).rightJustified(4, '0');
    auto* tidy = new QTimer(this);
    connect(tidy, &QTimer::timeout, this, [this] {
        for (Worker* w : std::as_const(m_workers))
            if (w->state() == Worker::State::Idle && w->idleMs() > kIdleStopMs) w->stop();
        prune();
    });
    tidy->start(60 * 1000);
}

Jobs::~Jobs()
{
    for (Worker* w : std::as_const(m_workers)) delete w;
    m_workers.clear();
}

Job* Jobs::submit(std::unique_ptr<Job> job)
{
    prune();
    job->id = QString("job%1-%2").arg(m_next++).arg(m_tag);
    job->state = Job::State::Queued;
    job->created = QDateTime::currentDateTime();
    Job* j = job.get();
    m_jobs[j->id] = std::move(job);
    if (j->runHere) {
        j->state = Job::State::Running;
        j->started = QDateTime::currentDateTime();
        j->runHere(j);
        return j;
    }
    m_queue << j;
    schedule();
    return j;
}

Job* Jobs::find(const QString& id) const
{
    const auto it = m_jobs.find(id);
    return it == m_jobs.end() ? nullptr : it->second.get();
}

QVector<const Job*> Jobs::list() const
{
    QVector<const Job*> out;
    for (const auto& [id, j] : m_jobs) out << j.get();
    std::sort(out.begin(), out.end(), [](const Job* a, const Job* b) { return a->created > b->created || (a->created == b->created && a->id > b->id); });
    return out;
}

int Jobs::busyWorkers() const
{
    return int(std::count_if(m_workers.begin(), m_workers.end(), [](Worker* w) { return w->state() == Worker::State::Busy; }));
}

QVector<qint64> Jobs::workerPids() const
{
    QVector<qint64> pids;
    for (Worker* w : m_workers) pids << w->pid();
    return pids;
}

void Jobs::cancel(Job* job)
{
    if (job->over()) return;
    const bool queued = job->state == Job::State::Queued;
    job->state = Job::State::Cancelled;
    job->ended = QDateTime::currentDateTime();
    job->result = Mcp::toolError("CANCELLED", "the job was cancelled");
    if (queued) {
        m_queue.removeAll(job);
    } else if (job->cancelHere) {
        job->cancelHere();
    } else {
        // The worker stops right there (a half written output file stays); a new one starts when needed
        for (Worker* w : std::as_const(m_workers))
            if (w->job() == job) w->kill();
        if (!job->writes.isEmpty()) m_writing.remove(job->writes);
    }
    emit changed(job);
    schedule();
}

void Jobs::setProgress(Job* job, int percent)
{
    if (job->over() || percent == job->progress) return;
    job->progress = std::clamp(percent, 0, 100);
    emit changed(job);
}

void Jobs::end(Job* job, const QJsonObject& result)
{
    if (job->over()) return;
    job->result = result;
    job->state = result["isError"].toBool() ? Job::State::Failed : Job::State::Done;
    if (job->state == Job::State::Done) job->progress = 100;
    job->ended = QDateTime::currentDateTime();
    if (!job->writes.isEmpty()) m_writing.remove(job->writes);
    emit changed(job);
    schedule();
}

void Jobs::schedule()
{
    int waiting = 0;
    QSet<QString> claimed; // files of calls waiting in this round (the first in line gets the next worker)
    for (int i = 0; i < m_queue.size();) {
        Job* j = m_queue[i];
        if (!j->writes.isEmpty() && (m_writing.contains(j->writes) || claimed.contains(j->writes))) {
            ++i;
            continue;
        }
        const auto idle = std::find_if(m_workers.begin(), m_workers.end(),
                                       [](Worker* w) { return w->state() == Worker::State::Idle; });
        if (idle == m_workers.end()) {
            ++waiting;
            if (!j->writes.isEmpty()) claimed.insert(j->writes);
            ++i;
            continue;
        }
        m_queue.removeAt(i);
        if (!j->writes.isEmpty()) m_writing.insert(j->writes);
        j->state = Job::State::Running;
        j->started = QDateTime::currentDateTime();
        (*idle)->run(j);
        emit changed(j);
    }
    const int starting = int(std::count_if(m_workers.begin(), m_workers.end(),
                                           [](Worker* w) { return w->state() == Worker::State::Starting; }));
    // Stopping workers still count: they end soon and must not push the number of processes over the limit
    for (int n = starting; n < waiting && m_workers.size() < m_max; ++n) spawn();
}

void Jobs::spawn()
{
    auto* w = new Worker(QCoreApplication::applicationFilePath(), m_args, m_dir, this);
    connect(w, &Worker::ready, this, &Jobs::onReady);
    connect(w, &Worker::done, this, &Jobs::onDone);
    connect(w, &Worker::gone, this, &Jobs::onGone, Qt::QueuedConnection); // deletes it: not inside its own signal
    connect(w, &Worker::progress, this, [this](Worker* worker, int percent) {
        if (Job* j = worker->job()) setProgress(j, percent);
    });
    m_workers << w;
}

void Jobs::onReady(Worker*)
{
    m_startFailures = 0;
    schedule();
}

void Jobs::onDone(Worker* w, Job* job, const QJsonObject& result)
{
    if (w->calls() >= kRecycleAfter) w->stop();
    if (job->over()) { // cancelled meanwhile
        schedule();
        return;
    }
    end(job, m_filter ? m_filter(*job, result) : result);
}

void Jobs::onGone(Worker* w, bool started)
{
    m_workers.removeAll(w);
    Job* j = w->job();
    if (j && !j->over()) {
        if (!j->writes.isEmpty()) m_writing.remove(j->writes);
        j->state = Job::State::Failed;
        j->ended = QDateTime::currentDateTime();
        j->result = Mcp::toolError("WORKER_CRASHED", "the process running this call ended unexpectedly (a bug in "
                                                     "schneidi or broken material); you can try again");
        emit changed(j);
    }
    if (!started) ++m_startFailures;
    w->deleteLater();
    // A worker that cannot even start (program missing, MLT broken): do not try forever
    if (m_startFailures >= 3) {
        m_startFailures = 0;
        const QVector<Job*> queued = m_queue;
        m_queue.clear();
        for (Job* q : queued) {
            q->state = Job::State::Failed;
            q->ended = QDateTime::currentDateTime();
            q->result = Mcp::toolError("WORKER_FAILED", "the worker process does not start (see the server's log)");
            emit changed(q);
        }
        return;
    }
    schedule();
}

void Jobs::prune()
{
    const QDateTime now = QDateTime::currentDateTime();
    QVector<const Job*> ended;
    for (const auto& [id, j] : m_jobs)
        if (j->over()) ended << j.get();
    std::sort(ended.begin(), ended.end(), [](const Job* a, const Job* b) { return a->ended > b->ended; });
    for (int i = 0; i < ended.size(); ++i)
        if (i >= kKeepEnded || ended[i]->ended.msecsTo(now) > kKeepEndedMs) m_jobs.erase(ended[i]->id);
}

} // namespace Cli::Serve
