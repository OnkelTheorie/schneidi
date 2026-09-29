#pragma once
// Mitgelieferte Laufzeit (AppImage unter Linux, später Programmordner unter Windows): MLT-Module/-Daten, frei0r und
// ffmpeg/ffprobe liegen beim Programm statt im System. Gleicher Aufbau auf beiden Systemen:
//   <prefix>/lib/mlt-7, <prefix>/share/mlt-7, <prefix>/lib/frei0r-1, Programme im Programmordner
// mit <prefix> = Programmordner (Windows) bzw. dessen Elternordner (…/usr/bin -> …/usr, AppImage).

#include <QString>

namespace Bundle {

// Ordner mit den mitgelieferten MLT-Modulen, leer = System-MLT benutzen
QString mltModuleDir();
// Setzt MLT_DATA, MLT_PROFILES_PATH und FREI0R_PATH auf die mitgelieferten Ordner (vor Mlt::Factory::init)
void prepareMltEnvironment();
// Pfad eines externen Programms (z. B. "ffmpeg"): erst neben dem Programm, dann im PATH; sonst der bloße Name
QString tool(const QString& name);

} // namespace Bundle
