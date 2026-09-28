#pragma once
// Einzige Stelle, die MLT kennt (zusammen mit TimelineBuilder).
// UI spricht nur über diese Klasse -> Engine wäre austauschbar.

#include "core/Types.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <atomic>
#include <memory>

namespace Mlt {
class Profile;
class Producer;
class Consumer;
class Tractor;
} // namespace Mlt
class TimelineBuilder;

class Engine : public QObject {
    Q_OBJECT
public:
    enum class Mode { Timeline, Source };

    explicit Engine(QObject* parent = nullptr);
    ~Engine() override;

    bool init(QString* error);
    int fps() const;
    QSize frameSize() const; // Projektformat in Pixeln

    MediaInfo probe(const QString& path);
    QImage thumbnail(const QString& path, int frame, const QSize& size);

    void updateTimeline(const Timeline& tl); // Modell geändert -> neu aufbauen
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

private:
    void connectProducer(Mlt::Producer* producer, int position);
    void refresh();
    void onFrameShown(void* mltFrame);

    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<Mlt::Consumer> m_consumer;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Tractor> m_timeline;
    std::unique_ptr<Mlt::Producer> m_source;
    Mlt::Producer* m_current = nullptr;

    Mode m_mode = Mode::Timeline;
    double m_speed = 0.0;
    std::atomic<int> m_position{0};
    QSize m_previewSize{960, 540};

    friend struct EngineCallbacks;
};
