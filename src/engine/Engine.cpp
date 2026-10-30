// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Engine.h"

#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/MediaAspect.h"
#include "core/Multicam.h"
#include "core/TimelineOps.h"
#include "engine/Background.h"
#include "engine/Bundle.h"
#include "engine/Exporter.h"
#include "engine/Frei0r.h"
#include "engine/LiveUpdate.h"
#include "engine/MediaProbe.h"
#include "engine/MemoryRelease.h"
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
#include "engine/picture/MediaStills.h"
#include "engine/picture/Renderer.h"
#include "engine/video/Reader.h"
#include "engine/video/Still.h"
#include "ui/MulticamGrid.h"

#include <Mlt.h>
#include <QCoreApplication>
#include <QImageReader>
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

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

// Hands freed memory back to the system right away. glibc raises its mmap threshold after the first freed frame buffer
// (several MB each), from then on frames come from the heap and stay there when freed: every multicam grid built (one
// decoder per cell) left hundreds of MB behind (4 GB after flipping the pages of 20 angles a few times).
void releaseFreedMemory()
{
    MemoryRelease::trimHeap();
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

// A rendered frame as QImage without copying: the image keeps the pooled buffer until it goes away
QImage frameImage(const Picture::Frame& f)
{
    if (f.isNull()) return {};
    auto* keep = new Picture::Frame(f);
    const Picture::Frame& k = *keep;
    return QImage(k.bits(), k.width(), k.height(), k.stride(), QImage::Format_RGBA8888,
                  [](void* p) { delete static_cast<Picture::Frame*>(p); }, keep);
}

// Preview size: the project format scaled down to a longer edge of 960 (even), the full size when the viewer zooms
// to 100 % or more (full) or with SCHNEIDI_FULLRES_PREVIEW
QSize previewSizeOf(const ProjectFormat& format, bool full)
{
    if (full || qEnvironmentVariableIsSet("SCHNEIDI_FULLRES_PREVIEW")) return format.size();
    const QSize fit = format.size().scaled(960, 960, Qt::KeepAspectRatio);
    return QSize(std::max(2, fit.width() & ~1), std::max(2, fit.height() & ~1));
}

} // namespace

// Own picture engine: proxy and render-cache files the renderers read (their worker threads ask; ProxyManager and
// RenderCache are UI-thread objects that look at the disk, so the answers are worked out on the UI thread per
// timeline and swapped in whole)
struct Engine::Resolved {
    std::mutex mutex;
    QHash<QString, QString> proxies; // original -> file the preview reads
    QHash<int, QString> cache;       // top-level clip id -> rendered clip output
    std::shared_ptr<std::atomic<bool>> bypass;

    QString proxy(const QString& path)
    {
        std::lock_guard lock(mutex);
        return proxies.value(path, path);
    }
    QString cached(const Clip& c)
    {
        if (bypass && bypass->load()) return {}; // cached clips contain the correction
        std::lock_guard lock(mutex);
        return cache.value(c.id);
    }
};

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
      m_idleRelease(new IdleRelease([this] { return activity(); }, this)),
      m_loudness(std::make_unique<SharedLoudness>()), m_seekFade(std::make_unique<SeekFade>()), m_preroll(std::make_unique<Preroll>())
{
    // Render cache pauses during playback
    connect(this, &Engine::speedChanged, m_renderCache, [this](double s) { m_renderCache->setPlaying(s != 0.0); });
    // MLT's pool and the heap keep what playback, rebuilds and rendered clips used: given back once all is idle
    connect(this, &Engine::speedChanged, m_idleRelease, [this](double s) {
        if (s == 0.0) m_idleRelease->request();
    });
    connect(m_renderCache, &RenderCache::cacheChanged, m_idleRelease, &IdleRelease::request);
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
    m_pump.reset(); // its threads use the renderers
    m_timelineContent.reset();
    m_gridRenderers.clear();
    m_sourceRenderer.reset();
    m_renderer.reset();
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
    m_previewSize = previewSizeOf(m_format, m_previewFull);
    m_own = m_player && Picture::ownPicture();
    if (m_own) {
        // Own picture engine: no MLT consumer, the pump follows the player's clock
        if (!m_resolved) {
            m_resolved = std::make_shared<Resolved>();
            m_resolved->bypass = m_gradeBypass;
        }
        if (!m_pump)
            m_pump = std::make_unique<Picture::Pump>(
                [this] { return clockFrames(); },
                [this](const QImage& image, int64_t position, double speed) { onPumpShown(image, position, speed); });
        return true;
    }

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
    if (m_pump) m_pump->setContent(nullptr, 0, 0);
    m_timelineContent.reset();
    m_renderer.reset();
    m_sourceRenderer.reset();
    m_gridRenderers.clear();
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
    else if (m_own) applyTimeline(std::exchange(m_built, Timeline{}), false); // the shown timeline in the new format
}

MediaInfo Engine::probe(const QString& path)
{
    return probe(m_format, path);
}

MediaInfo Engine::probe(const ProjectFormat& format, const QString& path)
{
    if (std::optional<MediaInfo> own = MediaProbe::read(path, format.rate)) return *std::move(own);
    return probeMlt(format, path);
}

MediaInfo Engine::probeMlt(const ProjectFormat& format, const QString& path)
{
    MediaInfo info;
    info.path = path;
    info.name = QFileInfo(path).fileName();
    const auto profile = makeProfile(format);
    const int fps = format.rate.timebase();
    Mlt::Producer p(*profile, path.toUtf8().constData());
    if (!p.is_valid()) return info;

    const QString service = p.get("mlt_service");
    info.isImage = Still::isStillService(service.toUtf8().constData());
    if (info.isImage) {
        info.hasVideo = true;
        info.length = 5 * fps; // Still image: 5 seconds as in DaVinci
    } else {
        // Audio files with embedded cover do not count as video
        info.hasVideo = !MediaProbe::isAudioFile(path) && p.get_int("video_index") >= 0;
        info.hasAudio = p.get_int("audio_index") >= 0;
        if (info.hasAudio) info.audioStreams = audioStreamsOf(p);
        info.length = p.get_length();
    }
    return info;
}

QString Engine::startTimecode(const QString& path)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) return {};
    // The container's tag, else the one of the (first) video stream, else of any stream (QuickTime: a timecode
    // track, shown on the stream)
    const auto tag = [](const AVDictionary* d) {
        const AVDictionaryEntry* e = av_dict_get(d, "timecode", nullptr, 0);
        return e ? QString::fromUtf8(e->value).trimmed() : QString();
    };
    QString out = tag(fmt->metadata);
    const int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (out.isEmpty() && video >= 0) out = tag(fmt->streams[video]->metadata);
    for (unsigned i = 0; i < fmt->nb_streams && out.isEmpty(); ++i) out = tag(fmt->streams[i]->metadata);
    avformat_close_input(&fmt);
    return out;
}

double Engine::pictureAspect(const QString& path)
{
    // The shape the picture engine and the timeline fit into the frame: display aspect, upright
    if (const std::optional<MediaInfo> m = MediaProbe::read(path, FrameRate{25, 1}); m && m->isImage) {
        QImageReader reader(path);
        QSize size = reader.size();
        if (reader.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
        return size.width() > 0 && size.height() > 0 ? double(size.width()) / size.height() : 0.0;
    }
    Video::Reader::Options options;
    options.hardware = false;
    const std::unique_ptr<Video::Reader> reader = Video::Reader::open(path, options);
    if (!reader) return 0;
    const Video::Info& i = reader->info();
    double w = i.width, h = i.height;
    if (w <= 0 || h <= 0) return 0;
    w *= i.sar.num > 0 && i.sar.den > 0 ? av_q2d(i.sar) : 1.0;
    const bool sideways = std::lround(std::abs(i.rotation)) % 180 == 90;
    return sideways ? h / w : w / h;
}

QImage Engine::thumbnail(const QString& path, int frame, const QSize& size)
{
    return thumbnail(m_format, path, frame, size);
}

QImage Engine::thumbnail(const ProjectFormat& format, const QString& path, int frame, const QSize& size, bool ownShape)
{
    if (Picture::ownPicture()) {
        // As a timeline shows the file (format's shape), or in its own shape: a format of that shape it fills
        ProjectFormat shape = format;
        QSize fit = format.size().scaled(size, Qt::KeepAspectRatio);
        if (const double aspect = ownShape ? pictureAspect(path) : 0; aspect > 0) {
            shape.width = std::max(2, int(std::lround(aspect * format.height)));
            fit = QSizeF(aspect * size.height(), size.height()).scaled(QSizeF(size), Qt::KeepAspectRatio).toSize();
        }
        Picture::MediaStills stills(shape, &ProxyManager::decodeSource, 1); // proxy if present (4K: 0.2-0.7 s a frame)
        const QImage img = stills.frame(path, frame, QSize(std::max(2, fit.width()), std::max(2, fit.height())));
        if (img.isNull() || img.size() == size) return img;
        QImage out(size, QImage::Format_RGBA8888);
        out.fill(Qt::black);
        QPainter painter(&out);
        painter.drawImage(QPoint((size.width() - img.width()) / 2, (size.height() - img.height()) / 2), img);
        painter.end();
        return out;
    }
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

// A single frame through an own renderer (always from the originals, project size)
QImage Engine::ownStill(const Timeline& tl, int position, bool transparent)
{
    Picture::Options o;
    o.format = m_format;
    o.mode = Picture::Mode::Still;
    o.transparent = transparent;
    Timeline picture = pictureOnly(tl);
    picture.audio.clear();
    Picture::Renderer renderer(picture, o);
    return renderer.render(position).toImage();
}

QImage Engine::grabStill(const Timeline& tl)
{
    if (m_own) {
        Timeline shown = tl;
        int at = m_position;
        if (m_mode == Mode::Source && !m_sourcePath.isEmpty()) {
            shown = Timeline{};
            shown.video.resize(1);
            Clip c;
            c.id = 1;
            c.mediaPath = m_sourcePath;
            c.out = std::max(0, m_sourceLength - 1);
            shown.video[0].clips << c;
        }
        const QImage img = ownStill(shown, at, false);
        return img.isNull() ? QImage() : img.convertToFormat(QImage::Format_RGB32);
    }
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
    if (m_own) return ownStill(tl, position, true);
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
    m_idleRelease->request(); // the old graph's frames are free now (an AI session rebuilds a lot)
}

MemoryRelease::Activity Engine::activity() const
{
    MemoryRelease::Activity a;
    a.playing = m_speed != 0.0;
    a.exporting = Exporter::anyRunning();
    a.caching = m_renderCache->isRendering();
    return a;
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

bool Engine::resolveFiles(const Timeline& tl, bool timeline)
{
    QHash<QString, QString> proxies;
    QHash<int, QString> cache;
    std::function<void(const Timeline&, bool)> walk = [&](const Timeline& t, bool top) {
        for (const Track& track : t.video)
            for (const Clip& c : track.clips) {
                if (!c.mediaPath.isEmpty() && !proxies.contains(c.mediaPath))
                    proxies.insert(c.mediaPath, m_proxies->resolve(c.mediaPath));
                if (top && !c.mediaPath.isEmpty() && !c.isTitle())
                    if (const QString file = m_renderCache->resolve(c); !file.isEmpty()) cache.insert(c.id, file);
            }
    };
    walk(tl, true);
    if (tl.nested)
        for (const Timeline& inner : *tl.nested) walk(inner, false);
    std::lock_guard lock(m_resolved->mutex);
    // Proxies are only added/updated (the source view and the multicam grid read through the same map), the render
    // cache is the timeline's
    bool changed = false;
    for (auto it = proxies.cbegin(); it != proxies.cend(); ++it) {
        QString& file = m_resolved->proxies[it.key()];
        if (file != it.value()) {
            file = it.value();
            changed = true;
        }
    }
    if (timeline && cache != m_resolved->cache) {
        m_resolved->cache = std::move(cache);
        changed = true;
    }
    return changed;
}

Picture::Options Engine::previewOptions(const QSize& output) const
{
    Picture::Options o;
    o.format = m_format;
    o.output = output;
    o.mode = Picture::Mode::Preview;
    const std::shared_ptr<Resolved> resolved = m_resolved;
    o.proxy = [resolved](const QString& path) { return resolved->proxy(path); };
    o.renderCache = [resolved](const Clip& c) { return resolved->cached(c); };
    o.gradeBypass = m_gradeBypass;
    o.parallelFrames = m_pump ? m_pump->workers() : 1;
    return o;
}

std::shared_ptr<const Picture::Pump::Content> Engine::rendererContent(std::shared_ptr<Picture::Renderer> renderer,
                                                                      bool timeline)
{
    auto c = std::make_shared<Picture::Pump::Content>();
    c->render = [renderer](int64_t f) { return frameImage(renderer->render(f)); };
    if (timeline) {
        c->last = [last = m_lastFrame] { return last->load(); };
    } else {
        const int64_t last = std::max(0, m_sourceLength - 1);
        c->last = [last] { return last; };
    }
    c->direction = [renderer](int dir) { renderer->setDirection(dir); };
    c->prepare = [renderer](int64_t f) { renderer->prepare(f); };
    c->trim = [renderer] { renderer->trim(); };
    c->fps = m_format.rate.fps();
    return c;
}

void Engine::applyTimeline(const Timeline& tl, bool hinted)
{
    if (m_own) {
        // Own picture engine: the renderer takes every change as a new snapshot (no rebuild, playback goes on);
        // files open lazily in the render threads, the window never waits for them
        if (m_mode == Mode::Timeline) m_player->setTimeline(tl, m_format.rate);
        m_lastFrame->store(std::max(0, TimelineOps::endFrame(tl) - 1));
        const bool files = resolveFiles(tl, true);
        if (m_renderer && !files && samePicture(m_built, tl)) {
            m_built = tl;
            return;
        }
        if (!m_renderer || !samePicture(m_built, tl)) {
            m_renderCache->sync(tl); // queue missing clip outputs, discard outdated ones
            Stabilizer::instance()->sync(Stabilizer::missing(tl, m_format));
        }
        m_built = tl;
        const QSize output = m_previewSize;
        if (!m_renderer) {
            m_renderer = std::make_shared<Picture::Renderer>(pictureOnly(tl), previewOptions(output));
            m_timelineContent = rendererContent(m_renderer, true);
            if (m_mode == Mode::Timeline) showContent(m_timelineContent, m_position);
            return;
        }
        m_renderer->update(pictureOnly(tl));
        if (m_mode == Mode::Timeline) m_pump->refresh();
        return;
    }
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
    if (m_own) {
        if (!m_timelineContent) return;
        if (!keepSpeed) m_speed = 0;
        m_mode = Mode::Timeline;
        m_offset = 0;
        showContent(m_timelineContent, position);
        syncPlayer(position, true);
        if (!m_gridRenderers.empty()) {
            m_gridRenderers.clear(); // not shown any more: free their decoders
            releaseFreedMemory();
        }
        m_sourceRenderer.reset();
        emit modeChanged(m_mode);
        return;
    }
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
    if (!m_own && (!m_consumer || !m_gridBuilder)) return false;
    if (m_mode == Mode::Multicam && (m_own ? !m_gridRenderers.empty() : m_grid != nullptr) && content == m_gridContent && angles == m_gridAngles
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
    if (m_own) {
        // Each angle's picture timeline rendered at its cell's size, tiled into the preview frame (cells have the
        // frame's aspect ratio, an angle's own bars come along)
        if (angles.isEmpty()) return false;
        const int n = angles.size();
        const int per = MulticamGrid::perPage(n, options.columns);
        const int page = std::clamp(options.page, 0, MulticamGrid::pages(n, options.columns) - 1);
        const QRect frame(QPoint(0, 0), m_previewSize);
        struct Cell {
            std::shared_ptr<Picture::Renderer> renderer;
            QRect rect;
        };
        auto layout = std::make_shared<std::vector<Cell>>();
        std::vector<std::shared_ptr<Picture::Renderer>> renderers;
        for (int i = 0; i < per; ++i) {
            const int angle = page * per + i;
            if (angle >= n) break;
            const Timeline t = Multicam::angleTimeline(content, angles[angle], TrackKind::Video);
            if (t.video.isEmpty()) continue; // sound-only angle: its cell stays black
            const QRect cell = MulticamGrid::cell(frame, n, i, options.columns);
            Picture::Options o = previewOptions(cell.size());
            o.decoderThreads = cells <= 1 ? 0 : std::max(1, QThread::idealThreadCount() / cells);
            auto r = std::make_shared<Picture::Renderer>(t, o);
            layout->push_back({r, cell});
            renderers.push_back(std::move(r));
        }
        resolveFiles(content, false);
        auto c = std::make_shared<Picture::Pump::Content>();
        const QSize size = m_previewSize;
        c->render = [layout, size](int64_t f) {
            QImage out(size, QImage::Format_RGBA8888);
            out.fill(Qt::black);
            for (const Cell& cell : *layout) {
                const Picture::Frame fr = cell.renderer->render(f);
                if (fr.isNull()) continue;
                const int w = std::min(fr.width(), cell.rect.width()), h = std::min(fr.height(), cell.rect.height());
                for (int y = 0; y < h; ++y)
                    std::memcpy(out.scanLine(cell.rect.y() + y) + size_t(cell.rect.x()) * 4,
                                fr.bits() + size_t(y) * fr.stride(), size_t(w) * 4);
            }
            return out;
        };
        c->last = [] { return int64_t(-1); }; // no end of its own: the window moves on to the next segment
        c->direction = [layout](int dir) {
            for (const Cell& cell : *layout) cell.renderer->setDirection(dir);
        };
        c->prepare = [layout](int64_t f) {
            for (const Cell& cell : *layout) cell.renderer->prepare(f);
        };
        c->trim = [layout] {
            for (const Cell& cell : *layout) cell.renderer->trim();
        };
        c->fps = m_format.rate.fps();
        // Building took a while and the old grid played on meanwhile: go on from there, not from `position`
        if (m_speed != 0.0 && m_mode == Mode::Multicam) position = m_position;
        m_gridRenderers = std::move(renderers);
        m_sourceRenderer.reset();
        m_gridContent = content;
        m_gridAngles = angles;
        m_gridColumns = options.columns;
        m_gridPage = options.page;
        m_offset = offset;
        const bool changed = m_mode != Mode::Multicam;
        m_mode = Mode::Multicam;
        m_gridSoundAngle = options.activeAngle;
        showContent(c, position);
        releaseFreedMemory(); // the old grid's decoders and frames
        syncPlayer(position, true);
        if (changed) emit modeChanged(m_mode);
        return true;
    }
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
    if (m_own) {
        const MediaInfo info = probe(m_format, path);
        if (!info.hasVideo && !info.hasAudio) return;
        m_sourceLength = std::max(1, info.length);
        Timeline picture;
        picture.video.resize(1);
        Clip c;
        c.id = 1;
        c.mediaPath = path;
        c.out = m_sourceLength - 1;
        if (info.hasVideo) picture.video[0].clips << c;
        resolveFiles(picture, false);
        const QSize output = m_previewSize;
        m_sourceRenderer = std::make_shared<Picture::Renderer>(picture, previewOptions(output));
        m_sourcePath = path;
        m_speed = 0;
        m_mode = Mode::Source;
        m_offset = 0;
        m_sourceSound = Timeline{}; // the sound of the original file, stream 0
        m_sourceSound.audio.resize(1);
        m_sourceSound.audio[0].kind = TrackKind::Audio;
        m_sourceSound.audio[0].clips << c;
        showContent(rendererContent(m_sourceRenderer, false), std::max(0, position));
        syncPlayer(std::max(0, position), true);
        if (!m_gridRenderers.empty()) {
            m_gridRenderers.clear();
            releaseFreedMemory();
        }
        emit modeChanged(m_mode);
        return;
    }
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

bool Engine::connected() const
{
    if (!m_own) return m_current != nullptr;
    switch (m_mode) {
    case Mode::Timeline: return m_timelineContent != nullptr;
    case Mode::Source: return m_sourceRenderer != nullptr;
    case Mode::Multicam: return true;
    }
    return false;
}

void Engine::showContent(std::shared_ptr<const Picture::Pump::Content> content, int position)
{
    ++m_seekEpoch;
    m_seekTarget = -1;
    m_pump->setContent(std::move(content), std::max(0, position - m_offset), m_speed);
    m_position = position;
    emit positionChanged(position);
    emit speedChanged(m_speed);
}

void Engine::onPumpShown(const QImage& image, int64_t position, double speed)
{
    if (m_player && speed != 0.0) m_player->release(); // the picture is there: the sound may start
    if (!image.isNull()) emit frameReady(image);       // queued -> UI thread
    const int shown = int(position) + m_offset;        // timeline frame
    if (speed != 0.0 && shown != m_position.exchange(shown)) emit framePosition(shown, m_seekEpoch);
    if (speed != 0.0 && m_mode == Mode::Timeline) emitLevels();
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

void Engine::setPreviewFullSize(bool full)
{
    if (full == m_previewFull) return;
    m_previewFull = full;
    m_previewSize = previewSizeOf(m_format, full);
    if (m_renderer) m_renderer->setOutputSize(m_previewSize);
    if (m_sourceRenderer) m_sourceRenderer->setOutputSize(m_previewSize);
    if (m_consumer && !qEnvironmentVariableIsSet("SCHNEIDI_FULLRES_PREVIEW")) {
        m_consumer->set("width", m_previewSize.width());
        m_consumer->set("height", m_previewSize.height());
    }
    refresh(); // the frame shown again at the new size
}

void Engine::refresh()
{
    if (m_own) {
        if (m_pump) m_pump->refresh();
        return;
    }
    if (m_consumer) m_consumer->set("refresh", 1);
}

void Engine::play() { setSpeed(1.0); }
void Engine::pause() { setSpeed(0.0); }
void Engine::togglePlay() { setSpeed(m_speed == 0.0 ? 1.0 : 0.0); }

int Engine::lastFrame() const
{
    switch (m_mode) {
    case Mode::Timeline: return TimelineOps::endFrame(m_built) - 1;
    case Mode::Source:
        if (m_own) return m_sourceRenderer ? m_sourceLength - 1 : -1;
        return m_source ? m_source->get_length() - 1 : -1;
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
    if (m_speed != 0.0 && connected() && atEnd(m_speed)) pause();
}

void Engine::setSpeed(double speed)
{
    if (!connected()) return;
    if (speed != 0.0 && m_speed == 0.0 && atEnd(speed)) {
        // Play at the end starts from the front (Premiere, Final Cut); backwards at the start there is nothing
        if (speed < 0 || lastFrame() <= 0) return;
        seek(0);
    }
    const bool wasPlaying = m_speed != 0.0;
    const double before = m_speed;
    m_speed = speed;
    if (m_own) {
        // The pump goes on from the shown frame first (nothing from before is shown after this), then the sound
        const int at = std::max(0, m_position - m_offset);
        if (speed == 0.0) {
            m_player->pause();
            m_pump->setSpeed(0, at); // stay exactly on the shown frame
        } else if (wasPlaying && (before > 0) == (speed > 0)) {
            m_pump->setSpeed(speed, at);
            m_player->play(m_player->position(), speed); // faster/slower from where the sound is
        } else {
            if (wasPlaying) ++m_seekEpoch; // turning around: positions on their way lie the other way
            m_pump->setSpeed(speed, at);
            m_player->play(soundStart(at, speed), speed);
        }
        applyAudioState();
        emit speedChanged(speed);
        return;
    }
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
    if (!connected()) return;
    frame = std::max(0, frame);
    const int at = std::max(0, frame - m_offset); // frame of the producer (multicam grid: of the sequence)
    const bool playing = m_speed != 0.0;
    if (m_own) {
        ++m_seekEpoch;
        m_pump->seek(at); // first: no frame from before the jump is shown after this
        if (playing) {
            // The next seek within moments: the playhead is being dragged -> silent until it rests
            if (m_lastSeek.isValid() && m_lastSeek.elapsed() < 150) {
                m_player->setMuted(true);
                m_seekRelease->start();
            }
            m_player->play(soundStart(at, m_speed), m_speed);
        }
        m_lastSeek.start();
        m_position = frame;
        emit positionChanged(frame);
        return;
    }
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
