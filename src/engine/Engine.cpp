#include "engine/Engine.h"

#include "core/I18n.h"
#include "core/Loudness.h"
#include "engine/Bundle.h"
#include "engine/Frei0r.h"
#include "engine/LiveUpdate.h"
#include "engine/Preroll.h"
#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "engine/Stabilizer.h"
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
    // Runs in the MLT consumer thread as soon as a frame is to be shown
    static void onFrameShow(mlt_properties, void* self, mlt_event_data data)
    {
        static_cast<Engine*>(self)->onFrameShown(mlt_event_data_to_frame(data));
    }
};

Engine::Engine(QObject* parent)
    : QObject(parent), m_proxies(new ProxyManager(this)), m_renderCache(new RenderCache(this)),
      m_stills(new StillFetcher(this)),
      m_loudness(std::make_unique<SharedLoudness>()), m_seekFade(std::make_unique<SeekFade>()), m_preroll(std::make_unique<Preroll>())
{
    // Render cache pauses during playback
    connect(this, &Engine::speedChanged, m_renderCache, [this](double s) { m_renderCache->setPlaying(s != 0.0); });
    // Messages that were already on their way when jumping made the playhead briefly jump back
    connect(this, &Engine::framePosition, this, [this](int frame, int epoch) {
        if (epoch == m_seekEpoch) emit positionChanged(frame);
    }, Qt::QueuedConnection);
}

Engine::~Engine()
{
    if (m_consumer) m_consumer->stop();
    m_consumer.reset();
    m_preroll.reset(); // after the consumer (reports every frame to it), before the producers
    m_gridHooks.reset();
    m_grid.reset();
    m_gridBuilder.reset();
    m_timeline.reset();
    m_source.reset();
    m_builder.reset();
    m_factory.reset();
    m_profile.reset();
}

bool Engine::initMlt(QString* error)
{
    // Bundled MLT modules (AppImage/Windows program folder), otherwise those of the system
    Bundle::prepareMltEnvironment();
    Frei0r::prepareEnvironment();
    const QString modules = Bundle::mltModuleDir();
    if (!(modules.isEmpty() ? Mlt::Factory::init() : Mlt::Factory::init(Bundle::pathForMlt(modules).constData()))) {
        if (error) *error = T("MLT konnte nicht initialisiert werden.");
        return false;
    }
    Bundle::configureMltLogging();
    Bundle::pinNumericLocale();
    Frei0r::registerEffects();
    return true;
}

bool Engine::init(QString* error)
{
    if (!initMlt(error)) return false;
    m_format = ProjectFormat{};
    if (!createConsumer(error)) return false;
    updateTimeline(Timeline{});
    showTimeline(0);
    return true;
}

// Create profile, builder and preview consumer for the current project format
bool Engine::createConsumer(QString* error)
{
    m_profile = makeProfile(m_format);
    m_factory = std::make_unique<ProducerFactory>(*m_profile);
    m_builder = std::make_unique<TimelineBuilder>(*m_profile);
    // Only the image from the proxy: audio decodes cheaply and thus stays exact as in the export
    m_builder->setResolver([this](const QString& path, TrackKind kind) {
        return kind == TrackKind::Video ? m_proxies->resolve(path) : path;
    });
    m_builder->setGradeBypass(m_gradeBypass);
    m_builder->setClipCache([this](const Clip& c) { return m_renderCache->resolve(c); });
    m_gridBuilder = std::make_unique<TimelineBuilder>(*m_profile);
    m_gridBuilder->setResolver([this](const QString& path, TrackKind kind) {
        return kind == TrackKind::Video ? m_proxies->resolve(path) : path;
    });
    m_gridBuilder->setGradeBypass(m_gradeBypass);
    // Preview image in the project's aspect ratio (otherwise portrait gets distorted), even dimensions
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
    // Compute directly at preview size (crossfades, transform, effects) instead of project size and then
    // scale down – the Viewer only shows m_previewSize anyway. MLT scales rectangles/blur along with it.
    if (!qEnvironmentVariableIsSet("SCHNEIDI_FULLRES_PREVIEW")) {
        m_consumer->set("width", m_previewSize.width());
        m_consumer->set("height", m_previewSize.height());
    }
    applyAudioState(); // no audio device when stopped (see there)
    // Small lead-in buffer: otherwise the audio keeps running for ~1 s after pause/seek
    m_consumer->set("buffer", 2);
    m_consumer->set("prefill", 1);
    m_consumer->set("audio_buffer", audioBuffer());
    m_consumer->listen("consumer-frame-show", this, (mlt_listener)EngineCallbacks::onFrameShow);
    return true;
}

void Engine::setFormat(const ProjectFormat& format)
{
    if (format == m_format && m_profile) return;
    // Tear down everything attached to the old profile in this order: consumer first (reads the producers)
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
    m_gridHooks.reset();
    m_grid.reset();
    m_gridContent = {};
    m_gridAngles.clear();
    m_offset = 0;
    m_gridBuilder.reset();
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
        m_mode = Mode::Timeline; // source view hung on the old profile
        emit modeChanged(m_mode);
    }
    emit speedChanged(m_speed);
}

MediaInfo Engine::probe(const QString& path)
{
    return probe(m_format, path);
}

MediaInfo Engine::probe(const ProjectFormat& format, const QString& path)
{
    MediaInfo info;
    info.path = path;
    info.name = QFileInfo(path).fileName();
    const auto profile = makeProfile(format);
    const int fps = format.rate.timebase();
    Mlt::Producer p(*profile, path.toUtf8().constData());
    if (!p.is_valid()) return info;

    const QString service = p.get("mlt_service");
    info.isImage = service == "qimage" || service == "pixbuf";
    if (info.isImage) {
        info.hasVideo = true;
        info.length = 5 * fps; // Still image: 5 seconds as in DaVinci
    } else {
        // Audio files with embedded cover do not count as video
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
    // Fetch in the project's aspect ratio and center at the desired size (black borders)
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
    // Own profile and own producers (like the export) -> preview keeps running undisturbed
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
    // Transparent spots (empty timeline, zoomed-out clips) black as in the Viewer
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
            m_built = tl;
            if (m_speed == 0.0) refresh();
            return;
        }
    }
    m_renderCache->sync(tl); // queue missing clip outputs, discard outdated ones
    // Stabilization: analyses the timeline needs run in the background (until then the clips play unstabilized)
    Stabilizer::instance()->sync(Stabilizer::missing(tl, m_format));
    auto hooks = std::make_unique<MixerHooks>();
    auto tractor = m_builder->build(tl, hooks.get());
    // Only values changed (Inspector): copy them into the running tractor instead of restarting the consumer
    if (m_timeline && LiveUpdate::valuesOnly(m_built, tl)) {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        if (LiveUpdate::transfer(*m_timeline, *tractor)) {
            m_built = tl;
            if (m_speed == 0.0) refresh();
            return;
        }
    }
    m_built = tl;
    const bool active = m_mode == Mode::Timeline;
    const int pos = m_position;
    if (active && m_consumer) m_consumer->stop(); // do not let the old tractor be read anymore
    if (hooks->master.meter) {
        hooks->master.meter->set("_loudness", m_loudness.get(), 0);
        hooks->master.meter->set("_seek_fade", m_seekFade.get(), 0);
    }
    m_preroll->setPoints(m_builder->prerollPoints(), 3 * m_format.rate.timebase(), m_previewSize);
    {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        m_mixer = std::move(hooks);
        m_timeline = std::move(tractor);
    }
    if (active && m_consumer) connectProducer(m_timeline.get(), pos);
}

void Engine::showTimeline(int position, bool keepSpeed)
{
    if (!m_timeline) return;
    if (!keepSpeed) m_speed = 0;
    m_mode = Mode::Timeline;
    m_offset = 0;
    connectProducer(m_timeline.get(), position);
    if (m_grid) { // not shown any more: free its decoders
        m_gridHooks.reset();
        m_grid.reset();
        m_gridContent = {};
        m_gridAngles.clear();
    }
    emit modeChanged(m_mode);
}

bool Engine::showMulticam(const Timeline& content, const QVector<MulticamAngle>& angles,
                          const MulticamGridOptions& options, int offset, int position)
{
    if (!m_consumer || !m_gridBuilder) return false;
    if (m_mode == Mode::Multicam && m_grid && content == m_gridContent && angles == m_gridAngles
        && options.columns == m_gridColumns && options.page == m_gridPage) {
        setMulticamAudio(options.activeAngle);
        if (offset != m_offset) { // another segment of the same multicam clip, slipped: same timeline frame
            m_offset = offset;
            const int at = m_position;
            m_seekTarget = -1;
            seek(at);
        }
        return true;
    }
    auto hooks = std::make_unique<MulticamGridHooks>();
    auto grid = m_gridBuilder->buildMulticamGrid(content, angles, options, hooks.get());
    if (!grid || !grid->is_valid()) return false;
    m_consumer->stop(); // the old grid must not be read anymore
    m_grid = std::move(grid);
    m_gridHooks = std::move(hooks);
    m_gridContent = content;
    m_gridAngles = angles;
    m_gridColumns = options.columns;
    m_gridPage = options.page;
    m_offset = offset;
    const bool changed = m_mode != Mode::Multicam;
    m_mode = Mode::Multicam;
    connectProducer(m_grid.get(), position);
    if (changed) emit modeChanged(m_mode);
    return true;
}

void Engine::setMulticamAudio(int angle)
{
    if (m_gridHooks) TimelineBuilder::applyMulticamAudio(*m_gridHooks, angle);
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
    m_offset = 0;
    connectProducer(m_source.get(), std::max(0, position));
    emit modeChanged(m_mode);
}

void Engine::connectProducer(Mlt::Producer* producer, int position)
{
    m_consumer->stop();
    m_current = producer;
    m_current->set_speed(m_speed);
    applyAudioState();
    m_current->seek(std::max(0, position - m_offset));
    m_position = position;
    m_consumer->connect(*m_current);
    m_consumer->start();
    refresh();
    emit positionChanged(position);
    emit speedChanged(m_speed);
}

// Audio only during playback/shuttling. sdl2_audio closes the audio device on every stop() and reopens it
// at the first frame after start() – that crackles. Timeline rebuilds (Inspector) restart the consumer, hence
// audio_off when stopped: then no device is opened at all. scrub_audio off when stopped, otherwise every refresh
// sends out an audio snippet.
void Engine::applyAudioState()
{
    const bool playing = m_speed != 0.0;
    m_consumer->set("audio_off", playing ? 0 : 1);
    m_consumer->set("scrub_audio", playing ? 1 : 0);
    // Loudness only during normal playback of the timeline (shuttling/reverse/source distort the measurement)
    m_loudness->active = m_speed == 1.0 && m_mode == Mode::Timeline;
}

int Engine::audioBuffer()
{
#ifdef Q_OS_WIN
    constexpr int fallback = 2048; // 1024 crackled on Windows (user test 1)
#else
    constexpr int fallback = 1024;
#endif
    const int samples = QSettings().value("audio/buffer", fallback).toInt();
    return samples >= 256 && samples <= 16384 ? samples : fallback;
}

void Engine::setAudioBuffer(int samples)
{
    QSettings().setValue("audio/buffer", samples);
    // sdl2_audio reopens the device on every play (applyAudioState) and then reads the value
    if (m_consumer) m_consumer->set("audio_buffer", audioBuffer());
    qInfo("Audio-Puffer: %d Samples", audioBuffer());
}

QString Engine::audioDriver()
{
#ifdef Q_OS_WIN
    // SDL 2.32 over WASAPI crackled at every frame boundary and distorted the level (7.1 headset, 44.1 kHz);
    // DirectSound delivered the same audio cleanly in the recording (user test 1)
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
    refresh(); // filters read the switch at the next frame, no rebuild needed
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
        m_current->seek(std::max(0, m_position - m_offset)); // stay exactly on the shown frame
        m_consumer->purge();         // discard buffered audio immediately
        if (wasPlaying) {
            // Close the audio device right when stopping (restart without audio) instead of only at the next
            // timeline rebuild while stopped – closing crackles, which is not noticeable when stopping
            m_consumer->stop();
            applyAudioState();
            m_consumer->start();
        }
    } else if (!wasPlaying && m_mode == Mode::Timeline) {
        m_seekFade->fadeIn(m_position, speed);
    }
    applyAudioState();
    refresh();
    emit speedChanged(speed);
}

void Engine::seek(int frame)
{
    if (!m_current) return;
    frame = std::max(0, frame);
    const int at = std::max(0, frame - m_offset); // frame of the producer (multicam grid: of the sequence)
    const bool playing = m_speed != 0.0;
    // During playback already on the way to this frame: do not discard buffers and decode again
    if (playing && m_seekTarget == at) return;
    ++m_seekEpoch;
    m_seekSkipped = 0;
    m_seekTarget = playing ? at : -1;
    if (playing && m_mode == Mode::Timeline) m_seekFade->start(frame, m_speed);
    else m_seekFade->target = -1;
    m_current->seek(at);
    m_consumer->purge();
    m_position = frame;
    // Only redraw when stopped: during playback the next image comes anyway, and a refresh
    // also sends out an audio snippet with scrub_audio (audible stutter after the jump)
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
        // Only show again from the jump target on (buffer still contains images from before); emergency brake after 50 images
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
        emit frameReady(img); // queued -> UI thread
    }
    m_preroll->update(pos, m_mode == Mode::Timeline ? m_speed : 0.0);
    const int shown = pos + m_offset; // timeline frame
    if (m_speed != 0.0 && shown != m_position.exchange(shown)) emit framePosition(shown, m_seekEpoch);
    if (m_speed != 0.0 && m_mode == Mode::Timeline) emitLevels();
}

void Engine::emitLevels()
{
    // The level meters remember the level of the last processed audio
    // (runs minimally ahead of the image because of the small buffer – irrelevant for meters)
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
    emit audioLevels(levels); // queued -> UI thread
}
