// SPDX-License-Identifier: AGPL-3.0-only
// Background jobs of reading commands (Live::runInBackground, see Live.h): a small queue on the UI thread, one QThread
// per running job (an event loop for transcribe's whisper process), the result delivered back queued.
#include "cli/Live.h"

#include "cli/CommandsDetail.h"
#include "core/LiveHost.h"
#include "core/PathKey.h"
#include "core/Project.h"
#include "engine/Background.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutex>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

namespace Cli::Live {

class BackgroundJob {
public:
    int id = 0;
    QString command;
    QJsonObject args; // normalized
    QStringList keys; // the material it reads (PathKey of files, "@app"): one job per key at a time
    AppCopy copy;
    std::function<void(double, const QString&)> onProgress;
    std::function<void(const QJsonObject&)> onDone;
    QPointer<QObject> receiver;
    bool hasReceiver = false;

    std::atomic<bool> cancel{false};
    std::atomic<double> progress{0};
    std::atomic<bool> progressQueued{false};
    QMutex stageMutex;
    QString stage;

    // UI thread only
    QThread* thread = nullptr;
    bool queued = false, done = false;
    QJsonObject reply; // written by the job's thread before it finishes
};

namespace {

QJsonObject errorReply(const QString& code, const QString& message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

// The command's own reading forms only (see Live.h)
bool reading(const QString& command, const QJsonObject& args, QString* why)
{
    static const QStringList commands{"probe", "peaks",    "scenes", "highlights", "silence",   "loudness",
                                      "beats", "overview", "index",  "transcribe", "frames"};
    const auto no = [why](const QString& text) {
        if (why) *why = text;
        return false;
    };
    if (!commands.contains(command))
        return no(QString("'%1' does not run in the background (only %2)").arg(command, commands.join(", ")));
    const auto given = [&args](const char* key) {
        const QJsonValue v = args.value(key);
        return !(v.isUndefined() || v.isNull() || v == QJsonValue(false) || (v.isString() && v.toString().isEmpty())
                 || (v.isArray() && v.toArray().isEmpty()));
    };
    if (command == "beats" && given("apply")) return no("beats with apply changes the project: not in the background");
    if (command == "transcribe" && given("apply"))
        return no("transcribe with apply changes the project: not in the background");
    if (command == "index" && given("annotate"))
        return no("index with annotate writes descriptions: not in the background");
    return true;
}

// What a job reads: its files and project (keys compare like the media pool's paths)
QStringList keysOf(const QJsonObject& args)
{
    QStringList keys;
    const auto add = [&keys](const QString& path) {
        if (path.trimmed().isEmpty()) return;
        const QString key = isAppProject(path) ? QString(AppProject) : PathKey::of(path);
        if (!keys.contains(key)) keys << key;
    };
    for (const char* k : {"file", "source", "project"}) add(args.value(k).toString());
    for (const char* k : {"files", "sources"})
        for (const QJsonValue& v : args.value(k).toArray()) add(v.isObject() ? v.toObject().value("file").toString() : v.toString());
    return keys;
}

// Does the job read the project open in `host`? ("@app", or its file by path)
bool readsApp(const QJsonObject& args, LiveHost* host)
{
    const QString appPath = host ? host->liveProjectPath() : QString();
    const auto isApp = [&](const QString& path) {
        if (isAppProject(path)) return true;
        return !appPath.isEmpty() && !path.isEmpty() && PathKey::same(PathKey::of(path), PathKey::of(appPath));
    };
    for (const char* k : {"file", "source", "project"})
        if (isApp(args.value(k).toString())) return true;
    for (const char* k : {"files", "sources"})
        for (const QJsonValue& v : args.value(k).toArray())
            if (isApp(v.isObject() ? v.toObject().value("file").toString() : v.toString())) return true;
    return false;
}

class Queue : public QObject {
public:
    explicit Queue(QObject* parent) : QObject(parent) {}
    ~Queue() override
    {
        stopAll();
        for (const auto& job : m_running) delete job->thread; // their finished calls never come now
    }

    static Queue& instance()
    {
        // On the UI thread, child of the application: gone (threads waited for) when it goes
        static QPointer<Queue> q;
        if (!q) q = new Queue(QCoreApplication::instance());
        return *q;
    }

    void add(const std::shared_ptr<BackgroundJob>& job)
    {
        job->queued = true;
        m_waiting.push_back(job);
        pump();
    }

    void cancel(const std::shared_ptr<BackgroundJob>& job)
    {
        if (job->done) return;
        job->cancel = true;
        if (!job->queued) return; // running: its thread ends with CANCELED
        job->queued = false;
        m_waiting.erase(std::remove(m_waiting.begin(), m_waiting.end(), job), m_waiting.end());
        job->reply = errorReply("CANCELED", "canceled");
        QTimer::singleShot(0, this, [this, job] { deliver(job); });
    }

    // A refused job: onDone later, as for every other
    void refuse(const std::shared_ptr<BackgroundJob>& job, const QJsonObject& reply)
    {
        job->reply = reply;
        QTimer::singleShot(0, this, [this, job] { deliver(job); });
    }

    int limit() const
    {
        if (m_limit > 0) return m_limit;
        return std::clamp(Background::threads(QThread::idealThreadCount() / 2) / 2, 1, 4);
    }
    void setLimit(int jobs)
    {
        m_limit = std::max(0, jobs);
        pump();
    }
    int count() const { return int(m_waiting.size() + m_running.size()); }

    void stopAll()
    {
        for (const auto& job : m_waiting) cancel(job);
        for (const auto& job : m_running) job->cancel = true;
        for (const auto& job : m_running)
            if (job->thread) job->thread->wait();
    }

private:
    void pump()
    {
        for (auto it = m_waiting.begin(); it != m_waiting.end() && int(m_running.size()) < limit();) {
            const std::shared_ptr<BackgroundJob> job = *it;
            const bool busy = std::any_of(m_running.begin(), m_running.end(), [&](const auto& r) {
                return std::any_of(job->keys.begin(), job->keys.end(), [&](const QString& k) { return r->keys.contains(k); });
            });
            if (busy) {
                ++it;
                continue;
            }
            it = m_waiting.erase(it);
            start(job);
        }
    }

    void start(const std::shared_ptr<BackgroundJob>& job)
    {
        job->queued = false;
        m_running.push_back(job);
        // The queue outlives every job's thread (its destructor waits for them)
        job->thread = QThread::create([job, queue = this] {
            Background::lowerThisThread(Background::Level::Low);
            const AppCopyScope scope(&job->copy); // also without data: marks the thread as a background job
            Context ctx;
            ctx.cancel = &job->cancel;
            const auto post = [job, queue] {
                if (job->progressQueued.exchange(true)) return;
                QMetaObject::invokeMethod(queue, [job] {
                    job->progressQueued = false;
                    if (job->done || !job->onProgress || (job->hasReceiver && !job->receiver)) return;
                    QString stage;
                    {
                        QMutexLocker lock(&job->stageMutex);
                        stage = job->stage;
                    }
                    job->onProgress(job->progress.load(), stage);
                }, Qt::QueuedConnection);
            };
            ctx.progress = [job, post](double p) {
                job->progress = std::clamp(p, 0.0, 1.0);
                post();
            };
            ctx.stage = [job, post](const QString& s) {
                {
                    QMutexLocker lock(&job->stageMutex);
                    job->stage = s;
                }
                post();
            };
            QJsonObject reply;
            try {
                const Command* cmd = find(job->command);
                reply = {{"ok", true}, {"result", execute(*cmd, job->args, ctx)}};
            } catch (const Error& e) {
                reply = errorReply(e.code, e.message);
            } catch (const std::exception& e) {
                reply = errorReply("INTERNAL", QString("the command failed: %1").arg(e.what()));
            } catch (...) {
                reply = errorReply("INTERNAL", "the command failed");
            }
            // A job canceled at its very end still says so (the caller asked it to stop and may have moved on)
            if (job->cancel.load() && reply.value("ok").toBool()) reply = errorReply("CANCELED", "canceled");
            job->reply = reply;
        });
        connect(job->thread, &QThread::finished, this, [this, job] {
            m_running.erase(std::remove(m_running.begin(), m_running.end(), job), m_running.end());
            job->thread->deleteLater();
            job->thread = nullptr;
            deliver(job);
            pump();
        });
        job->thread->start();
    }

    void deliver(const std::shared_ptr<BackgroundJob>& job)
    {
        if (job->done) return;
        job->done = true;
        job->copy.data.reset(); // the project copy is not needed any more
        if (job->hasReceiver && !job->receiver) return;
        if (job->onDone) job->onDone(job->reply);
    }

    std::deque<std::shared_ptr<BackgroundJob>> m_waiting;
    std::vector<std::shared_ptr<BackgroundJob>> m_running;
    int m_limit = 0;
};

} // namespace

void JobHandle::cancel() const
{
    if (m_job) Queue::instance().cancel(m_job);
}

bool JobHandle::running() const { return m_job && !m_job->done; }

int JobHandle::id() const { return m_job ? m_job->id : 0; }

bool runsInBackground(const QString& command, const QJsonObject& args, QString* why) { return reading(command, args, why); }

JobHandle runInBackground(const QString& command, const QJsonObject& args,
                          std::function<void(double, const QString&)> onProgress,
                          std::function<void(const QJsonObject&)> onDone, LiveHost* host, QObject* receiver)
{
    Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread());
    static int ids = 0;
    auto job = std::make_shared<BackgroundJob>();
    job->id = ++ids;
    job->command = command;
    job->onProgress = std::move(onProgress);
    job->onDone = std::move(onDone);
    job->receiver = receiver;
    job->hasReceiver = receiver != nullptr;
    Queue& queue = Queue::instance();
    JobHandle handle;
    handle.m_job = job;
    const Command* cmd = find(command);
    QString why;
    if (!cmd) {
        queue.refuse(job, errorReply("BAD_REQUEST", "no command " + command));
        return handle;
    }
    try {
        job->args = normalize(*cmd, args);
    } catch (const Error& e) {
        queue.refuse(job, errorReply(e.code, e.message));
        return handle;
    }
    if (!reading(command, job->args, &why)) {
        queue.refuse(job, errorReply("NOT_IN_BACKGROUND", why));
        return handle;
    }
    job->keys = keysOf(job->args);
    // The app's project: copied now, on the UI thread (the job never touches the app's objects)
    if (host && host->liveProject() && readsApp(job->args, host)) {
        job->copy.path = host->liveProjectPath();
        job->copy.data = std::make_shared<const ProjectData>(host->liveProject()->data());
        job->copy.app = appState(host);
    } else if (readsApp(job->args, nullptr)) {
        queue.refuse(job, errorReply("NO_APP", "\"@app\" in a background job needs the app's project"));
        return handle;
    }
    queue.add(job);
    return handle;
}

int backgroundLimit() { return Queue::instance().limit(); }

void setBackgroundLimit(int jobs) { Queue::instance().setLimit(jobs); }

int backgroundJobs() { return Queue::instance().count(); }

void cancelAllBackground() { Queue::instance().stopAll(); }

} // namespace Cli::Live
