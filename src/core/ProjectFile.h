#pragma once
// Projektdatei (.schneidi, JSON): speichert nur Verweise auf die Originaldateien und den Schnitt
// (Spuren, Clips, Effekte, Marker) – die Medien selbst werden nie verändert (wie DaVinci).

#include "core/ProjectFormat.h"
#include "core/RenderJob.h"
#include "core/Types.h"

#include <QString>

struct ProjectData {
    ProjectFormat format; // alte Dateien ohne Angabe: 1920 × 1080, 25 fps
    int playhead = 0;
    QVector<MediaInfo> media;
    QVector<MediaBin> bins; // Media-Pool-Bins (ohne Master)
    Timeline timeline;
    int lastClipId = 0;
    int lastLinkId = 0;
    QVector<RenderJob> renderQueue; // Deliver: Render-Warteschlange (optional)
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
