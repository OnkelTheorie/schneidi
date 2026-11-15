// SPDX-License-Identifier: AGPL-3.0-only
// Files handed to the running app and videos opened as projects:
// - `schneidi <video>` (no app running) / File › Open Video as Project: a new unsaved project in the clip's format with
//   the clip on the timeline (MainWindow::openMediaAsProject, and the real program offscreen)
// - `schneidi <file>` while an app runs (Cli::Live::handOver over the live connection): media come into the open
//   project's pool and are selected, a .schneidi opens, also while AIs are refused; queued while the app waits
// - `app open` of schneidi-cli: the same for an AI, never over unsaved changes
// - the real program: a second `schneidi --open <file>` hands the file over and quits
// - File › Open: the picture engine has the opened timeline when openProject returns (not 30 ms later, after the
//   window's first paint), and switching projects back and forth shows each one's own length
#include "app/MainWindow.h"
#include "cli/Live.h"
#include "core/AppPaths.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/StartTemplates.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/MediaPool.h"

#include "check.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUndoStack>

#include <atomic>
#include <clocale>

namespace {

void settle(int ms = 100)
{
    QElapsedTimer t;
    t.start();
    while (!t.hasExpired(ms)) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

// handOver blocks on its socket: in a thread of its own while the server here answers
bool handOver(const QStringList& files)
{
    bool ok = false;
    QThread* t = QThread::create([&] { ok = Cli::Live::handOver(files); });
    t->start();
    while (!t->isFinished()) QApplication::processEvents(QEventLoop::AllEvents, 10);
    t->wait();
    delete t;
    settle(300); // the app opens right after its answer
    return ok;
}

// Waits for a child process while the server in this process keeps answering
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
        QApplication::processEvents();
    }
    return true;
}

QJsonObject cli(const QStringList& args)
{
    QProcess p;
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    if (!waitServing(p)) std::printf("       timeout: schneidi-cli %s\n", qPrintable(args.join(' ')));
    QApplication::processEvents();
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (o.isEmpty()) std::printf("       no JSON: %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

QString code(const QJsonObject& o)
{
    return o["ok"].toBool() ? QString("ok") : o["error"].toObject()["code"].toString();
}

int videoClips(MainWindow& w) { return w.liveProject()->timeline().video[0].clips.size(); }

QStringList selectedInPool(MainWindow& w)
{
    MediaPool* pool = w.findChild<MediaPool*>();
    return pool ? pool->selectedMedia() : QStringList{};
}

void testWindow(const QString& dir, const QString& clip, const QString& other)
{
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    Project* p = w.liveProject();

    // Video as project: the clip's format (whatever the default start template says), the clip on V1, unsaved, and
    // undo does not take the clip off again
    StartTemplates::setDefaultId("vertical");
    w.openMediaAsProject({clip});
    StartTemplates::setDefaultId(StartTemplates::NeutralId);
    CHECK_EQ(p->format().width, 320);
    CHECK_EQ(p->format().height, 240);
    CHECK_EQ(p->format().rate.fps(), 30.0);
    CHECK_EQ(videoClips(w), 1);
    CHECK_EQ(p->timeline().audio[0].clips.size(), 1);
    CHECK(p->isModified());
    CHECK(w.liveProjectPath().isEmpty());
    CHECK(!p->undoStack()->canUndo());

    Cli::Live::Server server(&w);
    if (!CHECK(server.start(&err))) return;

    // A recording handed over: into the pool, selected there, the timeline stays as it is
    CHECK(handOver({other}));
    CHECK(p->mediaInfo(other));
    CHECK_EQ(selectedInPool(w), QStringList{other});
    CHECK_EQ(videoClips(w), 1);
    // Again (already in the pool): selected, not twice
    CHECK(handOver({clip}));
    CHECK_EQ(selectedInPool(w), QStringList{clip});
    CHECK_EQ(p->media().size(), 2);

    // Also while the user refuses AIs: it is their own hand-over
    server.disconnectClient();
    const QString first = dir + "/first.schneidi";
    CHECK(w.liveSave(first, &err));
    CHECK(!p->isModified());
    // A project: the one open stays (comes to the front), another one opens
    CHECK(handOver({first}));
    CHECK_EQ(w.liveProjectPath(), first);
    CHECK_EQ(videoClips(w), 1);
    const QString second = dir + "/second.schneidi";
    {
        ProjectData d;
        d.timeline = emptyTimeline();
        d.format = p->format();
        CHECK(ProjectFile::save(d, second, &err));
    }
    CHECK(handOver({second}));
    CHECK_EQ(w.liveProjectPath(), second);
    CHECK_EQ(videoClips(w), 0);
    server.setAllowed(true);

    // While the app waits for the user (restore dialog at start): queued, opened once it stops waiting
    server.setWaiting("schneidi is waiting for the user");
    CHECK(handOver({clip}));
    CHECK(!p->mediaInfo(clip));
    server.setWaiting({});
    settle(300);
    CHECK(p->mediaInfo(clip));
    CHECK_EQ(selectedInPool(w), QStringList{clip});

    // An AI: `app open` with media and with a project
    QJsonObject r = cli({"app", "open", "--files", other});
    CHECK_EQ(code(r), QString("ok"));
    CHECK_EQ(r["media"].toArray().size(), 1);
    CHECK(p->mediaInfo(other));
    CHECK_EQ(selectedInPool(w), QStringList{other});
    CHECK_EQ(code(cli({"app", "open", "--files", dir + "/missing.mp4"})), QString("NOT_FOUND"));
    // Never over unsaved changes
    CHECK(p->isModified()); // the imports
    r = cli({"app", "open", "--files", first});
    CHECK_EQ(code(r), QString("OPEN_FAILED"));
    CHECK(r["error"].toObject()["message"].toString().contains("unsaved"));
    CHECK_EQ(w.liveProjectPath(), second);
    CHECK(w.liveSave({}, &err));
    r = cli({"app", "open", "--files", first});
    CHECK_EQ(code(r), QString("ok"));
    CHECK_EQ(r["opened"].toString(), first);
    CHECK_EQ(w.liveProjectPath(), first);
    CHECK_EQ(videoClips(w), 1);

    // File › Open from an empty project: the first picture the viewer gets is the opened project's, not the empty
    // timeline's black shown once more (the engine got the new timeline only 30 ms later, after the first paint)
    CHECK(w.openProject(second));
    settle(300);
    // Without an event loop turn (the old hand-over was a timer), the engine sends the opened project's picture: from
    // the moment the open starts, a picture that is not black comes (the engine may show the old one once more
    // first, as pausing always did). Direct: frames still queued from before do not count.
    const auto bright = [](const QImage& img) {
        int n = 0;
        for (int y = 0; y < img.height(); y += 8)
            for (int x = 0; x < img.width(); x += 8) n += qGray(img.pixel(x, y)) > 40;
        return n > 10; // testsrc2, not black
    };
    std::atomic<bool> armed{false}, sawPicture{false};
    const QMetaObject::Connection shown = QObject::connect(
        &engine, &Engine::frameReady, &w,
        [&](const QImage& img) {
            if (armed && bright(img)) sawPicture = true;
        },
        Qt::DirectConnection);
    armed = true;
    CHECK(w.openProject(first));
    QElapsedTimer waited;
    waited.start();
    while (!sawPicture && waited.elapsed() < 5000) QThread::msleep(5);
    QObject::disconnect(shown);
    CHECK(sawPicture.load());
    CHECK_EQ(engine.position(), 0);
    CHECK_EQ(videoClips(w), 1);
}

// The real program offscreen (own settings/data folders): a video alone opens as project; a second start with
// --open hands the file to the first one and quits
void testProgram(const QString& dir, const QString& clip, const QString& other)
{
#ifdef Q_OS_WIN
    Q_UNUSED(dir);
    Q_UNUSED(clip);
    Q_UNUSED(other);
    std::printf("       (program part skipped on Windows: XDG folders do not apply there)\n");
#else
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("SCHNEIDI_LIVE", "1"); // test runs are invisible to AIs otherwise
    env.insert("QT_QPA_PLATFORM", "offscreen");
    QProcess app;
    app.setProcessEnvironment(env);
    app.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    app.start(Check::appPath(), {"--screenshot", dir + "/shot.png", "--wait", "60000", clip});
    if (!CHECK(app.waitForStarted(10000))) return;
    // Its live registry entry appears once it runs
    QElapsedTimer t;
    t.start();
    QJsonObject r;
    while (!t.hasExpired(60000)) {
        r = cli({"info", "@app"});
        if (code(r) == "ok" && r["media"].toArray().size() > 0) break;
        settle(300);
    }
    CHECK_EQ(code(r), QString("ok"));
    CHECK_EQ(r["format"].toObject()["width"].toInt(), 320);
    CHECK_EQ(r["format"].toObject()["height"].toInt(), 240);
    CHECK_EQ(r["format"].toObject()["fps"].toDouble(), 30.0);
    const QJsonArray tracks = r["timeline"].toObject()["tracks"].toArray();
    CHECK_EQ(tracks.isEmpty() ? -1 : int(tracks.at(0).toObject()["clips"].toArray().size()), 1); // V1

    QProcess second;
    second.setProcessEnvironment(env);
    second.start(Check::appPath(), {"--open", other});
    const bool quit = second.waitForFinished(30000);
    CHECK(quit);
    if (!quit) second.kill();
    CHECK_EQ(second.exitCode(), 0);
    r = cli({"info", "@app"}); // still the only app: the second one did not stay
    CHECK_EQ(code(r), QString("ok"));
    bool there = false;
    for (const QJsonValue& m : r["media"].toArray()) there = there || m.toObject()["path"].toString() == other;
    CHECK(there);
    r = cli({"app", "state"});
    CHECK_EQ(code(r), QString("ok"));
    app.kill();
    app.waitForFinished(5000);
#endif
}

} // namespace

int main(int argc, char* argv[])
{
    QTemporaryDir tmp;
    // Own registry of running apps, settings and caches (also for the programs this test starts)
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    Check::initEnv();
    QApplication app(argc, argv);
    // Like the program (no organization on Linux): schneidi-cli finds the registry
    AppPaths::setProgramNames(); // the program's folders (also on Windows: with its organization)
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path() + "/config");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    const QString dir = tmp.path();
    const QStringList x264{"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-shortest"};
    const QString clip = Check::makeMedia(dir + "/clip.mp4", QStringList{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30:duration=2",
                                                                         "-f", "lavfi", "-i", "sine=frequency=440:duration=2"}
                                                                 + x264);
    const QString other = Check::makeMedia(dir + "/other.mp4", QStringList{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=1",
                                                                           "-f", "lavfi", "-i", "sine=frequency=330:duration=1"}
                                                                   + x264);
    if (!CHECK(!clip.isEmpty() && !other.isEmpty())) return Check::result();
    testWindow(dir, clip, other);
    // Nothing runs anymore: nobody takes the files
    CHECK(!handOver({other}));
    testProgram(dir, clip, other);
    return Check::result();
}
