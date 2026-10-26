// `schneidi-cli serve`: MCP over HTTP, files in and out, see Serve.h
#include "cli/Serve.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "cli/Http.h"
#include "cli/Mcp.h"
#include "cli/Sandbox.h"
#include "cli/ServeDetail.h"
#include "cli/ServeJobs.h"
#include "core/ProjectFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <memory>

namespace Cli::Serve {

using detail::fail;

QByteArray json(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

QJsonObject errorJson(const QString& code, const QString& message)
{
    return {{"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
}

namespace {

constexpr qint64 kMaxMessage = 16 << 20; // one JSON-RPC message (edit ops); files go through /files
constexpr int kMaxConnections = 256;
constexpr int kMaxPerAddress = 32; // one device (not behind a reverse proxy: everyone comes from it)
constexpr int kMaxSessions = 1000;

QString randomToken(int bytes)
{
    QByteArray b(bytes, Qt::Uninitialized);
    for (char& c : b) c = char(QRandomGenerator::system()->generate() & 0xff);
    return QString::fromLatin1(b.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// Every byte looked at, no early end: how long it takes does not tell how much of the token was right
bool sameToken(const QByteArray& given, const QByteArray& expected)
{
    if (expected.isEmpty()) return false;
    unsigned diff = unsigned(given.size() ^ expected.size());
    for (qsizetype i = 0; i < given.size(); ++i) diff |= unsigned(uchar(given[i]) ^ uchar(expected[i % expected.size()]));
    return diff == 0;
}

bool loopbackName(const QString& host)
{
    const QString h = host.toLower();
    if (h == "localhost" || h.endsWith(".localhost")) return true;
    const QHostAddress a(QString(h).remove('[').remove(']'));
    return !a.isNull() && a.isLoopback();
}

// "host:port" / "[::1]:port" -> host
QString hostOf(const QByteArray& hostHeader)
{
    const QString h = QString::fromLatin1(hostHeader).trimmed();
    if (h.startsWith('[')) return h.left(h.indexOf(']') + 1);
    return h.section(':', 0, 0);
}

// Server tools besides the commands: in the command table's form (schema and argument checks for free)
const QVector<Command>& serverTools(int jobWaitS)
{
    using T = ParamType;
    static const QVector<Command> list{
        {"jobs",
         QString("Jobs on this server. A call that takes longer than about %1 s (render, transcribe of long material, "
                 "upload …) answers {\"job\", \"state\":\"running\"} and goes on. Without 'job': all jobs, newest first. "
                 "With 'job': its state and progress, and once it ended its result (the answer the call would have "
                 "given, with download links); 'wait' waits up to that many seconds for the end; 'cancel' stops it.")
             .arg(jobWaitS),
         {{"job", T::String, "job id from a call's answer", false, true},
          {"wait", T::Number, QString("seconds to wait for the end (max %1)").arg(jobWaitS)},
          {"cancel", T::Boolean, "stop the job (a render leaves a half written file)"}},
         nullptr},
        {"files", "What is in the work folder of this server: files and folders with type (video, audio, image, project, "
                  "folder, file), size, modified time and a download url. The paths are relative to the work folder "
                  "and work as they are in the other tools.",
         {{"path", T::Path, "folder (or file) in the work folder (default: the work folder itself)", false, true},
          {"recursive", T::Boolean, QString("with the contents of sub folders (up to %1 entries)").arg(kListLimit)}},
         nullptr},
        {"upload", "Fetch a file from an http(s) URL into the work folder (e.g. material from a cloud link), with "
                   "progress. Answers path, size and url; a long download becomes a job (see `jobs`). People can also "
                   "upload with HTTP PUT /files/<path>.",
         {{"url", T::String, "http(s) address of the file", true, true},
          {"path", T::Path, "where to save it in the work folder: a file, or a folder (an existing one, or ending in "
                            "'/'; made if missing) for the URL's file name (default: the work folder itself)"},
          {"overwrite", T::Boolean, "replace an existing file"}},
         nullptr},
    };
    return list;
}

volatile std::sig_atomic_t g_quit = 0;
void onQuitSignal(int) { g_quit = 1; }

} // namespace

Server::Server(const Options& opt) : m_opt(opt), m_tcp(new QTcpServer(this)), m_net(new QNetworkAccessManager(this))
{
    m_mcp.server = true;
    m_mcp.app = false;
    m_mcp.extensions = opt.extensions;
    m_mcp.root = Sandbox::root();
    m_mcp.jobWaitS = opt.jobWaitS;
    for (const Command& c : serverTools(opt.jobWaitS))
        m_mcp.extraTools << QJsonObject{{"name", c.name}, {"description", c.summary}, {"inputSchema", inputSchema(c)}};

    // Workers: the same program as MCP server on stdio, in the same work folder (and nothing else). Without the maps:
    // the arguments come mapped already, and withLinks maps the answers back (twice would map a path again)
    QStringList args{"mcp", "--root", Sandbox::root()};
    if (!opt.extensions) args << "--no-extensions";
    if (opt.verbose) args << "--verbose";
    m_jobs = new Jobs(args, Sandbox::root(), opt.workers, this);
    m_jobs->setResultFilter([this](const Job& j, const QJsonObject& r) { return withLinks(j, r); });
}

bool Server::listen(QString* error)
{
    if (!m_tcp->listen(m_opt.listen, quint16(m_opt.port))) {
        *error = m_tcp->errorString();
        return false;
    }
    connect(m_tcp, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* s = m_tcp->nextPendingConnection()) {
            // Not one device taking all connections (slow or idle ones); a reverse proxy (this computer, --public-url)
            // brings everyone
            const QHostAddress peer = s->peerAddress();
            const QString address = peer.isLoopback() || !m_opt.publicUrl.isEmpty() ? QString() : peer.toString();
            if (m_connections >= kMaxConnections || (!address.isEmpty() && m_perAddress.value(address) >= kMaxPerAddress)) {
                s->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                s->disconnectFromHost();
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                continue;
            }
            auto* c = new Http::Connection(s, this);
            ++m_connections;
            if (!address.isEmpty()) ++m_perAddress[address];
            connect(c, &Http::Connection::headReceived, this, [this](Http::Connection* x) { onHead(x); });
            connect(c, &Http::Connection::requestReceived, this, [this](Http::Connection* x) { onRequest(x); });
            connect(c, &Http::Connection::closed, this, [this, address](Http::Connection* x) {
                --m_connections;
                if (!address.isEmpty() && --m_perAddress[address] <= 0) m_perAddress.remove(address);
                m_uploads.erase(x); // an unfinished upload: the temporary file goes away
            });
        }
    });
    return true;
}

QString Server::url() const
{
    if (!m_opt.publicUrl.isEmpty()) return m_opt.publicUrl + "/mcp";
    QString host = m_opt.listenText;
    if (m_opt.listen == QHostAddress::Any || m_opt.listen == QHostAddress::AnyIPv4 || m_opt.listen == QHostAddress::AnyIPv6)
        host = QHostInfo::localHostName();
    else if (m_opt.listen.protocol() == QAbstractSocket::IPv6Protocol)
        host = '[' + m_opt.listen.toString() + ']';
    return QString("http://%1:%2/mcp").arg(host).arg(port());
}

// ---------- Access ----------

// Without a token only this computer can connect; a web page must not get in through a name that points here
// (DNS rebinding): only local host names
bool Server::allowedHost(const Http::Request& r) const
{
    if (!m_opt.token.isEmpty()) return true;
    const QByteArray host = r.header("host");
    if (host.isEmpty()) return true;
    const QString h = hostOf(host);
    return loopbackName(h) || (!m_opt.publicUrl.isEmpty() && QUrl(m_opt.publicUrl).host().compare(h, Qt::CaseInsensitive) == 0);
}

// Requests from web pages carry an Origin: only local pages and the ones allowed with --allow-origin
bool Server::allowedOrigin(const Http::Request& r) const
{
    const QByteArray origin = r.header("origin");
    if (origin.isEmpty()) return true; // not from a browser (curl, AI clients)
    const QString o = QString::fromLatin1(origin);
    if (m_opt.origins.contains(o) || m_opt.origins.contains("*")) return true;
    if (!m_opt.publicUrl.isEmpty() && QUrl(m_opt.publicUrl).adjusted(QUrl::RemovePath).toString() == o) return true;
    return o != "null" && loopbackName(QUrl(o).host());
}

bool Server::authorized(const Http::Request& r) const
{
    if (m_opt.token.isEmpty()) return true;
    const QByteArray h = r.header("authorization");
    if (!h.left(7).toLower().startsWith("bearer ")) return false;
    return sameToken(h.mid(7).trimmed(), m_opt.token.toUtf8());
}

Http::Headers Server::cors(const Http::Request& r) const
{
    const QByteArray origin = r.header("origin");
    if (origin.isEmpty()) return {};
    return {{"Access-Control-Allow-Origin", origin},
            {"Vary", "Origin"},
            {"Access-Control-Expose-Headers", "Mcp-Session-Id, Content-Range"}};
}

QString Server::baseUrl(const Http::Request& r) const
{
    if (!m_opt.publicUrl.isEmpty()) return m_opt.publicUrl;
    // Behind a reverse proxy (TLS): the address the client used
    QByteArray host = r.header("x-forwarded-host").split(',').first().trimmed();
    if (host.isEmpty()) host = r.header("host");
    if (host.isEmpty()) return QUrl(url()).adjusted(QUrl::RemovePath).toString();
    const QByteArray proto = r.header("x-forwarded-proto").split(',').first().trimmed().toLower();
    return QString("%1://%2").arg(proto == "https" ? "https" : "http", QString::fromLatin1(host));
}

QString Server::fileUrl(const QString& base, const QString& path) const
{
    const QString rel = Sandbox::relative(path);
    if (rel.isNull()) return {};
    QStringList parts;
    for (const QString& p : rel.split('/', Qt::SkipEmptyParts)) parts << QString::fromLatin1(QUrl::toPercentEncoding(p));
    return base + "/files/" + parts.join('/');
}

// ---------- Requests ----------

void Server::onHead(Http::Connection* c)
{
    const Http::Request& r = c->request();
    if (!allowedHost(r) || !allowedOrigin(r)) {
        c->respondJson(403, errorJson("FORBIDDEN", "this Origin/Host may not use the server (web pages: --allow-origin; "
                                                   "other host names: use a token)"));
        return;
    }
    if (r.method == "OPTIONS") { // CORS preflight of an allowed web page
        Http::Headers h = cors(r);
        h << QPair<QByteArray, QByteArray>{"Access-Control-Allow-Methods", "GET, HEAD, POST, PUT, DELETE"}
          << QPair<QByteArray, QByteArray>{"Access-Control-Allow-Headers",
                                           "Authorization, Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version, "
                                           "Last-Event-ID, Range, If-None-Match"}
          << QPair<QByteArray, QByteArray>{"Access-Control-Max-Age", "600"};
        c->respond(204, {}, {}, h);
        return;
    }
    if (!authorized(r)) {
        Http::Headers h = cors(r);
        h << QPair<QByteArray, QByteArray>{"WWW-Authenticate", "Bearer realm=\"schneidi\""};
        c->respondJson(401, errorJson("UNAUTHORIZED", "missing or wrong token (header Authorization: Bearer <token>)"), h);
        return;
    }
    if (r.path == "/mcp") {
        if (r.method == "POST") c->bufferBody(kMaxMessage);
        return; // answered in onRequest
    }
    if (r.path == "/files" || r.path.startsWith("/files/")) {
        filesHead(c);
        return;
    }
    c->respondJson(404, errorJson("NOT_FOUND", "nothing here: MCP is at /mcp, files at /files/<path>"), cors(r));
}

void Server::onRequest(Http::Connection* c)
{
    const Http::Request& r = c->request();
    if (r.path == "/mcp") {
        if (r.method == "POST") {
            mcpPost(c);
        } else if (r.method == "DELETE") {
            const QString id = QString::fromLatin1(r.header("mcp-session-id"));
            c->respond(m_sessions.remove(id) ? 204 : 404, {}, {}, cors(r));
        } else {
            // No stream of messages from the server alone (it never asks the client anything)
            Http::Headers h = cors(r);
            h << QPair<QByteArray, QByteArray>{"Allow", "POST, DELETE"};
            c->respondJson(405, errorJson("METHOD", "POST JSON-RPC messages to /mcp"), h);
        }
        return;
    }
    if (r.method == "PUT") filesPutDone(c);
    else filesGet(c);
}

// ---------- MCP over HTTP ----------

void Server::mcpPost(Http::Connection* c)
{
    const Http::Request& r = c->request();
    const auto rpcError = [&](int status, int code, const QString& message) {
        c->respondJson(status, {{"jsonrpc", "2.0"}, {"id", QJsonValue::Null},
                                {"error", QJsonObject{{"code", code}, {"message", message}}}}, cors(r));
    };
    const QString version = QString::fromLatin1(r.header("mcp-protocol-version"));
    if (!version.isEmpty() && !Mcp::versions().contains(version))
        return rpcError(400, -32600, "unsupported MCP-Protocol-Version " + version + " (spoken: " + Mcp::versions().join(", ") + ")");
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(r.body, &err);
    if (!doc.isObject() && !doc.isArray()) return rpcError(400, -32700, "parse error: " + err.errorString());
    const bool batch = doc.isArray(); // 2025-03-26 clients may send several messages at once
    const QJsonArray messages = batch ? doc.array() : QJsonArray{doc.object()};
    if (messages.isEmpty()) return rpcError(400, -32600, "empty batch");
    bool initialize = false;
    for (const QJsonValue& m : messages) {
        if (!m.isObject()) return rpcError(400, -32600, "a message is not a JSON object");
        if (m["method"].toString() == "initialize" && m.toObject().contains("id")) initialize = true;
    }
    // initialize opens a session (its id comes back as header), every other message needs one
    QString session;
    if (initialize) {
        if (messages.size() > 1) return rpcError(400, -32600, "initialize must be sent alone");
        const QDateTime now = QDateTime::currentDateTime();
        for (auto it = m_sessions.begin(); it != m_sessions.end();)
            it = it->used.secsTo(now) > 24 * 3600 ? m_sessions.erase(it) : std::next(it);
        if (m_sessions.size() >= kMaxSessions) {
            auto oldest = std::min_element(m_sessions.begin(), m_sessions.end(),
                                           [](const Session& a, const Session& b) { return a.used < b.used; });
            m_sessions.erase(oldest);
        }
        session = randomToken(18);
        m_sessions[session].used = now;
    } else {
        session = QString::fromLatin1(r.header("mcp-session-id"));
        if (session.isEmpty()) return rpcError(400, -32600, "missing Mcp-Session-Id header (send initialize first)");
        if (!m_sessions.contains(session))
            return rpcError(404, -32001, "unknown session (it ended or the server restarted): initialize again");
        m_sessions[session].used = QDateTime::currentDateTime();
    }
    // Requests get answers; notifications and responses of the client just "Accepted"
    QVector<QJsonObject> requests;
    for (const QJsonValue& m : messages) {
        const QJsonObject o = m.toObject();
        if (o.contains("id") && o.contains("method")) requests << o;
        else mcpNotification(session, o);
    }
    Http::Headers headers = cors(r);
    if (initialize) headers << QPair<QByteArray, QByteArray>{"Mcp-Session-Id", session.toLatin1()};
    if (requests.isEmpty()) {
        c->respond(202, {}, {}, headers);
        return;
    }
    const QString base = baseUrl(r);
    QPointer<Http::Connection> conn(c);
    auto* context = new QObject(c); // goes with the connection: waiting answers are dropped (their jobs go on)
    const QJsonObject& first = requests.first();
    const QJsonValue token = first["params"]["_meta"]["progressToken"];
    const bool stream = !batch && first["method"].toString() == "tools/call" && !token.isUndefined() && !token.isNull()
                        && r.header("accept").contains("text/event-stream");
    if (stream) {
        // Progress as it comes, then the answer, as Server-Sent Events
        c->beginStream(200, "text/event-stream", headers);
        auto* ping = new QTimer(context); // keeps proxies from closing a quiet stream (a job waiting for its project)
        connect(ping, &QTimer::timeout, context, [conn] {
            if (conn) conn->streamData(": ping\n\n");
        });
        ping->start(15000);
        const Mcp::Send send = [conn, context](const QJsonObject& msg) {
            if (!conn) return;
            conn->streamData("event: message\ndata: " + json(msg) + "\n\n");
            if (msg.contains("result") || msg.contains("error")) {
                conn->endStream();
                context->deleteLater();
            }
        };
        mcpRequest(first, session, base, send, context);
        return;
    }
    // JSON: the answer(s) once all are there; progress has no way to the client
    auto answers = std::make_shared<QJsonArray>();
    for (int i = 0; i < requests.size(); ++i) *answers << QJsonValue::Null;
    auto left = std::make_shared<int>(int(requests.size()));
    for (int i = 0; i < requests.size(); ++i) {
        const Mcp::Send send = [conn, context, answers, left, i, batch, headers](const QJsonObject& msg) {
            if (!(msg.contains("result") || msg.contains("error"))) return;
            (*answers)[i] = msg;
            if (--*left > 0 || !conn) return;
            conn->respond(200, "application/json",
                          batch ? QJsonDocument(*answers).toJson(QJsonDocument::Compact) : json((*answers)[0].toObject()),
                          headers);
            context->deleteLater();
        };
        mcpRequest(requests[i], session, base, send, context);
    }
}

void Server::mcpRequest(const QJsonObject& msg, const QString& session, const QString& base, const Mcp::Send& send,
                        QObject* context)
{
    const QString key = QString::fromUtf8(QJsonDocument(QJsonArray{msg["id"]}).toJson(QJsonDocument::Compact));
    const Mcp::ToolCall call = [this, session, key, base, context](const QJsonObject& params, const Mcp::Send& notify,
                                                                     const Mcp::Done& done) {
        callTool(params, session, key, base, notify, done, context);
    };
    Mcp::handle(msg, m_mcp, call, send);
}

void Server::mcpNotification(const QString& session, const QJsonObject& msg)
{
    // The client gave up on a call (e.g. the user pressed Esc): stop its job
    if (msg["method"].toString() != "notifications/cancelled") return;
    const QString key = QString::fromUtf8(QJsonDocument(QJsonArray{msg["params"]["requestId"]}).toJson(QJsonDocument::Compact));
    const auto s = m_sessions.find(session);
    if (s == m_sessions.end()) return;
    if (Job* j = m_jobs->find(s->calls.take(key))) m_jobs->cancel(j);
}

QString Server::writtenProject(const Command& cmd, const QJsonObject& args) const
{
    QString file;
    if (runsInApp(cmd, args) || cmd.name == "new") file = args["project"].toString();
    else if (cmd.name == "transcribe" && args["apply"].toBool()) file = args["source"].toString();
    else if (cmd.name == "beats" && !args["apply"].toString().isEmpty()) file = args["file"].toString();
    if (file.isEmpty()) return {};
    // The same key for a file `new` creates and the existing one (a work folder reached through a link)
    const QFileInfo fi(file);
    const QString folder = QFileInfo(fi.absolutePath()).canonicalFilePath();
    const QString key = fi.exists() ? fi.canonicalFilePath()
                        : folder.isEmpty() ? fi.absoluteFilePath()
                                           : QDir(folder).filePath(fi.fileName());
#ifdef Q_OS_WIN
    return key.toLower();
#else
    return key;
#endif
}

void Server::callTool(const QJsonObject& params, const QString& session, const QString& requestKey, const QString& base,
                      const Mcp::Send& notify, const Mcp::Done& done, QObject* context)
{
    const QString name = params["name"].toString();
    const QJsonValue token = params["_meta"].toObject().value("progressToken");
    const Command* cmd = nullptr;
    for (const Command& c : serverTools(m_opt.jobWaitS))
        if (c.name == name) cmd = &c;
    if (!cmd) cmd = find(name);
    if (!cmd || !Mcp::offered(name, m_mcp)) {
        done(Mcp::toolError("NOT_FOUND", "unknown tool " + name));
        return;
    }
    auto job = std::make_unique<Job>();
    try {
        // Checked here already (paths in the work folder …): mistakes answer at once, without a worker
        job->args = normalize(*cmd, params["arguments"].toObject());
        if (name == "files") {
            done(Mcp::toolResult(filesTool(job->args, base)));
            return;
        }
        if (name == "jobs") {
            jobsTool(job->args, token, notify, done, context);
            return;
        }
        job->tool = name;
        job->baseUrl = base;
        if (name == "upload") prepareUpload(*job, params["arguments"]["path"].toString());
        else job->writes = writtenProject(*cmd, job->args);
    } catch (const Error& e) {
        done(Mcp::toolError(e.code, e.message));
        return;
    }
    const Job* j = m_jobs->submit(std::move(job));
    if (auto s = m_sessions.find(session); s != m_sessions.end()) {
        if (s->calls.size() > 1000) s->calls.clear();
        s->calls[requestKey] = j->id;
    }
    waitFor(j->id, m_opt.jobWaitS, token, notify, done, context, false);
}

void Server::waitFor(const QString& id, double seconds, const QJsonValue& token, const Mcp::Send& notify,
                     const Mcp::Done& done, QObject* context, bool withJob)
{
    const Job* j = m_jobs->find(id);
    if (!j || j->over()) {
        done(j ? answer(*j, withJob) : Mcp::toolError("NOT_FOUND", "no job " + id));
        return;
    }
    auto* waiter = new QObject(context);
    auto fired = std::make_shared<bool>(false);
    const bool progress = !token.isUndefined() && !token.isNull() && notify;
    connect(m_jobs, &Jobs::changed, waiter, [=, this](Job* changed) {
        if (*fired || changed->id != id) return;
        if (changed->over()) {
            *fired = true;
            done(answer(*changed, withJob));
            waiter->deleteLater();
        } else if (progress && changed->state == Job::State::Running) {
            notify({{"jsonrpc", "2.0"}, {"method", "notifications/progress"},
                    {"params", QJsonObject{{"progressToken", token}, {"progress", changed->progress}, {"total", 100}}}});
        }
    });
    // Too long for one HTTP request: answer with the job, it goes on
    QTimer::singleShot(int(seconds * 1000), waiter, [=, this] {
        if (*fired) return;
        *fired = true;
        const Job* now = m_jobs->find(id);
        done(now ? answer(*now, true) : Mcp::toolError("NOT_FOUND", "no job " + id));
        waiter->deleteLater();
    });
}

QJsonObject Server::answer(const Job& j, bool withJob) const
{
    if (j.over()) {
        if (!withJob) return j.result;
        // The call's own answer, plus which job it was
        QJsonObject r = j.result;
        QJsonArray content = r["content"].toArray();
        QJsonObject first = QJsonDocument::fromJson(content.first()["text"].toString().toUtf8()).object();
        first["job"] = j.id;
        first["state"] = stateName(j.state);
        content[0] = QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(json(first))}};
        r["content"] = content;
        return r;
    }
    QJsonObject o{{"job", j.id}, {"tool", j.tool}, {"state", stateName(j.state)}, {"progress", j.progress}};
    o["note"] = QString("%1: call `jobs` with job '%2' and wait (up to %3 s) for its result")
                    .arg(j.state == Job::State::Queued
                             ? "waiting for its turn (another change to the same project, or all workers busy), it "
                               "starts by itself"
                             : "still running on the server (nothing is lost)",
                         j.id)
                    .arg(m_opt.jobWaitS);
    return Mcp::toolResult(o);
}

// Files the call wrote (render out, frames, scopes image, srt …) get a download link
QJsonObject Server::withLinks(const Job& j, const QJsonObject& result) const
{
    QJsonArray content = result["content"].toArray();
    if (content.isEmpty() || content.first()["type"].toString() != "text") return result;
    QJsonObject answer = QJsonDocument::fromJson(content.first()["text"].toString().toUtf8()).object();
    if (answer.isEmpty()) return result;
    const QDateTime since = j.started.addSecs(-1); // file times of some file systems are rounded
    std::function<void(QJsonObject&)> walk = [&](QJsonObject& o) {
        QJsonObject links;
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it->isObject()) {
                QJsonObject sub = it->toObject();
                walk(sub);
                *it = sub;
                continue;
            }
            if (it->isArray()) {
                QJsonArray arr = it->toArray();
                for (int i = 0; i < arr.size(); ++i)
                    if (arr[i].isObject()) {
                        QJsonObject sub = arr[i].toObject();
                        walk(sub);
                        arr[i] = sub;
                    }
                *it = arr;
                continue;
            }
            static const QStringList keys{"out", "file", "sheet", "image", "srt", "frames_dir"};
            if (!it->isString() || !keys.contains(it.key())) continue;
            const QString path = it->toString(); // as the worker wrote it: a path of this computer
            if (Sandbox::relative(path).isNull()) continue; // not in the work folder
            const QFileInfo fi(path);
            const bool fresh = it.key() == "frames_dir" ? fi.isDir() : fi.isFile() && fi.lastModified() >= since;
            if (!fresh) continue;
            const QString link = fileUrl(j.baseUrl, path);
            if (!link.isEmpty()) links[it.key() == "out" || it.key() == "file" ? QString("url") : it.key() + "_url"] = link;
        }
        for (auto it = links.begin(); it != links.end(); ++it)
            if (!o.contains(it.key())) o[it.key()] = it.value();
    };
    walk(answer);
    answer = Sandbox::mapBack(QJsonValue(answer)).toObject(); // paths as the client knows them (--map)
    content[0] = QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(json(answer))}};
    QJsonObject r = result;
    r["content"] = content;
    return r;
}

// ---------- Server tools ----------

void Server::jobsTool(const QJsonObject& args, const QJsonValue& token, const Mcp::Send& notify, const Mcp::Done& done,
                      QObject* context)
{
    const QString id = args["job"].toString().trimmed();
    if (id.isEmpty()) {
        done(Mcp::toolResult(jobList()));
        return;
    }
    Job* j = m_jobs->find(id);
    if (!j) {
        done(Mcp::toolError("NOT_FOUND", QString("no job '%1' (ended jobs are kept for an hour; `jobs` lists them)").arg(id)));
        return;
    }
    if (args["cancel"].toBool()) m_jobs->cancel(j);
    const double wait = std::clamp(args["wait"].toDouble(), 0.0, double(m_opt.jobWaitS));
    if (wait <= 0 || j->over()) {
        done(answer(*j, true));
        return;
    }
    waitFor(id, wait, token, notify, done, context, true);
}

QJsonObject Server::jobList() const
{
    QJsonArray list;
    const QDateTime now = QDateTime::currentDateTime();
    int ended = 0, older = 0;
    for (const Job* j : m_jobs->list()) {
        // All that run or wait, of the ended ones the newest (every call is a job, most end at once)
        if (j->over() && ++ended > 20) {
            ++older;
            continue;
        }
        QJsonObject o{{"job", j->id}, {"tool", j->tool}, {"state", stateName(j->state)}, {"progress", j->progress},
                      {"created", j->created.toString(Qt::ISODate)}};
        for (const char* k : {"project", "source", "file", "out", "url"})
            if (j->args.contains(k)) o[k] = Sandbox::mapBack(j->args[k]);
        if (j->started.isValid())
            o["seconds"] = std::round(j->started.msecsTo(j->ended.isValid() ? j->ended : now) / 100.0) / 10;
        if (j->state == Job::State::Failed) {
            const QJsonObject r = QJsonDocument::fromJson(j->result["content"][0]["text"].toString().toUtf8()).object();
            o["error"] = r["error"];
        }
        list << o;
    }
    QJsonObject out{{"jobs", list}, {"workers", QJsonObject{{"busy", m_jobs->busyWorkers()}, {"max", m_jobs->maxWorkers()}}}};
    if (older) out["older_ended"] = older; // still there for `jobs` with their id (an hour)
    return out;
}

namespace {

// ---------- Options ----------

QString tokenFile() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/serve-token"; }

Options parse(const QStringList& args)
{
    const auto o = parseOptions(args,
                                {"listen", "port", "root", "map", "token", "token-file", "workers", "job-wait",
                                 "max-upload", "public-url", "allow-origin"},
                                {"allow-extensions"});
    const auto one = [&o](const char* name) { return o.value(name).isEmpty() ? QString() : o.value(name).last(); };
    const auto number = [&](const char* name, int lo, int hi, int fallback) {
        if (!o.contains(name)) return fallback;
        bool ok = false;
        const int v = one(name).toInt(&ok);
        if (!ok || v < lo || v > hi) fail("USAGE", QString("--%1 must be a number from %2 to %3").arg(name).arg(lo).arg(hi));
        return v;
    };
    Options opt;
    if (o.contains("listen")) {
        opt.listenText = one("listen");
        const QString l = opt.listenText.toLower();
        if (l == "localhost") opt.listen = QHostAddress::LocalHost;
        else if (l == "*" || l == "any") opt.listen = QHostAddress::Any;
        else if (!opt.listen.setAddress(QString(opt.listenText).remove('[').remove(']')))
            fail("USAGE", "--listen needs an IP address like 127.0.0.1, 0.0.0.0 or ::, got " + opt.listenText);
    }
    opt.port = number("port", 0, 65535, 8642);
    // Off this computer the work folder is given on purpose (started in the home folder, all of it would be served)
    if (!o.contains("root") && !opt.listen.isLoopback())
        fail("USAGE", "--root is needed when the server is reachable from other devices: the work folder, everything in "
                      "it can be read and written");
    opt.root = o.contains("root") ? one("root") : QDir::currentPath();
    opt.maps = o.value("map");
    opt.workers = number("workers", 1, 64, std::clamp(QThread::idealThreadCount() / 2, 1, 4));
    opt.jobWaitS = number("job-wait", 1, 3600, 50);
    opt.maxUpload = qint64(number("max-upload", 1, 100 * 1024 * 1024, 50 * 1024)) << 20;
    opt.extensions = o.contains("allow-extensions");
    opt.publicUrl = one("public-url");
    while (opt.publicUrl.endsWith('/')) opt.publicUrl.chop(1);
    if (!opt.publicUrl.isEmpty() && !QUrl(opt.publicUrl).isValid()) fail("USAGE", "--public-url is not a URL");
    opt.origins = o.value("allow-origin");
    opt.verbose = qEnvironmentVariableIsSet("SCHNEIDI_VERBOSE");
    // Token: given, from a file, from the environment; off this computer always (made once and kept)
    if (o.contains("token")) {
        opt.token = one("token");
        opt.tokenFrom = "--token";
    } else if (o.contains("token-file")) {
        QFile f(one("token-file"));
        if (!f.open(QIODevice::ReadOnly)) fail("USAGE", "cannot read --token-file " + f.fileName());
        opt.token = QString::fromUtf8(f.readLine()).trimmed();
        if (opt.token.isEmpty()) fail("USAGE", "--token-file is empty: " + f.fileName());
        opt.tokenFrom = f.fileName();
    } else if (!qEnvironmentVariable("SCHNEIDI_TOKEN").trimmed().isEmpty()) {
        opt.token = qEnvironmentVariable("SCHNEIDI_TOKEN").trimmed();
        opt.tokenFrom = "SCHNEIDI_TOKEN";
    } else if (!opt.listen.isLoopback()) {
        QFile f(tokenFile());
        if (f.open(QIODevice::ReadOnly)) opt.token = QString::fromUtf8(f.readLine()).trimmed();
        f.close();
        if (opt.token.isEmpty()) {
            opt.token = randomToken(32);
            QDir().mkpath(QFileInfo(f.fileName()).absolutePath());
            if (!f.open(QIODevice::WriteOnly) || f.write(opt.token.toUtf8() + '\n') < 0)
                fail("WRITE_FAILED", "cannot store the token in " + f.fileName());
            f.close();
            f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        }
        opt.tokenFrom = f.fileName();
        opt.tokenShown = true;
    }
    // An empty --token would switch the check off for everyone on the network
    if ((!opt.token.isEmpty() || !opt.listen.isLoopback()) && opt.token.size() < 16)
        fail("USAGE", "the token is too short (at least 16 characters; leave it out to get a generated one)");
    return opt;
}

} // namespace

QHash<QString, QStringList> parseOptions(const QStringList& args, const QStringList& valued, const QStringList& flags)
{
    QHash<QString, QStringList> out;
    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        if (!a.startsWith("--") || a.size() < 3) fail("USAGE", "unexpected argument: " + a + " (see: schneidi-cli help)");
        QString name = a.mid(2);
        QString value;
        const qsizetype eq = name.indexOf('=');
        if (eq > 0) {
            value = name.mid(eq + 1);
            name = name.left(eq);
        }
        if (flags.contains(name) && eq < 0) {
            out[name] << "true";
            continue;
        }
        if (!valued.contains(name)) fail("USAGE", "unknown option --" + name + " (see: schneidi-cli help)");
        if (eq < 0) {
            if (i + 1 >= args.size()) fail("USAGE", "missing value for --" + name);
            value = args[++i];
        }
        out[name] << value;
    }
    return out;
}

QJsonObject help()
{
    return {
        {"mcp",
         QJsonObject{{"usage", "schneidi-cli mcp [--root <folder>] [--map '<their path>=<path here>']… [--no-extensions]"},
                     {"summary", "MCP server on stdin/stdout (Claude Desktop, Claude Code, …): every command is a tool."},
                     {"options",
                      QJsonObject{{"root", "work folder: relative paths start there, nothing outside it can be used "
                                           "(also no live connection to the app)"},
                                  {"map", "path of another computer = path here, e.g. 'Z:\\Clips=/data/clips' "
                                          "(repeatable); answers show the other computer's paths"},
                                  {"no-extensions", "no `extensions` tool (downloads programs)"}}}}},
        {"serve",
         QJsonObject{
             {"usage", "schneidi-cli serve [--listen 127.0.0.1] [--port 8642] [--root <folder>] [--token-file <file>] …"},
             {"summary", "MCP server over HTTP (Streamable HTTP) for AIs on other devices and automation: POST /mcp, "
                         "files in and out with PUT/GET /files/<path>, long calls as jobs (tool `jobs`)."},
             {"options",
              QJsonObject{
                  {"listen", "address to listen on (default 127.0.0.1 = only this computer; 0.0.0.0 = all networks, "
                             "then a token is required)"},
                  {"port", "TCP port (default 8642; 0 = any free one, printed at start)"},
                  {"root", "work folder: all paths are inside it, its hidden files are not served (default on "
                           "127.0.0.1: the current folder; required on any other address)"},
                  {"map", "path of another computer = path here, e.g. 'Z:\\Clips=/data/clips' (repeatable)"},
                  {"token", "token clients must send (Authorization: Bearer …); better --token-file or the "
                            "environment variable SCHNEIDI_TOKEN (not visible in the process list). Not on 127.0.0.1: "
                            "without one a token is generated once, stored and printed"},
                  {"token-file", "file with the token (first line)"},
                  {"workers", "worker processes for parallel calls (default: half the processor threads, max 4)"},
                  {"job-wait", "seconds a call waits before it answers with a job (default 50)"},
                  {"max-upload", "largest upload in MB (default 51200)"},
                  {"public-url", "address clients use (behind a reverse proxy), e.g. https://schneidi.example.org"},
                  {"allow-origin", "web page allowed to call the server, e.g. https://example.org (repeatable)"},
                  {"allow-extensions", "offer the `extensions` tool (downloads programs onto the server)"}}}}},
    };
}

int run(const QStringList& args)
{
    const auto print = [](const QJsonObject& o) {
        const QByteArray out = json(o) + '\n';
        std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
        std::fflush(stdout);
    };
    Options opt;
    try {
        opt = parse(args);
        Sandbox::configure({opt.root, opt.maps, false});
    } catch (const Error& e) {
        print(errorJson(e.code, e.message));
        return 2;
    }
    Server server(opt);
    QString error;
    if (!server.listen(&error)) {
        print(errorJson("LISTEN_FAILED", QString("cannot listen on %1:%2: %3").arg(opt.listenText).arg(opt.port).arg(error)));
        return 1;
    }
    // stdout: one line JSON for programs (tests, scripts); stderr: the banner for people
    const QString url = server.url();
    print({{"ok", true}, {"url", url}, {"port", server.port()}, {"root", Sandbox::root()}, {"token", !opt.token.isEmpty()}});
    QString banner = QString("schneidi server %1\n  MCP:      %2\n  Files:    %3/files/<path>\n  Folder:   %4 (all paths "
                             "are inside it)\n  Workers:  up to %5\n")
                         .arg(QCoreApplication::applicationVersion(), url, url.chopped(4), Sandbox::root())
                         .arg(opt.workers);
    QString header;
    if (opt.token.isEmpty()) {
        banner += "  Token:    none (only this computer can connect)\n";
    } else if (opt.tokenShown) {
        banner += QString("  Token:    %1\n            (stored in %2; delete the file for a new one)\n").arg(opt.token, opt.tokenFrom);
        header = QString(" --header \"Authorization: Bearer %1\"").arg(opt.token);
    } else {
        banner += QString("  Token:    from %1\n").arg(opt.tokenFrom);
        header = opt.tokenFrom == "SCHNEIDI_TOKEN" ? " --header \"Authorization: Bearer $SCHNEIDI_TOKEN\""
                                                   : " --header \"Authorization: Bearer <token>\"";
    }
    banner += QString("Add it to Claude Code:\n  claude mcp add --transport http schneidi %1%2\n").arg(url, header);
    std::fputs(banner.toUtf8().constData(), stderr);
    std::fflush(stderr);

    // Ctrl+C / docker stop: end cleanly (the workers are stopped with the server)
    std::signal(SIGINT, onQuitSignal);
    std::signal(SIGTERM, onQuitSignal);
    QTimer quit;
    QObject::connect(&quit, &QTimer::timeout, [] {
        if (g_quit) QCoreApplication::quit();
    });
    quit.start(200);
    return QCoreApplication::exec();
}

} // namespace Cli::Serve
