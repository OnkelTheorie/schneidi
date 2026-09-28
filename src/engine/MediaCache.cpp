#include "engine/MediaCache.h"

#include "engine/Profiles.h"

#include <Mlt.h>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kMaxThumbJobs = 96;
constexpr int kThumbCacheKB = 160 * 1024;

QVector<QVector<quint8>> buildLevels(QVector<quint8> base)
{
    QVector<QVector<quint8>> levels;
    levels << std::move(base);
    while (levels.last().size() > 1) {
        const QVector<quint8>& prev = levels.last();
        QVector<quint8> next((prev.size() + 1) / 2);
        for (int i = 0; i < next.size(); ++i) {
            const int a = prev[2 * i];
            const int b = 2 * i + 1 < prev.size() ? prev[2 * i + 1] : 0;
            next[i] = quint8(std::max(a, b));
        }
        levels << std::move(next);
    }
    return levels;
}

// Platten-Cache für Wellenformen: Schlüssel aus Pfad, Größe und Änderungszeit
QString waveCacheFile(const QString& path, const FrameRate& rate)
{
    const QFileInfo fi(path);
    const QString id = QString("%1|%2|%3|%4|%5")
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(QString("%1/%2").arg(rate.num).arg(rate.den))
                           .arg(Waveform::kBucketsPerFrame);
    const QString hash = QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/peaks";
    QDir().mkpath(dir);
    return dir + "/" + hash + ".peaks";
}

bool loadWave(const QString& file, Waveform& w)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream in(&f);
    quint32 magic = 0;
    qint32 frames = 0;
    QByteArray data;
    in >> magic >> frames >> data;
    if (magic != 0x53574631 || in.status() != QDataStream::Ok
        || data.size() != frames * Waveform::kBucketsPerFrame)
        return false;
    w.levels = buildLevels(QVector<quint8>(data.begin(), data.end()));
    w.frames = frames;
    w.complete = true;
    return true;
}

void saveWave(const QString& file, const QVector<quint8>& base, int frames)
{
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return;
    QDataStream out(&f);
    out << quint32(0x53574631) << qint32(frames)
        << QByteArray(reinterpret_cast<const char*>(base.constData()), base.size());
    f.commit();
}

} // namespace

MediaCache::MediaCache(QObject* parent) : QObject(parent)
{
    m_thumbs.setMaxCost(kThumbCacheKB);
    m_thumbThread = QThread::create([this] { thumbLoop(); });
    m_waveThread = QThread::create([this] { waveLoop(); });
    m_thumbThread->start(QThread::LowPriority);
    m_waveThread->start(QThread::LowestPriority);
}

MediaCache::~MediaCache()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
    }
    m_thumbCond.wakeAll();
    m_waveCond.wakeAll();
    m_thumbThread->wait();
    m_waveThread->wait();
    delete m_thumbThread;
    delete m_waveThread;
}

QString MediaCache::thumbKey(const QString& path, int frame)
{
    return path + '#' + QString::number(frame);
}

QImage MediaCache::cachedThumbnail(const QString& path, int frame)
{
    QMutexLocker lock(&m_mutex);
    if (const QImage* img = m_thumbs.object(thumbKey(path, frame))) return *img;
    return {};
}

QImage MediaCache::thumbnail(const QString& path, int frame)
{
    const QString key = thumbKey(path, frame);
    QMutexLocker lock(&m_mutex);
    if (const QImage* img = m_thumbs.object(key)) return *img;
    if (!m_thumbPending.contains(key)) {
        m_thumbPending.insert(key);
        m_thumbJobs.push_front({path, frame});
        // Alte Anfragen verwerfen (z. B. von Stellen, an denen man längst weitergescrollt hat)
        while (m_thumbJobs.size() > kMaxThumbJobs) {
            m_thumbPending.remove(thumbKey(m_thumbJobs.back().path, m_thumbJobs.back().frame));
            m_thumbJobs.pop_back();
        }
        m_thumbCond.wakeOne();
    }
    return {};
}

void MediaCache::setFormat(const ProjectFormat& format)
{
    {
        QMutexLocker lock(&m_mutex);
        if (format == m_format) return;
        m_format = format;
        ++m_generation;
        m_thumbs.clear();
        m_thumbJobs.clear();
        m_thumbPending.clear();
        m_waves.clear();
        m_waveJobs.clear();
        m_waveRequested.clear();
    }
    emit updated();
}

std::shared_ptr<const Waveform> MediaCache::waveform(const QString& path)
{
    QMutexLocker lock(&m_mutex);
    if (!m_waveRequested.contains(path)) {
        m_waveRequested.insert(path);
        m_waveJobs.push_back(path);
        m_waveCond.wakeOne();
    }
    return m_waves.value(path);
}

void MediaCache::thumbLoop()
{
    std::unique_ptr<Mlt::Profile> profile;
    int generation = -1;
    // Offene Producer wiederverwenden (Datei öffnen ist teurer als Suchen)
    QHash<QString, std::shared_ptr<Mlt::Producer>> producers;

    for (;;) {
        ThumbJob job;
        ProjectFormat format;
        int gen = 0;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_thumbJobs.empty()) m_thumbCond.wait(&m_mutex);
            if (m_quit) return;
            job = m_thumbJobs.front();
            m_thumbJobs.pop_front();
            format = m_format;
            gen = m_generation;
        }
        if (gen != generation) { // neues Projektformat: Producer hängen am alten Profil
            producers.clear();
            profile = makeProfile(format);
            generation = gen;
        }

        std::shared_ptr<Mlt::Producer> prod = producers.value(job.path);
        if (!prod) {
            if (producers.size() >= 8) producers.clear();
            prod = std::make_shared<Mlt::Producer>(*profile, job.path.toUtf8().constData());
            producers.insert(job.path, prod);
        }

        QImage img;
        if (prod->is_valid()) {
            prod->seek(job.frame);
            std::unique_ptr<Mlt::Frame> f(prod->get_frame());
            if (f) {
                // Im Seitenverhältnis des Projekts, mindestens so groß wie eine 16:9-Kachel
                // (die Timeline schneidet die Mitte aus; Hochformat also etwas höher)
                mlt_image_format fmt = mlt_image_rgba;
                const QSize want = format.size().scaled(kThumbHeight * 16 / 9, kThumbHeight, Qt::KeepAspectRatioByExpanding);
                int w = std::max(2, want.width()), h = std::max(2, want.height());
                const int reqW = w, reqH = h;
                const uint8_t* data = f->get_image(fmt, w, h);
                if (data && w > 0 && h > 0) {
                    QImage raw(w, h, QImage::Format_RGBA8888);
                    std::memcpy(raw.bits(), data, size_t(w) * h * 4);
                    img = (w == reqW && h == reqH) ? raw : raw.scaled(reqW, reqH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    img = img.convertToFormat(QImage::Format_RGB32); // schneller zu zeichnen
                }
            }
        }

        {
            QMutexLocker lock(&m_mutex);
            if (gen != m_generation) continue; // inzwischen neues Format
            const QString key = thumbKey(job.path, job.frame);
            m_thumbPending.remove(key);
            if (!img.isNull()) m_thumbs.insert(key, new QImage(img), int(img.sizeInBytes() / 1024) + 1);
        }
        if (!img.isNull()) emit updated();
    }
}

void MediaCache::waveLoop()
{
    constexpr int N = Waveform::kBucketsPerFrame;

    for (;;) {
        QString path;
        ProjectFormat format;
        int gen = 0;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_waveJobs.empty()) m_waveCond.wait(&m_mutex);
            if (m_quit) return;
            path = m_waveJobs.front();
            m_waveJobs.pop_front();
            format = m_format;
            gen = m_generation;
        }
        // Wellenform zählt Frames in der Projekt-Framerate
        const std::unique_ptr<Mlt::Profile> profile = makeProfile(format);
        const double fps = profile->fps();

        auto publish = [&](std::shared_ptr<const Waveform> w) {
            {
                QMutexLocker lock(&m_mutex);
                if (gen != m_generation) return; // inzwischen neues Format
                m_waves.insert(path, std::move(w));
            }
            emit updated();
        };

        const QString cacheFile = waveCacheFile(path, format.rate);
        {
            auto w = std::make_shared<Waveform>();
            if (loadWave(cacheFile, *w)) {
                publish(w);
                continue;
            }
        }

        Mlt::Producer p(*profile, path.toUtf8().constData());
        const int length = p.is_valid() ? p.get_length() : 0;
        if (length <= 0) {
            auto w = std::make_shared<Waveform>();
            w->complete = true;
            publish(w);
            continue;
        }

        QVector<quint8> base(length * N, 0);
        QElapsedTimer sincePublish;
        sincePublish.start();
        bool aborted = false;
        for (int i = 0; i < length; ++i) {
            if (i % 256 == 0) {
                QMutexLocker lock(&m_mutex);
                if (m_quit || gen != m_generation) {
                    aborted = true;
                    break;
                }
            }
            p.seek(i);
            std::unique_ptr<Mlt::Frame> f(p.get_frame());
            if (!f) continue;
            // Nur Ton holen – ohne get_image wird das Video gar nicht dekodiert
            mlt_audio_format fmt = mlt_audio_s16;
            int freq = 48000, channels = 2;
            int samples = mlt_audio_calculate_frame_samples(float(fps), freq, i);
            const auto* pcm = static_cast<const int16_t*>(f->get_audio(fmt, freq, channels, samples));
            if (!pcm || samples <= 0 || channels <= 0) continue;
            for (int b = 0; b < N; ++b) {
                const int s0 = samples * b / N, s1 = samples * (b + 1) / N;
                int peak = 0;
                for (int s = s0 * channels; s < s1 * channels; ++s) peak = std::max(peak, std::abs(int(pcm[s])));
                base[i * N + b] = quint8(std::min(255, peak >> 7));
            }

            // Zwischenstände zeigen, damit lange Dateien nicht ewig leer bleiben
            if (sincePublish.elapsed() > 700) {
                auto w = std::make_shared<Waveform>();
                w->levels = buildLevels(base);
                w->frames = i + 1;
                publish(w);
                sincePublish.restart();
            }
        }
        if (aborted) {
            QMutexLocker lock(&m_mutex);
            if (m_quit) return;
            continue; // Formatwechsel: Ergebnis gilt nicht mehr
        }

        auto w = std::make_shared<Waveform>();
        w->levels = buildLevels(base);
        w->frames = length;
        w->complete = true;
        publish(w);
        saveWave(cacheFile, base, length);
    }
}
