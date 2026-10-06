#pragma once
// Mitgelieferte Laufzeit (AppImage unter Linux, später Programmordner unter Windows): MLT-Module/-Daten, frei0r und
// ffmpeg/ffprobe liegen beim Programm statt im System. Gleicher Aufbau auf beiden Systemen:
//   <prefix>/lib/mlt-7, <prefix>/share/mlt-7, <prefix>/lib/frei0r-1, Programme im Programmordner
// mit <prefix> = Programmordner (Windows) bzw. dessen Elternordner (…/usr/bin -> …/usr, AppImage).

#include <QByteArray>
#include <QString>

namespace Bundle {

// Ordner mit den mitgelieferten MLT-Modulen, leer = System-MLT benutzen
QString mltModuleDir();
// Setzt MLT_DATA, MLT_PROFILES_PATH und FREI0R_PATH auf die mitgelieferten Ordner (vor Mlt::Factory::init);
// lädt das ungenutzte jackrack-Modul nicht (MLT_REPOSITORY_DENY)
void prepareMltEnvironment();
// Ausführliche Meldungen? Umgebungsvariable SCHNEIDI_VERBOSE (main setzt sie bei --verbose)
bool verboseLogging();
// Direkt nach Mlt::Factory::init: Meldestufen von MLT und FFmpeg. Normal: MLT ab Warnung, FFmpeg nur Fehler;
// ausführlich: MLT ab Info, FFmpeg ab Warnung
void configureMltLogging();
// Pfad so, wie MLT ihn für den Modulordner/Umgebungsvariablen erwartet (Windows: ANSI-Codepage bzw. 8.3-Pfad)
QByteArray pathForMlt(const QString& path);
// Pfad eines externen Programms (z. B. "ffmpeg"): erst neben dem Programm, dann im PATH; sonst der bloße Name
QString tool(const QString& name);

} // namespace Bundle
