// SPDX-License-Identifier: AGPL-3.0-only
// Live connection app <-> schneidi-cli, see Live.h
#include "cli/Live.h"

#include "cli/CommandsDetail.h"
#include "core/I18n.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX // std::max/std::clamp below
#endif
#include <windows.h>
#endif

namespace Cli::Live {

namespace {

LiveHost* g_host = nullptr;
bool g_isApp = false; // this process runs a server (or is told so by disable()): never connect to an app as a client
QString g_clientName = "schneidi-cli";

QByteArray line(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }

QJsonObject errorReply(const QString& code, const QString& message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

QString randomHex(int bytes)
{
    QByteArray b(bytes, Qt::Uninitialized);
    for (char& c : b) c = char(QRandomGenerator::system()->generate() & 0xff);
    return QString::fromLatin1(b.toHex());
}

// Same project file? (paths as the app and the command line give them; Windows ignores case)
QString canonical(const QString& path)
{
    if (path.isEmpty()) return {};
    const QFileInfo fi(path);
    const QString c = fi.exists() ? fi.canonicalFilePath() : fi.absoluteFilePath();
#ifdef Q_OS_WIN
    return c.toLower();
#else
    return c;
#endif
}

// Every clip (all timelines) as saved in the project file, and where it is: tells which clips a command added,
// changed or removed (and where a removed one was, for its "ghost" in the timeline)
struct ClipState {
    QByteArray json;
    LiveChange::Removed place;
};

QHash<int, ClipState> clipStates(const Project& p)
{
    QHash<int, ClipState> out;
    for (const Sequence& q : p.sequences())
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
            const QVector<Track>& tracks = q.timeline.tracks(k);
            for (int i = 0; i < tracks.size(); ++i)
                for (const Clip& c : tracks[i].clips)
                    out.insert(c.id, {QJsonDocument(ProjectFile::clipJson(c)).toJson(QJsonDocument::Compact),
                                      {c.id, q.id, {k, i}, c.start, c.end(), p.clipName(c)}});
        }
    return out;
}

// What a command did to the clips (sorted ids, so the log and the tests see a stable order)
void compareClips(const QHash<int, ClipState>& before, const QHash<int, ClipState>& after, LiveChange& change)
{
    for (auto it = after.begin(); it != after.end(); ++it) {
        const auto old = before.find(it.key());
        if (old == before.end()) change.added << it.key();
        else if (old->json != it->json) change.changed << it.key();
    }
    for (auto it = before.begin(); it != before.end(); ++it)
        if (!after.contains(it.key())) change.removed << it->place;
    std::sort(change.added.begin(), change.added.end());
    std::sort(change.changed.begin(), change.changed.end());
    std::sort(change.removed.begin(), change.removed.end(),
              [](const LiveChange::Removed& a, const LiveChange::Removed& b) { return a.id < b.id; });
}

// What a failed edit leaves behind outside the undo history (imported media, source In/Out, source markers, media
// lengths after a frame rate change): set back to how it was before the edit
void restoreMedia(Project& p, const QVector<MediaInfo>& before)
{
    // Files the edit imported (append/place/insert of a new file) leave the Media Pool again
    if (p.media().size() != before.size()) {
        QVector<MediaInfo> kept;
        for (const MediaInfo& m : p.media())
            if (std::any_of(before.cbegin(), before.cend(), [&](const MediaInfo& old) { return old.path == m.path; }))
                kept << m;
        p.replaceMedia(kept);
    }
    bool lengths = false;
    for (const MediaInfo& old : before) {
        const MediaInfo* now = p.mediaInfo(old.path);
        if (!now) continue;
        lengths = lengths || now->length != old.length;
        p.setMediaMarks(old.path, old.markIn, old.markOut);
        p.setMediaMarkers(old.path, old.markers);
    }
    if (!lengths) return;
    QVector<MediaInfo> media = p.media();
    for (MediaInfo& m : media)
        for (const MediaInfo& old : before)
            if (old.path == m.path) m.length = old.length;
    p.replaceMedia(media);
}

// Takes the step at `index` off the undo stack without running it: a failed command's undone macro would stay as a
// redo step (redo would bring the half state back), an empty one would be a step that does nothing. QUndoStack
// deletes an obsolete command instead of redoing/undoing it.
void dropStep(QUndoStack* undo, int index)
{
    if (index < 0 || index >= undo->count()) return;
    const_cast<QUndoCommand*>(undo->command(index))->setObsolete(true);
    if (undo->index() == index) undo->redo();
    else if (undo->index() == index + 1) undo->undo();
}

// The ops of an edit: "split, delete"
QString opNames(const QJsonObject& args)
{
    QJsonValue v = args.value("ops");
    if (v.isObject()) v = QJsonArray{v};
    QStringList names;
    for (const QJsonValue& op : v.toArray())
        if (const QString n = op.toObject().value("op").toString(); !n.isEmpty() && !names.contains(n)) names << n;
    if (names.size() > 4) names = names.mid(0, 4) << "…";
    return names.join(", ");
}

// Undo text: "KI: split, delete"
QString undoText(const QJsonObject& args) { return T("KI: %1").arg(opNames(args)); }

// The step of a command for the app's AI log: the ops of an edit, the action of `app` ("seek 00:00:04:00",
// "undo: KI: split") …
QString stepText(const QString& command, const QJsonObject& args, const QJsonObject& result, const Project& p)
{
    if (command == "edit") return opNames(args);
    if (command == "watch") { // set up a folder, or what one pass brought in
        if (args.contains("add")) return "watch: add " + QFileInfo(args.value("add").toString()).fileName();
        if (args.contains("remove")) return "watch: remove " + QFileInfo(args.value("remove").toString()).fileName();
        return QString("watch: %1 new").arg(result.value("imported").toInt());
    }
    if (command != "app") return command;
    QString step = args.value("action").toString().trimmed().toLower();
    if (const QString undone = result.value("undone").toString(result.value("redone").toString()); !undone.isEmpty())
        step += ": " + undone;
    else if (step == "open" && !args.value("files").toArray().isEmpty())
        step += " " + QFileInfo(args.value("files").toArray().first().toString()).fileName();
    else if (args.contains("at") && result.contains("app"))
        step += " " + detail::tc(result["app"].toObject().value("playhead").toInt(), p.format());
    return step;
}

struct HostScope {
    explicit HostScope(LiveHost* h) { g_host = h; }
    ~HostScope() { g_host = nullptr; }
};

} // namespace

bool isAppProject(const QString& project) { return project.trimmed() == AppProject; }

QString registryDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/live";
}

LiveHost* host() { return g_host; }

QJsonObject appState(LiveHost* h)
{
    Project* p = h->liveProject();
    const ProjectFormat& fmt = p->format();
    const int playhead = h->livePlayhead();
    QJsonArray selected;
    QList<int> ids = h->liveSelection()->ids().values();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) selected << id;
    QJsonObject o{{"project", h->liveProjectPath()},
                  {"modified", p->isModified()},
                  {"timeline", QJsonObject{{"id", p->currentSequence()}, {"name", p->sequenceName(p->currentSequence())}}},
                  {"playhead", playhead},
                  {"playhead_tc", detail::tc(playhead, fmt)},
                  {"playing", h->livePlaying()},
                  {"selection", selected}};
    if (p->undoStack()->canUndo()) o["undo"] = p->undoStack()->undoText(); // what `app` undo would take back
    const Timeline& tl = p->timeline();
    if (tl.markIn >= 0 || tl.markOut >= 0)
        o["marks"] = QJsonObject{{"in", tl.markIn}, {"out", tl.markOut}};
    return o;
}

// ---------- App side ----------

Server::Server(LiveHost* host, QObject* parent) : QObject(parent), m_host(host)
{
    g_isApp = true;
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    connect(m_idle, &QTimer::timeout, this, &Server::letGoIfIdle);
}

void Server::setIdleLimit(int ms)
{
    m_idleMs = std::max(0, ms);
    if (m_idleMs == 0) m_idle->stop();
    else if (m_attached) m_idle->start(m_idleMs);
}

void Server::restartIdle()
{
    if (m_idleMs > 0) m_idle->start(m_idleMs);
}

void Server::letGoIfIdle()
{
    if (!m_attached) return;
    // Work it reported is still running (a long render): not idle
    if (m_running || !m_peers.value(m_attached).jobs.isEmpty()) return restartIdle();
    detach(m_attached); // the socket stays open: its next request takes the app again if it is free
}

Server::~Server()
{
    if (!m_file.isEmpty()) QFile::remove(m_file);
}

bool Server::start(QString* error)
{
    const QString dir = registryDir();
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString name = QString("schneidi-live-%1-%2").arg(QCoreApplication::applicationPid()).arg(randomHex(4));
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(name)) {
        if (error) *error = m_server->errorString();
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &Server::onConnection);
    m_token = randomHex(16);
    m_chatSecret = randomHex(16);
    m_file = QString("%1/%2.json").arg(dir).arg(QCoreApplication::applicationPid());
    // Written whole or not at all: a client reading a half-written file would take the app for gone and delete it
    QSaveFile f(m_file);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    // Owner-only before the token goes in
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    f.write(QJsonDocument(QJsonObject{{"server", m_server->fullServerName()},
                                      {"token", m_token},
                                      {"pid", QCoreApplication::applicationPid()},
                                      {"version", QCoreApplication::applicationVersion()}})
                .toJson());
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

QHash<QString, QString> Server::chatEnvironment() const
{
    if (!m_server) return {};
    return {{ChatServerEnv, m_server->fullServerName()}, {ChatSecretEnv, m_chatSecret}};
}

void Server::disconnectClient(const QString& reason)
{
    m_allowed = false;
    m_refusal = reason;
    if (m_attached) m_attached->disconnectFromServer(); // detach() follows via disconnected()
    emit stateChanged();
}

void Server::setAllowed(bool on)
{
    if (m_allowed == on) return;
    m_allowed = on;
    if (on) m_refusal.clear();
    emit stateChanged();
}

void Server::setWaiting(const QString& message)
{
    m_waiting = message;
    if (m_waiting.isEmpty() && !m_pendingOpen.isEmpty()) QTimer::singleShot(0, this, &Server::openPending);
}

void Server::openPending()
{
    if (m_pendingOpen.isEmpty() || !m_waiting.isEmpty()) return; // setWaiting({}) comes back here
    // An AI command or an earlier hand-over (its question still open) runs: right after it
    if (m_running || m_opening) return void(QTimer::singleShot(200, this, &Server::openPending));
    const QStringList files = std::exchange(m_pendingOpen, {});
    m_opening = true;
    // The host may ask "save changes?" (a modal dialog with its own event loop, which delivers live requests): AIs
    // wait meanwhile, an edit must not land in the project the user is about to save or leave
    const QString before = std::exchange(m_waiting, "schneidi is waiting for the user (opening files handed to it, "
                                                     "maybe asking to save changes); try again in a moment");
    QString error;
    if (!m_host->liveOpen(files, true, &error) && !error.isEmpty())
        qWarning("Opening handed-over files failed: %s", qPrintable(error));
    m_waiting = before; // the app may have been waiting already
    m_opening = false;
    if (!m_pendingOpen.isEmpty()) QTimer::singleShot(0, this, &Server::openPending); // handed over meanwhile
}

void Server::onConnection()
{
    while (QLocalSocket* s = m_server->nextPendingConnection()) {
        m_peers.insert(s, {});
        connect(s, &QLocalSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QLocalSocket::disconnected, this, [this, s] {
            detach(s);
            // Its work outside the app ends with it (a stopped chat, a client that crashed)
            for (int job : m_peers.value(s).jobs) {
                LiveActivity a;
                a.id = job;
                a.state = LiveActivity::State::Failed;
                a.error = "interrupted";
                m_host->liveActivity(a);
            }
            m_peers.remove(s);
            s->deleteLater();
        });
    }
}

void Server::attach(QLocalSocket* s, const QString& client)
{
    m_attached = s;
    m_client = client;
    m_peers[s].client = client;
    restartIdle();
    m_host->liveConnectionChanged(true, m_client);
    emit stateChanged();
}

void Server::detach(QLocalSocket* s)
{
    if (m_attached != s) return;
    m_attached = nullptr;
    m_idle->stop();
    const QString client = m_client;
    m_client.clear();
    m_host->liveConnectionChanged(false, client);
    emit stateChanged();
}

void Server::onReadyRead(QLocalSocket* s)
{
    if (!m_peers.contains(s)) return;
    m_peers[s].buffer += s->readAll();
    QPointer<QLocalSocket> guard(s);
    while (guard && m_peers.contains(s)) {
        QByteArray& buf = m_peers[s].buffer;
        const int nl = buf.indexOf('\n');
        if (nl < 0) break;
        const QByteArray msg = buf.left(nl).trimmed();
        const QJsonDocument doc = QJsonDocument::fromJson(msg);
        // While a command runs (it may spin the event loop) only what leaves the project alone: a hello and the user's
        // hand-over (opened once the command is done), so a launch with files does not take the app for gone and
        // open a second window. Everything else is picked up when the command is done.
        if (m_running && !msg.isEmpty()) {
            const QString type = doc.object().value("type").toString();
            if (type != "hello" && type != "open") break;
        }
        buf.remove(0, nl + 1);
        if (msg.isEmpty()) continue;
        if (!doc.isObject()) {
            s->write(line(errorReply("BAD_REQUEST", "not a JSON object")));
            continue;
        }
        handle(s, doc.object());
    }
}

void Server::handle(QLocalSocket* s, const QJsonObject& msg)
{
    const auto reply = [s](const QJsonObject& o) { s->write(line(o)); };
    const QString type = msg.value("type").toString();
    Peer& peer = m_peers[s];
    if (!peer.authed) {
        const QString token = msg.value("token").toString();
        const bool chat = !m_chatSecret.isEmpty() && token == m_chatSecret;
        if (type != "hello" || (token != m_token && !chat)) {
            reply(errorReply("AUTH", "wrong token"));
            s->disconnectFromServer();
            return;
        }
        peer.authed = true;
        peer.chat = chat;
    }
    if (s == m_attached) restartIdle(); // anything it sends shows it is still at work
    if (type == "hello") {
        // The app's own chat is always allowed and never waits for another AI
        QJsonObject o{{"ok", true},
                      {"project", m_host->liveProjectPath()},
                      {"busy", !peer.chat && m_attached && m_attached != s},
                      {"allowed", peer.chat || m_allowed},
                      {"pid", QCoreApplication::applicationPid()},
                      {"version", QCoreApplication::applicationVersion()}};
        if (!m_waiting.isEmpty()) o["waiting"] = m_waiting;
        if (!peer.chat && !m_allowed && !m_refusal.isEmpty()) o["refusal"] = m_refusal;
        return reply(o);
    }
    // The user's own hand-over (`schneidi <file>` while the app runs): no AI session, so neither attach nor the AI
    // switch applies. Opened after the answer (a "save changes?" question must not keep the other process waiting).
    if (type == "open") {
        QStringList files;
        for (const QJsonValue& v : msg.value("files").toArray())
            if (const QString f = v.toString(); !f.isEmpty()) files << QFileInfo(f).absoluteFilePath();
        if (files.isEmpty()) return reply(errorReply("BAD_REQUEST", "no files to open"));
        m_pendingOpen += files;
        QTimer::singleShot(0, this, &Server::openPending);
        return reply({{"ok", true}, {"queued", !m_waiting.isEmpty()}, {"pid", QCoreApplication::applicationPid()}});
    }
    // Let go while it was idle: its next request takes the app again if it is free
    const bool wantsApp = type == "snapshot" || type == "run"
                          || (type == "activity" && msg.value("state").toString() == "start");
    if (wantsApp && !peer.chat && m_attached != s && !peer.client.isEmpty() && m_waiting.isEmpty()) {
        if (!m_attached && m_allowed) attach(s, peer.client);
        else if (type != "activity")
            return reply(m_allowed ? errorReply("APP_BUSY", "another AI session connected to the schneidi app while "
                                                            "this one was idle")
                                   : errorReply("APP_DISCONNECTED", m_refusal.isEmpty()
                                                                        ? "the user disconnected the AI in the schneidi app"
                                                                        : m_refusal));
    }
    const bool attached = m_attached == s || peer.chatAttached;
    // Work of the attached AI outside the app: shown there, never answered (the client does not wait for it)
    if (type == "activity") {
        if (!attached || !m_waiting.isEmpty()) return;
        static const QHash<QString, LiveActivity::State> states{{"start", LiveActivity::State::Started},
                                                                 {"progress", LiveActivity::State::Progress},
                                                                 {"end", LiveActivity::State::Finished},
                                                                 {"failed", LiveActivity::State::Failed}};
        const QString state = msg.value("state").toString();
        if (!states.contains(state)) return;
        LiveActivity a;
        a.state = states.value(state);
        // Ids of the client's own count: another process (the chat beside an AI from outside) has the same ones
        const int clientId = msg.value("id").toInt();
        if (a.state == LiveActivity::State::Started) peer.jobs.insert(clientId, ++m_lastJob);
        if (!peer.jobs.contains(clientId)) return; // its start was not seen
        a.id = peer.jobs.value(clientId);
        if (a.state == LiveActivity::State::Finished || a.state == LiveActivity::State::Failed) peer.jobs.remove(clientId);
        a.tool = msg.value("tool").toString();
        a.target = msg.value("target").toString();
        a.percent = msg.value("percent").toInt(-1);
        a.summary = msg.value("summary").toObject();
        a.error = msg.value("error").toString();
        m_host->liveActivity(a);
        return;
    }
    // The app waits for the user (restore dialog at start): its project is not the one the user will work on yet
    if (!m_waiting.isEmpty() && (type == "attach" || type == "snapshot" || type == "run"))
        return reply(errorReply("APP_WAITING", m_waiting));
    if (type == "attach") {
        if (peer.chat) { // not shown as a connected AI: the chat panel shows what it does
            peer.chatAttached = true;
            return reply({{"ok", true}});
        }
        if (!m_allowed)
            return reply(errorReply("APP_DISCONNECTED", m_refusal.isEmpty() ? "the user disconnected the AI in the schneidi app"
                                                                             : m_refusal));
        if (m_attached && m_attached != s)
            return reply(errorReply("APP_BUSY", "another AI session is connected to the schneidi app"));
        if (m_attached != s) attach(s, msg.value("client").toString("schneidi-cli"));
        return reply({{"ok", true}});
    }
    if (!attached) return reply(errorReply("NOT_ATTACHED", "attach first"));
    if (type == "snapshot") {
        Project* p = m_host->liveProject();
        const QString path = m_host->liveProjectPath();
        return reply({{"ok", true},
                      {"project", path},
                      {"data", QString::fromUtf8(ProjectFile::toJson(p->data(), path))},
                      {"app", appState(m_host)}});
    }
    if (type == "run") {
        QPointer<QLocalSocket> guard(s);
        m_running = true;
        const bool ownChat = peer.chat;
        m_host->liveCommandRunning(true, ownChat);
        const QJsonObject result = runCommand(msg, ownChat);
        m_host->liveCommandRunning(false, ownChat);
        m_running = false;
        if (guard && guard == m_attached) restartIdle(); // a long command counts from its end
        if (guard) guard->write(line(result));
        // Requests that came in meanwhile
        QTimer::singleShot(0, this, [this] {
            for (QLocalSocket* p : m_peers.keys()) onReadyRead(p);
        });
        return;
    }
    reply(errorReply("BAD_REQUEST", "unknown request type " + type));
}

QJsonObject Server::runCommand(const QJsonObject& msg, bool ownChat)
{
    const QString name = msg.value("command").toString();
    const Command* cmd = find(name);
    const QJsonObject args = msg.value("args").toObject(); // normalized by the client
    if (!cmd || !runsInApp(*cmd, args)) return errorReply("BAD_REQUEST", "command does not run in the app: " + name);
    HostScope scope(m_host);
    Project* p = m_host->liveProject();
    const QHash<int, ClipState> before = clipStates(*p);
    const QVector<MediaInfo> mediaBefore = p->media();
    const QVector<WatchFolder> watchBefore = p->watchFolders();
    // One undo step per changing command, all or nothing like on the file; it never merges with the user's slider
    // series before or after it (`restore` is one step of its own and changes nothing before it can fail)
    QUndoStack* undo = p->undoStack();
    const bool step = name == "edit" || name == "import" || name == "watch";
    const int index = undo->index();
    if (step) {
        p->closeMerge();
        undo->beginMacro(name == "edit" ? undoText(args) : T("KI: %1").arg(name));
    }
    QJsonObject result;
    Context ctx;
    // Every step goes into the app's AI log (reading `app state` is not one)
    const bool logged = !(name == "app" && args.value("action").toString().trimmed().toLower() == "state");
    LiveChange change;
    change.command = name;
    change.ownChat = ownChat;
    try {
        result = cmd->run(args, ctx);
    } catch (const Error& e) {
        if (step) {
            undo->endMacro();
            if (undo->index() > index) undo->undo(); // all or nothing, like on the file
            dropStep(undo, index);                   // and no redo step that brings it back
            restoreMedia(*p, mediaBefore);
            if (p->watchFolders() != watchBefore) p->setWatchFolders(watchBefore);
        }
        if (logged) {
            change.step = stepText(name, args, {}, *p);
            change.error = e.message;
            m_host->liveChanged(change);
        }
        return errorReply(e.code, e.message);
    }
    if (step) {
        undo->endMacro();
        // An import of media only changes nothing in the undo history: no empty step
        if (name != "edit" && undo->index() == index + 1 && undo->command(index)->childCount() == 0)
            dropStep(undo, index);
        p->closeMerge();
    }
    // `app open` of a project: another project now, its clips are no AI changes
    if (!result.contains("opened")) compareClips(before, clipStates(*p), change);
    change.step = stepText(name, args, result, *p);
    // A `watch` pass that found nothing new is no step (a follow runs one whenever a watched folder changes)
    const bool quiet = name == "watch" && p->media().size() == mediaBefore.size() && !args.contains("add")
                       && !args.contains("remove");
    if ((logged && !quiet) || !change.added.isEmpty() || !change.changed.isEmpty() || !change.removed.isEmpty())
        m_host->liveChanged(change);
    return {{"ok", true}, {"result", result}};
}

// ---------- schneidi-cli side ----------

class Connection {
public:
    QLocalSocket socket;
    QString project; // as the app reported it

    // Answers carry no request id: a request that timed out ends the connection, else its late answer would be read
    // as the answer to the next request (connectionFor() opens a new one)
    QJsonObject request(const QJsonObject& msg, int timeoutMs)
    {
        socket.write(line(msg));
        if (!socket.waitForBytesWritten(5000) && socket.bytesToWrite() > 0) lost();
        while (true) {
            if (const int nl = m_buffer.indexOf('\n'); nl >= 0) {
                const QByteArray reply = m_buffer.left(nl);
                m_buffer.remove(0, nl + 1);
                return QJsonDocument::fromJson(reply).object();
            }
            if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(timeoutMs)) lost();
            m_buffer += socket.readAll();
        }
    }

private:
    QByteArray m_buffer;
    [[noreturn]] void lost()
    {
        const bool timedOut = socket.state() == QLocalSocket::ConnectedState;
        socket.abort();
        m_buffer.clear();
        if (timedOut)
            detail::fail("APP_BUSY", "the schneidi app did not answer in time (it is busy), try again");
        detail::fail("APP_DISCONNECTED", "the connection to the schneidi app was closed (app quit or the user "
                                         "disconnected the AI)");
    }
};

namespace {

std::map<QString, std::unique_ptr<Connection>>& connections()
{
    static std::map<QString, std::unique_ptr<Connection>> map;
    return map;
}

struct Candidate {
    std::unique_ptr<Connection> conn;
    bool busy = false;
    bool allowed = true;
    QString refusal; // not allowed: why (empty = the user disconnected the AI)
    QString waiting; // the app waits for the user (why, for the AI)
    bool silent = false; // did not answer hello in time (busy): its project is unknown
    qint64 pid = 0;
    QString server, token; // from its registry entry: a second try (handOver waits longer for a silent app)
};

std::vector<Candidate> runningApps()
{
    std::vector<Candidate> out;
    // Started by the app's own AI chat: only that app, with the chat's secret (Live.h)
    QVector<QJsonObject> entries;
    QStringList files;
    if (const QString own = qEnvironmentVariable(ChatServerEnv); !own.isEmpty()) {
        entries << QJsonObject{{"server", own}, {"token", qEnvironmentVariable(ChatSecretEnv)}};
        files << QString();
    } else {
        for (const QFileInfo& fi : QDir(registryDir()).entryInfoList({"*.json"}, QDir::Files)) {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QJsonObject entry = QJsonDocument::fromJson(f.readAll()).object();
            if (entry.value("server").toString().isEmpty()) continue; // unreadable: never delete what we did not parse
            entries << entry;
            files << fi.absoluteFilePath();
        }
    }
    for (int i = 0; i < entries.size(); ++i) {
        const QJsonObject& entry = entries[i];
        auto c = std::make_unique<Connection>();
        const QString server = entry.value("server").toString();
        const QString token = entry.value("token").toString();
        const qint64 pid = qint64(entry.value("pid").toDouble());
        c->socket.connectToServer(server);
        if (!c->socket.waitForConnected(1000)) {
            // App gone without cleaning up (crash): forget it and its socket file
            if (!files[i].isEmpty()
                && (c->socket.error() == QLocalSocket::ServerNotFoundError
                    || c->socket.error() == QLocalSocket::ConnectionRefusedError)) {
                QFile::remove(files[i]);
                QLocalServer::removeServer(entry.value("server").toString());
            }
            continue;
        }
        QJsonObject hello;
        try {
            hello = c->request({{"type", "hello"}, {"token", token}}, 3000);
        } catch (const Error& e) {
            // Busy (a long command in its event loop): it may have the project open, so it is no "not running"
            if (e.code == "APP_BUSY") {
                Candidate busy;
                busy.conn = std::move(c);
                busy.silent = true;
                busy.pid = pid;
                busy.server = server;
                busy.token = token;
                out.push_back(std::move(busy));
            }
            continue;
        }
        if (!hello.value("ok").toBool()) continue;
        c->project = hello.value("project").toString();
        out.push_back({std::move(c), hello.value("busy").toBool(), hello.value("allowed").toBool(true),
                       hello.value("refusal").toString(), hello.value("waiting").toString(), false,
                       qint64(hello.value("pid").toDouble(double(pid))), server, token});
    }
    return out;
}

} // namespace

void setClientName(const QString& name) { g_clientName = name; }

void disable() { g_isApp = true; }

void release()
{
    for (auto& [key, conn] : connections()) {
        conn->socket.disconnectFromServer();
        if (conn->socket.state() != QLocalSocket::UnconnectedState) conn->socket.waitForDisconnected(1000);
    }
    connections().clear();
}

Connection* connectionFor(const QString& project)
{
    if (g_isApp || project.isEmpty()) return nullptr;
    const bool any = isAppProject(project);
    const QString key = any ? QString(AppProject) : canonical(project);
    auto& map = connections();
    if (auto it = map.find(key); it != map.end()) {
        if (it->second->socket.state() == QLocalSocket::ConnectedState) return it->second.get();
        map.erase(it);
    }
    // The file of an app this process is already attached to (e.g. as "@app"): same connection, a second one
    // would be refused as busy
    if (!any)
        for (auto& [k, conn] : map)
            if (conn->socket.state() == QLocalSocket::ConnectedState) {
                const QString open = projectPath(conn.get());
                if (!open.isEmpty() && canonical(open) == key) return conn.get();
            }
    std::vector<Candidate> apps = runningApps();
    const QString silent = "a schneidi app is running but did not answer in time (it is busy and may have this "
                           "project open), try again";
    Candidate* pick = nullptr;
    if (any) {
        if (apps.empty()) detail::fail("NO_APP", "the schneidi app is not running (\"@app\" needs it)");
        if (apps.size() > 1) {
            QStringList projects;
            for (const Candidate& c : apps)
                projects << (c.silent ? "(not answering)" : c.conn->project.isEmpty() ? "(unsaved)" : c.conn->project);
            detail::fail("AMBIGUOUS", "several schneidi apps are running, give the project file instead of \"@app\": "
                                          + projects.join(", "));
        }
        pick = &apps[0];
    } else {
        for (Candidate& c : apps)
            if (!c.silent && !c.conn->project.isEmpty() && canonical(c.conn->project) == key) pick = &c;
        // Never a silent write to the file: an app that did not answer may have it open
        if (!pick)
            for (const Candidate& c : apps)
                if (c.silent) detail::fail("APP_BUSY", silent);
        if (!pick) return nullptr;
    }
    if (pick->silent) detail::fail("APP_BUSY", silent);
    if (!pick->waiting.isEmpty()) detail::fail("APP_WAITING", pick->waiting);
    if (!pick->allowed)
        detail::fail("APP_DISCONNECTED", !pick->refusal.isEmpty()
                                             ? pick->refusal
                                             : "the project is open in the schneidi app and the user disconnected the "
                                               "AI there (the app can allow it again)");
    if (pick->busy)
        detail::fail("APP_BUSY", "the project is open in the schneidi app and another AI session is connected to it");
    const QJsonObject r = pick->conn->request({{"type", "attach"}, {"client", g_clientName}}, 3000);
    if (!r.value("ok").toBool())
        detail::fail(r["error"]["code"].toString("APP_BUSY"), r["error"]["message"].toString());
    Connection* c = pick->conn.get();
    map[key] = std::move(pick->conn);
    return c;
}

std::unique_ptr<QLockFile> lockLaunch(int timeoutMs)
{
    const QString dir = registryDir();
    QDir().mkpath(dir);
    auto l = std::make_unique<QLockFile>(dir + "/launch.lock");
    // Stale only when its process is gone, never by age: the holder keeps it while its window is built (slow at a
    // first start on Windows, the virus scanner looks at every DLL), longer than Qt's default 30 s
    l->setStaleLockTime(0);
    if (!l->tryLock(timeoutMs)) return nullptr;
    return l;
}

bool handOver(const QStringList& files, QString* appProject, int patienceMs)
{
    if (files.isEmpty()) return false;
    QJsonArray list;
    QStringList projects;
    for (const QString& f : files) {
        list << QFileInfo(f).absoluteFilePath();
        if (QFileInfo(f).suffix() == QLatin1String(ProjectFile::Extension)) projects << canonical(f);
    }
    std::vector<Candidate> apps = runningApps();
    Candidate* pick = nullptr;
    for (Candidate& c : apps) // the app that has the project open already
        if (!c.silent && !c.conn->project.isEmpty() && projects.contains(canonical(c.conn->project))) pick = &c;
    for (Candidate& c : apps)
        if (!pick && !c.silent) pick = &c;
    const auto open = [&](Candidate& c) {
#ifdef Q_OS_WIN
        // Windows lets only the foreground process give the foreground away: the app may raise its window
        if (c.pid > 0) AllowSetForegroundWindow(DWORD(c.pid));
#endif
        try {
            const QJsonObject r = c.conn->request({{"type", "open"}, {"files", list}}, 5000);
            if (!r.value("ok").toBool()) return false; // an older app without "open": start one of our own
        } catch (const Error&) {
            return false;
        }
        if (appProject) *appProject = c.conn->project;
        c.conn->socket.disconnectFromServer();
        return true;
    };
    if (pick) return open(*pick);
    // Only apps that did not answer in time (still starting, a command that keeps its event loop busy): one of them
    // rather takes the files late than a second window opens. Asked again on a new connection (the first one ended
    // with its timeout), with more patience.
    for (Candidate& c : apps) {
        if (!c.silent || c.server.isEmpty()) continue;
        c.conn = std::make_unique<Connection>();
        c.conn->socket.connectToServer(c.server);
        if (!c.conn->socket.waitForConnected(1000)) continue;
        try {
            const QJsonObject hello = c.conn->request({{"type", "hello"}, {"token", c.token}}, std::max(patienceMs, 0));
            if (!hello.value("ok").toBool()) continue;
            c.conn->project = hello.value("project").toString();
        } catch (const Error&) {
            continue;
        }
        if (open(c)) return true;
    }
    return false;
}

QString projectPath(Connection* c)
{
    // hello again: the app may have saved the project under a new name since
    const QJsonObject r = c->request({{"type", "hello"}}, 3000);
    if (r.value("ok").toBool()) c->project = r.value("project").toString();
    return c->project;
}

Snapshot snapshot(Connection* c)
{
    const QJsonObject r = c->request({{"type", "snapshot"}}, 60000);
    if (!r.value("ok").toBool()) detail::fail(r["error"]["code"].toString(), r["error"]["message"].toString());
    return {r.value("project").toString(), r.value("data").toString().toUtf8(), r.value("app").toObject()};
}

QJsonObject run(Connection* c, const QString& command, const QJsonObject& args)
{
    // Long ops (normalize measures the audio) may take a while
    const QJsonObject r = c->request({{"type", "run"}, {"command", command}, {"args", args}}, 30 * 60 * 1000);
    if (!r.value("ok").toBool()) detail::fail(r["error"]["code"].toString(), r["error"]["message"].toString());
    return r.value("result").toObject();
}

// ---------- Work outside the app (schneidi-cli side) ----------

namespace {

// Commands worth showing while they run: they look at material or write files and may take a while
bool isJob(const QString& command)
{
    static const QStringList jobs{"probe",  "silence", "scenes", "peaks",           "highlights", "frames",
                                  "transcribe", "loudness", "scopes", "sync", "render", "export_timeline",
                                  "extensions", "check", "overview", "beats"};
    return jobs.contains(command);
}

// What a job works on: a file or project ("@app" stays as it is, the app names its project), several files as the
// first one plus their number
QString targetOf(const QJsonObject& args)
{
    for (const char* key : {"file", "source"})
        if (const QString v = args.value(key).toString(); !v.isEmpty()) return v;
    for (const char* key : {"files", "sources"}) {
        const QJsonArray list = args.value(key).toArray();
        if (list.isEmpty()) continue;
        const QJsonValue first = list.first();
        const QString name = first.isObject() ? first["file"].toString() : first.toString();
        return list.size() > 1 ? QString("%1 +%2").arg(name).arg(list.size() - 1) : name;
    }
    if (const QString v = args.value("project").toString(); !v.isEmpty()) return v;
    return args.value("install").toString(); // extensions
}

// The app to report to: the one that has the command's project open (attaching like the command itself would), else
// the one this process already talks to (an MCP session that used "@app"). nullptr = nobody to tell.
Connection* reportingConnection(const QJsonObject& args)
{
    for (const char* key : {"project", "file", "source"}) {
        const QString v = args.value(key).toString();
        if (v.isEmpty() || !detail::isProjectFile(v)) continue;
        try {
            if (Connection* c = connectionFor(v)) return c;
        } catch (const Error&) {
            // The command runs into the same error itself and reports it
        }
        break;
    }
    for (auto& [key, conn] : connections())
        if (conn->socket.state() == QLocalSocket::ConnectedState) return conn.get();
    return nullptr;
}

// Things a job found: the first list of `keys` in the result, summed over the files of a multi-file result
int countOf(const QJsonObject& r, const QStringList& keys)
{
    for (const char* list : {"files", "sources"})
        if (r.value(list).isArray() && !keys.contains(list)) {
            int n = 0;
            for (const QJsonValue& v : r.value(list).toArray()) n += countOf(v.toObject(), keys);
            return n;
        }
    for (const QString& k : keys)
        if (r.value(k).isArray()) return int(r.value(k).toArray().size());
    return 0;
}

} // namespace

QJsonObject activitySummary(const QString& tool, const QJsonObject& r)
{
    static const QHash<QString, QStringList> counted{
        {"silence", {"silence"}},       {"scenes", {"cuts", "changes"}}, {"peaks", {"peaks"}},
        {"highlights", {"best", "candidates"}}, {"frames", {"frames"}},  {"transcribe", {"segments"}},
        {"check", {"issues"}},           {"overview", {"windows", "files"}},
    };
    if (counted.contains(tool)) return {{"count", countOf(r, counted.value(tool))}};
    if (tool == "sync") {
        int found = 0;
        for (const QJsonValue& f : r.value("files").toArray()) found += f.toObject().contains("offset_s") ? 1 : 0;
        return {{"count", found}};
    }
    if (tool == "loudness") {
        const QJsonValue lufs = r.value("loudness").toObject().value("integrated_lufs");
        if (lufs.isDouble()) return {{"lufs", lufs}};
        if (r.value("integrated_lufs").isDouble()) return {{"lufs", r.value("integrated_lufs")}};
        return {{"count", countOf(r, {"clips"})}};
    }
    if (tool == "beats" && r.value("bpm").isDouble()) return {{"bpm", r.value("bpm")}};
    if (tool == "render" || tool == "export_timeline")
        if (const QString out = r.value("out").toString(); !out.isEmpty()) return {{"file", out}};
    return {};
}

Activity::Activity(const QString& command, const QJsonObject& args, Context& ctx) : m_ctx(ctx), m_tool(command)
{
    if (g_isApp || g_host || !isJob(command)) return;
    m_conn = reportingConnection(args);
    if (!m_conn) return;
    static int jobs = 0;
    m_id = ++jobs;
    send({{"state", "start"}, {"target", targetOf(args)}});
    m_progress = ctx.progress;
    ctx.progress = [this](double p) { progress(p); };
}

Activity::~Activity()
{
    if (!m_conn) return;
    m_ctx.progress = m_progress;
    if (!m_done) send({{"state", "failed"}, {"error", "interrupted"}});
}

void Activity::finished(const QJsonObject& result)
{
    if (!m_conn || m_done) return;
    m_done = true;
    send({{"state", "end"}, {"summary", activitySummary(m_tool, result)}});
}

void Activity::failed(const Error& error)
{
    if (!m_conn || m_done) return;
    m_done = true;
    send({{"state", "failed"}, {"error", error.message}});
}

void Activity::progress(double p)
{
    if (m_progress) m_progress(p);
    // Only from the thread that owns the connection (some analyses report from their worker threads)
    if (!m_conn || QThread::currentThread() != m_conn->socket.thread()) return;
    const int percent = std::clamp(int(p * 100), 0, 100);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (percent == m_percent || (now - m_lastSent < 250 && percent < 100)) return;
    m_percent = percent;
    m_lastSent = now;
    send({{"state", "progress"}, {"percent", percent}});
}

void Activity::send(QJsonObject msg)
{
    QLocalSocket& s = m_conn->socket;
    if (s.state() != QLocalSocket::ConnectedState) return; // the app is gone: the command goes on
    msg["type"] = "activity";
    msg["id"] = m_id;
    msg["tool"] = m_tool;
    s.write(line(msg));
    s.flush();
    if (s.bytesToWrite() > 0) s.waitForBytesWritten(1000);
}

} // namespace Cli::Live
