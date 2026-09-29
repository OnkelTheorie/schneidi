#include "engine/Engine.h"

#include "core/I18n.h"
#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <cstring>
#include <utility>

struct EngineCallbacks {
    // Läuft im MLT-Consumer-Thread, sobald ein Frame angezeigt werden soll
    static void onFrameShow(mlt_properties, void* self, mlt_event_data data)
    {
        static_cast<Engine*>(self)->onFrameShown(mlt_event_data_to_frame(data));
    }
};

Engine::Engine(QObject* parent) : QObject(parent), m_proxies(new ProxyManager(this))
{
    // Meldungen, die schon unterwegs waren, als gesprungen wurde, ließen den Playhead kurz zurückspringen
    connect(this, &Engine::framePosition, this, [this](int frame, int epoch) {
        if (epoch == m_seekEpoch) emit positionChanged(frame);
    }, Qt::QueuedConnection);
}

Engine::~Engine()
{
    if (m_consumer) m_consumer->stop();
    m_consumer.reset();
    m_timeline.reset();
    m_source.reset();
    m_builder.reset();
    m_profile.reset();
}

bool Engine::init(QString* error)
{
#ifdef Q_OS_WIN
    // Unter Windows liegen die MLT-Plugins neben der .exe (portable Installation)
    const QString appDir = QCoreApplication::applicationDirPath();
    qputenv("MLT_DATA", QDir(appDir).filePath("share/mlt-7").toUtf8());
    qputenv("MLT_PROFILES_PATH", QDir(appDir).filePath("share/mlt-7/profiles").toUtf8());
    if (!Mlt::Factory::init(QDir(appDir).filePath("lib/mlt-7").toUtf8().constData())) {
#else
    if (!Mlt::Factory::init()) {
#endif
        if (error) *error = T("MLT konnte nicht initialisiert werden.");
        return false;
    }

    m_format = ProjectFormat{};
    if (!createConsumer(error)) return false;
    updateTimeline(Timeline{});
    showTimeline(0);
    return true;
}

// Profil, Builder und Vorschau-Consumer zum aktuellen Projektformat anlegen
bool Engine::createConsumer(QString* error)
{
    m_profile = makeProfile(m_format);
    m_builder = std::make_unique<TimelineBuilder>(*m_profile);
    // Nur das Bild vom Proxy: Ton dekodiert billig und bleibt so exakt wie im Export
    m_builder->setResolver([this](const QString& path, TrackKind kind) {
        return kind == TrackKind::Video ? m_proxies->resolve(path) : path;
    });
    // Vorschaubild im Seitenverhältnis des Projekts (sonst wird Hochformat verzerrt), gerade Maße
    const QSize fit = m_format.size().scaled(960, 960, Qt::KeepAspectRatio);
    m_previewSize = QSize(std::max(2, fit.width() & ~1), std::max(2, fit.height() & ~1));

    for (const char* id : {"sdl2_audio", "rtaudio"}) {
        m_consumer = std::make_unique<Mlt::Consumer>(*m_profile, id);
        if (m_consumer->is_valid()) break;
    }
    if (!m_consumer || !m_consumer->is_valid()) {
        if (error) *error = T("Kein MLT-Audio-Consumer (sdl2_audio/rtaudio) gefunden.");
        return false;
    }
    m_consumer->set("terminate_on_pause", 0);
    m_consumer->set("real_time", 1);
    m_consumer->set("scrub_audio", 1);
    // Kleiner Vorlauf-Puffer: sonst läuft der Ton nach Pause/Seek noch ~1 s weiter
    m_consumer->set("buffer", 2);
    m_consumer->set("prefill", 1);
    m_consumer->set("audio_buffer", 1024);
    m_consumer->listen("consumer-frame-show", this, (mlt_listener)EngineCallbacks::onFrameShow);
    return true;
}

void Engine::setFormat(const ProjectFormat& format)
{
    if (format == m_format && m_profile) return;
    // Alles, was am alten Profil hängt, in dieser Reihenfolge abbauen: Consumer zuerst (liest die Producer)
    if (m_consumer) m_consumer->stop();
    m_current = nullptr;
    m_consumer.reset();
    {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        m_mixer.reset();
        m_timeline.reset();
    }
    m_source.reset();
    m_builder.reset();
    m_profile.reset();

    m_format = format;
    m_speed = 0;
    QString error;
    if (!createConsumer(&error)) qWarning("%s", qPrintable(error));
    if (m_mode != Mode::Timeline) {
        m_mode = Mode::Timeline; // Quellansicht hing am alten Profil
        emit modeChanged(m_mode);
    }
    emit speedChanged(m_speed);
}

MediaInfo Engine::probe(const QString& path)
{
    MediaInfo info;
    info.path = path;
    info.name = QFileInfo(path).fileName();
    Mlt::Producer p(*m_profile, path.toUtf8().constData());
    if (!p.is_valid()) return info;

    const QString service = p.get("mlt_service");
    info.isImage = service == "qimage" || service == "pixbuf";
    if (info.isImage) {
        info.hasVideo = true;
        info.length = 5 * fps(); // Standbild: 5 Sekunden wie in DaVinci
    } else {
        // Audiodateien mit eingebettetem Cover gelten nicht als Video
        static const QStringList audioExt{"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma", "aiff"};
        const bool audioFile = audioExt.contains(QFileInfo(path).suffix().toLower());
        info.hasVideo = !audioFile && p.get_int("video_index") >= 0;
        info.hasAudio = p.get_int("audio_index") >= 0;
        info.length = p.get_length();
    }
    return info;
}

QImage Engine::thumbnail(const QString& path, int frame, const QSize& size)
{
    Mlt::Producer p(*m_profile, path.toUtf8().constData());
    if (!p.is_valid()) return {};
    p.seek(frame);
    std::unique_ptr<Mlt::Frame> f(p.get_frame());
    if (!f) return {};
    // Im Seitenverhältnis des Projekts holen und mittig auf die gewünschte Größe setzen (schwarze Ränder)
    const QSize fit = m_format.size().scaled(size, Qt::KeepAspectRatio);
    mlt_image_format fmt = mlt_image_rgba;
    int w = std::max(2, fit.width()), h = std::max(2, fit.height());
    const uint8_t* data = f->get_image(fmt, w, h);
    if (!data || w <= 0 || h <= 0) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), data, size_t(w) * h * 4);
    if (img.size() == size) return img;
    QImage out(size, QImage::Format_RGBA8888);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.drawImage(QPoint((size.width() - w) / 2, (size.height() - h) / 2), img);
    painter.end();
    return out;
}

QImage Engine::grabStill(const Timeline& tl)
{
    // Eigenes Profil und eigene Producer (wie der Export) -> Vorschau läuft ungestört weiter
    auto profile = makeProfile(m_format);
    std::unique_ptr<TimelineBuilder> builder;
    std::unique_ptr<Mlt::Producer> producer;
    if (m_mode == Mode::Source && !m_sourcePath.isEmpty()) {
        producer = std::make_unique<Mlt::Producer>(*profile, m_sourcePath.toUtf8().constData());
    } else {
        builder = std::make_unique<TimelineBuilder>(*profile);
        producer = builder->build(tl);
    }
    if (!producer || !producer->is_valid()) return {};
    producer->seek(m_position);
    std::unique_ptr<Mlt::Frame> f(producer->get_frame());
    if (!f) return {};
    mlt_image_format fmt = mlt_image_rgba;
    int w = profile->width(), h = profile->height();
    const uint8_t* data = f->get_image(fmt, w, h);
    if (!data || w <= 0 || h <= 0) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), data, size_t(w) * h * 4);
    // Transparente Stellen (leere Timeline, herausgezoomte Clips) wie im Viewer schwarz
    QImage out(img.size(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.drawImage(0, 0, img);
    return out;
}

void Engine::updateTimeline(const Timeline& tl)
{
    const bool mixerOnly = std::exchange(m_mixerOnlyNext, false);
    {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        if (mixerOnly && m_timeline && m_mixer && TimelineBuilder::applyMixer(tl, *m_mixer)) {
            if (m_speed == 0.0) refresh();
            return;
        }
    }
    const bool active = m_mode == Mode::Timeline;
    const int pos = m_position;
    if (active && m_consumer) m_consumer->stop(); // alten Tractor nicht mehr lesen lassen
    auto hooks = std::make_unique<MixerHooks>();
    auto tractor = m_builder->build(tl, hooks.get());
    {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        m_mixer = std::move(hooks);
        m_timeline = std::move(tractor);
    }
    if (active && m_consumer) connectProducer(m_timeline.get(), pos);
}

void Engine::showTimeline(int position)
{
    if (!m_timeline) return;
    m_speed = 0;
    m_mode = Mode::Timeline;
    connectProducer(m_timeline.get(), position);
    emit modeChanged(m_mode);
}

void Engine::showSource(const QString& path, int position)
{
    auto p = std::make_unique<Mlt::Producer>(*m_profile, m_proxies->resolve(path).toUtf8().constData());
    if (!p->is_valid()) return;
    m_consumer->stop();
    m_source = std::move(p);
    m_sourcePath = path;
    m_speed = 0;
    m_mode = Mode::Source;
    connectProducer(m_source.get(), std::max(0, position));
    emit modeChanged(m_mode);
}

void Engine::connectProducer(Mlt::Producer* producer, int position)
{
    m_consumer->stop();
    m_current = producer;
    m_current->set_speed(m_speed);
    m_current->seek(position);
    m_position = position;
    m_consumer->connect(*m_current);
    m_consumer->start();
    refresh();
    emit positionChanged(position);
    emit speedChanged(m_speed);
}

void Engine::refresh()
{
    if (m_consumer) m_consumer->set("refresh", 1);
}

void Engine::play() { setSpeed(1.0); }
void Engine::pause() { setSpeed(0.0); }
void Engine::togglePlay() { setSpeed(m_speed == 0.0 ? 1.0 : 0.0); }

void Engine::setSpeed(double speed)
{
    if (!m_current) return;
    m_speed = speed;
    m_current->set_speed(speed);
    if (speed == 0.0) {
        m_current->seek(m_position); // exakt auf dem angezeigten Frame stehen bleiben
        m_consumer->purge();         // gepufferten Ton sofort verwerfen
    }
    refresh();
    emit speedChanged(speed);
}

void Engine::seek(int frame)
{
    if (!m_current) return;
    frame = std::max(0, frame);
    ++m_seekEpoch;
    m_seekSkipped = 0;
    m_seekTarget = m_speed != 0.0 ? frame : -1;
    m_current->seek(frame);
    m_consumer->purge();
    m_position = frame;
    refresh();
    emit positionChanged(frame);
}

void Engine::onFrameShown(void* mltFrame)
{
    if (!mltFrame) return;
    Mlt::Frame frame(static_cast<mlt_frame>(mltFrame));
    const int pos = frame.get_position();
    const int target = m_seekTarget;
    if (target >= 0) {
        // Erst ab dem Sprungziel wieder anzeigen (Puffer enthält noch Bilder von vorher); Notbremse nach 50 Bildern
        const double speed = m_speed;
        const bool arrived = speed == 0.0 || (speed > 0 ? pos >= target && pos <= target + 60 : pos <= target && pos >= target - 60);
        if (!arrived && ++m_seekSkipped < 50) return;
        m_seekTarget = -1;
    }
    mlt_image_format fmt = mlt_image_rgba;
    int w = m_previewSize.width(), h = m_previewSize.height();
    const uint8_t* data = frame.get_image(fmt, w, h);
    if (data && w > 0 && h > 0) {
        QImage img(w, h, QImage::Format_RGBA8888);
        std::memcpy(img.bits(), data, size_t(w) * h * 4);
        emit frameReady(img); // queued -> UI-Thread
    }
    if (m_speed != 0.0 && pos != m_position.exchange(pos)) emit framePosition(pos, m_seekEpoch);
    if (m_speed != 0.0 && m_mode == Mode::Timeline) emitLevels();
}

void Engine::emitLevels()
{
    // Die audiolevel-Filter merken sich den Pegel des zuletzt verarbeiteten Tons
    // (läuft dem Bild wegen des kleinen Puffers minimal voraus – für Meter egal)
    auto read = [](const MixerHooks::Strip& s, QVector<float>& out) {
        float l = -200.f, r = -200.f;
        if (s.audible && s.meter) {
            if (s.meter->get("_audio_level.0")) l = s.meter->get_double("_audio_level.0");
            r = s.meter->get("_audio_level.1") ? float(s.meter->get_double("_audio_level.1")) : l; // Mono
        }
        out << l << r;
    };
    QVector<float> levels;
    {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        if (!m_mixer) return;
        for (const auto& s : m_mixer->tracks) read(s, levels);
        read(m_mixer->master, levels);
    }
    emit audioLevels(levels); // queued -> UI-Thread
}
