#pragma once
// Deliver-Seite wie DaVinci: Render-Einstellungen, Vorlagen (eingebaut + eigene) und Render-Warteschlange.
// Nur Daten + JSON; gerendert wird in engine/RenderQueue (über den Exporter).

#include <QJsonArray>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVector>

// Ausgabeformat = Container + Codecs (wie DaVincis Format/Codec-Paare, abgespeckt)
struct RenderFormatInfo {
    const char* id;         // in Projekt/Vorlagen gespeichert
    const char* label;      // Anzeige (N_ markiert, wo übersetzt)
    const char* extension;  // Dateiendung
    const char* videoCodec; // "" = nur Audio
    const char* audioCodec;
    const char* pixFmt;     // Pixelformat des Videos
    bool hasQuality;        // CRF-Stufen (H.264/H.265); ProRes hat feste Qualität
};
const QVector<RenderFormatInfo>& renderFormats();
const RenderFormatInfo& renderFormat(const QString& id); // unbekannt -> H.264

struct RenderSettings {
    QString format = "h264";
    int shortSide = 0; // Auflösung: 0 = Timeline, sonst kürzere Bildkante im Seitenverhältnis der Timeline
    QSize size;        // feste Ausgabegröße (z. B. Hochformat 1080 × 1920); hat Vorrang vor shortSide
    int quality = 0;   // 0 = Hoch, 1 = Mittel, 2 = Klein (nur H.264/H.265)
    int audioBitrateK = 320; // nur AAC
    // Untertitel exportieren wie DaVinci (sichtbare Untertitelspur): keine, ins Bild einbrennen, als SRT-Datei daneben
    enum Subtitles { NoSubtitles = 0, BurnSubtitles = 1, SrtFile = 2 };
    int subtitles = NoSubtitles;

    bool audioOnly() const;
    QSize outputSize(QSize timeline) const; // gerade Maße
    int crf() const;
    QString qualityLabel() const;
    // Kurzbeschreibung für die Warteschlange, z. B. "H.264 (MP4) · 1920 × 1080 · Hoch"
    QString summary(QSize output) const;

    QJsonObject toJson() const;
    static RenderSettings fromJson(const QJsonObject& o);
    bool operator==(const RenderSettings& o) const;
    bool operator!=(const RenderSettings& o) const { return !(*this == o); }
};

// Ausgabegröße mit dem Seitenverhältnis der Timeline und gegebener kürzerer Kante (gerade Maße)
QSize sizeForShortSide(QSize timeline, int shortSide);

struct RenderPreset {
    QString name;
    RenderSettings settings;
    bool builtin = false;
};

namespace RenderPresets {
QVector<RenderPreset> builtins();
// Eigene Vorlagen: JSON in QStandardPaths::AppConfigLocation ("render-presets.json")
QString userFile();
QVector<RenderPreset> loadUser();
bool saveUser(const QVector<RenderPreset>& presets);
QVector<RenderPreset> all(); // eingebaute, dann eigene
} // namespace RenderPresets

enum class RenderStatus { Queued, Rendering, Done, Failed, Canceled };

// Ein Auftrag der Render-Warteschlange (wie DaVinci: Einstellungen und Bereich werden beim Hinzufügen festgehalten,
// gerendert wird die Timeline, wie sie beim Rendern ist)
struct RenderJob {
    int id = 0;
    QString path;   // Zieldatei (absolut)
    QString preset; // Name der Vorlage beim Hinzufügen (Anzeige), leer = eigene Einstellungen
    RenderSettings settings;
    QSize size;          // Ausgabegröße (leer bei nur Audio)
    bool inOut = false;  // In/Out-Bereich statt ganzer Timeline
    int from = 0, to = -1; // Bereich (Frames, inklusive), to < 0 = bis zum Ende
    RenderStatus status = RenderStatus::Queued;
    QString message;     // Fehlertext bzw. Hinweis
    qint64 renderMs = 0; // Dauer des letzten Renderns

    bool operator==(const RenderJob& o) const;
};

namespace RenderQueueJson {
QJsonArray toJson(const QVector<RenderJob>& jobs);
// "rendering" (Programm während des Renderns beendet) wird wieder "wartet"
QVector<RenderJob> fromJson(const QJsonArray& arr);
} // namespace RenderQueueJson

QString renderStatusText(const RenderJob& job); // "Wartet", "Fertig (0:12)", "Fehler: …"
