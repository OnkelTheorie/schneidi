#pragma once
#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QObject>
#include <QTimer>
#include <atomic>
#include <memory>

namespace Mlt {
class Profile;
class Consumer;
class Event;
class Tractor;
} // namespace Mlt
class TimelineBuilder;

struct ExportSettings {
    QString path;
    ProjectFormat format; // Projekteinstellungen (Framerate, Bezugsgröße für Pixelwerte im Schnitt)
    QSize size;           // Ausgabegröße; leer = Timeline-Auflösung
    QString videoCodec = "libx264"; // leer = nur Audio (kein Videostrom)
    QString audioCodec = "aac";
    QString pixFmt = "yuv420p";
    int crf = 20;             // Qualität: kleiner = besser
    QString preset = "medium"; // Geschwindigkeit vs. Dateigröße
    int audioBitrateK = 192;
    bool burnSubtitles = false; // sichtbare Untertitelspur ins Bild einbrennen
    QString subtitlePath;       // sichtbare Untertitelspur zusätzlich als SRT hierhin schreiben (leer = nicht)
    int from = 0, to = -1; // Bereich (Frames, inklusive); to < 0 = bis zum Ende der Timeline
    int cores = 0;         // CPU-Kerne fürs Rendern (Bilder parallel, Decoder, Encoder); 0 = alle
};

// Rendert die Timeline in eine Datei. Eigene Producer-Instanzen,
// damit Vorschau und Export sich nicht in die Quere kommen.
class Exporter : public QObject {
    Q_OBJECT
public:
    explicit Exporter(QObject* parent = nullptr);
    ~Exporter() override;

    bool isRunning() const { return m_consumer != nullptr; }
    // Läuft gerade irgendein Export? (Render-Cache pausiert solange)
    static bool anyRunning();
    // CPU-Kerne fürs Rendern (Deliver-Seite, gilt pro Rechner, QSettings "render/cores"): 0 = alle
    static int savedCores();
    static void setSavedCores(int cores);
    static int availableCores(); // Threads des Prozessors (mindestens 1)
    // Wie viele Bilder gleichzeitig gerendert werden: cores (0 = alle), aber 1, wenn die Timeline (auch verschachtelte
    // Sequenzen) einen frei0r-Effekt nutzt (Green Screen) – die sind nicht thread-sicher
    static int parallelFrames(const Timeline& tl, int cores);
    // Liest die Timeline (auch verschachtelte Sequenzen) diese Datei? Dann darf der Export sie nicht überschreiben
    // (Quelle und Ausgabe zerstört) – wie DaVinci verweigern statt nachfragen.
    static bool readsFile(const Timeline& tl, const QString& path);
    // Zeigen beide Pfade auf dieselbe vorhandene Datei? (Windows: Groß-/Kleinschreibung egal)
    static bool sameFile(const QString& a, const QString& b);
    bool start(const Timeline& tl, const ExportSettings& settings, QString* error);
    void cancel();

signals:
    void progress(int percent);
    void finished(bool ok, const QString& message);

private:
    void poll();
    void cleanup();

    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Tractor> m_tractor;
    std::unique_ptr<Mlt::Consumer> m_consumer;
    std::unique_ptr<Mlt::Event> m_stoppedEvent;
    std::atomic<bool> m_threadDone{false}; // Encoder-Thread hat sich beendet ("consumer-stopped")
    int m_restarts = 0;
    QTimer m_timer;
    int m_length = 0;
    int m_from = 0;
    QString m_path;
};
