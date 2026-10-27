// Live connection (cli/Live.h): a server on a fake app in this process, schneidi-cli as a real second process.
// Reading commands see the app's (unsaved) project, edits change it as one undo step without touching the file,
// `app` controls playhead/selection/undo, one AI session at a time, the user can disconnect it, while the app waits
// for the user (restore dialog at start) AIs hear that and nothing runs. The app hears what every command did
// (added/changed/removed clips with the place of the removed ones, the step, failures) and, while an AI session is
// connected, its work outside the app (analyses: start, progress, end with a short result).
#include "cli/Live.h"
#include "core/EffectRegistry.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ProxyCleanup.h"
#include "engine/ProxyManager.h"

#include "check.h"

#include <QGuiApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
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
    QVector<LiveChange> changes;
    QVector<LiveActivity> activities;
    QStringList proxies;

    Project* liveProject() override { return &project; }
    QString liveProjectPath() const override { return path; }
    Selection* liveSelection() override { return &selection; }
    int livePlayhead() const override { return playhead; }
    bool livePlaying() const override { return playing; }
    void liveSeek(int frame) override { playhead = frame; }
    void livePlay(bool play) override { playing = play; }
    bool liveSave(const QString& to, QString* error) override
    {
        if (!to.isEmpty()) path = to;
        if (path.isEmpty()) return false;
        return ProjectFile::save(project.data(), path, error);
    }
    void liveGenerateProxies(const QStringList& files) override { proxies << files; }
    QJsonObject liveProxyState(const QString&) const override { return {{"state", "pending"}, {"percent", 0}}; }
    void liveConnectionChanged(bool connected, const QString& client) override
    {
        events << (connected ? "attach " + client : QString("detach"));
    }
    void liveChanged(const LiveChange& c) override
    {
        changes << c;
        flashed = c.added + c.changed;
    }
    void liveActivity(const LiveActivity& a) override { activities << a; }
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
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8()); // own proxy folder (clean_proxies deletes there)
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
    {
        // The app hears what changed: the right halves are new, the left ones changed, nothing removed
        if (CHECK(!fake.changes.isEmpty())) {
            const LiveChange& c = fake.changes.last();
            CHECK_EQ(c.command, QString("edit"));
            CHECK_EQ(c.step, QString("split, marker"));
            CHECK_EQ(c.added.size(), 2);
            CHECK_EQ(c.changed.size(), 2);
            CHECK(c.removed.isEmpty());
            CHECK(c.error.isEmpty());
            CHECK(fake.project.timeline().video[0].clips.last().id == c.added.first()
                  || fake.project.timeline().video[0].clips.last().id == c.added.last());
        }
        // Deleting: where the clip was (for its ghost in the timeline)
        const Clip right = fake.project.timeline().video[0].clips.last();
        fake.changes.clear();
        r = cli({"edit", fake.path, "--ops", QString(R"([{"op":"delete","clips":[%1],"linked":false}])").arg(right.id)});
        CHECK_EQ(code(r), QString("ok"));
        if (CHECK_EQ(fake.changes.size(), 1) && CHECK_EQ(fake.changes[0].removed.size(), 1)) {
            const LiveChange::Removed& gone = fake.changes[0].removed[0];
            CHECK_EQ(gone.id, right.id);
            CHECK_EQ(gone.start, right.start);
            CHECK_EQ(gone.end, right.end());
            CHECK(gone.track == (TrackRef{TrackKind::Video, 0}));
            CHECK_EQ(gone.sequence, fake.project.currentSequence());
            CHECK_EQ(gone.name, QString("a.mp4"));
        }
        CHECK_EQ(code(cli({"app", "undo"})), QString("ok"));
        CHECK_EQ(videoClips(fake.project), 2);
        if (CHECK(!fake.changes.isEmpty())) {
            CHECK(fake.changes.last().step.startsWith("undo: KI: delete"));
            CHECK_EQ(fake.changes.last().added, QVector<int>{right.id}); // back again
        }
        // A failing edit is a step too (with the reason), reading the app state is none
        fake.changes.clear();
        CHECK_EQ(code(cli({"edit", fake.path, "--ops", R"([{"op":"delete","clips":[999]}])"})), QString("NOT_FOUND"));
        CHECK_EQ(code(cli({"app", "state"})), QString("ok"));
        if (CHECK_EQ(fake.changes.size(), 1)) {
            CHECK_EQ(fake.changes[0].step, QString("delete"));
            CHECK(!fake.changes[0].error.isEmpty());
            CHECK(fake.changes[0].added.isEmpty() && fake.changes[0].changed.isEmpty());
        }
        // No analysis without an app session: a file of its own reports nothing
        CHECK(fake.activities.isEmpty());
        // Effect order in the app: two effects added and one moved in one edit = one undo step there
        const int left = fake.project.timeline().video[0].clips.first().id;
        const int steps = fake.project.undoStack()->index();
        r = cli({"edit", fake.path, "--ops",
                 QString(R"([{"op":"effect","clips":[%1],"effect":"blur"},{"op":"effect","clips":[%1],"effect":"color"},
                             {"op":"effect","clips":[%1],"effect":"color","move":"first"}])").arg(left)});
        CHECK_EQ(code(r), QString("ok"));
        const Clip* moved = TimelineOps::findClip(fake.project.timeline(), left);
        CHECK(moved && EffectRegistry::order(*moved) == (QStringList{"color", "blur"}));
        CHECK_EQ(fake.project.undoStack()->index(), steps + 1);
        CHECK_EQ(code(cli({"app", "undo"})), QString("ok"));
        CHECK(fake.project.timeline().video[0].clips.first().effects.isEmpty());
    }

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
    if (!ProxyManager::cacheDir().startsWith(tmp.path())) {
        // Windows ignores XDG_CACHE_HOME: never clean up the real proxy folder of whoever runs the tests
        std::printf("       clean_proxies skipped: the proxy folder is not in the test folder (%s)\n",
                    qPrintable(ProxyManager::cacheDir()));
    } else {
        // clean_proxies: lists the proxy files neither the app's project nor a recent one needs (dry run), deletes
        // them only with --delete; the app project's proxy stays
        QDir().mkpath(ProxyManager::cacheDir());
        auto write = [](const QString& path, int bytes) {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly)) f.write(QByteArray(bytes, 'x'));
        };
        const QString mine = ProxyManager::proxyPath("/x/a.mp4");
        write(mine, 100);
        const QString goneOriginal = tmp.path() + "/gone.mp4";
        write(goneOriginal, 10);
        const QString old = ProxyManager::proxyPath(goneOriginal);
        write(old, 300);
        ProxyCleanup::writeNote(old, goneOriginal, ProxyManager::kMaxSize);
        QFile::remove(goneOriginal);
        r = cli({"app", "clean_proxies"});
        CHECK_EQ(code(r), QString("ok"));
        CHECK(r["dry_run"].toBool());
        const QJsonArray files = r["files"].toArray();
        if (CHECK_EQ(files.size(), 1)) {
            CHECK_EQ(files[0].toObject()["file"].toString(), old);
            CHECK_EQ(files[0].toObject()["reason"].toString(), QString("missing"));
            CHECK_EQ(files[0].toObject()["original"].toString(), goneOriginal);
        }
        CHECK_EQ(r["kept"].toInt(), 1);
        CHECK(QFileInfo::exists(old));
        r = cli({"app", "clean_proxies", "--delete"});
        CHECK_EQ(code(r), QString("ok"));
        CHECK(r["freed_bytes"].toInteger() >= 300);
        CHECK_EQ(r["freed_bytes"].toInteger(), r["bytes"].toInteger());
        CHECK(!QFileInfo::exists(old));
        CHECK(!QFileInfo::exists(ProxyCleanup::notePath(old)));
        CHECK(QFileInfo::exists(mine));
    }
    CHECK_EQ(code(cli({"app", "redo"})), QString("ok"));
    CHECK_EQ(code(cli({"app", "save"})), QString("ok"));
    CHECK_EQ(fileVideoClips(fake.path), 2);
    {
        // `path` only names the file of a project that was never saved, and never overwrites
        CHECK_EQ(code(cli({"app", "save", "--path", tmp.path() + "/elsewhere.schneidi"})), QString("BAD_ARGUMENT"));
        const QString own = fake.path;
        fake.path.clear();
        CHECK_EQ(code(cli({"app", "save"})), QString("SAVE_FAILED"));
        CHECK_EQ(code(cli({"app", "save", "--path", own})), QString("EXISTS"));
        r = cli({"app", "save", "--path", tmp.path() + "/cut"});
        CHECK_EQ(code(r), QString("ok"));
        CHECK_EQ(r["saved"].toString(), tmp.path() + "/cut.schneidi");
        CHECK_EQ(fileVideoClips(tmp.path() + "/cut.schneidi"), 2);
        fake.path = own;
    }
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
    // ... or turned AI connections off in the settings: the AI hears why
    server.disconnectClient("AI connections are turned off in the settings");
    r = cli({"info", "@app"});
    CHECK_EQ(code(r), QString("APP_DISCONNECTED"));
    CHECK(r["error"].toObject()["message"].toString().contains("turned off in the settings"));
    server.setAllowed(true);
    r = cli({"info", fake.path});
    CHECK_EQ(code(r), QString("ok"));

    // The app waits for the user (restore dialog at start, before the project is open): AIs hear that instead of
    // "not running", nothing runs meanwhile; afterwards everything works
    {
        server.setWaiting("schneidi is waiting for the user (restore dialog)");
        r = cli({"info", "@app"});
        CHECK_EQ(code(r), QString("APP_WAITING"));
        CHECK(r["error"].toObject()["message"].toString().contains("restore dialog"));
        CHECK_EQ(code(cli({"app", "state"})), QString("APP_WAITING"));
        CHECK_EQ(code(cli({"info", fake.path})), QString("APP_WAITING"));
        const int clipsBefore = videoClips(fake.project);
        CHECK_EQ(code(cli({"edit", "@app", "--ops", R"([{"op":"split","at":60}])"})), QString("APP_WAITING"));
        CHECK_EQ(videoClips(fake.project), clipsBefore);
        CHECK(!server.attached());
        server.setWaiting({});
        CHECK_EQ(code(cli({"info", "@app"})), QString("ok"));
        CHECK_EQ(code(cli({"app", "state"})), QString("ok"));
    }

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
        // Work outside the app while the session is connected: the app sees it start and end with its result
        if (Check::haveFfmpeg()) {
            const QString talk = Check::makeMedia(tmp.path() + "/talk.wav",
                                                  {"-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-f", "lavfi",
                                                   "-i", "anullsrc=r=44100:cl=mono:d=1.5", "-f", "lavfi", "-i",
                                                   "sine=frequency=440:duration=1", "-filter_complex",
                                                   "[0][1][2]concat=n=3:v=0:a=1"});
            fake.activities.clear();
            mcp.write(QByteArray(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"silence","arguments":{"file":")")
                      + talk.toUtf8() + R"("}}})" "\n");
            mcp.write(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"probe","arguments":{"file":"/nowhere/x.mp4"}}})" "\n");
            for (int i = 0; i < 4000 && out.count('\n') < 5; ++i) {
                mcp.waitForReadyRead(5);
                out += mcp.readAllStandardOutput();
                QCoreApplication::processEvents();
            }
            for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QStringList seen;
            for (const LiveActivity& a : fake.activities)
                seen << QString("%1 %2").arg(a.tool).arg(int(a.state));
            const auto first = [&](const QString& tool, LiveActivity::State st) -> const LiveActivity* {
                for (const LiveActivity& a : fake.activities)
                    if (a.tool == tool && a.state == st) return &a;
                return nullptr;
            };
            const LiveActivity* start = first("silence", LiveActivity::State::Started);
            const LiveActivity* end = first("silence", LiveActivity::State::Finished);
            if (!CHECK(start && end)) std::printf("       activities: %s\n", qPrintable(seen.join(", ")));
            if (start && end) {
                CHECK_EQ(start->target, talk);
                CHECK_EQ(end->id, start->id);
                CHECK(end->summary.value("count").toInt() >= 1); // the 1.5 s pause
            }
            const LiveActivity* failed = first("probe", LiveActivity::State::Failed);
            CHECK(failed && !failed->error.isEmpty());
        }
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
        // A client asking past hello while the app waits: refused with the reason, not attached
        server.setWaiting("schneidi is waiting for the user (restore dialog)");
        QLocalSocket ok;
        ok.connectToServer(entry["server"].toString());
        CHECK(ok.waitForConnected(1000));
        ok.write(QByteArray(R"({"type":"hello","token":")") + entry["token"].toString().toUtf8() + "\"}\n"
                 + R"({"type":"attach","client":"raw"})" "\n");
        ok.flush();
        reply.clear();
        for (int i = 0; i < 200 && reply.count('\n') < 2; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            ok.waitForReadyRead(5);
            reply += ok.readAll();
        }
        CHECK(reply.contains("\"waiting\":\"schneidi is waiting"));
        CHECK(reply.contains("APP_WAITING"));
        CHECK(!server.attached());
        server.setWaiting({});

        // Work outside the app of two processes at once (an AI from outside and the app's own chat): both count their
        // jobs from 1, the app keeps them apart; a process that goes away ends its running work
        const auto talk = [](QLocalSocket& s, const QByteArray& lines, int answers) {
            s.write(lines);
            s.flush();
            QByteArray got;
            for (int i = 0; i < 200 && got.count('\n') < answers; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                s.waitForReadyRead(5);
                got += s.readAll();
            }
            return got;
        };
        const QByteArray job = R"({"type":"activity","id":1,"state":"start","tool":"silence"})" "\n";
        QLocalSocket outside, chat;
        outside.connectToServer(entry["server"].toString());
        chat.connectToServer(server.chatEnvironment().value(Cli::Live::ChatServerEnv));
        CHECK(outside.waitForConnected(1000) && chat.waitForConnected(1000));
        CHECK(talk(outside, QByteArray(R"({"type":"hello","token":")") + entry["token"].toString().toUtf8() + "\"}\n"
                                + R"({"type":"attach","client":"raw"})" "\n", 2).contains("\"ok\":true"));
        CHECK(talk(chat, QByteArray(R"({"type":"hello","token":")")
                             + server.chatEnvironment().value(Cli::Live::ChatSecretEnv).toUtf8() + "\"}\n"
                             + R"({"type":"attach","client":"chat"})" "\n", 2).contains("\"ok\":true"));
        fake.activities.clear();
        talk(outside, job, 0);
        talk(chat, job, 0);
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (CHECK_EQ(fake.activities.size(), qsizetype(2))) CHECK(fake.activities[0].id != fake.activities[1].id);
        const int chatJob = fake.activities.value(1).id;
        chat.disconnectFromServer();
        for (int i = 0; i < 50 && fake.activities.size() < 3; ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (CHECK_EQ(fake.activities.size(), qsizetype(3))) {
            CHECK_EQ(fake.activities[2].id, chatJob);
            CHECK(fake.activities[2].state == LiveActivity::State::Failed);
            CHECK_EQ(fake.activities[2].error, QString("interrupted"));
        }
        outside.disconnectFromServer();
        for (int i = 0; i < 50 && server.attached(); ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        // An AI that is done (its MCP process still alive) is let go after the idle limit: the app shows no AI any
        // more, another one may attach; its next request takes the app again if it is free, else APP_BUSY. Work it
        // reported keeps it attached.
        server.setIdleLimit(300);
        const auto idle = [](int ms) {
            QElapsedTimer t;
            t.start();
            while (!t.hasExpired(ms)) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        };
        const QByteArray hello = QByteArray(R"({"type":"hello","token":")") + entry["token"].toString().toUtf8() + "\"}\n";
        const QByteArray snap = R"({"type":"snapshot"})" "\n";
        QLocalSocket first, second;
        first.connectToServer(entry["server"].toString());
        second.connectToServer(entry["server"].toString());
        CHECK(first.waitForConnected(1000) && second.waitForConnected(1000));
        CHECK(talk(first, hello + R"({"type":"attach","client":"first"})" "\n", 2).contains("\"ok\":true"));
        CHECK(server.attached());
        idle(150);
        CHECK(talk(first, snap, 1).contains("\"ok\":true")); // a request restarts the time
        idle(200);
        CHECK(server.attached());
        idle(300);
        CHECK(!server.attached());
        CHECK_EQ(fake.events.last(), QString("detach"));
        CHECK(first.state() == QLocalSocket::ConnectedState);
        // Free again: the first one takes it with its next request, no new attach
        CHECK(talk(first, snap, 1).contains("\"ok\":true"));
        CHECK(server.attached());
        CHECK_EQ(server.client(), QString("first"));
        // Running work is not idle
        talk(first, R"({"type":"activity","id":7,"state":"start","tool":"render"})" "\n", 0);
        idle(500);
        CHECK(server.attached());
        talk(first, R"({"type":"activity","id":7,"state":"end","tool":"render"})" "\n", 0);
        idle(500);
        CHECK(!server.attached());
        // Never let go (Settings › AI › "Nie"): stays attached however long it is silent
        server.setIdleLimit(0);
        CHECK(talk(first, snap, 1).contains("\"ok\":true"));
        CHECK(server.attached());
        idle(500);
        CHECK(server.attached());
        server.setIdleLimit(300);
        idle(500);
        CHECK(!server.attached());
        // Another AI attaches meanwhile: the first one hears so
        CHECK(talk(second, hello, 1).contains("\"busy\":false"));
        CHECK(talk(second, R"({"type":"attach","client":"second"})" "\n", 1).contains("\"ok\":true"));
        const QByteArray busy = talk(first, snap, 1);
        CHECK(busy.contains("APP_BUSY") && busy.contains("idle"));
        CHECK_EQ(server.client(), QString("second"));
        second.disconnectFromServer();
        for (int i = 0; i < 50 && server.attached(); ++i) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(talk(first, snap, 1).contains("\"ok\":true"));
        CHECK_EQ(server.client(), QString("first"));
        // The user disconnects the AI while none is attached: the one let go is refused too
        idle(500);
        server.disconnectClient();
        CHECK(talk(first, snap, 1).contains("APP_DISCONNECTED"));
        CHECK(!server.attached());
        server.setAllowed(true);
        first.disconnectFromServer();
    }

    return Check::result();
}
