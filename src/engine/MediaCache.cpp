#include "engine/MediaCache.h"

#include "engine/Profiles.h"
#include "engine/ProxyManager.h"

#include <Mlt.h>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <algorithm>
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

// Vorhandenen Proxy fürs Dekodieren nehmen (viel kleiner, dichte Keyframes), gleiche Frame-Zählung
QString decodePath(const QString& path)
{
    const QString proxy = ProxyManager::proxyPath(path);
    return QFileInfo::exists(proxy) ? proxy : path;
}

QSize thumbSize(const ProjectFormat& format)
{
    // Im Seitenverhältnis des Projekts, mindestens so groß wie eine 16:9-Kachel
    // (die Timeline schneidet die Mitte aus; Hochformat also etwas höher)
    const QSize s = format.size().scaled(MediaCache::kThumbHeight * 16 / 9, MediaCache::kThumbHeight, Qt::KeepAspectRatioByExpanding);
    return QSize(std::max(2, s.width()), std::max(2, s.height()));
}

} // namespace

MediaCache::MediaCache(QObject* parent) : QObject(parent)
{
    m_thumbs.setMaxCost(kThumbCacheKB);
    m_thumbThread = QThread::create([this] { thumbLoop(); });
    m_stripThread = QThread::create([this] { stripLoop(); });
    m_waveThread = QThread::create([this] { waveLoop(); });
    m_thumbThread->start(QThread::LowPriority);
    m_stripThread->start(QThread::LowPriority);
    m_waveThread->start(QThread::LowestPriority);
}

MediaCache::~MediaCache()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
    }
    m_thumbCond.wakeAll();
    m_stripCond.wakeAll();
    m_waveCond.wakeAll();
    m_thumbThread->wait();
    m_stripThread->wait();
    m_waveThread->wait();
    delete m_thumbThread;
    delete m_stripThread;
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

QImage MediaCache::thumbnail(const QString& path, int frame, int tolerance)
{
    QMutexLocker lock(&m_mutex);
    if (const QImage* img = m_thumbs.object(thumbKey(path, frame))) return *img;

    auto it = m_keys.find(path);
    if (it == m_keys.end()) { // erster Blick auf die Datei -> Keyframe-Durchlauf starten
        it = m_keys.insert(path, {});
        m_stripJobs.push_back(path);
        m_stripCond.wakeOne();
    }
    const KeyIndex& ki = *it;
    const auto next = std::upper_bound(ki.frames.cbegin(), ki.frames.cend(), frame);
    QImage approx;
    bool enough = false;
    if (next != ki.frames.cbegin()) {
        const int key = *(next - 1);
        if (const QImage* img = m_thumbs.object(thumbKey(path, key))) {
            approx = *img;
            enough = frame - key < tolerance; // Fehler kleiner als der Kachelabstand -> sieht man nicht
        }
    }
    // Genaues Frame nur, wenn der Keyframe-Durchlauf diese Stelle schon hinter sich hat (oder scheiterte)
    const bool passed = ki.done || next != ki.frames.cend();
    if (!enough && (ki.failed || passed)) queueThumb(path, frame);
    return approx;
}

void MediaCache::queueThumb(const QString& path, int frame)
{
    const QString key = thumbKey(path, frame);
    if (m_thumbPending.contains(key)) return;
    m_thumbPending.insert(key);
    m_thumbJobs.push_front({path, frame});
    // Alte Anfragen verwerfen (z. B. von Stellen, an denen man längst weitergescrollt hat)
    while (m_thumbJobs.size() > kMaxThumbJobs) {
        m_thumbPending.remove(thumbKey(m_thumbJobs.back().path, m_thumbJobs.back().frame));
        m_thumbJobs.pop_back();
    }
    m_thumbCond.wakeOne();
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
        m_keys.clear();
        m_stripJobs.clear();
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
            prod = std::make_shared<Mlt::Producer>(*profile, decodePath(job.path).toUtf8().constData());
            prod->set("audio_index", -1); // Ton wird hier nie gebraucht
            producers.insert(job.path, prod);
        }

        QImage img;
        if (prod->is_valid()) {
            prod->seek(job.frame);
            std::unique_ptr<Mlt::Frame> f(prod->get_frame());
            if (f) {
                mlt_image_format fmt = mlt_image_rgba;
                const QSize want = thumbSize(format);
                int w = want.width(), h = want.height();
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

void MediaCache::stripLoop()
{
    static const QRegularExpression ptsRe(QStringLiteral("\\bn:\\s*\\d+.*\\bpts_time:(-?[0-9.]+)"));
    for (;;) {
        QString path;
        ProjectFormat format;
        int gen = 0;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_stripJobs.empty()) m_stripCond.wait(&m_mutex);
            if (m_quit) return;
            path = m_stripJobs.front();
            m_stripJobs.pop_front();
            format = m_format;
            gen = m_generation;
        }
        const QSize size = thumbSize(format);
        const double fps = format.rate.fps();
        // Nur Keyframes dekodieren, anamorphe Pixel entzerren, wie MLT mittig mit schwarzen Rändern ins
        // Projektformat einpassen; showinfo meldet die Zeit jedes Bilds auf stderr (Drehung macht ffmpeg selbst)
        const QString vf = QString("scale='if(gt(sar,0),iw*sar,iw)':ih,scale=%1:%2:force_original_aspect_ratio=decrease,"
                                   "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:black,setsar=1,showinfo")
                               .arg(size.width())
                               .arg(size.height());
        QProcess ff;
        ff.start("ffmpeg", {"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-skip_frame", "nokey", "-i",
                            decodePath(path), "-map", "0:v:0", "-an", "-sn", "-vf", vf, "-fps_mode", "passthrough",
                            "-f", "rawvideo", "-pix_fmt", "rgba", "-"});
        const qint64 frameBytes = qint64(size.width()) * size.height() * 4;
        QByteArray out, err;
        std::deque<int> times; // Quell-Frames aus showinfo, noch ohne Bild
        int count = 0;
        bool aborted = false;
        auto takeFrames = [&] {
            err += ff.readAllStandardError();
            int nl;
            while ((nl = err.indexOf('\n')) >= 0) {
                const QString line = QString::fromUtf8(err.left(nl));
                err.remove(0, nl + 1);
                if (!line.contains("showinfo")) continue;
                const auto m = ptsRe.match(line);
                if (m.hasMatch()) times.push_back(int(std::lround(m.captured(1).toDouble() * fps)));
            }
            out += ff.readAllStandardOutput();
            while (out.size() >= frameBytes && !times.empty()) {
                QImage raw(reinterpret_cast<const uchar*>(out.constData()), size.width(), size.height(), QImage::Format_RGBA8888);
                const QImage img = raw.convertToFormat(QImage::Format_RGB32); // kopiert, schneller zu zeichnen
                out.remove(0, int(frameBytes));
                const int frame = std::max(0, times.front());
                times.pop_front();
                QMutexLocker lock(&m_mutex);
                if (m_quit || gen != m_generation) {
                    aborted = true;
                    return;
                }
                KeyIndex& ki = m_keys[path];
                const auto pos = std::lower_bound(ki.frames.begin(), ki.frames.end(), frame);
                if (pos == ki.frames.end() || *pos != frame) ki.frames.insert(pos, frame);
                m_thumbs.insert(thumbKey(path, frame), new QImage(img), int(img.sizeInBytes() / 1024) + 1);
                ++count;
            }
        };
        if (ff.waitForStarted(5000)) {
            while (!aborted) {
                const bool running = ff.state() != QProcess::NotRunning;
                if (running) ff.waitForReadyRead(100);
                const int before = count;
                takeFrames();
                if (count != before) emit updated();
                if (!running) break;
                QMutexLocker lock(&m_mutex);
                if (m_quit || gen != m_generation) aborted = true;
            }
            if (aborted) {
                ff.kill();
                ff.waitForFinished(1000);
            }
        }
        if (aborted) continue;
        {
            QMutexLocker lock(&m_mutex);
            if (gen != m_generation) continue;
            KeyIndex& ki = m_keys[path];
            ki.done = true;
            ki.failed = count == 0; // kein ffmpeg / unlesbar -> wie früher über MLT
        }
        emit updated();
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
