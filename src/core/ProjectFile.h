#pragma once
// Projektdatei (.schneidi, JSON): speichert nur Verweise auf die Originaldateien und den Schnitt
// (Spuren, Clips, Effekte, Marker) – die Medien selbst werden nie verändert (wie DaVinci).

#include "core/Types.h"

#include <QString>

struct ProjectData {
    int fps = 25;
    int playhead = 0;
    QVector<MediaInfo> media;
    Timeline timeline;
    int lastClipId = 0;
    int lastLinkId = 0;
};

namespace ProjectFile {

inline constexpr const char* Extension = "schneidi";

// Pfade werden absolut und relativ zur Projektdatei gespeichert, damit ein
// zusammen verschobener Ordner (Projekt + Medien) ohne Neuverknüpfen weiter funktioniert.
QByteArray toJson(const ProjectData& data, const QString& projectPath);
bool fromJson(const QByteArray& json, const QString& projectPath, ProjectData* data, QString* error);

bool save(const ProjectData& data, const QString& path, QString* error);
bool load(const QString& path, ProjectData* data, QString* error);

// Fehlende Medien: gleichnamige Datei in einem Ordner (rekursiv) suchen und alle Verweise umbiegen.
// Liefert die Anzahl wiedergefundener Dateien.
int relink(ProjectData* data, const QString& searchDir);
QStringList missingMedia(const ProjectData& data);

} // namespace ProjectFile
