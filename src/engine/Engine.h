#pragma once
// Einzige Stelle, die MLT kennt (zusammen mit TimelineBuilder).
// UI spricht nur über diese Klasse -> Engine wäre austauschbar.

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
class ProxyManager;
struct MixerHooks;

class Engine : public QObject {
    Q_OBJECT
public:
    enum class Mode { Timeline, Source };

    explicit Engine(QObject* parent = nullptr);
    ~Engine() override;

    bool init(QString* error);
    int fps() const;
    QSize frameSize() const; // Projektformat in Pixeln

    // Proxy-Medien: Vorschau (Timeline-Videospuren, Quellansicht) liest den Proxy, falls vorhanden und
    // eingeschaltet; Ton und Export immer vom Original. Nach Änderungen updateTimeline() aufrufen.
    ProxyManager* proxies() const { return m_proxies; }

    MediaInfo probe(const QString& path);
    QImage thumbnail(const QString& path, int frame, const QSize& size);

    void updateTimeline(const Timeline& tl); // Modell geändert -> neu aufbauen
    // Nächstes updateTimeline() ändert nur Fader/Pan -> live übernehmen statt neu aufbauen
    // (sonst stockt die Wiedergabe beim Fader-Ziehen)
    void mixerOnlyNext() { m_mixerOnlyNext = true; }
    void showTimeline(int position);
    void showSource(const QString& path);
    Mode mode() const { return m_mode; }

    void play();
    void pause();
    void togglePlay();
    void setSpeed(double speed);
    double speed() const { return m_speed; }
    void seek(int frame);
    int position() const { return m_position; }

signals:
    void frameReady(const QImage& image);
    void positionChanged(int frame);
    void speedChanged(double speed);
    void modeChanged(Engine::Mode mode);
    // Nur während der Wiedergabe: Spitzenpegel in dBFS, [A1 L, A1 R, A2 L, …, Master L, Master R]
    void audioLevels(const QVector<float>& db);

private:
    void connectProducer(Mlt::Producer* producer, int position);
    void refresh();
    void onFrameShown(void* mltFrame);
    void emitLevels();

    ProxyManager* m_proxies;
    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<Mlt::Consumer> m_consumer;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Tractor> m_timeline;
    std::unique_ptr<MixerHooks> m_mixer;
    std::mutex m_mixerMutex; // m_mixer wird im Consumer-Thread gelesen
    bool m_mixerOnlyNext = false;
    std::unique_ptr<Mlt::Producer> m_source;
    Mlt::Producer* m_current = nullptr;

    Mode m_mode = Mode::Timeline;
    double m_speed = 0.0;
    std::atomic<int> m_position{0};
    QSize m_previewSize{960, 540};

    friend struct EngineCallbacks;
};
