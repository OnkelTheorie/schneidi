// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Live connection between schneidi-cli (and its MCP server) and the running app: if the project is open in the
// app, commands work on what the app shows instead of the file. Reading commands get a copy of the open project
// (unsaved changes included), changing commands (edit, import, app) run inside the app, one undo step each.
//
// Transport: QLocalSocket (Unix socket / Windows named pipe), one JSON object per line. Every app writes a small
// file into registryDir() with its socket name and a random token; only the user can read it, the token proves
// the client is that user. Messages from the client:
//   {"type":"hello","token":…}            -> {"ok":true,"project":path,"busy":bool,"allowed":bool,"pid":…,
//                                              "waiting":text (only while the app waits, see Server::setWaiting)}
//   {"type":"attach","client":name}       -> {"ok":true} (one attached client at a time; shown in the app)
// An attached client that sends nothing for Server::idleLimit() (and has no work running) is let go: the app no
// longer shows it and another AI may attach. Its socket stays open; its next snapshot/run/activity takes the app
// again if it is free (no new attach needed), else APP_BUSY. So an MCP session that is done (its chat still open,
// the process alive) never blocks the app for the next AI.
//   {"type":"snapshot"}                    -> {"ok":true,"data":project file JSON,"app":state}
//   {"type":"run","command":…,"args":{…}} -> {"ok":true,"result":{…}} or {"ok":false,"error":{code,message}}
//   {"type":"activity","id":n,"state":"start"|"progress"|"end"|"failed","tool":…,"target":…,"percent":…,
//    "summary":{…},"error":…}         -> no answer (work outside the app, see Activity; attached client only)
//   {"type":"open","files":[…]}           -> {"ok":true,"queued":bool} (`schneidi <file>` while the app runs: the
//                                              user's hand-over, see handOver(); no attach needed, also when AIs are
//                                              refused; opened right after the answer, or once the app stops waiting)
// hello and open are also answered while a command runs (others wait for its end): a hand-over meanwhile is queued
// attach, snapshot and run while the app waits (Server::setWaiting, or a modal dialog is open) -> {"ok":false,"error":{"code":"APP_WAITING","message":text}}
// With only a modal dialog open hello has "reads":true: attach, snapshot and `app state` are still answered, other
// runs (changes) get APP_WAITING
// Plans (`edit` plan:true, plan_id + run/discard) run in the app too: making or dropping one is no undo step, the host
// hears of them (LiveHost::livePlan); in the chat's plan mode (LiveHost::livePlanOnly) the chat's other edits are
// refused with PLAN_MODE - the user runs a plan through Server::runForChat. `app` undo/redo/save of the chat are
// refused in plan mode (PLAN_MODE) and in review mode (LiveHost::liveReviewOnly, REVIEW_MODE): they are the user's.
// hello of an app that refuses AIs has "allowed":false and "refusal": why (the user disconnected, or the setting)
//
// The app's own AI chat starts `schneidi-cli mcp` with the environment of Server::chatEnvironment(): that process
// talks to exactly this app and says hello with the chat's secret instead of the token. The chat is always allowed
// (also when the user turned AI connections off: it is the app itself) and never busy (it does not take the place of
// an attached AI from outside); everything else works as for any client.

#include "cli/Commands.h"

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class LiveHost;
struct ProjectData;
class QLocalServer;
class QLockFile;
class QTimer;

namespace Cli::Live {

// "@app" as project = the project open in the (only) running app
inline constexpr const char* AppProject = "@app";
// Environment of the app's own chat process (see above): the app's socket and the chat's secret
inline constexpr const char* ChatServerEnv = "SCHNEIDI_LIVE_SERVER";
inline constexpr const char* ChatSecretEnv = "SCHNEIDI_LIVE_SECRET";
bool isAppProject(const QString& project);

QString registryDir();

// The app the current command runs in (set by the server while it runs a command), nullptr in schneidi-cli. Per thread:
// only the thread that runs the command sees it (a background job's thread never does, see AppCopy)
LiveHost* host();
// State the AI sees: playhead, selection, open timeline, playing, marks
QJsonObject appState(LiveHost* host);

// ---------- App side ----------

class Server : public QObject {
    Q_OBJECT
public:
    explicit Server(LiveHost* host, QObject* parent = nullptr);
    ~Server() override;
    bool start(QString* error = nullptr);

    bool attached() const { return m_attached; }
    QString client() const { return m_client; }
    // The user disconnects the AI ("Trennen") or turned AI connections off in the settings: the session ends and
    // new ones are refused until allowed again; `reason` tells the AI why (empty = "the user disconnected the AI")
    void disconnectClient(const QString& reason = {});
    bool allowed() const { return m_allowed; }
    void setAllowed(bool on);
    // The app cannot take commands yet (its restore dialog at start waits for the user, a project is being opened):
    // `message` tells the AI why (empty = ready). The server runs from the start, so AIs hear "waiting" instead of
    // "the app is not running"; nothing runs meanwhile (a modal dialog's event loop still delivers requests).
    // A modal dialog of the app (a QWindow with modality) counts as waiting by itself (waitingNow).
    void setWaiting(const QString& message);
    QString waiting() const { return m_waiting; }
    // What AIs hear now: the setWaiting message, or that a modal dialog is open; empty = ready
    // (readsAllowed: only a modal dialog is open - reads still go through, changes wait)
    QString waitingNow(bool* readsAllowed = nullptr) const;
    // For the app's own AI chat process (variable -> value): it connects to this app with the chat's secret
    QHash<QString, QString> chatEnvironment() const;
    // How long an attached AI may stay silent before it is let go (see above; default 2 min, 0 = never: it keeps
    // the app until it disconnects; Settings › AI)
    int idleLimit() const { return m_idleMs; }
    void setIdleLimit(int ms);
    // A command the app itself runs for the user on behalf of its own chat (a plan's steps from the chat's plan card:
    // edit plan_id + run/discard), as the chat's commands run: one undo step, lit up, in the AI log. The reply as for
    // a "run" request ({"ok":true,"result"} or {"ok":false,"error"}); APP_BUSY while another command runs.
    QJsonObject runForChat(const QString& command, const QJsonObject& args);

signals:
    void stateChanged(); // attached/allowed changed

private:
    struct Peer {
        QByteArray buffer;
        bool authed = false;
        bool chat = false;         // the app's own AI chat (said hello with the chat's secret)
        bool chatAttached = false; // ... and attached
        QHash<int, int> jobs;      // running work outside the app: the client's activity id -> LiveActivity::id
        QString client;            // attached once under this name: may take the app again after it was let go
    };
    void onConnection();
    void onReadyRead(QLocalSocket* s);
    void handle(QLocalSocket* s, const QJsonObject& msg);
    QJsonObject runCommand(const QJsonObject& msg, bool ownChat, bool byUser = false);
    void attach(QLocalSocket* s, const QString& client);
    void detach(QLocalSocket* s);
    void letGoIfIdle();
    void restartIdle(); // the idle limit counts again from now
    void openPending(); // files handed over (type "open") into the app, when it can take them

    LiveHost* m_host;
    QLocalServer* m_server = nullptr;
    QString m_token;
    QString m_chatSecret; // the app's own AI chat
    QString m_file; // registry entry
    QHash<QLocalSocket*, Peer> m_peers;
    QPointer<QLocalSocket> m_attached;
    QString m_client;
    bool m_allowed = true;
    QString m_refusal; // why new sessions are refused (empty = the user disconnected the AI)
    QString m_waiting;
    bool m_running = false; // a command runs (it may spin the event loop: no second one meanwhile)
    QTimer* m_idle = nullptr; // runs while an AI is attached, restarted by everything it sends
    int m_idleMs = 2 * 60 * 1000;
    int m_lastJob = 0;      // LiveActivity::id: unique in the app (every client process counts from 1)
    QStringList m_pendingOpen; // handed-over files not opened yet
    bool m_opening = false;    // the host opens handed-over files (may show "save changes?")
};

// Hands files to a schneidi app that already runs (`schneidi <file>`, `schneidi --open <file>`, a recorder that passes
// its clips on): the app that has the project file open (else the only/first one that answers) opens them like
// LiveHost::liveOpen and comes to the front. An app that does not answer at once (still starting; a running command
// does not keep it from answering) gets up to `patienceMs` before this gives up on it and opens a window of its own. False = no app took them (none runs, none answered, an older
// version): the caller starts the app itself.
bool handOver(const QStringList& files, QString* app = nullptr, int patienceMs = 10000);

// Launches with files decide "hand over or become the app" one at a time: lock, handOver(), and when that fails keep
// the lock until this process's Server::start() registered it (then release it). Two launches at the same moment
// end in one app, the second one hands its files to the first. A lock file in registryDir(); a crashed holder leaves a
// stale lock that QLockFile detects. nullptr = not taken within timeoutMs (go on without it, as before).
std::unique_ptr<QLockFile> lockLaunch(int timeoutMs = 60000);

// A command the app runs for the user from one of its own menu entries that maps onto an edit op (Clip › "Musik auf
// Länge…" = op fit_music) on the host's project (also without a running Server): one undo step named `undoText`, all
// or nothing, but the user's own: not in the AI log, not lit up as the AI's, plan mode does not apply. The reply as
// for Server::runForChat ({"ok":true,"result"} or {"ok":false,"error":{code,message}}); APP_BUSY while a command runs.
// `cancel` turning true stops the analyses inside (the op fails with CANCELED, the step is taken back).
QJsonObject runForUser(LiveHost* host, const QString& command, const QJsonObject& args, const QString& undoText,
                       const std::atomic<bool>* cancel = nullptr);

// ---------- Background jobs (app side, also any process with an event loop) ----------

// Reading commands that take a while (analyses, transcribe, frames) on a thread of their own, so the app stays usable
// and the user can cancel them. Only these commands, and only their reading forms: probe, peaks, scenes, highlights,
// silence, loudness, beats (without apply), overview, index (without annotate), transcribe (without apply), frames.
// Everything else (and a command that would change something) -> onDone with NOT_IN_BACKGROUND.
//
// - Call on the UI thread (the thread of QCoreApplication). onProgress and onDone are called there too (queued),
//   never from inside runInBackground. onDone comes exactly once per call (also for a refusal, a canceled or queued
//   job), with the reply of runForUser: {"ok":true,"result":{…}} or {"ok":false,"error":{"code","message"}}
//   (code CANCELED whenever cancel() came before onDone; its error has "late":true when the command had got to its end
//   anyway, so what it caches may be there). onProgress(fraction 0..1, stage) may be dropped/merged when the job is fast.
// - "@app" as project/file/source (or the path of the project open in `host`): the project and the app state are copied
//   here when the job is queued; the job reads that copy (openSession(@app, Read) on its thread), never the app's
//   objects. Without `host` "@app" fails with NO_APP.
// - A queue: at most one job per material file (or project) at a time, at most backgroundLimit() jobs at once (each
//   analysis uses several threads itself). The threads run at low priority (Background::lowerThisThread).
// - `receiver` (optional): when it is destroyed before the job ends, the callbacks are not called (a closed page).
// - Quitting: the jobs left are canceled and waited for at QCoreApplication::aboutToQuit (when the application goes at
//   the latest; or cancelAllBackground()).
class BackgroundJob;
class JobHandle {
public:
    JobHandle() = default;
    void cancel() const;  // stops it (queued: at once; running: within a moment); onDone still comes, with CANCELED
    bool running() const; // queued or running (onDone not called yet)
    int id() const;       // 0 = refused at once (onDone brings why)
    explicit operator bool() const { return m_job != nullptr; }

private:
    friend JobHandle runInBackground(const QString&, const QJsonObject&, std::function<void(double, const QString&)>,
                                     std::function<void(const QJsonObject&)>, LiveHost*, QObject*);
    std::shared_ptr<BackgroundJob> m_job;
};
JobHandle runInBackground(const QString& command, const QJsonObject& args,
                          std::function<void(double fraction, const QString& stage)> onProgress,
                          std::function<void(const QJsonObject& reply)> onDone, LiveHost* host = nullptr,
                          QObject* receiver = nullptr);
// May `command` with `args` run as a background job? (*why: for the refusal)
bool runsInBackground(const QString& command, const QJsonObject& args, QString* why = nullptr);
// Jobs at once (0 = automatic: Background::threads, half the processors, halved again, 1..4)
int backgroundLimit();
void setBackgroundLimit(int jobs);
// Queued + running jobs
int backgroundJobs();
// Cancels every job and waits until their threads are gone (their onDone calls still come, queued)
void cancelAllBackground();

// A background job's thread: the app's project as it was when the job was queued (data empty = the job reads no
// project of the app). openSession serves "@app" and the app's project path from it, read only.
struct AppCopy {
    QString path; // the app's project file (empty = never saved)
    std::shared_ptr<const ProjectData> data;
    QJsonObject app; // appState() at that time
};
const AppCopy* appCopy(); // the calling thread's, nullptr = not a background job
struct AppCopyScope {
    explicit AppCopyScope(const AppCopy* copy);
    ~AppCopyScope();
    AppCopyScope(const AppCopyScope&) = delete;
    AppCopyScope& operator=(const AppCopyScope&) = delete;

private:
    const AppCopy* m_old;
};

// ---------- schneidi-cli side ----------

// Connection to the app that has `project` open; nullptr = no app has it open (work on the file). Throws
// APP_BUSY (another AI session is connected), APP_DISCONNECTED (the user disconnected the AI), APP_WAITING (the
// app waits for the user, e.g. its restore dialog at start) and, for "@app", NO_APP / AMBIGUOUS. The connection
// stays open for the process (the MCP server keeps its session).
class Connection;
Connection* connectionFor(const QString& project);
// Project file open in the app (empty = never saved)
QString projectPath(Connection* c);
// The open project as a project file (JSON) plus the app state; for reading commands
struct Snapshot {
    QString path; // project file in the app (empty = never saved)
    QByteArray data;
    QJsonObject app;
    QStringList trustedCode; // sha1 (CodeTrust::hashOf of one) of the project's custom code the app trusts
};
// The Color page stills' pictures come only when asked for (`frames` still:): they are most of a project's bytes
Snapshot snapshot(Connection* c, bool stillPictures = false);
// Snapshots taken while one lives include the stills' pictures (a `frames` with still:)
struct StillPictures {
    StillPictures();
    ~StillPictures();
};
// Runs a command inside the app; throws its Error
QJsonObject run(Connection* c, const QString& command, const QJsonObject& args);
// Name shown in the app ("schneidi-cli" or "schneidi-cli mcp"); set by main()
void setClientName(const QString& name);
// Never connect to an app (server, work folder: cli/Sandbox.h): projects are always files
void disable();
// Closes this process's connections to apps (the app lets go at once, another AI may attach): a long-running
// `watch` follow that only now and then runs a command there must not keep the app
void release();

// Work an AI does outside the app while it is connected to it (analyses, check, render, transcribe … here in
// schneidi-cli): reported to the app so the user sees it working, with progress and a short result
// ({"type":"activity"}). Put around a command by Cli::execute. Reports only when this process already talks to an
// app (an MCP session that used "@app" or a project open there) or the command's own project is open in one; does
// nothing inside the app, for quick reading commands (info, help …) and when the app is gone (reporting never fails
// a command).
class Activity {
public:
    Activity(const QString& command, const QJsonObject& args, Context& ctx);
    ~Activity(); // neither finished() nor failed(): reported as failed (interrupted)
    Activity(const Activity&) = delete;
    Activity& operator=(const Activity&) = delete;
    void finished(const QJsonObject& result);
    void failed(const Error& error);
    bool reporting() const { return m_conn != nullptr; }

private:
    void send(QJsonObject msg);
    void progress(double p);

    Connection* m_conn = nullptr;
    Context& m_ctx;
    std::function<void(double)> m_progress; // the command's own progress (restored at the end)
    QString m_tool;
    int m_id = 0;
    int m_percent = -1;
    qint64 m_lastSent = 0; // ms, throttles progress messages
    bool m_done = false;
};
// One-line result of a job for the app (counts, not the details): {"count": n}, {"lufs": x} or {"file": path}
QJsonObject activitySummary(const QString& tool, const QJsonObject& result);

} // namespace Cli::Live
