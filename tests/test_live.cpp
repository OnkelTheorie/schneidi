// Live connection (cli/Live.h): a server on a fake app in this process, schneidi-cli as a real second process.
// Reading commands see the app's (unsaved) project, edits change it as one undo step without touching the file,
// `app` controls playhead/selection/undo, one AI session at a time, the user can disconnect it.
#include "cli/Live.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include "check.h"

#include <QGuiApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QUndoStack>

namespace {

class FakeApp : public LiveHost {
public:
    Project project;
    Selection selection;
    QString path;
    int playhead = 0;
    bool playing = false;
    QStringList events; // "attach <client>", "detach"
    QVector<int> flashed;
    QStringList proxies;

    Project* liveProject() override { return &project; }
    QString liveProjectPath() const override { return path; }
    Selection* liveSelection() override { return &selection; }
    int livePlayhead() const override { return playhead; }
    bool livePlaying() const override { return playing; }
    void liveSeek(int frame) override { playhead = frame; }
    void livePlay(bool play) override { playing = play; }
    bool liveSave(QString* error) override { return ProjectFile::save(project.data(), path, error); }
    void liveGenerateProxies(const QStringList& files) override { proxies << files; }
    QJsonObject liveProxyState(const QString&) const override { return {{"state", "pending"}, {"percent", 0}}; }
    void liveConnectionChanged(bool connected, const QString& client) override
    {
        events << (connected ? "attach " + client : QString("detach"));
    }
    void liveClipsChanged(const QVector<int>& ids) override { flashed = ids; }
};

// Project with one 100-frame media file (not on disk: edits only need its length) on V1/A1
void setup(Project& p)
{
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    p.edit("setup", [&](Timeline& tl) {
        Clip v; v.id = p.newClipId(); v.mediaPath = "/x/a.mp4"; v.out = 99; v.linkId = p.newLinkId();
        Clip a = v; a.id = p.newClipId();
        tl.video[0].clips << v;
        tl.audio[0].clips << a;
    });
}

// Waits for a child process while the server in this process keeps answering. Not `while (!waitForFinished(5))`:
// when processEvents() reaps the finished child, waitForFinished() returns false for good ("already finished") and
// the loop never ends; happens when the child exits while this process is busy (under load: the old 300-s hang).
bool waitServing(QProcess& p, int timeoutMs = 120000)
{
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning) {
        if (t.hasExpired(timeoutMs)) {
            p.kill();
            p.waitForFinished(1000);
            return false;
        }
        p.waitForFinished(5);
        QCoreApplication::processEvents();
    }
    return true;
}

// schneidi-cli as its own process; the server here answers while it waits
QJsonObject cli(const QStringList& args, const QByteArray& stdinData = {})
{
    QProcess p;
    p.start(SCHNEIDI_CLI, args);
    if (!stdinData.isEmpty()) p.write(stdinData);
    p.closeWriteChannel();
    if (!waitServing(p)) std::printf("       timeout: schneidi-cli %s\n", qPrintable(args.join(' ')));
    QCoreApplication::processEvents();
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (o.isEmpty()) std::printf("       no JSON: %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

QString code(const QJsonObject& o)
{
    return o["ok"].toBool() ? QString("ok") : o["error"].toObject()["code"].toString();
}

int videoClips(const Project& p) { return p.timeline().video[0].clips.size(); }

int fileVideoClips(const QString& path)
{
    ProjectData d;
    ProjectFile::load(path, &d, nullptr);
    return d.timeline.video[0].clips.size();
}

} // namespace

int main(int argc, char* argv[])
{
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8()); // own registry of running apps (and backups)
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("schneidi");

    FakeApp fake;
    setup(fake.project);
    fake.path = tmp.path() + "/film.schneidi";
    QString error;
    CHECK(ProjectFile::save(fake.project.data(), fake.path, &error));
    fake.project.markSaved();
    Cli::Live::Server server(&fake);
    if (!CHECK(server.start(&error))) {
        std::printf("       %s\n", qPrintable(error));
        return Check::result();
    }

    // Reading: the app's project, with the app state
    fake.playhead = 12;
    QJsonObject r = cli({"info", fake.path});
    CHECK_EQ(code(r), QString("ok"));
    CHECK_EQ(r["app"].toObject()["playhead"].toInt(), 12);
    CHECK_EQ(fake.events, QStringList({"attach schneidi-cli", "detach"}));

    // Edit: in the app, one undo step, file untouched, changed clips flash
    const int undoBefore = fake.project.undoStack()->index();
    r = cli({"edit", fake.path, "--ops", R"([{"op":"split","at":50},{"op":"marker","at":10}])"});
    CHECK_EQ(code(r), QString("ok"));
    CHECK(r["live"].toBool());
    CHECK(!r["saved"].toBool());
    CHECK_EQ(videoClips(fake.project), 2);
    CHECK_EQ(fake.project.undoStack()->index(), undoBefore + 1);
    CHECK(fake.project.undoStack()->undoText().startsWith("KI: split, marker"));
    CHECK_EQ(fileVideoClips(fake.path), 1);
    CHECK(fake.flashed.size() >= 2); // both halves of V1 (and A1)

    // Reading sees the unsaved change
    r = cli({"info", fake.path});
    CHECK(QJsonDocument(r).toJson().contains("\"undo\""));

    // A failing op leaves the app project as it was (all or nothing)
    r = cli({"edit", fake.path, "--ops", R"([{"op":"split","at":20},{"op":"delete","clips":[999]}])"});
    CHECK_EQ(code(r), QString("NOT_FOUND"));
    CHECK_EQ(videoClips(fake.project), 2);
    CHECK_EQ(fake.project.undoStack()->index(), undoBefore + 1);
    // ... also what is not in the undo history: source markers and source In/Out
    r = cli({"edit", fake.path, "--ops",
             R"([{"op":"source_marker","media":"/x/a.mp4","at":30},{"op":"media","media":"/x/a.mp4","in":5,"out":60},
                 {"op":"delete","clips":[999]}])"});
    CHECK_EQ(code(r), QString("NOT_FOUND"));
    CHECK(fake.project.mediaInfo("/x/a.mp4")->markers.isEmpty());
    CHECK_EQ(fake.project.mediaInfo("/x/a.mp4")->markIn, -1);
    CHECK_EQ(fake.project.mediaInfo("/x/a.mp4")->markOut, -1);
    // ... and files it imported leave the Media Pool again
    const QString broll = Check::makeMedia(tmp.path() + "/broll.mkv",
                                           {"-f", "lavfi", "-i", "testsrc2=size=64x64:rate=25:duration=2", "-c:v", "ffv1"});
    if (CHECK(!broll.isEmpty())) {
        Check::initMlt(); // append reads the new file
        const int mediaBefore = fake.project.media().size();
        r = cli({"edit", fake.path, "--ops",
                 QString(R"([{"op":"append","media":"%1"},{"op":"delete","clips":[999]}])").arg(broll)});
        CHECK_EQ(code(r), QString("NOT_FOUND"));
        CHECK_EQ(fake.project.media().size(), mediaBefore);
        CHECK(!fake.project.mediaInfo(broll));
    }

    // Dry run: computed on a copy
    r = cli({"edit", fake.path, "--dry-run", "--ops", R"([{"op":"split","at":30}])"});
    CHECK_EQ(code(r), QString("ok"));
    CHECK_EQ(videoClips(fake.project), 2);

    // app: seek, select, play, undo, save, proxies; "@app" = the running app
    r = cli({"app", "seek", "--at", "40"});
    CHECK_EQ(code(r), QString("ok"));
    CHECK_EQ(fake.playhead, 40);
    CHECK_EQ(r["app"].toObject()["playhead"].toInt(), 40);
    const int first = fake.project.timeline().video[0].clips[0].id;
    r = cli({"app", "select", "--project", fake.path, "--clips", QString::number(first)});
    CHECK_EQ(code(r), QString("ok"));
    CHECK(fake.selection.contains(first));
    CHECK_EQ(code(cli({"app", "select", "--clips", "999"})), QString("NOT_FOUND"));
    cli({"app", "play"});
    CHECK(fake.playing);
    cli({"app", "stop"});
    CHECK(!fake.playing);
    r = cli({"app", "undo"});
    CHECK(r["undone"].toString().startsWith("KI: split"));
    CHECK_EQ(videoClips(fake.project), 1);
    r = cli({"app", "proxies"});
    CHECK_EQ(fake.proxies, QStringList({"/x/a.mp4"}));
    CHECK_EQ(r["proxies"].toArray().size(), 1);
    CHECK_EQ(code(cli({"app", "redo"})), QString("ok"));
    CHECK_EQ(code(cli({"app", "save"})), QString("ok"));
    CHECK_EQ(fileVideoClips(fake.path), 2);
    CHECK_EQ(code(cli({"info", "@app"})), QString("ok"));

    // backups/restore: "@app" is the app's file; restore replaces the timelines in the app as one undo step
    CHECK(!ProjectFile::backup(fake.path).isEmpty()); // state with 2 clips
    r = cli({"backups", "@app"});
    CHECK_EQ(code(r), QString("ok"));
    CHECK_EQ(r["backups"].toArray().size(), 1);
    CHECK_EQ(code(cli({"edit", "@app", "--ops", R"([{"op":"split","at":70}])"})), QString("ok"));
    CHECK_EQ(videoClips(fake.project), 3);
    r = cli({"restore", "@app"});
    CHECK_EQ(code(r), QString("ok"));
    CHECK(r["live"].toBool());
    CHECK_EQ(videoClips(fake.project), 2);
    CHECK_EQ(fake.project.undoStack()->undoText(), QString("KI: restore"));
    CHECK_EQ(code(cli({"app", "undo"})), QString("ok"));
    CHECK_EQ(videoClips(fake.project), 3);
    // The file path of the project open in the app uses the same connection (not "busy" with itself)
    CHECK_EQ(code(cli({"restore", fake.path})), QString("ok"));
    CHECK_EQ(videoClips(fake.project), 2);

    // The user disconnects the AI: refused until allowed again
    server.disconnectClient();
    CHECK_EQ(code(cli({"info", fake.path})), QString("APP_DISCONNECTED"));
    server.setAllowed(true);
    CHECK_EQ(code(cli({"info", fake.path})), QString("ok"));

    // One AI session at a time: an MCP server keeps its connection
    {
        QProcess mcp;
        mcp.start(SCHNEIDI_CLI, {"mcp"});
        mcp.write(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})" "\n");
        // "@app" first, then the file path of the same project: one connection, not busy with itself
        mcp.write(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"info","arguments":{"project":"@app"}}})" "\n");
        mcp.write(QByteArray(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"info","arguments":{"project":")")
                  + fake.path.toUtf8() + R"("}}})" "\n");
        QByteArray out;
        for (int i = 0; i < 2000 && out.count('\n') < 3; ++i) {
            mcp.waitForReadyRead(5);
            out += mcp.readAllStandardOutput();
            QCoreApplication::processEvents();
        }
        CHECK_EQ(out.count("\\\"ok\\\":true"), qsizetype(2));
        CHECK(!out.contains("APP_BUSY"));
        CHECK(server.attached());
        CHECK_EQ(server.client(), QString("schneidi-cli mcp"));
        CHECK_EQ(code(cli({"info", fake.path})), QString("APP_BUSY"));
        mcp.closeWriteChannel();
        CHECK(waitServing(mcp));
        for (int i = 0; i < 50 && server.attached(); ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(!server.attached());
    }

    // Other projects stay file based
    const QString other = tmp.path() + "/other.schneidi";
    {
        Project p;
        setup(p);
        CHECK(ProjectFile::save(p.data(), other, &error));
    }
    r = cli({"edit", other, "--ops", R"([{"op":"split","at":50}])"});
    CHECK(r["saved"].toBool());
    CHECK(!r.contains("live"));
    CHECK_EQ(fileVideoClips(other), 2);
    CHECK_EQ(code(cli({"app", "state", "--project", other})), QString("NO_APP"));

    // Wrong token: refused
    {
        QFile f(Cli::Live::registryDir() + QString("/%1.json").arg(QCoreApplication::applicationPid()));
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject entry = QJsonDocument::fromJson(f.readAll()).object();
        CHECK(!f.permissions().testFlag(QFile::ReadOther));
        QLocalSocket s;
        s.connectToServer(entry["server"].toString());
        CHECK(s.waitForConnected(1000));
        s.write(R"({"type":"hello","token":"nope"})" "\n");
        s.flush();
        QByteArray reply;
        for (int i = 0; i < 200 && !reply.contains('\n'); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            s.waitForReadyRead(5);
            reply += s.readAll();
        }
        CHECK(reply.contains("AUTH"));
    }

    return Check::result();
}
