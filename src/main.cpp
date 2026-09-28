#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QMessageBox>
#include <clocale>

int main(int argc, char* argv[])
{
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
    if (!files.isEmpty()) w.importFiles(files, demo);
    return app.exec();
}
