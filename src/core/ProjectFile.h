#pragma once
// Projektdatei (.schneidi, JSON): speichert nur Verweise auf die Originaldateien und den Schnitt
// (Spuren, Clips, Effekte, Marker) – die Medien selbst werden nie verändert (wie DaVinci).

#include "core/ProjectFormat.h"
#include "core/RenderJob.h"
#include "core/Types.h"

#include <QJsonObject>
#include <QLockFile>
#include <QString>

#include <memory>

struct ProjectData {
    ProjectFormat format; // alte Dateien ohne Angabe: 1920 × 1080, 25 fps
    int playhead = 0;
    QVector<MediaInfo> media;
    QVector<MediaBin> bins; // Media-Pool-Bins (ohne Master)
    Timeline timeline; // geöffnete Timeline (Sequenz currentSequence)
    int lastClipId = 0;
    int lastLinkId = 0;
    QVector<RenderJob> renderQueue; // Deliver: Render-Warteschlange (optional)
    // Alle Sequenzen (Timelines, Compound Clips); die geöffnete bekommt beim Laden `timeline`.
    // Leer (alte Dateien) = nur `timeline` als „Timeline 1“.
    QVector<Sequence> sequences;
    int currentSequence = 0;
};

namespace ProjectFile {

inline constexpr const char* Extension = "schneidi";

// Pfade werden absolut und relativ zur Projektdatei gespeichert, damit ein
// zusammen verschobener Ordner (Projekt + Medien) ohne Neuverknüpfen weiter funktioniert.
QByteArray toJson(const ProjectData& data, const QString& projectPath);
bool fromJson(const QByteArray& json, const QString& projectPath, ProjectData* data, QString* error);

// Ein Clip wie in der Projektdatei, Medienverweis als Pfad (z. B. für Cache-Schlüssel: neue Clip-Felder zählen
// automatisch mit, sobald sie gespeichert werden)
QJsonObject clipJson(const Clip& c);

bool save(const ProjectData& data, const QString& path, QString* error);
// Sicherungskopien wie DaVinci „Project Backups“: vor dem Überschreiben wandert der bisherige Stand in einen
// eigenen Ordner je Projekt (unter AppDataLocation/backups), die neuesten `keep` bleiben. Unveränderter Stand
// (gleich der neuesten Kopie) wird nicht doppelt abgelegt. Liefert den Pfad der neuen Kopie (leer = keine).
QString backup(const QString& projectPath, int keep = 20);
// Ordner der Sicherungskopien eines Projekts (leerer Pfad = Ordner aller Projekte)
QString backupDir(const QString& projectPath);
bool load(const QString& path, ProjectData* data, QString* error);
// Write lock of a project file, so that the app and schneidi-cli never write it at the same time: held from
// reading until the save is done. The lock file lives in the temp folder (not next to the project); a crashed
// holder leaves a stale lock that QLockFile detects. nullptr = still locked by someone else after timeoutMs.
std::unique_ptr<QLockFile> lock(const QString& projectPath, int timeoutMs = 5000);
// Content fingerprint of a saved project file (empty = unreadable): tells own saves from changes by others
QByteArray fingerprint(const QString& path);

// Fehlende Medien: gleichnamige Datei in einem Ordner (rekursiv) suchen und alle Verweise umbiegen.
// Liefert die Anzahl wiedergefundener Dateien.
int relink(ProjectData* data, const QString& searchDir);
QStringList missingMedia(const ProjectData& data);
// Effects this computer does not know (e.g. frei0r plugin missing, project from another computer), sorted: they stay
// in the file with their values and keyframes but have no effect
QStringList missingEffects(const ProjectData& data);
// Dateiname aus einem Pfad beider Systeme ("/home/…/a.mp4", "C:\…\a.mp4"): Backslash ist unter Linux kein
// Trenner, QFileInfo::fileName() liefert dort für Windows-Pfade sonst den ganzen Pfad
QString fileNameAnyOs(const QString& path);

} // namespace ProjectFile
