// schneidi-cli serve (cli/Serve.h): the work folder rules in this process (cli/Sandbox.h), then real server processes
// on a free port with a temporary work folder. Token, sessions, tools over HTTP (new/edit/info on a project), paths
// outside the work folder (.., absolute, symlink, inside edit ops), files up and down (Range), render with progress as
// Server-Sent Events and a download link, upload from a URL, long calls as jobs (wait, cancel), changes to one project
// one after another while other projects and reading calls run side by side, a crashed worker comes back.
// Slow calls without slow media: the workers' test hooks SCHNEIDI_TEST_CALL_DELAY_MS / SCHNEIDI_TEST_CRASH_TOOL
// (cli/Mcp.cpp).
#include "cli/Commands.h"
#include "cli/Sandbox.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

#include "check.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

namespace {

const QByteArray kToken = "test-token-0123456789abcdef";

QString codeOf(const std::function<void()>& f)
{
    try {
        f();
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

// ---------- Work folder rules, in this process ----------

void sandboxRules(const QString& base)
{
    QDir(base).mkpath("root/clips/sub");
    QDir(base).mkpath("outside");
    const QString root = QDir(base).filePath("root");
    QFile(QDir(base).filePath("outside/secret.mp4")).open(QIODevice::WriteOnly);
    QFile::link(QDir(base).filePath("outside"), root + "/link");
    QFile::link(QDir(base).filePath("outside/missing.mp4"), root + "/dangling.mp4");
    const QString cwd = QDir::currentPath();
    Cli::Sandbox::configure({root, {"Z:\\Clips=clips", "/home/me/Videos/=/nowhere"}});
    using Cli::Sandbox::resolve;
    CHECK_EQ(resolve("a.mp4"), root + "/a.mp4");
    CHECK_EQ(resolve(root + "/clips/./x.mp4"), root + "/clips/x.mp4");
    CHECK_EQ(codeOf([] { resolve("../outside/secret.mp4"); }), QString("OUTSIDE_ROOT"));
    CHECK_EQ(codeOf([] { resolve("clips/../../outside"); }), QString("OUTSIDE_ROOT"));
    CHECK_EQ(codeOf([&] { resolve(QDir(base).filePath("outside/secret.mp4")); }), QString("OUTSIDE_ROOT"));
    // Symlinks (Windows: QFile::link makes a shortcut, not a link)
    if (QFileInfo(root + "/link").isSymLink()) {
        CHECK_EQ(codeOf([] { resolve("link/secret.mp4"); }), QString("OUTSIDE_ROOT")); // out of the work folder
        CHECK_EQ(codeOf([] { resolve("link/new.mp4"); }), QString("OUTSIDE_ROOT"));    // writing through it
        CHECK_EQ(codeOf([] { resolve("dangling.mp4"); }), QString("OUTSIDE_ROOT"));    // would create its target
    }
    CHECK_EQ(codeOf([] { resolve("@app"); }), QString("APP_OFF"));
    CHECK(!Cli::Sandbox::appAllowed());
    // Paths of another computer: drive letters and backslashes, any case; answers map back
    CHECK_EQ(resolve("Z:\\Clips\\sub\\a.mp4"), root + "/clips/sub/a.mp4");
    CHECK_EQ(resolve("z:/clips/b.mp4"), root + "/clips/b.mp4");
    CHECK_EQ(resolve("Z:\\Clips"), root + "/clips");
    CHECK_EQ(codeOf([] { resolve("/home/me/Videos/x.mp4"); }), QString("OUTSIDE_ROOT")); // mapped outside: still refused
    CHECK_EQ(Cli::Sandbox::mapBack(root + "/clips/sub/a.mp4"), QString("Z:\\Clips\\sub\\a.mp4"));
    CHECK_EQ(Cli::Sandbox::mapBack(root + "/clipsX/a.mp4"), root + "/clipsX/a.mp4");
    CHECK_EQ(Cli::Sandbox::mapBack(QJsonValue(QJsonObject{{"out", root + "/clips/r.mp4"}})).toObject()["out"].toString(),
             QString("Z:\\Clips\\r.mp4"));
    CHECK_EQ(Cli::Sandbox::relative(root + "/clips/sub/a.mp4"), QString("clips/sub/a.mp4"));
    CHECK(!Cli::Sandbox::relative(root).isNull());
    CHECK(Cli::Sandbox::relative(QDir(base).filePath("outside/x")).isNull());
    // Command parameters go through it: path parameters and the files inside edit ops
    const Cli::Command* probe = Cli::find("probe");
    CHECK_EQ(codeOf([&] { Cli::normalize(*probe, {{"file", "../outside/secret.mp4"}}); }), QString("OUTSIDE_ROOT"));
    CHECK_EQ(Cli::normalize(*probe, {{"file", "clips/a.mp4"}})["file"].toString(), root + "/clips/a.mp4");
    // A project naming a file outside: refused, also a missing one or one the engine would read through options,
    // a prefix or a link (MLT/FFmpeg open the string as written); offline media inside are fine
    {
        const auto projectWith = [](const QString& path) {
            auto p = std::make_unique<Project>();
            ProjectData d;
            d.timeline = emptyTimeline();
            p->load(d);
            p->addMedia({path, "x.mp4", 100, true, true, false});
            return p;
        };
        const auto check = [&](const QString& path) {
            const auto p = projectWith(path);
            return codeOf([&] { Cli::Sandbox::checkProject(*p); });
        };
        CHECK_EQ(check(root + "/clips/offline.mp4"), QString("none"));
        CHECK_EQ(check(root + "/clips/./sub//a.mp4"), QString("none"));
        CHECK_EQ(check(QDir(base).filePath("outside/secret.mp4")), QString("OUTSIDE_ROOT"));
        CHECK_EQ(check("/somewhere/else/offline.mp4"), QString("OUTSIDE_ROOT"));
        CHECK_EQ(check(QDir(base).filePath("outside/secret.mp4") + "?x"), QString("OUTSIDE_ROOT"));
        CHECK_EQ(check(root + "/clips/a.mp4?/../../../outside/secret.mp4"), QString("OUTSIDE_ROOT"));
        CHECK_EQ(check(root + "/clips/../clips/a.mp4"), QString("OUTSIDE_ROOT")); // ".." follows links on disk
        CHECK_EQ(check("avformat:" + QDir(base).filePath("outside/secret.mp4")), QString("OUTSIDE_ROOT"));
        CHECK_EQ(check("clips/a.mp4"), QString("OUTSIDE_ROOT")); // not a plain absolute path
        if (QFileInfo(root + "/link").isSymLink()) {
            CHECK_EQ(check(root + "/link/secret.mp4"), QString("OUTSIDE_ROOT"));
            CHECK_EQ(check(root + "/link/missing.mp4"), QString("OUTSIDE_ROOT"));
            CHECK_EQ(check(root + "/dangling.mp4"), QString("OUTSIDE_ROOT"));
        }
    }
    CHECK_EQ(codeOf([] { Cli::Sandbox::configure({"/no/such/folder"}); }), QString("BAD_ARGUMENT"));
    CHECK_EQ(codeOf([&] { Cli::Sandbox::configure({root, {"no-equals-sign"}}); }), QString("BAD_ARGUMENT"));
    Cli::Sandbox::configure({}); // back to normal for the rest of this process
    QDir::setCurrent(cwd);
    CHECK(Cli::Sandbox::appAllowed());
    CHECK_EQ(Cli::Sandbox::resolve("x.mp4"), QDir(cwd).filePath("x.mp4"));
}

// ---------- HTTP client ----------

struct Reply {
    int status = 0;
    QByteArray body;
    QHash<QByteArray, QByteArray> headers; // lower case names
    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

QNetworkAccessManager* net()
{
    static QNetworkAccessManager* n = new QNetworkAccessManager;
    return n;
}

QNetworkReply* send(const QByteArray& method, const QString& url, const QByteArray& body,
                    const QList<QPair<QByteArray, QByteArray>>& headers)
{
    QNetworkRequest req{QUrl(url)};
    for (const auto& [k, v] : headers) req.setRawHeader(k, v);
    return net()->sendCustomRequest(req, method, body);
}

Reply wait(QNetworkReply* r, int timeoutMs = 120000)
{
    if (!r->isFinished()) {
        QEventLoop loop;
        QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
    }
    Reply out;
    out.status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = r->readAll();
    for (const auto& [k, v] : r->rawHeaderPairs()) out.headers[k.toLower()] = v;
    if (!r->isFinished()) std::printf("       timeout: %s\n", qPrintable(r->url().toString()));
    r->deleteLater();
    return out;
}

Reply http(const QByteArray& method, const QString& url, const QByteArray& body = {},
           const QList<QPair<QByteArray, QByteArray>>& headers = {})
{
    return wait(send(method, url, body, headers));
}

// Status of a request sent as it is (QNetworkAccessManager cleans up "%2e%2e" in paths before sending)
int rawStatus(const QString& base, const QByteArray& method, const QByteArray& target, const QByteArray& body = {})
{
    const QUrl url(base);
    QTcpSocket s;
    s.connectToHost(url.host(), quint16(url.port()));
    if (!s.waitForConnected(5000)) return 0;
    s.write(method + ' ' + target + " HTTP/1.1\r\nHost: " + url.authority().toLatin1() + "\r\nAuthorization: Bearer "
            + kToken + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    QByteArray reply;
    QElapsedTimer t;
    t.start();
    while (!reply.contains("\r\n") && !t.hasExpired(10000))
        if (s.waitForReadyRead(100)) reply += s.readAll();
    return reply.split(' ').value(1).toInt();
}

// The JSON-RPC messages of a Server-Sent Events stream (or the one JSON answer)
QVector<QJsonObject> messages(const Reply& r)
{
    QVector<QJsonObject> out;
    if (!r.headers.value("content-type").startsWith("text/event-stream")) {
        out << r.json();
        return out;
    }
    for (const QByteArray& event : r.body.split('\n'))
        if (event.startsWith("data: ")) out << QJsonDocument::fromJson(event.mid(6)).object();
    return out;
}

// One schneidi-cli serve process
struct Server {
    QProcess p;
    QString base;  // http://127.0.0.1:port
    QString root;
    QByteArray token;
    QString session;

    bool start(const QString& rootDir, const QString& dataDir, const QStringList& extra, const QByteArray& tok,
               const QHash<QString, QString>& env = {})
    {
        root = rootDir;
        token = tok;
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.insert("XDG_DATA_HOME", dataDir + "/data");
        e.insert("XDG_CONFIG_HOME", dataDir + "/config");
        e.remove("SCHNEIDI_TOKEN");
        for (auto it = env.begin(); it != env.end(); ++it) e.insert(it.key(), it.value());
        p.setProcessEnvironment(e);
        p.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QStringList args{"serve", "--port", "0", "--root", rootDir};
        if (!tok.isEmpty()) args << "--token" << QString::fromLatin1(tok);
        p.start(SCHNEIDI_CLI, args + extra);
        QElapsedTimer t;
        t.start();
        while (!p.canReadLine() && p.state() != QProcess::NotRunning && !t.hasExpired(60000)) p.waitForReadyRead(100);
        const QJsonObject o = QJsonDocument::fromJson(p.readLine()).object();
        if (!o["ok"].toBool()) {
            std::printf("       server did not start: %s\n", QJsonDocument(o).toJson().constData());
            return false;
        }
        base = QString("http://127.0.0.1:%1").arg(o["port"].toInt());
        return true;
    }
    ~Server()
    {
        if (p.state() == QProcess::NotRunning) return;
        p.terminate(); // SIGTERM: ends cleanly, its workers with it
        if (!p.waitForFinished(15000)) {
            p.kill();
            p.waitForFinished(2000);
        }
    }
    QList<QPair<QByteArray, QByteArray>> headers(bool withSession = true) const
    {
        QList<QPair<QByteArray, QByteArray>> h{{"Content-Type", "application/json"},
                                               {"Accept", "application/json, text/event-stream"}};
        if (!token.isEmpty()) h << QPair<QByteArray, QByteArray>{"Authorization", "Bearer " + token};
        if (withSession && !session.isEmpty()) h << QPair<QByteArray, QByteArray>{"Mcp-Session-Id", session.toLatin1()};
        return h;
    }
    QNetworkReply* post(const QJsonObject& msg) const
    {
        return send("POST", base + "/mcp", QJsonDocument(msg).toJson(QJsonDocument::Compact), headers());
    }
    bool initialize()
    {
        const Reply r = http("POST", base + "/mcp",
                             R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18",)"
                             R"("capabilities":{},"clientInfo":{"name":"test","version":"1"}}})",
                             headers(false));
        session = QString::fromLatin1(r.headers.value("mcp-session-id"));
        http("POST", base + "/mcp", R"({"jsonrpc":"2.0","method":"notifications/initialized"})", headers());
        return r.status == 200 && !session.isEmpty();
    }
    QNetworkReply* startCall(const QString& tool, const QJsonObject& args, const QJsonValue& progressToken = {}) const
    {
        static int id = 100;
        QJsonObject params{{"name", tool}, {"arguments", args}};
        if (!progressToken.isUndefined()) params["_meta"] = QJsonObject{{"progressToken", progressToken}};
        return post({{"jsonrpc", "2.0"}, {"id", ++id}, {"method", "tools/call"}, {"params", params}});
    }
    // The tool's answer (text content as JSON); "ok": false with error for errors
    static QJsonObject answer(const Reply& r)
    {
        const QVector<QJsonObject> msgs = messages(r);
        const QJsonObject last = msgs.isEmpty() ? QJsonObject() : msgs.last();
        const QJsonArray content = last["result"]["content"].toArray();
        if (content.isEmpty()) {
            std::printf("       no tool answer: %d %s\n", r.status, r.body.left(300).constData());
            return {};
        }
        return QJsonDocument::fromJson(content.first()["text"].toString().toUtf8()).object();
    }
    QJsonObject call(const QString& tool, const QJsonObject& args) const { return answer(wait(startCall(tool, args))); }
};

QString errorCode(const QJsonObject& a) { return a["ok"].toBool() ? QString("ok") : a["error"]["code"].toString(); }

QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    for (char& c : b) c = char(QRandomGenerator::global()->generate() & 0xff);
    return b;
}

// State of the jobs of one tool call each, as `jobs` lists them
QStringList jobStates(const Server& s, const QStringList& tools)
{
    QStringList out;
    const QJsonArray jobs = s.call("jobs", {})["jobs"].toArray();
    for (const QString& t : tools)
        for (const QJsonValue& j : jobs)
            if (j["tool"].toString() == t && (j["state"] == "running" || j["state"] == "queued")) out << j["state"].toString();
    return out;
}

void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

int main(int argc, char* argv[])
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("serve");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg missing");
    QTemporaryDir tmp;
    sandboxRules(tmp.filePath("rules"));

    const QString root = tmp.filePath("root");
    QDir().mkpath(root);
    QDir().mkpath(tmp.filePath("outside"));
    const QString clip = Check::makeMedia(root + "/clip.mp4", {"-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25:duration=2",
                                                               "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
                                                               "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                                               "-shortest"});
    if (clip.isEmpty()) return Check::skip("cannot make a test clip");
    {
        // Old material: answers naming it get no download link (only files a call wrote)
        QFile f(clip);
        f.open(QIODevice::ReadWrite);
        f.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime);
    }
    QFile::copy(clip, tmp.filePath("outside/other.mp4"));
    QFile::link(tmp.filePath("outside"), root + "/link");
    const bool links = QFileInfo(root + "/link").isSymLink();

    // An empty token on the network would switch the check off: refused before anything listens
    {
        QProcess p;
        p.start(SCHNEIDI_CLI, {"serve", "--listen", "0.0.0.0", "--port", "0", "--root", root, "--token", ""});
        CHECK(p.waitForFinished(20000));
        CHECK_EQ(p.exitCode(), 2);
        CHECK(QJsonDocument::fromJson(p.readAllStandardOutput()).object()["error"].toObject()["message"].toString().contains("token"));
    }

    // ---------- Server 1: token, files, tools ----------
    {
        Server s;
        if (!CHECK(s.start(root, tmp.filePath("s1"), {"--workers", "2"}, kToken))) return Check::result();
        // Token on every request, compared as a whole
        CHECK_EQ(http("POST", s.base + "/mcp", "{}", {{"Content-Type", "application/json"}}).status, 401);
        CHECK_EQ(http("POST", s.base + "/mcp", "{}", {{"Authorization", "Bearer " + kToken.chopped(1) + "x"}}).status, 401);
        CHECK_EQ(http("GET", s.base + "/files/clip.mp4").status, 401);
        CHECK(http("POST", s.base + "/mcp", "{}", {{"Authorization", "Bearer " + kToken}}).status != 401);
        // A web page of another site: refused (DNS rebinding / cross-site), a local one is fine
        CHECK_EQ(http("POST", s.base + "/mcp", "{}", s.headers() << QPair<QByteArray, QByteArray>{"Origin", "http://evil.example"}).status, 403);
        // Session: from initialize, required afterwards, unknown = 404
        CHECK_EQ(http("POST", s.base + "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", s.headers()).status, 400);
        CHECK(s.initialize());
        Reply r = http("POST", s.base + "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                       s.headers() << QPair<QByteArray, QByteArray>{"MCP-Protocol-Version", "2025-06-18"});
        CHECK_EQ(r.status, 200);
        QStringList tools;
        for (const QJsonValue& t : r.json()["result"].toObject()["tools"].toArray()) tools << t["name"].toString();
        CHECK(tools.contains("render") && tools.contains("jobs") && tools.contains("files") && tools.contains("upload"));
        CHECK(!tools.contains("app") && !tools.contains("extensions"));
        CHECK_EQ(http("POST", s.base + "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})",
                      s.headers() << QPair<QByteArray, QByteArray>{"MCP-Protocol-Version", "1999-01-01"}).status, 400);
        CHECK_EQ(http("POST", s.base + "/mcp", R"({"jsonrpc":"2.0","method":"notifications/initialized"})", s.headers()).status, 202);
        CHECK_EQ(http("GET", s.base + "/mcp", {}, s.headers()).status, 405);
        CHECK_EQ(http("POST", s.base + "/mcp", "{not json", s.headers()).status, 400);
        {
            Server other;
            other.base = s.base;
            other.token = kToken;
            other.session = "no-such-session";
            CHECK_EQ(http("POST", s.base + "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})", other.headers()).status, 404);
            // An older client (2025-03-26) gets its version and may send several messages at once
            CHECK(other.initialize());
            r = http("POST", s.base + "/mcp", R"([{"jsonrpc":"2.0","id":7,"method":"ping"},{"jsonrpc":"2.0","id":8,"method":"tools/list"}])",
                     other.headers());
            const QJsonArray batch = QJsonDocument::fromJson(r.body).array();
            CHECK_EQ(batch.size(), qsizetype(2));
            CHECK_EQ(batch[0]["id"].toInt(), 7);
            CHECK(batch[1]["result"]["tools"].isArray());
            CHECK_EQ(http("DELETE", s.base + "/mcp", {}, other.headers()).status, 204);
            CHECK_EQ(http("POST", s.base + "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})", other.headers()).status, 404);
            other.base.clear(); // not ours to stop
        }

        // A project in the work folder, relative paths
        QJsonObject a = s.call("new", {{"project", "film.schneidi"}, {"media", QJsonArray{"clip.mp4"}}, {"width", 160}, {"height", 90}});
        CHECK_EQ(errorCode(a), QString("ok"));
        CHECK_EQ(a["project"].toString(), root + "/film.schneidi");
        a = s.call("edit", {{"project", "film.schneidi"}, {"ops", QJsonArray{QJsonObject{{"op", "append"}, {"media", "clip.mp4"}}}}});
        CHECK_EQ(errorCode(a), QString("ok"));
        a = s.call("info", {{"project", "film.schneidi"}, {"summary", true}});
        CHECK_EQ(a["timeline"].toObject()["clips"].toInt(), 2); // video and its sound
        // Nothing outside the work folder: .., absolute, symlink, inside edit ops, the app
        CHECK_EQ(errorCode(s.call("probe", {{"file", "../outside/other.mp4"}})), QString("OUTSIDE_ROOT"));
        CHECK_EQ(errorCode(s.call("probe", {{"file", tmp.filePath("outside/other.mp4")}})), QString("OUTSIDE_ROOT"));
        CHECK_EQ(errorCode(s.call("edit", {{"project", "film.schneidi"},
                                           {"ops", QJsonArray{QJsonObject{{"op", "append"}, {"media", "../outside/other.mp4"}}}}})),
                 QString("OUTSIDE_ROOT"));
        if (links) {
            CHECK_EQ(errorCode(s.call("probe", {{"file", "link/other.mp4"}})), QString("OUTSIDE_ROOT"));
            CHECK_EQ(errorCode(s.call("edit", {{"project", "film.schneidi"},
                                               {"ops", QJsonArray{QJsonObject{{"op", "append"}, {"media", "link/other.mp4"}}}}})),
                     QString("OUTSIDE_ROOT"));
        }
        CHECK_EQ(errorCode(s.call("render", {{"project", "film.schneidi"}, {"out", "../outside/x.mp4"}})), QString("OUTSIDE_ROOT"));
        CHECK(!QFileInfo::exists(tmp.filePath("outside/x.mp4")));
        CHECK_EQ(errorCode(s.call("info", {{"project", "@app"}})), QString("APP_OFF"));
        CHECK_EQ(errorCode(s.call("app", {{"action", "state"}})), QString("NOT_FOUND"));
        CHECK_EQ(s.call("probe", {{"file", "clip.mp4"}})["frames"].toInt(), 50);

        // Files: PUT streams into the work folder, GET (with a range) back out
        const QByteArray data = randomBytes(300000);
        r = http("PUT", s.base + "/files/in/sub/data.bin", data, {{"Authorization", "Bearer " + kToken}});
        CHECK_EQ(r.status, 201);
        CHECK_EQ(r.json()["path"].toString(), QString("in/sub/data.bin"));
        CHECK_EQ(QFileInfo(root + "/in/sub/data.bin").size(), qint64(data.size()));
        CHECK_EQ(http("PUT", s.base + "/files/in/sub/data.bin", data.left(10), {{"Authorization", "Bearer " + kToken}}).status, 200);
        CHECK_EQ(http("PUT", s.base + "/files/in/sub/data.bin", data, {{"Authorization", "Bearer " + kToken}, {"If-None-Match", "*"}}).status, 412);
        http("PUT", s.base + "/files/in/sub/data.bin", data, {{"Authorization", "Bearer " + kToken}});
        r = http("GET", s.base + "/files/in/sub/data.bin", {}, {{"Authorization", "Bearer " + kToken}});
        CHECK_EQ(r.status, 200);
        CHECK(r.body == data);
        r = http("GET", s.base + "/files/in/sub/data.bin", {}, {{"Authorization", "Bearer " + kToken}, {"Range", "bytes=1000-1999"}});
        CHECK_EQ(r.status, 206);
        CHECK(r.body == data.mid(1000, 1000));
        CHECK_EQ(r.headers.value("content-range"), QByteArray("bytes 1000-1999/300000"));
        r = http("GET", s.base + "/files/in/sub/data.bin", {}, {{"Authorization", "Bearer " + kToken}, {"Range", "bytes=-10"}});
        CHECK(r.body == data.right(10));
        CHECK_EQ(http("GET", s.base + "/files/in/sub/data.bin", {}, {{"Authorization", "Bearer " + kToken}, {"Range", "bytes=400000-"}}).status, 416);
        CHECK_EQ(http("GET", s.base + "/files/nope.bin", {}, {{"Authorization", "Bearer " + kToken}}).status, 404);
        CHECK_EQ(rawStatus(s.base, "GET", "/files/%2e%2e/outside/other.mp4"), 403);
        CHECK_EQ(rawStatus(s.base, "GET", "/files/../outside/other.mp4"), 403);
        if (links) CHECK_EQ(http("GET", s.base + "/files/link/other.mp4", {}, {{"Authorization", "Bearer " + kToken}}).status, 403);
        CHECK_EQ(rawStatus(s.base, "PUT", "/files/%2e%2e/outside/evil.bin", "x"), 403);
        CHECK(!QFileInfo::exists(tmp.filePath("outside/evil.bin")));
        CHECK_EQ(http("PUT", s.base + "/files/in", "x", {{"Authorization", "Bearer " + kToken}}).status, 409);
        CHECK_EQ(http("PUT", s.base + "/files/x.bin", "x").status, 401);
        CHECK(!QFileInfo::exists(root + "/x.bin"));

        // `files`: what is there (symlinks out of the work folder stay hidden)
        a = s.call("files", {{"recursive", true}});
        QHash<QString, QString> types;
        for (const QJsonValue& e : a["entries"].toArray()) types[e["path"].toString()] = e["type"].toString();
        CHECK_EQ(types.value("clip.mp4"), QString("video"));
        CHECK_EQ(types.value("film.schneidi"), QString("project"));
        CHECK_EQ(types.value("in/sub"), QString("folder"));
        CHECK_EQ(types.value("in/sub/data.bin"), QString("file"));
        CHECK(!types.contains("link") && !types.contains("link/other.mp4"));
        CHECK_EQ(errorCode(s.call("files", {{"path", "../outside"}})), QString("OUTSIDE_ROOT"));

        // Render: progress as Server-Sent Events when asked for, the answer with a download link
        r = wait(s.startCall("render", {{"project", "film.schneidi"}, {"out", "out/film.mp4"}}, "render-1"));
        CHECK(r.headers.value("content-type").startsWith("text/event-stream"));
        a = Server::answer(r);
        CHECK_EQ(errorCode(a), QString("ok"));
        CHECK_EQ(a["url"].toString(), s.base + "/files/out/film.mp4");
        for (const QJsonObject& m : messages(r))
            if (m["method"] == "notifications/progress") CHECK_EQ(m["params"]["progressToken"].toString(), QString("render-1"));
        r = http("GET", a["url"].toString(), {}, {{"Authorization", "Bearer " + kToken}});
        CHECK_EQ(r.status, 200);
        CHECK_EQ(r.body.mid(4, 4), QByteArray("ftyp"));
        CHECK_EQ(r.headers.value("content-type"), QByteArray("video/mp4"));
        // Without progress token: plain JSON
        r = wait(s.startCall("info", {{"project", "film.schneidi"}, {"summary", true}}));
        CHECK(r.headers.value("content-type").startsWith("application/json"));
        // frames into a folder: a link per image; source media in an answer get none
        a = s.call("frames", {{"source", "clip.mp4"}, {"at", QJsonArray{0, 25}}, {"out", "stills"}});
        CHECK_EQ(a["frames"].toArray().size(), qsizetype(2));
        CHECK(a["frames"][0]["url"].toString().startsWith(s.base + "/files/stills/"));
        CHECK(!s.call("probe", {{"file", "clip.mp4"}}).contains("url"));

        // ---------- Server 2: no token (this computer only), slow calls ----------
        Server slow;
        const QString root2 = tmp.filePath("root2");
        QDir().mkpath(root2);
        QFile::copy(clip, root2 + "/clip.mp4");
        if (!CHECK(slow.start(root2, tmp.filePath("s2"), {"--workers", "2"}, {},
                              {{"SCHNEIDI_TEST_CALL_DELAY_MS", "2000"}, {"SCHNEIDI_TEST_CRASH_TOOL", "scopes"}})))
            return Check::result();
        CHECK(slow.initialize());
        // Only local host names without a token
        CHECK_EQ(http("POST", slow.base + "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                      slow.headers() << QPair<QByteArray, QByteArray>{"Host", "rebind.example:80"}).status, 403);

        // Upload from a URL (here: a file of the other server; it needs no token) into server 1
        QFile::copy(clip, root2 + "/fetch me.mp4");
        a = s.call("upload", {{"url", slow.base + "/files/fetch%20me.mp4"}, {"path", "fetched/"}});
        CHECK_EQ(errorCode(a), QString("ok"));
        CHECK_EQ(a["path"].toString(), root + "/fetched/fetch me.mp4");
        CHECK_EQ(QFileInfo(root + "/fetched/fetch me.mp4").size(), QFileInfo(clip).size());
        CHECK_EQ(errorCode(s.call("upload", {{"url", slow.base + "/files/fetch%20me.mp4"}, {"path", "fetched"}})), QString("EXISTS"));
        CHECK_EQ(errorCode(s.call("upload", {{"url", slow.base + "/files/fetch%20me.mp4"}, {"path", "fetched/fetch me.mp4"}})),
                 QString("EXISTS"));
        CHECK_EQ(errorCode(s.call("upload", {{"url", slow.base + "/files/missing.mp4"}, {"path", "x.mp4"}})), QString("DOWNLOAD_FAILED"));
        CHECK_EQ(errorCode(s.call("upload", {{"url", "file:///etc/hostname"}})), QString("BAD_ARGUMENT"));
        CHECK_EQ(errorCode(s.call("upload", {{"url", slow.base + "/files/clip.mp4"}, {"path", "../outside/c.mp4"}})), QString("OUTSIDE_ROOT"));

        // Progress mid-call as an event, then the answer
        r = wait(slow.startCall("effects", {}, 7));
        const QVector<QJsonObject> msgs = messages(r);
        CHECK(msgs.size() >= 2);
        bool half = false;
        for (const QJsonObject& m : msgs)
            if (m["method"] == "notifications/progress") {
                CHECK_EQ(m["params"]["progressToken"].toInt(), 7);
                half = half || m["params"]["progress"].toInt() == 50;
            }
        CHECK(half);
        CHECK(msgs.last()["result"]["content"].isArray());
        // Two workers ready (started with the first calls)
        {
            QNetworkReply* x = slow.startCall("effects", {});
            QNetworkReply* y = slow.startCall("effects", {});
            wait(x);
            wait(y);
        }
        for (const char* name : {"a.schneidi", "b.schneidi"}) {
            Project p;
            ProjectData d;
            d.timeline = emptyTimeline();
            p.load(d);
            QString error;
            ProjectFile::save(p.data(), root2 + "/" + name, &error);
        }
        const QJsonObject appendClip{{"op", "append"}, {"media", "clip.mp4"}};
        // Changes to two projects: side by side
        {
            QNetworkReply* x = slow.startCall("edit", {{"project", "a.schneidi"}, {"ops", QJsonArray{appendClip}}});
            QNetworkReply* y = slow.startCall("edit", {{"project", "b.schneidi"}, {"ops", QJsonArray{appendClip}}});
            waitMs(700);
            CHECK_EQ(jobStates(slow, {"edit"}), QStringList({"running", "running"}));
            CHECK_EQ(errorCode(Server::answer(wait(x))), QString("ok"));
            CHECK_EQ(errorCode(Server::answer(wait(y))), QString("ok"));
        }
        // Two changes to one project: one after the other (both get through, none sees the project locked)
        {
            QNetworkReply* x = slow.startCall("edit", {{"project", "a.schneidi"}, {"ops", QJsonArray{appendClip}}});
            waitMs(200);
            QNetworkReply* y = slow.startCall("edit", {{"project", "a.schneidi"}, {"ops", QJsonArray{appendClip}}});
            waitMs(500);
            QStringList states = jobStates(slow, {"edit"});
            states.sort();
            CHECK_EQ(states, QStringList({"queued", "running"}));
            QElapsedTimer t;
            t.start();
            CHECK_EQ(errorCode(Server::answer(wait(x))), QString("ok"));
            CHECK_EQ(errorCode(Server::answer(wait(y))), QString("ok"));
            CHECK(t.elapsed() > 2000); // the second waited for the first
            ProjectData d;
            ProjectFile::load(root2 + "/a.schneidi", &d, nullptr);
            CHECK_EQ(d.timeline.video[0].clips.size(), qsizetype(3));
        }
        // Reading the project while it changes: not held up
        {
            QNetworkReply* x = slow.startCall("edit", {{"project", "b.schneidi"}, {"ops", QJsonArray{appendClip}}});
            waitMs(200);
            QNetworkReply* y = slow.startCall("info", {{"project", "b.schneidi"}});
            waitMs(500);
            CHECK_EQ(jobStates(slow, {"edit", "info"}), QStringList({"running", "running"}));
            wait(x);
            wait(y);
        }
        // A worker that crashes: the call fails, the next one gets a new worker
        CHECK_EQ(errorCode(slow.call("scopes", {{"source", "clip.mp4"}})), QString("WORKER_CRASHED"));
        CHECK_EQ(errorCode(slow.call("effects", {})), QString("ok"));
    }

    // ---------- Server 3: calls longer than the wait become jobs ----------
    {
        Server s;
        const QString root3 = tmp.filePath("root3");
        QDir().mkpath(root3);
        if (!CHECK(s.start(root3, tmp.filePath("s3"), {"--workers", "1", "--job-wait", "1"}, kToken,
                           {{"SCHNEIDI_TEST_CALL_DELAY_MS", "3000"}})))
            return Check::result();
        CHECK(s.initialize());
        QJsonObject a = s.call("effects", {});
        CHECK_EQ(a["state"].toString(), QString("running"));
        const QString job = a["job"].toString();
        CHECK(!job.isEmpty());
        CHECK(a["note"].toString().contains("jobs"));
        a = s.call("jobs", {{"job", job}, {"wait", 0}});
        CHECK_EQ(a["state"].toString(), QString("running"));
        // Waiting longer than --job-wait is cut to it: ask until it is over
        for (int i = 0; i < 20 && a["state"].toString() == "running"; ++i) a = s.call("jobs", {{"job", job}, {"wait", 10}});
        CHECK_EQ(a["state"].toString(), QString("done"));
        CHECK_EQ(a["job"].toString(), job);
        CHECK(a["effects"].isArray()); // the call's own answer
        // Cancel: a queued one never runs, a running one stops
        const QString running = s.call("effects", {})["job"].toString();
        const QString queued = s.call("effects", {})["job"].toString();
        CHECK_EQ(s.call("jobs", {{"job", queued}})["state"].toString(), QString("queued"));
        CHECK_EQ(s.call("jobs", {{"job", queued}, {"cancel", true}})["state"].toString(), QString("cancelled"));
        a = s.call("jobs", {{"job", running}, {"cancel", true}});
        CHECK_EQ(a["state"].toString(), QString("cancelled"));
        CHECK_EQ(a["error"].toObject()["code"].toString(), QString("CANCELLED"));
        CHECK_EQ(errorCode(s.call("jobs", {{"job", "job999-zzzz"}})), QString("NOT_FOUND"));
        a = s.call("jobs", {});
        CHECK(a["jobs"].toArray().size() >= 3);
        CHECK_EQ(a["workers"].toObject()["max"].toInt(), 1);
    }
    return Check::result();
}
