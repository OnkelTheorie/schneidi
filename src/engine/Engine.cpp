#include "engine/Engine.h"

#include "core/I18n.h"
#include "core/Loudness.h"
#include "engine/Bundle.h"
#include "engine/Frei0r.h"
#include "engine/Preroll.h"
#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "engine/StillFetcher.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QSettings>
#include <cstring>
#include <utility>

struct EngineCallbacks {
    // Läuft im MLT-Consumer-Thread, sobald ein Frame angezeigt werden soll
    static void onFrameShow(mlt_properties, void* self, mlt_event_data data)
    {
        static_cast<Engine*>(self)->onFrameShown(mlt_event_data_to_frame(data));
    }
};

Engine::Engine(QObject* parent)
    : QObject(parent), m_proxies(new ProxyManager(this)), m_renderCache(new RenderCache(this)),
      m_stills(new StillFetcher(this)),
      m_loudness(std::make_unique<SharedLoudness>()), m_preroll(std::make_unique<Preroll>())
{
    // Render-Cache pausiert während der Wiedergabe
    connect(this, &Engine::speedChanged, m_renderCache, [this](double s) { m_renderCache->setPlaying(s != 0.0); });
    // Meldungen, die schon unterwegs waren, als gesprungen wurde, ließen den Playhead kurz zurückspringen
    connect(this, &Engine::framePosition, this, [this](int frame, int epoch) {
        if (epoch == m_seekEpoch) emit positionChanged(frame);
    }, Qt::QueuedConnection);
}

Engine::~Engine()
{
    if (m_consumer) m_consumer->stop();
    m_consumer.reset();
    m_preroll.reset(); // nach dem Consumer (meldet ihm jedes Frame), vor den Producern
    m_timeline.reset();
    m_source.reset();
    m_builder.reset();
    m_factory.reset();
    m_profile.reset();
}

bool Engine::init(QString* error)
{
    // Mitgelieferte MLT-Module (AppImage/Windows-Programmordner), sonst die des Systems
    Bundle::prepareMltEnvironment();
    Frei0r::prepareEnvironment();
    const QString modules = Bundle::mltModuleDir();
    if (!(modules.isEmpty() ? Mlt::Factory::init() : Mlt::Factory::init(Bundle::pathForMlt(modules).constData()))) {
        if (error) *error = T("MLT konnte nicht initialisiert werden.");
        return false;
    }
    Frei0r::registerEffects();

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
    m_factory = std::make_unique<ProducerFactory>(*m_profile);
    m_builder = std::make_unique<TimelineBuilder>(*m_profile);
    // Nur das Bild vom Proxy: Ton dekodiert billig und bleibt so exakt wie im Export
    m_builder->setResolver([this](const QString& path, TrackKind kind) {
        return kind == TrackKind::Video ? m_proxies->resolve(path) : path;
    });
    m_builder->setGradeBypass(m_gradeBypass);
    m_builder->setClipCache([this](const Clip& c) { return m_renderCache->resolve(c); });
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
    // Gleich in Vorschaugröße rechnen (Überblendungen, Transform, Effekte) statt in Projektgröße und danach
    // verkleinern – der Viewer zeigt ohnehin nur m_previewSize. MLT skaliert Rechtecke/Blur dabei mit.
    if (!qEnvironmentVariableIsSet("SCHNEIDI_FULLRES_PREVIEW")) {
        m_consumer->set("width", m_previewSize.width());
        m_consumer->set("height", m_previewSize.height());
    }
    applyAudioState(); // im Stand kein Audiogerät (siehe dort)
    // Kleiner Vorlauf-Puffer: sonst läuft der Ton nach Pause/Seek noch ~1 s weiter
    m_consumer->set("buffer", 2);
    m_consumer->set("prefill", 1);
    m_consumer->set("audio_buffer", audioBuffer());
    m_consumer->listen("consumer-frame-show", this, (mlt_listener)EngineCallbacks::onFrameShow);
    return true;
}

void Engine::setFormat(const ProjectFormat& format)
{
    if (format == m_format && m_profile) return;
    // Alles, was am alten Profil hängt, in dieser Reihenfolge abbauen: Consumer zuerst (liest die Producer)
    if (m_consumer) m_consumer->stop();
    m_preroll->clear();
    m_current = nullptr;
    m_consumer.reset();
    {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        m_mixer.reset();
        m_timeline.reset();
    }
    m_source.reset();
    m_builder.reset();
    m_factory.reset();
    m_profile.reset();

    m_format = format;
    m_renderCache->setFormat(format);
    m_stills->setFormat(format);
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
        if (info.hasAudio) info.audioStreams = audioStreamsOf(p);
        info.length = p.get_length();
    }
    return info;
}

QImage Engine::thumbnail(const QString& path, int frame, const QSize& size)
{
    ProducerFactory factory(*m_profile);
    const std::unique_ptr<Mlt::Producer> producer = factory.open(path);
    Mlt::Producer& p = *producer;
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
    ProducerFactory factory(*profile);
    std::unique_ptr<TimelineBuilder> builder;
    std::unique_ptr<Mlt::Producer> producer;
    if (m_mode == Mode::Source && !m_sourcePath.isEmpty()) {
        producer = factory.open(m_sourcePath);
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
    m_renderCache->sync(tl); // fehlende Clip-Ausgaben einreihen, überholte verwerfen
    const bool active = m_mode == Mode::Timeline;
    const int pos = m_position;
    if (active && m_consumer) m_consumer->stop(); // alten Tractor nicht mehr lesen lassen
    auto hooks = std::make_unique<MixerHooks>();
    auto tractor = m_builder->build(tl, hooks.get());
    if (hooks->master.meter) hooks->master.meter->set("_loudness", m_loudness.get(), 0);
    m_preroll->setPoints(m_builder->prerollPoints(), 3 * m_format.rate.timebase(), m_previewSize);
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
    auto p = m_factory->open(m_proxies->resolve(path));
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
    applyAudioState();
    m_current->seek(position);
    m_position = position;
    m_consumer->connect(*m_current);
    m_consumer->start();
    refresh();
    emit positionChanged(position);
    emit speedChanged(m_speed);
}

// Ton nur bei laufender Wiedergabe/Spulen. sdl2_audio schließt das Audiogerät bei jedem stop() und öffnet es
// beim ersten Frame nach start() neu – das knackt. Timeline-Umbauten (Inspector) starten den Consumer neu, darum
// im Stand audio_off: dann wird gar kein Gerät geöffnet. scrub_audio im Stand aus, sonst schickt jeder Refresh
// ein Audio-Häppchen raus.
void Engine::applyAudioState()
{
    const bool playing = m_speed != 0.0;
    m_consumer->set("audio_off", playing ? 0 : 1);
    m_consumer->set("scrub_audio", playing ? 1 : 0);
    // Lautheit nur bei normaler Wiedergabe der Timeline (Spulen/Rückwärts/Quelle verfälschen die Messung)
    m_loudness->active = m_speed == 1.0 && m_mode == Mode::Timeline;
}

int Engine::audioBuffer()
{
#ifdef Q_OS_WIN
    constexpr int fallback = 2048; // 1024 knackte unter Windows (Nutzer-Test 1)
#else
    constexpr int fallback = 1024;
#endif
    const int samples = QSettings().value("audio/buffer", fallback).toInt();
    return samples >= 256 && samples <= 16384 ? samples : fallback;
}

void Engine::setAudioBuffer(int samples)
{
    QSettings().setValue("audio/buffer", samples);
    // sdl2_audio öffnet das Gerät bei jedem Play neu (applyAudioState) und liest den Wert dann
    if (m_consumer) m_consumer->set("audio_buffer", audioBuffer());
    qInfo("Audio-Puffer: %d Samples", audioBuffer());
}

QString Engine::audioDriver()
{
#ifdef Q_OS_WIN
    // SDL 2.32 über WASAPI knackte an jeder Bildgrenze und verfälschte den Pegel (7.1-Headset, 44,1 kHz);
    // DirectSound lieferte im Mitschnitt denselben Ton sauber (Nutzer-Test 1)
    constexpr const char* fallback = "directsound";
#else
    constexpr const char* fallback = "";
#endif
    return QSettings().value("audio/driver", fallback).toString();
}

void Engine::setAudioDriver(const QString& driver)
{
    QSettings().setValue("audio/driver", driver);
}

void Engine::applyAudioSettings()
{
    const QString driver = audioDriver();
    if (!driver.isEmpty() && !qEnvironmentVariableIsSet("SDL_AUDIODRIVER"))
        qputenv("SDL_AUDIODRIVER", driver.toUtf8());
    const QString used = qEnvironmentVariable("SDL_AUDIODRIVER");
    qInfo("Audio: Treiber %s, Puffer %d Samples", used.isEmpty() ? "automatisch" : qPrintable(used), audioBuffer());
}

LoudnessReading Engine::loudness() const
{
    LoudnessReading r;
    std::lock_guard<std::mutex> lock(m_loudness->mutex);
    const LoudnessMeter& m = m_loudness->meter;
    r.momentary = m.momentary();
    r.shortTerm = m.shortTerm();
    r.integrated = m.integrated();
    r.range = m.range();
    r.seconds = m.measuredSeconds();
    r.truePeak = m_loudness->truePeak.maxDb();
    return r;
}

void Engine::resetLoudness()
{
    std::lock_guard<std::mutex> lock(m_loudness->mutex);
    m_loudness->meter.reset();
    m_loudness->truePeak.reset();
}

void Engine::setColorBypass(bool on)
{
    if (m_gradeBypass->exchange(on) == on) return;
    refresh(); // Filter lesen den Schalter beim nächsten Bild, kein Neuaufbau nötig
    emit colorBypassChanged(on);
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
    const bool wasPlaying = m_speed != 0.0;
    m_speed = speed;
    m_current->set_speed(speed);
    if (speed == 0.0) {
        m_current->seek(m_position); // exakt auf dem angezeigten Frame stehen bleiben
        m_consumer->purge();         // gepufferten Ton sofort verwerfen
        if (wasPlaying) {
            // Audiogerät gleich beim Anhalten schließen (Neustart ohne Ton) statt erst beim nächsten
            // Timeline-Umbau im Stand – das Schließen knackt, beim Anhalten fällt es nicht auf
            m_consumer->stop();
            applyAudioState();
            m_consumer->start();
        }
    }
    applyAudioState();
    refresh();
    emit speedChanged(speed);
}

void Engine::seek(int frame)
{
    if (!m_current) return;
    frame = std::max(0, frame);
    const bool playing = m_speed != 0.0;
    // Während der Wiedergabe schon unterwegs zu diesem Frame: nicht noch einmal Puffer verwerfen und neu dekodieren
    if (playing && m_seekTarget == frame) return;
    ++m_seekEpoch;
    m_seekSkipped = 0;
    m_seekTarget = playing ? frame : -1;
    m_current->seek(frame);
    m_consumer->purge();
    m_position = frame;
    // Nur im Stand neu zeichnen lassen: während der Wiedergabe kommt das nächste Bild ohnehin, und ein Refresh
    // schickt mit scrub_audio zusätzlich ein Ton-Häppchen raus (hörbares Stottern nach dem Sprung)
    if (!playing) refresh();
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
    m_preroll->update(pos, m_mode == Mode::Timeline ? m_speed : 0.0);
    if (m_speed != 0.0 && pos != m_position.exchange(pos)) emit framePosition(pos, m_seekEpoch);
    if (m_speed != 0.0 && m_mode == Mode::Timeline) emitLevels();
}

void Engine::emitLevels()
{
    // Die Pegelmesser merken sich den Pegel des zuletzt verarbeiteten Tons
    // (läuft dem Bild wegen des kleinen Puffers minimal voraus – für Meter egal)
    auto read = [](const MixerHooks::Strip& s, QVector<float>& out, const char* key) {
        float l = -200.f, r = -200.f;
        if (s.audible && s.meter) {
            const QByteArray k0 = QByteArray(key) + ".0", k1 = QByteArray(key) + ".1";
            if (s.meter->get(k0.constData())) l = s.meter->get_double(k0.constData());
            r = s.meter->get(k1.constData()) ? float(s.meter->get_double(k1.constData())) : l; // Mono
        }
        out << l << r;
    };
    QVector<float> levels;
    {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        if (!m_mixer) return;
        for (const auto& s : m_mixer->tracks) read(s, levels, "_audio_level");
        read(m_mixer->master, levels, "_true_peak"); // Master in dBTP (True Peak)
    }
    emit audioLevels(levels); // queued -> UI-Thread
}
