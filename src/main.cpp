#include "app/InputBindings.h"
#include "core/ProjectFile.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QAction>
#include <QMessageBox>
#include <QFileInfo>
#include <QTimer>
#include <clocale>

int main(int argc, char* argv[])
{
    // Testhilfe: --screenshot bild.png [--wait ms] läuft unsichtbar und stumm (kein Fenster auf dem Desktop),
    // löst --actions aus, speichert das Fensterbild und beendet sich
    QString screenshot;
    int screenshotWait = 4000;
    for (int i = 1; i + 1 < argc; ++i) {
        if (qstrcmp(argv[i], "--screenshot") == 0) screenshot = QString::fromLocal8Bit(argv[i + 1]);
        if (qstrcmp(argv[i], "--wait") == 0) screenshotWait = QByteArray(argv[i + 1]).toInt();
    }
    if (!screenshot.isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("SDL_AUDIODRIVER", "dummy");
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName("schneidi");
    QApplication::setApplicationVersion("0.1.0");

    // MLT liest Kommazahlen mit Punkt – deutsches Locale würde "0.5" als 0 lesen
    std::setlocale(LC_NUMERIC, "C");

    // SDL (Audio-Ausgabe von MLT) soll SIGTERM/SIGINT nicht abfangen, sonst lässt sich
    // das Programm per kill/Strg+C nicht beenden
    qputenv("SDL_NO_SIGNAL_HANDLERS", "1");

    Theme::apply(app);
    InputBindings::instance().load();

    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        QMessageBox::critical(nullptr, "schneidi", error);
        return 1;
    }

    MainWindow w(&engine);
    w.show();

    // Dateien aus der Kommandozeile direkt in den Media Pool
    QStringList files = app.arguments().mid(1);
    const bool demo = files.removeAll("--demo") > 0; // Testhilfe: zusätzlich auf die Timeline legen
    // Testhilfe: --actions select_all,toggle_enabled löst Aktionen (IDs aus keybindings.json) nach dem Start aus
    for (const char* opt : {"--screenshot", "--wait"})
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
    if (!projectFile.isEmpty()) w.openProject(QFileInfo(projectFile).absoluteFilePath());
    else if (!testRun) w.offerAutosaveRestore();
    if (!files.isEmpty()) w.importFiles(files, demo);
    QTimer::singleShot(1500, &w, [actions] {
        for (const QString& id : actions)
            for (const auto& e : InputBindings::instance().entries())
                if (e.id == id && e.action) e.action->trigger();
    });
    if (!screenshot.isEmpty()) {
        w.resize(1600, 950);
        QTimer::singleShot(1500 + screenshotWait, &w, [&w, screenshot] {
            w.grab().save(screenshot);
            QApplication::quit();
        });
    }
    return app.exec();
}
