#include "buildinfo.h"
#include "app/InputBindings.h"
#include "app/Log.h"
#include "core/ProjectFile.h"
#include "app/MainWindow.h"
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

// Ladefenster: Logo (wie assets/schneidi.svg, von Hand gezeichnet – ohne QtSvg), Name und Version in den Designfarben
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
    // Testhilfe: --screenshot bild.png [--wait ms] läuft unsichtbar und stumm (kein Fenster auf dem Desktop),
    // löst --actions aus, speichert das Fensterbild und beendet sich
    QString screenshot;
    int screenshotWait = 4000;
    QString langOverride;
    QString formatArg; // Testhilfe: --format 1080x1920@30 (Projekteinstellungen für diesen Lauf)
    QString storageArg; // Testhilfe: --storage <Ordner>
    QString designArg;  // Testhilfe: --design midnight (nur für diesen Lauf)
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--format") == 0) formatArg = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--screenshot") == 0) screenshot = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--wait") == 0) screenshotWait = QByteArray(argv[i + 1]).toInt();
        if (qstrcmp(argv[i], "--lang") == 0) langOverride = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--storage") == 0) storageArg = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--design") == 0) designArg = QString::fromLocal8Bit(argv[i + 1]);
    }
    if (!screenshot.isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("SDL_AUDIODRIVER", "dummy");
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName("schneidi");
#ifdef Q_OS_WIN
    // Ohne Organisationsnamen kann QSettings unter Windows nicht in die Registry schreiben (unter Linux greift
    // „Unknown Organization“ als Ersatz – dort bleibt es dabei, sonst wären vorhandene Einstellungen weg)
    QApplication::setOrganizationName("schneidi");
#endif
    QApplication::setApplicationVersion(QString("%1 (%2)").arg(SCHNEIDI_VERSION, SCHNEIDI_BUILD)); // Version aus CMakeLists, Git-Stand, siehe cmake/BuildInfo.cmake
    Log::install();
    // Testhilfe: --lang en|de gilt nur für diesen Lauf (Einstellung bleibt unverändert)
    I18n::install(langOverride);

    // MLT liest Kommazahlen mit Punkt – deutsches Locale würde "0.5" als 0 lesen
    std::setlocale(LC_NUMERIC, "C");

    // SDL (Audio-Ausgabe von MLT) soll SIGTERM/SIGINT nicht abfangen, sonst lässt sich
    // das Programm per kill/Strg+C nicht beenden
    qputenv("SDL_NO_SIGNAL_HANDLERS", "1");

    Theme::load(designArg);
    Theme::apply(app);
    InputBindings::instance().load();

    // Ladefenster: das Laden der MLT-Module dauert (unter Windows beim ersten Start deutlich, Virenscanner)
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

    Log::installMlt(); // vor der MLT-Initialisierung: fehlende Module landen auch im Log
    Engine::applyAudioSettings(); // vor der MLT-Initialisierung (SDL liest den Audiotreiber beim Start)
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
    // Wie DaVinci maximiert starten; Screenshot-Testläufe (offscreen) behalten die feste Größe aus MainWindow
    if (app.arguments().contains("--screenshot")) w.show();
    else w.showMaximized();
    if (splash) splash->finish(&w);
    qInfo("Startzeit: MLT %lld ms, Fenster sichtbar nach %lld ms", afterMlt - beforeMlt, startTimer.elapsed());

    // Dateien aus der Kommandozeile direkt in den Media Pool
    QStringList files = app.arguments().mid(1);
    const bool demo = files.removeAll("--demo") > 0; // Testhilfe: zusätzlich auf die Timeline legen
    // Testhilfe: --actions select_all,toggle_enabled löst Aktionen (IDs aus keybindings.json) nach dem Start aus
    for (const char* opt : {"--screenshot", "--wait", "--lang", "--format", "--storage", "--design", "--size"})
        if (const int i = files.indexOf(opt); i >= 0) files.remove(i, std::min<qsizetype>(2, files.size() - i));
    QStringList actions;
    if (const int i = files.indexOf("--actions"); i >= 0 && i + 1 < files.size()) {
        actions = files.at(i + 1).split(',', Qt::SkipEmptyParts);
        files.remove(i, 2);
    }
    // .schneidi-Datei als Argument = Projekt öffnen (z. B. Doppelklick im Dateimanager)
    QString projectFile;
    for (const QString& f : files)
        if (f.endsWith(QString(".%1").arg(ProjectFile::Extension))) projectFile = f;
    files.removeAll(projectFile);
    // Testläufe (--demo/--actions) lassen die automatische Sicherung des Nutzers in Ruhe
    const bool testRun = demo || !actions.isEmpty() || !screenshot.isEmpty();
    if (testRun) w.disableAutosave();
    // Erst die Sicherung nach einem Absturz anbieten – sonst löscht das Öffnen per Doppelklick sie ungefragt.
    // Wiederhergestellt: dieses Projekt bleibt offen (die Datei lässt sich danach normal öffnen).
    const bool restored = !testRun && w.offerAutosaveRestore();
    if (!projectFile.isEmpty() && !restored) w.openProject(QFileInfo(projectFile).absoluteFilePath());
    if (!formatArg.isEmpty()) {
        const QStringList parts = formatArg.split(QRegularExpression("[x@]"));
        ProjectFormat f;
        f.width = parts.value(0).toInt() & ~1;
        f.height = parts.value(1).toInt() & ~1;
        f.rate = nearestFrameRate(parts.value(2, "25").toDouble());
        if (f.width >= 16 && f.height >= 16) w.setProjectFormat(f);
    }
    if (!files.isEmpty()) w.importFiles(files, demo, formatArg.isEmpty());
    // Testhilfe: --storage <Ordner> öffnet den Ordner im Media Storage (wird nicht gespeichert)
    if (!storageArg.isEmpty())
        if (auto* storage = w.findChild<MediaStorage*>()) storage->setFolder(QFileInfo(storageArg).absoluteFilePath());
    QTimer::singleShot(1500, &w, [actions] {
        for (const QString& id : actions)
            for (const auto& e : InputBindings::instance().entries())
                if (e.id == id && e.action) e.action->trigger();
    });
    if (!screenshot.isEmpty()) {
        // --size 2560x1440: Fenstergröße im Testlauf (z. B. Layout im Vollbild prüfen)
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
    // Neustart nach Design-/Sprachwechsel: erst hier, wenn das Fenster zu ist und alles gespeichert wurde
    if (w.restartRequested()) QProcess::startDetached(QCoreApplication::applicationFilePath(), w.restartArguments());
    return rc;
}
