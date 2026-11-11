// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli serve (cli/Serve.h): the work folder rules in this process (cli/Sandbox.h), then real server processes
// on a free port with a temporary work folder. Token, sessions, tools over HTTP (new/edit/info on a project), paths
// outside the work folder (.., absolute, symlink, inside edit ops), files up and down (Range), render with progress as
// Server-Sent Events and a download link, upload from a URL, long calls as jobs (wait, cancel), changes to one project
// one after another while other projects and reading calls run side by side, a crashed worker comes back.
// Slow calls without slow media: the workers' test hooks SCHNEIDI_TEST_CALL_DELAY_MS / SCHNEIDI_TEST_CRASH_TOOL
// (cli/Mcp.cpp).
#include "cli/Commands.h"
#include "cli/Mcp.h"
#include "cli/Sandbox.h"
#include "cli/ServeJobs.h"
#include "core/BrandKit.h"
#include "core/CompGraph.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

#include "check.h"

#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
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
    // Hidden files stay out for every tool path, read or written (".bashrc", ".config/autostart/…")
    const Cli::Command* exportTl = Cli::find("export_timeline");
    CHECK_EQ(codeOf([] { resolve(".bashrc"); }), QString("HIDDEN"));
    CHECK_EQ(codeOf([] { resolve("clips/.config/autostart/x.desktop"); }), QString("HIDDEN"));
    CHECK_EQ(codeOf([&] { Cli::normalize(*exportTl, {{"project", "a.schneidi"}, {"out", ".bashrc"}}); }), QString("HIDDEN"));
    CHECK_EQ(codeOf([&] { Cli::normalize(*probe, {{"file", ".hidden/key.mp4"}}); }), QString("HIDDEN"));
    // Files that make MLT/FFmpeg read other files or addresses (MLT XML, playlists) are no media here, by name or
    // content; other XML (a timeline of another program) is fine
    const auto write = [&](const QString& rel, const QByteArray& data) {
        QFile f(root + "/" + rel);
        if (f.open(QIODevice::WriteOnly)) f.write(data);
    };
    write("clips/list.mlt", "<mlt><producer><property name=\"resource\">/etc/passwd</property></producer></mlt>");
    write("clips/inner.xml", "\xef\xbb\xbf<?xml version=\"1.0\"?>\n<!-- x -->\n<mlt LC_NUMERIC=\"C\">\n</mlt>");
    write("clips/playlist.mp4", "#EXTM3U\n#EXTINF:1,\nhttp://169.254.169.254/latest\n");
    write("clips/fcp.xml", "<?xml version=\"1.0\"?>\n<xmeml version=\"5\"></xmeml>");
    for (const char* bad : {"clips/list.mlt", "clips/inner.xml", "clips/playlist.mp4"})
        CHECK_EQ(codeOf([&] { Cli::normalize(*probe, {{"file", bad}}); }), QString("BAD_MEDIA"));
    CHECK_EQ(codeOf([&] { Cli::normalize(*probe, {{"file", "clips/fcp.xml"}}); }), QString("none"));
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
        CHECK_EQ(check(root + "/clips/list.mlt"), QString("BAD_MEDIA"));
        CHECK_EQ(check(root + "/clips/playlist.mp4"), QString("BAD_MEDIA"));
        CHECK_EQ(check(root + "/.hidden/key.mp4"), QString("HIDDEN"));
        if (QFileInfo(root + "/link").isSymLink()) {
            CHECK_EQ(check(root + "/link/secret.mp4"), QString("OUTSIDE_ROOT"));
            CHECK_EQ(check(root + "/link/missing.mp4"), QString("OUTSIDE_ROOT"));
            CHECK_EQ(check(root + "/dangling.mp4"), QString("OUTSIDE_ROOT"));
        }
    }
    // Every file a project names, not only its media: a picture, LUT or texture of a compositing node, a title's or
    // subtitle style's texture picture, a wipe image, the project brand kit's files. Outside: refused; inside, a
    // built-in LUT or wipe: fine
    {
        const QString in = root + "/clips/pic.png", out = QDir(base).filePath("outside/pic.png");
        QFile(in).open(QIODevice::WriteOnly);
        QFile(out).open(QIODevice::WriteOnly);
        const auto check = [](const std::function<void(ProjectData&, Clip&)>& setup) {
            ProjectData d;
            d.timeline = emptyTimeline();
            Clip c;
            c.id = 1;
            c.kind = ClipKind::Title;
            c.out = 24;
            setup(d, c);
            d.timeline.video[0].clips << c;
            auto p = std::make_unique<Project>();
            p->load(d);
            return codeOf([&] { Cli::Sandbox::checkProject(*p); });
        };
        const auto node = [&](const QString& type, const QString& key, const QString& file) {
            return check([&](ProjectData&, Clip& c) {
                EffectInstance e;
                e.effectId = Comp::kEffectId;
                CompGraph g;
                g.nodes << CompNode{1, type, {}};
                g.out = 1;
                Comp::setGraph(e, g);
                e.params[CompGraph::paramKey(1, key)] = file;
                c.effects << e;
            });
        };
        CHECK_EQ(node(Comp::kMediaType, Comp::kPathKey, out), QString("OUTSIDE_ROOT")); // an image node
        CHECK_EQ(node(Comp::kMediaType, Comp::kPathKey, in), QString("none"));
        CHECK_EQ(node(Comp::kGradeType, Comp::kLutKey, out), QString("OUTSIDE_ROOT"));
        CHECK_EQ(node(Comp::kGradeType, Comp::kLutKey, ":/luts/edit.cube"), QString("none")); // built-in
        // Only a real built-in name: no way out of the resources, never for media
        const QString escape = ":/luts/../../.." + QDir(base).filePath("outside/secret.mp4");
        CHECK_EQ(node(Comp::kGradeType, Comp::kLutKey, escape), QString("OUTSIDE_ROOT"));
        CHECK_EQ(node(Comp::kMediaType, Comp::kPathKey, escape), QString("OUTSIDE_ROOT"));
        CHECK_EQ(check([&](ProjectData&, Clip& c) { c.transInStyle.luma = ":/lumas/../x"; }), QString("OUTSIDE_ROOT"));
        CHECK_EQ(check([&](ProjectData& d, Clip&) { d.brand.emplace().logo = ":/luts/edit.cube"; }), QString("OUTSIDE_ROOT"));
        CHECK_EQ(node(Comp::kTextType, Comp::kTextureKey, out), QString("OUTSIDE_ROOT"));
        CHECK_EQ(node(Comp::kTextType, Comp::kTextureKey, root + "/.hidden/key.png"), QString("HIDDEN"));
        CHECK_EQ(node(Comp::kTextType, "font", "/usr/share/fonts/x.ttf"), QString("none")); // a font name, no file
        const auto texture = [&](const QString& file) {
            return check([&](ProjectData&, Clip& c) { c.title.look.texture = file; });
        };
        CHECK_EQ(texture(out), QString("OUTSIDE_ROOT"));
        CHECK_EQ(texture(in), QString("none"));
        CHECK_EQ(texture("clips/pic.png"), QString("OUTSIDE_ROOT")); // not a plain absolute path, as for media
        CHECK_EQ(check([&](ProjectData& d, Clip&) {
                     SubtitleTrack t;
                     t.style.look.texture = out;
                     d.timeline.subtitles << t;
                 }),
                 QString("OUTSIDE_ROOT"));
        CHECK_EQ(check([&](ProjectData&, Clip& c) { c.transInStyle.luma = out; }), QString("OUTSIDE_ROOT"));
        CHECK_EQ(check([&](ProjectData&, Clip& c) { c.transOutStyle.luma = ":/lumas/clock"; }), QString("none"));
        for (QString BrandKit::*field : {&BrandKit::logo, &BrandKit::intro, &BrandKit::outro}) {
            CHECK_EQ(check([&](ProjectData& d, Clip&) { d.brand.emplace().*field = out; }), QString("OUTSIDE_ROOT"));
            CHECK_EQ(check([&](ProjectData& d, Clip&) { d.brand.emplace().*field = in; }), QString("none"));
        }
        // The server's own global kit is no exception: a project kit naming its files outside is refused (a template
        // saved from the project would copy them in); a project kit set from it (brand set global:false) leaves them out,
        // keeps those inside
        BrandKit global;
        global.logo = out;
        global.intro = in;
        CHECK(Brand::setGlobal(global));
        CHECK_EQ(check([&](ProjectData& d, Clip&) { d.brand.emplace().logo = out; }), QString("OUTSIDE_ROOT"));
        {
            ProjectData d;
            d.timeline = emptyTimeline();
            QString err;
            CHECK(ProjectFile::save(d, root + "/brand.schneidi", &err));
            const Cli::Command* brand = Cli::find("brand");
            Cli::Context ctx;
            CHECK_EQ(codeOf([&] {
                         brand->run(Cli::normalize(*brand, {{"action", "set"}, {"global", false},
                                                            {"project", "brand.schneidi"}, {"colors", "#ff0000"}}),
                                    ctx);
                     }),
                     QString("none"));
            ProjectData saved;
            if (CHECK(ProjectFile::load(root + "/brand.schneidi", &saved, &err)) && CHECK(saved.brand.has_value())) {
                CHECK(saved.brand->logo.isEmpty());
                CHECK_EQ(saved.brand->intro, in);
                CHECK_EQ(saved.brand->colors.size(), qsizetype(1));
            }
            CHECK_EQ(Brand::global().logo, out); // the server's own kit stays as it is
        }
        CHECK(Brand::setGlobal({}));
        // Commands that save a project without opening one check it too: `restore` of a backup naming a file outside,
        // `templates fill` without copying the template's files (they stay in the template library)
        {
            const auto run = [](const QString& name, const QJsonObject& args) {
                const Cli::Command* cmd = Cli::find(name);
                Cli::Context ctx;
                return codeOf([&] { cmd->run(Cli::normalize(*cmd, args), ctx); });
            };
            ProjectData d;
            d.timeline = emptyTimeline();
            Clip c;
            c.id = 1;
            c.kind = ClipKind::Title;
            c.out = 24;
            c.title.look.texture = out;
            d.timeline.video[0].clips << c;
            QString err;
            const QString file = root + "/restore.schneidi";
            CHECK(ProjectFile::save(d, file, &err));
            CHECK(!ProjectFile::backup(file).isEmpty());
            d.timeline.video[0].clips[0].title.look.texture = in;
            CHECK(ProjectFile::save(d, file, &err));
            CHECK_EQ(run("restore", {{"project", "restore.schneidi"}}), QString("OUTSIDE_ROOT"));
            ProjectData now;
            if (CHECK(ProjectFile::load(file, &now, &err)))
                CHECK_EQ(now.timeline.video[0].clips[0].title.look.texture, in); // nothing restored
            CHECK_EQ(run("templates", {{"fill", "intro-outro"}, {"out_project", "filled.schneidi"}, {"copy_media", false}}),
                     QString("OUTSIDE_ROOT"));
            CHECK(!QFileInfo::exists(root + "/filled.schneidi"));
            CHECK_EQ(run("templates", {{"fill", "intro-outro"}, {"out_project", "filled.schneidi"}}), QString("none"));
        }
        // In a timeline that is not the open one too
        CHECK_EQ(check([&](ProjectData& d, Clip&) {
                     Sequence open, other;
                     open.id = 1;
                     other.id = 7;
                     other.timeline = emptyTimeline();
                     Clip c;
                     c.id = 2;
                     c.kind = ClipKind::Title;
                     c.out = 24;
                     c.title.look.texture = out;
                     other.timeline.video[0].clips << c;
                     d.sequences << open << other;
                     d.currentSequence = 1;
                 }),
                 QString("OUTSIDE_ROOT"));
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
    QList<QPair<QByteArray, QByteArray>> extra; // on every request (Host behind a proxy)

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
        p.start(Check::cliPath(), args + extra);
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
        return h + extra;
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

void writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
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

    // Jobs that run in the server itself (upload) wait for their file like the others; ending or cancelling one never
    // frees the file of another job
    {
        using Cli::Serve::Job;
        Cli::Serve::Jobs jobs({}, tmp.path(), 1);
        QStringList ran;
        const auto make = [&](const QString& file) {
            auto j = std::make_unique<Job>();
            j->tool = "upload";
            j->writes = file;
            j->runHere = [&ran](Job* x) { ran << x->id; };
            return j;
        };
        Job* a = jobs.submit(make("/w/film.schneidi"));
        Job* b = jobs.submit(make("/w/film.schneidi"));
        Job* other = jobs.submit(make("/w/other.mp4"));
        CHECK(a->state == Job::State::Running && other->state == Job::State::Running);
        CHECK(b->state == Job::State::Queued);
        jobs.cancel(b); // queued: a keeps its file
        Job* c = jobs.submit(make("/w/film.schneidi"));
        CHECK(c->state == Job::State::Queued);
        jobs.end(a, Cli::Mcp::toolResult({}));
        CHECK(c->state == Job::State::Running);
        Job* d = jobs.submit(make("/w/film.schneidi"));
        c->cancelHere = [] {};
        jobs.cancel(c); // running (with its own cancel): frees the file for the next
        CHECK(d->state == Job::State::Running);
        CHECK_EQ(ran.size(), 4);
    }

    // A C++ exception in a tool (out of memory, a library) fails that call, the MCP server goes on
    {
        QProcess mcp;
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.insert("SCHNEIDI_TEST_THROW_TOOL", "templates");
        mcp.setProcessEnvironment(e);
        mcp.start(Check::cliPath(), {"mcp"});
        mcp.write(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"templates","arguments":{}}})" "\n"
                  R"({"jsonrpc":"2.0","id":2,"method":"ping"})" "\n");
        mcp.closeWriteChannel();
        CHECK(mcp.waitForFinished(30000));
        CHECK_EQ(mcp.exitCode(), 0);
        const QList<QByteArray> lines = mcp.readAllStandardOutput().trimmed().split('\n');
        if (CHECK_EQ(lines.size(), qsizetype(2))) {
            const QJsonObject first = QJsonDocument::fromJson(lines[0]).object();
            CHECK(first["result"].toObject()["isError"].toBool());
            CHECK(lines[0].contains("INTERNAL"));
            CHECK_EQ(QJsonDocument::fromJson(lines[1]).object()["id"].toInt(), 2);
        }
    }

    // Over stdio too: a batch gets one array of answers, a request without its method an error (it waited forever),
    // a JSON value that is no message no "parse error: no error occurred"
    {
        QProcess mcp;
        mcp.start(Check::cliPath(), {"mcp"});
        mcp.write(R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"},)"
                  R"({"jsonrpc":"2.0","id":2,"method":"tools/list"},7])" "\n"
                  R"({"jsonrpc":"2.0","id":3})" "\n"
                  R"({"jsonrpc":"2.0","id":4,"result":{}})" "\n"
                  "[]\n"
                  R"({"jsonrpc":"2.0","id":5,"method":"ping"})" "\n");
        mcp.closeWriteChannel();
        CHECK(mcp.waitForFinished(30000));
        const QList<QByteArray> lines = mcp.readAllStandardOutput().trimmed().split('\n');
        if (CHECK_EQ(lines.size(), qsizetype(4))) {
            const QJsonArray batch = QJsonDocument::fromJson(lines[0]).array();
            if (CHECK_EQ(batch.size(), qsizetype(3))) {
                CHECK_EQ(batch[0]["id"].toInt(), 1);
                CHECK(batch[1]["result"]["tools"].toArray().size() > 5);
                CHECK_EQ(batch[2]["error"]["code"].toInt(), -32600);
            }
            const QJsonObject noMethod = QJsonDocument::fromJson(lines[1]).object();
            CHECK(noMethod["id"].toInt() == 3 && noMethod["error"]["code"].toInt() == -32600);
            CHECK(lines[2].contains("empty batch") && !lines[2].contains("no error occurred"));
            CHECK_EQ(QJsonDocument::fromJson(lines[3]).object()["id"].toInt(), 5);
        }
    }

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

    // An option given twice: a list takes both (it kept only the last: `frames --at 0 --at 1s` showed one picture),
    // anything else is refused instead of silently taking the last
    {
        QProcess p;
        p.start(Check::cliPath(), {"frames", "--source", clip, "--at", "0", "--at", "1s", "--width", "64", "--out",
                               tmp.filePath("twice")});
        CHECK(p.waitForFinished(60000));
        CHECK_EQ(QJsonDocument::fromJson(p.readAllStandardOutput().trimmed().split('\n').last())
                     .object()["frames"].toArray().size(), qsizetype(2));
        QProcess q;
        q.start(Check::cliPath(), {"frames", "--source", clip, "--width", "64", "--width", "32", "--out", tmp.filePath("twice2")});
        CHECK(q.waitForFinished(60000));
        CHECK(q.exitCode() != 0 && q.readAllStandardOutput().contains("--width is given twice"));
    }

    // An empty token on the network would switch the check off: refused before anything listens
    {
        QProcess p;
        p.start(Check::cliPath(), {"serve", "--listen", "0.0.0.0", "--port", "0", "--root", root, "--token", ""});
        CHECK(p.waitForFinished(20000));
        CHECK_EQ(p.exitCode(), 2);
        CHECK(QJsonDocument::fromJson(p.readAllStandardOutput()).object()["error"].toObject()["message"].toString().contains("token"));
    }

    // Behind a reverse proxy (--public-url) everyone comes from this computer: a token anyway (made and kept)
    {
        Server proxied;
        if (CHECK(proxied.start(root, tmp.filePath("s0"), {"--public-url", "https://edit.example"}, {}))) {
            CHECK_EQ(http("POST", proxied.base + "/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                          proxied.headers() << QPair<QByteArray, QByteArray>{"Host", "edit.example"}).status, 401);
            CHECK_EQ(http("GET", proxied.base + "/files/clip.mp4").status, 401);
        }
    }
    // The generated token goes into a new file that is owner-only before the token is in it: one that was readable by
    // others (and opened by them) never shows it (the token was written into the old file, then made owner-only)
    {
        const QString data = tmp.filePath("s0t");
        {
            Server first;
            CHECK(first.start(root, data, {"--public-url", "https://edit.example"}, {}));
        }
        QString tokenPath;
        QDirIterator it(data, {"serve-token"}, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) tokenPath = it.next();
        if (CHECK(!tokenPath.isEmpty())) {
            const auto others = QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther;
            CHECK(QFile::resize(tokenPath, 0)); // empty: a new token is made
            CHECK(QFile::setPermissions(tokenPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther));
#ifndef Q_OS_WIN // Windows cannot replace a file another program has open
            QFile before(tokenPath);
            CHECK(before.open(QIODevice::ReadOnly));
#endif
            Server second;
            CHECK(second.start(root, data, {"--public-url", "https://edit.example"}, {}));
#ifndef Q_OS_WIN
            CHECK(before.readAll().isEmpty());
#endif
            QFile now(tokenPath);
            CHECK(now.open(QIODevice::ReadOnly) && now.readLine().trimmed().size() >= 16);
            CHECK(!(QFileInfo(tokenPath).permissions() & others));
        }
    }
    // ... and its clients are elsewhere although it listens on 127.0.0.1: `upload` must not fetch from this computer
    // for them (the server itself, other local services)
    {
        Server proxied;
        if (CHECK(proxied.start(root, tmp.filePath("s0b"), {"--public-url", "https://edit.example"}, kToken))) {
            proxied.extra = {{"Host", "edit.example"}};
            CHECK(proxied.initialize());
            const QJsonObject a = proxied.call("upload", {{"url", proxied.base + "/files/clip.mp4"}, {"path", "self.mp4"}});
            CHECK_EQ(errorCode(a), QString("BAD_URL"));
            CHECK(!QFileInfo::exists(root + "/self.mp4"));
        }
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
        // A request (id) with an empty or not-string method gets an error answer, not a connection that hangs forever
        for (const QByteArray body : {QByteArray(R"({"jsonrpc":"2.0","id":5,"method":""})"),
                                      QByteArray(R"({"jsonrpc":"2.0","id":5,"method":5})"),
                                      QByteArray(R"([{"jsonrpc":"2.0","id":5,"method":null},{"jsonrpc":"2.0","id":6,"method":"ping"}])")}) {
            const Reply bad = wait(send("POST", s.base + "/mcp", body, s.headers()), 10000);
            CHECK_EQ(bad.status, 200);
            CHECK(bad.body.contains("-32600"));
        }
        // A key with U+0000 (Qt may read "id\u0000" as "id", found by fuzz_mcp): refused here and on stdio
        CHECK_EQ(http("POST", s.base + "/mcp", R"({"jsonrpc":"2.0","id\u0000":1,"method":"ping"})", s.headers()).status, 400);
        {
            QJsonObject answer;
            const QJsonObject msg = QJsonDocument::fromJson(R"({"jsonrpc":"2.0","id":7,"method":"tools/call",
                                                               "params":{"name":"probe","arguments":{"file\u0000":"x"}}})").object();
            CHECK(Cli::Mcp::handle(msg, {}, {}, [&answer](const QJsonObject& a) { answer = a; }));
            CHECK(answer.value("id").isNull() && answer.value("error").toObject().value("code").toInt() == -32600);
        }
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
        // A name no file system takes: a client error that says so, not 500 "cannot write"
        {
            const QString longName = QString(300, 'a') + ".bin";
            const auto tooLong = http("PUT", s.base + "/files/in/" + longName, "x", {{"Authorization", "Bearer " + kToken}});
            CHECK_EQ(tooLong.status, 400);
            CHECK(tooLong.body.contains("NAME_TOO_LONG"));
            CHECK_EQ(http("PUT", s.base + "/files/" + QString(256, 'b') + "/x.bin", "x",
                          {{"Authorization", "Bearer " + kToken}}).status, 400);
            CHECK_EQ(http("PUT", s.base + "/files/in/" + QString(255, 'c'), "x", {{"Authorization", "Bearer " + kToken}}).status,
                     201); // 255 bytes still go
            CHECK_EQ(errorCode(s.call("upload", {{"url", "https://example.com/a.mp4"}, {"path", "in/" + longName}})),
                     QString("NAME_TOO_LONG"));
        }
        CHECK_EQ(http("PUT", s.base + "/files/x.bin", "x").status, 401);
        // Hidden files of the work folder (a home folder's .ssh …) are not served, not written
        QDir(root).mkpath(".hidden");
        QFile(root + "/.hidden/key").open(QIODevice::WriteOnly);
        CHECK_EQ(http("GET", s.base + "/files/.hidden/key", {}, {{"Authorization", "Bearer " + kToken}}).status, 403);
        CHECK_EQ(http("PUT", s.base + "/files/.ssh/authorized_keys", "x", {{"Authorization", "Bearer " + kToken}}).status, 403);
        CHECK(!QFileInfo::exists(root + "/.ssh"));
        CHECK_EQ(errorCode(s.call("files", {{"path", ".hidden"}})), QString("HIDDEN"));
        // An early error answer while the body still comes in: one answer, not a second one for the body
        {
            const QUrl url(s.base);
            QTcpSocket raw;
            raw.connectToHost(url.host(), quint16(url.port()));
            CHECK(raw.waitForConnected(5000));
            raw.write("POST /mcp HTTP/1.1\r\nHost: " + url.authority().toLatin1()
                      + "\r\nContent-Type: application/json\r\nContent-Length: 2000000\r\n\r\n" + QByteArray(1000, ' '));
            QByteArray got;
            QElapsedTimer t;
            t.start();
            while (!t.hasExpired(1500))
                if (raw.waitForReadyRead(100)) got += raw.readAll();
            CHECK_EQ(got.count("HTTP/1.1 "), qsizetype(1));
            CHECK(got.startsWith("HTTP/1.1 401"));
        }
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
        // Files a project names besides its media: a picture in a compositing node outside the work folder (a project
        // file written elsewhere) is refused, one inside works; a title's texture picture from an edit op likewise
        {
            QImage pic(16, 16, QImage::Format_RGB32);
            pic.fill(Qt::red);
            CHECK(pic.save(tmp.filePath("outside/pic.png")) && pic.save(root + "/pic.png"));
            const auto withImageNode = [&](const QString& file) {
                ProjectData d;
                QString err;
                if (!ProjectFile::load(root + "/film.schneidi", &d, &err) || d.timeline.video.isEmpty()
                    || d.timeline.video[0].clips.isEmpty())
                    return false;
                Clip& c = d.timeline.video[0].clips[0];
                c.effects.clear();
                EffectInstance e;
                e.effectId = Comp::kEffectId;
                CompGraph g;
                g.nodes << CompNode{1, Comp::kMediaType, {}};
                g.out = 1;
                Comp::setGraph(e, g);
                e.params[CompGraph::paramKey(1, Comp::kPathKey)] = file;
                c.effects << e;
                return ProjectFile::save(d, root + "/film.schneidi", &err);
            };
            const QJsonObject still{{"source", "film.schneidi"}, {"at", QJsonArray{0}}, {"out", "comp-stills"}};
            CHECK(withImageNode(tmp.filePath("outside/pic.png")));
            CHECK_EQ(errorCode(s.call("frames", still)), QString("OUTSIDE_ROOT"));
            CHECK(withImageNode(root + "/pic.png"));
            CHECK_EQ(errorCode(s.call("frames", still)), QString("ok"));
            CHECK(withImageNode({}));
            const auto title = [](const QString& texture) {
                return QJsonArray{QJsonObject{{"op", "title"}, {"text", "Hi"}, {"at", 0}, {"texture", texture}}};
            };
            CHECK_EQ(errorCode(s.call("edit", {{"project", "film.schneidi"}, {"ops", title("../outside/pic.png")}})),
                     QString("OUTSIDE_ROOT"));
            CHECK_EQ(errorCode(s.call("edit", {{"project", "film.schneidi"}, {"ops", title(tmp.filePath("outside/pic.png"))}})),
                     QString("OUTSIDE_ROOT"));
            CHECK_EQ(errorCode(s.call("edit", {{"project", "film.schneidi"}, {"ops", title("pic.png")}})), QString("ok"));
            ProjectData d;
            QString err;
            QStringList textures;
            if (CHECK(ProjectFile::load(root + "/film.schneidi", &d, &err)))
                for (const Track& t : d.timeline.video)
                    for (const Clip& c : t.clips)
                        if (c.isTitle()) textures << c.title.look.texture;
            CHECK_EQ(textures, QStringList{root + "/pic.png"});
        }

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
        // A job: running, or still queued while a busy machine starts the worker process
        const auto unfinished = [](const QJsonObject& o) {
            return o["state"].toString() == "running" || o["state"].toString() == "queued";
        };
        QJsonObject a = s.call("effects", {});
        CHECK(unfinished(a));
        const QString job = a["job"].toString();
        CHECK(!job.isEmpty());
        CHECK(a["note"].toString().contains("jobs"));
        a = s.call("jobs", {{"job", job}, {"wait", 0}});
        CHECK(unfinished(a));
        // Waiting longer than --job-wait is cut to it: ask until it is over
        for (int i = 0; i < 30 && unfinished(a); ++i) a = s.call("jobs", {{"job", job}, {"wait", 10}});
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

    // ---------- Server 4: a render cancelled while it writes leaves no half written file ----------
    {
        Server s;
        const QString root4 = tmp.filePath("root4");
        QDir().mkpath(root4 + "/out");
        QFile::copy(clip, root4 + "/clip.mp4");
        const QString temp4 = tmp.filePath("temp4"); // the workers' temp folder (GIF intermediate)
        QDir().mkpath(temp4);
        if (!CHECK(s.start(root4, tmp.filePath("s4"), {"--workers", "1", "--job-wait", "1"}, kToken,
                           {{"TMPDIR", temp4}, {"TMP", temp4}, {"TEMP", temp4}})))
            return Check::result();
        CHECK(s.initialize());
        CHECK_EQ(errorCode(s.call("new", {{"project", "long.schneidi"}, {"media", QJsonArray{"clip.mp4"}}})), QString("ok"));
        QJsonArray ops;
        for (int i = 0; i < 60; ++i) ops << QJsonObject{{"op", "append"}, {"media", "clip.mp4"}};
        CHECK_EQ(errorCode(s.call("edit", {{"project", "long.schneidi"}, {"ops", ops}})), QString("ok"));
        writeFile(root4 + "/out/long.mp4", "an older render");
        const auto parts = [&] {
            QStringList l = QDir(root4 + "/out").entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
            l.removeAll("long.mp4");
            l.removeAll("long.0badf00d.part.mp4");
            return l;
        };
        const QString job = s.call("render", {{"project", "long.schneidi"}, {"out", "out/long.mp4"}, {"overwrite", true},
                                              {"encoder", "software"}, {"threads", 1}})["job"].toString();
        QElapsedTimer t;
        t.start();
        while (parts().isEmpty() && !t.hasExpired(60000)) waitMs(100); // writing started
        if (CHECK(!job.isEmpty() && !parts().isEmpty())) {
            // Another job writing to the same target meanwhile (made after this one started): its file stays
            writeFile(root4 + "/out/long.0badf00d.part.mp4", "another job's render");
            CHECK_EQ(s.call("jobs", {{"job", job}, {"cancel", true}})["state"].toString(), QString("cancelled"));
            t.restart();
            while (!parts().isEmpty() && !t.hasExpired(10000)) waitMs(100);
            if (!CHECK(parts().isEmpty())) std::printf("       left: %s\n", qPrintable(parts().join(", ")));
            CHECK_EQ(readFile(root4 + "/out/long.mp4"), QByteArray("an older render"));
            CHECK_EQ(readFile(root4 + "/out/long.0badf00d.part.mp4"), QByteArray("another job's render"));
        }
        // A GIF render cancelled while it writes its intermediate: that goes too
        const auto intermediates = [&] { return QDir(temp4).entryList({"schneidi-gif-*"}, QDir::Files); };
        const QString gif = s.call("render", {{"project", "long.schneidi"}, {"out", "out/long.gif"}, {"format", "gif"},
                                              {"height", 120}, {"threads", 1}})["job"].toString();
        t.restart();
        while (intermediates().isEmpty() && !t.hasExpired(60000)) waitMs(100);
        if (CHECK(!gif.isEmpty() && !intermediates().isEmpty())) {
            CHECK_EQ(s.call("jobs", {{"job", gif}, {"cancel", true}})["state"].toString(), QString("cancelled"));
            t.restart();
            while (!intermediates().isEmpty() && !t.hasExpired(10000)) waitMs(100);
            if (!CHECK(intermediates().isEmpty())) std::printf("       left: %s\n", qPrintable(intermediates().join(", ")));
        }
    }
    return Check::result();
}
