#pragma once
// Einzige Stelle, die MLT kennt (zusammen mit TimelineBuilder).
// UI spricht nur über diese Klasse -> Engine wäre austauschbar.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QVector>
#include <atomic>
#include <memory>
#include <mutex>

namespace Mlt {
class Profile;
class Producer;
class Consumer;
class Tractor;
} // namespace Mlt
class TimelineBuilder;
class ProducerFactory;
class ProxyManager;
class RenderCache;
class Preroll;
struct MixerHooks;
struct SharedLoudness;

// Loudness-Meter (ITU-R BS.1770) der Vorschau: gemessen am Master, nur bei Wiedergabe mit normaler Geschwindigkeit
struct LoudnessReading {
    double momentary = -200, shortTerm = -200, integrated = -200; // LUFS (-200 = noch nichts)
    double range = 0;   // LU
    double seconds = 0; // gemessene Dauer
    double truePeak = -200; // höchster True Peak seit Reset (dBTP, alle Kanäle; -200 = noch nichts)
};

class Engine : public QObject {
    Q_OBJECT
public:
    enum class Mode { Timeline, Source };

    explicit Engine(QObject* parent = nullptr);
    ~Engine() override;

    bool init(QString* error);
    // Projekteinstellungen: baut Profil, Vorschau-Consumer und alle Producer neu auf (Viewer zeigt danach
    // die Timeline). Die Timeline selbst kommt mit dem nächsten updateTimeline().
    void setFormat(const ProjectFormat& format);
    const ProjectFormat& format() const { return m_format; }
    int fps() const { return m_format.rate.timebase(); } // Timecode-Frames pro Sekunde (29,97 -> 30)
    QSize frameSize() const { return m_format.size(); }  // Projektformat in Pixeln

    // Proxy-Medien: Vorschau (Timeline-Videospuren, Quellansicht) liest den Proxy, falls vorhanden und
    // eingeschaltet; Ton und Export immer vom Original. Nach Änderungen updateTimeline() aufrufen.
    ProxyManager* proxies() const { return m_proxies; }
    // Render-Cache: Vorschau spielt vorgerenderte Clip-Ausgaben (Export nie); updateTimeline() reiht fehlende ein
    RenderCache* renderCache() const { return m_renderCache; }

    MediaInfo probe(const QString& path);
    QImage thumbnail(const QString& path, int frame, const QSize& size);

    // Standbild (Grab Still) des aktuell gezeigten Bilds (Timeline bzw. Quelle) in voller Projektauflösung,
    // immer aus den Originalen (nicht aus Proxies)
    QImage grabStill(const Timeline& tl);

    void updateTimeline(const Timeline& tl); // Modell geändert -> neu aufbauen
    // Nächstes updateTimeline() ändert nur Fader/Pan/Mute/Solo -> live übernehmen statt neu aufbauen
    // (sonst stockt die Wiedergabe beim Fader-Ziehen)
    void mixerOnlyNext() { m_mixerOnlyNext = true; }
    bool mixerOnlyPending() const { return m_mixerOnlyNext; }
    void showTimeline(int position);
    void showSource(const QString& path, int position = 0);
    Mode mode() const { return m_mode; }
    // Zuletzt in der Quellansicht gezeigte Datei (bleibt nach Q/Timeline erhalten, leer = noch keine)
    const QString& sourcePath() const { return m_sourcePath; }

    void play();
    void pause();
    void togglePlay();
    void setSpeed(double speed);
    double speed() const { return m_speed; }
    void seek(int frame);
    int position() const { return m_position; }

    // Vorher/Nachher (Color-Seite, wie DaVinci „Bypass Color Grades“): Vorschau ohne Farbkorrektur; Export unberührt
    void setColorBypass(bool on);
    bool colorBypass() const { return m_gradeBypass->load(); }
    LoudnessReading loudness() const;
    void resetLoudness(); // Integrated/LRA neu beginnen (wie der Reset-Knopf in DaVinci)

    // Audio-Ausgabe (Menü Wiedergabe → Audio-Ausgabe, gespeichert in QSettings). Größerer Puffer gegen Knacken/
    // Aussetzer (Windows/WASAPI braucht mehr als PulseAudio), dafür etwas mehr Verzögerung. Wirkt ab dem nächsten Play.
    static int audioBuffer();
    void setAudioBuffer(int samples);
    // Audiotreiber von SDL (leer = automatisch; Windows: "directsound" (Standard)/"wasapi"). Wirkt nach dem Neustart.
    static QString audioDriver();
    static void setAudioDriver(const QString& driver);
    // Vor init() aufrufen: setzt den gewählten Treiber für SDL (eine gesetzte Umgebungsvariable hat Vorrang)
    static void applyAudioSettings();

signals:
    void frameReady(const QImage& image);
    void positionChanged(int frame);
    void speedChanged(double speed);
    void colorBypassChanged(bool on);
    void modeChanged(Engine::Mode mode);
    // Nur während der Wiedergabe: Spitzenpegel in dBFS, [A1 L, A1 R, A2 L, …, Master L, Master R];
    // der Master misst True Peak (dBTP, BS.1770 4x überabgetastet) wie die Lieferprüfung
    void audioLevels(const QVector<float>& db);
    // intern: Position aus dem Consumer-Thread, epoch = Sprung-Zähler beim Anzeigen (ältere werden verworfen)
    void framePosition(int frame, int epoch);

private:
    bool createConsumer(QString* error);
    void connectProducer(Mlt::Producer* producer, int position);
    void refresh();
    void applyAudioState();
    void onFrameShown(void* mltFrame);
    void emitLevels();

    ProxyManager* m_proxies;
    RenderCache* m_renderCache;
    ProjectFormat m_format;
    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<ProducerFactory> m_factory; // Quellansicht (BT.601-Umweg, Profiles.h); überlebt m_source
    std::unique_ptr<Mlt::Consumer> m_consumer;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Tractor> m_timeline;
    std::unique_ptr<MixerHooks> m_mixer;
    std::unique_ptr<SharedLoudness> m_loudness; // vom Master-Pegelmesser im Audio-Thread gefüttert
    std::unique_ptr<Preroll> m_preroll;         // Decoder vor Sprungstellen vorab positionieren
    std::mutex m_mixerMutex; // m_mixer wird im Consumer-Thread gelesen
    bool m_mixerOnlyNext = false;
    std::shared_ptr<std::atomic<bool>> m_gradeBypass = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<Mlt::Producer> m_source;
    QString m_sourcePath;
    Mlt::Producer* m_current = nullptr;

    Mode m_mode = Mode::Timeline;
    double m_speed = 0.0;
    std::atomic<int> m_position{0};
    // Sprung während der Wiedergabe: Bilder aus dem Puffer von vor dem Sprung nicht mehr zeigen/melden
    std::atomic<int> m_seekEpoch{0};
    std::atomic<int> m_seekTarget{-1};
    std::atomic<int> m_seekSkipped{0};
    QSize m_previewSize{960, 540}; // Projektformat verkleinert (längere Kante 960)

    friend struct EngineCallbacks;
};
