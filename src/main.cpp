#include "buildinfo.h"
#include "app/InputBindings.h"
#include "app/Log.h"
#include "core/ProjectFile.h"
#include "app/MainWindow.h"
#include "cli/Live.h"
#include "ui/MediaStorage.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPainter>
#include <QSplashScreen>
#include <QProcess>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <clocale>

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
    // --verbose: detailed MLT/FFmpeg messages on the console (otherwise only errors, see Bundle::configureMltLogging)
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--verbose") == 0) qputenv("SCHNEIDI_VERBOSE", "1");
    if (!screenshot.isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("SDL_AUDIODRIVER", "dummy");
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
    QApplication::setApplicationVersion(QString("%1 (%2)").arg(SCHNEIDI_VERSION, SCHNEIDI_BUILD)); // version from CMakeLists, Git state, see cmake/BuildInfo.cmake
    Log::install();
    // Test aid: --lang <code> (en, de, fr, …) only applies to this run (the setting stays unchanged)
    I18n::install(langOverride);

    // MLT reads decimal numbers with a dot - a German locale would read "0.5" as 0
    std::setlocale(LC_NUMERIC, "C");

    // SDL (MLT's audio output) should not catch SIGTERM/SIGINT, otherwise
    // the program cannot be ended with kill/Ctrl+C
    qputenv("SDL_NO_SIGNAL_HANDLERS", "1");

    Theme::load(designArg);
    Theme::apply(app);
    InputBindings::instance().load();

    // Splash window: loading the MLT modules takes a while (clearly on Windows at first start, virus scanner)
    std::unique_ptr<QSplashScreen> splash;
    const auto status = [&splash](const QString& text) {
        if (!splash) return;
        splash->showMessage("  " + text + "\n", Qt::AlignLeft | Qt::AlignBottom, Theme::textDim);
        QCoreApplication::processEvents();
    };
    if (screenshot.isEmpty()) {
        splash = std::make_unique<QSplashScreen>(splashPixmap());
        splash->show();
        status(T("Lade Module…"));
    }

    Log::installMlt(); // before MLT initialization: missing modules also end up in the log
    Engine::applyAudioSettings(); // before MLT initialization (SDL reads the audio driver at startup)
    Engine engine;
    QString error;
    const qint64 beforeMlt = startTimer.elapsed();
    if (!engine.init(&error)) {
        splash.reset();
        QMessageBox::critical(nullptr, "schneidi", error);
        return 1;
    }
    const qint64 afterMlt = startTimer.elapsed();
    status(T("Öffne Fenster…"));

    MainWindow w(&engine);
    // Start maximized like DaVinci; screenshot test runs (offscreen) keep the fixed size from MainWindow
    if (app.arguments().contains("--screenshot")) w.show();
    else w.showMaximized();
    if (splash) splash->finish(&w);
    qInfo("Startzeit: MLT %lld ms, Fenster sichtbar nach %lld ms", afterMlt - beforeMlt, startTimer.elapsed());

    // Files from the command line directly into the Media Pool
    QStringList files = app.arguments().mid(1);
    files.removeAll("--verbose");
    const bool demo = files.removeAll("--demo") > 0; // test helper: additionally place on the timeline
    // Test helper: --actions select_all,toggle_enabled triggers actions (IDs from keybindings.json) after startup
    for (const char* opt : {"--screenshot", "--wait", "--lang", "--format", "--storage", "--design", "--size"})
        if (const int i = files.indexOf(opt); i >= 0) files.remove(i, std::min<qsizetype>(2, files.size() - i));
    QStringList actions;
    if (const int i = files.indexOf("--actions"); i >= 0 && i + 1 < files.size()) {
        actions = files.at(i + 1).split(',', Qt::SkipEmptyParts);
        files.remove(i, 2);
    }
    // .schneidi file as argument = open project (e.g. double-click in the file manager)
    QString projectFile;
    for (const QString& f : files)
        if (f.endsWith(QString(".%1").arg(ProjectFile::Extension))) projectFile = f;
    files.removeAll(projectFile);
    // Test runs (--demo/--actions) leave the user's automatic backup alone
    const bool testRun = demo || !actions.isEmpty() || !screenshot.isEmpty();
    if (testRun) w.disableAutosave();
    // First offer the backup after a crash - otherwise opening by double-click deletes it without asking.
    // Restored: this project stays open (the file can then be opened normally).
    const bool restored = !testRun && w.offerAutosaveRestore();
    if (!projectFile.isEmpty() && !restored) w.openProject(QFileInfo(projectFile).absoluteFilePath());
    // Live connection for schneidi-cli / MCP (cli/Live.h); test runs stay invisible to AIs unless SCHNEIDI_LIVE=1
    std::unique_ptr<Cli::Live::Server> live;
    if (qEnvironmentVariable("SCHNEIDI_LIVE", testRun ? "0" : "1") == "1") {
        live = std::make_unique<Cli::Live::Server>(&w);
        QString liveError;
        if (live->start(&liveError)) {
            Cli::Live::Server* server = live.get();
            w.setLiveControls([server] { server->disconnectClient(); }, [server] { server->setAllowed(true); });
        } else {
            qWarning("Live connection not available: %s", qPrintable(liveError));
            live.reset();
        }
    }
    if (!formatArg.isEmpty()) {
        const QStringList parts = formatArg.split(QRegularExpression("[x@]"));
        ProjectFormat f;
        f.width = parts.value(0).toInt() & ~1;
        f.height = parts.value(1).toInt() & ~1;
        f.rate = nearestFrameRate(parts.value(2, "25").toDouble());
        if (f.width >= 16 && f.height >= 16) w.setProjectFormat(f);
    }
    if (!files.isEmpty()) w.importFiles(files, demo, formatArg.isEmpty());
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
    if (w.restartRequested()) QProcess::startDetached(QCoreApplication::applicationFilePath(), w.restartArguments());
    return rc;
}
