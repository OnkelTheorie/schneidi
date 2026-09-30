#pragma once
// Log-Datei für Rückmeldungen (unter Windows gibt es kein Terminal): Qt-Meldungen, MLT-Warnungen und
// ffmpeg-Fehler landen in <AppData>/logs/schneidi.log. Beim Start rotiert, die letzten 3 Läufe bleiben.

#include <QString>

namespace Log {

// Nach dem Setzen des Programmnamens aufrufen (Pfad hängt davon ab); leitet weiterhin auf stderr aus
void install();
// MLT-Meldungen (ab Warnung) mitschreiben – nach der MLT-Initialisierung aufrufen
void installMlt();
QString directory();

} // namespace Log
