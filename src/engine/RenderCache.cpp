#include "engine/RenderCache.h"

#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/EffectRegistry.h"
#include "engine/Bundle.h"
#include "engine/Exporter.h"
#include "engine/Profiles.h"
#include "engine/Stabilizer.h"
#include "engine/TimelineBuilder.h"
#include "engine/Background.h"

#include <Mlt.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QSet>
#include <QThread>
#include <algorithm>

namespace {

constexpr const char* kSettingKey = "playback/renderCache";
constexpr const char* kLimitKey = "playback/renderCacheLimit";
constexpr int kIdleMs = 1500; // this long a rest after a change before rendering
// Increase when the way of rendering changes (codec, color space …) -> old cache files no longer fit
constexpr const char* kVersion = "rc1";
// Cropped clips render differently since project format 3 (crop measured on the fitted picture, letterbox bars stay
// transparent): only their cache files are stale
constexpr const char* kCropVersion = "crop2";
// Since 0.13 the Green Screen key renders right after the grade, before the other effects (before: at its place in
// the clip's effect list): only cache files of keyed clips are stale
constexpr const char* kKeyVersion = "key2";

QString modeId(RenderCache::Mode m)
{
    switch (m) {
    case RenderCache::Mode::Off: return "off";
    case RenderCache::Mode::Smart: return "smart";
    default: return "user";
    }
}

RenderCache::Mode modeFromId(const QString& id)
{
    if (id == "off") return RenderCache::Mode::Off;
    if (id == "smart") return RenderCache::Mode::Smart;
    return RenderCache::Mode::User;
}

// One image of the clip output at project size (RGBA)
const uint8_t* frameImage(Mlt::Producer& p, int pos, int w, int h, std::unique_ptr<Mlt::Frame>& keep)
{
    p.seek(pos);
    keep.reset(p.get_frame());
    if (!keep) return nullptr;
    mlt_image_format fmt = mlt_image_rgba;
    int iw = w, ih = h;
    const uint8_t* d = keep->get_image(fmt, iw, ih);
    return d && iw == w && ih == h ? d : nullptr;
}

bool hasAlpha(const uint8_t* rgba, int w, int h)
{
    const size_t n = size_t(w) * h;
    for (size_t i = 0; i < n; ++i)
        if (rgba[i * 4 + 3] < 255) return true;
    return false;
}

// Transparent? Cropping/opacity/transform for sure (even if only a part is animated), otherwise samples
// (aspect ratio different from the project, Green Screen …)
bool needsAlpha(const Clip& c, Mlt::Producer& p, int len, int w, int h)
{
    if (Keys::hasCrop(c) || Keys::hasOpacity(c) || Keys::hasTransform(c)) return true;
    for (int pos : {0, len / 2, len - 1}) {
        std::unique_ptr<Mlt::Frame> f;
        const uint8_t* d = frameImage(p, std::max(0, pos), w, h, f);
        if (d && hasAlpha(d, w, h)) return true;
    }
    return false;
}

} // namespace

RenderCache::RenderCache(QObject* parent) : QObject(parent)
{
    m_mode = modeFromId(QSettings().value(kSettingKey, "user").toString());
    m_limit = std::max<qint64>(0, QSettings().value(kLimitKey, kDefaultLimit).toLongLong());
    m_idle.setSingleShot(true);
    m_idle.setInterval(kIdleMs);
    connect(&m_idle, &QTimer::timeout, this, &RenderCache::startNext);
    m_poll.setInterval(250);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        const int p = m_progress ? m_progress->load() : -1;
        if (p != m_lastProgress) {
            m_lastProgress = p;
            emit progressChanged();
        }
    });
}

RenderCache::~RenderCache()
{
    m_jobs.clear();
    cancelCurrent();
}

QString RenderCache::cacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/rendercache";
}

void RenderCache::setMode(Mode mode)
{
    if (mode == m_mode) return;
    m_mode = mode;
    QSettings().setValue(kSettingKey, modeId(mode));
    if (mode == Mode::Off) {
        m_jobs.clear();
        cancelCurrent();
    }
    emit modeChanged(mode);
    emit cacheChanged(); // rebuild preview (reads cache resp. no longer), jobs come with the next sync()
}

void RenderCache::setFormat(const ProjectFormat& format)
{
    if (format == m_format) return;
    m_format = format;
    m_jobs.clear();
    cancelCurrent(); // keys depend on the format; the next sync() queues anew
}

bool RenderCache::isExpensive(const Clip& c)
{
    const bool fx = std::any_of(c.effects.begin(), c.effects.end(), [](const EffectInstance& e) { return e.enabled; });
    return fx || (!c.freeze && (c.speed != 1.0 || c.reverse || !c.ramp.isEmpty()));
}

bool RenderCache::wanted(const Clip& c, Mode mode)
{
    if (c.isTitle() || c.mediaPath.isEmpty() || !c.enabled || c.length() <= 0) return false;
    switch (mode) {
    case Mode::Off: return false;
    case Mode::User: return c.renderCache;
    case Mode::Smart: return c.renderCache || isExpensive(c);
    }
    return false;
}

Clip RenderCache::normalized(const Clip& c)
{
    Clip n = c;
    n.id = 0;
    n.start = 0;
    n.linkId = 0;
    n.volumeDb = 0;
    n.pan = 0;
    n.enabled = true;
    n.transIn = n.transOut = 0;
    n.transInAlone = n.transOutAlone = false;
    n.transInStyle = n.transOutStyle = TransitionStyle{};
    n.fadeIn = n.fadeOut = 0;
    n.keepPitch = true;
    n.renderCache = false;
    n.keys.remove(AnimParam::Volume);
    n.keys.remove(AnimParam::Pan);
    return n;
}

QString RenderCache::key(const Clip& c, const ProjectFormat& f)
{
    // Clip as in the project file: new fields (e.g. further effects) automatically count along
    const QByteArray clip = QJsonDocument(ProjectFile::clipJson(normalized(c))).toJson(QJsonDocument::Compact);
    const QFileInfo fi(c.mediaPath);
    const QString id = QString("%1|%2|%3|%4|%5x%6@%7/%8|")
                           .arg(kVersion, fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(f.width)
                           .arg(f.height)
                           .arg(f.rate.num)
                           .arg(f.rate.den);
    const QByteArray crop = Keys::hasCrop(c) ? QByteArray(kCropVersion) : QByteArray();
    const QByteArray key = EffectRegistry::has(c, "chromakey") ? QByteArray(kKeyVersion) : QByteArray();
    return QCryptographicHash::hash(id.toUtf8() + crop + key + clip, QCryptographicHash::Sha1).toHex();
}

QString RenderCache::path(const Clip& c) const
{
    return cacheDir() + "/" + key(c, m_format) + ".mov";
}

QString RenderCache::resolve(const Clip& c) const
{
    if (!wanted(c)) return {};
    const QString file = path(c);
    return QFileInfo::exists(file) ? file : QString();
}

QVector<RenderCache::Span> RenderCache::spans(const Timeline& tl) const
{
    QVector<Span> out;
    if (m_mode == Mode::Off) return out;
    const double running = m_progress ? m_progress->load() / 1000.0 : 0.0;
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips) {
            if (!wanted(c)) continue;
            const QString k = key(c, m_format);
            double done = QFileInfo::exists(cacheDir() + "/" + k + ".mov") ? 1.0 : 0.0;
            if (done == 0.0 && k == m_currentKey) done = running;
            out << Span{c.start, c.end(), done};
        }
    return out;
}

int RenderCache::pendingCount() const
{
    return m_jobs.size();
}

void RenderCache::sync(const Timeline& tl)
{
    QVector<Job> jobs;
    QSet<QString> seen, inUse;
    if (m_mode != Mode::Off) {
        // Sorted by position (tracks from top to bottom) -> start of the timeline first
        QVector<const Clip*> clips;
        for (int i = tl.video.size() - 1; i >= 0; --i)
            for (const Clip& c : tl.video[i].clips) clips << &c;
        std::stable_sort(clips.begin(), clips.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
        for (const Clip* c : clips) {
            if (!wanted(*c) || !QFileInfo::exists(c->mediaPath)) continue;
            // Stabilization still analyzing: the output would be unstabilized (rendered once the result is there)
            if (const EffectInstance* st = EffectRegistry::instance(*c, Stabilizer::EffectId);
                st && st->enabled && Stabilizer::available())
                if (const auto r = Stabilizer::request(*c, m_format); r && !Stabilizer::find(*r).valid()) continue;
            const QString k = key(*c, m_format);
            inUse.insert(k);
            if (seen.contains(k) || m_failed.contains(k)) continue;
            const QString file = cacheDir() + "/" + k + ".mov";
            if (QFileInfo::exists(file)) {
                // Used again -> counts as recent for the size limit (once per session is enough)
                if (!m_touched.contains(k)) {
                    QFile f(file);
                    if (f.open(QIODevice::ReadWrite)) f.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
                    m_touched.insert(k);
                }
                continue;
            }
            seen.insert(k);
            jobs << Job{k, normalized(*c)};
        }
    }
    m_inUse = inUse;
    if (!m_trimmedOnce) { // old files from earlier sessions
        m_trimmedOnce = true;
        trim();
    }
    m_jobs = jobs;
    // Running job outdated (clip changed/deleted, marker removed) -> cancel
    if (!m_currentKey.isEmpty() && !seen.contains(m_currentKey)) cancelCurrent();
    if (!m_jobs.isEmpty() && !m_thread) m_idle.start(); // wait (anew) as long as changes continue
}

void RenderCache::setPlaying(bool on)
{
    m_playing = on;
}

void RenderCache::clear()
{
    cancelCurrent();
    m_failed.clear();
    const QDir dir(cacheDir());
    for (const QString& f : dir.entryList({"*.mov"}, QDir::Files)) QFile::remove(dir.filePath(f));
    emit cacheChanged(); // preview reads the originals again; the following sync() renders anew
}

void RenderCache::setLimit(qint64 bytes)
{
    bytes = std::max<qint64>(0, bytes);
    if (bytes == m_limit) return;
    m_limit = bytes;
    QSettings().setValue(kLimitKey, bytes);
    trim();
}

qint64 RenderCache::usedBytes() const
{
    qint64 sum = 0;
    const QDir dir(cacheDir());
    for (const QFileInfo& fi : dir.entryInfoList({"*.mov"}, QDir::Files)) sum += fi.size();
    return sum;
}

int RenderCache::trimDir(const QString& dirPath, qint64 limit, const QSet<QString>& keep)
{
    if (limit <= 0) return 0;
    QFileInfoList files = QDir(dirPath).entryInfoList({"*.mov"}, QDir::Files);
    qint64 sum = 0;
    for (const QFileInfo& fi : files) sum += fi.size();
    if (sum <= limit) return 0;
    std::sort(files.begin(), files.end(),
              [](const QFileInfo& a, const QFileInfo& b) { return a.lastModified() < b.lastModified(); });
    int removed = 0;
    for (const QFileInfo& fi : files) {
        if (sum <= limit) break;
        if (keep.contains(fi.completeBaseName())) continue;
        if (QFile::remove(fi.absoluteFilePath())) {
            sum -= fi.size();
            ++removed;
        }
    }
    return removed;
}

void RenderCache::trim()
{
    QSet<QString> keep = m_inUse;
    // Lowering the limit while a clip renders: its file in the making (<key>.mov.part.mov) stays
    if (!m_currentKey.isEmpty()) keep.insert(m_currentKey + ".mov.part");
    if (trimDir(cacheDir(), m_limit, keep) > 0) emit cacheChanged();
}

void RenderCache::cancelCurrent()
{
    m_idle.stop();
    if (!m_thread) return;
    *m_cancel = true;
    m_thread->disconnect(this);
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    m_currentKey.clear();
    m_progress.reset();
    m_poll.stop();
    emit progressChanged();
}

void RenderCache::startNext()
{
    if (m_thread || m_jobs.isEmpty() || m_mode == Mode::Off) return;
    const Job job = m_jobs.takeFirst();
    const QString target = cacheDir() + "/" + job.key + ".mov";
    if (QFileInfo::exists(target)) { // finished in the meantime (same clip twice)
        startNext();
        return;
    }
    QDir().mkpath(cacheDir());
    m_currentKey = job.key;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_progress = std::make_shared<std::atomic<int>>(0);
    m_lastProgress = -1;
    auto error = std::make_shared<QString>();
    auto ok = std::make_shared<bool>(false);
    const ProjectFormat format = m_format;
    auto cancel = m_cancel;
    auto progress = m_progress;
    m_thread = QThread::create([this, job, format, target, error, ok, cancel, progress] {
        Background::lowerThisThread(Background::Level::Lowest);
        // Pause while the preview runs or an export is running (they take precedence)
        auto paused = [this] { return m_playing.load() || Exporter::anyRunning() || Background::foregroundBusy(); };
        *ok = render(job.clip, format, target, error.get(), cancel.get(), paused, progress.get());
    });
    connect(m_thread, &QThread::finished, this, [this, error, ok] { onFinished(*ok, *error); });
    m_thread->start(QThread::LowestPriority);
    m_poll.start();
    emit progressChanged();
}

void RenderCache::onFinished(bool ok, const QString& error)
{
    const QString key = m_currentKey;
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    m_currentKey.clear();
    m_progress.reset();
    m_poll.stop();
    if (!ok) {
        m_failed.insert(key, true);
        if (!error.isEmpty() && !m_reported.contains(error)) {
            m_reported.insert(error, true);
            emit failed(error);
        }
    }
    if (ok) {
        m_touched.insert(key);
        trimDir(cacheDir(), m_limit, m_inUse);
    }
    emit cacheChanged();
    if (!m_jobs.isEmpty()) QTimer::singleShot(0, this, &RenderCache::startNext);
}

bool RenderCache::render(const Clip& clip, const ProjectFormat& format, const QString& file, QString* error,
                         const std::atomic<bool>* cancel, const std::function<bool()>& paused,
                         std::atomic<int>* progress)
{
    auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    auto cancelled = [&] { return cancel && cancel->load(); };
    const Clip c = normalized(clip);
    const int len = c.length();
    if (len <= 0) return fail({});

    auto profile = makeProfile(format);
    TimelineBuilder builder(*profile);
    std::unique_ptr<Mlt::Tractor> tractor = builder.buildClipOutput(c);
    if (!tractor || !tractor->is_valid()) return fail(T("Render-Cache: Clip konnte nicht geöffnet werden."));
    const int w = profile->width(), h = profile->height();
    const bool alpha = needsAlpha(c, *tractor, len, w, h);

    QString ffmpeg = Bundle::tool("ffmpeg");
    if (!QFileInfo(ffmpeg).isAbsolute()) return fail(T("ffmpeg wurde nicht gefunden. Bitte ffmpeg installieren."));

    // Quickly decodable intra codec: ProRes 422 LT, with transparency ProRes 4444 (alpha). MLT delivers RGBA in the
    // project profile (BT.709) -> tag and convert accordingly, then the preview reads back the same colors.
    const QString part = file + ".part.mov";
    const QStringList args{
        "-hide_banner", "-nostdin", "-v", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgba", "-s", QString("%1x%2").arg(w).arg(h),
        "-framerate", QString("%1/%2").arg(format.rate.num).arg(format.rate.den), "-i", "pipe:0",
        // setparams: otherwise no color tag ends up in the file and MLT takes it (below 720 lines) for BT.601
        "-vf", "scale=out_color_matrix=bt709:out_range=tv,"
               "setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv",
        "-c:v", "prores_ks", "-profile:v", alpha ? "4" : "1",
        "-pix_fmt", alpha ? "yuva444p10le" : "yuv422p10le",
        "-an", "-f", "mov", part};

    QProcess proc;
    proc.setStandardOutputFile(QProcess::nullDevice());
    Background::lowerProcess(proc); // preview and operation take precedence
    proc.start(ffmpeg, args);
    if (!proc.waitForStarted(10000)) return fail(T("Render-Cache: ffmpeg ließ sich nicht starten."));

    auto abort = [&] {
        proc.kill();
        proc.waitForFinished(3000);
        QFile::remove(part);
        return false;
    };
    const qint64 frameBytes = qint64(w) * h * 4;
    for (int i = 0; i < len; ++i) {
        while (paused && paused() && !cancelled()) QThread::msleep(100);
        if (cancelled()) return abort();
        std::unique_ptr<Mlt::Frame> f;
        const uint8_t* d = frameImage(*tractor, i, w, h, f);
        if (!d) {
            abort();
            return fail(T("Render-Cache: Bild %1 ließ sich nicht rendern.").arg(i));
        }
        if (proc.write(reinterpret_cast<const char*>(d), frameBytes) != frameBytes) {
            const QString msg = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
            abort();
            return fail(T("Render-Cache fehlgeschlagen:\n%1").arg(msg.section('\n', -3)));
        }
        // Keep the buffer small (a few images), otherwise memory grows with slow encoding
        while (proc.bytesToWrite() > 2 * frameBytes && proc.state() == QProcess::Running)
            proc.waitForBytesWritten(1000);
        if (progress) *progress = int(qint64(i + 1) * 1000 / len);
    }
    while (proc.bytesToWrite() > 0 && proc.state() == QProcess::Running) proc.waitForBytesWritten(1000);
    proc.closeWriteChannel();
    while (!proc.waitForFinished(200))
        if (cancelled() || proc.state() == QProcess::NotRunning) break;
    if (cancelled()) return abort();
    const QString log = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || QFileInfo(part).size() <= 0) {
        QFile::remove(part);
        qWarning("Render-Cache fehlgeschlagen (Code %d): %s", proc.exitCode(), qPrintable(log.right(2000)));
        return fail(T("Render-Cache fehlgeschlagen:\n%1").arg(log.section('\n', -3)));
    }
    QFile::remove(file);
    if (!QFile::rename(part, file)) { // only now visible -> preview never reads half-finished files
        QFile::remove(part);
        return fail(T("Render-Cache: Datei konnte nicht gespeichert werden."));
    }
    return true;
}
