#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QAction>
#include <QMessageBox>
#include <QTimer>
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
    // Testhilfe: --actions select_all,toggle_enabled löst Aktionen (IDs aus keybindings.json) nach dem Start aus
    QStringList actions;
    if (const int i = files.indexOf("--actions"); i >= 0 && i + 1 < files.size()) {
        actions = files.at(i + 1).split(',', Qt::SkipEmptyParts);
        files.remove(i, 2);
    }
    if (!files.isEmpty()) w.importFiles(files, demo);
    QTimer::singleShot(1500, &w, [actions] {
        for (const QString& id : actions)
            for (const auto& e : InputBindings::instance().entries())
                if (e.id == id && e.action) e.action->trigger();
    });
    return app.exec();
}
