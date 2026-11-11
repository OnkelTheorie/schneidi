// SPDX-License-Identifier: AGPL-3.0-only
#include "buildinfo.h"
#include "app/CrashReport.h"
#include "app/InputBindings.h"
#include "app/LaunchArgs.h"
#include "app/Log.h"
#include "core/StartupTrace.h"
#include "core/ProjectFile.h"
#include "app/MainWindow.h"
#include "app/AiSettingsPage.h"
#include "cli/Commands.h"
#include "cli/Live.h"
#include "cli/Setup.h"
#include "ui/FrameView.h"
#include "ui/MediaStorage.h"
#include "app/Theme.h"
#include "core/CodeTrust.h"
#include "core/I18n.h"
#include "core/SettingsIsolation.h"
#include "core/Tours.h"
#include "engine/Engine.h"
#include "engine/Extensions.h"
#include "engine/RenderQueue.h"
#include "engine/StorageThumbCache.h"
#include "engine/FfmpegLog.h"
#include "engine/picture/gpu/Gpu.h"

#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
#include <QLockFile>
#include <QMessageBox>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QProcess>
#include <QFileInfo>
#include <QRegularExpression>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>
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

// Startup time of the last run (ms to the shown window): a splash only when it was slower than this
constexpr const char* kLastStartKey = "startup/lastWindowMs";
constexpr qint64 kSplashAboveMs = 900;

// A splash that never blocks: QSplashScreen waits up to a second for the window system to expose it (on show and in
// finish()), which made every start a second slower. This one paints when it gets the chance - a short, bounded look
// at the event queue after show() - and goes away with the main window's show.
class SplashWindow : public QWidget {
public:
    explicit SplashWindow(const QPixmap& pixmap)
        : QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), m_pixmap(pixmap)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setFixedSize(pixmap.deviceIndependentSize().toSize());
        if (const QScreen* screen = QGuiApplication::primaryScreen())
            move(screen->availableGeometry().center() - rect().center());
    }
    void showNow(const QString& message)
    {
        m_message = message;
        show();
        // At most ~60 ms for the expose (a local display answers in a few): enough for the picture on screen, never
        // the full second QSplashScreen allows
        QElapsedTimer t;
        t.start();
        while (windowHandle() && !windowHandle()->isExposed() && t.elapsed() < 60)
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        if (windowHandle() && windowHandle()->isExposed()) repaint();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.drawPixmap(0, 0, m_pixmap);
        p.setPen(Theme::textDim);
        p.drawText(rect().adjusted(8, 0, -8, -10), Qt::AlignLeft | Qt::AlignBottom, m_message);
    }

private:
    QPixmap m_pixmap;
    QString m_message;
};

// SCHNEIDI_STARTUP_TRACE: marks the window's first expose and first paint (startup phase table)
class FirstPaintTrace : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::Expose && !m_exposed) {
            m_exposed = true;
            StartupTrace::mark("first expose");
        } else if (e->type() == QEvent::Paint && !m_painted && o == parent()) {
            m_painted = true;
            StartupTrace::mark("first paint (main window)");
            QTimer::singleShot(0, this, [] { StartupTrace::mark("event loop idle after first paint"); });
        }
        return false;
    }

private:
    bool m_exposed = false, m_painted = false;
};

// Claude Code or Claude Desktop is on this computer, but not connected to schneidi: asked once on start (Cli::Setup
// offer rules: at most a few times, two weeks apart, never after "don't ask again"). Waits while another dialog is open
// (restore, tours).
QPointer<QThread> g_claudeSetupThread; // waited for before main returns (it runs `claude …` processes)

void offerClaude(MainWindow* window, const std::function<QJsonObject(bool)>& setup, int tries = 0)
{
    if (!window->isVisible() || QApplication::activeModalWidget() || window->busyForQuestions()) {
        if (tries < 20) QTimer::singleShot(30000, window, [=] { offerClaude(window, setup, tries + 1); });
        return;
    }
    if (!Cli::Setup::offerDue()) return;
    Cli::Setup::offerShown();
    QMessageBox box(QMessageBox::Question, T("Mit Claude verbinden"),
                    T("%1 ist auf diesem Computer installiert. Soll schneidi sich damit verbinden?")
                        .arg(Cli::Setup::claudeFound().join(T(" und "))),
                    QMessageBox::NoButton, window);
    box.setInformativeText(T("Dann kann Claude deine Videos mit schneidi schneiden: Es bekommt die schneidi-Werkzeuge "
                             "(MCP) und eine Schnitt-Anleitung (Skill). Geht auch später unter Einstellungen › KI."));
    QPushButton* connect = box.addButton(T("Jetzt verbinden"), QMessageBox::AcceptRole);
    QPushButton* later = box.addButton(T("Später"), QMessageBox::RejectRole);
    QPushButton* never = box.addButton(T("Nicht mehr fragen"), QMessageBox::DestructiveRole);
    // It comes unasked while the user may be typing: Enter/Space/Escape must not connect
    for (QAbstractButton* b : box.buttons()) b->setFocusPolicy(Qt::TabFocus);
    box.setDefaultButton(later);
    box.setEscapeButton(later);
    box.exec();
    if (box.clickedButton() == never) {
        Cli::Setup::offerNever();
        return;
    }
    if (box.clickedButton() != connect) return;
    // `claude mcp add` may take a few seconds: in a helper thread, the window stays usable
    auto result = std::make_shared<QJsonObject>();
    QThread* worker = QThread::create([setup, result] { *result = setup(false); });
    g_claudeSetupThread = worker;
    QObject::connect(worker, &QThread::finished, window, [window, worker, result] {
        worker->deleteLater();
        bool done = false, failed = false;
        for (const QJsonValue& v : *result) {
            done |= v.toObject().value("state").toString() == "done";
            failed |= v.toObject().value("state").toString() == "failed";
        }
        const QString text = AiSettingsPage::claudeResultText(*result, false);
        if (failed || !done) QMessageBox::warning(window, T("Mit Claude verbinden"), text);
        else QMessageBox::information(window, T("Mit Claude verbinden"), text);
    });
    worker->start();
}

} // namespace

int main(int argc, char* argv[])
{
    StartupTrace::start();
    QElapsedTimer startTimer;
    startTimer.start();
    SettingsIsolation::apply(); // test runs only (SCHNEIDI_TEST_SETTINGS): never the user's settings, before any use
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
    StartupTrace::mark("QApplication");
    QApplication::setApplicationVersion(QString::fromUtf8(SCHNEIDI_VERSION_LABEL) + (*SCHNEIDI_BUILD ? QString(" (%1)").arg(SCHNEIDI_BUILD) : QString())); // version from CMakeLists, Git state, see cmake/BuildInfo.cmake
    // Already running: hand the files over to that app (a double-click in the file manager, a recorder passing its
    // clip on: `schneidi --open <file>` or just `schneidi <file>`) and quit before anything of this process shows or
    // writes (log, crash report). --new-instance starts another window anyway; test runs never hand over.
    // One launch at a time decides (two double-clicks at once end in one app): a launch that becomes the app keeps
    // the lock until its live server is registered, the next one then hands its files to it.
    std::unique_ptr<QLockFile> launchLock;
    if (const QStringList args = app.arguments().mid(1);
        LaunchArgs::handsOver(args) && screenshot.isEmpty() && qEnvironmentVariable("SCHNEIDI_LIVE", "1") == "1") {
        launchLock = Cli::Live::lockLaunch();
        if (Cli::Live::handOver(LaunchArgs::files(args))) return 0;
    }
    Log::install();
    Extensions::cleanUp(); // old versions a running program held at its update
    CrashReport::install(); // after the log: attaches the previous run's log to its crash report
    // Test aid: --lang <code> (en, de, fr, …) only applies to this run (the setting stays unchanged)
    I18n::install(langOverride);

    // C libraries (FFmpeg option parsing) read decimal numbers with a dot - a German locale would read "0.5" as 0
    std::setlocale(LC_NUMERIC, "C");

    Theme::load(designArg);
    Theme::apply(app);
    InputBindings::instance().load();
    StartupTrace::mark("log, i18n, theme, bindings");

    // Splash window while the window is built (Windows at first start: the virus scanner looks at every DLL; weak
    // hardware). Only when the previous start was slow (or there was none): on a fast machine the window itself comes
    // before a splash would be worth its round trip to the window system.
    std::unique_ptr<SplashWindow> splash;
    if (screenshot.isEmpty() && QSettings().value(kLastStartKey, kSplashAboveMs + 1).toLongLong() > kSplashAboveMs) {
        splash = std::make_unique<SplashWindow>(splashPixmap());
        splash->showNow(T("Öffne Fenster…"));
    }
    StartupTrace::mark("splash shown");

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
    StartupTrace::mark("engine init");

    // Deliver › Prüf-Render: the marks of `render review:"video"` (the command layer, which the app's library does not link)
    RenderQueue::setReviewMarks([](const ProjectData& data, int sequence, const std::atomic<bool>& cancel) {
        return Cli::reviewOverlay(data, sequence, &cancel);
    });
    // The viewer trusts the last run's GPU check; the real one (a throwaway OpenGL/D3D device) comes after the window
    FrameView::setUseCachedGpuCheck(true);
    MainWindow w(&engine);
    StartupTrace::mark("MainWindow constructed");
    // Menu entries that map onto an edit op of the command layer ("Musik auf Länge…"): the user's own undo step
    w.setUserRunner([&w](const QString& command, const QJsonObject& args, const QString& undoText) {
        return Cli::Live::runForUser(&w, command, args, undoText);
    });
    if (StartupTrace::enabled()) {
        auto* trace = new FirstPaintTrace(&w);
        w.installEventFilter(trace);
        w.winId(); // the native window exists now: its expose reaches the filter
        w.windowHandle()->installEventFilter(trace);
    }
    // Start maximized like DaVinci; screenshot test runs (offscreen) keep the fixed size from MainWindow
    if (app.arguments().contains("--screenshot")) w.show();
    else w.showMaximized();
    StartupTrace::mark("window show() returned");
    // The real GPU check once the window is on screen and idle (a stored one decided the viewer's start), at the
    // latest after the first interactions have had their turn
    QTimer::singleShot(1500, &w, [] {
        FrameView::checkCachedGpu();
        StartupTrace::mark("viewer GPU check done");
    });
    splash.reset(); // never waits for the window system: the main window covers the spot anyway
    StartupTrace::mark("splash finished");
    qInfo("Startzeit: Engine %lld ms, Fenster sichtbar nach %lld ms", afterEngine - beforeEngine, startTimer.elapsed());
    if (screenshot.isEmpty()) QSettings().setValue(kLastStartKey, startTimer.elapsed()); // decides the next splash

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
    // In-app tours never ask in test runs or without a screen (core/Tours.h)
    if (testRun || QGuiApplication::platformName() == QLatin1String("offscreen")) TourState::setSuppressed(true);
    // Settings › AI › "Connect with Claude": the same as `schneidi-cli setup claude` (Claude Code and Desktop)
    const auto claudeSetup = [](bool remove) {
        Cli::Setup::Options o;
        o.remove = remove;
        const QJsonObject result = Cli::Setup::claude(o);
        if (remove) Cli::Setup::offerNever(); // removed on purpose: no offer to connect again
        return result;
    };
    w.setClaudeSetup(claudeSetup);
    // Not connected to Claude yet: asked once, a few seconds after the window is up (never in test runs)
    if (!testRun && QGuiApplication::platformName() != QLatin1String("offscreen")
        && qEnvironmentVariableIsEmpty("CLAUDECODE")) // started by Claude Code: nobody there to answer
        QTimer::singleShot(6000, &w, [&w, claudeSetup] { offerClaude(&w, claudeSetup); });
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
            w.setLiveWaiting([server](const QString& message) { server->setWaiting(message); }); // e.g. during a tour
            // The chat's plan card runs the plan's steps through the same server (one undo step, like the chat's edits)
            w.setChatRunner([server](const QString& command, const QJsonObject& args) {
                return server->runForChat(command, args);
            });
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
    if (!testRun) CodeTrust::prune(); // trust records of deleted files (at most once a day)
    // Old Media Storage thumbnails (at most once a day, a folder listing: not on the GUI thread)
    if (!testRun) QThreadPool::globalInstance()->start([] { StorageThumbCache::prune(); });
    // The first start's tour question, once, after the window settled (the overlay does not block an AI or the app)
    if (!testRun) QTimer::singleShot(1200, &w, [&w] { w.offerTours(); });
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
    if (g_claudeSetupThread) g_claudeSetupThread->wait(); // a running `claude mcp add` finishes first
    // Restart after design/language change: only here, when the window is closed and everything has been saved
    // The live server goes first: the new process would otherwise hand its project back to this one, which is quitting
    if (w.restartRequested()) {
        live.reset();
        QProcess::startDetached(QCoreApplication::applicationFilePath(), w.restartArguments());
    }
    return rc;
}
