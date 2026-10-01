#pragma once
// Effekte importieren: Ordner im Nutzerverzeichnis (QStandardPaths::AppDataLocation/effects), in den man fremde
// Effekte legt, plus die mitgelieferten (Kategorie „schneidi“). Bisher LUTs (Unterordner „LUTs“, mitgeliefert als
// Qt-Ressource ":/luts/<id>.cube") und Verlaufsblenden (Unterordner „Transitions“: Graustufenbilder wie in
// Kdenlive/Shotcut/OpenShot, mitgeliefert als ":/lumas/<id>", erzeugt von engine/Lumas). Unterordner werden zu
// Kategorien der Effects Library.

#include <QString>
#include <QStringList>
#include <QVector>

namespace EffectFolders {

// Eintrag der Effects Library / Drag-Daten für eine LUT: "lut:<Pfad>" (statt einer Effekt-ID)
inline constexpr const char* LutPrefix = "lut:";

QString root();   // Effekte-Ordner
QString lutDir(); // root/LUTs
QString transitionDir(); // root/Transitions (ASCII-Name: MLT/FFmpeg unter Windows)
void ensure();    // Ordner (und Liesmich) anlegen, falls sie fehlen

// LUT-Formate (Endungen klein, ohne Punkt): .cube, .3dl, .csp, Hald-CLUT-Bilder
const QStringList& lutSuffixes();
bool isLutFile(const QString& path);
QString lutFileFilter(); // für QFileDialog

// Verlaufsbilder: Graustufen (dunkle Stellen wechseln zuerst), alle gängigen Bildformate
const QStringList& transitionSuffixes();
bool isTransitionFile(const QString& path);

struct LutEntry {
    QString path;  // Datei bzw. ":/luts/<id>.cube" / ":/lumas/<id>"
    QString name;  // Anzeige
    QString group; // Unterordner relativ zu lutDir() ("/" getrennt), leer = direkt im Ordner
};
QVector<LutEntry> builtinLuts(); // mitgeliefert, Namen übersetzt
QVector<LutEntry> userLuts();    // lutDir() rekursiv, sortiert nach Gruppe und Name
QVector<LutEntry> builtinTransitions(); // mitgelieferte Verlaufsblenden
QVector<LutEntry> userTransitions();    // transitionDir() rekursiv
QStringList watchDirs();                // LUTs-/Transitions-Ordner und alle Unterordner (zum Beobachten)
bool isBuiltin(const QString& path);
QString builtinId(const QString& path);  // ":/lumas/kreis" -> "kreis"
QString displayName(const QString& path); // mitgeliefert: übersetzter Name, sonst Dateiname (LUT) bzw. ohne Endung
// Projekt von einem anderen Rechner: gleichnamige Datei im LUTs- bzw. Transitions-Ordner suchen, leer = keine
QString findUserLut(const QString& fileName);
QString findUserTransition(const QString& fileName);

} // namespace EffectFolders
