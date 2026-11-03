// SPDX-License-Identifier: AGPL-3.0-only
#include "buildinfo.h"
#include "app/CrashReport.h"
#include "app/InputBindings.h"
#include "app/LaunchArgs.h"
#include "app/Log.h"
#include "core/ProjectFile.h"
#include "app/MainWindow.h"
#include "cli/Live.h"
#include "cli/Setup.h"
#include "ui/MediaStorage.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "engine/Engine.h"
#include "engine/FfmpegLog.h"
#include "engine/picture/gpu/Gpu.h"

#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
#include <QLockFile>
#include <QMessageBox>
#include <QPainter>
#include <QSplashScreen>
#include <QProcess>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <clocale>
#include <memory>

namespace {

// Splash window: logo (like assets/schneidi.svg, drawn by hand - without QtSvg), name and version in the design colors
QPixmap splashPixmap()
{
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(QSize(460, 240) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Theme::panel);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Theme::border, 1));
    p.drawRect(QRectF(0.5, 0.5, 459, 239));
    p.translate(36, 56);
    p.scale(1.6, 1.6); // Logo 64 -> ~100 px
    p.setPen(Qt::NoPen);
    const auto box = [&p](qreal x, qreal y, qreal w, qreal h, qreal r, const char* color) {
        p.setBrush(QColor(color));
        p.drawRoundedRect(QRectF(x, y, w, h), r, r);
    };
    box(4, 4, 56, 56, 12, "#232327");
    box(12, 18, 40, 6, 2, "#3d7fd6");
    box(12, 29, 24, 6, 2, "#3d7fd6");
    box(38, 29, 14, 6, 2, "#8a5cc7");
    box(12, 40, 40, 6, 2, "#3f9a5a");
    box(30, 12, 3, 40, 1, "#e8414a");
    p.drawPolygon(QPolygonF{QPointF(26, 10), QPointF(37, 10), QPointF(31.5, 16)});
    p.resetTransform();
    QFont title = qApp->font();
    title.setPixelSize(40);
    title.setBold(true);
    p.setFont(title);
    p.setPen(Theme::text);
    p.drawText(QRectF(160, 70, 280, 50), Qt::AlignLeft | Qt::AlignVCenter, "schneidi");
    QFont small = qApp->font();
    small.setPixelSize(13);
    p.setFont(small);
    p.setPen(Theme::textDim);
    p.drawText(QRectF(162, 118, 280, 24), Qt::AlignLeft | Qt::AlignVCenter,
               T("Version %1").arg(QCoreApplication::applicationVersion()));
    return pm;
}

} // namespace

int main(int argc, char* argv[])
{
    QElapsedTimer startTimer;
    startTimer.start();
    // Test helper: --screenshot image.png [--wait ms] runs invisibly and silently (no window on the desktop),
    // triggers --actions, saves the window image and quits
    QString screenshot;
    int screenshotWait = 4000;
    QString langOverride;
    QString formatArg; // test helper: --format 1080x1920@30 (project settings for this run)
    QString storageArg; // test helper: --storage <folder>
    QString designArg;  // test helper: --design midnight (this run only)
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--format") == 0) formatArg = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--screenshot") == 0) screenshot = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--wait") == 0) screenshotWait = QByteArray(argv[i + 1]).toInt();
        if (qstrcmp(argv[i], "--lang") == 0) langOverride = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--storage") == 0) storageArg = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--design") == 0) designArg = QString::fromLocal8Bit(argv[i + 1]);
    }
    // --verbose: FFmpeg warnings too (otherwise only errors, see FfmpegLog)
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--verbose") == 0) qputenv("SCHNEIDI_VERBOSE", "1");
    if (!screenshot.isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    // Names before the application object: I18n::prepareEnvironment reads the language setting
    QApplication::setApplicationName("schneidi");
#ifdef Q_OS_WIN
    // Without an organization name QSettings cannot write to the registry on Windows (on Linux
    // "Unknown Organization" serves as a substitute - it stays that way there, otherwise existing settings would be gone)
    QApplication::setOrganizationName("schneidi");
#endif
    I18n::prepareEnvironment(langOverride);
    QApplication app(argc, argv);
    QApplication::setApplicationVersion(QString::fromUtf8(SCHNEIDI_VERSION_LABEL) + (*SCHNEIDI_BUILD ? QString(" (%1)").arg(SCHNEIDI_BUILD) : QString())); // version from CMakeLists, Git state, see cmake/BuildInfo.cmake
    // Already running: hand the files over to that app (a double-click in the file manager, a recorder passing its
    // clip on: `schneidi --open <file>` or just `schneidi <file>`) and quit before anything of this process shows or
    // writes (log, crash report). --new-instance starts another window anyway; test runs never hand over.
    // One launch at a time decides (two double-clicks at once end in one app): a launch that becomes the app keeps
    // the lock until its live server is registered, the next one then hands its files to it.
    std::unique_ptr<QLockFile> launchLock;
    if (const QStringList args = app.arguments().mid(1), handed = LaunchArgs::files(args);
        !handed.isEmpty() && screenshot.isEmpty() && !args.contains("--demo") && !args.contains("--actions")
        && !args.contains("--new-instance") && qEnvironmentVariable("SCHNEIDI_LIVE", "1") == "1") {
        launchLock = Cli::Live::lockLaunch();
        if (Cli::Live::handOver(handed)) return 0;
    }
    Log::install();
    CrashReport::install(); // after the log: attaches the previous run's log to its crash report
    // Test aid: --lang <code> (en, de, fr, …) only applies to this run (the setting stays unchanged)
    I18n::install(langOverride);

    // C libraries (FFmpeg option parsing) read decimal numbers with a dot - a German locale would read "0.5" as 0
    std::setlocale(LC_NUMERIC, "C");

    Theme::load(designArg);
    Theme::apply(app);
    InputBindings::instance().load();

    // Splash window while the window is built (Windows at first start: the virus scanner looks at every DLL)
    std::unique_ptr<QSplashScreen> splash;
    const auto status = [&splash](const QString& text) {
        if (!splash) return;
        splash->showMessage("  " + text + "\n", Qt::AlignLeft | Qt::AlignBottom, Theme::textDim);
        QCoreApplication::processEvents();
    };
    if (screenshot.isEmpty()) {
        splash = std::make_unique<QSplashScreen>(splashPixmap());
        splash->show();
        status(T("Öffne Fenster…"));
    }

    FfmpegLog::install([](bool error, const QString& text) { Log::message(error ? "AV-E" : "AV-W", text); });
    // The GPU picture engine's OpenGL surface from the event loop, not on the way to the first window
    Picture::Gpu::Service::setDeferredPrepare(true);
    Engine engine;
    QString error;
    const qint64 beforeEngine = startTimer.elapsed();
    if (!engine.init(&error)) {
        launchLock.reset(); // the next launch must not wait for this message box
        splash.reset();
        QMessageBox::critical(nullptr, "schneidi", error);
        return 1;
    }
    const qint64 afterEngine = startTimer.elapsed();

    MainWindow w(&engine);
    // Start maximized like DaVinci; screenshot test runs (offscreen) keep the fixed size from MainWindow
    if (app.arguments().contains("--screenshot")) w.show();
    else w.showMaximized();
    if (splash) splash->finish(&w);
    qInfo("Startzeit: Engine %lld ms, Fenster sichtbar nach %lld ms", afterEngine - beforeEngine, startTimer.elapsed());

    // Files from the command line directly into the Media Pool
    const QStringList args = app.arguments().mid(1);
    QStringList files = LaunchArgs::files(args); // without options (also Qt's X11 ones such as -geometry)
    const bool demo = args.contains("--demo"); // test helper: additionally place on the timeline
    // Test helper: --actions select_all,toggle_enabled triggers actions (IDs from keybindings.json) after startup
    QStringList actions;
    if (const int i = args.indexOf("--actions"); i >= 0 && i + 1 < args.size())
        actions = args.at(i + 1).split(',', Qt::SkipEmptyParts);
    // .schneidi file as argument = open project (e.g. double-click in the file manager)
    QString projectFile;
    for (const QString& f : files)
        if (f.endsWith(QString(".%1").arg(ProjectFile::Extension))) projectFile = f;
    files.removeAll(projectFile);
    // Test runs (--demo/--actions) leave the user's automatic backup alone
    const bool testRun = demo || !actions.isEmpty() || !screenshot.isEmpty();
    if (testRun) w.disableAutosave();
    // Settings › AI › "Connect with Claude": the same as `schneidi-cli setup claude` (Claude Code and Desktop)
    w.setClaudeSetup([](bool remove) {
        Cli::Setup::Options o;
        o.remove = remove;
        return Cli::Setup::claude(o);
    });
    // Live connection for schneidi-cli / MCP (cli/Live.h); test runs stay invisible to AIs unless SCHNEIDI_LIVE=1.
    // Started before the restore dialog: while it waits for the user, AIs hear that instead of "not running".
    std::unique_ptr<Cli::Live::Server> live;
    if (qEnvironmentVariable("SCHNEIDI_LIVE", testRun ? "0" : "1") == "1") {
        live = std::make_unique<Cli::Live::Server>(&w);
        QString liveError;
        if (live->start(&liveError)) {
            Cli::Live::Server* server = live.get();
            w.setLiveControls([server](const QString& reason) { server->disconnectClient(reason); },
                              [server] { server->setAllowed(true); }, [server](int ms) { server->setIdleLimit(ms); });
        } else {
            qWarning("Live connection not available: %s", qPrintable(liveError));
            live.reset();
        }
    }
    launchLock.reset(); // registered (or never will be): the next launch may hand its files over
    // AI chat panel: its tools are this installation's `schneidi-cli mcp`, talking to this app (cli/Live.h). Test runs
    // (and runs without a screen) never send anything to an AI provider.
    // Without the live connection the chat's tools must not reach another schneidi that runs: a server that is not there
    w.setAiChatTools(Cli::Setup::serverCommand(), live ? live->chatEnvironment()
                                                       : QHash<QString, QString>{{Cli::Live::ChatServerEnv, "none"}});
    w.setAiChatOffline(testRun || QGuiApplication::platformName() == QLatin1String("offscreen"));
    const auto liveWaiting = [&live](const QString& message) {
        if (live) live->setWaiting(message);
    };
    // First offer the backup after a crash - otherwise opening by double-click deletes it without asking.
    // Restored: this project stays open (the file can then be opened normally).
    liveWaiting("schneidi is waiting for the user (restore dialog: restore unsaved changes after a crash?); "
                "try again once it is answered");
    const bool restored = !testRun && w.offerAutosaveRestore();
    liveWaiting("schneidi is opening a project; try again in a moment");
    if (!projectFile.isEmpty() && !restored) w.openProject(QFileInfo(projectFile).absoluteFilePath());
    liveWaiting({});
    if (!testRun) QTimer::singleShot(0, &w, [&w] { CrashReport::offer(&w); }); // crashed last time: show the report
    if (!testRun) w.startUpdateCheck(); // at most once a day, a few seconds from now
    if (!formatArg.isEmpty()) {
        const QStringList parts = formatArg.split(QRegularExpression("[x@]"));
        ProjectFormat f;
        f.width = parts.value(0).toInt() & ~1;
        f.height = parts.value(1).toInt() & ~1;
        f.rate = nearestFrameRate(parts.value(2, "25").toDouble());
        if (f.width >= 16 && f.height >= 16) w.setProjectFormat(f);
    }
    // A video alone (no project, nothing restored): opens as a new project in the clip's format, the clip on the
    // timeline. Test runs with --demo/--format keep the project they set up and put the files on it.
    if (!files.isEmpty()) {
        if (projectFile.isEmpty() && !restored && !demo && formatArg.isEmpty()) w.openMediaAsProject(files);
        else w.importFiles(files, demo, formatArg.isEmpty());
    }
    // Test helper: --storage <folder> opens the folder in Media Storage (is not saved)
    if (!storageArg.isEmpty())
        if (auto* storage = w.findChild<MediaStorage*>()) storage->setFolder(QFileInfo(storageArg).absoluteFilePath());
    QTimer::singleShot(1500, &w, [actions] {
        for (const QString& id : actions)
            for (const auto& e : InputBindings::instance().entries())
                if (e.id == id && e.action) e.action->trigger();
    });
    if (!screenshot.isEmpty()) {
        // --size 2560x1440: window size in the test run (e.g. check layout in full screen)
        QSize size(1600, 950);
        if (const int i = app.arguments().indexOf("--size"); i >= 0 && i + 1 < app.arguments().size()) {
            const QStringList wh = app.arguments()[i + 1].split('x');
            if (wh.size() == 2 && wh[0].toInt() > 0 && wh[1].toInt() > 0) size = QSize(wh[0].toInt(), wh[1].toInt());
        }
        w.resize(size);
        QTimer::singleShot(1500 + screenshotWait, &w, [&w, screenshot] {
            w.grab().save(screenshot);
            QApplication::quit();
        });
    }
    const int rc = app.exec();
    // Restart after design/language change: only here, when the window is closed and everything has been saved
    // The live server goes first: the new process would otherwise hand its project back to this one, which is quitting
    if (w.restartRequested()) {
        live.reset();
        QProcess::startDetached(QCoreApplication::applicationFilePath(), w.restartArguments());
    }
    return rc;
}
