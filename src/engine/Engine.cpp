// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Engine.h"

#include "core/CodeTrust.h"
#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/MediaAspect.h"
#include "core/Multicam.h"
#include "core/TimelineOps.h"
#include "engine/Background.h"
#include "engine/Bundle.h"
#include "engine/Exporter.h"
#include "engine/FileStamp.h"
#include "engine/MediaProbe.h"
#include "engine/OpenMedia.h"
#include "engine/MemoryRelease.h"
#include "engine/ProxyManager.h"
#include "engine/RenderCache.h"
#include "engine/Stabilizer.h"
#include "engine/StillFetcher.h"
#include "engine/audio/Player.h"
#include "engine/picture/MediaStills.h"
#include "engine/picture/Renderer.h"
#include "engine/video/Reader.h"
#include "ui/MulticamGrid.h"

#include <QCoreApplication>
#include <QGuiApplication>
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

// Seeks this close together (ms) while stopped are scrubbing (Engine::seek)
constexpr qint64 kScrubMs = 150;

// Hands freed memory back to the system right away. glibc raises its mmap threshold after the first freed frame buffer
// (several MB each), from then on frames come from the heap and stay there when freed: every multicam grid built (one
// decoder per cell) left hundreds of MB behind (4 GB after flipping the pages of 20 angles a few times).
void releaseFreedMemory()
{
    MemoryRelease::trimHeap();
}

constexpr int kSoundRate = 48000; // the own audio engine's rate (the device converts if it runs at another)

// What the renderer gets – no sound tracks, no master (the player has them)
Timeline pictureOnly(const Timeline& tl)
{
    Timeline p = tl;
    p.audio.clear();
    p.masterVolumeDb = 0;
    p.masterLimiter = false;
    return p;
}

// Same picture: only the sound differs (the length counts: the picture lasts as long as the sound)
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

// Preview size: the project format scaled down to a longer edge of 960 (even; larger when `edge` asks for it, e.g. the
// full-screen viewer's screen, never beyond the project), the full size when the viewer zooms to 100 % or more (full)
// or with SCHNEIDI_FULLRES_PREVIEW
QSize previewSizeOf(const ProjectFormat& format, bool full, int edge)
{
    if (full || qEnvironmentVariableIsSet("SCHNEIDI_FULLRES_PREVIEW")) return format.size();
    const QSize project = format.size();
    if (edge >= std::max(project.width(), project.height())) return project;
    const int longer = std::max(960, edge);
    const QSize fit = project.scaled(longer, longer, Qt::KeepAspectRatio);
    return QSize(std::max(2, fit.width() & ~1), std::max(2, fit.height() & ~1));
}

} // namespace

// Proxy and render-cache files the renderers read (their worker threads ask; ProxyManager and
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

Engine::Engine(QObject* parent)
    : QObject(parent), m_proxies(new ProxyManager(this)), m_renderCache(new RenderCache(this)),
      m_stills(new StillFetcher(this)),
      m_idleRelease(new IdleRelease([this] { return activity(); }, this)),
      m_loudness(std::make_unique<SharedLoudness>())
{
    // Render cache pauses during playback
    connect(this, &Engine::speedChanged, m_renderCache, [this](double s) { m_renderCache->setPlaying(s != 0.0); });
    // The picture pools and the heap keep what playback, rebuilds and rendered clips used: given back once all is idle
    connect(this, &Engine::speedChanged, m_idleRelease, [this](double s) {
        if (s == 0.0) m_idleRelease->request();
    });
    connect(m_renderCache, &RenderCache::cacheChanged, m_idleRelease, &IdleRelease::request);
    // A stabilization analysis finished: its correction is read while drawing, but the still shown (and the frames
    // kept for stepping back) were drawn without it, and the render cache left such clips out until it was there
    connect(Stabilizer::instance(), &Stabilizer::resultReady, this, [this] {
        if (!m_renderer) return;
        m_renderCache->sync(m_built);
        // Paused: the still again. Playing: newly drawn frames take the correction (a refresh drops the frames
        // prepared ahead - a hitch for each analysis that finishes)
        if (m_mode == Mode::Timeline && m_speed == 0.0) refresh();
    });
    // ... with the recently shown frames (up to 192 MB): stepping back after a pause renders again
    connect(m_idleRelease, &IdleRelease::releasing, this, [this] {
        if (m_pump) m_pump->dropRecent();
    });
    m_seekRelease = new QTimer(this);
    m_seekRelease->setSingleShot(true);
    m_seekRelease->setInterval(120);
    connect(m_seekRelease, &QTimer::timeout, this, [this] { m_player->setMuted(false); });
    Audio::Player::Options o;
    o.rate = kSoundRate;
    o.buffer = audioBuffer();
    o.backend = audioDriver();
    o.output = audioOutput();
    m_player = std::make_unique<Audio::Player>(o);
    m_player->setLoudness(m_loudness.get());
    // Messages that were already on their way when jumping made the playhead briefly jump back
    connect(this, &Engine::framePosition, this, [this](int frame, int epoch) {
        if (epoch != m_seekEpoch) return;
        emit positionChanged(frame);
        stopAtEnd();
    }, Qt::QueuedConnection);

    // A media file overwritten under its name. Seen by a renderer (any thread), which asks for the file's proxy right
    // after: the old content's proxy must not be read again (a proxy of the new content cannot exist yet). The rest -
    // render cache, frames kept for stepping back, the still shown - on the UI thread.
    m_resolved = std::make_shared<Resolved>(); // made once here: the observer keeps it
    m_resolved->bypass = m_gradeBypass;
    m_stampObserver = FileStamp::addObserver([this, resolved = m_resolved](const QString& path) {
        {
            std::lock_guard lock(resolved->mutex);
            if (auto it = resolved->proxies.find(path); it != resolved->proxies.end()) *it = path;
        }
        QMetaObject::invokeMethod(this, [this] { mediaFileChanged(); }, Qt::QueuedConnection);
    });
    // Back in the app (a file re-rendered in another program meanwhile): look at the files at once
    if (auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        connect(gui, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) checkFiles();
        });
}

void Engine::checkFiles()
{
    QSet<QString> files;
    const auto collect = [&files](const Timeline& t) {
        for (const auto* tracks : {&t.video, &t.audio})
            for (const Track& track : *tracks)
                for (const Clip& c : track.clips)
                    if (!c.mediaPath.isEmpty()) files.insert(c.mediaPath);
    };
    collect(m_built);
    if (m_built.nested)
        for (const Timeline& inner : *m_built.nested) collect(inner);
    bool changing = false;
    for (const QString& f : std::as_const(files)) {
        FileStamp::now(f); // a settled change calls the observers
        changing |= FileStamp::changing(f);
    }
    // A file still being written (or just overwritten): look again once it may have settled
    if (changing && !m_recheckQueued) {
        m_recheckQueued = true;
        QTimer::singleShot(FileStamp::kSettleMs + 200, this, [this] {
            m_recheckQueued = false;
            checkFiles();
        });
    }
}

void Engine::mediaFileChanged()
{
    // Coalesced: several files (or several renderers seeing one) in one go
    if (m_fileChangeQueued) return;
    m_fileChangeQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_fileChangeQueued = false;
        resolveFiles(m_built, true);  // proxies and render cache files of the new content
        m_renderCache->sync(m_built); // clip outputs of the old content go, new ones are queued
        if (m_pump) m_pump->dropRecent();
        m_player->reopenFiles(); // the sound mix keeps its readers otherwise
        refresh();
    });
}

Engine::~Engine()
{
    FileStamp::removeObserver(m_stampObserver);
    m_pump.reset(); // its threads use the renderers
    m_timelineContent.reset();
    m_gridRenderers.clear();
    m_sourceRenderer.reset();
    m_renderer.reset();
    m_player.reset(); // feeds m_loudness
}

void Engine::initHeadless()
{
    MediaAspect::setProbe(&Engine::pictureAspect);
    // Qt refuses to decode a picture above 256 MB: a 108-megapixel TIFF (12000x9000, 432 MB as RGBA) was imported and
    // rendered, but `frames` of the file and the viewer at its own size "could not decode" it. What the probe accepts
    // (below FFmpeg's limit for one image, MediaProbe) fits 1 GB at 4 bytes per pixel. QT_IMAGEIO_MAXALLOC can set more.
    if (const int mb = QImageReader::allocationLimit(); mb > 0 && mb < 1024) QImageReader::setAllocationLimit(1024);
}

bool Engine::init(QString*)
{
    initHeadless();
    m_format = ProjectFormat{};
    m_formatSet = true;
    createPump();
    updateTimeline(Timeline{});
    showTimeline(0);
    return true;
}

void Engine::createPump()
{
    // Preview image in the project's aspect ratio (otherwise portrait gets distorted), even dimensions
    m_previewSize = previewSizeOf(m_format, m_previewFull, m_previewEdge);
    if (!m_resolved) {
        m_resolved = std::make_shared<Resolved>();
        m_resolved->bypass = m_gradeBypass;
    }
    if (!m_pump)
        m_pump = std::make_unique<Picture::Pump>(
            [this] { return clockFrames(); },
            [this](const QImage& image, int64_t position, double speed) { onPumpShown(image, position, speed); });
}

void Engine::setFormat(const ProjectFormat& format)
{
    if (format == m_format && m_formatSet) return;
    m_player->pause();
    if (m_pump) m_pump->setContent(nullptr, 0, 0);
    m_timelineContent.reset();
    m_renderer.reset();
    m_sourceRenderer.reset();
    m_gridRenderers.clear();
    m_gridContent = {};
    m_gridAngles.clear();
    m_offset = 0;

    m_format = format;
    m_renderCache->setFormat(format);
    m_stills->setFormat(format);
    m_formatSet = true;
    m_speed = 0;
    createPump();
    if (m_mode != Mode::Timeline) {
        m_mode = Mode::Timeline; // the source view was in the old format
        emit modeChanged(m_mode);
    }
    emit speedChanged(m_speed);
    applyTimeline(std::exchange(m_built, Timeline{})); // the shown timeline in the new format
}

MediaInfo Engine::probe(const QString& path)
{
    return probe(m_format, path);
}

MediaInfo Engine::probe(const ProjectFormat& format, const QString& path)
{
    if (std::optional<MediaInfo> own = MediaProbe::read(path, format.rate)) return *std::move(own);
    MediaInfo info; // unreadable: no picture, no sound
    info.path = path;
    info.name = QFileInfo(path).fileName();
    return info;
}

QString Engine::startTimecode(const QString& path)
{
    AVFormatContext* fmt = nullptr;
    if (OpenMedia::input(&fmt, path.toUtf8().constData()) < 0) return {};
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

QImage Engine::thumbnail(const ProjectFormat& format, const QString& path, int frame, const QSize& size, bool ownShape,
                         double aspect)
{
    // As a timeline shows the file (format's shape), or in its own shape: a format of that shape it fills
    ProjectFormat shape = format;
    QSize fit = format.size().scaled(size, Qt::KeepAspectRatio);
    if (const double own = !ownShape ? 0 : aspect > 0 ? aspect : pictureAspect(path); own > 0) {
        shape.width = std::max(2, int(std::lround(own * format.height)));
        fit = QSizeF(own * size.height(), size.height()).scaled(QSizeF(size), Qt::KeepAspectRatio).toSize();
    }
    Picture::MediaStills stills(shape, &ProxyManager::decodeSource, 1); // proxy if present (4K: 0.2-0.7 s a frame)
    const QImage img = stills.frame(path, frame, QSize(std::max(2, fit.width()), std::max(2, fit.height())), aspect);
    if (img.isNull() || img.size() == size) return img;
    QImage out(size, QImage::Format_RGBA8888);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.drawImage(QPoint((size.width() - img.width()) / 2, (size.height() - img.height()) / 2), img);
    painter.end();
    return out;
}

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
    Timeline shown = tl;
    int at = m_position;
    if (m_mode == Mode::Source && !m_sourcePath.isEmpty()) {
        shown = Timeline{};
        shown.video.resize(1);
        Clip c;
        c.id = 1;
        c.mediaPath = m_sourcePath;
        c.pictureAspect = m_sourceAspect;
        c.out = std::max(0, m_sourceLength - 1);
        shown.video[0].clips << c;
    }
    const QImage img = ownStill(shown, at, false);
    return img.isNull() ? QImage() : img.convertToFormat(QImage::Format_RGB32);
}

QImage Engine::grabFrame(const Timeline& tl, int position)
{
    return ownStill(tl, position, true);
}

void Engine::updateTimeline(const Timeline& tl)
{
    m_mixerOnlyNext = false; // the player takes mixer changes live like every other change
    applyTimeline(tl);
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
    c->renderWant = [renderer](int64_t f, Picture::Pump::Want& want) {
        Picture::RenderHints hints;
        hints.nearest = want.nearest;
        hints.cancel = want.cancel;
        const Picture::Frame frame = renderer->render(f, hints);
        want.exact = frame.exact();
        return frameImage(frame);
    };
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

void Engine::applyTimeline(const Timeline& tl)
{
    // Own picture engine: the renderer takes every change as a new snapshot (no rebuild, playback goes on);
    // files open lazily in the render threads, the window never waits for them
    if (m_mode == Mode::Timeline) m_player->setTimeline(tl, m_format.rate);
    m_lastFrame->store(m_openEnd ? -1 : std::max(0, TimelineOps::endFrame(tl) - 1)); // -1: the pump has no end
    const bool files = resolveFiles(tl, true);
    // Code trusted meanwhile (core/CodeTrust.h): the same timeline draws another picture
    const quint64 trust = CodeTrust::epoch();
    const bool same = std::exchange(m_builtTrust, trust) == trust && samePicture(m_built, tl);
    if (m_renderer && !files && same) {
        m_built = tl;
        return;
    }
    if (!m_renderer || !same) {
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
}

void Engine::setMediaLengths(const QHash<QString, int>& lengths)
{
    m_player->setFileLengths(lengths);
}

void Engine::showTimeline(int position, bool keepSpeed)
{
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
}

bool Engine::showMulticam(const Timeline& content, const QVector<MulticamAngle>& angles,
                          const MulticamGridOptions& options, int offset, int position)
{
    if (m_mode == Mode::Multicam && !m_gridRenderers.empty() && content == m_gridContent && angles == m_gridAngles
        && options.columns == m_gridColumns && options.page == m_gridPage) {
        setMulticamAudio(options.activeAngle);
        if (offset != m_offset) { // another segment of the same multicam clip, slipped: same timeline frame
            m_offset = offset;
            const int at = m_position;
            seek(at);
        }
        return true;
    }
    // One decoder per cell: their own frame threads only cost memory (each holds frames of its own) once the cells
    // keep all cores busy; a single cell decodes with all of them
    const int cells = std::min<int>(angles.size(), MulticamGrid::perPage(angles.size(), options.columns));
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

void Engine::setMulticamAudio(int angle)
{
    if (m_mode == Mode::Multicam && angle != m_gridSoundAngle) {
        m_gridSoundAngle = angle;
        syncPlayer(m_position, false); // crossfades to the other angle's sound
    }
}

void Engine::showSource(const QString& path, int position, double aspect)
{
    const MediaInfo info = probe(m_format, path);
    if (!info.hasVideo && !info.hasAudio) return;
    m_sourceLength = std::max(1, info.length);
    Timeline picture;
    picture.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = path;
    c.pictureAspect = std::max(0.0, aspect);
    c.out = m_sourceLength - 1;
    if (info.hasVideo) picture.video[0].clips << c;
    resolveFiles(picture, false);
    const QSize output = m_previewSize;
    m_sourceRenderer = std::make_shared<Picture::Renderer>(picture, previewOptions(output));
    m_sourcePath = path;
    m_sourceAspect = c.pictureAspect;
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
}

bool Engine::connected() const
{
    switch (m_mode) {
    case Mode::Timeline: return m_timelineContent != nullptr;
    case Mode::Source: return m_sourceRenderer != nullptr;
    case Mode::Multicam: return true;
    }
    return false;
}

void Engine::showContent(std::shared_ptr<const Picture::Pump::Content> content, int position)
{
    m_pump->setContent(std::move(content), std::max(0, position - m_offset), m_speed);
    ++m_seekEpoch; // after the pump: a frame being shown during the call carried the new epoch (playhead jumped back)
    m_position = position;
    emit positionChanged(position);
    emit speedChanged(m_speed);
}

void Engine::onPumpShown(const QImage& image, int64_t position, double speed)
{
    if (speed != 0.0) m_player->release(); // the picture is there: the sound may start
    if (!image.isNull()) emit frameReady(image);       // queued -> UI thread
    const int shown = int(position) + m_offset;        // timeline frame
    if (speed != 0.0 && shown != m_position.exchange(shown)) emit framePosition(shown, m_seekEpoch);
    if (speed != 0.0 && m_mode == Mode::Timeline) emitLevels();
}

void Engine::applyAudioState()
{
    // Loudness only during normal playback of the timeline (shuttling/reverse/source distort the measurement)
    m_loudness->active = m_speed == 1.0 && m_mode == Mode::Timeline;
}

double Engine::clockFrames() const
{
    if (!m_player->playing()) return NAN; // a frame still on its way from before the stop: at once
    return m_player->position() * m_format.rate.fps() / kSoundRate;
}

double Engine::soundFrame() const
{
    return clockFrames() + m_offset; // NaN stays NaN
}

double Engine::soundStart(int frame, double speed) const
{
    // Forward the sound of a frame starts at its first sample, backward after its last
    return double(speed < 0 ? frame + 1 : frame) * kSoundRate / m_format.rate.fps();
}

// The player plays what the current mode shows (positions = content frames); restart: playing goes on at
// `position` (other content: another time axis)
void Engine::syncPlayer(int position, bool restart)
{
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
    m_player->setDevice(audioBuffer(), audioDriver(), audioOutput());
    qInfo("Audio-Puffer: %d Samples", audioBuffer());
}

QString Engine::audioDriver()
{
#ifdef Q_OS_WIN
    // WASAPI crackled at every frame boundary and distorted the level (7.1 headset, 44.1 kHz, SDL 2.32 in 0.9);
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
    m_player->setDevice(audioBuffer(), audioDriver(), audioOutput());
    qInfo("Audio-Ausgabe: %s", id.isEmpty() ? "Systemstandard" : qPrintable(id));
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
    refresh(); // the renderers read the switch at the next frame
    emit colorBypassChanged(on);
}

void Engine::setPreviewFullSize(bool full)
{
    if (full == m_previewFull) return;
    m_previewFull = full;
    applyPreviewSize();
}

void Engine::setPreviewEdge(int px)
{
    px = std::max(0, px);
    if (px == m_previewEdge) return;
    m_previewEdge = px;
    applyPreviewSize();
}

void Engine::applyPreviewSize()
{
    const QSize size = previewSizeOf(m_format, m_previewFull, m_previewEdge);
    if (size == m_previewSize) return;
    m_previewSize = size;
    if (m_renderer) m_renderer->setOutputSize(m_previewSize);
    if (m_sourceRenderer) m_sourceRenderer->setOutputSize(m_previewSize);
    refresh(); // the frame shown again at the new size
}

void Engine::refresh()
{
    if (m_pump) m_pump->refresh();
}

void Engine::play() { setSpeed(1.0); }
void Engine::pause() { setSpeed(0.0); }
void Engine::togglePlay() { setSpeed(m_speed == 0.0 ? 1.0 : 0.0); }

int Engine::lastFrame() const
{
    switch (m_mode) {
    case Mode::Timeline: return m_openEnd ? INT_MAX : TimelineOps::endFrame(m_built) - 1;
    case Mode::Source: return m_sourceRenderer ? m_sourceLength - 1 : -1;
    case Mode::Multicam: break;
    }
    return INT_MAX;
}

void Engine::setOpenEnd(bool on)
{
    if (on == m_openEnd) return;
    m_openEnd = on;
    m_lastFrame->store(on ? -1 : std::max(0, TimelineOps::endFrame(m_built) - 1));
    stopAtEnd(); // closed again while playing behind the end
}

bool Engine::atEnd(double speed) const
{
    if (m_mode == Mode::Multicam) return false;
    return speed > 0 ? m_position >= lastFrame() : m_position <= 0;
}

// Playback stops by itself at the end, but the engine kept its speed: nothing moved, the button still showed
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
    // The pump goes on from the shown frame first (nothing from before is shown after this), then the sound
    const int at = std::max(0, m_position - m_offset);
    if (speed == 0.0) {
        m_pump->setSpeed(0, at); // stay exactly on the shown frame; no frame of the playback is shown after this
        m_player->pause();
        // A frame shown between reading `at` and the pump stopping moved the position one on: back to the still's
        const int still = at + m_offset;
        if (m_position.exchange(still) != still) {
            ++m_seekEpoch;
            emit positionChanged(still);
        }
    } else if (wasPlaying && (before > 0) == (speed > 0)) {
        m_pump->setSpeed(speed, at);
        m_player->play(m_player->position(), speed); // faster/slower from where the sound is
    } else {
        m_pump->setSpeed(speed, at);
        if (wasPlaying) ++m_seekEpoch; // turning around: positions on their way lie the other way (after the pump)
        m_player->play(soundStart(at, speed), speed);
    }
    applyAudioState();
    emit speedChanged(speed);
}

void Engine::seek(int frame)
{
    if (!connected()) return;
    frame = std::max(0, frame);
    const int at = std::max(0, frame - m_offset); // frame of the producer (multicam grid: of the sequence)
    const bool playing = m_speed != 0.0;
    // Stopped and the last seek moments ago: scrubbing (dragging, a held arrow key)
    const bool scrub = !playing && m_lastSeek.isValid() && m_lastSeek.elapsed() < kScrubMs;
    m_pump->seek(at, scrub); // first: no frame from before the jump is shown after this
    ++m_seekEpoch; // then: a frame shown while the pump was being told carries the old epoch and is dropped
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
    stopAtEnd(); // a click behind the end while playing: nothing to show there, stop instead of playing over nothing
}

Picture::Pump::Stats Engine::pictureStats() const
{
    return m_pump ? m_pump->stats() : Picture::Pump::Stats{};
}

void Engine::emitLevels()
{
    // Peaks of what was heard since the last frame
    const std::vector<float> peaks = m_player->takeLevels();
    if (peaks.empty()) return;
    QVector<float> levels;
    levels.reserve(int(peaks.size()));
    for (const float p : peaks) levels << (p > 1e-10f ? 20.f * std::log10(p) : -200.f);
    emit audioLevels(levels); // queued -> UI thread
}

