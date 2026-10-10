#pragma once
#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <atomic>
#include <memory>

namespace Mlt {
class Profile;
class Consumer;
class Event;
class Tractor;
class Producer;
} // namespace Mlt
class TimelineBuilder;

struct ExportSettings {
    QString path;
    ProjectFormat format; // Projekteinstellungen (Framerate, Bezugsgröße für Pixelwerte im Schnitt)
    QSize size;           // Ausgabegröße; leer = Timeline-Auflösung
    QString videoCodec = "libx264"; // empty = audio only (no video stream)
    // videoCodec is a hardware encoder (h264_nvenc …): software codec to use when it does not work (empty = none).
    // The hardware encoder is tested with the output size first (MLT crashes when it cannot open an encoder).
    QString softwareCodec;
    QString encoderNote; // note from choosing the encoder (e.g. not available here), reported with the result
    QString audioCodec = "aac";
    QString pixFmt = "yuv420p";
    int crf = 20;             // Qualität: kleiner = besser
    QString preset = "medium"; // Geschwindigkeit vs. Dateigröße
    int audioBitrateK = 192;
    QString audioSampleFormat; // leer = Vorgabe des Codecs (Apple Lossless: s16p/s32p für 16/24 Bit)
    int audioRate = 0, audioChannels = 0; // 0 = 48 kHz stereo (speech to text wants 16 kHz mono)
    FrameRate rate{0, 1};      // Bildrate der Datei; num 0 = wie die Timeline (format.rate)
    bool burnSubtitles = false; // sichtbare Untertitelspur ins Bild einbrennen
    QString subtitlePath;       // sichtbare Untertitelspur zusätzlich als SRT hierhin schreiben (leer = nicht)
    int from = 0, to = -1; // Bereich (Frames, inklusive); to < 0 = bis zum Ende der Timeline
    int threads = 0;       // Threads (logische Prozessoren) fürs Rendern (Bilder parallel, Decoder, Encoder); 0 = alle
};

// Rendert die Timeline in eine Datei. Eigene Producer-Instanzen,
// damit Vorschau und Export sich nicht in die Quere kommen.
class Exporter : public QObject {
    Q_OBJECT
public:
    explicit Exporter(QObject* parent = nullptr);
    ~Exporter() override;

    bool isRunning() const { return m_consumer != nullptr || m_check != nullptr; }
    // Läuft gerade irgendein Export? (Render-Cache pausiert solange)
    static bool anyRunning();
    // Threads fürs Rendern (Deliver-Seite, gilt pro Rechner, QSettings "render/cores" – Schlüssel bleibt für alte Einstellungen): 0 = alle
    static int savedThreads();
    static void setSavedThreads(int threads);
    static int availableThreads(); // logische Prozessoren (QThread::idealThreadCount) (mindestens 1)
    // Wie viele Bilder gleichzeitig gerendert werden: threads (0 = alle), aber 1, wenn die Timeline (auch verschachtelte
    // Sequenzen) einen frei0r-Effekt nutzt (Green Screen) – die sind nicht thread-sicher
    static int parallelFrames(const Timeline& tl, int threads);
    // Liest die Timeline (auch verschachtelte Sequenzen) diese Datei? Dann darf der Export sie nicht überschreiben
    // (Quelle und Ausgabe zerstört) – wie DaVinci verweigern statt nachfragen.
    static bool readsFile(const Timeline& tl, const QString& path);
    // Zeigen beide Pfade auf dieselbe vorhandene Datei? (Windows: Groß-/Kleinschreibung egal)
    static bool sameFile(const QString& a, const QString& b);
    bool start(const Timeline& tl, const ExportSettings& settings, QString* error);
    void cancel();
    // After finished(): FFmpeg video codec that wrote the file (empty for audio only) and a note for the user
    // (e.g. the hardware encoder did not work and software was used; empty = nothing to say)
    QString usedCodec() const { return m_usedCodec; }
    QString note() const { return m_note; }

signals:
    void progress(int percent);
    void finished(bool ok, const QString& message);

private:
    void poll();
    void cleanup();
    bool startConsumer(const QString& codec, QString* error);
    void onCheckFinished(bool works);
    // Hardware encoder failed: render again with ExportSettings::softwareCodec; false = no fallback possible
    bool fallBackToSoftware(QString* error);

    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Tractor> m_tractor;
    // Andere Bildrate als die Timeline: Timeline als MLT-XML, abgespielt über den "consumer"-Producer im Profil der
    // Ausgabe (rechnet Bilder um, Ton bleibt lückenlos). Sonst zeigt m_source auf m_tractor.
    std::unique_ptr<Mlt::Profile> m_outProfile;
    std::unique_ptr<Mlt::Producer> m_source;
    QString m_xmlPath;
    std::unique_ptr<Mlt::Consumer> m_consumer;
    std::unique_ptr<Mlt::Event> m_stoppedEvent;
    std::unique_ptr<Mlt::Event> m_fatalEvent;
    std::atomic<bool> m_fatal{false}; // avformat reported a fatal error (e.g. the encoder failed while rendering)
    std::unique_ptr<QProcess> m_check; // test encode of the hardware encoder before the render starts
    ExportSettings m_settings;
    bool m_convertRate = false;
    int m_threads = 0, m_frames = 1;
    QString m_usedCodec, m_note;
    std::atomic<bool> m_threadDone{false}; // Encoder-Thread hat sich beendet ("consumer-stopped")
    int m_restarts = 0;
    QTimer m_timer;
    int m_length = 0;
    int m_from = 0;
    QString m_path;
};
