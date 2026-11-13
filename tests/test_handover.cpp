// SPDX-License-Identifier: AGPL-3.0-only
// Launches with files while an app runs (`schneidi <file>`, Cli::Live::handOver + lockLaunch), the cases where a
// second window used to open:
// - an app that does not answer at once (its event loop blocked for a few seconds: still starting) still gets the files
// - an app that runs an AI command (which spins the event loop) answers the hand-over meanwhile and opens the files
//   once the command is done
// - two launches at the same moment end in one app: the second one hands its file to the first (this test started
//   twice as a child process, doing what main() does)
// Own registry of running apps (temporary folder), never the user's schneidi.
#include "cli/Live.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include "check.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <functional>
#include <utility>

namespace {

class FakeApp : public LiveHost {
public:
    Project project;
    Selection selection;
    QStringList opened;
    std::function<void()> whileRunning; // called while an AI command runs (it may spin the event loop)

    FakeApp()
    {
        ProjectData d;
        d.timeline = emptyTimeline();
        project.load(d);
    }
    Project* liveProject() override { return &project; }
    QString liveProjectPath() const override { return {}; }
    Selection* liveSelection() override { return &selection; }
    int livePlayhead() const override { return 0; }
    bool livePlaying() const override { return false; }
    void liveSeek(int) override {}
    void livePlay(bool) override {}
    bool liveSave(const QString&, QString*) override { return false; }
    void liveGenerateProxies(const QStringList&) override {}
    QJsonObject liveProxyState(const QString&) const override { return {}; }
    bool liveOpen(const QStringList& files, bool, QString*) override
    {
        opened << files;
        return true;
    }
    void liveConnectionChanged(bool, const QString&) override {}
    void liveChanged(const LiveChange&) override {}
    void liveActivity(const LiveActivity&) override {}
    void liveCommandRunning(bool running, bool) override
    {
        if (running && whileRunning) std::exchange(whileRunning, {})();
    }
};

void settle(int ms)
{
    QElapsedTimer t;
    t.start();
    while (!t.hasExpired(ms)) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// handOver blocks on its socket: in a thread of its own while the server here answers (or does not)
struct HandOverThread {
    std::atomic<bool> ok{false};
    QThread* thread = nullptr;
    explicit HandOverThread(const QStringList& files)
    {
        thread = QThread::create([this, files] { ok = Cli::Live::handOver(files); });
        thread->start();
    }
    ~HandOverThread()
    {
        thread->wait();
        delete thread;
    }
    // Serves until it is done or `ms` passed; true = done
    bool serve(int ms)
    {
        QElapsedTimer t;
        t.start();
        while (!thread->isFinished() && !t.hasExpired(ms)) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        return thread->isFinished();
    }
};

// An app whose event loop is blocked longer than the 3 s a hello may take (still starting: building the window,
// opening a project): the files arrive once it answers, no second window
void testSlowApp(const QString& dir)
{
    FakeApp app;
    Cli::Live::Server server(&app);
    QString err;
    if (!CHECK(server.start(&err))) return;
    const QString file = dir + "/slow.mp4";
    HandOverThread h({file});
    QThread::msleep(5000); // no events meanwhile
    CHECK(h.serve(60000));
    CHECK(h.ok);
    settle(300); // opened right after the answer
    CHECK_EQ(app.opened, QStringList{file});
}

// One line request on a raw socket (an AI client, see Live.h)
QJsonObject request(QLocalSocket& s, const QJsonObject& msg)
{
    s.write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
    s.waitForBytesWritten(5000);
    QByteArray buf;
    while (!buf.contains('\n') && s.waitForReadyRead(60000)) buf += s.readAll();
    return QJsonDocument::fromJson(buf.left(buf.indexOf('\n'))).object();
}

// An AI command runs in the app and spins its event loop: the hand-over is answered meanwhile (not after the
// command, which may take minutes), the files open once the command is done
void testDuringCommand(const QString& dir)
{
    FakeApp app;
    Cli::Live::Server server(&app);
    QString err;
    if (!CHECK(server.start(&err))) return;
    const QString file = dir + "/during.mp4";
    bool answeredDuring = false;
    bool openedDuring = true;
    app.whileRunning = [&] {
        HandOverThread h({file});
        answeredDuring = h.serve(8000) && h.ok;
        openedDuring = app.opened.contains(file);
        h.serve(60000);
    };
    // The AI client: hello, attach, run (blocks in its own thread)
    const QString entry = QDir(Cli::Live::registryDir()).filePath(QString("%1.json").arg(QCoreApplication::applicationPid()));
    QFile f(entry);
    if (!CHECK(f.open(QIODevice::ReadOnly))) return;
    const QJsonObject reg = QJsonDocument::fromJson(f.readAll()).object();
    std::atomic<bool> ran{false};
    QThread* ai = QThread::create([&] {
        QLocalSocket s;
        s.connectToServer(reg.value("server").toString());
        if (!s.waitForConnected(5000)) return;
        if (!request(s, {{"type", "hello"}, {"token", reg.value("token").toString()}}).value("ok").toBool()) return;
        if (!request(s, {{"type", "attach"}, {"client", "test"}}).value("ok").toBool()) return;
        ran = !request(s, {{"type", "run"}, {"command", "app"}, {"args", QJsonObject{{"action", "state"}, {"project", "@app"}}}})
                   .isEmpty();
        s.disconnectFromServer();
    });
    ai->start();
    QElapsedTimer t;
    t.start();
    while (!ai->isFinished() && !t.hasExpired(120000)) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ai->wait();
    delete ai;
    CHECK(ran);
    CHECK(!app.whileRunning); // the command ran
    CHECK(answeredDuring);
    CHECK(!openedDuring); // not in the middle of the command
    settle(500);
    CHECK_EQ(app.opened, QStringList{file});
}

// Child: what main() does with a file on the command line. Prints "handed" or becomes the app (slow start: the window
// takes a while before the server, the project after it), prints what it was handed, quits after `lifeMs`.
int launch(const QString& file, int lifeMs)
{
    std::unique_ptr<QLockFile> lock = Cli::Live::lockLaunch();
    if (Cli::Live::handOver({file})) {
        std::printf("handed\n");
        return 0;
    }
    QThread::msleep(1000); // building the window
    FakeApp app;
    Cli::Live::Server server(&app);
    QString err;
    if (!server.start(&err)) return 2;
    lock.reset();
    std::printf("app\n");
    std::fflush(stdout);
    QThread::msleep(4000); // opening its own file: no events meanwhile
    settle(lifeMs);
    for (const QString& f : app.opened) std::printf("opened %s\n", qPrintable(QFileInfo(f).fileName()));
    return 0;
}

// Two launches with files at the same moment: one app, it gets the other one's file
void testTwoLaunches(const QString& dir, const QString& self)
{
    QProcess a, b;
    for (QProcess* p : {&a, &b}) p->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    a.start(self, {"--launch", dir + "/a.mp4", "12000"});
    b.start(self, {"--launch", dir + "/b.mp4", "12000"});
    for (QProcess* p : {&a, &b}) {
        if (!p->waitForFinished(120000)) {
            p->kill();
            p->waitForFinished(5000);
        }
    }
    const QString outA = QString::fromUtf8(a.readAllStandardOutput());
    const QString outB = QString::fromUtf8(b.readAllStandardOutput());
    CHECK_EQ(a.exitCode(), 0);
    CHECK_EQ(b.exitCode(), 0);
    const int apps = int(outA.startsWith("app")) + int(outB.startsWith("app"));
    CHECK_EQ(apps, 1);
    CHECK_EQ(int(outA.startsWith("handed")) + int(outB.startsWith("handed")), 1);
    const QString app = outA.startsWith("app") ? outA : outB;
    const QString other = outA.startsWith("app") ? "b.mp4" : "a.mp4";
    CHECK(app.contains("opened " + other));
    if (apps != 1) std::printf("       a: %s       b: %s", qPrintable(outA), qPrintable(outB));
}

} // namespace

int main(int argc, char* argv[])
{
    // Own registry of running apps: a temporary folder (inherited by the children), never the user's
    const bool child = argc >= 4 && qstrcmp(argv[1], "--launch") == 0;
    QTemporaryDir tmp;
    if (!child) qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    Check::initEnv();
    QGuiApplication qapp(argc, argv);
    QCoreApplication::setOrganizationName("schneidi-tests");
    QCoreApplication::setApplicationName("schneidi-test-handover");
    if (child) return launch(QString::fromLocal8Bit(argv[2]), QByteArray(argv[3]).toInt());
    // Nothing runs: nobody takes the files, at once
    QElapsedTimer t;
    t.start();
    CHECK(!Cli::Live::handOver({tmp.path() + "/none.mp4"}));
    CHECK(t.elapsed() < 2000);
    testSlowApp(tmp.path());
    testDuringCommand(tmp.path());
    testTwoLaunches(tmp.path(), QCoreApplication::applicationFilePath());
    return Check::result();
}
