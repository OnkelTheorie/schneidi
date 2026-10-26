// Live connection app <-> schneidi-cli, see Live.h
#include "cli/Live.h"

#include "cli/CommandsDetail.h"
#include "core/I18n.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

namespace Cli::Live {

namespace {

LiveHost* g_host = nullptr;
bool g_isApp = false; // this process runs a server: never connect to an app as a client
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

// Every clip (all timelines) as saved in the project file: tells which clips a command changed
QHash<int, QByteArray> clipStates(const Project& p)
{
    QHash<int, QByteArray> out;
    for (const Sequence& q : p.sequences())
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : q.timeline.tracks(k))
                for (const Clip& c : t.clips)
                    out.insert(c.id, QJsonDocument(ProjectFile::clipJson(c)).toJson(QJsonDocument::Compact));
    return out;
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

// Undo text: "KI: split, delete"
QString undoText(const QJsonObject& args)
{
    QJsonValue v = args.value("ops");
    if (v.isObject()) v = QJsonArray{v};
    QStringList names;
    for (const QJsonValue& op : v.toArray())
        if (const QString n = op.toObject().value("op").toString(); !n.isEmpty() && !names.contains(n)) names << n;
    if (names.size() > 4) names = names.mid(0, 4) << "…";
    return T("KI: %1").arg(names.join(", "));
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

Server::Server(LiveHost* host, QObject* parent) : QObject(parent), m_host(host) { g_isApp = true; }

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
    m_file = QString("%1/%2.json").arg(dir).arg(QCoreApplication::applicationPid());
    QFile f(m_file);
    // Owner-only before the token goes in
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    f.write(QJsonDocument(QJsonObject{{"server", m_server->fullServerName()},
                                      {"token", m_token},
                                      {"pid", QCoreApplication::applicationPid()},
                                      {"version", QCoreApplication::applicationVersion()}})
                .toJson());
    return true;
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
}

void Server::onConnection()
{
    while (QLocalSocket* s = m_server->nextPendingConnection()) {
        m_peers.insert(s, {});
        connect(s, &QLocalSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QLocalSocket::disconnected, this, [this, s] {
            detach(s);
            m_peers.remove(s);
            s->deleteLater();
        });
    }
}

void Server::detach(QLocalSocket* s)
{
    if (m_attached != s) return;
    m_attached = nullptr;
    const QString client = m_client;
    m_client.clear();
    m_host->liveConnectionChanged(false, client);
    emit stateChanged();
}

void Server::onReadyRead(QLocalSocket* s)
{
    if (!m_peers.contains(s)) return;
    m_peers[s].buffer += s->readAll();
    if (m_running) return; // picked up when the running command is done
    QPointer<QLocalSocket> guard(s);
    while (guard && m_peers.contains(s)) {
        QByteArray& buf = m_peers[s].buffer;
        const int nl = buf.indexOf('\n');
        if (nl < 0) break;
        const QByteArray msg = buf.left(nl).trimmed();
        buf.remove(0, nl + 1);
        if (msg.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(msg);
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
        if (type != "hello" || msg.value("token").toString() != m_token) {
            reply(errorReply("AUTH", "wrong token"));
            s->disconnectFromServer();
            return;
        }
        peer.authed = true;
    }
    if (type == "hello") {
        QJsonObject o{{"ok", true},
                      {"project", m_host->liveProjectPath()},
                      {"busy", m_attached && m_attached != s},
                      {"allowed", m_allowed},
                      {"pid", QCoreApplication::applicationPid()},
                      {"version", QCoreApplication::applicationVersion()}};
        if (!m_waiting.isEmpty()) o["waiting"] = m_waiting;
        if (!m_allowed && !m_refusal.isEmpty()) o["refusal"] = m_refusal;
        return reply(o);
    }
    // The app waits for the user (restore dialog at start): its project is not the one the user will work on yet
    if (!m_waiting.isEmpty() && (type == "attach" || type == "snapshot" || type == "run"))
        return reply(errorReply("APP_WAITING", m_waiting));
    if (type == "attach") {
        if (!m_allowed)
            return reply(errorReply("APP_DISCONNECTED", m_refusal.isEmpty() ? "the user disconnected the AI in the schneidi app"
                                                                             : m_refusal));
        if (m_attached && m_attached != s)
            return reply(errorReply("APP_BUSY", "another AI session is connected to the schneidi app"));
        m_attached = s;
        m_client = msg.value("client").toString("schneidi-cli");
        m_host->liveConnectionChanged(true, m_client);
        emit stateChanged();
        return reply({{"ok", true}});
    }
    if (m_attached != s) return reply(errorReply("NOT_ATTACHED", "attach first"));
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
        const QJsonObject result = runCommand(msg);
        m_running = false;
        if (guard) guard->write(line(result));
        // Requests that came in meanwhile
        QTimer::singleShot(0, this, [this] {
            for (QLocalSocket* p : m_peers.keys()) onReadyRead(p);
        });
        return;
    }
    reply(errorReply("BAD_REQUEST", "unknown request type " + type));
}

QJsonObject Server::runCommand(const QJsonObject& msg)
{
    const QString name = msg.value("command").toString();
    const Command* cmd = find(name);
    const QJsonObject args = msg.value("args").toObject(); // normalized by the client
    if (!cmd || !runsInApp(*cmd, args)) return errorReply("BAD_REQUEST", "command does not run in the app: " + name);
    HostScope scope(m_host);
    Project* p = m_host->liveProject();
    const QHash<int, QByteArray> before = clipStates(*p);
    const QVector<MediaInfo> mediaBefore = p->media();
    // One undo step per edit command; it never merges with the user's slider series before or after it
    QUndoStack* undo = p->undoStack();
    const bool step = name == "edit";
    const int index = undo->index();
    if (step) {
        p->closeMerge();
        undo->beginMacro(undoText(args));
    }
    QJsonObject result;
    Context ctx;
    try {
        result = cmd->run(args, ctx);
    } catch (const Error& e) {
        if (step) {
            undo->endMacro();
            if (undo->index() > index) undo->undo(); // all or nothing, like on the file
            restoreMedia(*p, mediaBefore);
        }
        return errorReply(e.code, e.message);
    }
    if (step) {
        undo->endMacro();
        p->closeMerge();
    }
    QVector<int> changed;
    const QHash<int, QByteArray> after = clipStates(*p);
    for (auto it = after.begin(); it != after.end(); ++it)
        if (before.value(it.key()) != it.value()) changed << it.key();
    if (!changed.isEmpty()) m_host->liveClipsChanged(changed);
    return {{"ok", true}, {"result", result}};
}

// ---------- schneidi-cli side ----------

class Connection {
public:
    QLocalSocket socket;
    QString project; // as the app reported it

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
};

std::vector<Candidate> runningApps()
{
    std::vector<Candidate> out;
    const QDir dir(registryDir());
    for (const QFileInfo& fi : dir.entryInfoList({"*.json"}, QDir::Files)) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject entry = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        auto c = std::make_unique<Connection>();
        c->socket.connectToServer(entry.value("server").toString());
        if (!c->socket.waitForConnected(1000)) {
            // App gone without cleaning up (crash): forget it and its socket file
            if (c->socket.error() == QLocalSocket::ServerNotFoundError
                || c->socket.error() == QLocalSocket::ConnectionRefusedError) {
                QFile::remove(fi.absoluteFilePath());
                QLocalServer::removeServer(entry.value("server").toString());
            }
            continue;
        }
        QJsonObject hello;
        try {
            hello = c->request({{"type", "hello"}, {"token", entry.value("token").toString()}}, 3000);
        } catch (const Error&) {
            continue;
        }
        if (!hello.value("ok").toBool()) continue;
        c->project = hello.value("project").toString();
        out.push_back({std::move(c), hello.value("busy").toBool(), hello.value("allowed").toBool(true),
                       hello.value("refusal").toString(), hello.value("waiting").toString()});
    }
    return out;
}

} // namespace

void setClientName(const QString& name) { g_clientName = name; }

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
    Candidate* pick = nullptr;
    if (any) {
        if (apps.empty()) detail::fail("NO_APP", "the schneidi app is not running (\"@app\" needs it)");
        if (apps.size() > 1) {
            QStringList projects;
            for (const Candidate& c : apps) projects << (c.conn->project.isEmpty() ? "(unsaved)" : c.conn->project);
            detail::fail("AMBIGUOUS", "several schneidi apps are running, give the project file instead of \"@app\": "
                                          + projects.join(", "));
        }
        pick = &apps[0];
    } else {
        for (Candidate& c : apps)
            if (!c.conn->project.isEmpty() && canonical(c.conn->project) == key) pick = &c;
        if (!pick) return nullptr;
    }
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

} // namespace Cli::Live
