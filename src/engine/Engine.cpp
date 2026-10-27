#include "engine/Engine.h"

#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/MediaAspect.h"
#include "core/Multicam.h"
#include "core/TimelineOps.h"
#include "engine/Background.h"
#include "engine/Bundle.h"
#include "engine/Frei0r.h"
#include "engine/LiveUpdate.h"
#include "engine/Preroll.h"
#include "engine/PreviewConsumer.h"
#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "engine/Stabilizer.h"
#include "engine/StillFetcher.h"
#include "engine/TimelineBuilder.h"
#include "engine/TimelineBuilderDetail.h"
#include "engine/audio/Path.h"
#include "engine/audio/Player.h"
#include "ui/MulticamGrid.h"

#include <Mlt.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

// Hands freed memory back to the system. glibc raises its mmap threshold after the first freed frame buffer (several
// MB each), from then on frames come from the heap and stay there when freed: every multicam grid built (one decoder
// per cell) left hundreds of MB behind (4 GB after flipping the pages of 20 angles a few times).
void releaseFreedMemory()
{
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

constexpr int kSoundRate = 48000; // the own audio engine's rate (the device converts if it runs at another)

// Own audio engine: what the picture graph is built from – no sound tracks, no master (the player has them)
Timeline pictureOnly(const Timeline& tl)
{
    Timeline p = tl;
    p.audio.clear();
    p.masterVolumeDb = 0;
    p.masterLimiter = false;
    return p;
}

// Same picture graph: only the sound differs (the length counts: the graph lasts as long as the sound)
bool samePicture(const Timeline& a, const Timeline& b)
{
    return pictureOnly(a) == pictureOnly(b) && TimelineOps::endFrame(a) == TimelineOps::endFrame(b)
           && TimelineOps::sameNested(a, b);
}

} // namespace

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
    m_seekRelease = new QTimer(this);
    m_seekRelease->setSingleShot(true);
    m_seekRelease->setInterval(120);
    connect(m_seekRelease, &QTimer::timeout, this, [this] {
        m_seekFade->release();
        if (m_player) m_player->setMuted(false);
    });
    if (Audio::ownPath()) {
        Audio::Player::Options o;
        o.rate = kSoundRate;
        o.buffer = audioBuffer();
        o.backend = audioDriver();
        o.output = audioOutput();
        m_player = std::make_unique<Audio::Player>(o);
        m_player->setLoudness(m_loudness.get());
    }
    // Messages that were already on their way when jumping made the playhead briefly jump back
    connect(this, &Engine::framePosition, this, [this](int frame, int epoch) {
        if (epoch != m_seekEpoch) return;
        emit positionChanged(frame);
        stopAtEnd();
    }, Qt::QueuedConnection);
}

Engine::~Engine()
{
    stopOpening();
    if (m_consumer) m_consumer->stop();
    m_consumer.reset();
    m_preview.reset();
    m_player.reset(); // feeds m_loudness
    m_consumerProfile.reset();
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
    Bundle::prepareMltCaches();
    Frei0r::registerEffects();
    MediaAspect::setProbe(&Engine::pictureAspect);
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
    m_factory->setPreview(true);
    m_builder = std::make_unique<TimelineBuilder>(*m_profile);
    m_builder->setPreview(true);
    // Only the image from the proxy: audio decodes cheaply and thus stays exact as in the export
    m_builder->setResolver([this](const QString& path, TrackKind kind) {
        return kind == TrackKind::Video ? m_proxies->resolve(path) : path;
    });
    m_builder->setGradeBypass(m_gradeBypass);
    m_builder->setClipCache([this](const Clip& c) { return m_renderCache->resolve(c); });
    m_gridBuilder = std::make_unique<TimelineBuilder>(*m_profile);
    m_gridBuilder->setPreview(true);
    m_gridBuilder->setResolver([this](const QString& path, TrackKind kind) {
        return kind == TrackKind::Video ? m_proxies->resolve(path) : path;
    });
    m_gridBuilder->setGradeBypass(m_gradeBypass);
    m_gridBuilder->setGridSound(!m_player); // own audio engine: the player plays the angle's sound
    // Preview image in the project's aspect ratio (otherwise portrait gets distorted), even dimensions
    const QSize fit = m_format.size().scaled(960, 960, Qt::KeepAspectRatio);
    m_previewSize = QSize(std::max(2, fit.width() & ~1), std::max(2, fit.height() & ~1));

    m_consumerProfile = makeProfile(m_format);
    if (m_player) {
        // Own audio engine: the picture follows the player's clock, MLT renders no sound
        m_preview = std::make_unique<PreviewConsumer>(*m_consumerProfile, [this] { return clockFrames(); });
        m_consumer = std::make_unique<Mlt::Consumer>(m_preview->consumer());
        m_consumer->set("audio_off", 1);
    } else {
        for (const char* id : {"sdl2_audio", "rtaudio"}) {
            m_consumer = std::make_unique<Mlt::Consumer>(*m_consumerProfile, id);
            if (m_consumer->is_valid()) break;
        }
        if (!m_consumer || !m_consumer->is_valid()) {
            if (error) *error = T("Kein MLT-Audio-Consumer (sdl2_audio/rtaudio) gefunden.");
            return false;
        }
        m_consumer->set("terminate_on_pause", 0);
    }
    m_consumer->set("real_time", 1);
    // Scale smoothly like the export: the audio consumers default to "nearest", so downscaled pictures (preview size,
    // multicam cells, transform zoom) dropped fine detail – 1-px lines vanished in the multicam grid
    m_consumer->set("rescale", "bilinear");
    // The render thread delivers RGBA, which the Viewer shows anyway: with the default (YUV) every clip with an
    // RGBA filter (grade, windows) was converted RGBA -> YUV -> RGBA – a 1080p60 project with a grade on every clip
    // played at ~36 instead of ~51 fps
    m_consumer->set("mlt_image_format", "rgba");
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
    if (!m_player) m_consumer->set("audio_buffer", audioBuffer());
    m_consumer->listen("consumer-frame-show", this, (mlt_listener)EngineCallbacks::onFrameShow);
    return true;
}

void Engine::setFormat(const ProjectFormat& format)
{
    if (format == m_format && m_profile) return;
    const bool waiting = m_opening != nullptr;
    stopOpening(); // its producers belong to the old builder
    // Tear down everything attached to the old profile in this order: consumer first (reads the producers)
    if (m_consumer) m_consumer->stop();
    if (m_player) m_player->pause();
    m_preroll->clear();
    m_current = nullptr;
    m_consumer.reset();
    m_preview.reset();
    m_consumerProfile.reset();
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
    if (waiting) updateTimeline(std::exchange(m_waiting, Timeline{})); // what was being opened, now in the new format
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

QString Engine::startTimecode(const QString& path)
{
    const auto profile = makeProfile(ProjectFormat{});
    Mlt::Producer p(*profile, "avformat", path.toUtf8().constData());
    if (!p.is_valid()) return {};
    // The container's tag, else the one of the video stream (QuickTime: a timecode track, shown on the stream)
    QString container, stream;
    const QString video = QString("meta.attr.%1.stream.timecode.markup").arg(p.get_int("video_index"));
    for (int i = 0; i < p.count(); ++i) {
        const QString name = p.get_name(i);
        if (name == "meta.attr.timecode.markup") container = p.get(i);
        else if (name == video || (stream.isEmpty() && name.endsWith(".stream.timecode.markup"))) stream = p.get(i);
    }
    return (container.isEmpty() ? stream : container).trimmed();
}

double Engine::pictureAspect(const QString& path)
{
    // Opened like the TimelineBuilder opens it, so the shape is the one it fits into the frame
    const auto profile = makeProfile(ProjectFormat{});
    ProducerFactory factory(*profile); // owns profiles the producer may use: declared first, destroyed last
    const std::unique_ptr<Mlt::Producer> p = factory.open(path);
    if (!p || !p->is_valid() || p->get_int("video_index") < 0) return 0;
    double aspect = TimelineBuilderDetail::sourceAspect(*p);
    if (aspect <= 0) { // still images: size only after the first frame
        std::unique_ptr<Mlt::Frame> frame(p->get_frame());
        aspect = TimelineBuilderDetail::sourceAspect(*p);
    }
    return aspect;
}

QImage Engine::thumbnail(const QString& path, int frame, const QSize& size)
{
    return thumbnail(m_format, path, frame, size);
}

QImage Engine::thumbnail(const ProjectFormat& format, const QString& path, int frame, const QSize& size, bool ownShape)
{
    const auto profile = makeProfile(format);
    ProducerFactory factory(*profile);
    // Proxy if present: a 4K original takes ~0.2–0.7 s per exactly sought frame
    const std::unique_ptr<Mlt::Producer> producer = factory.open(ProxyManager::decodeSource(path));
    Mlt::Producer& p = *producer;
    if (!p.is_valid()) return {};
    p.seek(frame);
    std::unique_ptr<Mlt::Frame> f(p.get_frame());
    if (!f) return {};
    // Fetch in the project's (or the file's) aspect ratio and center at the desired size (black borders); MLT fits the
    // picture into the size asked for, so asking in the file's shape gives it without borders
    QSize fit = format.size().scaled(size, Qt::KeepAspectRatio);
    if (const double aspect = ownShape ? TimelineBuilderDetail::sourceAspect(p) : 0; aspect > 0)
        fit = QSizeF(aspect * size.height(), size.height()).scaled(QSizeF(size), Qt::KeepAspectRatio).toSize();
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

namespace {

// Frame `position` of `producer` as RGBA (with transparency) in the profile's size; empty on failure
QImage rgbaFrame(Mlt::Profile& profile, Mlt::Producer& producer, int position)
{
    if (!producer.is_valid()) return {};
    producer.seek(position);
    std::unique_ptr<Mlt::Frame> f(producer.get_frame());
    if (!f) return {};
    mlt_image_format fmt = mlt_image_rgba;
    int w = profile.width(), h = profile.height();
    const uint8_t* data = f->get_image(fmt, w, h);
    if (!data || w <= 0 || h <= 0) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), data, size_t(w) * h * 4);
    return img;
}

} // namespace

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
    if (!producer) return {};
    const QImage img = rgbaFrame(*profile, *producer, m_position);
    if (img.isNull()) return {};
    // Transparent spots (empty timeline, zoomed-out clips) black as in the Viewer
    QImage out(img.size(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.drawImage(0, 0, img);
    return out;
}

QImage Engine::grabFrame(const Timeline& tl, int position)
{
    auto profile = makeProfile(m_format);
    TimelineBuilder builder(*profile);
    builder.setTransparentBackground(true);
    const std::unique_ptr<Mlt::Producer> producer = builder.build(tl);
    return producer ? rgbaFrame(*profile, *producer, position) : QImage();
}

void Engine::updateTimeline(const Timeline& tl)
{
    const bool hinted = std::exchange(m_mixerOnlyNext, false);
    // Files are being opened: the newest timeline is built when they are open
    if (m_opening) {
        m_waiting = tl;
        m_waitingHinted = m_waitingHinted || hinted;
        return;
    }
    applyTimeline(tl, hinted);
    stopAtEnd(); // deleted while playing: the playhead may lie behind the new end
}

struct Engine::Opening {
    quint64 id = 0;
    std::vector<QString> jobs;
    std::vector<std::unique_ptr<Mlt::Producer>> producers; // written by the thread, read after join
    std::atomic<bool> cancel{false};
    std::thread thread;
};

bool Engine::openInBackground(const Timeline& picture, const Timeline& tl)
{
    if (!m_openInBackground) return false;
    std::vector<QString> jobs = m_builder->openJobs(picture);
    if (jobs.empty()) return false;
    // Other content (a project opened, nothing of the old picture stays): black instead of the old picture meanwhile
    auto media = [](const Timeline& t) {
        QSet<QString> paths;
        for (const auto* tracks : {&t.video, &t.audio})
            for (const Track& track : *tracks)
                for (const Clip& c : track.clips)
                    if (!c.mediaPath.isEmpty()) paths.insert(c.mediaPath);
        return paths;
    };
    const QSet<QString> shown = media(m_built);
    if (!m_timeline || (!shown.isEmpty() && !shown.intersects(media(tl)))) applyTimeline(Timeline{}, false);

    auto o = std::make_unique<Opening>();
    o->id = ++m_openings;
    o->jobs = std::move(jobs);
    m_waiting = tl;
    m_waitingHinted = false;
    Opening* raw = o.get();
    o->thread = std::thread([this, raw, builder = m_builder.get()] {
        {
            const Background::Foreground busy; // thumbnails, waveforms … wait (ends here: quitting must not wait)
            raw->producers = builder->openAll(raw->jobs, &raw->cancel);
        }
        const quint64 id = raw->id;
        QMetaObject::invokeMethod(this, [this, id] { finishOpening(id); }, Qt::QueuedConnection);
    });
    m_opening = std::move(o);
    emit openingChanged(true);
    return true;
}

void Engine::finishOpening(quint64 id)
{
    if (!m_opening || m_opening->id != id) return; // stopped meanwhile (stopOpening)
    m_opening->thread.join();
    m_builder->adopt(m_opening->jobs, std::move(m_opening->producers));
    m_opening.reset();
    const Timeline tl = std::exchange(m_waiting, Timeline{});
    applyTimeline(tl, std::exchange(m_waitingHinted, false)); // opens again if the newest one needs more files
    stopAtEnd();
    if (!m_opening) emit openingChanged(false);
}

void Engine::stopOpening()
{
    if (!m_opening) return;
    m_opening->cancel = true; // the running opens finish (MLT cannot be interrupted), the rest is skipped
    m_opening->thread.join();
    m_opening.reset(); // its producers go before the builder they belong to
    emit openingChanged(false);
}

void Engine::applyTimeline(const Timeline& tl, bool hinted)
{
    const Timeline picture = m_player ? pictureOnly(tl) : tl;
    if (m_player) {
        // The sound waits for the picture while its files open (otherwise new sound under the old picture)
        if (!(m_timeline && samePicture(m_built, tl)) && openInBackground(picture, tl)) return;
        // Own audio engine: the player takes every change live; the picture graph only changes with the picture
        if (m_mode == Mode::Timeline) m_player->setTimeline(tl, m_format.rate);
        if (m_timeline && samePicture(m_built, tl)) {
            m_built = tl;
            return;
        }
    }
    const bool mixerOnly = !m_player && (hinted || TimelineBuilder::mixerOnlyChange(m_built, tl));
    {
        std::lock_guard<std::mutex> lock(m_mixerMutex);
        if (mixerOnly && m_timeline && m_mixer && TimelineBuilder::applyMixer(tl, *m_mixer)) {
            m_built = tl;
            if (m_speed == 0.0) refresh();
            return;
        }
    }
    if (!m_player && openInBackground(picture, tl)) return;
    const Background::Foreground busy; // the build opens files too: background work waits
    m_renderCache->sync(tl); // queue missing clip outputs, discard outdated ones
    // Stabilization: analyses the timeline needs run in the background (until then the clips play unstabilized)
    Stabilizer::instance()->sync(Stabilizer::missing(tl, m_format));
    m_builder->setMinimumLength(m_player ? TimelineOps::endFrame(tl) : 0);
    auto hooks = std::make_unique<MixerHooks>();
    auto tractor = m_builder->build(picture, m_player ? nullptr : hooks.get());
    // Only values changed (Inspector): copy them into the running tractor instead of restarting the consumer
    if (m_timeline && LiveUpdate::valuesOnly(m_player ? pictureOnly(m_built) : m_built, picture)) {
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
    m_seekFade->restart = true; // the sound starts anew (device reopened): fade in
    if (active && m_consumer) connectProducer(m_timeline.get(), pos);
}

void Engine::setMediaLengths(const QHash<QString, int>& lengths)
{
    if (m_player) m_player->setFileLengths(lengths);
}

void Engine::showTimeline(int position, bool keepSpeed)
{
    if (!m_timeline) return;
    if (!keepSpeed) m_speed = 0;
    m_mode = Mode::Timeline;
    m_offset = 0;
    connectProducer(m_timeline.get(), position);
    syncPlayer(position, true);
    if (m_grid) { // not shown any more: free its decoders
        m_gridHooks.reset();
        m_grid.reset();
        m_gridContent = {};
        m_gridAngles.clear();
        releaseFreedMemory();
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
    // One decoder per cell: their own frame threads only cost memory (each holds frames of its own) once the cells
    // keep all cores busy; a single cell decodes with all of them
    const int cells = std::min<int>(angles.size(), MulticamGrid::perPage(angles.size(), options.columns));
    m_gridBuilder->setDecoderThreads(cells <= 1 ? 0 : std::max(1, QThread::idealThreadCount() / cells));
    auto hooks = std::make_unique<MulticamGridHooks>();
    auto grid = m_gridBuilder->buildMulticamGrid(content, angles, options, hooks.get());
    if (!grid || !grid->is_valid()) return false;
    m_consumer->stop(); // the old grid must not be read anymore
    // Building took a while and the old grid played on meanwhile: go on from there, not from `position`
    if (m_speed != 0.0 && m_mode == Mode::Multicam) position = m_position;
    m_grid = std::move(grid);
    m_gridHooks = std::move(hooks);
    releaseFreedMemory(); // the old grid's decoders and frames
    m_gridContent = content;
    m_gridAngles = angles;
    m_gridColumns = options.columns;
    m_gridPage = options.page;
    m_offset = offset;
    const bool changed = m_mode != Mode::Multicam;
    m_mode = Mode::Multicam;
    m_gridSoundAngle = options.activeAngle;
    connectProducer(m_grid.get(), position);
    syncPlayer(position, true);
    if (changed) emit modeChanged(m_mode);
    return true;
}

void Engine::setMulticamAudio(int angle)
{
    if (m_player && m_mode == Mode::Multicam && angle != m_gridSoundAngle) {
        m_gridSoundAngle = angle;
        syncPlayer(m_position, false); // crossfades to the other angle's sound
    }
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
    if (m_player) { // the sound of the original file (the picture may come from the proxy), stream 0 like MLT
        Clip c;
        c.id = 1;
        c.mediaPath = path;
        c.out = std::max(0, m_source->get_length() - 1);
        m_sourceSound = Timeline{};
        m_sourceSound.audio.resize(1);
        m_sourceSound.audio[0].kind = TrackKind::Audio;
        m_sourceSound.audio[0].clips << c;
    }
    connectProducer(m_source.get(), std::max(0, position));
    syncPlayer(std::max(0, position), true);
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
    // Loudness only during normal playback of the timeline (shuttling/reverse/source distort the measurement)
    m_loudness->active = m_speed == 1.0 && m_mode == Mode::Timeline;
    if (m_player) return; // own audio engine: MLT renders no sound
    const bool playing = m_speed != 0.0;
    m_consumer->set("audio_off", playing ? 0 : 1);
    m_consumer->set("scrub_audio", playing ? 1 : 0);
}

double Engine::clockFrames() const
{
    if (!m_player->playing()) return NAN; // a frame still on its way from before the stop: at once
    return m_player->position() * m_format.rate.fps() / kSoundRate;
}

double Engine::soundFrame() const
{
    if (!m_player) return NAN;
    return clockFrames() + m_offset; // NaN stays NaN
}

double Engine::soundStart(int frame, double speed) const
{
    // Forward the sound of a frame starts at its first sample, backward after its last
    return double(speed < 0 ? frame + 1 : frame) * kSoundRate / m_format.rate.fps();
}

// Own audio engine: the player plays what the current mode shows (positions = frames of the connected producer);
// restart: playing goes on at `position` (another producer: another time axis)
void Engine::syncPlayer(int position, bool restart)
{
    if (!m_player) return;
    switch (m_mode) {
    case Mode::Timeline: m_player->setTimeline(m_built, m_format.rate); break;
    case Mode::Source: m_player->setTimeline(m_sourceSound, m_format.rate); break;
    case Mode::Multicam: {
        Timeline sound;
        if (m_gridSoundAngle >= 0 && m_gridSoundAngle < m_gridAngles.size())
            sound = Multicam::angleTimeline(m_gridContent, m_gridAngles[m_gridSoundAngle], TrackKind::Audio);
        m_player->setTimeline(sound, m_format.rate);
        break;
    }
    }
    if (!restart) return;
    if (m_speed != 0.0) m_player->play(soundStart(position - m_offset, m_speed), m_speed);
    else m_player->pause();
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
    if (m_player) m_player->setDevice(audioBuffer(), audioDriver(), audioOutput());
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

QString Engine::audioOutput()
{
    return QSettings().value("audio/output").toString();
}

void Engine::setAudioOutput(const QString& id)
{
    QSettings().setValue("audio/output", id);
    if (m_player) m_player->setDevice(audioBuffer(), audioDriver(), audioOutput());
    qInfo("Audio-Ausgabe: %s", id.isEmpty() ? "Systemstandard" : qPrintable(id));
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

int Engine::lastFrame() const
{
    switch (m_mode) {
    case Mode::Timeline: return TimelineOps::endFrame(m_built) - 1;
    case Mode::Source: return m_source ? m_source->get_length() - 1 : -1;
    case Mode::Multicam: break;
    }
    return INT_MAX;
}

bool Engine::atEnd(double speed) const
{
    if (m_mode == Mode::Multicam) return false;
    return speed > 0 ? m_position >= lastFrame() : m_position <= 0;
}

// The producer pauses by itself at its end, but the engine kept its speed: nothing moved, the button still showed
// playing, and the next rebuild (an edit, an import) played on by itself (user report 2026-10-24). Stop for real.
void Engine::stopAtEnd()
{
    if (m_speed != 0.0 && m_current && atEnd(m_speed)) pause();
}

void Engine::setSpeed(double speed)
{
    if (!m_current) return;
    if (speed != 0.0 && m_speed == 0.0 && atEnd(speed)) {
        // Play at the end starts from the front (Premiere, Final Cut); backwards at the start there is nothing
        if (speed < 0 || lastFrame() <= 0) return;
        seek(0);
    }
    const bool wasPlaying = m_speed != 0.0;
    const double before = m_speed;
    m_speed = speed;
    m_current->set_speed(speed);
    if (m_player) {
        // Own audio engine: the device stays open, the player fades. The picture waits for the sound's clock.
        const int at = std::max(0, m_position - m_offset);
        if (speed == 0.0) {
            m_player->pause();
            seekProducer(at); // stay exactly on the shown frame
        } else if (wasPlaying && (before > 0) == (speed > 0)) {
            m_player->play(m_player->position(), speed); // faster/slower from where the sound is
        } else {
            if (wasPlaying) { // turning around: the picture rendered ahead lies the other way
                ++m_seekEpoch;
                m_seekSkipped = 0;
                m_seekTarget = at;
                seekProducer(at);
            }
            m_player->play(soundStart(at, speed), speed);
        }
        applyAudioState();
        refresh();
        emit speedChanged(speed);
        return;
    }
    if (speed == 0.0) {
        seekProducer(std::max(0, m_position - m_offset)); // stay exactly on the shown frame, no buffered audio
        if (wasPlaying) {
            // Close the audio device right when stopping (restart without audio) instead of only at the next
            // timeline rebuild while stopped – closing crackles, which is not noticeable when stopping
            m_consumer->stop();
            applyAudioState();
            m_consumer->start();
        }
    } else if (m_mode == Mode::Timeline && (!wasPlaying || speed != m_seekFade->direction)) {
        m_seekFade->play(speed);
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
    if (m_player) {
        if (playing) {
            // The next seek within moments: the playhead is being dragged -> silent until it rests
            if (m_lastSeek.isValid() && m_lastSeek.elapsed() < 150) {
                m_player->setMuted(true);
                m_seekRelease->start();
            }
            m_player->play(soundStart(at, m_speed), m_speed);
        }
        m_lastSeek.start();
        seekProducer(at);
        m_position = frame;
        if (!playing) refresh();
        emit positionChanged(frame);
        return;
    }
    const bool timelinePlaying = playing && m_mode == Mode::Timeline;
    if (timelinePlaying) {
        // The master meter fades at the jump by itself (SeekFade). The next seek within moments: the playhead is
        // being dragged -> silent until it rests (each seek's own fade-in crackled)
        if (m_lastSeek.isValid() && m_lastSeek.elapsed() < 150) {
            m_seekFade->hold = true;
            m_seekRelease->start();
        }
    } else {
        m_seekFade->release();
    }
    m_lastSeek.start();
    if (timelinePlaying) ++m_seekFade->seeks;
    seekProducer(at);
    m_position = frame;
    // Only redraw when stopped: during playback the next image comes anyway, and a refresh
    // also sends out an audio snippet with scrub_audio (audible stutter after the jump)
    if (!playing) refresh();
    emit positionChanged(frame);
}

void Engine::seekProducer(int at)
{
    if (m_preview) {
        m_preview->seek(at); // never overridden by one of its catch-up jumps
        return;
    }
    m_current->seek(at);
    m_consumer->purge();
}

void Engine::onFrameShown(void* mltFrame)
{
    if (!mltFrame) return;
    Mlt::Frame frame(static_cast<mlt_frame>(mltFrame));
    const int pos = frame.get_position();
    const double speed = m_speed; // read once: the UI thread may change it meanwhile
    const Mode mode = m_mode;
    const int target = m_seekTarget;
    if (target >= 0) {
        // Only show again from the jump target on (buffer still contains images from before); emergency brake after 50 images
        const bool arrived = speed == 0.0 || (speed > 0 ? pos >= target && pos <= target + 60 : pos <= target && pos >= target - 60);
        if (!arrived && ++m_seekSkipped < 50) return;
        m_seekTarget = -1;
    }
    if (m_player && speed != 0.0) m_player->release(); // the picture is there: the sound may start
    mlt_image_format fmt = mlt_image_rgba;
    int w = m_previewSize.width(), h = m_previewSize.height();
    const uint8_t* data = frame.get_image(fmt, w, h);
    if (data && w > 0 && h > 0) {
        QImage img(w, h, QImage::Format_RGBA8888);
        std::memcpy(img.bits(), data, size_t(w) * h * 4);
        emit frameReady(img); // queued -> UI thread
    }
    m_preroll->update(pos, mode == Mode::Timeline ? speed : 0.0);
    const int shown = pos + m_offset; // timeline frame
    if (speed != 0.0 && shown != m_position.exchange(shown)) emit framePosition(shown, m_seekEpoch);
    if (speed != 0.0 && mode == Mode::Timeline) emitLevels();
}

void Engine::emitLevels()
{
    if (m_player) { // peaks of what was heard since the last frame
        const std::vector<float> peaks = m_player->takeLevels();
        if (peaks.empty()) return;
        QVector<float> levels;
        levels.reserve(int(peaks.size()));
        for (const float p : peaks) levels << (p > 1e-10f ? 20.f * std::log10(p) : -200.f);
        emit audioLevels(levels); // queued -> UI thread
        return;
    }
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
