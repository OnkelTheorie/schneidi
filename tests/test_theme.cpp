// Designs: eingebaute JSON-Dateien vollständig, „Dark Orange“ = Vorgabe im Code, Signalfarben gleich in allen Designs,
// Speichern/Laden der Nutzerwahl (Primär-/Sekundärfarbe je Design, Signalfarben für alle), Design-Dialog.
// DESIGN_DUMP=ordner speichert Bilder des Dialogs je Design.
#include "check.h"

#include "app/DesignDialog.h"
#include "app/Theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {

QJsonObject designJson(const QString& id)
{
    QFile f(":/themes/" + id + ".json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void clearSettings()
{
    QSettings s;
    s.remove("design");
    s.remove("signal");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("theme");
    clearSettings();

    // --- Vorgabe im Code (vor load) = Design „Dark Orange“
    const QColor window0 = Theme::window, primary0 = Theme::primary, lane0 = Theme::lane, playhead0 = Theme::playhead;
    const Theme::Colors dark = Theme::defaults("dark-orange");
    CHECK_EQ(dark.value("window"), window0);
    CHECK_EQ(dark.value("primary"), primary0);
    CHECK_EQ(dark.value("lane"), lane0);
    CHECK_EQ(dark.value("playhead"), playhead0);
    Theme::activate(dark);
    CHECK_EQ(Theme::window, window0);
    CHECK_EQ(Theme::onPrimary, QColor(Qt::black));

    // --- Eingebaute Designs: vorhanden, alle Schlüssel gesetzt und gültig, keine Signalfarben in den Dateien
    const QList<Theme::Design> designs = Theme::designs();
    CHECK_EQ(int(designs.size()), 4);
    CHECK_EQ(Theme::defaultDesign(), QString("dark-orange"));
    const QStringList designKeys = designJson("dark-orange").value("colors").toObject().keys();
    CHECK_EQ(int(designKeys.size()), 23);
    const QStringList signalKeys = Theme::signalKeys();
    for (const Theme::Design& d : designs) {
        const QJsonObject o = designJson(d.id);
        CHECK(!d.name.isEmpty());
        CHECK(!d.description.isEmpty());
        const QJsonObject colors = o.value("colors").toObject();
        CHECK_EQ(colors.keys(), designKeys);
        const Theme::Colors def = Theme::defaults(d.id);
        for (const QString& k : designKeys) {
            const QColor c(colors.value(k).toString());
            if (!CHECK(c.isValid())) std::printf("       %s/%s\n", qPrintable(d.id), qPrintable(k));
            CHECK_EQ(def.value(k), c);
        }
        for (const QString& k : signalKeys) {
            CHECK(!colors.contains(k));
            CHECK_EQ(def.value(k), dark.value(k)); // Signalfarben ändern sich nicht mit dem Design
        }
        // Text muss sich vom Hintergrund abheben (grobe Helligkeitsdifferenz)
        auto y = [](const QColor& c) { return 0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue(); };
        CHECK(std::abs(y(def.value("text")) - y(def.value("panel"))) > 110);
        CHECK(std::abs(y(def.value("text")) - y(def.value("trackHeader"))) > 100);
        CHECK(std::abs(y(def.value("textDim")) - y(def.value("panel"))) > 50);
    }
    CHECK_EQ(Theme::defaults("gibt-es-nicht").value("window"), window0); // unbekannt -> Vorgabe

    // --- Hilfen
    CHECK_EQ(Theme::readableOn(QColor("#e87a3a")), QColor(Qt::black));
    CHECK_EQ(Theme::readableOn(QColor("#2f6fdc")), QColor(Qt::white));
    CHECK_EQ(Theme::mix(QColor(0, 0, 0), QColor(200, 100, 50), 0.5), QColor(100, 50, 25));
    CHECK_EQ(Theme::alpha(QColor(1, 2, 3), 40).alpha(), 40);
    CHECK(Theme::selectionFrom(QColor("#2f6fdc")).lightnessF() >= 0.61);

    // --- Speichern: nur Abweichungen von der Vorgabe; Primär-/Sekundärfarbe je Design, Signalfarben für alle
    CHECK_EQ(Theme::savedDesign(), QString("dark-orange"));
    Theme::Colors mid = Theme::defaults("midnight");
    mid.insert("primary", QColor("#ff00aa"));
    mid.insert("playhead", QColor("#00ff00"));
    Theme::save("midnight", mid);
    {
        QSettings s;
        CHECK_EQ(s.value("design/id").toString(), QString("midnight"));
        CHECK_EQ(s.value("design/midnight/primary").toString(), QString("#ff00aa"));
        CHECK(!s.contains("design/midnight/secondary"));
        CHECK_EQ(s.value("signal/playhead").toString(), QString("#00ff00"));
        CHECK(!s.contains("signal/videoClip"));
    }
    CHECK_EQ(Theme::savedDesign(), QString("midnight"));
    CHECK_EQ(Theme::saved("midnight").value("primary"), QColor("#ff00aa"));
    CHECK_EQ(Theme::saved("graphit").value("primary"), Theme::defaults("graphit").value("primary"));
    CHECK_EQ(Theme::saved("graphit").value("playhead"), QColor("#00ff00"));

    // --- Laden beim Start
    Theme::load();
    CHECK_EQ(Theme::primary, QColor("#ff00aa"));
    CHECK_EQ(Theme::window, Theme::defaults("midnight").value("window"));
    CHECK_EQ(Theme::playhead, QColor("#00ff00"));
    CHECK_EQ(Theme::onPrimary, Theme::readableOn(QColor("#ff00aa")));
    Theme::load("hell"); // --design: nur dieser Lauf
    CHECK_EQ(Theme::window, Theme::defaults("hell").value("window"));
    CHECK_EQ(Theme::onPrimary, QColor(Qt::white));
    CHECK_EQ(Theme::savedDesign(), QString("midnight"));
    Theme::load("gibt-es-nicht");
    CHECK_EQ(Theme::primary, QColor("#ff00aa"));

    // Zurück auf die Vorgabe -> Einträge verschwinden
    Theme::save("midnight", Theme::defaults("midnight"));
    {
        QSettings s;
        CHECK(!s.contains("design/midnight/primary"));
        CHECK(!s.contains("signal/playhead"));
    }
    clearSettings();

    // --- Dialog
    {
        Theme::save("graphit", Theme::defaults("graphit"));
        DesignDialog d;
        CHECK_EQ(d.design(), QString("graphit"));
        d.setColor("primary", QColor("#123456"));
        d.setColor("videoClip", QColor("#654321"));
        CHECK_EQ(d.colors().value("primary"), QColor("#123456"));
        d.selectDesign("hell"); // eigene Farbe gehört zum Design, Signalfarbe bleibt
        CHECK_EQ(d.colors().value("primary"), Theme::defaults("hell").value("primary"));
        CHECK_EQ(d.colors().value("videoClip"), QColor("#654321"));
        d.selectDesign("graphit");
        CHECK_EQ(d.colors().value("primary"), QColor("#123456"));
        d.resetDesignColor("primary");
        CHECK_EQ(d.colors().value("primary"), Theme::defaults("graphit").value("primary"));
        d.setColor("secondary", QColor("#abcdef"));
        d.resetSignals();
        CHECK_EQ(d.colors().value("videoClip"), dark.value("videoClip"));
        d.accept();
        CHECK_EQ(Theme::savedDesign(), QString("graphit"));
        CHECK_EQ(Theme::saved("graphit").value("secondary"), QColor("#abcdef"));

        if (const QString dir = qEnvironmentVariable("DESIGN_DUMP"); !dir.isEmpty()) {
            QDir().mkpath(dir);
            for (const Theme::Design& de : designs) {
                Theme::activate(Theme::defaults(de.id)); // Dialog selbst im jeweiligen Design
                Theme::apply(app);
                DesignDialog shot;
                shot.selectDesign(de.id);
                shot.adjustSize();
                shot.grab().save(QDir(dir).filePath(de.id + ".png"));
            }
        }
    }
    clearSettings();
    return Check::result();
}
