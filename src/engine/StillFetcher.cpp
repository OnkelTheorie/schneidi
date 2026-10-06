#include "engine/StillFetcher.h"

#include "engine/Profiles.h"
#include "engine/ProxyManager.h"

#include <Mlt.h>
#include <QFileInfo>
#include <QHash>
#include <QThread>
#include <cstring>
#include <memory>

namespace {
constexpr int kCacheKB = 64 * 1024; // ~40 frames at 960x540

// Proxy if it exists (same rule as the filmstrip in MediaCache): much faster to seek than a 4K original
QString decodePath(const QString& path)
{
    const QString proxy = ProxyManager::proxyPath(path);
    return QFileInfo::exists(proxy) ? proxy : path;
}
} // namespace

StillFetcher::StillFetcher(QObject* parent) : QObject(parent)
{
    m_cache.setMaxCost(kCacheKB);
    m_thread = QThread::create([this] { loop(); });
    m_thread->start();
}

StillFetcher::~StillFetcher()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
    }
    m_cond.wakeAll();
    m_thread->wait();
    delete m_thread;
}

QString StillFetcher::key(const QString& path, int frame)
{
    return path + QChar(0) + QString::number(frame);
}

void StillFetcher::setFormat(const ProjectFormat& format)
{
    QMutexLocker lock(&m_mutex);
    if (format == m_format) return;
    m_format = format;
    ++m_generation;
    m_cache.clear();
    m_jobs.clear();
}

void StillFetcher::setMaxEdge(int pixels)
{
    QMutexLocker lock(&m_mutex);
    if (pixels == m_maxEdge) return;
    m_maxEdge = std::max(16, pixels);
    m_cache.clear();
}

QImage StillFetcher::cached(const QString& path, int frame) const
{
    QMutexLocker lock(&m_mutex);
    const QImage* img = m_cache.object(key(path, frame));
    return img ? *img : QImage();
}

void StillFetcher::request(const QVector<Request>& requests)
{
    QMutexLocker lock(&m_mutex);
    m_jobs.clear();
    for (const Request& r : requests) {
        if (r.path.isEmpty() || m_cache.contains(key(r.path, r.frame))) continue;
        bool dup = false;
        for (const Request& j : m_jobs) dup |= j.path == r.path && j.frame == r.frame;
        if (!dup) m_jobs << r;
    }
    if (!m_jobs.isEmpty()) m_cond.wakeOne();
}

int StillFetcher::pending() const
{
    QMutexLocker lock(&m_mutex);
    return m_jobs.size();
}

int StillFetcher::decodedCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_decoded;
}

void StillFetcher::loop()
{
    std::unique_ptr<Mlt::Profile> profile;
    std::unique_ptr<ProducerFactory> factory; // BT.601 workaround (Profiles.h), outlives the producers
    int generation = -1;
    // Open producers are reused: seeking near the last frame (dragging an edit) is much cheaper than opening
    QHash<QString, std::shared_ptr<Mlt::Producer>> producers;

    for (;;) {
        Request job;
        ProjectFormat format;
        int gen = 0, maxEdge = 0;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_jobs.isEmpty()) m_cond.wait(&m_mutex);
            if (m_quit) return;
            job = m_jobs.takeFirst();
            format = m_format;
            gen = m_generation;
            maxEdge = m_maxEdge;
        }
        if (gen != generation) { // new project format: producers belong to the old profile
            producers.clear();
            factory.reset();
            profile = makeProfile(format);
            factory = std::make_unique<ProducerFactory>(*profile);
            generation = gen;
        }

        std::shared_ptr<Mlt::Producer> prod = producers.value(job.path);
        if (!prod) {
            if (producers.size() >= 6) producers.clear();
            prod = factory->open(decodePath(job.path));
            prod->set("audio_index", -1); // no sound needed here
            producers.insert(job.path, prod);
        }

        QImage img;
        if (prod->is_valid()) {
            prod->seek(job.frame);
            std::unique_ptr<Mlt::Frame> f(prod->get_frame());
            if (f) {
                const QSize want = format.size().scaled(maxEdge, maxEdge, Qt::KeepAspectRatio);
                mlt_image_format fmt = mlt_image_rgba;
                int w = std::max(2, want.width()), h = std::max(2, want.height());
                const uint8_t* data = f->get_image(fmt, w, h);
                if (data && w > 0 && h > 0) {
                    QImage raw(w, h, QImage::Format_RGBA8888);
                    std::memcpy(raw.bits(), data, size_t(w) * h * 4);
                    img = raw.convertToFormat(QImage::Format_RGB32); // faster to draw
                }
            }
        }

        {
            QMutexLocker lock(&m_mutex);
            if (gen != m_generation) continue; // format changed meanwhile
            ++m_decoded;
            if (!img.isNull())
                m_cache.insert(key(job.path, job.frame), new QImage(img), int(img.sizeInBytes() / 1024) + 1);
        }
        if (!img.isNull()) emit ready(job.path, job.frame, img);
    }
}
