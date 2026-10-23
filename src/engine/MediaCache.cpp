#include "engine/MediaCache.h"
#include "engine/Bundle.h"

#include "engine/Profiles.h"
#include "engine/ProxyManager.h"
#include "engine/audio/Path.h"
#include "engine/audio/Reader.h"
#include "engine/Background.h"

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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

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

// Disk cache for waveforms: key from path, size and modification time
QString waveCacheFile(const QString& path, int stream, const FrameRate& rate, bool own)
{
    const QFileInfo fi(path);
    // further streams with a suffix so that existing waveforms (stream 0) stay valid; the own audio engine places
    // some files' sound differently than MLT (sound stream later than the picture, 44.1 kHz) -> own entries
    const QString id = (own ? QStringLiteral("own|") : QString()) + (stream > 0 ? QString("s%1|").arg(stream) : QString())
                       + QString("%1|%2|%3|%4|%5")
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

// Use an existing proxy for decoding (much smaller, dense keyframes), same frame counting
QString decodePath(const QString& path)
{
    const QString proxy = ProxyManager::proxyPath(path);
    return QFileInfo::exists(proxy) ? proxy : path;
}

QSize thumbSize(const ProjectFormat& format)
{
    // In the project's aspect ratio, at least as large as a 16:9 tile
    // (the timeline crops the middle; so portrait is somewhat taller)
    const QSize s = format.size().scaled(MediaCache::kThumbHeight * 16 / 9, MediaCache::kThumbHeight, Qt::KeepAspectRatioByExpanding);
    return QSize(std::max(2, s.width()), std::max(2, s.height()));
}

} // namespace

MediaCache::MediaCache(QObject* parent) : QObject(parent)
{
    m_thumbs.setMaxCost(kThumbCacheKB);
    m_thumbThread = QThread::create([this] {
        Background::lowerThisThread(Background::Level::Low);
        thumbLoop();
    });
    m_stripThread = QThread::create([this] {
        Background::lowerThisThread(Background::Level::Low); // also the ffmpeg it starts (inherits the nice value)
        stripLoop();
    });
    m_waveThread = QThread::create([this] {
        Background::lowerThisThread(Background::Level::Lowest);
        waveLoop();
    });
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
    if (it == m_keys.end()) { // first look at the file -> start the keyframe pass
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
            enough = frame - key < tolerance; // error smaller than the tile spacing -> not visible
        }
    }
    // Exact frame only if the keyframe pass has already passed this spot (or failed)
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
    // Discard old requests (e.g. from spots one has long scrolled past)
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

std::shared_ptr<const Waveform> MediaCache::waveform(const QString& path, int stream)
{
    // Key: path, for further streams with an appended number (\0 does not occur in paths)
    const QString key = stream > 0 ? path + QChar(0) + QString::number(stream) : path;
    QMutexLocker lock(&m_mutex);
    if (!m_waveRequested.contains(key)) {
        m_waveRequested.insert(key);
        m_waveJobs.push_back(key);
        m_waveCond.wakeOne();
    }
    return m_waves.value(key);
}

void MediaCache::thumbLoop()
{
    std::unique_ptr<Mlt::Profile> profile;
    std::unique_ptr<ProducerFactory> factory; // BT.601 detour (Profiles.h), outlives the producers
    int generation = -1;
    // Reuse open producers (opening a file is more expensive than seeking)
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
        Background::yieldToForeground();
        if (gen != generation) { // new project format: producers hang on the old profile
            producers.clear();
            factory.reset();
            profile = makeProfile(format);
            factory = std::make_unique<ProducerFactory>(*profile);
            generation = gen;
        }

        std::shared_ptr<Mlt::Producer> prod = producers.value(job.path);
        if (!prod) {
            if (producers.size() >= 8) producers.clear();
            prod = factory->open(decodePath(job.path));
            prod->set("audio_index", -1); // audio is never needed here
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
                    img = img.convertToFormat(QImage::Format_RGB32); // faster to draw
                }
            }
        }

        {
            QMutexLocker lock(&m_mutex);
            if (gen != m_generation) continue; // new format in the meantime
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
        Background::yieldToForeground();
        const QSize size = thumbSize(format);
        const double fps = format.rate.fps();
        // Only decode keyframes, unsqueeze anamorphic pixels, fit into the project format centered with black borders like MLT;
        // showinfo reports the time of each image on stderr (ffmpeg rotates on its own)
        const QString vf = QString("scale='if(gt(sar,0),iw*sar,iw)':ih,scale=%1:%2:force_original_aspect_ratio=decrease,"
                                   "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:black,setsar=1,showinfo")
                               .arg(size.width())
                               .arg(size.height());
        QProcess ff;
        ff.start(Bundle::tool("ffmpeg"), {"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-skip_frame", "nokey", "-i",
                            decodePath(path), "-map", "0:v:0", "-an", "-sn", "-vf", vf, "-fps_mode", "passthrough",
                            "-f", "rawvideo", "-pix_fmt", "rgba", "-"});
        const qint64 frameBytes = qint64(size.width()) * size.height() * 4;
        QByteArray out, err;
        std::deque<int> times; // source frames from showinfo, still without image
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
                const QImage img = raw.convertToFormat(QImage::Format_RGB32); // copied, faster to draw
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
            ki.failed = count == 0; // no ffmpeg / unreadable -> via MLT as before
        }
        emit updated();
    }
}

void MediaCache::waveLoop()
{
    constexpr int N = Waveform::kBucketsPerFrame;

    for (;;) {
        QString key, path;
        int stream = 0;
        ProjectFormat format;
        int gen = 0;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_waveJobs.empty()) m_waveCond.wait(&m_mutex);
            if (m_quit) return;
            lock.unlock();
            Background::yieldToForeground();
            lock.relock();
            if (m_quit || m_waveJobs.empty()) continue;
            key = path = m_waveJobs.front();
            m_waveJobs.pop_front();
            if (const int z = key.indexOf(QChar(0)); z >= 0) {
                path = key.left(z);
                stream = key.mid(z + 1).toInt();
            }
            format = m_format;
            gen = m_generation;
        }
        // Waveform counts frames in the project frame rate
        const bool own = Audio::ownPath();
        auto publish = [&](std::shared_ptr<const Waveform> w) {
            {
                QMutexLocker lock(&m_mutex);
                if (gen != m_generation) return; // new format in the meantime
                m_waves.insert(key, std::move(w));
            }
            emit updated();
        };

        const QString cacheFile = waveCacheFile(path, stream, format.rate, own);
        {
            auto w = std::make_shared<Waveform>();
            if (loadWave(cacheFile, *w)) {
                publish(w);
                continue;
            }
        }

        // Peaks of frame i: fills peaks[0..N) with 0..255; false = no sound for it
        int length = 0;
        std::function<bool(int, quint8*)> framePeaks;
        // Own engine: the stream as the mixer reads it (stereo, 48 kHz), frame i = samples of timeline frame i
        std::unique_ptr<Audio::Reader> reader;
        std::vector<float> buf[2];
        int64_t bufFrom = 0, bufTo = 0;
        // MLT audio path
        std::unique_ptr<Mlt::Profile> profile;
        std::unique_ptr<Mlt::Producer> producer;
        if (own) {
            constexpr int kRate = 48000;
            reader = Audio::Reader::open(path, stream, Audio::Reader::Options{kRate, 2});
            const FrameRate fps = format.rate;
            const auto frameStart = [fps](int64_t f) {
                const int64_t n = f * kRate * fps.den;
                return (2 * n + fps.num) / (2 * int64_t(fps.num)); // = Audio::Mixer::frameStart (f >= 0)
            };
            if (reader)
                length = int(std::ceil(double(std::max<int64_t>(0, reader->info().end)) * fps.num / (double(kRate) * fps.den)));
            framePeaks = [&, frameStart](int i, quint8* peaks) {
                const int64_t a = frameStart(i), b = frameStart(i + 1);
                if (b > bufTo) { // read about a second ahead
                    bufFrom = a;
                    bufTo = std::max(b, frameStart(i + 25));
                    float* out[2];
                    for (int c = 0; c < 2; ++c) {
                        buf[c].resize(size_t(bufTo - bufFrom));
                        out[c] = buf[c].data();
                    }
                    reader->read(bufFrom, int(bufTo - bufFrom), out);
                }
                const int samples = int(b - a);
                for (int k = 0; k < N; ++k) {
                    const int s0 = samples * k / N, s1 = samples * (k + 1) / N;
                    float peak = 0.f;
                    for (int c = 0; c < 2; ++c)
                        for (int s = s0; s < s1; ++s) peak = std::max(peak, std::abs(buf[c][size_t(a - bufFrom + s)]));
                    peaks[k] = quint8(std::min(255, int(std::min(peak, 1.f) * 32767.f) >> 7)); // as the s16 path
                }
                return true;
            };
        } else {
            profile = makeProfile(format);
            const double fps = profile->fps();
            producer = std::make_unique<Mlt::Producer>(*profile, path.toUtf8().constData());
            if (producer->is_valid()) selectAudioStream(*producer, stream);
            length = producer->is_valid() ? producer->get_length() : 0;
            framePeaks = [&, fps](int i, quint8* peaks) {
                Mlt::Producer& p = *producer;
                p.seek(i);
                std::unique_ptr<Mlt::Frame> f(p.get_frame());
                if (!f) return false;
                // Only fetch audio – without get_image the video is not decoded at all
                mlt_audio_format fmt = mlt_audio_s16;
                int freq = 48000, channels = 2;
                int samples = mlt_audio_calculate_frame_samples(float(fps), freq, i);
                const auto* pcm = static_cast<const int16_t*>(f->get_audio(fmt, freq, channels, samples));
                if (!pcm || samples <= 0 || channels <= 0) return false;
                for (int b = 0; b < N; ++b) {
                    const int s0 = samples * b / N, s1 = samples * (b + 1) / N;
                    int peak = 0;
                    for (int s = s0 * channels; s < s1 * channels; ++s) peak = std::max(peak, std::abs(int(pcm[s])));
                    peaks[b] = quint8(std::min(255, peak >> 7));
                }
                return true;
            };
        }
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
            if (!framePeaks(i, base.data() + i * N)) continue;

            // Show intermediate states so that long files do not stay empty forever
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
            continue; // format change: result no longer applies
        }

        auto w = std::make_shared<Waveform>();
        w->levels = buildLevels(base);
        w->frames = length;
        w->complete = true;
        publish(w);
        saveWave(cacheFile, base, length);
    }
}
