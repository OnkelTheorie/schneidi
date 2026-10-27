#include "engine/GradedThumbs.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "engine/Background.h"
#include "engine/Engine.h"

#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <atomic>

namespace {

constexpr size_t kMaxJobs = 256;        // older requests are dropped (scrolled past), asked for again when visible
constexpr int kDoneCacheKB = 64 * 1024; // ~1100 graded 160x90 tiles
constexpr int kShownCacheKB = 32 * 1024;
constexpr int kLooksCacheKB = 8 * 1024; // ~140 clips
constexpr int kMaxMemos = 20000;

std::atomic<bool> g_enabled{true};

int costKB(const QImage& img) { return int(img.sizeInBytes() / 1024) + 1; }

// FNV-1a over the values that change the picture
struct Hasher {
    quint64 h = 1469598103934665603ULL;
    void bytes(const void* data, size_t n)
    {
        const auto* b = static_cast<const uchar*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ULL;
        }
    }
    void num(double v)
    {
        if (v == 0) v = 0; // -0 and 0 alike
        bytes(&v, sizeof v);
    }
    void num(int v) { bytes(&v, sizeof v); }
    void params(const ColorGrade::Params& p)
    {
        for (int i = 0; i < 4; ++i) {
            num(p.lift[i]);
            num(p.gamma[i]);
            num(p.gain[i]);
            num(p.offset[i]);
        }
        for (double v : {p.contrast, p.pivot, p.saturation, p.temperature, p.tint, p.exposure}) num(v);
    }
};

} // namespace

// ---------- GradedThumbs ----------

GradedThumbs::Spec GradedThumbs::specAt(const Clip& c, double t)
{
    Spec s;
    const EffectInstance* e = EffectRegistry::instance(c, ColorGrade::EffectId);
    if (!e || !e->enabled) return s;
    s.params = ColorGrade::at(c, t);
    s.lut = ColorGrade::lutPath(c);
    s.windows = ColorGrade::windowsAt(c, t);
    if (s.params.isNeutral() && s.lut.isEmpty() && s.windows.isEmpty()) return s;
    Hasher h;
    h.params(s.params);
    h.bytes(s.lut.constData(), size_t(s.lut.size()) * sizeof(QChar));
    for (const ColorGrade::Window& w : s.windows) {
        const GradeWindows::Geometry& g = w.geometry;
        h.num(int(g.shape));
        for (double v : {g.x, g.y, g.width, g.height, g.rotation, g.softness}) h.num(v);
        h.num(int(g.invert));
        h.params(w.params);
        h.num(int(w.fill.fill));
        h.num(w.fill.amount);
        h.num(int(w.fill.color.rgba()));
    }
    s.hash = h.h ? h.h : 1;
    return s;
}

bool GradedThumbs::animated(const Clip& c)
{
    for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it) {
        if (it->isEmpty()) continue;
        if (ColorGrade::animParams().contains(it.key())) return true;
        // Windows: dynamic parameters of the grade effect ("w1.x", "w1.liftY" …)
        QString effect;
        if (Keys::effectParamOf(it.key(), &effect, nullptr) && effect == QLatin1String(ColorGrade::EffectId))
            return true;
    }
    return false;
}

QImage GradedThumbs::apply(const QImage& image, const Spec& spec)
{
    // RGBA8888: bytes in the order ColorGrade works on, rows without gaps (4 bytes per pixel)
    QImage img = image.convertToFormat(QImage::Format_RGBA8888);
    if (img.isNull() || spec.isNull()) return img.convertToFormat(QImage::Format_RGB32);
    std::shared_ptr<const ColorGrade::Lut> lut;
    if (!spec.lut.isEmpty()) lut = ColorGrade::loadLut(spec.lut); // missing/broken: without LUT, like the preview
    uint8_t* bits = img.bits();
    if (!spec.params.isNeutral() || lut) ColorGrade::apply(bits, img.width(), img.height(), spec.params, lut.get());
    for (const ColorGrade::Window& w : spec.windows) ColorGrade::applyWindow(bits, img.width(), img.height(), w);
    return img.convertToFormat(QImage::Format_RGB32); // faster to draw
}

void GradedThumbs::setEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool GradedThumbs::enabled() { return g_enabled.load(std::memory_order_relaxed); }

GradedThumbs::GradedThumbs(QObject* parent) : QObject(parent)
{
    m_clock.start();
    m_done.setMaxCost(kDoneCacheKB);
    m_shown.setMaxCost(kShownCacheKB);
    m_looks.setMaxCost(kLooksCacheKB);
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    m_settle->setTimerType(Qt::PreciseTimer); // a coarse timer may fire early: the redraw would still find it moving
    connect(m_settle, &QTimer::timeout, this, &GradedThumbs::updated);
    m_thread = QThread::create([this] {
        Background::lowerThisThread(Background::Level::Low);
        loop();
    });
    m_thread->start(QThread::LowPriority);
}

GradedThumbs::~GradedThumbs()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
    }
    m_cond.wakeAll();
    m_thread->wait();
    delete m_thread;
}

GradedThumbs::ClipGrade GradedThumbs::forClip(const Clip& c)
{
    ClipGrade g;
    if (!enabled()) return g;
    // Clips without a grade: one short look at their effects, nothing else
    const auto e = std::find_if(c.effects.cbegin(), c.effects.cend(), [](const EffectInstance& i) {
        return i.effectId == QLatin1String(ColorGrade::EffectId);
    });
    if (e == c.effects.cend() || !e->enabled) return g;
    // Unchanged clips compare in no time: their effects and keyframes still share the data with the memo
    auto it = m_memos.find(c.id);
    if (it == m_memos.end() || it->effects != c.effects || it->keys != c.keys || it->mediaPath != c.mediaPath) {
        Memo m;
        m.mediaPath = c.mediaPath;
        m.effects = c.effects;
        m.keys = c.keys;
        m.animated = animated(c);
        if (!m.animated) m.spec = specAt(c, 0);
        // A changed grade waits until it has been still for a moment (dragging a wheel); first sight: right away
        if (it != m_memos.end() && it->mediaPath == c.mediaPath) {
            const bool changed = m.animated || it->animated || !(m.spec == it->spec);
            m.settledAt = changed ? m_clock.elapsed() + kSettleMs : it->settledAt;
        }
        if (it == m_memos.end() && m_memos.size() >= kMaxMemos) m_memos.clear();
        it = m_memos.insert(c.id, std::move(m));
    }
    if (!it->animated && it->spec.isNull()) return g; // grade that changes nothing
    g.m_owner = this;
    g.m_clip = &c;
    g.m_animated = it->animated;
    g.m_spec = it->spec;
    g.m_settledAt = it->settledAt;
    return g;
}

QImage GradedThumbs::ClipGrade::image(const QImage& raw, int t) const
{
    return m_owner ? m_owner->image(*this, raw, t) : raw;
}

QImage GradedThumbs::image(const ClipGrade& g, const QImage& raw, int t)
{
    const Spec spec = g.m_animated ? specAt(*g.m_clip, t) : g.m_spec;
    if (spec.isNull() || raw.isNull()) return raw;
    const Key key{raw.cacheKey(), spec.hash};
    const TileKey tile{g.m_clip->id, t};
    if (const QImage* done = m_done.object(key)) {
        const QImage* shown = m_shown.object(tile);
        if (!shown || shown->cacheKey() != done->cacheKey()) m_shown.insert(tile, new QImage(*done), costKB(*done));
        const Look* look = m_looks.object(tile.clip);
        if (!look || look->grade != spec.hash || look->image.cacheKey() != done->cacheKey())
            m_looks.insert(tile.clip, new Look{spec.hash, *done}, costKB(*done));
        ++m_tilesGraded;
        return *done;
    }
    const qint64 now = m_clock.elapsed();
    if (now < g.m_settledAt) { // still changing: look again once it has settled
        const int wait = int(g.m_settledAt - now) + 1;
        if (!m_settle->isActive() || m_settle->remainingTime() < wait) m_settle->start(wait);
    } else {
        queue(key, raw, spec);
    }
    if (const QImage* shown = m_shown.object(tile)) {
        ++m_tilesPrevious;
        return *shown;
    }
    // A tile seen for the first time (scrolled or zoomed into view): a neighbor's picture with the same look, which
    // is closer than the ungraded one; for one frame or two, like the coarser thumbnails before the exact ones
    if (const Look* look = m_looks.object(tile.clip); look && look->grade == spec.hash) {
        ++m_tilesNeighbor;
        return look->image;
    }
    ++m_tilesUngraded;
    return raw;
}

void GradedThumbs::queue(const Key& key, const QImage& raw, const Spec& spec)
{
    QMutexLocker lock(&m_mutex);
    if (m_pending.contains(key)) return;
    m_pending.insert(key);
    m_jobs.push_front(Job{key, raw, spec, m_generation});
    while (m_jobs.size() > kMaxJobs) {
        m_pending.remove(m_jobs.back().key);
        m_jobs.pop_back();
    }
    m_cond.wakeOne();
}

void GradedThumbs::loop()
{
    for (;;) {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_jobs.empty()) m_cond.wait(&m_mutex);
            if (m_quit) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            m_busy = true;
        }
        Background::yieldToForeground();
        const QImage img = apply(job.raw, job.spec);
        {
            QMutexLocker lock(&m_mutex);
            m_busy = false;
            ++m_computed;
        }
        QMetaObject::invokeMethod(
            this, [this, key = job.key, img, gen = job.generation] { finished(key, img, gen); }, Qt::QueuedConnection);
    }
}

void GradedThumbs::finished(const Key& key, const QImage& img, int generation)
{
    {
        QMutexLocker lock(&m_mutex);
        m_pending.remove(key);
    }
    if (generation != m_generation || img.isNull()) return;
    m_done.insert(key, new QImage(img), costKB(img));
    emit updated();
}

void GradedThumbs::clear()
{
    ++m_generation;
    m_done.clear();
    m_shown.clear();
    m_looks.clear();
    m_memos.clear();
    QMutexLocker lock(&m_mutex);
    m_jobs.clear();
    m_pending.clear();
}

int GradedThumbs::computed() const
{
    QMutexLocker lock(&m_mutex);
    return m_computed;
}

bool GradedThumbs::idle() const
{
    QMutexLocker lock(&m_mutex);
    return m_jobs.empty() && !m_busy && m_pending.isEmpty();
}

// ---------- TimelinePosters ----------

TimelinePosters::Source TimelinePosters::sourceOf(const Project& project, const Timeline& tl)
{
    // Earliest picture; at that frame the topmost track wins (tracks from V1 upward, ties go to the higher one)
    const Clip* best = nullptr;
    for (const Track& track : tl.video) {
        if (track.hidden) continue;
        for (const Clip& c : track.clips) {
            const MediaInfo* m = c.kind == ClipKind::Media && c.enabled ? project.mediaInfo(c.mediaPath) : nullptr;
            if (!m || !m->hasVideo) continue;
            if (!best || c.start <= best->start) best = &c;
            break; // sorted by start: the first usable clip is the track's earliest
        }
    }
    Source s;
    if (!best) return s;
    const MediaInfo* m = project.mediaInfo(best->mediaPath);
    s.path = best->mediaPath;
    if (m->isImage) s.frame = 0;
    else if (best->isRetimed() && m->length > 0)
        s.frame = std::clamp(int(RetimeMap(*best, m->length).fileFrameAt(best->freeze ? best->stillFrame() : best->in)), 0,
                             m->length - 1);
    else s.frame = best->in;
    s.grade = GradedThumbs::specAt(*best, 0);
    return s;
}

TimelinePosters::TimelinePosters(Project* project, const QSize& size, QObject* parent)
    : QObject(parent), m_project(project), m_size(size)
{
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(1);
    m_pool->setThreadPriority(QThread::LowPriority);
    // Edits come in bursts (dragging a wheel = one change per step): look once they are over
    m_settle = new QTimer(this);
    m_settle->setSingleShot(true);
    m_settle->setInterval(GradedThumbs::kSettleMs);
    connect(m_settle, &QTimer::timeout, this, &TimelinePosters::refresh);
    connect(project, &Project::timelineChanged, m_settle, qOverload<>(&QTimer::start));
    connect(project, &Project::formatChanged, this, [this] { // pictures in the new aspect ratio, asked for anew
        ++m_generation;
        m_entries.clear();
    });
}

TimelinePosters::~TimelinePosters()
{
    m_pool->clear();
    m_pool->waitForDone();
}

QImage TimelinePosters::poster(int sequenceId)
{
    auto it = m_entries.find(sequenceId);
    if (it == m_entries.end()) {
        const Sequence* q = m_project->sequence(sequenceId);
        if (!q) return {};
        Entry e;
        e.wanted = sourceOf(*m_project, q->timeline);
        m_entries.insert(sequenceId, e);
        start(sequenceId);
        it = m_entries.find(sequenceId);
    }
    return it->image;
}

void TimelinePosters::refresh()
{
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        const Sequence* q = m_project->sequence(it.key());
        it->wanted = q ? sourceOf(*m_project, q->timeline) : Source{};
    }
    const QList<int> ids = m_entries.keys();
    for (int id : ids) start(id);
}

void TimelinePosters::start(int sequenceId)
{
    const auto it = m_entries.find(sequenceId);
    if (it == m_entries.end() || it->busy || it->wanted == it->shown) return; // busy: done() looks again
    const Source src = it->wanted;
    if (src.path.isEmpty()) { // no picture (any more): the type icon
        *it = Entry{src, src, {}, {}, false};
        emit changed(sequenceId);
        return;
    }
    it->busy = true;
    // Same frame, only the grade changed: the decoded frame is reused
    const QImage raw = src.path == it->shown.path && src.frame == it->shown.frame ? it->raw : QImage();
    const ProjectFormat format = m_project->format();
    const QSize size = m_size;
    const int generation = m_generation;
    m_pool->start([this, sequenceId, src, raw, format, size, generation] {
        Background::lowerThisThread(Background::Level::Low); // own pool: its thread does nothing else
        Background::yieldToForeground();
        const QImage frame = raw.isNull() ? Engine::thumbnail(format, src.path, src.frame, size) : raw;
        const QImage img = frame.isNull() || src.grade.isNull() ? frame : GradedThumbs::apply(frame, src.grade);
        QMetaObject::invokeMethod(
            this, [this, sequenceId, src, frame, img, generation] { done(sequenceId, src, frame, img, generation); },
            Qt::QueuedConnection);
    });
}

void TimelinePosters::done(int sequenceId, const Source& source, const QImage& raw, const QImage& image, int generation)
{
    if (generation != m_generation) return;
    const auto it = m_entries.find(sequenceId);
    if (it == m_entries.end()) return;
    it->busy = false;
    it->shown = source;
    it->raw = raw;
    it->image = image;
    const bool again = !(it->wanted == it->shown); // changed again meanwhile
    emit changed(sequenceId);
    if (again) start(sequenceId);
}
