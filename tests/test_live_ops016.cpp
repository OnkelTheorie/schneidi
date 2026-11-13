// SPDX-License-Identifier: AGPL-3.0-only
// The 0.16 edit ops live in the app: each `edit` call is one undo step, undo gives back exactly the project before,
// and the result matches the same ops run on the project file. While the user's hand-over (`schneidi <file>`) asks its
// "save changes?" question, an AI command waits (it would run inside the question's event loop and be lost or saved).
#include "cli/Live.h"
#include "core/AppPaths.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include "check.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QUndoStack>

#include <functional>

namespace {

class FakeApp : public LiveHost {
public:
    Project project;
    Selection selection;
    QString path;
    Project* liveProject() override { return &project; }
    QString liveProjectPath() const override { return path; }
    Selection* liveSelection() override { return &selection; }
    int livePlayhead() const override { return 0; }
    bool livePlaying() const override { return false; }
    void liveSeek(int) override {}
    void livePlay(bool) override {}
    bool liveSave(const QString& to, QString* error) override
    {
        if (!to.isEmpty()) path = to;
        return ProjectFile::save(project.data(), path, error);
    }
    void liveGenerateProxies(const QStringList&) override {}
    QJsonObject liveProxyState(const QString&) const override { return {}; }
    void liveConnectionChanged(bool, const QString&) override {}
    void liveChanged(const LiveChange&) override {}
    void liveActivity(const LiveActivity&) override {}
    // The user's hand-over: a modal "save changes?" question (its own event loop) while an AI sends an edit
    std::function<void()> duringQuestion;
    bool liveOpen(const QStringList&, bool byUser, QString*) override
    {
        if (byUser && duringQuestion) duringQuestion();
        return true; // the user kept the project
    }
};

void setup(Project& p)
{
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    // Made-up media, absolute as an op makes its path (Windows: with the drive)
    p.addMedia({Check::absPath("/x/a.mp4"), "a.mp4", 300, true, true, false});
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = p.newClipId();
        v.mediaPath = Check::absPath("/x/a.mp4");
        v.out = 199;
        v.linkId = p.newLinkId();
        Clip a = v;
        a.id = p.newClipId();
        tl.video[0].clips << v;
        tl.audio[0].clips << a;
    });
}

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

QJsonObject cli(const QStringList& args, const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment())
{
    QProcess p;
    p.setProcessEnvironment(env);
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    if (!waitServing(p)) std::printf("       timeout: schneidi-cli %s\n", qPrintable(args.join(' ')));
    QCoreApplication::processEvents();
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (!o["ok"].toBool()) std::printf("       %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

// The project as JSON without the id counters (they go on counting on undo) and the current timeline
QByteArray state(const Project& p)
{
    Project q; // seen from the first timeline: which one the app shows is no part of the edit
    q.load(p.data());
    q.setCurrentSequence(q.sequences().first().id);
    ProjectData d = q.data();
    d.lastClipId = d.lastLinkId = d.lastSequenceId = 0;
    return ProjectFile::toJson(d, "/x/film.schneidi");
}

} // namespace

int main(int argc, char* argv[])
{
    Check::isolateSettings(); // never the user's settings (start template, import presets), nor in schneidi-cli
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    QGuiApplication app(argc, argv);
    AppPaths::setProgramNames(); // the program's folders (also on Windows: with its organization)

    FakeApp fake;
    setup(fake.project);
    fake.path = tmp.path() + "/film.schneidi";
    QString error;
    CHECK(ProjectFile::save(fake.project.data(), fake.path, &error));
    fake.project.markSaved();
    const QString copy = tmp.path() + "/copy.schneidi";
    CHECK(QFile::copy(fake.path, copy));
    Cli::Live::Server server(&fake);
    if (!CHECK(server.start(&error))) return Check::result();
    {
        // The registry entry holds the token: only the user may read it
        const QFileInfo entry(QString("%1/%2.json").arg(Cli::Live::registryDir()).arg(QCoreApplication::applicationPid()));
        CHECK(entry.filePath().startsWith(tmp.path() + "/data/"));
        CHECK(entry.exists());
#ifndef Q_OS_WIN // no group/other bits there: Qt reports them from the read-only flag alone (the profile's ACL protects AppData)
        CHECK(!(entry.permissions() & (QFile::ReadGroup | QFile::ReadOther)));
#endif
    }
    // The file copy is edited without the app (no live connection for it: another path)
    QProcessEnvironment noLive = QProcessEnvironment::systemEnvironment();
    noLive.insert("XDG_DATA_HOME", tmp.path() + "/other"); // no registry: no running app

    const QStringList calls{
        R"([{"op":"new_timeline","name":"S","from":"Timeline 1","aspect":"9:16","reframe":"center","switch":false}])",
        R"([{"op":"format","timeline":"S","aspect":"1:1"}])",
        R"([{"op":"title","kind":"counter","from":0,"to":50,"at":10,"duration":40}])",
        R"([{"op":"captions","words":[{"from":5,"to":15,"text":"hi"},{"from":20,"to":30,"text":"there"}]}])",
        R"([{"op":"speed","clips":[1],"speed":0.5,"interpolate":"blend"}])",
        R"([{"op":"media","media":"/x/a.mp4","picture_aspect":"4:3"}])",
        R"([{"op":"picture_aspect","aspect":"4:3"}])",
        R"([{"op":"timeline","timeline":"S"},{"op":"title","kind":"progress"},{"op":"format","timeline":"S","aspect":"project"}])",
        R"([{"op":"split","at":100},{"op":"transition","at":100,"type":"whip_left","length":10}])",
        R"([{"op":"timeline","timeline":"Timeline 1"},{"op":"track","track":"ST1","captions":"word","highlight":"#ffff00"}])",
        R"([{"op":"new_timeline","name":"R","from":"Timeline 1","aspect":"9:16"},{"op":"reframe","mode":"center","keep":"0.7,0.05,0.3,0.2"}])",
        R"([{"op":"reframe","mode":"fit","keep":"none"},{"op":"crop_bars"}])",
        R"([{"op":"format","width":1280,"height":720,"confirm":true}])",
    };
    for (QString ops : calls) {
        // Which timeline is edited, said every time (on a file the user's open one stays; the app shows the last)
        if (!ops.startsWith(R"([{"op":"timeline")")) ops.insert(1, R"({"op":"timeline","timeline":"Timeline 1"},)");
        const QByteArray before = state(fake.project);
        const int index = fake.project.undoStack()->index();
        const QJsonObject live = cli({"edit", fake.path, "--ops", ops});
        CHECK(live["live"].toBool());
        const QJsonObject file = cli({"edit", copy, "--ops", ops}, noLive);
        CHECK(!file["live"].toBool());
        if (!CHECK_EQ(fake.project.undoStack()->index(), index + 1)) std::printf("       ops %s\n", qPrintable(ops));
        // Same result as on the file (the open timeline aside: the app keeps the user's)
        ProjectData d;
        ProjectFile::load(copy, &d, nullptr);
        Project onFile;
        onFile.load(d);
        onFile.setCurrentSequence(fake.project.currentSequence());
        if (!CHECK(state(onFile) == state(fake.project))) {
            std::printf("       differs after %s\n", qPrintable(ops));
            if (qEnvironmentVariableIsSet("DUMP") && !QFile::exists(qEnvironmentVariable("DUMP") + "/file.json")) {
                QFile a(qEnvironmentVariable("DUMP") + "/file.json"), b(qEnvironmentVariable("DUMP") + "/live.json");
                a.open(QIODevice::WriteOnly), a.write(state(onFile)), b.open(QIODevice::WriteOnly), b.write(state(fake.project));
            }
        }
        // Undo gives back what was there, redo the change
        const QByteArray after = state(fake.project);
        fake.project.undoStack()->undo();
        if (!CHECK(state(fake.project) == before)) {
            std::printf("       undo differs after %s\n", qPrintable(ops));
            if (qEnvironmentVariableIsSet("DUMP")) {
                QFile a(qEnvironmentVariable("DUMP") + "/undo-before.json"), b(qEnvironmentVariable("DUMP") + "/undo-after.json");
                a.open(QIODevice::WriteOnly), a.write(before), b.open(QIODevice::WriteOnly), b.write(state(fake.project));
            }
        }
        fake.project.undoStack()->redo();
        if (!CHECK(state(fake.project) == after)) std::printf("       redo differs after %s\n", qPrintable(ops));
    }

    // An AI edit while the hand-over's question is open: refused as waiting (nothing changes under the question)
    {
        const int index = fake.project.undoStack()->index();
        int indexInQuestion = -1;
        QString codeInQuestion;
        bool asked = false;
        fake.duringQuestion = [&] {
            asked = true;
            QProcess p;
            p.start(Check::cliPath(), {"edit", fake.path, "--ops", R"([{"op":"marker","at":3}])"});
            p.closeWriteChannel();
            waitServing(p, 60000); // the question's event loop keeps the server answering
            const QJsonObject r = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
            codeInQuestion = r["ok"].toBool() ? QString("ok") : r["error"].toObject()["code"].toString();
            indexInQuestion = fake.project.undoStack()->index();
        };
        bool handed = false;
        QThread* t = QThread::create([&] { handed = Cli::Live::handOver({tmp.path() + "/other.schneidi"}); });
        t->start();
        while (!t->isFinished()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        t->wait();
        delete t;
        CHECK(handed);
        QElapsedTimer wait;
        wait.start();
        while (!asked && !wait.hasExpired(5000)) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        CHECK(asked);
        CHECK_EQ(indexInQuestion, index);
        CHECK_EQ(codeInQuestion, QString("APP_WAITING"));
        fake.duringQuestion = nullptr;
        // Afterwards AIs are welcome again
        const QJsonObject r = cli({"edit", fake.path, "--ops", R"([{"op":"marker","at":3}])"});
        CHECK(r["ok"].toBool());
        CHECK_EQ(fake.project.undoStack()->index(), index + 1);
    }
    return Check::result();
}
