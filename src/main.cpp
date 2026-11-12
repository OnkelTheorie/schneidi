// SPDX-License-Identifier: AGPL-3.0-only
#include "buildinfo.h"
#include "app/CrashReport.h"
#include "app/DisplayLoss.h"
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
#include "engine/video/Reader.h"

#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
#include <QDir>
#include <QLibrary>
#include <QLockFile>
#include <QMessageBox>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QProcess>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QPointer>
#include <QPushButton>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>
#include <clocale>
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
#include <dlfcn.h>
#endif
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

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

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
// KDE's platform theme (plasma-integration) checks while the QApplication is made whether OpenGL works for Qt Quick:
// a throwaway OpenGL context, 250-350 ms with the NVIDIA driver, before anything of schneidi shows. schneidi has no
// Qt Quick (its OpenGL is the viewer's own QRhi), and the theme skips the check when a Qt Quick backend is already
// chosen: chosen here, before the QApplication. Only where that theme loads (it brings Qt Quick along anyway), and
// without linking Qt Quick: the symbol is looked up, nothing happens without it.
void skipKdeQuickOpenGlCheck(int argc, char* argv[])
{
    // Without a screen (offscreen/minimal: test runs, --screenshot) there is no OpenGL check to skip
    QByteArray platform = qgetenv("QT_QPA_PLATFORM");
    for (int i = 1; i + 1 < argc; ++i)
        if (qstrcmp(argv[i], "-platform") == 0 || qstrcmp(argv[i], "--platform") == 0) platform = argv[i + 1];
    if (platform.startsWith("offscreen") || platform.startsWith("minimal")) return;
    const QByteArray theme = qgetenv("QT_QPA_PLATFORMTHEME").toLower();
    const bool kde = theme.isEmpty() ? qgetenv("XDG_CURRENT_DESKTOP").toUpper().contains("KDE")
                                           || !qEnvironmentVariableIsEmpty("KDE_FULL_SESSION")
                                     : theme.contains("kde");
    if (!kde) return;
    // The Qt Quick of this Qt only (next to its Qt Core: a bundled Qt never mixes with the system's; where Qt's
    // libraries live in separate folders the check simply stays)
    // (looked up in Qt Core itself: &qVersion in the program may be its own call stub)
    void* coreLib = dlopen("libQt6Core.so.6", RTLD_LAZY | RTLD_NOLOAD);
    void* coreSymbol = coreLib ? dlsym(coreLib, "qVersion") : nullptr;
    Dl_info core{};
    const bool found = coreSymbol && dladdr(coreSymbol, &core) && core.dli_fname;
    if (coreLib) dlclose(coreLib); // NOLOAD: only the reference this took
    if (!found) return;
    QLibrary quick(QFileInfo(QString::fromLocal8Bit(core.dli_fname)).absoluteDir().filePath(
        QStringLiteral("libQt6Quick.so.6")));
    using SetBackend = void (*)(const QString&);
    // static void QQuickWindow::setSceneGraphBackend(const QString&)
    if (auto set = reinterpret_cast<SetBackend>(quick.resolve("_ZN12QQuickWindow20setSceneGraphBackendERK7QString")))
        set(QStringLiteral("rhi")); // Qt Quick's default backend, named
}
#endif

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

// Work that must not delay the main window's first picture: run once the window is painted, at the latest after
// kAfterFirstPaintMaxMs (a window that is never painted, e.g. started minimized)
constexpr int kAfterFirstPaintMaxMs = 3000;
std::vector<std::function<void()>> g_afterFirstPaint;
bool g_firstPaintDone = false;
QElapsedTimer g_sinceFirstPaint;

void afterFirstPaint(std::function<void()> job)
{
    if (g_firstPaintDone) QTimer::singleShot(0, QCoreApplication::instance(), std::move(job));
    else g_afterFirstPaint.push_back(std::move(job));
}

void firstPaintDone()
{
    if (std::exchange(g_firstPaintDone, true)) return;
    g_sinceFirstPaint.start();
    // Each on its own event loop turn: the painted frame reaches the screen first
    for (std::function<void()>& job : std::exchange(g_afterFirstPaint, {}))
        QTimer::singleShot(0, QCoreApplication::instance(), std::move(job));
}

// The window manager animates a new window for ~150-300 ms after its first picture (KWin: from ~150 ms after the
// first paint on). Making an OpenGL context in that time - on any thread - stalls the X server with the NVIDIA driver,
// and the animation stutters (measured: frames 50-90 ms apart); a busy GUI thread alone or making a Vulkan device does
// not. The OpenGL setup waits until the animation is over.
constexpr int kOpenAnimationMs = 450;

void afterOpenAnimation(std::function<void()> job)
{
    afterFirstPaint([job = std::move(job)]() mutable {
        const qint64 left = kOpenAnimationMs - g_sinceFirstPaint.elapsed();
        QTimer::singleShot(std::max<qint64>(0, left), QCoreApplication::instance(), std::move(job));
    });
}

// The GPU picture engine's OpenGL surface (Picture::Gpu::Service::setDeferredPrepare) after the open animation and
// after the viewer's GPU surface (both at once would add up into one longer stall)
std::function<void()> g_enginePrepare;
bool g_viewerAttached = false, g_openAnimationOver = false;

void prepareEngineWhenFree()
{
    if (g_viewerAttached && g_openAnimationOver && g_enginePrepare)
        QTimer::singleShot(0, QCoreApplication::instance(), std::exchange(g_enginePrepare, {}));
}

// The viewer's deferred GPU surface (FrameView::attachDeferredSurfaces) after the open animation: Vulkan stalls the
// GUI thread ~50-150 ms (its drivers were loaded meanwhile, FrameView::warmUpGpu), OpenGL ~300 ms (two contexts).
// Until then the viewer paints with QPainter - a project's first picture shows that way without waiting for it.
void attachViewerSurface()
{
    afterOpenAnimation([] {
        FrameView::attachDeferredSurfaces();
        StartupTrace::mark("viewer GPU surface attached");
        g_viewerAttached = true;
        prepareEngineWhenFree();
    });
}

// The main window's first picture (afterFirstPaint): its first expose, one event loop turn later (Qt paints and
// flushes the exposed window while handling the expose; the window's own Paint may never come when children cover
// it, and children paint during show() already). Seen app-wide until then; SCHNEIDI_STARTUP_TRACE also marks the
// first Paint after the expose.
class FirstPaintWatch : public QObject {
public:
    explicit FirstPaintWatch(QWidget* window) : QObject(window), m_window(window)
    {
        QCoreApplication::instance()->installEventFilter(this);
        // SCHNEIDI_STARTUP_TRACE=2: a 5 ms ticker lists every stall of the GUI thread from here on (made before the
        // window shows: the first gap is the way from show() to the event loop) - what the user sees as a stuttering
        // open animation or a frozen window right after it appeared
        if (qEnvironmentVariableIntValue("SCHNEIDI_STARTUP_TRACE") == 2) {
            auto* ticker = new QTimer(this);
            ticker->setTimerType(Qt::PreciseTimer);
            ticker->setInterval(5);
            connect(ticker, &QTimer::timeout, this, [this] {
                const qint64 now = StartupTrace::elapsed();
                if (now - m_lastTick > kStallMs) m_stalls.push_back({m_lastTick, now});
                // After the first expose (its own paint counts)
                if (m_exposedAt >= 0 && now - std::max(m_lastTick, m_exposedAt) > m_longestStall) {
                    m_longestStall = now - std::max(m_lastTick, m_exposedAt);
                    m_stallEnd = now;
                }
                m_lastTick = now;
            });
            m_lastTick = StartupTrace::elapsed();
            ticker->start();
        }
    }
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::Expose && !m_exposed && o == m_window->windowHandle()
            && m_window->windowHandle()->isExposed()) {
            m_exposed = true;
            StartupTrace::mark("first expose");
            QTimer::singleShot(0, this, [] {
                StartupTrace::mark("event loop idle after first paint");
                firstPaintDone();
            });
            // SCHNEIDI_STARTUP_TRACE=2: quits by itself (measuring runs), after the work deferred past the paint;
            // exit(), not quit(): no window is asked to close (no question about unsaved changes). Then it lists the
            // ticker's stalls and the longest one after the first expose.
            if (qEnvironmentVariableIntValue("SCHNEIDI_STARTUP_TRACE") == 2) {
                m_exposedAt = StartupTrace::elapsed();
                QTimer::singleShot(2500, qApp, [this] {
                    for (const auto& [from, to] : m_stalls)
                        std::fprintf(stderr, "startup: GUI stall %lld..%lld ms (%lld ms)\n", static_cast<long long>(from),
                                     static_cast<long long>(to), static_cast<long long>(to - from));
                    std::fprintf(stderr, "startup: longest GUI stall after first expose %lld ms (ended at %lld ms)\n",
                                 static_cast<long long>(m_longestStall), static_cast<long long>(m_stallEnd));
                    StartupTrace::mark("quit (SCHNEIDI_STARTUP_TRACE=2)");
                    QCoreApplication::exit(0);
                });
            }
            if (!StartupTrace::enabled()) QCoreApplication::instance()->removeEventFilter(this);
        } else if (e->type() == QEvent::Paint && m_exposed && o->isWidgetType()
                   && static_cast<QWidget*>(o)->window() == m_window) {
            StartupTrace::mark("first paint (main window)");
            QCoreApplication::instance()->removeEventFilter(this);
        }
        return false;
    }

private:
    QWidget* m_window;
    bool m_exposed = false;
    // SCHNEIDI_STARTUP_TRACE=2
    static constexpr qint64 kStallMs = 16; // one frame at 60 Hz
    qint64 m_lastTick = -1, m_exposedAt = -1, m_longestStall = 0, m_stallEnd = 0;
    std::vector<std::pair<qint64, qint64>> m_stalls; // [from, to] in ms since main()
};

// Started with a project (a double-click on a .schneidi file) or media: the video files of the first picture open
// on helper threads while the window is built (Video::Reader::prepare) - the picture engine takes them instead of
// opening each file only once the window is up. The project is read once more here (a few ms).
void prepareFirstPicture(const QStringList& files)
{
    QString project;
    for (const QString& f : files)
        if (f.endsWith(QString(".%1").arg(ProjectFile::Extension))) project = f;
    QStringList videos;
    if (!project.isEmpty()) {
        ProjectData data;
        QString error;
        if (!ProjectFile::load(QFileInfo(project).absoluteFilePath(), &data, &error)) return;
        for (const Track& t : data.timeline.video)
            for (const Clip& c : t.hidden ? QVector<Clip>{} : t.clips)
                if (c.kind == ClipKind::Media && c.enabled && !c.mediaPath.isEmpty() && c.start <= data.playhead
                    && data.playhead < c.end() && !videos.contains(c.mediaPath))
                    videos << c.mediaPath;
    } else if (!files.isEmpty()) {
        videos << QFileInfo(files.first()).absoluteFilePath(); // a new project of the clip, shown from its start
    }
    // The picture engine's preview readers keep a seek window (the size is the taker's)
    Video::Reader::Options preview;
    preview.keepBytes = 1;
    for (const QString& v : videos)
        if (QImageReader::imageFormat(v).isEmpty()) Video::Reader::prepare(v, preview); // stills are read another way
}

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
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID)
    skipKdeQuickOpenGlCheck(argc, argv); // (--screenshot set the offscreen platform above)
#endif
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
    DisplayLoss::install(); // after the log: the display gone ends the program at once, logged (app/DisplayLoss.h)
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
    // The GPU picture engine's OpenGL surface from the event loop, not on the way to the first window and not while it
    // animates open
    Picture::Gpu::Service::setDeferredPrepare(true, [](std::function<void()> job) {
        g_enginePrepare = std::move(job);
        prepareEngineWhenFree();
    });
    afterOpenAnimation([] {
        g_openAnimationOver = true;
        prepareEngineWhenFree();
    });
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
    // The viewer trusts the last run's GPU check; the real one comes after the window (X11: made on a helper thread
    // while the window is built, warmUpGpu; elsewhere a throwaway device once the window is up)
    FrameView::setUseCachedGpuCheck(true);
    FrameView::warmUpGpu(); // while the window is built
    // Started with a project or media (a double-click on a .schneidi file): the video decoder opens beside the
    // window's setup; otherwise a while after the window settled (it costs video memory, wakes a laptop's card)
    if (const QStringList launchFiles = LaunchArgs::files(app.arguments().mid(1)); !launchFiles.isEmpty()) {
        MainWindow::warmUpVideoDecoding();
        if (screenshot.isEmpty()) prepareFirstPicture(launchFiles);
    } else afterOpenAnimation([] { QTimer::singleShot(1500, qApp, [] { MainWindow::warmUpVideoDecoding(); }); });
    MainWindow w(&engine);
    StartupTrace::mark("MainWindow constructed");
    // Menu entries that map onto an edit op of the command layer ("Musik auf Länge…"): the user's own undo step
    w.setUserRunner([&w](const QString& command, const QJsonObject& args, const QString& undoText) {
        return Cli::Live::runForUser(&w, command, args, undoText);
    });
    new FirstPaintWatch(&w);
    QTimer::singleShot(kAfterFirstPaintMaxMs, &w, [] { firstPaintDone(); });
    // The viewer's GPU surface follows the open animation (X11: the window's GPU setup no longer delays the first
    // picture); the window is made as a GPU (Vulkan/OpenGL) window right away so that it can take the surface in place
    FrameView::prepareWindow(&w);
    attachViewerSurface();
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
    StartupTrace::mark("live server");
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
    StartupTrace::mark("autosave restore check");
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
    StartupTrace::mark("event loop starts");
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
