#pragma once
// Render-Cache wie DaVinci („Render Cache Clip Output“ im Rechtsklick-Menü eines Timeline-Clips,
// Wiedergabe > Render-Cache: Aus / Smart / Benutzer). Ein Videoclip wird mit allen Clip-Effekten, Transform,
// Geschwindigkeit und Keyframes im Hintergrund in eine schnell dekodierbare Datei (ProRes, Projektauflösung)
// gerendert; die Vorschau spielt dann diese Datei statt Original + Effekte. Export und Standbild rendern immer
// aus den Originalen.
//
// Der Dateiname ist ein Hash aus allem, was das Bild des Clips bestimmt (Clip wie in der Projektdatei ohne
// Position/Ton/Fades/Übergänge, Größe + Änderungszeit der Quelle, Projektformat) -> jede Änderung am Clip
// macht den Cache ungültig, Verschieben nicht. Fades und Übergänge wirken weiter live über dem Cache
// (Übergänge und Titel werden nicht gecacht). Ablage wie die Proxies unter QStandardPaths::CacheLocation.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class QThread;

class RenderCache : public QObject {
    Q_OBJECT
public:
    enum class Mode { Off, Smart, User };

    explicit RenderCache(QObject* parent = nullptr);
    ~RenderCache() override;

    static QString cacheDir();

    // Aus = nie; Benutzer = nur Clips mit „Render-Cache Clip-Ausgabe“; Smart = zusätzlich Clips mit
    // teuren Effekten oder geänderter Geschwindigkeit (gespeichert, Standard Benutzer)
    Mode mode() const { return m_mode; }
    void setMode(Mode mode);

    void setFormat(const ProjectFormat& format);
    const ProjectFormat& format() const { return m_format; }

    // Soll der Clip (auf einer Videospur) im Modus gecacht werden? Titel nie.
    static bool wanted(const Clip& c, Mode mode);
    bool wanted(const Clip& c) const { return wanted(c, m_mode); }
    // Smart: Effekte oder Geschwindigkeit/Rückwärts
    static bool isExpensive(const Clip& c);
    // Nur was das Bild bestimmt (ohne Position, ID, Ton, Fades, Übergänge, Aktiv-Schalter, Cache-Markierung)
    static Clip normalized(const Clip& c);
    static QString key(const Clip& c, const ProjectFormat& format);
    QString path(const Clip& c) const; // Ablageort (unabhängig davon, ob schon gerendert)

    // Vorschau: fertige Cache-Datei, wenn der Modus den Clip cacht und sie zum aktuellen Stand passt; sonst leer
    QString resolve(const Clip& c) const;

    // Anzeige über der Timeline: gecachte (blau) und noch nicht gecachte (rot) Bereiche; done = gerenderter
    // Anteil vom Clip-Anfang (1 = fertig)
    struct Span {
        int start = 0, end = 0; // Timeline-Frames, end exklusiv
        double done = 0;
    };
    QVector<Span> spans(const Timeline& tl) const;

    // Timeline geändert: fehlende Clips einreihen (startet nach kurzer Ruhe), überholte Aufträge verwerfen
    void sync(const Timeline& tl);
    // Während der Wiedergabe (und solange ein Export läuft) pausiert das Rendern
    void setPlaying(bool on);
    void clear(); // „Render-Cache löschen“: alle Dateien weg (gewünschte Clips werden neu gerendert)

    bool isRendering() const { return m_thread != nullptr; }
    int pendingCount() const;

    // Rendert die Ausgabe eines Clips (siehe TimelineBuilder::buildClipOutput) synchron in `file`.
    // Mit Alpha (ProRes 4444), wenn der Clip Transparenz hat, sonst ProRes 422 LT. cancel/paused dürfen null sein.
    static bool render(const Clip& c, const ProjectFormat& format, const QString& file, QString* error,
                       const std::atomic<bool>* cancel = nullptr, const std::function<bool()>& paused = {},
                       std::atomic<int>* progress = nullptr);

signals:
    // Cache-Datei fertig/gelöscht oder Modus geändert -> Vorschau neu bauen, Anzeige auffrischen
    void cacheChanged();
    void progressChanged(); // Fortschritt des laufenden Clips (Anzeige)
    void modeChanged(RenderCache::Mode mode);
    void failed(const QString& message);

private:
    struct Job {
        QString key;
        Clip clip; // normalisiert
    };
    void startNext();
    void cancelCurrent();
    void onFinished(bool ok, const QString& error);

    Mode m_mode = Mode::User;
    ProjectFormat m_format;
    QVector<Job> m_jobs;        // gewünscht und noch nicht fertig, in Timeline-Reihenfolge
    QHash<QString, bool> m_failed; // Schlüssel, deren Rendern scheiterte (nicht endlos neu versuchen)
    QHash<QString, bool> m_reported; // schon gemeldete Fehlermeldungen (z. B. ffmpeg fehlt: nur einmal)
    QTimer m_idle;              // kurze Ruhe nach Änderungen, bevor gerendert wird
    QTimer m_poll;              // Fortschritt abfragen
    QThread* m_thread = nullptr;
    QString m_currentKey;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<std::atomic<int>> m_progress; // 0..1000
    int m_lastProgress = -1;
    std::atomic<bool> m_playing{false};
};
