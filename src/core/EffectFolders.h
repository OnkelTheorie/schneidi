#pragma once
// Effekte importieren: Ordner im Nutzerverzeichnis (QStandardPaths::AppDataLocation/effects), in den man fremde
// Effekte legt, plus die mitgelieferten Looks (Kategorie „schneidi“, Qt-Ressource ":/luts/<id>.cube").
// Bisher nur LUTs (Unterordner „LUTs“, dessen Unterordner werden zu Kategorien der Effects Library).

#include <QString>
#include <QStringList>
#include <QVector>

namespace EffectFolders {

// Eintrag der Effects Library / Drag-Daten für eine LUT: "lut:<Pfad>" (statt einer Effekt-ID)
inline constexpr const char* LutPrefix = "lut:";

QString root();   // Effekte-Ordner
QString lutDir(); // root/LUTs
void ensure();    // Ordner (und Liesmich) anlegen, falls sie fehlen

// LUT-Formate (Endungen klein, ohne Punkt): .cube, .3dl, .csp, Hald-CLUT-Bilder
const QStringList& lutSuffixes();
bool isLutFile(const QString& path);
QString lutFileFilter(); // für QFileDialog

struct LutEntry {
    QString path;  // Datei bzw. ":/luts/<id>.cube"
    QString name;  // Anzeige
    QString group; // Unterordner relativ zu lutDir() ("/" getrennt), leer = direkt im Ordner
};
QVector<LutEntry> builtinLuts(); // mitgeliefert, Namen übersetzt
QVector<LutEntry> userLuts();    // lutDir() rekursiv, sortiert nach Gruppe und Name
QStringList userLutDirs();       // lutDir() und alle Unterordner (zum Beobachten)
bool isBuiltin(const QString& path);
QString displayName(const QString& lutPath); // mitgeliefert: übersetzter Name, sonst Dateiname
// Projekt von einem anderen Rechner: gleichnamige Datei im LUTs-Ordner suchen, leer = keine
QString findUserLut(const QString& fileName);

} // namespace EffectFolders
