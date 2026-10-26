#pragma once
// Tool calls of `schneidi-cli serve` as jobs (Serve.cpp): each call runs in one of a few worker processes
// (`schneidi-cli mcp` on stdio, same program, started when needed and again after a crash). Why processes: commands
// run synchronously on MLT/FFmpeg (not made for threads), a long render must not hold up other clients and a crash
// must not take the server down. Calls that change a project run one after another per project file; everything
// else runs as soon as a worker is free. A job stays listed (with its result) for an hour after it ended.

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>

namespace Cli::Serve {

struct Job {
    enum class State { Queued, Running, Done, Failed, Cancelled };

    QString id;
    QString tool;
    QJsonObject args;       // normalized arguments
    QString writes;         // project file the call changes (one call at a time per file); empty = it only reads
    State state = State::Queued;
    int progress = 0;       // percent
    QJsonObject result;     // MCP tool result once ended (content, isError)
    QDateTime created, started, ended;
    QString baseUrl;        // for download links in the result
    // A tool of the server itself (upload): runs in this process, ends through Jobs::end()
    std::function<void(Job*)> runHere;
    std::function<void()> cancelHere;

    bool over() const { return state == State::Done || state == State::Failed || state == State::Cancelled; }
};

QString stateName(Job::State s);

// One `schneidi-cli mcp` process
class Worker : public QObject {
    Q_OBJECT
public:
    enum class State { Starting, Idle, Busy, Stopping };

    Worker(const QString& program, const QStringList& args, const QString& dir, QObject* parent = nullptr);
    ~Worker() override;
    State state() const { return m_state; }
    Job* job() const { return m_job; }
    int calls() const { return m_calls; }
    qint64 idleMs() const { return m_idle.elapsed(); }
    qint64 pid() const { return m_proc.processId(); }
    void run(Job* job);
    void kill();
    void stop(); // ends after the current call (stdin closed)

signals:
    void ready(Cli::Serve::Worker* w);
    void progress(Cli::Serve::Worker* w, int percent);
    void done(Cli::Serve::Worker* w, Cli::Serve::Job* job, const QJsonObject& result);
    void gone(Cli::Serve::Worker* w, bool started); // started = it had answered initialize

private:
    void onOutput();
    void onLine(const QByteArray& line);
    void onGone();
    void send(const QJsonObject& msg);

    QProcess m_proc;
    State m_state = State::Starting;
    Job* m_job = nullptr;
    QByteArray m_buf;
    int m_callId = 0;
    int m_calls = 0;
    bool m_gone = false;
    QElapsedTimer m_idle;
};

class Jobs : public QObject {
    Q_OBJECT
public:
    Jobs(const QStringList& workerArgs, const QString& workerDir, int maxWorkers, QObject* parent = nullptr);
    ~Jobs() override;

    Job* submit(std::unique_ptr<Job> job);
    Job* find(const QString& id) const;
    QVector<const Job*> list() const; // newest first
    void cancel(Job* job);
    // Jobs that run here (runHere) report through these
    void setProgress(Job* job, int percent);
    void end(Job* job, const QJsonObject& result);
    // Result of a worker before it is stored (download links)
    void setResultFilter(std::function<QJsonObject(const Job&, const QJsonObject&)> f) { m_filter = std::move(f); }
    int maxWorkers() const { return m_max; }
    int busyWorkers() const;
    QVector<qint64> workerPids() const;

signals:
    void changed(Cli::Serve::Job* job); // progress or state

private:
    void schedule();
    void spawn();
    void onReady(Worker* w);
    void onDone(Worker* w, Job* job, const QJsonObject& result);
    void onGone(Worker* w, bool started);
    void prune();

    QStringList m_args;
    QString m_dir;
    int m_max;
    std::map<QString, std::unique_ptr<Job>> m_jobs;
    QVector<Job*> m_queue;
    QSet<QString> m_writing; // project files a call is changing
    QVector<Worker*> m_workers;
    int m_startFailures = 0;
    quint64 m_next = 1;
    QString m_tag; // random part of the ids (a restarted server does not hand out old ids again)
    std::function<QJsonObject(const Job&, const QJsonObject&)> m_filter;
};

} // namespace Cli::Serve
